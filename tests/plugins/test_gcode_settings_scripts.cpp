#include <catch2/catch.hpp>

#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include <boost/filesystem.hpp>

#include "plugin_test_helpers.hpp"
#include "gcode_test_helpers.hpp"
#include "layer_extrusion_edit_transition_test_helpers.hpp"

#include "libslic3r/Api/host/ApiHostUtils.hpp"
#include "libslic3r/Api/host/GCodeScriptProcessor.hpp"
#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/host/Plugin.hpp"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_gcode.h"
#include "libslic3r/Api/plugin/cpp/ExtrusionViews.hpp"
#include "libslic3r/Api/plugin/cpp/gcode/DefaultGCodeFirmwareSession.hpp"
#include "libslic3r/Api/plugin/cpp/gcode/GCodeFirmwareViews.hpp"
#include "libslic3r/Api/plugin/cpp/gcode/GCodeScriptProcessorViews.hpp"
#include "libslic3r/Api/plugin/cpp/gcode/MachineEnvelope.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Config/ConfigSnapshotSerialization.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityVisitors.hpp"
#include "libslic3r/GCode.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Geometry/ArcWelder.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/LayerRegion.hpp"
#include "libslic3r/Plugins/GCode/Firmware/BuiltinGCodeFirmwares.hpp"
#include "libslic3r/Plugins/GCode/Firmware/KlipperGCodeFirmware.hpp"
#include "libslic3r/Plugins/GCode/Firmware/Marlin1GCodeFirmware.hpp"
#include "libslic3r/Plugins/GCode/Firmware/Marlin2GCodeFirmware.hpp"
#include "libslic3r/Plugins/GCode/Firmware/PrusaGCodeFirmware.hpp"
#include "libslic3r/Plugins/GCode/Firmware/RepRapGCodeFirmware.hpp"
#include "libslic3r/Plugins/GCode/Firmware/SprinterGCodeFirmware.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Config/PrintConfig.hpp"
#include "libslic3r/PrintObject.hpp"
#include "libslic3r/Printing/PrintingPlan.hpp"
#include "libslic3r/Steps/StepExtrusionEdition.hpp"
#include "libslic3r/Steps/StepGenerateGcode.hpp"
#include "libslic3r/Steps/StepPipeline.hpp"
#include "test_data.hpp"

/*
Settings script tests verify how configured user scripts are inserted into the final PrintingPlan scopes.
Shared plan construction and output helpers live in gcode_test_helpers so this file keeps only component-specific behavior.
*/

namespace {
using namespace Slic3r;
using namespace Slic3r::Printing;
using namespace Slic3r::Test::GCode;

} // namespace

TEST_CASE("Configured G-code scripts become typed global plan events",
          "[plugins][gcode][script-type][extrusion-edit]") {
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    PrintConfig &config = const_cast<PrintConfig &>(print.config());
    config.start_gcode.value = "M117 configured start";
    config.end_gcode.value = "M117 configured end";
    PrintingPlan &plan = print.mutable_printing_plan();

    Steps::StepExtrusionEdition::clean_and_prepare(print);
    Steps::StepExtrusionEdition::run_step(Orchestrator::instance(), print);

    REQUIRE(plan.events.before().child_count() == 1);
    REQUIRE(plan.events.after().child_count() == 1);
    const ExtrusionEntity &start = plan.events.before().child(0);
    const ExtrusionEntity &end = plan.events.after().child(0);
    const ExtrusionPropertyCustomGcode *start_property = start.get_property<ExtrusionPropertyCustomGcode>();
    const ExtrusionPropertyCustomGcode *end_property = end.get_property<ExtrusionPropertyCustomGcode>();
    REQUIRE(start_property != nullptr);
    REQUIRE(end_property != nullptr);
    CHECK(start_property->kind == C_EXTRUSION_CUSTOM_GCODE_SCRIPT);
    CHECK(start_property->script_type == GCODE_SCRIPT_TYPE_START_GCODE);
    CHECK(start.custom_gcode_string(*start_property) == "M117 configured start");
    CHECK(end_property->kind == C_EXTRUSION_CUSTOM_GCODE_SCRIPT);
    CHECK(end_property->script_type == GCODE_SCRIPT_TYPE_END_GCODE);
    CHECK(end.custom_gcode_string(*end_property) == "M117 configured end");

    config.start_gcode.value.clear();
    config.end_gcode.value.clear();
    Steps::StepExtrusionEdition::clean_and_prepare(print);
    Steps::StepExtrusionEdition::run_step(Orchestrator::instance(), print);
    CHECK_FALSE(plan.events.has_before());
    CHECK_FALSE(plan.events.has_after());
}
TEST_CASE("Configured layer and filament scripts become scoped plan events",
          "[plugins][gcode][script-type][extrusion-edit]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    configure_standard_firmware(print, 2);
    PrintConfig &config = const_cast<PrintConfig &>(print.config());
    config.before_layer_gcode.value = "; before layer";
    config.layer_gcode.value = "; after layer move";
    config.toolchange_gcode.value = "; custom toolchange";
    config.start_filament_gcode.set(std::vector<std::string>{"; start filament 0", "; start filament 1"});
    config.end_filament_gcode.set(std::vector<std::string>{"; end filament 0", "; end filament 1"});
    config.end_gcode.value = "; end print";

    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    PrintingGroup &group = plan.groups.front();
    group.layers.emplace_back();
    group.layers.back().print_z = scale_i(0.2);
    group.layers.back().tool_groups.emplace_back();
    group.layers.back().tool_groups.back().extruder_id = 1;
    group.layers.emplace_back();
    group.layers.back().print_z = scale_i(0.4);
    group.layers.back().tool_groups.emplace_back();
    group.layers.back().tool_groups.back().extruder_id = 1;
    group.layers.back().tool_groups.emplace_back();
    group.layers.back().tool_groups.back().extruder_id = 0;

    Steps::StepExtrusionEdition::clean_and_prepare(print);
    Steps::StepExtrusionEdition::run_step(Orchestrator::instance(), print);

    REQUIRE(group.events.before().child_count() == 1);
    const ExtrusionPropertyCustomGcode *first_before_layer =
        group.events.before().child(0).get_property<ExtrusionPropertyCustomGcode>();
    REQUIRE(first_before_layer != nullptr);
    CHECK(first_before_layer->script_type == GCODE_SCRIPT_TYPE_BEFORE_LAYER_GCODE);
    CHECK_FALSE(group.layers[0].events.has_before());

    REQUIRE(group.layers[0].events.after().child_count() == 1);
    const ExtrusionPropertyCustomGcode *second_before_layer =
        group.layers[0].events.after().child(0).get_property<ExtrusionPropertyCustomGcode>();
    REQUIRE(second_before_layer != nullptr);
    CHECK(second_before_layer->script_type == GCODE_SCRIPT_TYPE_BEFORE_LAYER_GCODE);
    REQUIRE(group.layers[1].events.before().child_count() == 1);
    const ExtrusionPropertyCustomGcode *second_layer_script =
        group.layers[1].events.before().child(0).get_property<ExtrusionPropertyCustomGcode>();
    REQUIRE(second_layer_script != nullptr);
    CHECK(second_layer_script->script_type == GCODE_SCRIPT_TYPE_LAYER_GCODE);
    const ExtrusionEntity &second_layer_entity = group.layers[1].events.before().child(0);
    const slic3r_api::ExtrusionEntity second_layer_event(
        reinterpret_cast<const extrusion_entity_handle *>(&second_layer_entity));
    const slic3r_api::EPropertyCustomGcode *second_layer_payload =
        second_layer_event.get(slic3r_api::EPropertyCustomGcode::key);
    REQUIRE(second_layer_payload != nullptr);
    CHECK(second_layer_payload->processing_extruder_id == GCODE_SCRIPT_PROCESSING_EXTRUDER_INVALID);
    CHECK(stored_script_int(second_layer_entity, *second_layer_script, "layer_num") == 1);
    CHECK(stored_script_float(second_layer_entity, *second_layer_script, "layer_z") == Approx(0.4));
    CHECK(stored_script_float(second_layer_entity, *second_layer_script, "previous_layer_z") == Approx(0.2));
    CHECK(stored_script_float(second_layer_entity, *second_layer_script, "max_layer_z") == Approx(0.4));

    PrintingToolGroup &initial_tool = group.layers[0].tool_groups[0];
    REQUIRE(initial_tool.events.before().child_count() == 1);
    const ExtrusionPropertyCustomGcode *initial_start =
        initial_tool.events.before().child(0).get_property<ExtrusionPropertyCustomGcode>();
    REQUIRE(initial_start != nullptr);
    CHECK(initial_start->script_type == GCODE_SCRIPT_TYPE_START_FILAMENT_GCODE);
    CHECK(initial_start->processing_extruder_id == 1);

    PrintingToolGroup &unchanged_tool = group.layers[1].tool_groups[0];
    CHECK_FALSE(unchanged_tool.events.has_before());
    REQUIRE(unchanged_tool.events.after().child_count() == 1);

    PrintingToolGroup &changed_tool = group.layers[1].tool_groups[1];
    REQUIRE(changed_tool.events.before().child_count() == 1);
    const ExtrusionPropertyCustomGcode *end_filament =
        unchanged_tool.events.after().child(0).get_property<ExtrusionPropertyCustomGcode>();
    const ExtrusionPropertyCustomGcode *start_filament =
        changed_tool.events.before().child(0).get_property<ExtrusionPropertyCustomGcode>();
    REQUIRE(end_filament != nullptr);
    REQUIRE(start_filament != nullptr);
    CHECK(end_filament->script_type == GCODE_SCRIPT_TYPE_END_FILAMENT_GCODE);
    CHECK(end_filament->processing_extruder_id == 1);
    CHECK(start_filament->script_type == GCODE_SCRIPT_TYPE_START_FILAMENT_GCODE);
    CHECK(start_filament->processing_extruder_id == 0);

    REQUIRE(plan.events.after().child_count() == 3);
    const ExtrusionPropertyCustomGcode *final_tool_zero =
        plan.events.after().child(0).get_property<ExtrusionPropertyCustomGcode>();
    const ExtrusionPropertyCustomGcode *final_tool_one =
        plan.events.after().child(1).get_property<ExtrusionPropertyCustomGcode>();
    const ExtrusionPropertyCustomGcode *end_print =
        plan.events.after().child(2).get_property<ExtrusionPropertyCustomGcode>();
    REQUIRE(final_tool_zero != nullptr);
    REQUIRE(final_tool_one != nullptr);
    REQUIRE(end_print != nullptr);
    CHECK(final_tool_zero->script_type == GCODE_SCRIPT_TYPE_END_FILAMENT_GCODE);
    CHECK(final_tool_zero->processing_extruder_id == 0);
    CHECK(final_tool_one->script_type == GCODE_SCRIPT_TYPE_END_FILAMENT_GCODE);
    CHECK(final_tool_one->processing_extruder_id == 1);
    CHECK(end_print->script_type == GCODE_SCRIPT_TYPE_END_GCODE);
    CHECK(end_print->processing_extruder_id == 0);
    CHECK(stored_script_int(
              plan.events.after().child(2), *end_print, "layer_num") == 1);
    CHECK(stored_script_float(
              plan.events.after().child(2), *end_print, "previous_layer_z") == Approx(0.2));

    config.before_layer_gcode.value.clear();
    config.layer_gcode.value.clear();
    config.toolchange_gcode.value.clear();
    config.start_filament_gcode.set(std::vector<std::string>{"", ""});
    config.end_filament_gcode.set(std::vector<std::string>{"", ""});
    config.end_gcode.value.clear();
    Steps::StepExtrusionEdition::clean_and_prepare(print);
    Steps::StepExtrusionEdition::run_step(Orchestrator::instance(), print);
    CHECK_FALSE(group.events.has_before());
    CHECK_FALSE(group.layers[0].events.has_after());
    CHECK_FALSE(group.layers[1].events.has_before());
    CHECK_FALSE(unchanged_tool.events.has_after());
    CHECK_FALSE(changed_tool.events.has_before());
    CHECK_FALSE(plan.events.has_after());
}
TEST_CASE("Between-object scripts select the requested side of the group move",
          "[plugins][gcode][script-type][extrusion-edit][sequential]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    DynamicPrintConfig source_config = DynamicPrintConfig::full_print_config();
    Print print;
    Model model;
    Test::init_print({make_cube(5., 5., 0.2), make_cube(4., 4., 0.2)}, print, model, source_config);
    REQUIRE(print.objects().size() == 2);
    print.objects()[0].model_object()->name = "First object";
    print.objects()[1].model_object()->name = "Second/object";

    PrintConfig &config = const_cast<PrintConfig &>(print.config());
    config.start_gcode.value.clear();
    config.end_gcode.value.clear();
    config.before_layer_gcode.value.clear();
    config.layer_gcode.value.clear();
    config.toolchange_gcode.value.clear();
    config.start_filament_gcode.set(std::vector<std::string>{""});
    config.end_filament_gcode.set(std::vector<std::string>{""});
    config.between_objects_gcode.value =
        "; between [previous_object_id]:[previous_object_name] [next_object_id]:[next_object_name]";

    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    plan.groups.back().object_instances.push_back(PrintingObjectInstance{&print.objects()[0], 0});
    // Auxiliary groups do not participate in object-to-object script
    // selection, even when they sit between two real object groups.
    plan.groups.emplace_back();
    plan.groups.emplace_back();
    plan.groups.back().object_instances.push_back(PrintingObjectInstance{&print.objects()[1], 0});

    config.between_objects_gcode_before_move.value = false;
    Steps::StepExtrusionEdition::clean_and_prepare(print);
    Steps::StepExtrusionEdition::run_step(Orchestrator::instance(), print);
    CHECK_FALSE(plan.groups[0].events.has_after());
    CHECK_FALSE(plan.groups[1].events.has_before());
    CHECK_FALSE(plan.groups[1].events.has_after());
    REQUIRE(plan.groups[2].events.before().child_count() == 1);

    const ExtrusionEntity &event = plan.groups[2].events.before().child(0);
    const slic3r_api::ExtrusionEntity event_view(
        reinterpret_cast<const extrusion_entity_handle *>(&event));
    const slic3r_api::EPropertyCustomGcode *property =
        event_view.get(slic3r_api::EPropertyCustomGcode::key);
    REQUIRE(property != nullptr);
    REQUIRE(property->script_type == GCODE_SCRIPT_TYPE_BETWEEN_OBJECTS_GCODE);
    GCodeScriptProcessor host_processor(print, Orchestrator::instance());
    const slic3r_api::GCodeGeneration::GCodeScriptProcessorView scripts(host_processor.c_processor());
    PluginStorage script_storage;
    storage_handle *script_storage_handle =
        reinterpret_cast<storage_handle *>(&script_storage);
    ScriptStateFirmwareProbe firmware(scripts, script_storage_handle);
    const slic3r_api::Print print_view(reinterpret_cast<const print_handle *>(&print));
    const slic3r_api::PrintingPlan plan_view = printing_plan_view(print);
    CHECK(firmware.begin_print(print_view).empty());
    CHECK(firmware.begin_group(plan_view.group(0)).empty());
    CHECK(firmware.begin_group(plan_view.group(1)).empty());
    CHECK(firmware.begin_group(plan_view.group(2)).empty());
    CHECK(firmware.write_event(plan_view.group(2).events().before()) ==
          "; between 0:First_object 1:Second_object\n");

    config.between_objects_gcode_before_move.value = true;
    Steps::StepExtrusionEdition::clean_and_prepare(print);
    Steps::StepExtrusionEdition::run_step(Orchestrator::instance(), print);
    REQUIRE(plan.groups[0].events.after().child_count() == 1);
    CHECK_FALSE(plan.groups[1].events.has_before());
    CHECK_FALSE(plan.groups[1].events.has_after());
    CHECK_FALSE(plan.groups[2].events.has_before());

    ScriptStateFirmwareProbe before_move_firmware(scripts, script_storage_handle);
    const slic3r_api::PrintingPlan refreshed_plan_view = printing_plan_view(print);
    CHECK(before_move_firmware.begin_print(print_view).empty());
    CHECK(before_move_firmware.begin_group(refreshed_plan_view.group(0)).empty());
    CHECK(before_move_firmware.write_event(refreshed_plan_view.group(0).events().after()) ==
          "; between 0:First_object 1:Second_object\n");
}
TEST_CASE("Single-extruder multimaterial finalizes only the active logical tool",
          "[plugins][gcode][script-type][extrusion-edit]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    configure_standard_firmware(print, 2);
    PrintConfig &config = const_cast<PrintConfig &>(print.config());
    config.start_gcode.value.clear();
    config.end_gcode.value.clear();
    config.toolchange_gcode.value.clear();
    config.start_filament_gcode.set(std::vector<std::string>{"", ""});
    config.end_filament_gcode.set(std::vector<std::string>{"; finalize 0", "; finalize 1"});
    config.single_extruder_multi_material.value = true;

    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    plan.groups.front().layers.emplace_back();
    plan.groups.front().layers.front().print_z = scale_i(0.2);
    plan.groups.front().layers.front().tool_groups.emplace_back();
    plan.groups.front().layers.front().tool_groups.back().extruder_id = 0;
    plan.groups.front().layers.front().tool_groups.emplace_back();
    plan.groups.front().layers.front().tool_groups.back().extruder_id = 1;

    Steps::StepExtrusionEdition::clean_and_prepare(print);
    Steps::StepExtrusionEdition::run_step(Orchestrator::instance(), print);

    REQUIRE(plan.events.after().child_count() == 1);
    const ExtrusionEntity &event = plan.events.after().child(0);
    const ExtrusionPropertyCustomGcode *host_property =
        event.get_property<ExtrusionPropertyCustomGcode>();
    REQUIRE(host_property != nullptr);
    CHECK(event.custom_gcode_string(*host_property) == "; finalize 1");
    const slic3r_api::ExtrusionEntity event_view(
        reinterpret_cast<const extrusion_entity_handle *>(&event));
    const slic3r_api::EPropertyCustomGcode *property =
        event_view.get(slic3r_api::EPropertyCustomGcode::key);
    REQUIRE(property != nullptr);
    CHECK(property->processing_extruder_id == 1);
}
TEST_CASE("Configured scripts use ordered plan tools and final layer state",
          "[plugins][gcode][script-type][firmware][integration]") {
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    select_printing_plan_writer(print);
    configure_standard_firmware(print, 2);
    PrintConfig &config = const_cast<PrintConfig &>(print.config());
    config.filament_diameter.set(std::vector<double>{1.75, 2.85});
    DynamicPrintConfig &full_config = const_cast<DynamicPrintConfig &>(print.full_print_config());
    full_config.set_key_value("filament_diameter", new ConfigOptionFloats(std::vector<double>{1.75, 2.85}));
    config.print_first_layer_bed_temperature.value = 71;
    config.print_first_layer_bed_temperature.set_enabled(true);
    config.start_gcode.value = "; SCRIPT_START T[current_extruder] D{filament_diameter[current_extruder]} "
                               "B[start_gcode_bed_temperature]";
    config.before_layer_gcode.value = "; BEFORE_LAYER L[layer_num] Z[layer_z] P[previous_layer_z]";
    config.layer_gcode.value = "; LAYER L[layer_num] Z[layer_z]";
    config.toolchange_gcode.value = "; CUSTOM_TOOL P[previous_extruder] N[next_extruder]";
    config.start_filament_gcode.set(std::vector<std::string>{
        "; START_FILAMENT F[filament_extruder_id] P[previous_extruder] N[next_extruder]",
        "; START_FILAMENT F[filament_extruder_id] P[previous_extruder] N[next_extruder]"
    });
    config.end_filament_gcode.set(std::vector<std::string>{
        "; END_FILAMENT F[filament_extruder_id] P[previous_extruder] N[next_extruder]",
        "; END_FILAMENT F[filament_extruder_id] P[previous_extruder] N[next_extruder]"
    });
    config.end_gcode.value = "; SCRIPT_END L[layer_num] Z[layer_z] M[max_layer_z] F[filament_extruder_id] "
                             "P[previous_extruder] N[next_extruder]";

    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    PrintingGroup &group = plan.groups.front();
    group.layers.emplace_back();
    group.layers.back().print_z = scale_i(0.2);
    group.layers.back().tool_groups.emplace_back();
    group.layers.back().tool_groups.back().extruder_id = 1;
    group.layers.emplace_back();
    group.layers.back().print_z = scale_i(0.6);
    group.layers.back().tool_groups.emplace_back();
    group.layers.back().tool_groups.back().extruder_id = 0;

    Steps::StepExtrusionEdition::clean_and_prepare(print);
    Slic3r::Test::TransitionPipeline::run_layer_plugins(
        print, {Slic3r::Test::TransitionPipeline::TOOLCHANGE_PLUGIN});
    Steps::StepExtrusionEdition::run_step(Orchestrator::instance(), print);

    // Structural values belong to the event and remain unchanged if a later
    // plugin edits the plan without rebuilding the configured scripts.
    group.layers[0].print_z = scale_i(0.25);
    group.layers[1].print_z = scale_i(0.75);

    const boost::filesystem::path output_path = temporary_gcode_path();
    remove_output_pair(output_path);
    Orchestrator &orchestrator = Orchestrator::instance();
    orchestrator.reset_plugin_cancel();
    try {
        Steps::StepGenerateGcode::run_step(orchestrator, print, output_path.string());
        const std::string output = read_text_file(output_path);
        INFO(output);

        const std::string start = "; SCRIPT_START T1 D2.85 B71\n";
        const std::string first_before_layer = "; BEFORE_LAYER L0 Z0.2 P0\n";
        const std::string first_start_filament = "; START_FILAMENT F1 P-1 N1\n";
        const std::string second_before_layer = "; BEFORE_LAYER L1 Z0.6 P0.2\n";
        const std::string second_layer = "; LAYER L1 Z0.6\n";
        const std::string transition_end_filament = "; END_FILAMENT F1 P1 N0\n";
        const std::string custom_toolchange = "; CUSTOM_TOOL P1 N0\n";
        const std::string second_start_filament = "; START_FILAMENT F0 P1 N0\n";
        const std::string final_end_filament_zero = "; END_FILAMENT F0 P0 N-1\n";
        const std::string final_end_filament_one = "; END_FILAMENT F1 P0 N-1\n";
        const std::string end = "; SCRIPT_END L1 Z0.6 M0.6 F0 P0 N-1\n";
        const size_t start_position = output.find(start);
        const size_t first_before_layer_position = output.find(first_before_layer);
        const size_t first_tool_position = output.find("T1\n");
        const size_t first_start_filament_position = output.find(first_start_filament);
        const size_t second_before_layer_position = output.find(second_before_layer);
        const size_t second_layer_position = output.find(second_layer);
        const size_t transition_end_position = output.find(transition_end_filament);
        const size_t custom_toolchange_position = output.find(custom_toolchange);
        const size_t second_start_filament_position = output.find(second_start_filament);
        const size_t final_zero_position = output.find(final_end_filament_zero);
        const size_t final_one_position = output.find(final_end_filament_one);
        const size_t end_position = output.find(end);
        REQUIRE(start_position != std::string::npos);
        REQUIRE(first_before_layer_position != std::string::npos);
        REQUIRE(first_tool_position != std::string::npos);
        REQUIRE(first_start_filament_position != std::string::npos);
        REQUIRE(second_before_layer_position != std::string::npos);
        REQUIRE(second_layer_position != std::string::npos);
        REQUIRE(transition_end_position != std::string::npos);
        REQUIRE(custom_toolchange_position != std::string::npos);
        REQUIRE(second_start_filament_position != std::string::npos);
        REQUIRE(final_zero_position != std::string::npos);
        REQUIRE(final_one_position != std::string::npos);
        REQUIRE(end_position != std::string::npos);
        CHECK(start_position < first_before_layer_position);
        CHECK(first_before_layer_position < first_tool_position);
        CHECK(first_tool_position < first_start_filament_position);
        CHECK(first_start_filament_position < transition_end_position);
        CHECK(transition_end_position < second_before_layer_position);
        CHECK(second_before_layer_position < second_layer_position);
        CHECK(second_layer_position < custom_toolchange_position);
        CHECK(custom_toolchange_position < second_start_filament_position);
        CHECK(second_start_filament_position < final_zero_position);
        CHECK(final_zero_position < final_one_position);
        CHECK(final_one_position < end_position);
        CHECK(count_command_lines(output, "T1") == 1);
        CHECK(count_command_lines(output, "T0") == 0);
        CHECK(output.back() == '\n');
    } catch (...) {
        orchestrator.reset_plugin_cancel();
        remove_output_pair(output_path);
        throw;
    }
    orchestrator.reset_plugin_cancel();
    remove_output_pair(output_path);
}
TEST_CASE("Layer scripts report consumed filament",
          "[plugins][gcode][script-type][firmware][integration]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    select_printing_plan_writer(print);
    configure_standard_firmware(print);
    PrintConfig &config = const_cast<PrintConfig &>(print.config());
    config.start_gcode.value.clear();
    config.end_gcode.value.clear();
    config.start_filament_gcode.set(std::vector<std::string>{""});
    config.end_filament_gcode.set(std::vector<std::string>{""});
    config.before_layer_gcode.value = "; USED L[layer_num] F{layer_used_filament[0]}";

    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    PrintingGroup &group = plan.groups.front();
    group.layers.emplace_back();
    group.layers.back().print_z = scale_i(0.2);
    group.layers.back().tool_groups.emplace_back();
    PrintingToolGroup &first_tool = group.layers.back().tool_groups.back();
    first_tool.extruder_id = 0;
    append_path_extrusion(
        first_tool,
        make_firmware_path(
            ArcPolyline(Points{Point(scale_i(0.0), scale_i(0.0)), Point(scale_i(1.0), scale_i(0.0))}),
            15.f, 400.f));
    append_path_extrusion(
        first_tool,
        make_firmware_travel(
            ArcPolyline(Points{Point(scale_i(1.0), scale_i(0.0)), Point(scale_i(5.0), scale_i(0.0))}),
            80.f, 800.f));
    append_path_extrusion(
        first_tool,
        make_firmware_path(
            ArcPolyline(Points{Point(scale_i(5.0), scale_i(0.0)), Point(scale_i(6.0), scale_i(0.0))}),
            15.f, 400.f));
    append_path_extrusion(
        first_tool,
        make_firmware_path(
            ArcPolyline(Points{Point(scale_i(10.0), scale_i(0.0)), Point(scale_i(11.0), scale_i(0.0))}),
            15.f, 400.f, -1.f, -1.f, -1.f, ExtrusionRole::InternalInfill));
    group.layers.emplace_back();
    group.layers.back().print_z = scale_i(0.4);
    group.layers.back().tool_groups.emplace_back();
    group.layers.back().tool_groups.back().extruder_id = 0;

    Steps::StepExtrusionEdition::clean_and_prepare(print);
    Steps::StepExtrusionEdition::run_step(Orchestrator::instance(), print);

    const boost::filesystem::path output_path = temporary_gcode_path();
    remove_output_pair(output_path);
    Orchestrator &orchestrator = Orchestrator::instance();
    orchestrator.reset_plugin_cancel();
    try {
        Steps::StepGenerateGcode::run_step(orchestrator, print, output_path.string());
        const std::string output = read_text_file(output_path);
        INFO(output);

        CHECK(output.find("; USED L0 F0\n") != std::string::npos);
        const std::string second_layer_prefix = "; USED L1 F";
        const size_t second_layer_position = output.find(second_layer_prefix);
        REQUIRE(second_layer_position != std::string::npos);
        const size_t second_layer_value_begin = second_layer_position + second_layer_prefix.size();
        const size_t second_layer_value_end = output.find('\n', second_layer_value_begin);
        REQUIRE(second_layer_value_end != std::string::npos);
        CHECK(std::stod(output.substr(
                  second_layer_value_begin,
                  second_layer_value_end - second_layer_value_begin)) > 0.0);
    } catch (...) {
        orchestrator.reset_plugin_cancel();
        remove_output_pair(output_path);
        throw;
    }
    orchestrator.reset_plugin_cancel();
    remove_output_pair(output_path);
}
TEST_CASE("First layer bed temperature follows used tools unless overridden",
          "[plugins][gcode][script-type][temperature]") {
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.option<ConfigOptionInts>("first_layer_bed_temperature")->set(std::vector<int32_t>{55, 73});
    config.option<ConfigOptionInt>("print_first_layer_bed_temperature")->set_enabled(false);
    config.set_deserialize_strict({{"perimeter_extruder", 2}, {"infill_extruder", 2}, {"solid_infill_extruder", 2}});

    Print print;
    Model model;
    Test::init_print({make_cube(5., 5., 0.2)}, print, model, config);
    print.process();
    CHECK(print.first_layer_bed_temperature() == 73);

    PrintConfig &print_config = const_cast<PrintConfig &>(print.config());
    print_config.print_first_layer_bed_temperature.value = 64;
    print_config.print_first_layer_bed_temperature.set_enabled(true);
    CHECK(print.first_layer_bed_temperature() == 64);
}
TEST_CASE("End script uses stable defaults when a plan has no layers",
          "[plugins][gcode][script-type][firmware][integration]") {
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    select_printing_plan_writer(print);
    configure_standard_firmware(print);
    PrintConfig &config = const_cast<PrintConfig &>(print.config());
    config.start_gcode.value.clear();
    config.end_gcode.value = "; EMPTY_END L[layer_num] Z[layer_z] M[max_layer_z] F[filament_extruder_id] "
                             "P[previous_extruder] N[next_extruder]";
    print.mutable_printing_plan();

    Steps::StepExtrusionEdition::clean_and_prepare(print);
    Steps::StepExtrusionEdition::run_step(Orchestrator::instance(), print);

    const boost::filesystem::path output_path = temporary_gcode_path();
    remove_output_pair(output_path);
    Orchestrator &orchestrator = Orchestrator::instance();
    orchestrator.reset_plugin_cancel();
    try {
        Steps::StepGenerateGcode::run_step(orchestrator, print, output_path.string());
        const std::string output = read_text_file(output_path);
        INFO(output);
        CHECK(output == "; EMPTY_END L-1 Z0 M0 F0 P-1 N-1\n");
    } catch (...) {
        orchestrator.reset_plugin_cancel();
        remove_output_pair(output_path);
        throw;
    }
    orchestrator.reset_plugin_cancel();
    remove_output_pair(output_path);
}
