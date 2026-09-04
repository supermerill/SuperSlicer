///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "CustomGCodePerPrintZ.hpp"

#include <cassert>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "CustomGCodePerPrintZRecord.hpp"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_extrusion_edit.h"
#include "libslic3r/Api/plugin/cpp/ConfigViews.hpp"
#include "libslic3r/Api/plugin/cpp/ExtrusionViews.hpp"
#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"
#include "libslic3r/Api/plugin/cpp/PrintingPlanViews.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"

/*
Custom height-marker G-code insertion
=====================================

This plugin converts Model custom-G-code markers associated with print heights
into executable events in the PrintingPlan. It runs at STEP_EXTRUSION_EDIT,
after the plan and its tool groups exist, and keeps each marker in the order in
which it appears in the model data.

The normal execution flow is:

    run_impl()
    |-- read the marker table and the effective printing-plan mode
    |-- map marker rows to their target printing-plan layer groups
    |-- choose the first tool group's event scope, or the layer scope when no
    |   tool group exists
    |-- convert each marker:
    |   |-- ColorChange: create a processor-tagged color-change script
    |   |-- ToolChange: skip an ordinary tool-change row; convert the
    |   |   single-extruder MultiAsSingle case into a color event
    |   |-- PausePrint: create a tagged pause script
    |   |-- Template: create a tagged template script
    |   `-- Custom: append the row's raw G-code after its processor tag
    `-- warn about markers that lie above the last plan layer

Each generated marker is a non-sortable, non-reversible wrapper containing the
processor tag followed by the script or raw G-code. This keeps the tag directly
adjacent to the event while allowing the script properties to remain attached
to their own child. Script arguments such as the current tool, target color,
and pause text are stored in a temporary Config for the G-code script
processor.

Color and tool-change markers are accepted only when the marker-table mode and
the print-plan mode agree about single- versus multi-extruder operation. A
color change without a target tool group is rejected because it cannot identify
the physical extruder. Unmapped rows are reported as warnings and do not alter
the plan. This plugin edits the PrintingPlan events; it does not itself emit
final machine G-code.
*/

namespace slic3r_api::GCodeGeneration::CustomGCodePerPrintZPlugin {
namespace {

const char *const k_no_dependencies[] = {nullptr};

/* Add one scalar integer placeholder to the temporary script Config. */
void set_int_argument(MutableConfig &config, const char *key, int32_t value);

/* Add one scalar string placeholder to the temporary script Config. */
void set_string_argument(MutableConfig &config, const char *key, const std::string &value);

/*
Check that tool-sensitive markers and the PrintingPlan describe compatible
single-tool or multi-tool operation before producing an executable event.
*/
bool tool_and_color_modes_are_compatible(Slic3r::CustomGCode::Mode print_mode,
                                         Slic3r::CustomGCode::Mode record_mode);

/*
Recognize the legacy single-nozzle interpretation of a MultiAsSingle
ToolChange. Such a row pauses for filament replacement instead of selecting a
different physical tool.
*/
bool row_is_converted_tool_change(const CustomGCodeRow &row,
                                  Slic3r::CustomGCode::Mode print_mode,
                                  Slic3r::CustomGCode::Mode record_mode);

/*
Return the zero-based tool associated with a color-change script, including a
converted ToolChange, or no value when the row has no color action in this
printer mode.
*/
std::optional<uint16_t> color_event_tool(const CustomGCodeRow &row,
                                         Slic3r::CustomGCode::Mode print_mode,
                                         Slic3r::CustomGCode::Mode record_mode);

/* Build the GCodeProcessor marker that identifies the row in generated output. */
std::string processor_tag(const CustomGCodeRow &row, uint16_t color_tool);

/*
Append one indivisible tag-plus-script event to a scope. A non-sortable wrapper
keeps the processor marker immediately before the script, while script_gcode()
clones the producer Config into the extrusion property for later execution.
*/
void append_tagged_script(storage_handle *storage,
                          const PrintingScopeEvents &events,
                          const std::string &tag,
                          const std::string &script,
                          gcode_script_type script_type,
                          const Config &arguments,
                          uint16_t processing_extruder);

/*
Append one indivisible tag-plus-raw-G-code event. This path is used for Custom
rows because their text is already final G-code and must bypass the placeholder
processor.
*/
void append_tagged_raw_gcode(storage_handle *storage,
                             const PrintingScopeEvents &events,
                             const std::string &tag,
                             const std::string &gcode);

void set_int_argument(MutableConfig &config, const char *key, int32_t value)
{
    config.get_or_add(key, SLIC3R_CONFIG_OPTION_INT).set_int(value);
}

void set_string_argument(MutableConfig &config, const char *key, const std::string &value)
{
    config.get_or_add(key, SLIC3R_CONFIG_OPTION_STRING).set_string(value);
}

bool tool_and_color_modes_are_compatible(Slic3r::CustomGCode::Mode print_mode,
                                         Slic3r::CustomGCode::Mode record_mode)
{
    return (print_mode == Slic3r::CustomGCode::MultiExtruder) ==
           (record_mode == Slic3r::CustomGCode::MultiExtruder);
}

bool row_is_converted_tool_change(const CustomGCodeRow &row,
                                  Slic3r::CustomGCode::Mode print_mode,
                                  Slic3r::CustomGCode::Mode record_mode)
{
    return row.type == Slic3r::CustomGCode::ToolChange &&
           print_mode == Slic3r::CustomGCode::SingleExtruder &&
           record_mode == Slic3r::CustomGCode::MultiAsSingle;
}

std::optional<uint16_t> color_event_tool(const CustomGCodeRow &row,
                                         Slic3r::CustomGCode::Mode print_mode,
                                         Slic3r::CustomGCode::Mode record_mode)
{
    if (!tool_and_color_modes_are_compatible(print_mode, record_mode))
        return std::nullopt;
    if (row.type == Slic3r::CustomGCode::ColorChange)
        return print_mode == Slic3r::CustomGCode::SingleExtruder ?
            std::optional<uint16_t>(0) : std::optional<uint16_t>(uint16_t(row.extruder - 1));
    if (row_is_converted_tool_change(row, print_mode, record_mode))
        return uint16_t(0);
    return std::nullopt;
}

std::string processor_tag(const CustomGCodeRow &row, uint16_t color_tool)
{
    if (row.type == Slic3r::CustomGCode::ColorChange ||
        row.type == Slic3r::CustomGCode::ToolChange)
        return ";" + Slic3r::GCodeProcessor::reserved_tag(
                   Slic3r::GCodeProcessor::ETags::Color_Change) +
               ",T" + std::to_string(color_tool) + "," + row.color + "\n";
    if (row.type == Slic3r::CustomGCode::PausePrint)
        return ";" + Slic3r::GCodeProcessor::reserved_tag(
                   Slic3r::GCodeProcessor::ETags::Pause_Print) + "\n";
    return ";" + Slic3r::GCodeProcessor::reserved_tag(
               Slic3r::GCodeProcessor::ETags::Custom_Code) + "\n";
}

void append_tagged_script(storage_handle *storage,
                          const PrintingScopeEvents &events,
                          const std::string &tag,
                          const std::string &script,
                          gcode_script_type script_type,
                          const Config &arguments,
                          uint16_t processing_extruder)
{
    /* One non-sortable event root keeps the processor tag immediately before
       the script while still allowing each child to own its custom property. */
    StoredExtrusionEntity event(storage);
    event.disable_sort().disable_reverse();
    MutableExtrusionEntity tag_entity = event.emplace_child();
    tag_entity.custom_gcode(tag, C_EXTRUSION_CUSTOM_GCODE_GCODE);
    MutableExtrusionEntity script_entity = event.emplace_child();
    script_entity.script_gcode(script, script_type, arguments, processing_extruder);
    if (!events.append_before_move(event.mutable_view()).valid())
        throw std::runtime_error("Cannot append a custom height-marker script to the printing plan.");
}

void append_tagged_raw_gcode(storage_handle *storage,
                             const PrintingScopeEvents &events,
                             const std::string &tag,
                             const std::string &gcode)
{
    StoredExtrusionEntity event(storage);
    event.disable_sort().disable_reverse();
    MutableExtrusionEntity tag_entity = event.emplace_child();
    tag_entity.custom_gcode(tag, C_EXTRUSION_CUSTOM_GCODE_GCODE);
    MutableExtrusionEntity gcode_entity = event.emplace_child();
    gcode_entity.custom_gcode(gcode, C_EXTRUSION_CUSTOM_GCODE_GCODE);
    if (!events.append_before_move(event.mutable_view()).valid())
        throw std::runtime_error("Cannot append custom height-marker G-code to the printing plan.");
}

class CustomGCodePerPrintZ final : public PluginBase
{
public:
    static CustomGCodePerPrintZ &instance(orchestrator_handle *orchestrator)
    {
        static CustomGCodePerPrintZ plugin(orchestrator);
        return plugin;
    }
    explicit CustomGCodePerPrintZ(orchestrator_handle *orchestrator) : PluginBase(orchestrator) {}

private:
    const char *id_impl() const noexcept override { return "gcode.custom_gcode_per_print_z"; }
    const char *name_impl() const noexcept override { return "Custom G-code at height"; }
    const char *description_impl() const noexcept override {
        return "Converts Model height markers into ordered printing-plan scripts and G-code events.";
    }
    slicing_step_t step_impl() const noexcept override { return STEP_EXTRUSION_EDIT; }
    const char *const *dependencies_impl() const noexcept override { return k_no_dependencies; }
    int32_t priority_impl() const noexcept override { return 100; }
    int32_t used_config_keys(raw_used_config_key *keys) const noexcept override {
        if (keys == nullptr)
            return 4;
        keys[0] = raw_used_config_key{"color_change_gcode", RAW_CO_STRING,
                                      RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE};
        keys[1] = raw_used_config_key{"pause_print_gcode", RAW_CO_STRING,
                                      RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE};
        keys[2] = raw_used_config_key{"template_custom_gcode", RAW_CO_STRING,
                                      RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE};
        keys[3] = raw_used_config_key{"nozzle_diameter", RAW_CO_VECTOR_FLOAT,
                                      RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE};
        return 4;
    }

    void run_impl(const plugin_run_context *run_context) const override
    {
        const run_ctx_extrusion_edition *context = plugin_ctx_as_extrusion_edition(run_context);
        assert(context != nullptr && context->print != nullptr && context->plan != nullptr);
        if (context == nullptr || context->print == nullptr || context->plan == nullptr ||
            run_context->plugin_storage == nullptr)
            return;

        const Print print(context->print);
        const PrintingPlan plan(context->plan);
        const std::optional<CustomGCodeTable> table_optional = read_custom_gcode_per_print_z(print);
        if (!table_optional || table_optional->rows.empty())
            return;

        const CustomGCodeTable &table = *table_optional;
        const Config config = print.config();
        const Slic3r::CustomGCode::Mode print_mode = printing_plan_mode(print, plan);
        const Slic3r::CustomGCode::Mode record_mode = effective_record_mode(table);
        std::vector<bool> mapped_rows(table.rows.size(), false);
        std::optional<uint16_t> last_tool;

        for (uint32_t group_idx = 0; group_idx < plan.group_count(); ++group_idx) {
            const PrintingGroup group = plan.group(group_idx);
            const std::vector<std::vector<const CustomGCodeRow *>> layer_rows = rows_by_layer(table, group);
            for (uint32_t layer_idx = 0; layer_idx < group.layer_group_count(); ++layer_idx) {
                const PrintingLayerGroup layer = group.layer_group(layer_idx);
                if (layer_rows[layer_idx].empty()) {
                    if (layer.tool_group_count() > 0)
                        last_tool = layer.tool_group(layer.tool_group_count() - 1).extruder_id();
                    continue;
                }

                const bool has_tool_group = layer.tool_group_count() > 0;
                const PrintingScopeEvents destination_events = has_tool_group ?
                    layer.tool_group(0).events() : layer.events();
                const uint16_t current_tool = has_tool_group ?
                    layer.tool_group(0).extruder_id() :
                    last_tool.value_or(GCODE_SCRIPT_PROCESSING_EXTRUDER_INVALID);

                for (const CustomGCodeRow *row : layer_rows[layer_idx]) {
                    mapped_rows[row->source_row] = true;
                    const std::optional<uint16_t> color_tool =
                        color_event_tool(*row, print_mode, record_mode);
                    if (color_tool) {
                        if (!has_tool_group)
                            throw std::runtime_error("A color change has no tool group on its target layer.");
                        StoredConfig arguments(run_context->plugin_storage);
                        set_int_argument(arguments, "color_change_extruder", int32_t(*color_tool));
                        set_string_argument(arguments, "next_color", row->color);
                        set_string_argument(arguments, "next_colour", row->color);
                        append_tagged_script(
                            run_context->plugin_storage, destination_events,
                            processor_tag(*row, *color_tool),
                            config.string_or_default("color_change_gcode", std::string()),
                            GCODE_SCRIPT_TYPE_COLOR_CHANGE_GCODE, arguments, *color_tool);
                        continue;
                    }

                    if (row->type == Slic3r::CustomGCode::ToolChange)
                        continue;
                    if (row->type == Slic3r::CustomGCode::PausePrint) {
                        StoredConfig arguments(run_context->plugin_storage);
                        set_int_argument(arguments, "color_change_extruder",
                                         current_tool == GCODE_SCRIPT_PROCESSING_EXTRUDER_INVALID ?
                                             0 : int32_t(current_tool));
                        set_string_argument(arguments, "next_color", std::string());
                        set_string_argument(arguments, "next_colour", std::string());
                        set_string_argument(arguments, "pause_message", row->extra);
                        append_tagged_script(
                            run_context->plugin_storage, destination_events,
                            processor_tag(*row, 0),
                            config.string_or_default("pause_print_gcode", std::string()),
                            GCODE_SCRIPT_TYPE_PAUSE_PRINT_GCODE, arguments, current_tool);
                    } else if (row->type == Slic3r::CustomGCode::Template) {
                        StoredConfig arguments(run_context->plugin_storage);
                        append_tagged_script(
                            run_context->plugin_storage, destination_events,
                            processor_tag(*row, 0),
                            config.string_or_default("template_custom_gcode", std::string()),
                            GCODE_SCRIPT_TYPE_TEMPLATE_CUSTOM_GCODE, arguments, current_tool);
                    } else {
                        append_tagged_raw_gcode(run_context->plugin_storage, destination_events,
                                                processor_tag(*row, 0), row->extra);
                    }
                }
                if (layer.tool_group_count() > 0)
                    last_tool = layer.tool_group(layer.tool_group_count() - 1).extruder_id();
            }
        }

        for (uint32_t row_idx = 0; row_idx < mapped_rows.size(); ++row_idx)
            if (!mapped_rows[row_idx]) {
                const std::string warning = "Ignoring custom G-code marker above the last printing-plan layer (row " +
                    std::to_string(row_idx) + ").";
                report_warning(run_context, warning.c_str());
            }
    }
};

} // namespace

void register_custom_gcode_per_print_z_plugins(orchestrator_handle *orchestrator)
{
    // The two ordering passes and the later event pass implement one feature.
    // Running only a subset can leave tool visits inconsistent with the
    // height-marker events emitted into the final PrintingPlan.
    const char *const members[] = {
        "ordering.custom_gcode_tool_overrides",
        "ordering.custom_gcode_event_tools",
        "gcode.custom_gcode_per_print_z"
    };
    if (!orchestrator_register_activation_group(
            orchestrator, "custom_gcode_per_print_z", {members, 3}))
        throw std::runtime_error(
            "Cannot register the custom_gcode_per_print_z activation group.");

    register_custom_gcode_per_print_z_ordering_plugins(orchestrator);
    orchestrator_register_plugin(orchestrator, CustomGCodePerPrintZ::instance(orchestrator).c_instance());
}

} // namespace slic3r_api::GCodeGeneration::CustomGCodePerPrintZPlugin
