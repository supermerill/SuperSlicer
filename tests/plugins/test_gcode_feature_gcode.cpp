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
Feature G-code tests verify role-transition annotations and their serialized PlaceholderParser context.
Shared plan construction and output helpers live in gcode_test_helpers so this file keeps only component-specific behavior.
*/

namespace {
using namespace Slic3r;
using namespace Slic3r::Printing;
using namespace Slic3r::Test::GCode;

} // namespace

TEST_CASE("Feature G-code follows explicit role transitions without duplicating annotations",
          "[plugins][gcode][feature-gcode][extrusion-edit][firmware]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    select_printing_plan_writer(print);
    configure_standard_firmware(print);
    PrintConfig &config = const_cast<PrintConfig &>(print.config());
    config.start_gcode.value.clear();
    config.end_gcode.value.clear();
    config.before_layer_gcode.value.clear();
    config.layer_gcode.value.clear();
    config.toolchange_gcode.value.clear();
    config.between_objects_gcode.value.clear();
    config.start_filament_gcode.set(std::vector<std::string>{""});
    config.end_filament_gcode.set(std::vector<std::string>{""});
    config.feature_gcode.value =
        "; FEATURE [previous_extrusion_role]>[next_extrusion_role]";

    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    plan.groups.emplace_back();

    PrintingGroup &first_group = plan.groups[0];
    first_group.layers.reserve(2);
    first_group.layers.emplace_back();
    first_group.layers.front().print_z = scale_i(0.2);
    first_group.layers.front().tool_groups.emplace_back();
    PrintingToolGroup &first_tool = first_group.layers.front().tool_groups.front();
    first_tool.extruder_id = 0;

    append_path_extrusion(
        first_tool,
        make_firmware_path(
            ArcPolyline(Points{Point(scale_i(0.0), scale_i(0.0)), Point(scale_i(1.0), scale_i(0.0))}),
            15.f, 400.f));
    append_path_extrusion(
        first_tool,
        make_firmware_path(
            ArcPolyline(Points{Point(scale_i(1.0), scale_i(0.0)), Point(scale_i(2.0), scale_i(0.0))}),
            15.f, 400.f));
    std::unique_ptr<ExtrusionPath> travel = make_firmware_travel(
        ArcPolyline(Points{Point(scale_i(2.0), scale_i(0.0)), Point(scale_i(5.0), scale_i(0.0))}),
        80.f, 800.f);
    travel->add_property(ExtrusionPropertyCustomGcodeText(
        ExtrusionPropertyCustomGcodeText::Code::GCODE, "M117 existing travel"));
    append_path_extrusion(first_tool, std::move(travel));

    first_group.layers.emplace_back();
    PrintingLayerGroup &second_layer = first_group.layers.back();
    second_layer.print_z = scale_i(0.4);
    second_layer.tool_groups.reserve(2);
    second_layer.tool_groups.emplace_back();
    PrintingToolGroup &same_role_next_layer = second_layer.tool_groups.back();
    same_role_next_layer.extruder_id = 0;
    append_path_extrusion(
        same_role_next_layer,
        make_firmware_travel(
            ArcPolyline(Points{Point(scale_i(5.0), scale_i(0.0)), Point(scale_i(5.2), scale_i(0.0))}),
            80.f, 800.f));

    second_layer.tool_groups.emplace_back();
    PrintingToolGroup &same_role_next_tool = second_layer.tool_groups.back();
    same_role_next_tool.extruder_id = 0;
    append_path_extrusion(
        same_role_next_tool,
        make_firmware_travel(
            ArcPolyline(Points{Point(scale_i(5.2), scale_i(0.0)), Point(scale_i(5.4), scale_i(0.0))}),
            80.f, 800.f));

    PrintingGroup &second_group = plan.groups[1];
    second_group.layers.emplace_back();
    second_group.layers.front().print_z = scale_i(0.2);
    second_group.layers.front().tool_groups.emplace_back();
    PrintingToolGroup &second_tool = second_group.layers.front().tool_groups.front();
    second_tool.extruder_id = 0;
    append_path_extrusion(
        second_tool,
        make_firmware_path(
            ArcPolyline(Points{Point(scale_i(5.0), scale_i(0.0)), Point(scale_i(6.0), scale_i(0.0))}),
            15.f, 400.f, -1.f, -1.f, -1.f, ExtrusionRole::InternalInfill));

    Steps::StepExtrusionEdition::clean_and_prepare(print);
    Steps::StepExtrusionEdition::run_step(Orchestrator::instance(), print);

    const ExtrusionEntity &first_root = *first_tool.extrusions[0].root;
    const ExtrusionPropertyCustomGcode *first_property =
        first_root.get_property<ExtrusionPropertyCustomGcode>();
    REQUIRE(first_property != nullptr);
    CHECK(first_property->script_type == GCODE_SCRIPT_TYPE_FEATURE_GCODE);
    CHECK(stored_script_string(first_root, *first_property, "previous_extrusion_role") == "Unknown");
    CHECK(stored_script_string(first_root, *first_property, "next_extrusion_role") == "Perimeter");
    CHECK(stored_script_string(first_root, *first_property, "last_extrusion_role") == "Unknown");
    CHECK(stored_script_string(first_root, *first_property, "extrusion_role") == "Perimeter");

    const ExtrusionEntity &same_role_root = *first_tool.extrusions[1].root;
    CHECK(same_role_root.get_property<ExtrusionPropertyCustomGcode>() == nullptr);

    const ExtrusionEntity &travel_root = *first_tool.extrusions[2].root;
    REQUIRE(travel_root.child_count() == 2);
    const ExtrusionEntity &feature_event = travel_root.child(0);
    const ExtrusionPropertyCustomGcode *travel_property =
        feature_event.get_property<ExtrusionPropertyCustomGcode>();
    REQUIRE(travel_property != nullptr);
    CHECK(travel_property->script_type == GCODE_SCRIPT_TYPE_FEATURE_GCODE);
    CHECK(stored_script_string(feature_event, *travel_property, "previous_extrusion_role") == "Perimeter");
    CHECK(stored_script_string(feature_event, *travel_property, "next_extrusion_role") == "Travel");
    const ExtrusionPropertyCustomGcode *preserved_property =
        travel_root.child(1).get_property<ExtrusionPropertyCustomGcode>();
    REQUIRE(preserved_property != nullptr);
    CHECK(travel_root.child(1).custom_gcode_string(*preserved_property) == "M117 existing travel");

    CHECK(same_role_next_layer.extrusions[0].root->get_property<ExtrusionPropertyCustomGcode>() == nullptr);
    CHECK(same_role_next_tool.extrusions[0].root->get_property<ExtrusionPropertyCustomGcode>() == nullptr);

    const ExtrusionEntity &infill_root = *second_tool.extrusions[0].root;
    const ExtrusionPropertyCustomGcode *infill_property =
        infill_root.get_property<ExtrusionPropertyCustomGcode>();
    REQUIRE(infill_property != nullptr);
    CHECK(stored_script_string(infill_root, *infill_property, "previous_extrusion_role") == "Travel");
    CHECK(stored_script_string(infill_root, *infill_property, "next_extrusion_role") == "Internal infill");

    // A second execution removes and recreates only this plugin's annotations.
    // The custom travel remains in the preserved second child and the feature
    // event remains the first child of exactly one ordered wrapper.
    Steps::StepExtrusionEdition::clean_and_prepare(print);
    Steps::StepExtrusionEdition::run_step(Orchestrator::instance(), print);
    CHECK(first_root.child_count() == 0);
    REQUIRE(travel_root.child_count() == 2);
    CHECK(travel_root.child(0).child_count() == 0);
    CHECK(travel_root.child(1).child_count() == 0);

    const std::string output = export_with_firmware(print, "gcode.firmware.marlin2");
    INFO(output);
    CHECK(count_occurrences(output, "; FEATURE ") == 3);
    const size_t first_feature = output.find("; FEATURE Unknown>Perimeter\n");
    const size_t travel_feature = output.find("; FEATURE Perimeter>Travel\n");
    const size_t preserved_event = output.find("M117 existing travel\n");
    const size_t travel_move = output.find("G0 X5", travel_feature);
    const size_t infill_feature = output.find("; FEATURE Travel>Internal infill\n", travel_move);
    const size_t infill_move = output.find("G1 X6", infill_feature);
    REQUIRE(first_feature != std::string::npos);
    REQUIRE(travel_feature != std::string::npos);
    REQUIRE(preserved_event != std::string::npos);
    REQUIRE(travel_move != std::string::npos);
    REQUIRE(infill_feature != std::string::npos);
    REQUIRE(infill_move != std::string::npos);
    CHECK(first_feature < travel_feature);
    CHECK(travel_feature < preserved_event);
    CHECK(preserved_event < travel_move);
    CHECK(travel_move < infill_feature);
    CHECK(infill_feature < infill_move);

    config.feature_gcode.value.clear();
    Steps::StepExtrusionEdition::clean_and_prepare(print);
    Steps::StepExtrusionEdition::run_step(Orchestrator::instance(), print);
    CHECK(first_root.get_property<ExtrusionPropertyCustomGcode>() == nullptr);
    CHECK(travel_root.child_count() == 0);
    const ExtrusionPropertyCustomGcode *cleaned_travel_property =
        travel_root.get_property<ExtrusionPropertyCustomGcode>();
    REQUIRE(cleaned_travel_property != nullptr);
    CHECK(cleaned_travel_property->script_type == GCODE_SCRIPT_TYPE_INVALID);
    CHECK(cleaned_travel_property->config_id == EXTRUSION_DATA_ID_INVALID);
    CHECK(infill_root.get_property<ExtrusionPropertyCustomGcode>() == nullptr);
}
TEST_CASE("Feature G-code validates and normalizes rich extrusion roles",
          "[plugins][gcode][feature-gcode][extrusion-edit][validation]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    auto prepare_role = [](raw_extrusion_role raw_role) {
        std::unique_ptr<Print> print(new Print());
        PrintConfig &config = const_cast<PrintConfig &>(print->config());
        config.feature_gcode.value = "; FEATURE [next_extrusion_role]";
        PrintingPlan &plan = print->mutable_printing_plan();
        plan.groups.emplace_back();
        plan.groups.front().layers.emplace_back();
        plan.groups.front().layers.front().tool_groups.emplace_back();
        PrintingToolGroup &tool_group = plan.groups.front().layers.front().tool_groups.front();
        tool_group.extruder_id = 0;
        std::unique_ptr<ExtrusionPath> path = make_firmware_path(
            ArcPolyline(Points{Point(0, 0), Point(scale_i(1.0), 0)}),
            15.f, 400.f);
        ExtrusionAttributes *attributes = path->get_property<ExtrusionAttributes>();
        REQUIRE(attributes != nullptr);
        // Build a valid path first, then alter its raw payload so malformed
        // ABI values reach FeatureGCode instead of tripping ExtrusionRole's
        // debug assertion inside the test fixture itself.
        attributes->role = raw_role;
        append_path_extrusion(tool_group, std::move(path));
        return print;
    };

    SECTION("Unsupported but structurally valid roles use Custom") {
        std::unique_ptr<Print> print = prepare_role(
            RAW_EXTRUSION_ROLE_PERIMETER | RAW_EXTRUSION_ROLE_SOLID);
        Steps::StepExtrusionEdition::clean_and_prepare(*print);
        Steps::StepExtrusionEdition::run_step(Orchestrator::instance(), *print);
        const ExtrusionEntity &root =
            *print->mutable_printing_plan().groups.front().layers.front().tool_groups.front().extrusions.front().root;
        const ExtrusionPropertyCustomGcode *property =
            root.get_property<ExtrusionPropertyCustomGcode>();
        REQUIRE(property != nullptr);
        CHECK(stored_script_string(root, *property, "next_extrusion_role") == "Custom");
    }

    SECTION("Multiple base roles reject the plugin run") {
        std::unique_ptr<Print> print = prepare_role(
            RAW_EXTRUSION_ROLE_PERIMETER | RAW_EXTRUSION_ROLE_INFILL);
        Orchestrator &orchestrator = Orchestrator::instance();
        orchestrator.reset_plugin_cancel();
        (void)orchestrator.consume_plugin_messages();
        Steps::StepExtrusionEdition::clean_and_prepare(*print);
        Steps::StepExtrusionEdition::run_step(orchestrator, *print);
        CHECK(orchestrator.is_plugin_cancelled());
        bool found_role_error = false;
        for (const Orchestrator::PluginMessage &message : orchestrator.consume_plugin_messages())
            found_role_error = found_role_error ||
                               message.message.find("invalid extrusion role") != std::string::npos;
        CHECK(found_role_error);
        orchestrator.reset_plugin_cancel();
    }

    SECTION("Unknown role bits reject the plugin run") {
        std::unique_ptr<Print> print = prepare_role(raw_extrusion_role(uint32_t(1) << 15));
        Orchestrator &orchestrator = Orchestrator::instance();
        orchestrator.reset_plugin_cancel();
        (void)orchestrator.consume_plugin_messages();
        Steps::StepExtrusionEdition::clean_and_prepare(*print);
        Steps::StepExtrusionEdition::run_step(orchestrator, *print);
        CHECK(orchestrator.is_plugin_cancelled());
        bool found_role_error = false;
        for (const Orchestrator::PluginMessage &message : orchestrator.consume_plugin_messages())
            found_role_error = found_role_error ||
                               message.message.find("invalid extrusion role") != std::string::npos;
        CHECK(found_role_error);
        orchestrator.reset_plugin_cancel();
    }
}
