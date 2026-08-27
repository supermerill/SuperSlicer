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
Firmware dialect tests keep cross-dialect command matrices together so protocol differences remain directly comparable.
Shared plan construction and output helpers live in gcode_test_helpers so this file keeps only component-specific behavior.
*/

namespace {
using namespace Slic3r;
using namespace Slic3r::Printing;
using namespace Slic3r::Test::GCode;

class SprinterFirmwareProbe final :
    public slic3r_api::GCodeGeneration::Firmware::SprinterGCodeFirmwareSession
{
public:
    using SprinterGCodeFirmwareSession::encode_chamber_temperature;
    using SprinterGCodeFirmwareSession::encode_extruder_current;
};

class KlipperFirmwareProbe final :
    public slic3r_api::GCodeGeneration::Firmware::KlipperGCodeFirmwareSession
{
public:
    using KlipperGCodeFirmwareSession::encode_chamber_temperature;
    using KlipperGCodeFirmwareSession::encode_extruder_current;
};

} // namespace

TEST_CASE("G-code firmware service exposes its printer selector and standard session",
          "[plugins][gcode][firmware]")
{
    STATIC_REQUIRE(std::is_base_of<
        slic3r_api::GCodeFirmwareSession,
        slic3r_api::GCodeGeneration::DefaultGCodeFirmwareSession>::value);

    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Orchestrator &orchestrator = Orchestrator::instance();
    Print print;

    const std::set<std::string> expected_firmwares = {
        "gcode.firmware.marlin1",
        "gcode.firmware.marlin2",
        "gcode.firmware.prusa",
        "gcode.firmware.reprap",
        "gcode.firmware.sprinter",
        "gcode.firmware.klipper"
    };
    std::set<std::string> registered_firmwares;
    for (Plugin *plugin : orchestrator.get_active_plugins_for_step(GCODE_FIRMWARE))
        registered_firmwares.insert(plugin->get_id());
    CHECK(registered_firmwares == expected_firmwares);
    CHECK(orchestrator.get_plugin("gcode.firmware.default") == nullptr);

    Plugin *selected = Steps::selected_or_active_plugin_for_step(
        orchestrator, GCODE_FIRMWARE, &print.full_print_config());
    REQUIRE(selected != nullptr);
    CHECK(selected->get_id() == "gcode.firmware.marlin2");

    std::set<std::string> used_keys;
    for (const Plugin::UsedConfigKey &key : selected->get_used_config_keys())
        used_keys.insert(key.key);
    CHECK(used_keys.count("extruder_offset") == 1);
    CHECK(used_keys.count("filament_diameter") == 1);
    CHECK(used_keys.count("machine_limits_usage") == 1);
    CHECK(used_keys.count("machine_max_acceleration_x") == 1);
    CHECK(used_keys.count("machine_max_feedrate_x") == 1);
    CHECK(used_keys.count("machine_max_jerk_x") == 1);
    CHECK(used_keys.count("travel_acceleration") == 0);
    CHECK(used_keys.count("travel_speed") == 1);
    CHECK(used_keys.count("z_offset") == 1);

    const ConfigOptionDef *definition = PrintConfigDef::instance().get("gcode_firmware_plugin");
    REQUIRE(definition != nullptr);
    CHECK(definition->option_preset_type == RAW_PRESET_TYPE_FFF_PRINTER);
    CHECK(definition->invalidates_step == STEP_GCODE);

    const std::string merged = orchestrator.merged_ui_layout(
        "printer_fff.ui",
        "page:General:printer\n"
        "group:Firmware\n"
        "\tsetting:gcode_flavor\n");
    const size_t writer_pos = merged.find("setting:step_gcode_plugin");
    const size_t firmware_pos = merged.find("setting:gcode_firmware_plugin");
    REQUIRE(writer_pos != std::string::npos);
    REQUIRE(firmware_pos != std::string::npos);
    CHECK(writer_pos < firmware_pos);
    CHECK(std::string(slic3r_api::GCodeGeneration::Firmware::printer_ui_fragment())
              .find("gcode_firmware_plugin") != std::string::npos);

    STATIC_REQUIRE(std::is_base_of<
        slic3r_api::GCodeGeneration::Firmware::Marlin2GCodeFirmwareSession,
        slic3r_api::GCodeGeneration::Firmware::PrusaGCodeFirmwareSession>::value);

    // Printer presets that still name the removed generic provider resolve to
    // its concrete equivalent before the exclusive service is selected.
    Print migrated_print;
    DynamicPrintConfig &migrated_config =
        const_cast<DynamicPrintConfig &>(migrated_print.full_print_config());
    t_config_option_key migrated_key = "gcode_firmware_plugin";
    std::string migrated_value = "gcode.firmware.default";
    PrintConfigDef::handle_legacy_pair(migrated_key, migrated_value);
    CHECK(migrated_value == "gcode.firmware.marlin2");
    migrated_config.set_deserialize(migrated_key, migrated_value);
    Plugin *migrated_selection = Steps::selected_or_active_plugin_for_step(
        orchestrator, GCODE_FIRMWARE, &migrated_print.full_print_config());
    REQUIRE(migrated_selection != nullptr);
    CHECK(migrated_selection->get_id() == "gcode.firmware.marlin2");
}
TEST_CASE("Built-in firmware dialects encode their machine envelope preamble",
          "[plugins][gcode][firmware][dialects][machine-envelope]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    struct DialectEnvelopeExpectation
    {
        const char *id;
        const char *output;
    };
    const DialectEnvelopeExpectation expectations[] = {
        {
            "gcode.firmware.marlin1",
            "M201 X1000 Y1100 Z120 E1300 ; sets maximum accelerations, mm/sec^2\n"
            "M203 X200 Y210 Z12 E25 ; sets maximum feedrates, mm/sec\n"
            "M204 S500 T800 ; sets print and retract acceleration, mm/sec^2\n"
            "M205 X8.00 Y9.00 Z0.40 E2.50 ; sets the jerk limits, mm/sec\n"
            "M205 S3 T4 ; sets the minimum extruding and travel feed rate, mm/sec\n"
        },
        {
            "gcode.firmware.marlin2",
            "M201 X1000 Y1100 Z120 E1300 ; sets maximum accelerations, mm/sec^2\n"
            "M203 X200 Y210 Z12 E25 ; sets maximum feedrates, mm/sec\n"
            "M204 P500 R800 T900 ; sets print, retract and travel acceleration, mm/sec^2\n"
            "M205 X8.00 Y9.00 Z0.40 E2.50 ; sets the jerk limits, mm/sec\n"
            "M205 S3 T4 ; sets the minimum extruding and travel feed rate, mm/sec\n"
        },
        {
            "gcode.firmware.prusa",
            "M201 X1000 Y1100 Z120 E1300 ; sets maximum accelerations, mm/sec^2\n"
            "M203 X200 Y210 Z12 E25 ; sets maximum feedrates, mm/sec\n"
            "M204 P500 R800 T900 ; sets print, retract and travel acceleration, mm/sec^2\n"
            "M205 X8.00 Y9.00 Z0.40 E2.50 ; sets the jerk limits, mm/sec\n"
            "M205 S3 T4 ; sets the minimum extruding and travel feed rate, mm/sec\n"
        },
        {
            "gcode.firmware.reprap",
            "M201 X1000 Y1100 Z120 E1300 ; sets maximum accelerations, mm/sec^2\n"
            "M203 X12000 Y12600 Z720 E1500 I180 ; sets maximum feedrates, mm/min\n"
            "M204 P500 T900 ; sets print and travel acceleration, mm/sec^2\n"
            "M566 X480.00 Y540.00 Z24.00 E150.00 ; sets the jerk limits, mm/min\n"
        },
        {
            "gcode.firmware.sprinter",
            "M201 X1000 Y1100 Z120 E1300 ; sets maximum accelerations, mm/sec^2\n"
            "M203 X12000 Y12600 Z720 E1500 ; sets maximum feedrates, mm/min\n"
            "M204 P500 T900 ; sets print and travel acceleration, mm/sec^2\n"
        },
        {
            "gcode.firmware.klipper",
            "M204 P500 T900 ; sets the initial shared acceleration, mm/sec^2\n"
        }
    };

    for (const DialectEnvelopeExpectation &expectation : expectations) {
        INFO(expectation.id);
        Print print;
        configure_standard_firmware(print);
        configure_machine_envelope(print);
        print.mutable_printing_plan();
        CHECK(export_with_firmware(print, expectation.id) == expectation.output);
    }

    // The normal default keeps machine limits available for estimates without
    // inserting firmware commands into the file.
    Print disabled_print;
    configure_standard_firmware(disabled_print);
    disabled_print.mutable_printing_plan();
    CHECK(export_with_firmware(disabled_print, "gcode.firmware.marlin2").empty());
}
TEST_CASE("Built-in firmware dialects encode their process commands",
          "[plugins][gcode][firmware][dialects]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    struct DialectExpectation
    {
        const char *id;
        const char *tool;
        const char *temperature;
        const char *wait;
        const char *fan;
        const char *pressure_advance;
        const char *acceleration;
    };
    const DialectExpectation expectations[] = {
        {"gcode.firmware.marlin1", "T0\n", "M104 S200 T0\n", "M109 S200 T0\n",
         "M106 S128\n", "M900 K0.04\n", "M204 S500\n"},
        {"gcode.firmware.marlin2", "T0\n", "M104 S200 T0\n", "M109 S200 T0\n",
         "M106 S128\n", "M900 K0.04\n", "M204 P500\n"},
        {"gcode.firmware.prusa", "T0\n", "M104 S200 T0\n", "M109 S200 T0\n",
         "M106 S128\n", "M900 K0.04\n", "M204 P500\n"},
        {"gcode.firmware.reprap", "T0\n", "G10 P0 S200\n", "M116 P0\n",
         "M106 P0 S0.5\n", "M572 D0 S0.04\n", "M204 P500\n"},
        {"gcode.firmware.sprinter", "T0\n", "M104 S200 T0\n", "M109 S200 T0\n",
         "M106 S128\n", "M572 D0 S0.04\n", "M204 P500\n"},
        {"gcode.firmware.klipper", "ACTIVATE_EXTRUDER EXTRUDER=extruder\n",
         "M104 S200\n", "M109 S200\n", "M106 S128\n",
         "SET_PRESSURE_ADVANCE ADVANCE=0.04 EXTRUDER=extruder\n", "M204 S500\n"}
    };

    for (const DialectExpectation &expectation : expectations) {
        INFO(expectation.id);
        Print print;
        configure_standard_firmware(print);
        PrintingPlan &plan = print.mutable_printing_plan();
        plan.groups.emplace_back();
        plan.groups.front().layers.emplace_back();
        PrintingLayerGroup &layer = plan.groups.front().layers.front();
        layer.tool_groups.emplace_back();
        PrintingToolGroup &tool_group = layer.tool_groups.front();
        tool_group.extruder_id = 0;
        ExtrusionEntity::Children children;
        children.push_back(make_firmware_path(
                ArcPolyline(Points{
                    Point(scale_i(0.0), scale_i(0.0)),
                    Point(scale_i(1.0), scale_i(0.0))
                }),
                20.f,
                500.f,
                0.04f,
                50.f,
                200.f));
        children.push_back(
            special_command_entity(ExtrusionPropertySpecialCommand::Code::WAIT_FOR_TEMP));
        std::unique_ptr<ExtrusionEntity> root(
            new ExtrusionEntity(std::move(children), false, true, true));
        root->add_property(ExtrusionPropertySpeed(20.f, 500.f, 0.04f, 50.f, 200.f));
        append_path_extrusion(tool_group, std::move(root));

        const std::string output = export_with_firmware(print, expectation.id);
        INFO(output);
        CHECK(output.find(expectation.tool) != std::string::npos);
        CHECK(output.find(expectation.temperature) != std::string::npos);
        CHECK(output.find(expectation.wait) != std::string::npos);
        CHECK(output.find(expectation.fan) != std::string::npos);
        CHECK(output.find(expectation.pressure_advance) != std::string::npos);
        CHECK(output.find(expectation.acceleration) != std::string::npos);
        CHECK(output.find("G1 X1 E") != std::string::npos);
    }
}
TEST_CASE("Built-in firmware acceleration acknowledgements match their physical registers",
          "[plugins][gcode][firmware][dialects][acceleration]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    const char *const independent_dialects[] = {
        "gcode.firmware.marlin2",
        "gcode.firmware.prusa",
        "gcode.firmware.reprap"
    };
    for (const char *firmware_id : independent_dialects) {
        INFO(firmware_id);
        Print print;
        configure_standard_firmware(print);
        PrintingPlan &plan = print.mutable_printing_plan();
        plan.groups.emplace_back();
        plan.groups.front().layers.emplace_back();
        PrintingLayerGroup &layer = plan.groups.front().layers.front();
        layer.tool_groups.emplace_back();
        PrintingToolGroup &tool_group = layer.tool_groups.front();
        tool_group.extruder_id = 0;
        append_path_extrusion(tool_group, make_firmware_path(
            ArcPolyline(Points{Point(scale_i(0.0), scale_i(0.0)), Point(scale_i(1.0), scale_i(0.0))}),
            20.f, 500.f));
        append_path_extrusion(tool_group, make_firmware_travel(
            ArcPolyline(Points{Point(scale_i(1.0), scale_i(0.0)), Point(scale_i(2.0), scale_i(0.0))}),
            80.f, 900.f));
        append_path_extrusion(tool_group, make_firmware_path(
            ArcPolyline(Points{Point(scale_i(2.0), scale_i(0.0)), Point(scale_i(3.0), scale_i(0.0))}),
            20.f, 500.f));

        const std::string output = export_with_firmware(print, firmware_id);
        INFO(output);
        CHECK(count_occurrences(output, "M204 P500\n") == 1);
        CHECK(count_occurrences(output, "M204 T900\n") == 1);
    }

    struct SharedAccelerationExpectation
    {
        const char *id;
        const char *print_command;
        const char *travel_command;
    };
    const SharedAccelerationExpectation shared_dialects[] = {
        {"gcode.firmware.marlin1", "M204 S500\n", "M204 S900\n"},
        {"gcode.firmware.sprinter", "M204 P500\n", "M204 P900\n"},
        {"gcode.firmware.klipper", "M204 S500\n", "M204 S900\n"}
    };
    for (const SharedAccelerationExpectation &expectation : shared_dialects) {
        INFO(expectation.id);
        Print print;
        configure_standard_firmware(print);
        PrintingPlan &plan = print.mutable_printing_plan();
        plan.groups.emplace_back();
        plan.groups.front().layers.emplace_back();
        PrintingLayerGroup &layer = plan.groups.front().layers.front();
        layer.tool_groups.emplace_back();
        PrintingToolGroup &tool_group = layer.tool_groups.front();
        tool_group.extruder_id = 0;
        append_path_extrusion(tool_group, make_firmware_path(
            ArcPolyline(Points{Point(scale_i(0.0), scale_i(0.0)), Point(scale_i(1.0), scale_i(0.0))}),
            20.f, 500.f));
        append_path_extrusion(tool_group, make_firmware_travel(
            ArcPolyline(Points{Point(scale_i(1.0), scale_i(0.0)), Point(scale_i(2.0), scale_i(0.0))}),
            80.f, 900.f));
        append_path_extrusion(tool_group, make_firmware_path(
            ArcPolyline(Points{Point(scale_i(2.0), scale_i(0.0)), Point(scale_i(3.0), scale_i(0.0))}),
            20.f, 500.f));

        const std::string output = export_with_firmware(print, expectation.id);
        INFO(output);
        CHECK(count_occurrences(output, expectation.print_command) == 2);
        CHECK(count_occurrences(output, expectation.travel_command) == 1);

        // Even equal logical values are encoded again after the movement
        // category changes: the shared command acknowledges the current
        // category and explicitly invalidates the opposite one.
        Print equal_print;
        configure_standard_firmware(equal_print);
        PrintingPlan &equal_plan = equal_print.mutable_printing_plan();
        equal_plan.groups.emplace_back();
        equal_plan.groups.front().layers.emplace_back();
        PrintingLayerGroup &equal_layer = equal_plan.groups.front().layers.front();
        equal_layer.tool_groups.emplace_back();
        PrintingToolGroup &equal_tool_group = equal_layer.tool_groups.front();
        equal_tool_group.extruder_id = 0;
        append_path_extrusion(equal_tool_group, make_firmware_path(
            ArcPolyline(Points{Point(scale_i(0.0), scale_i(0.0)), Point(scale_i(1.0), scale_i(0.0))}),
            20.f, 500.f));
        append_path_extrusion(equal_tool_group, make_firmware_travel(
            ArcPolyline(Points{Point(scale_i(1.0), scale_i(0.0)), Point(scale_i(2.0), scale_i(0.0))}),
            80.f, 500.f));
        append_path_extrusion(equal_tool_group, make_firmware_path(
            ArcPolyline(Points{Point(scale_i(2.0), scale_i(0.0)), Point(scale_i(3.0), scale_i(0.0))}),
            20.f, 500.f));

        const std::string equal_output = export_with_firmware(equal_print, expectation.id);
        INFO(equal_output);
        CHECK(count_occurrences(equal_output, expectation.print_command) == 3);
    }
}
TEST_CASE("Klipper uses configured extruder names and I J arc centers",
          "[plugins][gcode][firmware][dialects][klipper]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    configure_standard_firmware(print, 2);
    PrintConfig &print_config = const_cast<PrintConfig &>(print.config());
    print_config.tool_name.set(
        std::vector<std::string>{"left_extruder", "right_extruder"});
    print_config.tool_name.set_enabled(true);

    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    plan.groups.front().layers.emplace_back();
    PrintingLayerGroup &layer = plan.groups.front().layers.front();
    layer.tool_groups.emplace_back();
    PrintingToolGroup &tool_group = layer.tool_groups.front();
    tool_group.extruder_id = 1;
    Geometry::ArcWelder::Path arc_path;
    arc_path.emplace_back(Point(scale_i(0.0), scale_i(0.0)));
    arc_path.emplace_back(
        Point(scale_i(10.0), scale_i(0.0)),
        float(scale_i(5.0)),
        Geometry::ArcWelder::Orientation::CCW);
    append_path_extrusion(
        tool_group,
        make_firmware_path(ArcPolyline(arc_path), 25.f, 600.f, 0.03f));

    const std::string output = export_with_firmware(print, "gcode.firmware.klipper");
    INFO(output);
    CHECK(output.find("ACTIVATE_EXTRUDER EXTRUDER=right_extruder\n") != std::string::npos);
    CHECK(output.find("SET_PRESSURE_ADVANCE ADVANCE=0.03 EXTRUDER=right_extruder\n") !=
          std::string::npos);
    CHECK(output.find("G3 X10 I") != std::string::npos);
    CHECK(output.find(" J") != std::string::npos);
    CHECK(output.find(" R5") == std::string::npos);
}
TEST_CASE("Radius-arc firmware dialects preserve R-form movements",
          "[plugins][gcode][firmware][dialects][arcs]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    const char *const firmware_ids[] = {
        "gcode.firmware.marlin1",
        "gcode.firmware.marlin2",
        "gcode.firmware.prusa",
        "gcode.firmware.reprap",
        "gcode.firmware.sprinter"
    };
    for (const char *firmware_id : firmware_ids) {
        INFO(firmware_id);
        Print print;
        configure_standard_firmware(print);
        PrintingPlan &plan = print.mutable_printing_plan();
        plan.groups.emplace_back();
        plan.groups.front().layers.emplace_back();
        PrintingLayerGroup &layer = plan.groups.front().layers.front();
        layer.tool_groups.emplace_back();
        PrintingToolGroup &tool_group = layer.tool_groups.front();
        tool_group.extruder_id = 0;

        Geometry::ArcWelder::Path arc_path;
        arc_path.emplace_back(Point(scale_i(0.0), scale_i(0.0)));
        arc_path.emplace_back(
            Point(scale_i(10.0), scale_i(0.0)),
            float(scale_i(5.0)),
            Geometry::ArcWelder::Orientation::CCW);
        append_path_extrusion(
            tool_group,
            make_firmware_path(ArcPolyline(arc_path), 25.f, 600.f));

        const std::string output = export_with_firmware(print, firmware_id);
        INFO(output);
        CHECK(output.find("G3 X10 R5 E") != std::string::npos);
        CHECK(output.find(" I") == std::string::npos);
        CHECK(output.find(" J") == std::string::npos);
    }
}
TEST_CASE("Unsupported dialect operations remain explicit G-code comments",
          "[plugins][gcode][firmware][dialects]")
{
    SprinterFirmwareProbe sprinter;
    KlipperFirmwareProbe klipper;

    CHECK(sprinter.encode_chamber_temperature(50, false) ==
          "; Unsupported firmware operation: chamber temperature\n");
    CHECK(sprinter.encode_extruder_current(0, 650.0) ==
          "; Unsupported firmware operation: extruder current\n");
    CHECK(klipper.encode_chamber_temperature(50, false) ==
          "; Unsupported firmware operation: chamber temperature\n");
    CHECK(klipper.encode_extruder_current(0, 650.0) ==
          "; Unsupported firmware operation: extruder current\n");
}
TEST_CASE("Marlin 2 shares fan state but keeps heater state per extruder",
          "[plugins][gcode][firmware]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    select_printing_plan_writer(print);
    configure_standard_firmware(print, 2);
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    plan.groups.front().layers.emplace_back();
    PrintingLayerGroup &layer = plan.groups.front().layers.front();

    append_multi_tool_process_sequence(layer);

    const boost::filesystem::path output_path = temporary_gcode_path();
    remove_output_pair(output_path);
    try {
        Steps::StepGenerateGcode::run_step(Orchestrator::instance(), print, output_path.string());
        const std::string output = read_text_file(output_path);
        INFO(output);
        CHECK(count_command_lines(output, "T0") == 2);
        CHECK(count_command_lines(output, "T1") == 1);
        CHECK(count_occurrences(output, "M104 S200 T0\n") == 1);
        CHECK(count_occurrences(output, "M104 S210 T1\n") == 1);
        CHECK(count_occurrences(output, "M106 S51\n") == 2);
        CHECK(count_occurrences(output, "M106 S77\n") == 1);
        CHECK(count_occurrences(output, "M900 K0.01\n") == 1);
        CHECK(count_occurrences(output, "M900 K0.02\n") == 1);

        // Tool zero resumes its own heater and absolute E histories. The fan
        // is re-encoded because M106 controls shared hardware whose last value
        // was changed while tool one was active.
        CHECK(output.find("G1 X2 E.03326") != std::string::npos);
        CHECK(output.find("G1 X3 E.06652") != std::string::npos);
    } catch (...) {
        remove_output_pair(output_path);
        throw;
    }
    remove_output_pair(output_path);
}
TEST_CASE("RepRapFirmware keeps addressed heaters and fans per extruder",
          "[plugins][gcode][firmware][dialects][reprap]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    configure_standard_firmware(print, 2);
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    plan.groups.front().layers.emplace_back();
    append_multi_tool_process_sequence(plan.groups.front().layers.front());

    const std::string output = export_with_firmware(print, "gcode.firmware.reprap");
    INFO(output);
    CHECK(count_occurrences(output, "G10 P0 S200\n") == 1);
    CHECK(count_occurrences(output, "G10 P1 S210\n") == 1);
    CHECK(count_occurrences(output, "M106 P0 S0.2\n") == 1);
    CHECK(count_occurrences(output, "M106 P1 S0.3\n") == 1);
}
