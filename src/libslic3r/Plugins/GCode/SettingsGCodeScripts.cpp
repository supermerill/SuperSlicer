///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "SettingsGCodeScripts.hpp"

#include <algorithm>
#include <cassert>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>

#include "libslic3r/Api/plugin/c/steps/slic3r_step_extrusion_edit.h"
#include "libslic3r/Api/plugin/cpp/ConfigViews.hpp"
#include "libslic3r/Api/plugin/cpp/DataTreeViews.hpp"
#include "libslic3r/Api/plugin/cpp/ExtrusionViews.hpp"
#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"
#include "libslic3r/Api/plugin/cpp/PrintingPlanViews.hpp"
#include "libslic3r/Api/plugin/cpp/properties/ExtrusionProperties.hpp"
#include "libslic3r/Plugins/LayerExtrusionEdit/ExtrusionScopeHelpers.hpp"
#include "libslic3r/Plugins/PrintingPlan/PrintingExtrusionScopeProperty.h"

/*
Configured G-code script placement
==================================

This plugin translates configuration into ordered PrintingPlan events. Each
event carries both its semantic script type and the immutable structural
PlaceholderParser values known at insertion time. Firmware sessions therefore
only need to add current machine state when the event is serialized.
*/

namespace slic3r_api { namespace GCodeGeneration { namespace SettingsGCodeScriptsPlugin {
namespace {

const char *const k_no_dependencies[] = {nullptr};

enum class ScopeEventPosition : uint8_t { Before, After };

struct LayerContext
{
    int32_t number = -1;
    coord_t print_z = 0;
};

struct PlanSummary
{
    coord_t max_print_z = 0;
    std::optional<uint16_t> first_extruder;
    std::optional<uint16_t> last_extruder;
    std::optional<LayerContext> first_layer;
    std::optional<LayerContext> previous_layer;
    std::optional<LayerContext> last_layer;
};

// Read global facts once before scripts are inserted, including values needed by plan-level events.
PlanSummary summarize_plan(const PrintingPlan &plan);
// Resolve a one-object group to the stable object index expected by PlaceholderParser.
std::optional<uint32_t> group_object_index(const Print &print, const PrintingGroup &group);
// Add one typed scalar to the temporary Config transported with a script.
void set_int_argument(MutableConfig &config, const char *key, int32_t value);
void set_float_argument(MutableConfig &config, const char *key, double value);
// Build the common layer placeholders from the final global traversal position.
StoredConfig layer_arguments(storage_handle *storage,
                             const LayerContext &layer,
                             const std::optional<LayerContext> &previous_layer,
                             coord_t max_print_z);
// Clone a small producer Config before adding values specific to one event.
StoredConfig copy_arguments(storage_handle *storage, const Config &source);
// Build one temporary event and move its contents into the selected fixed scope root.
void append_script_event(storage_handle *storage,
                         const PrintingScopeEvents &events,
                         const std::string &script,
                         gcode_script_type script_type,
                         const Config &arguments,
                         ScopeEventPosition position,
                         uint16_t processing_extruder_id = GCODE_SCRIPT_PROCESSING_EXTRUDER_INVALID);
// Search one tree for the first scope whose incoming boundary selects a tool.
MutableExtrusionEntity first_toolchange_scope_in_entity(
    MutableExtrusionEntity entity,
    const PluginPropertyKey<PrintingExtrusionScopeProperty> &scope_key);
// Return the first tool-change scope owned by one tool visit.
MutableExtrusionEntity first_toolchange_scope(
    const PrintingToolGroup &tool_group,
    const PluginPropertyKey<PrintingExtrusionScopeProperty> &scope_key);
// Find the semantic Unretract event in one phase subtree.
MutableExtrusionEntity find_unretract(MutableExtrusionEntity entity);
// Insert start-filament immediately before Unretract, or first in before.
void append_start_filament_script(
    MutableExtrusionEntity scope_root,
    const PluginPropertyKey<PrintingExtrusionScopeProperty> &scope_key,
    const std::string &script,
    const Config &arguments,
    uint16_t processing_extruder_id);

PlanSummary summarize_plan(const PrintingPlan &plan)
{
    PlanSummary summary;
    int32_t layer_number = 0;
    for (uint32_t group_idx = 0; group_idx < plan.group_count(); ++group_idx) {
        const PrintingGroup group = plan.group(group_idx);
        for (uint32_t layer_idx = 0; layer_idx < group.layer_group_count(); ++layer_idx) {
            const PrintingLayerGroup layer = group.layer_group(layer_idx);
            summary.max_print_z = std::max(summary.max_print_z, layer.print_z());
            const LayerContext current_layer{layer_number++, layer.print_z()};
            if (!summary.first_layer)
                summary.first_layer = current_layer;
            summary.previous_layer = summary.last_layer;
            summary.last_layer = current_layer;
            for (uint32_t tool_idx = 0; tool_idx < layer.tool_group_count(); ++tool_idx) {
                const uint16_t extruder_id = layer.tool_group(tool_idx).extruder_id();
                if (extruder_id == uint16_t(-1))
                    continue;
                if (!summary.first_extruder)
                    summary.first_extruder = extruder_id;
                summary.last_extruder = extruder_id;
            }
        }
    }
    return summary;
}

std::optional<uint32_t> group_object_index(const Print &print, const PrintingGroup &group)
{
    if (group.object_instance_count() != 1)
        return std::nullopt;
    const Object object = group.object_instance(0).object();
    for (uint32_t object_idx = 0; object_idx < print.object_count(); ++object_idx)
        if (print.object(object_idx).handle() == object.handle())
            return object_idx;
    return std::nullopt;
}

void set_int_argument(MutableConfig &config, const char *key, int32_t value)
{
    config.get_or_add(key, SLIC3R_CONFIG_OPTION_INT).set_int(value);
}

void set_float_argument(MutableConfig &config, const char *key, double value)
{
    config.get_or_add(key, SLIC3R_CONFIG_OPTION_FLOAT).set_float(value);
}

StoredConfig layer_arguments(storage_handle *storage,
                             const LayerContext &layer,
                             const std::optional<LayerContext> &previous_layer,
                             coord_t max_print_z)
{
    StoredConfig arguments(storage);
    set_int_argument(arguments, "layer_num", layer.number);
    set_float_argument(arguments, "layer_z", unscaled(layer.print_z));
    set_float_argument(arguments, "previous_layer_z",
                       previous_layer ? unscaled(previous_layer->print_z) : 0.0);
    set_float_argument(arguments, "max_layer_z", unscaled(max_print_z));
    return arguments;
}

StoredConfig copy_arguments(storage_handle *storage, const Config &source)
{
    StoredConfig copy(storage);
    copy.deserialize_all(source.serialize_all());
    return copy;
}

void append_script_event(storage_handle *storage,
                         const PrintingScopeEvents &events,
                         const std::string &script,
                         gcode_script_type script_type,
                         const Config &arguments,
                         ScopeEventPosition position,
                         uint16_t processing_extruder_id)
{
    if (script.empty())
        return;

    // The fixed scope root takes ownership of the complete temporary event,
    // including its text and typed argument buffers.
    StoredExtrusionEntity event(storage);
    event.script_gcode(script, script_type, arguments, processing_extruder_id);
    if (position == ScopeEventPosition::Before)
        events.append_before_move(event.mutable_view());
    else
        events.append_after_move(event.mutable_view());
}

MutableExtrusionEntity first_toolchange_scope_in_entity(
    MutableExtrusionEntity entity,
    const PluginPropertyKey<PrintingExtrusionScopeProperty> &scope_key)
{
    if (LayerExtrusionEdit::ExtrusionScope::is_scope(
            entity.readonly(), scope_key)) {
        const PrintingExtrusionScopeProperty *property = entity.get(scope_key);
        if (property != nullptr && printing_extrusion_scope_has_flag(
                *property, PRINTING_EXTRUSION_SCOPE_INCOMING_TOOLCHANGE))
            return entity;
        // Compact scopes never nest, so their large content subtree cannot
        // contain another matching tool-change scope.
        return MutableExtrusionEntity();
    }
    for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx) {
        MutableExtrusionEntity found = first_toolchange_scope_in_entity(
            entity.child_mutable(child_idx), scope_key);
        if (found.valid())
            return found;
    }
    return MutableExtrusionEntity();
}

MutableExtrusionEntity first_toolchange_scope(
    const PrintingToolGroup &tool_group,
    const PluginPropertyKey<PrintingExtrusionScopeProperty> &scope_key)
{
    for (uint32_t extrusion_idx = 0;
         extrusion_idx < tool_group.extrusion_count(); ++extrusion_idx) {
        MutableExtrusionEntity found = first_toolchange_scope_in_entity(
            tool_group.extrusion(extrusion_idx).mutable_root(), scope_key);
        if (found.valid())
            return found;
    }
    return MutableExtrusionEntity();
}

MutableExtrusionEntity find_unretract(MutableExtrusionEntity entity)
{
    const EPropertyAttributes *attributes = entity.get(EPropertyAttributes::key);
    if (attributes != nullptr &&
        RAW_EXTRUSION_ROLE_IS_UNRETRACT(attributes->extrusion_role()))
        return entity;
    for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx) {
        MutableExtrusionEntity found = find_unretract(
            entity.child_mutable(child_idx));
        if (found.valid())
            return found;
    }
    return MutableExtrusionEntity();
}

void append_start_filament_script(
    MutableExtrusionEntity scope_root,
    const PluginPropertyKey<PrintingExtrusionScopeProperty> &scope_key,
    const std::string &script,
    const Config &arguments,
    const uint16_t processing_extruder_id)
{
    if (script.empty())
        return;
    const LayerExtrusionEdit::ExtrusionScope::OrderedExtrusionScope scope(
        scope_root, scope_key);
    MutableExtrusionEntity before = scope.before();
    if (!before.valid())
        throw std::runtime_error(
            "A start-filament script needs an incoming transition phase.");

    // When retraction exists, anchor directly on its Unretract event so the
    // script remains the last operation before restoring E. Otherwise place
    // it at the beginning of the target's before phase; tool selection lives
    // outside the scope in PrintingToolGroup::events().before.
    MutableExtrusionEntity unretract = find_unretract(before);
    MutableExtrusionEntity event;
    if (unretract.valid()) {
        event = unretract.emplace_ordered_leaf(
            OrderedLeafPosition::Before,
            ExistingPropertyPlacement::MoveWithExistingContent);
    } else if (before.segment_count() == 0 && before.child_count() == 0 &&
               before.property_count() == 0) {
        event = before;
    } else {
        event = before.emplace_ordered_leaf(
            OrderedLeafPosition::Before,
            ExistingPropertyPlacement::MoveWithExistingContent);
    }
    if (!event.valid())
        throw std::runtime_error(
            "Unable to insert start-filament into an ordered tool change.");
    event.script_gcode(
        script, GCODE_SCRIPT_TYPE_START_FILAMENT_GCODE,
        arguments, processing_extruder_id);
}

class SettingsGCodeScripts final : public PluginBase
{
public:
    static SettingsGCodeScripts &instance(orchestrator_handle *orchestrator) {
        static SettingsGCodeScripts plugin(orchestrator);
        return plugin;
    }

    explicit SettingsGCodeScripts(orchestrator_handle *orchestrator) :
        PluginBase(orchestrator),
        m_scope_property(printing_extrusion_scope_property_key(orchestrator))
    {}

private:
    const char *id_impl() const noexcept override { return "gcode.settings_scripts"; }
    const char *name_impl() const noexcept override { return "Configured G-code scripts"; }
    const char *description_impl() const noexcept override {
        return "Places configured print, layer, tool and object scripts in the ordered printing plan.";
    }
    slicing_step_t step_impl() const noexcept override { return STEP_EXTRUSION_EDIT; }
    const char *const *dependencies_impl() const noexcept override { return k_no_dependencies; }
    int32_t priority_impl() const noexcept override { return 0; }
    int32_t used_config_keys(raw_used_config_key *keys) const noexcept override {
        if (keys == nullptr)
            return 9;
        keys[0] = raw_used_config_key{"start_gcode", RAW_CO_STRING, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE};
        keys[1] = raw_used_config_key{"end_gcode", RAW_CO_STRING, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE};
        keys[2] = raw_used_config_key{"before_layer_gcode", RAW_CO_STRING, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE};
        keys[3] = raw_used_config_key{"layer_gcode", RAW_CO_STRING, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE};
        keys[4] = raw_used_config_key{"between_objects_gcode", RAW_CO_STRING, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE};
        keys[5] = raw_used_config_key{"between_objects_gcode_before_move", RAW_CO_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE};
        keys[6] = raw_used_config_key{"start_filament_gcode", RAW_CO_VECTOR_STRING, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE};
        keys[7] = raw_used_config_key{"end_filament_gcode", RAW_CO_VECTOR_STRING, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE};
        keys[8] = raw_used_config_key{"single_extruder_multi_material", RAW_CO_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE};
        return 9;
    }
    void run_impl(const plugin_run_context *run_context) const override {
        const run_ctx_extrusion_edition *context = plugin_ctx_as_extrusion_edition(run_context);
        assert(context != nullptr && context->print != nullptr && context->plan != nullptr);
        if (context == nullptr || context->print == nullptr || context->plan == nullptr ||
            run_context->plugin_storage == nullptr)
            return;

        const Print print(context->print);
        const Config config = print.config();
        const PrintingPlan plan(context->plan);
        const PrintingScopeEvents plan_events = plan.events();
        const PlanSummary summary = summarize_plan(plan);

        const LayerContext first_layer = summary.first_layer.value_or(LayerContext{});
        StoredConfig start_arguments = layer_arguments(
            run_context->plugin_storage, first_layer, std::nullopt, summary.max_print_z);
        set_int_argument(start_arguments, "previous_extruder", -1);
        set_int_argument(start_arguments, "next_extruder",
                         summary.first_extruder ? int32_t(*summary.first_extruder) : -1);
        set_int_argument(start_arguments, "filament_extruder_id",
                         summary.first_extruder ? int32_t(*summary.first_extruder) : 0);
        append_script_event(run_context->plugin_storage, plan_events,
                            config.string_or_default("start_gcode", std::string()),
                            GCODE_SCRIPT_TYPE_START_GCODE, start_arguments,
                            ScopeEventPosition::Before,
                            summary.first_extruder.value_or(GCODE_SCRIPT_PROCESSING_EXTRUDER_INVALID));

        const std::string before_layer = config.string_or_default("before_layer_gcode", std::string());
        const std::string after_layer = config.string_or_default("layer_gcode", std::string());
        const std::string between_objects = config.string_or_default("between_objects_gcode", std::string());
        const bool between_before_move = config.bool_or_default("between_objects_gcode_before_move", false);
        const bool single_extruder_multi_material =
            config.bool_or_default("single_extruder_multi_material", false);
        int32_t current_tool = -1;
        int32_t layer_number = 0;
        bool seen_layer = false;
        std::set<uint16_t> used_tools;
        std::optional<uint32_t> previous_object;
        std::optional<LayerContext> previous_layer;
        std::optional<PrintingScopeEvents> previous_object_events;
        std::optional<PrintingScopeEvents> previous_tool_events;

        // The plan hierarchy is not resized during this pass. Borrowed event
        // roots therefore remain valid while scripts are appended to them.
        for (uint32_t group_idx = 0; group_idx < plan.group_count(); ++group_idx) {
            const PrintingGroup group = plan.group(group_idx);
            const PrintingScopeEvents group_events = group.events();
            const std::optional<uint32_t> current_object = group_object_index(print, group);
            if (current_object && previous_object && !between_objects.empty()) {
                StoredConfig arguments(run_context->plugin_storage);
                const LayerContext object_layer = previous_layer.value_or(LayerContext{});
                set_int_argument(arguments, "layer_num", object_layer.number);
                set_float_argument(arguments, "layer_z", unscaled(object_layer.print_z));
                set_int_argument(arguments, "previous_object_id", int32_t(*previous_object));
                set_int_argument(arguments, "next_object_id", int32_t(*current_object));
                if (between_before_move) {
                    assert(previous_object_events.has_value());
                    append_script_event(run_context->plugin_storage, *previous_object_events,
                                        between_objects, GCODE_SCRIPT_TYPE_BETWEEN_OBJECTS_GCODE,
                                        arguments, ScopeEventPosition::After);
                } else {
                    append_script_event(run_context->plugin_storage, group_events,
                                        between_objects, GCODE_SCRIPT_TYPE_BETWEEN_OBJECTS_GCODE,
                                        arguments, ScopeEventPosition::Before);
                }
            }
            if (current_object) {
                previous_object = current_object;
                previous_object_events = group_events;
            }

            std::optional<PrintingScopeEvents> previous_layer_events;
            for (uint32_t layer_idx = 0; layer_idx < group.layer_group_count(); ++layer_idx) {
                const PrintingLayerGroup layer = group.layer_group(layer_idx);
                const PrintingScopeEvents layer_events = layer.events();
                const LayerContext current_layer{layer_number++, layer.print_z()};
                const StoredConfig current_layer_arguments = layer_arguments(
                    run_context->plugin_storage, current_layer, previous_layer, summary.max_print_z);

                const PrintingScopeEvents before_layer_events = previous_layer_events ?
                    *previous_layer_events : group_events;
                const ScopeEventPosition before_layer_position = previous_layer_events ?
                    ScopeEventPosition::After : ScopeEventPosition::Before;
                append_script_event(run_context->plugin_storage, before_layer_events, before_layer,
                                    GCODE_SCRIPT_TYPE_BEFORE_LAYER_GCODE, current_layer_arguments,
                                    before_layer_position);
                const bool first_layer_of_sequential_object = current_object && layer_idx == 0;
                if (seen_layer && !first_layer_of_sequential_object)
                    append_script_event(run_context->plugin_storage, layer_events, after_layer,
                                        GCODE_SCRIPT_TYPE_LAYER_GCODE, current_layer_arguments,
                                        ScopeEventPosition::Before);
                seen_layer = true;

                for (uint32_t tool_idx = 0; tool_idx < layer.tool_group_count(); ++tool_idx) {
                    const PrintingToolGroup tool_group = layer.tool_group(tool_idx);
                    const PrintingScopeEvents tool_events = tool_group.events();
                    const uint16_t target_tool = tool_group.extruder_id();
                    used_tools.insert(target_tool);
                    if (current_tool == int32_t(target_tool)) {
                        previous_tool_events = tool_events;
                        continue;
                    }

                    StoredConfig tool_arguments = copy_arguments(
                        run_context->plugin_storage, current_layer_arguments);
                    set_int_argument(tool_arguments, "previous_extruder", current_tool);
                    set_int_argument(tool_arguments, "next_extruder", int32_t(target_tool));
                    set_float_argument(tool_arguments, "toolchange_z", unscaled(current_layer.print_z));
                    if (current_tool >= 0) {
                        assert(previous_tool_events.has_value());
                        StoredConfig end_filament_arguments = copy_arguments(
                            run_context->plugin_storage, tool_arguments);
                        set_int_argument(end_filament_arguments, "filament_extruder_id", current_tool);
                        append_script_event(
                            run_context->plugin_storage, *previous_tool_events,
                            config.vector_string_or_default("end_filament_gcode", uint32_t(current_tool), std::string()),
                            GCODE_SCRIPT_TYPE_END_FILAMENT_GCODE, end_filament_arguments,
                            ScopeEventPosition::After, uint16_t(current_tool));
                    }
                    const MutableExtrusionEntity transition_scope = current_tool >= 0 ?
                        first_toolchange_scope(tool_group, m_scope_property) :
                        MutableExtrusionEntity();
                    StoredConfig start_filament_arguments = copy_arguments(
                        run_context->plugin_storage, tool_arguments);
                    set_int_argument(start_filament_arguments, "filament_extruder_id", int32_t(target_tool));
                    const std::string start_filament =
                        config.vector_string_or_default(
                            "start_filament_gcode", target_tool, std::string());
                    if (transition_scope.valid())
                        append_start_filament_script(
                            transition_scope, m_scope_property, start_filament,
                            start_filament_arguments,
                            target_tool);
                    else
                        append_script_event(
                            run_context->plugin_storage, tool_events,
                            start_filament,
                            GCODE_SCRIPT_TYPE_START_FILAMENT_GCODE,
                            start_filament_arguments,
                            ScopeEventPosition::Before, target_tool);
                    current_tool = target_tool;
                    previous_tool_events = tool_events;
                }
                previous_layer = current_layer;
                previous_layer_events = layer_events;
            }
        }

        if (current_tool >= 0) {
            const std::set<uint16_t> final_tools = single_extruder_multi_material ?
                std::set<uint16_t>{uint16_t(current_tool)} : used_tools;
            for (uint16_t tool_id : final_tools) {
                StoredConfig arguments(run_context->plugin_storage);
                if (summary.last_layer)
                    arguments.deserialize_all(layer_arguments(
                        run_context->plugin_storage, *summary.last_layer,
                        summary.previous_layer, summary.max_print_z).serialize_all());
                set_int_argument(arguments, "filament_extruder_id", int32_t(tool_id));
                set_int_argument(arguments, "previous_extruder", current_tool);
                set_int_argument(arguments, "next_extruder", -1);
                append_script_event(
                    run_context->plugin_storage, plan_events,
                    config.vector_string_or_default("end_filament_gcode", tool_id, std::string()),
                    GCODE_SCRIPT_TYPE_END_FILAMENT_GCODE, arguments,
                    ScopeEventPosition::After, tool_id);
            }
        }

        const LayerContext last_layer = summary.last_layer.value_or(LayerContext{});
        StoredConfig end_arguments = layer_arguments(
            run_context->plugin_storage, last_layer, summary.previous_layer, summary.max_print_z);
        set_int_argument(end_arguments, "previous_extruder",
                         summary.last_extruder ? int32_t(*summary.last_extruder) : -1);
        set_int_argument(end_arguments, "next_extruder", -1);
        set_int_argument(end_arguments, "filament_extruder_id",
                         summary.last_extruder ? int32_t(*summary.last_extruder) : 0);
        append_script_event(run_context->plugin_storage, plan_events,
                            config.string_or_default("end_gcode", std::string()),
                            GCODE_SCRIPT_TYPE_END_GCODE, end_arguments,
                            ScopeEventPosition::After,
                            summary.last_extruder.value_or(GCODE_SCRIPT_PROCESSING_EXTRUDER_INVALID));
    }

    PluginPropertyKey<PrintingExtrusionScopeProperty> m_scope_property;
};

} // namespace

void register_settings_gcode_scripts_plugin(orchestrator_handle *orchestrator)
{
    orchestrator_register_plugin(orchestrator, SettingsGCodeScripts::instance(orchestrator).c_instance());
}

}}} // namespace slic3r_api::GCodeGeneration::SettingsGCodeScriptsPlugin
