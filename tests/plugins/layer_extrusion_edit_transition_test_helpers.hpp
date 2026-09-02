///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_tests_plugins_layer_extrusion_edit_transition_test_helpers_hpp_
#define slic3r_tests_plugins_layer_extrusion_edit_transition_test_helpers_hpp_

/*
Small transition-pipeline fixtures
==================================

The helpers build a real Print containing two independently owned printable
paths. Tests may then move the second path to another tool or layer before
running the real plugin steps. The resulting trees stay intentionally small so
scope phases and semantic process events can be inspected directly.
*/

#include <catch2/catch.hpp>

#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#include "plugin_test_helpers.hpp"
#include "test_data.hpp"

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/host/Plugin.hpp"
#include "libslic3r/Api/plugin/cpp/ExtrusionViews.hpp"
#include "libslic3r/Api/plugin/cpp/PrintingPlanViews.hpp"
#include "libslic3r/Api/plugin/cpp/properties/ExtrusionProperties.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/ExtrusionProperty.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Printing/PrintingPlan.hpp"
#include "libslic3r/Plugins/LayerExtrusionEdit/ExtrusionScopeHelpers.hpp"
#include "libslic3r/Plugins/PrintingPlan/PrintingExtrusionScopeProperty.h"
#include "libslic3r/Steps/StepExtrusionEdition.hpp"
#include "libslic3r/Steps/StepLayerExtrusionEdition.hpp"

namespace Slic3r { namespace Test { namespace TransitionPipeline {

using namespace Slic3r::Printing;

constexpr const char *RETRACTION_PLUGIN =
    "layer_extrusion_edit.retraction.default";
constexpr const char *TERMINAL_RETRACTION_PLUGIN =
    "extrusion_edit.terminal_retraction.default";
constexpr const char *TRANSITION_SCOPE_PLUGIN =
    "layer_extrusion_edit.transition_scope.default";
constexpr const char *SETTINGS_SCRIPTS_PLUGIN = "gcode.settings_scripts";

/* Preserve the process-wide active plugin set around one focused run. */
class ScopedActivePlugins
{
public:
    explicit ScopedActivePlugins(std::initializer_list<const char *> plugin_ids) :
        m_orchestrator(Orchestrator::instance())
    {
        for (Plugin *plugin : m_orchestrator.active_plugins())
            m_previous.push_back(plugin);
        m_orchestrator.clear_active_plugins();
        for (const char *plugin_id : plugin_ids)
            REQUIRE(m_orchestrator.set_plugin_active(plugin_id, true));
    }

    ~ScopedActivePlugins()
    {
        m_orchestrator.clear_active_plugins();
        for (Plugin *plugin : m_previous)
            m_orchestrator.set_plugin_active(plugin, true);
    }

private:
    Orchestrator &m_orchestrator;
    std::vector<Plugin *> m_previous;
};

/* Create one ordinary printable straight path. */
inline std::unique_ptr<ExtrusionPath> make_print_path(
    const double start_x, const double end_x)
{
    ArcPolyline polyline;
    polyline.append(Point(scale_i(start_x), 0));
    polyline.append(Point(scale_i(end_x), 0));
    return std::make_unique<ExtrusionPath>(
        polyline,
        ExtrusionAttributes(
            ExtrusionRole::Perimeter,
            ExtrusionFlow(0.2, 0.4f, 0.2f)),
        nullptr,
        true);
}

/* Append one independently rooted path to an ordered tool visit. */
inline void append_printing_extrusion(
    PrintingToolGroup &tool, std::unique_ptr<ExtrusionEntity> root)
{
    PrintingExtrusion extrusion;
    extrusion.root = std::move(root);
    extrusion.sregion_island_role = ExtrusionRole::Perimeter;
    tool.extrusions.push_back(std::move(extrusion));
}

/* Build a real Print whose full config is available through the plugin API. */
inline void prepare_print(Print &print, Model &model,
                          const char *retract_length = "2",
                          const char *toolchange_length = "2")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"retract_before_travel", "0"},
        {"retract_layer_change", "0"},
        {"retract_length", retract_length},
        {"retract_length_toolchange", toolchange_length},
        {"retract_restart_extra", "0.1"},
        {"retract_restart_extra_toolchange", "0.2"}
    });
    Slic3r::Test::init_print(
        {Slic3r::Test::TestMesh::cube_20x20x20}, print, model, config);

    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.clear();
    plan.groups.emplace_back();
    plan.groups.back().layers.emplace_back();
    PrintingLayerGroup &layer = plan.groups.back().layers.back();
    layer.print_z = scale_i(0.2);
    layer.tool_groups.emplace_back();
    PrintingToolGroup &tool = layer.tool_groups.back();
    tool.extruder_id = 0;
    append_printing_extrusion(tool, make_print_path(0.0, 10.0));
    append_printing_extrusion(tool, make_print_path(20.0, 30.0));
}

/* Return the typed runtime key shared by transition plugins. */
inline slic3r_api::PluginPropertyKey<slic3r_api::PrintingExtrusionScopeProperty>
scope_property_key()
{
    return slic3r_api::printing_extrusion_scope_property_key(
        reinterpret_cast<orchestrator_handle *>(&Orchestrator::instance()));
}

/* Open one core extrusion root through the plugin API. */
inline slic3r_api::MutableExtrusionEntity entity_view(ExtrusionEntity &entity)
{
    return slic3r_api::MutableExtrusionEntity(
        reinterpret_cast<extrusion_entity_handle *>(&entity));
}

/* Find a marked compact scope without descending into another marked scope. */
inline slic3r_api::MutableExtrusionEntity find_scope(
    slic3r_api::MutableExtrusionEntity entity)
{
    if (entity.get(scope_property_key()) != nullptr)
        return entity;
    for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx) {
        slic3r_api::MutableExtrusionEntity found = find_scope(
            entity.child_mutable(child_idx));
        if (found.valid())
            return found;
    }
    return slic3r_api::MutableExtrusionEntity();
}

/* Collect semantic events in their final depth-first execution order. */
inline void collect_transition_events(
    const slic3r_api::ExtrusionEntity &entity,
    std::vector<std::string> &events)
{
    if (const slic3r_api::EPropertyCustomGcode *script =
            entity.get(slic3r_api::EPropertyCustomGcode::key)) {
        if (script->script_type == GCODE_SCRIPT_TYPE_TOOLCHANGE_GCODE)
            events.push_back("toolchange_script");
        if (script->script_type == GCODE_SCRIPT_TYPE_START_FILAMENT_GCODE)
            events.push_back("start_filament_script");
    }
    if (const slic3r_api::EPropertySpecialCommand *command =
            entity.get(slic3r_api::EPropertySpecialCommand::key))
        if (command->code == C_EXTRUSION_SPECIAL_COMMAND_TOOLCHANGE)
            events.push_back("toolchange");
    if (const slic3r_api::EPropertyAttributes *attributes =
            entity.get(slic3r_api::EPropertyAttributes::key)) {
        if (RAW_EXTRUSION_ROLE_IS_RETRACT(attributes->extrusion_role()))
            events.push_back("retract");
        if (RAW_EXTRUSION_ROLE_IS_UNRETRACT(attributes->extrusion_role()))
            events.push_back("unretract");
    }
    for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        collect_transition_events(entity.child(child_idx), events);
}

/* Run the selected layer plugins through the real parallel host. */
inline void run_layer_plugins(
    Print &print, std::initializer_list<const char *> plugin_ids)
{
    ScopedActivePlugins active(plugin_ids);
    Orchestrator &orchestrator = Orchestrator::instance();
    orchestrator.reset_plugin_cancel();
    Steps::StepLayerExtrusionEdition::run_step(orchestrator, print);
    REQUIRE_FALSE(orchestrator.is_plugin_cancelled());
}

/* Run the selected sequential extrusion-edit plugins. */
inline void run_plan_plugins(
    Print &print, std::initializer_list<const char *> plugin_ids)
{
    ScopedActivePlugins active(plugin_ids);
    Orchestrator &orchestrator = Orchestrator::instance();
    orchestrator.reset_plugin_cancel();
    Steps::StepExtrusionEdition::run_step(orchestrator, print);
    REQUIRE_FALSE(orchestrator.is_plugin_cancelled());
}

}}} // namespace Slic3r::Test::TransitionPipeline

#endif // slic3r_tests_plugins_layer_extrusion_edit_transition_test_helpers_hpp_
