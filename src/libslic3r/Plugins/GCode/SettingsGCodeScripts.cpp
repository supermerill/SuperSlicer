///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "SettingsGCodeScripts.hpp"

#include <algorithm>
#include <cassert>
#include <optional>
#include <set>
#include <string>

#include "libslic3r/Api/plugin/c/steps/slic3r_step_extrusion_edit.h"
#include "libslic3r/Api/plugin/cpp/DataTreeViews.hpp"
#include "libslic3r/Api/plugin/cpp/ExtrusionViews.hpp"
#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"
#include "libslic3r/Api/plugin/cpp/PrintingPlanViews.hpp"

/*
Configured G-code script placement
==================================

This plugin translates configuration into ordered PrintingPlan events. It does
not parse placeholders and does not inspect firmware syntax. The SCRIPT
property carries a semantic script type so the firmware can prepare the right
host-owned PlaceholderParser context only when final machine state is known.
*/

namespace slic3r_api { namespace GCodeGeneration { namespace SettingsGCodeScriptsPlugin {
namespace {

const char *const k_no_dependencies[] = {nullptr};

enum class ScopeEventPosition : uint8_t { Before, After };

// Build one temporary event and move its contents into the selected fixed scope root.
void append_script_event(storage_handle *storage,
                         const PrintingScopeEvents &events,
                         const std::string &script,
                         gcode_script_type script_type,
                         ScopeEventPosition position,
                         uint16_t target_extruder_id = GCODE_SCRIPT_TARGET_EXTRUDER_INVALID);
// Whitespace-only toolchange scripts have the same meaning as an empty legacy setting.
bool has_visible_text(const std::string &text);

void append_script_event(storage_handle *storage,
                         const PrintingScopeEvents &events,
                         const std::string &script,
                         gcode_script_type script_type,
                         ScopeEventPosition position,
                         uint16_t target_extruder_id) {
    if (script.empty())
        return;

    /*
    StoredExtrusionEntity owns the temporary handle. append_*_move transfers
    its contents into the host-owned event tree; destroying the emptied
    temporary then releases only its storage wrapper.
    */
    StoredExtrusionEntity event(storage);
    event.script_gcode(script, script_type, target_extruder_id);
    if (position == ScopeEventPosition::Before)
        events.append_before_move(event.mutable_view());
    else
        events.append_after_move(event.mutable_view());
}

bool has_visible_text(const std::string &text)
{
    return text.find_first_not_of(" \t\r\n") != std::string::npos;
}

class SettingsGCodeScripts final : public PluginBase
{
public:
    static SettingsGCodeScripts &instance(orchestrator_handle *orchestrator) {
        static SettingsGCodeScripts plugin(orchestrator);
        return plugin;
    }

    explicit SettingsGCodeScripts(orchestrator_handle *orchestrator) : PluginBase(orchestrator) {}

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
            return 11;
        keys[0] = raw_used_config_key{"start_gcode", RAW_CO_STRING, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE};
        keys[1] = raw_used_config_key{"end_gcode", RAW_CO_STRING, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE};
        keys[2] = raw_used_config_key{"before_layer_gcode", RAW_CO_STRING, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE};
        keys[3] = raw_used_config_key{"layer_gcode", RAW_CO_STRING, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE};
        keys[4] = raw_used_config_key{"toolchange_gcode", RAW_CO_STRING, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE};
        keys[5] = raw_used_config_key{"between_objects_gcode", RAW_CO_STRING, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE};
        keys[6] = raw_used_config_key{"between_objects_gcode_before_move", RAW_CO_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE};
        keys[7] = raw_used_config_key{"start_filament_gcode", RAW_CO_VECTOR_STRING, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE};
        keys[8] = raw_used_config_key{"end_filament_gcode", RAW_CO_VECTOR_STRING, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE};
        keys[9] = raw_used_config_key{"single_extruder_multi_material", RAW_CO_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE};
        keys[10] = raw_used_config_key{"nozzle_diameter", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE};
        return 11;
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
        append_script_event(run_context->plugin_storage, plan_events,
                            config.string_or_default("start_gcode", std::string()), GCODE_SCRIPT_TYPE_START_GCODE,
                            ScopeEventPosition::Before);

        const std::string before_layer = config.string_or_default("before_layer_gcode", std::string());
        const std::string after_layer = config.string_or_default("layer_gcode", std::string());
        const std::string toolchange = config.string_or_default("toolchange_gcode", std::string());
        const std::string between_objects = config.string_or_default("between_objects_gcode", std::string());
        const bool between_before_move = config.bool_or_default("between_objects_gcode_before_move", false);
        const bool single_extruder_multi_material =
            config.bool_or_default("single_extruder_multi_material", false);
        const uint32_t configured_extruders = config.has("nozzle_diameter") ? config.get("nozzle_diameter").size() : 1;
        int32_t current_tool = -1;
        bool seen_layer = false;
        std::set<uint16_t> used_tools;
        bool previous_object = false;
        // The plan hierarchy is not resized during this pass. These borrowed
        // event views therefore remain valid while scripts are appended to
        // their fixed before/after roots.
        std::optional<PrintingScopeEvents> previous_object_events;
        std::optional<PrintingScopeEvents> previous_tool_events;

        for (uint32_t group_idx = 0; group_idx < plan.group_count(); ++group_idx) {
            const PrintingGroup group = plan.group(group_idx);
            const PrintingScopeEvents group_events = group.events();
            const bool current_object = group.object_instance_count() == 1;
            if (current_object && previous_object && !between_objects.empty()) {
                if (between_before_move) {
                    assert(previous_object_events.has_value());
                    append_script_event(run_context->plugin_storage, *previous_object_events,
                                        between_objects, GCODE_SCRIPT_TYPE_BETWEEN_OBJECTS_GCODE,
                                        ScopeEventPosition::After);
                } else {
                    append_script_event(run_context->plugin_storage, group_events, between_objects,
                                        GCODE_SCRIPT_TYPE_BETWEEN_OBJECTS_GCODE,
                                        ScopeEventPosition::Before);
                }
            }
            if (current_object) {
                previous_object = true;
                previous_object_events = group_events;
            }

            std::optional<PrintingScopeEvents> previous_layer_events;

            for (uint32_t layer_idx = 0; layer_idx < group.layer_group_count(); ++layer_idx) {
                const PrintingLayerGroup layer = group.layer_group(layer_idx);
                const PrintingScopeEvents layer_events = layer.events();

                const PrintingScopeEvents before_layer_events = previous_layer_events ?
                    *previous_layer_events : group_events;
                const ScopeEventPosition before_layer_position = previous_layer_events ?
                    ScopeEventPosition::After : ScopeEventPosition::Before;
                append_script_event(run_context->plugin_storage, before_layer_events, before_layer,
                                    GCODE_SCRIPT_TYPE_BEFORE_LAYER_GCODE, before_layer_position);
                const bool first_layer_of_sequential_object = current_object && layer_idx == 0;
                if (seen_layer && !first_layer_of_sequential_object)
                    append_script_event(run_context->plugin_storage, layer_events, after_layer,
                                        GCODE_SCRIPT_TYPE_LAYER_GCODE, ScopeEventPosition::Before);
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

                    if (current_tool >= 0) {
                        assert(previous_tool_events.has_value());
                        append_script_event(
                            run_context->plugin_storage, *previous_tool_events,
                            config.vector_string_or_default("end_filament_gcode", uint32_t(current_tool), std::string()),
                            GCODE_SCRIPT_TYPE_END_FILAMENT_GCODE, ScopeEventPosition::After,
                            uint16_t(current_tool));
                    }
                    if (current_tool >= 0 && configured_extruders > 1 && has_visible_text(toolchange))
                        append_script_event(run_context->plugin_storage, tool_events, toolchange,
                                            GCODE_SCRIPT_TYPE_TOOLCHANGE_GCODE, ScopeEventPosition::Before);

                    append_script_event(
                        run_context->plugin_storage, tool_events,
                        config.vector_string_or_default("start_filament_gcode", target_tool, std::string()),
                        GCODE_SCRIPT_TYPE_START_FILAMENT_GCODE, ScopeEventPosition::Before,
                        target_tool);
                    current_tool = target_tool;
                    previous_tool_events = tool_events;
                }
                previous_layer_events = layer_events;
            }
        }

        if (current_tool >= 0) {
            const std::set<uint16_t> final_tools = single_extruder_multi_material ?
                std::set<uint16_t>{uint16_t(current_tool)} : used_tools;
            for (uint16_t tool_id : final_tools) {
                append_script_event(
                    run_context->plugin_storage, plan_events,
                    config.vector_string_or_default("end_filament_gcode", tool_id, std::string()),
                    GCODE_SCRIPT_TYPE_END_FILAMENT_GCODE, ScopeEventPosition::After,
                    tool_id);
            }
        }

        append_script_event(run_context->plugin_storage, plan_events,
                            config.string_or_default("end_gcode", std::string()),
                            GCODE_SCRIPT_TYPE_END_GCODE, ScopeEventPosition::After);
    }
};

} // namespace

void register_settings_gcode_scripts_plugin(orchestrator_handle *orchestrator) {
    orchestrator_register_plugin(orchestrator, SettingsGCodeScripts::instance(orchestrator).c_instance());
}

}}} // namespace slic3r_api::GCodeGeneration::SettingsGCodeScriptsPlugin
