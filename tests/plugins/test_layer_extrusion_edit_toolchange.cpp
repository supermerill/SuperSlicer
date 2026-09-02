///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

/*
CreateToolChange tests
======================

These cases verify that physical tool selection is materialized before compact
transition scopes exist. The first tool remains implicit, while every later
change belongs to the target PrintingToolGroup's before events. Empty tool
visits participate in the same chronology because the firmware enters their
tool-group even when they contain no printable extrusion.
*/

#include "gcode_test_helpers.hpp"
#include "layer_extrusion_edit_transition_test_helpers.hpp"

#include <string>
#include <vector>

namespace {

using namespace Slic3r;
using namespace Slic3r::Printing;
using namespace Slic3r::Test::GCode;
using namespace Slic3r::Test::TransitionPipeline;

/* Return the first event stored before one tool visit. */
const ExtrusionEntity &first_before_event(const PrintingToolGroup &tool_group)
{
    REQUIRE(tool_group.events.has_before());
    REQUIRE(tool_group.events.before().child_count() == 1);
    return tool_group.events.before().child(0);
}

/* Read a semantic tool-selection event without depending on firmware output. */
const ExtrusionPropertySpecialCommand *semantic_toolchange(
    const PrintingToolGroup &tool_group)
{
    if (!tool_group.events.has_before())
        return nullptr;
    return first_before_event(tool_group)
        .get_property<ExtrusionPropertySpecialCommand>();
}

TEST_CASE("CreateToolChange registers before compact transition scopes",
          "[plugins][layer-extrusion-edit][toolchange][registration]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Orchestrator &orchestrator = Orchestrator::instance();
    Plugin *plugin = orchestrator.get_plugin(TOOLCHANGE_PLUGIN);
    REQUIRE(plugin != nullptr);
    CHECK(plugin->get_priority() == -110);
    CHECK(plugin->get_exclusive_group() ==
          "layer_extrusion_edit.toolchange");
    CHECK(plugin->get_dependencies().empty());

}

TEST_CASE("CreateToolChange follows the complete ordered tool chronology",
          "[plugins][layer-extrusion-edit][toolchange][chronology]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Print print;
    Model model;
    prepare_print(print, model);
    PrintConfig &config = const_cast<PrintConfig &>(print.config());
    config.toolchange_gcode.value.clear();

    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups[0].layers[0].tool_groups.emplace_back();
    plan.groups[0].layers[0].tool_groups.back().extruder_id = 0;
    plan.groups[0].layers[0].tool_groups.emplace_back();
    plan.groups[0].layers[0].tool_groups.back().extruder_id = 1;

    plan.groups[0].layers.emplace_back();
    plan.groups[0].layers.back().print_z = scale_i(0.4);
    plan.groups[0].layers.back().tool_groups.emplace_back();
    plan.groups[0].layers.back().tool_groups.back().extruder_id = 1;
    plan.groups[0].layers.back().tool_groups.emplace_back();
    plan.groups[0].layers.back().tool_groups.back().extruder_id = 0;

    plan.groups.emplace_back();
    plan.groups.back().layers.emplace_back();
    plan.groups.back().layers.back().print_z = scale_i(0.6);
    plan.groups.back().layers.back().tool_groups.emplace_back();
    plan.groups.back().layers.back().tool_groups.back().extruder_id = 1;

    run_layer_plugins(print, {TOOLCHANGE_PLUGIN});

    const PrintingLayerGroup &first_layer = plan.groups[0].layers[0];
    const PrintingLayerGroup &second_layer = plan.groups[0].layers[1];
    const PrintingGroup &second_group = plan.groups[1];
    // First selection and repeated visits need no explicit command.
    CHECK_FALSE(first_layer.tool_groups[0].events.has_before());
    CHECK_FALSE(first_layer.tool_groups[1].events.has_before());
    CHECK_FALSE(second_layer.tool_groups[0].events.has_before());

    const ExtrusionPropertySpecialCommand *local =
        semantic_toolchange(first_layer.tool_groups[2]);
    const ExtrusionPropertySpecialCommand *inter_layer =
        semantic_toolchange(second_layer.tool_groups[1]);
    const ExtrusionPropertySpecialCommand *inter_group =
        semantic_toolchange(second_group.layers[0].tool_groups[0]);
    REQUIRE(local != nullptr);
    REQUIRE(inter_layer != nullptr);
    REQUIRE(inter_group != nullptr);
    CHECK(local->code == C_EXTRUSION_SPECIAL_COMMAND_TOOLCHANGE);
    CHECK(local->extra_data == Approx(1.0));
    CHECK(inter_layer->extra_data == Approx(0.0));
    CHECK(inter_group->extra_data == Approx(1.0));
}

TEST_CASE("CreateToolChange stores the final structural script context",
          "[plugins][layer-extrusion-edit][toolchange][script]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Print print;
    Model model;
    prepare_print(print, model);
    PrintConfig &config = const_cast<PrintConfig &>(print.config());
    config.toolchange_gcode.value =
        "; P[previous_extruder] N[next_extruder] L[layer_num]";

    PrintingGroup &group = print.mutable_printing_plan().groups.front();
    group.layers.emplace_back();
    PrintingLayerGroup &target_layer = group.layers.back();
    target_layer.print_z = scale_i(0.6);
    target_layer.tool_groups.emplace_back();
    target_layer.tool_groups.back().extruder_id = 1;

    run_layer_plugins(print, {TOOLCHANGE_PLUGIN});

    CHECK_FALSE(group.layers.front().tool_groups.front().events.has_before());
    const PrintingToolGroup &target_tool = target_layer.tool_groups.front();
    const ExtrusionEntity &event = first_before_event(target_tool);
    const ExtrusionPropertyCustomGcode *script =
        event.get_property<ExtrusionPropertyCustomGcode>();
    REQUIRE(script != nullptr);
    CHECK(script->script_type == GCODE_SCRIPT_TYPE_TOOLCHANGE_GCODE);
    CHECK(script->processing_extruder_id == 1);
    CHECK(event.get_property<ExtrusionPropertySpecialCommand>() == nullptr);
    CHECK(stored_script_int(event, *script, "layer_num") == 1);
    CHECK(stored_script_int(event, *script, "previous_extruder") == 0);
    CHECK(stored_script_int(event, *script, "next_extruder") == 1);
    CHECK(stored_script_float(event, *script, "layer_z") == Approx(0.6));
    CHECK(stored_script_float(event, *script, "max_layer_z") == Approx(0.6));
    CHECK(stored_script_float(event, *script, "toolchange_z") == Approx(0.6));
}

TEST_CASE("Whitespace toolchange G-code uses only the semantic fallback",
          "[plugins][layer-extrusion-edit][toolchange][fallback]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Print print;
    Model model;
    prepare_print(print, model);
    PrintConfig &config = const_cast<PrintConfig &>(print.config());
    config.toolchange_gcode.value = " \t\r\n";

    PrintingLayerGroup &layer = print.mutable_printing_plan().groups.front()
        .layers.front();
    layer.tool_groups.emplace_back();
    layer.tool_groups.back().extruder_id = 1;

    run_layer_plugins(print, {TOOLCHANGE_PLUGIN});

    const PrintingToolGroup &target = layer.tool_groups.back();
    const ExtrusionEntity &event = first_before_event(target);
    const ExtrusionPropertySpecialCommand *command =
        event.get_property<ExtrusionPropertySpecialCommand>();
    REQUIRE(command != nullptr);
    CHECK(command->code == C_EXTRUSION_SPECIAL_COMMAND_TOOLCHANGE);
    CHECK(command->extra_data == Approx(1.0));
    CHECK(event.get_property<ExtrusionPropertyCustomGcode>() == nullptr);
}

} // namespace
