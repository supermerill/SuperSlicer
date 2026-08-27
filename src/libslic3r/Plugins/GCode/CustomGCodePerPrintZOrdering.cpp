///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "CustomGCodePerPrintZ.hpp"

#include <cassert>
#include <optional>
#include <stdexcept>
#include <vector>

#include "CustomGCodePerPrintZRecord.hpp"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_ordering.h"
#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"
#include "libslic3r/Api/plugin/cpp/PrintingPlanViews.hpp"

/*
Custom height-marker ordering
=============================

The first pass changes only normal object extrusions for MultiAsSingle tool
markers. The default tool sorter then chooses an efficient order. The second
pass runs afterwards and forces the tool needed by a color-change event to the
front, adding an empty visit when the layer otherwise has no work for it.
*/

namespace slic3r_api::GCodeGeneration::CustomGCodePerPrintZPlugin {
namespace {

const char *const k_no_dependencies[] = {nullptr};

/*
Check whether tool-oriented markers describe the same physical printer mode as
the PrintingPlan. Multi-extruder markers must only act on a multi-extruder
plan; all single-tool variants form the other compatible family.
*/
bool tool_and_color_modes_are_compatible(Slic3r::CustomGCode::Mode print_mode,
                                         Slic3r::CustomGCode::Mode record_mode);

/*
Resolve the zero-based tool that must be active to execute one color event.
This also converts a MultiAsSingle ToolChange into a color change on a
single-nozzle printer. A missing result means the row has no executable color
event in the current mode.
*/
std::optional<uint16_t> color_event_tool(const CustomGCodeRow &row,
                                         Slic3r::CustomGCode::Mode print_mode,
                                         Slic3r::CustomGCode::Mode record_mode);

/*
Find the first tool that prints a normal object extrusion in one group. This is
the initial state of the ToolChange timeline; support and auxiliary extrusions
must not choose the object tool.
*/
std::optional<uint16_t> first_object_tool(const PrintingGroup &group);

/* Return the index of one tool visit, or uint32_t(-1) when it is absent. */
uint32_t find_tool_group(const PrintingLayerGroup &layer, uint16_t extruder_id);

/*
Move every normal object extrusion on a layer to the selected tool while
leaving support and auxiliary roots untouched. The plan API updates source
contexts during each transfer, then obsolete empty source visits are removed.
*/
void move_object_extrusions_to_tool(const PrintingGroup &group,
                                    const PrintingLayerGroup &layer,
                                    uint16_t extruder_id);

/*
Replay the MultiAsSingle ToolChange timeline independently in every sequential
printing group. Once a marker changes the selected tool, that tool remains in
effect for the following layers until another marker replaces it.
*/
void apply_tool_overrides(const CustomGCodeTable &table,
                          const PrintingPlan &plan,
                          Slic3r::CustomGCode::Mode print_mode);

/*
Prepare the first tool visit of each layer for color-change execution. All
targets are validated before mutation so conflicting markers cannot leave a
partially reordered plan; a missing visit is created intentionally as an empty
structural tool change.
*/
void apply_color_event_tools(const CustomGCodeTable &table,
                             const PrintingPlan &plan,
                             Slic3r::CustomGCode::Mode print_mode);

bool tool_and_color_modes_are_compatible(Slic3r::CustomGCode::Mode print_mode,
                                         Slic3r::CustomGCode::Mode record_mode)
{
    return (print_mode == Slic3r::CustomGCode::MultiExtruder) ==
           (record_mode == Slic3r::CustomGCode::MultiExtruder);
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
    if (row.type == Slic3r::CustomGCode::ToolChange &&
        print_mode == Slic3r::CustomGCode::SingleExtruder &&
        record_mode == Slic3r::CustomGCode::MultiAsSingle)
        return uint16_t(0);
    return std::nullopt;
}

std::optional<uint16_t> first_object_tool(const PrintingGroup &group)
{
    for (uint32_t layer_idx = 0; layer_idx < group.layer_group_count(); ++layer_idx) {
        const PrintingLayerGroup layer = group.layer_group(layer_idx);
        for (uint32_t tool_idx = 0; tool_idx < layer.tool_group_count(); ++tool_idx) {
            const PrintingToolGroup tool = layer.tool_group(tool_idx);
            for (uint32_t extrusion_idx = 0; extrusion_idx < tool.extrusion_count(); ++extrusion_idx)
                if (printing_extrusion_is_object(group, tool.extrusion(extrusion_idx)))
                    return tool.extruder_id();
        }
    }
    return std::nullopt;
}

uint32_t find_tool_group(const PrintingLayerGroup &layer, uint16_t extruder_id)
{
    for (uint32_t idx = 0; idx < layer.tool_group_count(); ++idx)
        if (layer.tool_group(idx).extruder_id() == extruder_id)
            return idx;
    return uint32_t(-1);
}

void move_object_extrusions_to_tool(const PrintingGroup &group,
                                    const PrintingLayerGroup &layer,
                                    uint16_t extruder_id)
{
    uint32_t destination_idx = find_tool_group(layer, extruder_id);
    if (destination_idx == uint32_t(-1)) {
        if (!layer.append_tool_group(extruder_id).valid())
            throw std::runtime_error("Cannot create the tool group required by a custom tool change.");
        destination_idx = layer.tool_group_count() - 1;
    }

    /* Transfer only roots sourced from normal object layers. Re-fetch views on
       every operation because vector edits may invalidate earlier child views. */
    for (uint32_t source_idx = 0; source_idx < layer.tool_group_count(); ++source_idx) {
        if (source_idx == destination_idx)
            continue;
        uint32_t extrusion_idx = 0;
        while (extrusion_idx < layer.tool_group(source_idx).extrusion_count()) {
            const PrintingExtrusion extrusion = layer.tool_group(source_idx).extrusion(extrusion_idx);
            if (!printing_extrusion_is_object(group, extrusion)) {
                ++extrusion_idx;
                continue;
            }
            if (!layer.tool_group(source_idx).transfer_extrusion_to(
                    extrusion_idx, layer.tool_group(destination_idx)))
                throw std::runtime_error("Cannot transfer an object extrusion for a custom tool change.");
        }
    }

    /* Empty source visits no longer carry work. Remove them backwards so the
       destination index does not need to be tracked across each erase. */
    for (uint32_t idx = layer.tool_group_count(); idx > 0; --idx) {
        const uint32_t tool_idx = idx - 1;
        if (layer.tool_group(tool_idx).extrusion_count() == 0 &&
            !layer.tool_group(tool_idx).events().has_before() &&
            !layer.tool_group(tool_idx).events().has_after())
            layer.remove_empty_tool_group(tool_idx);
    }
}

void apply_tool_overrides(const CustomGCodeTable &table,
                          const PrintingPlan &plan,
                          Slic3r::CustomGCode::Mode print_mode)
{
    const Slic3r::CustomGCode::Mode record_mode = effective_record_mode(table);
    if (print_mode != Slic3r::CustomGCode::MultiAsSingle ||
        record_mode != Slic3r::CustomGCode::MultiAsSingle)
        return;

    for (uint32_t group_idx = 0; group_idx < plan.group_count(); ++group_idx) {
        const PrintingGroup group = plan.group(group_idx);
        const std::vector<std::vector<const CustomGCodeRow *>> layer_rows = rows_by_layer(table, group);
        std::optional<uint16_t> selected_tool = first_object_tool(group);
        for (uint32_t layer_idx = 0; layer_idx < group.layer_group_count(); ++layer_idx) {
            for (const CustomGCodeRow *row : layer_rows[layer_idx])
                if (row->type == Slic3r::CustomGCode::ToolChange)
                    selected_tool = uint16_t(row->extruder - 1);
            if (selected_tool)
                move_object_extrusions_to_tool(group, group.layer_group(layer_idx), *selected_tool);
        }
    }
}

void apply_color_event_tools(const CustomGCodeTable &table,
                             const PrintingPlan &plan,
                             Slic3r::CustomGCode::Mode print_mode)
{
    const Slic3r::CustomGCode::Mode record_mode = effective_record_mode(table);
    std::vector<std::vector<std::optional<uint16_t>>> targets(plan.group_count());

    /* Resolve every ambiguity before creating or moving any tool group. */
    for (uint32_t group_idx = 0; group_idx < plan.group_count(); ++group_idx) {
        const PrintingGroup group = plan.group(group_idx);
        const std::vector<std::vector<const CustomGCodeRow *>> layer_rows = rows_by_layer(table, group);
        targets[group_idx].resize(group.layer_group_count());
        for (uint32_t layer_idx = 0; layer_idx < group.layer_group_count(); ++layer_idx) {
            for (const CustomGCodeRow *row : layer_rows[layer_idx]) {
                const std::optional<uint16_t> target = color_event_tool(*row, print_mode, record_mode);
                if (!target)
                    continue;
                if (targets[group_idx][layer_idx] && targets[group_idx][layer_idx] != target)
                    throw std::runtime_error(
                        "Several color changes require different tools on the same layer.");
                targets[group_idx][layer_idx] = target;
            }
        }
    }

    for (uint32_t group_idx = 0; group_idx < plan.group_count(); ++group_idx) {
        const PrintingGroup group = plan.group(group_idx);
        for (uint32_t layer_idx = 0; layer_idx < group.layer_group_count(); ++layer_idx) {
            if (!targets[group_idx][layer_idx])
                continue;
            const PrintingLayerGroup layer = group.layer_group(layer_idx);
            uint32_t tool_idx = find_tool_group(layer, *targets[group_idx][layer_idx]);
            if (tool_idx == uint32_t(-1)) {
                if (!layer.append_tool_group(*targets[group_idx][layer_idx]).valid())
                    throw std::runtime_error("Cannot create the tool visit required by a color change.");
                tool_idx = layer.tool_group_count() - 1;
            }
            if (tool_idx != 0 && !layer.move_tool_group(tool_idx, 0))
                throw std::runtime_error("Cannot place the color-change tool first on its layer.");
        }
    }
}

class CustomGCodeToolOverrides final : public PluginBase
{
public:
    static CustomGCodeToolOverrides &instance(orchestrator_handle *orchestrator)
    {
        static CustomGCodeToolOverrides plugin(orchestrator);
        return plugin;
    }
    explicit CustomGCodeToolOverrides(orchestrator_handle *orchestrator) : PluginBase(orchestrator) {}

private:
    const char *id_impl() const noexcept override { return "ordering.custom_gcode_tool_overrides"; }
    const char *name_impl() const noexcept override { return "Custom height tool overrides"; }
    const char *description_impl() const noexcept override {
        return "Moves normal object extrusions according to MultiAsSingle tool-change markers.";
    }
    slicing_step_t step_impl() const noexcept override { return STEP_ORDERING; }
    const char *const *dependencies_impl() const noexcept override { return k_no_dependencies; }
    int32_t priority_impl() const noexcept override { return 50; }
    int32_t used_config_keys(raw_used_config_key *keys) const noexcept override {
        if (keys != nullptr)
            keys[0] = raw_used_config_key{"nozzle_diameter", RAW_CO_VECTOR_FLOAT,
                                          RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE};
        return 1;
    }
    void run_impl(const plugin_run_context *run_context) const override {
        const run_ctx_extrusion_ordering *context = plugin_ctx_as_extrusion_ordering(run_context);
        assert(context != nullptr && context->print != nullptr && context->plan != nullptr);
        if (context == nullptr || context->print == nullptr || context->plan == nullptr)
            return;
        const Print print(context->print);
        const PrintingPlan plan(context->plan);
        const std::optional<CustomGCodeTable> table = read_custom_gcode_per_print_z(print);
        if (table)
            apply_tool_overrides(*table, plan, printing_plan_mode(print, plan));
    }
};

class CustomGCodeEventTools final : public PluginBase
{
public:
    static CustomGCodeEventTools &instance(orchestrator_handle *orchestrator)
    {
        static CustomGCodeEventTools plugin(orchestrator);
        return plugin;
    }
    explicit CustomGCodeEventTools(orchestrator_handle *orchestrator) : PluginBase(orchestrator) {}

private:
    const char *id_impl() const noexcept override { return "ordering.custom_gcode_event_tools"; }
    const char *name_impl() const noexcept override { return "Custom height event tools"; }
    const char *description_impl() const noexcept override {
        return "Places the tool required by a color-change marker first on its layer.";
    }
    slicing_step_t step_impl() const noexcept override { return STEP_ORDERING; }
    const char *const *dependencies_impl() const noexcept override { return k_no_dependencies; }
    int32_t priority_impl() const noexcept override { return 150; }
    int32_t used_config_keys(raw_used_config_key *keys) const noexcept override {
        if (keys != nullptr)
            keys[0] = raw_used_config_key{"nozzle_diameter", RAW_CO_VECTOR_FLOAT,
                                          RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE};
        return 1;
    }
    void run_impl(const plugin_run_context *run_context) const override {
        const run_ctx_extrusion_ordering *context = plugin_ctx_as_extrusion_ordering(run_context);
        assert(context != nullptr && context->print != nullptr && context->plan != nullptr);
        if (context == nullptr || context->print == nullptr || context->plan == nullptr)
            return;
        const Print print(context->print);
        const PrintingPlan plan(context->plan);
        const std::optional<CustomGCodeTable> table = read_custom_gcode_per_print_z(print);
        if (table)
            apply_color_event_tools(*table, plan, printing_plan_mode(print, plan));
    }
};

} // namespace

void register_custom_gcode_per_print_z_ordering_plugins(orchestrator_handle *orchestrator)
{
    orchestrator_register_plugin(orchestrator, CustomGCodeToolOverrides::instance(orchestrator).c_instance());
    orchestrator_register_plugin(orchestrator, CustomGCodeEventTools::instance(orchestrator).c_instance());
}

} // namespace slic3r_api::GCodeGeneration::CustomGCodePerPrintZPlugin
