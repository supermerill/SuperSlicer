///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

/*
CreateRetraction tests
======================

The cases verify the compact-scope contract first: a source owns Retract in
after, while the target owns Unretract in before. Physical tool selection is
tested separately through CreateToolChange because it belongs to the target
PrintingToolGroup rather than to a compact scope. Plan-terminal retraction is
checked separately because it belongs to STEP_EXTRUSION_EDIT.
*/

#include "layer_extrusion_edit_transition_test_helpers.hpp"

namespace {

using namespace Slic3r;
using namespace Slic3r::Printing;
using namespace Slic3r::Test::TransitionPipeline;
using slic3r_api::LayerExtrusionEdit::ExtrusionScope::OrderedExtrusionScope;

TEST_CASE("CreateRetraction registers both transition providers",
          "[plugins][layer-extrusion-edit][retraction][registration]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Orchestrator &orchestrator = Orchestrator::instance();
    Plugin *retraction = orchestrator.get_plugin(RETRACTION_PLUGIN);
    Plugin *terminal = orchestrator.get_plugin(TERMINAL_RETRACTION_PLUGIN);
    REQUIRE(retraction != nullptr);
    REQUIRE(terminal != nullptr);
    CHECK(retraction->get_priority() == -90);
    CHECK(retraction->get_exclusive_group() ==
          "layer_extrusion_edit.retraction");
    REQUIRE_FALSE(retraction->get_dependencies().empty());
    CHECK(retraction->get_dependencies().front() == TRANSITION_SCOPE_PLUGIN);
    CHECK(terminal->get_priority() == -100);
    CHECK(terminal->get_exclusive_group() ==
          "extrusion_edit.terminal_retraction");
}

TEST_CASE("CreateRetraction places semantic E events in compact phases",
          "[plugins][layer-extrusion-edit][retraction][gap]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Print print;
    Model model;
    prepare_print(print, model);
    run_layer_plugins(print, {TRANSITION_SCOPE_PLUGIN, RETRACTION_PLUGIN});

    PrintingToolGroup &tool = print.mutable_printing_plan().groups.front()
        .layers.front().tool_groups.front();
    slic3r_api::MutableExtrusionEntity source_root = find_scope(
        entity_view(*tool.extrusions.front().root));
    slic3r_api::MutableExtrusionEntity target_root = find_scope(
        entity_view(*tool.extrusions.back().root));
    REQUIRE(source_root.valid());
    REQUIRE(target_root.valid());
    OrderedExtrusionScope source(source_root, scope_property_key());
    OrderedExtrusionScope target(target_root, scope_property_key());

    std::vector<std::string> source_events;
    std::vector<std::string> target_events;
    collect_transition_events(source.after().readonly(), source_events);
    collect_transition_events(target.before().readonly(), target_events);
    CHECK(source_events == std::vector<std::string>{"retract"});
    CHECK(target_events == std::vector<std::string>{"unretract"});

    const slic3r_api::EPropertyExtrusionAxis *retract_axis =
        source.after().get(slic3r_api::EPropertyExtrusionAxis::key);
    const slic3r_api::EPropertyExtrusionAxis *unretract_axis =
        target.before().get(slic3r_api::EPropertyExtrusionAxis::key);
    REQUIRE(retract_axis != nullptr);
    REQUIRE(unretract_axis != nullptr);
    CHECK(retract_axis->operation == C_EXTRUSION_AXIS_OPERATION_RETRACT_TO);
    CHECK(retract_axis->value == Approx(2.0));
    CHECK(unretract_axis->operation == C_EXTRUSION_AXIS_OPERATION_UNRETRACT);
    CHECK(unretract_axis->restart_extra == Approx(0.1));
}

TEST_CASE("Tool changes remain semantic when their retraction target is zero",
          "[plugins][layer-extrusion-edit][retraction][toolchange]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Print print;
    Model model;
    prepare_print(print, model, "0", "0");
    PrintingLayerGroup &layer = print.mutable_printing_plan().groups.front()
        .layers.front();
    PrintingExtrusion moved = std::move(layer.tool_groups.front().extrusions.back());
    layer.tool_groups.front().extrusions.pop_back();
    layer.tool_groups.emplace_back();
    layer.tool_groups.back().extruder_id = 1;
    layer.tool_groups.back().extrusions.push_back(std::move(moved));

    run_layer_plugins(print, {TRANSITION_SCOPE_PLUGIN, RETRACTION_PLUGIN});

    slic3r_api::MutableExtrusionEntity source_root = find_scope(entity_view(
        *layer.tool_groups.front().extrusions.front().root));
    slic3r_api::MutableExtrusionEntity target_root = find_scope(entity_view(
        *layer.tool_groups.back().extrusions.front().root));
    OrderedExtrusionScope source(source_root, scope_property_key());
    OrderedExtrusionScope target(target_root, scope_property_key());
    CHECK(source.has_outgoing_toolchange());
    CHECK(target.has_incoming_toolchange());

    std::vector<std::string> source_events;
    std::vector<std::string> target_events;
    collect_transition_events(source.after().readonly(), source_events);
    collect_transition_events(target.before().readonly(), target_events);
    CHECK(source_events.empty());
    CHECK(target_events.empty());
}

TEST_CASE("Tool-change restart extra follows the selected extruder",
          "[plugins][layer-extrusion-edit][retraction][toolchange]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Print print;
    Model model;
    prepare_print(print, model, "2", "3");
    PrintConfig &config = const_cast<PrintConfig &>(print.config());
    config.retract_restart_extra_toolchange.set(
        std::vector<double>{0.2, -0.3});

    PrintingLayerGroup &layer = print.mutable_printing_plan().groups.front()
        .layers.front();
    PrintingExtrusion moved = std::move(
        layer.tool_groups.front().extrusions.back());
    layer.tool_groups.front().extrusions.pop_back();
    layer.tool_groups.emplace_back();
    layer.tool_groups.back().extruder_id = 1;
    layer.tool_groups.back().extrusions.push_back(std::move(moved));

    run_layer_plugins(print, {TRANSITION_SCOPE_PLUGIN, RETRACTION_PLUGIN});

    OrderedExtrusionScope source(
        find_scope(entity_view(
            *layer.tool_groups.front().extrusions.front().root)),
        scope_property_key());
    OrderedExtrusionScope target(
        find_scope(entity_view(
            *layer.tool_groups.back().extrusions.front().root)),
        scope_property_key());
    const slic3r_api::EPropertyExtrusionAxis *retract_axis =
        source.after().get(slic3r_api::EPropertyExtrusionAxis::key);
    REQUIRE(retract_axis != nullptr);
    CHECK(retract_axis->value == Approx(3.0));
    CHECK(retract_axis->toolchange == 1);

    const slic3r_api::EPropertyExtrusionAxis *unretract_axis =
        target.before().get(slic3r_api::EPropertyExtrusionAxis::key);
    REQUIRE(unretract_axis != nullptr);
    CHECK(unretract_axis->restart_extra == Approx(-0.3));
    CHECK(unretract_axis->toolchange == 1);
}

TEST_CASE("Empty tool visits remain visible between printable scopes",
          "[plugins][layer-extrusion-edit][retraction][toolchange]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Print print;
    Model model;
    prepare_print(print, model);

    PrintingLayerGroup &layer = print.mutable_printing_plan().groups.front()
        .layers.front();
    PrintingExtrusion moved = std::move(
        layer.tool_groups.front().extrusions.back());
    layer.tool_groups.front().extrusions.pop_back();
    layer.tool_groups.emplace_back();
    layer.tool_groups.back().extruder_id = 1;
    layer.tool_groups.emplace_back();
    layer.tool_groups.back().extruder_id = 0;
    layer.tool_groups.back().extrusions.push_back(std::move(moved));

    run_layer_plugins(print, {TRANSITION_SCOPE_PLUGIN, RETRACTION_PLUGIN});

    OrderedExtrusionScope source(
        find_scope(entity_view(
            *layer.tool_groups.front().extrusions.front().root)),
        scope_property_key());
    OrderedExtrusionScope target(
        find_scope(entity_view(
            *layer.tool_groups.back().extrusions.front().root)),
        scope_property_key());
    CHECK(source.has_outgoing_toolchange());
    CHECK(target.has_incoming_toolchange());

    std::vector<std::string> target_events;
    collect_transition_events(target.before().readonly(), target_events);
    CHECK(target_events == std::vector<std::string>{"unretract"});
}

TEST_CASE("CreateRetraction shares one decision across non-empty layers",
          "[plugins][layer-extrusion-edit][retraction][parallel]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Print print;
    Model model;
    prepare_print(print, model);
    PrintingGroup &group = print.mutable_printing_plan().groups.front();
    PrintingExtrusion moved = std::move(
        group.layers.front().tool_groups.front().extrusions.back());
    group.layers.front().tool_groups.front().extrusions.pop_back();
    group.layers.emplace_back();
    group.layers.back().print_z = scale_i(0.4);
    group.layers.back().tool_groups.emplace_back();
    group.layers.back().tool_groups.back().extruder_id = 0;
    group.layers.back().tool_groups.back().extrusions.push_back(std::move(moved));

    run_layer_plugins(print, {TRANSITION_SCOPE_PLUGIN, RETRACTION_PLUGIN});

    slic3r_api::MutableExtrusionEntity source_root = find_scope(entity_view(
        *group.layers.front().tool_groups.front().extrusions.front().root));
    slic3r_api::MutableExtrusionEntity target_root = find_scope(entity_view(
        *group.layers.back().tool_groups.front().extrusions.front().root));
    OrderedExtrusionScope source(source_root, scope_property_key());
    OrderedExtrusionScope target(target_root, scope_property_key());
    std::vector<std::string> source_events;
    std::vector<std::string> target_events;
    collect_transition_events(source.after().readonly(), source_events);
    collect_transition_events(target.before().readonly(), target_events);
    CHECK(source_events == std::vector<std::string>{"retract"});
    CHECK(target_events == std::vector<std::string>{"unretract"});
}

TEST_CASE("Disable retraction overrides an enforced boundary",
          "[plugins][layer-extrusion-edit][retraction][modifier]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Print print;
    Model model;
    prepare_print(print, model);
    PrintingToolGroup &tool = print.mutable_printing_plan().groups.front()
        .layers.front().tool_groups.front();
    tool.extrusions.front().root
        ->get_or_add_property<ExtrusionPropertyModifier>()
        .set_enforce_retraction();
    tool.extrusions.back().root
        ->get_or_add_property<ExtrusionPropertyModifier>()
        .set_disable_retraction();

    run_layer_plugins(print, {TRANSITION_SCOPE_PLUGIN, RETRACTION_PLUGIN});

    OrderedExtrusionScope source(
        find_scope(entity_view(*tool.extrusions.front().root)),
        scope_property_key());
    OrderedExtrusionScope target(
        find_scope(entity_view(*tool.extrusions.back().root)),
        scope_property_key());
    std::vector<std::string> source_events;
    std::vector<std::string> target_events;
    collect_transition_events(source.after().readonly(), source_events);
    collect_transition_events(target.before().readonly(), target_events);
    CHECK(source_events.empty());
    CHECK(target_events.empty());
}

TEST_CASE("Direct gap threshold controls ordinary retraction",
          "[plugins][layer-extrusion-edit][retraction][threshold]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Print print;
    Model model;
    prepare_print(print, model);
    PrintConfig &config = const_cast<PrintConfig &>(print.config());
    config.retract_before_travel.set(std::vector<double>{20.0});

    run_layer_plugins(print, {TRANSITION_SCOPE_PLUGIN, RETRACTION_PLUGIN});

    PrintingToolGroup &tool = print.mutable_printing_plan().groups.front()
        .layers.front().tool_groups.front();
    OrderedExtrusionScope source(
        find_scope(entity_view(*tool.extrusions.front().root)),
        scope_property_key());
    OrderedExtrusionScope target(
        find_scope(entity_view(*tool.extrusions.back().root)),
        scope_property_key());
    std::vector<std::string> source_events;
    std::vector<std::string> target_events;
    collect_transition_events(source.after().readonly(), source_events);
    collect_transition_events(target.before().readonly(), target_events);
    CHECK(source_events.empty());
    CHECK(target_events.empty());
}

TEST_CASE("Layer-change retraction reserves phases for contiguous geometry",
          "[plugins][layer-extrusion-edit][retraction][layer-change]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Print print;
    Model model;
    prepare_print(print, model);
    PrintConfig &config = const_cast<PrintConfig &>(print.config());
    config.retract_layer_change.set(std::vector<unsigned char>{1});

    PrintingGroup &group = print.mutable_printing_plan().groups.front();
    PrintingExtrusion moved = std::move(
        group.layers.front().tool_groups.front().extrusions.back());
    group.layers.front().tool_groups.front().extrusions.pop_back();
    moved.root = make_print_path(10.0, 20.0);
    group.layers.emplace_back();
    group.layers.back().print_z = scale_i(0.2);
    group.layers.back().tool_groups.emplace_back();
    group.layers.back().tool_groups.back().extruder_id = 0;
    group.layers.back().tool_groups.back().extrusions.push_back(std::move(moved));

    run_layer_plugins(print, {TRANSITION_SCOPE_PLUGIN, RETRACTION_PLUGIN});

    OrderedExtrusionScope source(
        find_scope(entity_view(
            *group.layers.front().tool_groups.front().extrusions.front().root)),
        scope_property_key());
    OrderedExtrusionScope target(
        find_scope(entity_view(
            *group.layers.back().tool_groups.front().extrusions.front().root)),
        scope_property_key());
    REQUIRE(source.has_outgoing_transition());
    REQUIRE(target.has_incoming_transition());
    std::vector<std::string> source_events;
    std::vector<std::string> target_events;
    collect_transition_events(source.after().readonly(), source_events);
    collect_transition_events(target.before().readonly(), target_events);
    CHECK(source_events == std::vector<std::string>{"retract"});
    CHECK(target_events == std::vector<std::string>{"unretract"});
}

TEST_CASE("Terminal retraction belongs to PrintingPlan after events",
          "[plugins][layer-extrusion-edit][retraction][terminal]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Print print;
    Model model;
    prepare_print(print, model);
    run_layer_plugins(print, {TRANSITION_SCOPE_PLUGIN, RETRACTION_PLUGIN});
    run_plan_plugins(print, {TERMINAL_RETRACTION_PLUGIN});

    const slic3r_api::PrintingPlan plan(
        reinterpret_cast<printing_plan_handle *>(
            &print.mutable_printing_plan()));
    REQUIRE(plan.events().has_after());
    std::vector<std::string> events;
    collect_transition_events(plan.events().after(), events);
    CHECK(events == std::vector<std::string>{"retract"});
}

TEST_CASE("Configured tool and filament scripts keep separate ordered owners",
          "[plugins][layer-extrusion-edit][retraction][settings-scripts]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Print print;
    Model model;
    prepare_print(print, model);
    PrintConfig &config = const_cast<PrintConfig &>(print.config());
    config.nozzle_diameter.set(std::vector<double>{0.4, 0.4});
    config.toolchange_gcode.value = "; toolchange";
    config.start_filament_gcode.set(
        std::vector<std::string>{"", "; start filament"});

    PrintingLayerGroup &layer = print.mutable_printing_plan().groups.front()
        .layers.front();
    PrintingExtrusion moved = std::move(layer.tool_groups.front().extrusions.back());
    layer.tool_groups.front().extrusions.pop_back();
    layer.tool_groups.emplace_back();
    layer.tool_groups.back().extruder_id = 1;
    layer.tool_groups.back().extrusions.push_back(std::move(moved));

    run_layer_plugins(print, {
        TOOLCHANGE_PLUGIN, TRANSITION_SCOPE_PLUGIN, RETRACTION_PLUGIN});
    run_plan_plugins(print, {SETTINGS_SCRIPTS_PLUGIN});

    slic3r_api::MutableExtrusionEntity target_root = find_scope(entity_view(
        *layer.tool_groups.back().extrusions.front().root));
    OrderedExtrusionScope target(target_root, scope_property_key());
    std::vector<std::string> tool_events;
    const slic3r_api::PrintingToolGroup target_tool(
        reinterpret_cast<printing_tool_group_handle *>(
            &layer.tool_groups.back()));
    collect_transition_events(target_tool.events().before(), tool_events);
    CHECK(tool_events == std::vector<std::string>{"toolchange_script"});

    std::vector<std::string> events;
    collect_transition_events(target.before().readonly(), events);
    CHECK(events == std::vector<std::string>{
        "start_filament_script", "unretract"});
}

} // namespace
