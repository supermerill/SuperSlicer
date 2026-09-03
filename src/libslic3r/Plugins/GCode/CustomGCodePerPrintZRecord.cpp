///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "CustomGCodePerPrintZRecord.hpp"

#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

#include "libslic3r/Api/plugin/cpp/ConfigViews.hpp"
#include "libslic3r/Api/plugin/cpp/DataTreeViews.hpp"
#include "libslic3r/Api/plugin/cpp/PrintingPlanViews.hpp"

/*
Custom G-code PrintRecord reader
================================

The Model publishes custom height markers as a column-oriented DynamicConfig.
This file is the single decoder used by all pipeline plugins that consume
those markers. It transforms the columnar record into a validated
`CustomGCodeTable` and provides the common rules for matching rows to plan
layers and determining the printer mode.

The normal execution flow is:

    read_custom_gcode_per_print_z()
    |-- read the PrintRecord channel
    |-- validate the row-count and mode columns
    |-- validate every row column's type and length before copying data
    |-- validate finite, non-decreasing Z values and event types
    |-- validate tool numbers against `nozzle_diameter`
    `-- return the complete table, or no table when the channel is absent

    effective_record_mode()
    `-- use the stored mode, or infer SingleExtruder, MultiAsSingle, or
        MultiExtruder from the marker rows when the stored mode is undefined

    rows_by_layer()
    `-- assign each marker to the first PrintingLayerGroup whose print Z is
        greater than or equal to the marker Z

`printing_plan_mode()` inspects the tools used by object extrusions, rather
than support or auxiliary extrusions, to distinguish a true multi-extruder
plan from a multi-extruder printer operating as a single tool. This distinction
is used by both marker-ordering and marker-event plugins. Tool values are
one-based in the stored record and become zero-based when returned to plan
consumers.

All columns are checked before any row is copied, so malformed records cannot
partially change tool ordering or append only part of the requested scripts.
Rows above the last plan layer remain in the table and are reported by the
consumer that maps them; this reader does not silently discard them.
*/

namespace slic3r_api::GCodeGeneration::CustomGCodePerPrintZPlugin {
namespace {

/*
Return one mandatory table column after checking its exact Config type and
length. Centralizing this rule guarantees that every consumer rejects the same
malformed schema before reading any indexed value.
*/
ConfigOption required_option(const Config &record,
                             const char *key,
                             config_option_type expected_type,
                             uint32_t expected_size);

/* Accept only integer values represented by the public CustomGCode::Mode enum. */
bool valid_mode(int32_t mode);

/* Accept only integer values represented by the public CustomGCode::Type enum. */
bool valid_type(int32_t type);

ConfigOption required_option(const Config &record,
                             const char *key,
                             config_option_type expected_type,
                             uint32_t expected_size)
{
    if (!record.has(key))
        throw std::runtime_error(std::string("custom_gcode_per_print_z is missing column '") + key + "'.");
    const ConfigOption option = record.get(key);
    if (option.type() != expected_type || option.size() != expected_size)
        throw std::runtime_error(std::string("custom_gcode_per_print_z column '") + key +
                                 "' has an invalid type or size.");
    return option;
}

bool valid_mode(int32_t mode)
{
    return mode >= int32_t(Slic3r::CustomGCode::Undef) &&
           mode <= int32_t(Slic3r::CustomGCode::MultiExtruder);
}

bool valid_type(int32_t type)
{
    return type >= int32_t(Slic3r::CustomGCode::ColorChange) &&
           type <= int32_t(Slic3r::CustomGCode::Custom);
}

} // namespace

std::optional<CustomGCodeTable> read_custom_gcode_per_print_z(const Print &print)
{
    const std::optional<Config> record_optional =
        print.records().find(Slic3r::CustomGCode::PrintRecordChannel);
    if (!record_optional)
        return std::nullopt;
    const Config record = *record_optional;

    const ConfigOption size_option = required_option(
        record, Slic3r::CustomGCode::PrintRecordSizeKey, SLIC3R_CONFIG_OPTION_INT, 1);
    const int32_t signed_size = size_option.get_int();
    if (signed_size < 0)
        throw std::runtime_error("custom_gcode_per_print_z has a negative row count.");
    const uint32_t row_count = uint32_t(signed_size);

    const ConfigOption mode_option = required_option(
        record, Slic3r::CustomGCode::PrintRecordModeKey, SLIC3R_CONFIG_OPTION_INT, 1);
    const int32_t raw_mode = mode_option.get_int();
    if (!valid_mode(raw_mode))
        throw std::runtime_error("custom_gcode_per_print_z has an unknown mode.");

    /* Validate every column before copying a row. This keeps errors atomic and
       also guarantees that indexed reads below cannot fall back silently. */
    const ConfigOption print_z = required_option(
        record, Slic3r::CustomGCode::PrintRecordPrintZKey, SLIC3R_CONFIG_OPTION_FLOATS, row_count);
    const ConfigOption types = required_option(
        record, Slic3r::CustomGCode::PrintRecordTypeKey, SLIC3R_CONFIG_OPTION_INTS, row_count);
    const ConfigOption extruders = required_option(
        record, Slic3r::CustomGCode::PrintRecordExtruderKey, SLIC3R_CONFIG_OPTION_INTS, row_count);
    const ConfigOption colors = required_option(
        record, Slic3r::CustomGCode::PrintRecordColorKey, SLIC3R_CONFIG_OPTION_STRINGS, row_count);
    const ConfigOption extras = required_option(
        record, Slic3r::CustomGCode::PrintRecordExtraKey, SLIC3R_CONFIG_OPTION_STRINGS, row_count);

    const Config print_config = print.config();
    if (!print_config.has("nozzle_diameter") ||
        print_config.get("nozzle_diameter").type() != SLIC3R_CONFIG_OPTION_FLOATS ||
        print_config.get("nozzle_diameter").size() == 0)
        throw std::runtime_error("custom_gcode_per_print_z cannot validate extruders without nozzle_diameter.");
    const uint32_t extruder_count = print_config.get("nozzle_diameter").size();

    CustomGCodeTable result;
    result.mode = static_cast<Slic3r::CustomGCode::Mode>(raw_mode);
    result.rows.reserve(row_count);
    double previous_z = -std::numeric_limits<double>::infinity();
    for (uint32_t row_idx = 0; row_idx < row_count; ++row_idx) {
        const double row_z = print_z.get_float(row_idx);
        const int32_t row_type = types.get_int(row_idx);
        const int32_t row_extruder = extruders.get_int(row_idx);
        if (!std::isfinite(row_z))
            throw std::runtime_error("custom_gcode_per_print_z contains a non-finite print Z.");
        if (row_z < previous_z)
            throw std::runtime_error("custom_gcode_per_print_z rows are not ordered by print Z.");
        if (!valid_type(row_type))
            throw std::runtime_error("custom_gcode_per_print_z contains an unknown event type.");
        if (row_extruder < 0)
            throw std::runtime_error("custom_gcode_per_print_z contains a negative extruder.");
        if ((row_type == int32_t(Slic3r::CustomGCode::ColorChange) ||
             row_type == int32_t(Slic3r::CustomGCode::ToolChange)) &&
            (row_extruder == 0 || uint32_t(row_extruder) > extruder_count))
            throw std::runtime_error("custom_gcode_per_print_z contains an extruder outside the printer range.");

        CustomGCodeRow row;
        row.print_z_mm = row_z;
        row.type = static_cast<Slic3r::CustomGCode::Type>(row_type);
        row.extruder = row_extruder;
        row.color = colors.get_string(row_idx);
        row.extra = extras.get_string(row_idx);
        row.source_row = row_idx;
        result.rows.push_back(std::move(row));
        previous_z = row_z;
    }
    return result;
}

Slic3r::CustomGCode::Mode effective_record_mode(const CustomGCodeTable &table)
{
    if (table.mode != Slic3r::CustomGCode::Undef)
        return table.mode;

    bool single_extruder = true;
    for (const CustomGCodeRow &row : table.rows) {
        if (row.type == Slic3r::CustomGCode::ToolChange)
            return Slic3r::CustomGCode::MultiAsSingle;
        if (row.type == Slic3r::CustomGCode::ColorChange && row.extruder > 1)
            single_extruder = false;
    }
    return single_extruder ?
        Slic3r::CustomGCode::SingleExtruder : Slic3r::CustomGCode::MultiExtruder;
}

Slic3r::CustomGCode::Mode printing_plan_mode(const Print &print, const PrintingPlan &plan)
{
    const Config print_config = print.config();
    const uint32_t extruder_count = print_config.get("nozzle_diameter").size();
    if (extruder_count <= 1)
        return Slic3r::CustomGCode::SingleExtruder;

    std::set<uint16_t> object_tools;
    for (uint32_t group_idx = 0; group_idx < plan.group_count(); ++group_idx) {
        const PrintingGroup group = plan.group(group_idx);
        for (uint32_t layer_idx = 0; layer_idx < group.layer_group_count(); ++layer_idx) {
            const PrintingLayerGroup layer = group.layer_group(layer_idx);
            for (uint32_t tool_idx = 0; tool_idx < layer.tool_group_count(); ++tool_idx) {
                const PrintingToolGroup tool = layer.tool_group(tool_idx);
                for (uint32_t extrusion_idx = 0; extrusion_idx < tool.extrusion_count(); ++extrusion_idx)
                    if (printing_extrusion_is_object(group, tool.extrusion(extrusion_idx)))
                        object_tools.insert(tool.extruder_id());
            }
        }
    }
    return object_tools.size() <= 1 ?
        Slic3r::CustomGCode::MultiAsSingle : Slic3r::CustomGCode::MultiExtruder;
}

bool printing_extrusion_is_object(const PrintingGroup &group, const PrintingExtrusion &extrusion)
{
    const LayerRegionIsland region_island = extrusion.region_island();
    if (region_island.region_count() == 0)
        return false;
    const Layer source_layer = region_island.region(0).layer();
    for (uint32_t instance_idx = 0; instance_idx < group.object_instance_count(); ++instance_idx) {
        const Object object = group.object_instance(instance_idx).object();
        for (uint32_t layer_idx = 0; layer_idx < object.layer_count(); ++layer_idx)
            if (object.layer(layer_idx).same_handle(source_layer))
                return true;
    }
    return false;
}

std::vector<std::vector<const CustomGCodeRow *>> rows_by_layer(
    const CustomGCodeTable &table,
    const PrintingGroup &group)
{
    std::vector<std::vector<const CustomGCodeRow *>> result(group.layer_group_count());
    for (const CustomGCodeRow &row : table.rows) {
        const coord_t row_print_z = scale_to_layer_coord(row.print_z_mm);
        for (uint32_t layer_idx = 0; layer_idx < group.layer_group_count(); ++layer_idx) {
            if (group.layer_group(layer_idx).print_z() >= row_print_z) {
                result[layer_idx].push_back(&row);
                break;
            }
        }
    }
    return result;
}

} // namespace slic3r_api::GCodeGeneration::CustomGCodePerPrintZPlugin
