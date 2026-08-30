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
G-code script processor tests cover the host PlaceholderParser bridge, stored Config snapshots and machine-state import.
Shared plan construction and output helpers live in gcode_test_helpers so this file keeps only component-specific behavior.
*/

namespace {
using namespace Slic3r;
using namespace Slic3r::Printing;
using namespace Slic3r::Test::GCode;

// Verifies that a derived firmware may augment the host-prepared context at
// execution time without owning or exposing the PlaceholderParser itself.
class ScriptContextCompletingFirmwareProbe final :
    public slic3r_api::GCodeGeneration::DefaultGCodeFirmwareSession
{
public:
    explicit ScriptContextCompletingFirmwareProbe(
        slic3r_api::GCodeGeneration::GCodeScriptProcessorView scripts,
        storage_handle *storage = nullptr) :
        DefaultGCodeFirmwareSession(scripts, storage)
    {}

    using DefaultGCodeFirmwareSession::process_script;

    uint32_t completion_count() const { return m_completion_count; }

protected:
    void complete_script_context(
        gcode_script_type script_type,
        uint16_t,
        slic3r_api::GCodeGeneration::GCodeScriptConfig &config) override
    {
        ++m_completion_count;
        if (script_type == GCODE_SCRIPT_TYPE_START_GCODE && config.has("max_layer_z"))
            config.set("max_layer_z", 12.5);
    }

private:
    uint32_t m_completion_count = 0;
};

} // namespace

TEST_CASE("Host G-code script processor prepares typed isolated contexts",
          "[plugins][gcode][firmware][custom-gcode]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    configure_standard_firmware(print, 2);
    DynamicPrintConfig &full_config =
        const_cast<DynamicPrintConfig &>(print.full_print_config());
    full_config.set_key_value("travel_speed", new ConfigOptionFloat(100.0));
    Orchestrator &orchestrator = Orchestrator::instance();
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    plan.groups.front().layers.emplace_back();
    plan.groups.front().layers.front().tool_groups.emplace_back();
    plan.groups.front().layers.front().tool_groups.front().extruder_id = 1;
    GCodeScriptProcessor host_processor(print, orchestrator);
    const slic3r_api::GCodeGeneration::GCodeScriptProcessorView scripts(
        host_processor.c_processor());
    CHECK_THROWS(scripts.prepare(GCODE_SCRIPT_TYPE_CUSTOM_BEGIN - 1));

    PluginStorage argument_storage;
    storage_handle *argument_storage_handle =
        reinterpret_cast<storage_handle *>(&argument_storage);
    slic3r_api::StoredExtrusionEntity layer_event(
        argument_storage_handle);
    slic3r_api::StoredConfig layer_arguments(argument_storage_handle);
    layer_arguments.get_or_add("layer_num", SLIC3R_CONFIG_OPTION_INT).set_int(12);
    layer_arguments.get_or_add("layer_z", SLIC3R_CONFIG_OPTION_FLOAT).set_float(2.6);
    layer_arguments.get_or_add("previous_layer_z", SLIC3R_CONFIG_OPTION_FLOAT).set_float(2.4);
    layer_arguments.get_or_add("max_layer_z", SLIC3R_CONFIG_OPTION_FLOAT).set_float(20.0);
    layer_arguments.get_or_add("custom_bool", SLIC3R_CONFIG_OPTION_BOOL).set_bool(true);
    layer_arguments.get_or_add("custom_int", SLIC3R_CONFIG_OPTION_INT).set_int(-7);
    layer_arguments.get_or_add("custom_float", SLIC3R_CONFIG_OPTION_FLOAT).set_float(3.25);
    layer_arguments.get_or_add("custom_string", SLIC3R_CONFIG_OPTION_STRING).set_string("typed");
    slic3r_api::MutableConfigOption custom_ints =
        layer_arguments.get_or_add("custom_ints", SLIC3R_CONFIG_OPTION_INTS);
    custom_ints.resize(2);
    custom_ints.set_int(2, 0);
    custom_ints.set_int(4, 1);
    slic3r_api::MutableConfigOption custom_floats =
        layer_arguments.get_or_add("custom_floats", SLIC3R_CONFIG_OPTION_FLOATS);
    custom_floats.resize(2);
    custom_floats.set_float(1.5, 0);
    custom_floats.set_float(2.5, 1);
    const slic3r_api::EPropertyCustomGcode &layer_property = layer_event.script_gcode(
        "M117 layer", GCODE_SCRIPT_TYPE_BEFORE_LAYER_GCODE, layer_arguments);
    slic3r_api::StoredConfig stored_arguments(argument_storage_handle);
    stored_arguments.deserialize_all(layer_event.stored_string(layer_property.config_id));

    const slic3r_api::GCodeGeneration::GCodeScriptContext prepared_layer =
        scripts.prepare(GCODE_SCRIPT_TYPE_BEFORE_LAYER_GCODE, stored_arguments);
    CHECK(prepared_layer.config().get_int("layer_num") == 12);
    CHECK(prepared_layer.config().get_float("layer_z") == Approx(2.6));
    CHECK(prepared_layer.config().get_float("previous_layer_z") == Approx(2.4));
    CHECK(prepared_layer.config().get_float("max_layer_z") == Approx(20.0));
    CHECK(prepared_layer.config().get_bool("custom_bool"));
    CHECK(prepared_layer.config().get_int("custom_int") == -7);
    CHECK(prepared_layer.config().get_float("custom_float") == Approx(3.25));
    CHECK(prepared_layer.config().get_string("custom_string") == "typed");
    CHECK((prepared_layer.config().get_ints("custom_ints") == std::vector<int32_t>{2, 4}));
    CHECK((prepared_layer.config().get_floats("custom_floats") == std::vector<double>{1.5, 2.5}));
    CHECK(prepared_layer.process("L[layer_num] Z[layer_z] P[previous_layer_z] M[max_layer_z]", 0) ==
          "L12 Z2.6 P2.4 M20");

    // Feature scripts use the same producer Config path as every other script.
    // The modern names and their historical aliases therefore arrive as four
    // ordinary typed strings, with no feature-specific host context structure.
    slic3r_api::StoredConfig feature_arguments(argument_storage_handle);
    feature_arguments.get_or_add("previous_extrusion_role", SLIC3R_CONFIG_OPTION_STRING)
        .set_string("Travel");
    feature_arguments.get_or_add("next_extrusion_role", SLIC3R_CONFIG_OPTION_STRING)
        .set_string("Internal infill");
    feature_arguments.get_or_add("last_extrusion_role", SLIC3R_CONFIG_OPTION_STRING)
        .set_string("Travel");
    feature_arguments.get_or_add("extrusion_role", SLIC3R_CONFIG_OPTION_STRING)
        .set_string("Internal infill");
    const slic3r_api::GCodeGeneration::GCodeScriptContext prepared_feature =
        scripts.prepare(GCODE_SCRIPT_TYPE_FEATURE_GCODE, feature_arguments);
    CHECK(prepared_feature.process(
              "[previous_extrusion_role]>[next_extrusion_role] "
              "[last_extrusion_role]>[extrusion_role]", 0) ==
          "Travel>Internal infill Travel>Internal infill");

    // Producers may populate structural placeholders, but they cannot change
    // the type promised by the host's built-in script definition.
    slic3r_api::StoredExtrusionEntity wrong_type_event(
        argument_storage_handle);
    slic3r_api::StoredConfig wrong_type_arguments(argument_storage_handle);
    wrong_type_arguments.get_or_add("layer_num", SLIC3R_CONFIG_OPTION_FLOAT).set_float(12.0);
    const slic3r_api::EPropertyCustomGcode &wrong_type_property = wrong_type_event.script_gcode(
        "M117 wrong", GCODE_SCRIPT_TYPE_BEFORE_LAYER_GCODE, wrong_type_arguments);
    slic3r_api::StoredConfig stored_wrong_type(argument_storage_handle);
    stored_wrong_type.deserialize_all(wrong_type_event.stored_string(wrong_type_property.config_id));
    CHECK_THROWS(scripts.prepare(GCODE_SCRIPT_TYPE_BEFORE_LAYER_GCODE, stored_wrong_type));

    // Runtime machine values are owned by the firmware and remain protected
    // even when a producer transports an otherwise valid typed vector.
    slic3r_api::StoredExtrusionEntity reserved_event(
        argument_storage_handle);
    slic3r_api::StoredConfig reserved_arguments(argument_storage_handle);
    slic3r_api::MutableConfigOption position =
        reserved_arguments.get_or_add("position", SLIC3R_CONFIG_OPTION_FLOATS);
    position.resize(3);
    position.set_float(1.0, 0);
    position.set_float(2.0, 1);
    position.set_float(3.0, 2);
    const slic3r_api::EPropertyCustomGcode &reserved_property = reserved_event.script_gcode(
        "M117 reserved", GCODE_SCRIPT_TYPE_EXTRUSION_CUSTOM, reserved_arguments);
    slic3r_api::StoredConfig stored_reserved(argument_storage_handle);
    stored_reserved.deserialize_all(reserved_event.stored_string(reserved_property.config_id));
    CHECK_THROWS(scripts.prepare(GCODE_SCRIPT_TYPE_EXTRUSION_CUSTOM, stored_reserved));

    // Every name receives fresh machine state, while known names additionally
    // expose the exact option types declared by the legacy placeholder table.
    for (const auto &named_placeholders : custom_gcode_specific_placeholders()) {
        const slic3r_api::GCodeGeneration::GCodeScriptContext context = scripts.prepare(
            registered_script_type(orchestrator, named_placeholders.first));
        const slic3r_api::GCodeGeneration::GCodeScriptConfig config = context.config();
        for (const std::string &key : named_placeholders.second) {
            INFO(named_placeholders.first << ": " << key);
            const ConfigOptionDef *definition = custom_gcode_specific_config_def.get(key);
            REQUIRE(definition != nullptr);
            const config_option_handle *option = config_get(config.handle(), key.c_str());
            REQUIRE(option != nullptr);
            CHECK(config_option_type_get(option) ==
                  static_cast<config_option_type>(definition->type));
        }
    }

    const slic3r_api::GCodeGeneration::GCodeScriptContext toolchange = scripts.prepare(
        registered_script_type(orchestrator, "toolchange_gcode"));
    REQUIRE(toolchange.config().has("previous_extruder"));
    REQUIRE(toolchange.config().has("next_extruder"));
    toolchange.set("previous_extruder", int32_t(1));
    toolchange.set("next_extruder", int32_t(0));
    CHECK(toolchange.process(
              "M117 T{previous_extruder}>{next_extruder}", 0) ==
          "M117 T1>0");

    // prepare() discards per-call values rather than leaking the previous
    // toolchange context into the next script.
    const slic3r_api::GCodeGeneration::GCodeScriptContext fresh_toolchange = scripts.prepare(
        registered_script_type(orchestrator, "toolchange_gcode"));
    CHECK(fresh_toolchange.config().get_int("previous_extruder") == 0);
    CHECK_THROWS_AS(
        fresh_toolchange.set("previous_extruder", 1.5), std::invalid_argument);
    CHECK(fresh_toolchange.process("", 0).empty());

    const slic3r_api::GCodeGeneration::GCodeScriptContext color_change = scripts.prepare(
        registered_script_type(orchestrator, "color_change_gcode"));
    color_change.set("next_color", std::string("#12ab34"));
    CHECK(color_change.process("M117 {next_color}", 0) == "M117 #12ab34");

    // Names unknown to the specific-placeholder table remain useful: they see
    // the Print configuration and only the standard machine in/out options.
    const slic3r_api::GCodeGeneration::GCodeScriptContext generic = scripts.prepare(
        registered_script_type(orchestrator, "unknown_script"));
    CHECK(generic.process("M117 F{travel_speed}", 0) == "M117 F100");

    const slic3r_api::GCodeGeneration::GCodeScriptContext before_layer = scripts.prepare(
        registered_script_type(orchestrator, "before_layer_gcode"));
    const config_option_handle *used_filament =
        config_get(before_layer.config().handle(), "layer_used_filament");
    REQUIRE(used_filament != nullptr);
    CHECK(config_option_type_get(used_filament) == SLIC3R_CONFIG_OPTION_FLOATS);
    CHECK(config_option_size(used_filament) == 2);
    CHECK(before_layer.process("", 0).empty());

    // Parser globals deliberately survive between scripts handled by one
    // export, but a new host processor starts with an independent dictionary.
    CHECK(scripts.prepare(registered_script_type(orchestrator, "unknown_script"))
              .process("{global firmware_script_counter=7}", 0)
              .empty());
    CHECK(scripts.prepare(registered_script_type(orchestrator, "unknown_script"))
              .process("{firmware_script_counter}", 0) == "7");

    GCodeScriptProcessor independent_host(print, orchestrator);
    const slic3r_api::GCodeGeneration::GCodeScriptProcessorView independent_scripts(
        independent_host.c_processor());
    CHECK_THROWS(independent_scripts.prepare(registered_script_type(orchestrator, "unknown_script"))
                     .process("{firmware_script_counter}", 0));
}
TEST_CASE("Firmware scripts import validated machine state atomically",
          "[plugins][gcode][firmware][custom-gcode]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    configure_standard_firmware(print, 2);
    Orchestrator &orchestrator = Orchestrator::instance();
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    plan.groups.front().layers.emplace_back();
    plan.groups.front().layers.front().tool_groups.emplace_back();
    plan.groups.front().layers.front().tool_groups.front().extruder_id = 1;
    GCodeScriptProcessor host_processor(print, orchestrator);
    const slic3r_api::GCodeGeneration::GCodeScriptProcessorView scripts(
        host_processor.c_processor());
    ScriptStateFirmwareProbe firmware(scripts);
    const slic3r_api::Print print_view(
        reinterpret_cast<const print_handle *>(&print));
    CHECK(firmware.begin_print(print_view).empty());

    CHECK(firmware.process_script(
              GCODE_SCRIPT_TYPE_START_GCODE, "M104 S175", nullptr, uint16_t(1)) ==
          "M104 S175\n");
    REQUIRE(firmware.machine_extruder(1).heater().requested_temperature());
    CHECK(*firmware.machine_extruder(1).heater().requested_temperature() == 175);
    CHECK_FALSE(firmware.machine_extruder(0).heater().requested_temperature());

    // When the script itself leaves position untouched, the host derives the
    // new machine position from the successfully produced movement commands.
    CHECK(firmware.process_script(GCODE_SCRIPT_TYPE_EXTRUSION_CUSTOM, "G1 X12 Y34 Z5") == "G1 X12 Y34 Z5\n");
    REQUIRE(firmware.machine_gantry().position());
    CHECK(firmware.machine_gantry().position()->x == Approx(12.0));
    CHECK(firmware.machine_gantry().position()->y == Approx(34.0));
    CHECK(firmware.machine_gantry().position()->z == Approx(5.0));

    CHECK(
        firmware.process_script(GCODE_SCRIPT_TYPE_EXTRUSION_CUSTOM, "{position[0]=8}{position[1]=9}{position[2]=10}")
            .empty());
    REQUIRE(firmware.machine_gantry().position());
    CHECK(firmware.machine_gantry().position()->x == Approx(8.0));
    CHECK(firmware.machine_gantry().position()->y == Approx(9.0));
    CHECK(firmware.machine_gantry().position()->z == Approx(10.0));

    slic3r_api::GCodeGeneration::ExtrusionAxisState &axis =
        firmware.machine_extruder(0).extrusion_axis();
    CHECK(firmware.process_script(
              GCODE_SCRIPT_TYPE_EXTRUSION_CUSTOM,
              "M82\nG1 E5\nM83\nG1 E2\nM82\nG1 E8") ==
          "M82\nG1 E5\nM83\nG1 E2\nM82\nG1 E8\n");
    CHECK(axis.position() == Approx(8.0));
    CHECK(axis.used_filament() == Approx(8.0));
    CHECK_FALSE(axis.extrude(0.000004));
    REQUIRE(axis.extruded_dE_left() == Approx(0.000004));
    CHECK(firmware.process_script(GCODE_SCRIPT_TYPE_EXTRUSION_CUSTOM, "M117 unchanged").find("M117 unchanged") !=
          std::string::npos);
    CHECK(axis.extruded_dE_left() == Approx(0.000004));

    CHECK(firmware
              .process_script(GCODE_SCRIPT_TYPE_EXTRUSION_CUSTOM,
                              "{e_position[0]=4}{e_retracted[0]=1.5}{e_restart_extra[0]=0.2}")
              .empty());
    CHECK(axis.position() == Approx(4.0));

    CHECK(firmware.process_script(
              GCODE_SCRIPT_TYPE_EXTRUSION_CUSTOM,
              "G91\nG1 X1 Y2 Z3\nG90") ==
          "G91\nG1 X1 Y2 Z3\nG90\n");
    REQUIRE(firmware.machine_gantry().position());
    CHECK(firmware.machine_gantry().position()->x == Approx(9.0));
    CHECK(firmware.machine_gantry().position()->y == Approx(11.0));
    CHECK(firmware.machine_gantry().position()->z == Approx(13.0));

    CHECK(firmware.process_script(
              GCODE_SCRIPT_TYPE_EXTRUSION_CUSTOM,
              "G20\nG91\nG1 X1\nG90\nG21") ==
          "G20\nG91\nG1 X1\nG90\nG21\n");
    REQUIRE(firmware.machine_gantry().position());
    CHECK(firmware.machine_gantry().position()->x == Approx(34.4));

    CHECK(firmware.process_script(
              GCODE_SCRIPT_TYPE_EXTRUSION_CUSTOM,
              "T1\nM104 S205\nM140 S60\nM141 S35\nM106 S128") ==
          "T1\nM104 S205\nM140 S60\nM141 S35\nM106 S128\n");
    const slic3r_api::GCodeGeneration::DefaultExtruder &tool_one = firmware.machine_extruder(1);
    REQUIRE(tool_one.heater().requested_temperature());
    CHECK(*tool_one.heater().requested_temperature() == 205);
    REQUIRE(tool_one.fan().encoded_speed_percent());
    CHECK(*tool_one.fan().encoded_speed_percent() == Approx(128.0 * 100.0 / 255.0));
    REQUIRE(firmware.machine_printer().bed_heater().requested_temperature());
    CHECK(*firmware.machine_printer().bed_heater().requested_temperature() == 60);
    REQUIRE(firmware.machine_printer().chamber_heater().requested_temperature());
    CHECK(*firmware.machine_printer().chamber_heater().requested_temperature() == 35);

    CHECK(firmware.process_script(
              GCODE_SCRIPT_TYPE_EXTRUSION_CUSTOM,
              "M83\nG1 E2\nM82\nG92 E5\nG1 E6\nM107") ==
          "M83\nG1 E2\nM82\nG92 E5\nG1 E6\nM107\n");
    CHECK(firmware.machine_extruder(1).extrusion_axis().position() == Approx(6.0));
    CHECK(firmware.machine_extruder(1).extrusion_axis().used_filament() == Approx(3.0));
    REQUIRE(firmware.machine_extruder(1).fan().encoded_speed_percent());
    CHECK(*firmware.machine_extruder(1).fan().encoded_speed_percent() == Approx(0.0));

    CHECK(firmware.process_script(
              GCODE_SCRIPT_TYPE_EXTRUSION_CUSTOM,
              "{position[0]=50}G91\nG1 X1") ==
          "G91\nG1 X1\n");
    REQUIRE(firmware.machine_gantry().position());
    CHECK(firmware.machine_gantry().position()->x == Approx(50.0));
    CHECK(firmware.process_script(
              GCODE_SCRIPT_TYPE_EXTRUSION_CUSTOM,
              "M117 one newline\n\n") ==
          "M117 one newline\n");
    CHECK(axis.retracted() == Approx(1.5));
    CHECK(axis.restart_extra() == Approx(0.2));
    CHECK(axis.extruded_dE_left() == Approx(0.0));

    // Corrupting a prepared in/out vector simulates a hostile script or C
    // caller. Validation fails before any session state can be imported.
    const slic3r_api::GCodeGeneration::GCodeScriptContext invalid = scripts.prepare(
        GCODE_SCRIPT_TYPE_EXTRUSION_CUSTOM);
    DynamicConfig *prepared = dynamic_cast<DynamicConfig *>(
        ApiHost::to_config(invalid.config().handle()));
    REQUIRE(prepared != nullptr);
    prepared->set_key_value("position", new ConfigOptionFloats({1.0, 2.0}));
    CHECK_THROWS(invalid.process("", 0));
    REQUIRE(firmware.machine_gantry().position());
    CHECK(firmware.machine_gantry().position()->x == Approx(50.0));
    CHECK(axis.position() == Approx(4.0));

    ScriptStateFirmwareProbe processorless;
    CHECK_THROWS_AS(processorless.process_script(GCODE_SCRIPT_TYPE_EXTRUSION_CUSTOM, "M117 unavailable"),
                    std::invalid_argument);
    CHECK_THROWS_AS(
        firmware.process_script(GCODE_SCRIPT_TYPE_EXTRUSION_CUSTOM, "M117 invalid target", nullptr, uint16_t(2)),
        std::invalid_argument);
}
TEST_CASE("Derived firmware completes a script context immediately before parsing",
          "[plugins][gcode][firmware][custom-gcode]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    configure_standard_firmware(print);
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    plan.groups.front().layers.emplace_back();
    plan.groups.front().layers.front().print_z = scale_i(0.4);
    plan.groups.front().layers.front().tool_groups.emplace_back();
    plan.groups.front().layers.front().tool_groups.front().extruder_id = 0;

    GCodeScriptProcessor host_processor(print, Orchestrator::instance());
    const slic3r_api::GCodeGeneration::GCodeScriptProcessorView scripts(
        host_processor.c_processor());
    ScriptContextCompletingFirmwareProbe firmware(scripts);
    const slic3r_api::Print print_view(
        reinterpret_cast<const print_handle *>(&print));

    REQUIRE(firmware.begin_print(print_view).empty());
    CHECK(firmware.process_script(
              GCODE_SCRIPT_TYPE_START_GCODE, "M117 Z[max_layer_z]") ==
          "M117 Z12.5\n");
    CHECK(firmware.completion_count() == 1);
}
TEST_CASE("Toolchange scripts use the structural Config stored at insertion",
          "[plugins][gcode][firmware][custom-gcode][printing-plan]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    configure_standard_firmware(print, 2);
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    PrintingGroup &group = plan.groups.front();
    group.layers.emplace_back();
    PrintingLayerGroup &layer = group.layers.front();
    layer.print_z = scale_i(0.2);
    layer.tool_groups.emplace_back();
    layer.tool_groups.back().extruder_id = 0;
    layer.tool_groups.emplace_back();
    layer.tool_groups.back().extruder_id = 1;

    layer.tool_groups[0].extruder_id = 1;
    layer.tool_groups[1].extruder_id = 0;
    // The producer freezes only structural facts. The firmware will still add
    // position, E and retraction from the exact execution point.
    PluginStorage script_storage;
    storage_handle *script_storage_handle =
        reinterpret_cast<storage_handle *>(&script_storage);
    ExtrusionNop toolchange_script;
    slic3r_api::MutableExtrusionEntity toolchange_view(
        reinterpret_cast<extrusion_entity_handle *>(&toolchange_script));
    slic3r_api::StoredConfig toolchange_arguments(script_storage_handle);
    toolchange_arguments.get_or_add("previous_extruder", SLIC3R_CONFIG_OPTION_INT).set_int(1);
    toolchange_arguments.get_or_add("next_extruder", SLIC3R_CONFIG_OPTION_INT).set_int(0);
    toolchange_arguments.get_or_add("toolchange_z", SLIC3R_CONFIG_OPTION_FLOAT).set_float(0.2);
    toolchange_view.script_gcode(
        "; FINAL_TOOL P[previous_extruder] N[next_extruder]",
        GCODE_SCRIPT_TYPE_TOOLCHANGE_GCODE,
        toolchange_arguments,
        uint16_t(0));
    layer.tool_groups[1].events.append_before(toolchange_script);

    GCodeScriptProcessor host_processor(print, Orchestrator::instance());
    ScriptStateFirmwareProbe firmware(
        slic3r_api::GCodeGeneration::GCodeScriptProcessorView(
            host_processor.c_processor()),
        script_storage_handle);
    const slic3r_api::Print print_view(
        reinterpret_cast<const print_handle *>(&print));
    const slic3r_api::PrintingPlan plan_view = printing_plan_view(print);
    const slic3r_api::PrintingGroup group_view = plan_view.group(0);
    const slic3r_api::PrintingLayerGroup layer_view = group_view.layer_group(0);
    const slic3r_api::PrintingToolGroup first_tool = layer_view.tool_group(0);
    const slic3r_api::PrintingToolGroup second_tool = layer_view.tool_group(1);

    REQUIRE(firmware.begin_print(print_view).empty());
    REQUIRE(firmware.begin_group(group_view).empty());
    REQUIRE(firmware.begin_layer(layer_view).empty());
    CHECK(firmware.begin_tool_group(first_tool) == "T1\n");
    REQUIRE(firmware.end_tool_group().empty());
    REQUIRE(firmware.begin_tool_group(second_tool).empty());
    CHECK(firmware.write_event(second_tool.events().before()) ==
          "; FINAL_TOOL P1 N0\n");
}
TEST_CASE("G-code script types have stable built-in and runtime identities", "[plugins][gcode][script-type]") {
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Orchestrator &orchestrator = Orchestrator::instance();
    orchestrator_handle *handle = reinterpret_cast<orchestrator_handle *>(&orchestrator);

    CHECK(gcode_script_register_type(handle, "start_gcode") == GCODE_SCRIPT_TYPE_START_GCODE);
    CHECK(gcode_script_register_type(handle, "end_gcode") == GCODE_SCRIPT_TYPE_END_GCODE);
    CHECK(gcode_script_register_type(handle, "extrusion_custom_gcode_script") == GCODE_SCRIPT_TYPE_EXTRUSION_CUSTOM);
    CHECK(gcode_script_register_type(handle, "start_filament_gcode") == GCODE_SCRIPT_TYPE_START_FILAMENT_GCODE);
    CHECK(gcode_script_register_type(handle, "end_filament_gcode") == GCODE_SCRIPT_TYPE_END_FILAMENT_GCODE);
    CHECK(gcode_script_register_type(handle, "before_layer_gcode") == GCODE_SCRIPT_TYPE_BEFORE_LAYER_GCODE);
    CHECK(gcode_script_register_type(handle, "layer_gcode") == GCODE_SCRIPT_TYPE_LAYER_GCODE);
    CHECK(gcode_script_register_type(handle, "toolchange_gcode") == GCODE_SCRIPT_TYPE_TOOLCHANGE_GCODE);
    CHECK(gcode_script_register_type(handle, "between_objects_gcode") == GCODE_SCRIPT_TYPE_BETWEEN_OBJECTS_GCODE);
    CHECK(gcode_script_register_type(handle, "feature_gcode") == GCODE_SCRIPT_TYPE_FEATURE_GCODE);
    CHECK(std::string(gcode_script_type_name(handle, GCODE_SCRIPT_TYPE_START_GCODE)) == "start_gcode");

    const gcode_script_type first = gcode_script_register_type(handle, "test.script_type.first");
    const gcode_script_type repeated = gcode_script_register_type(handle, "test.script_type.first");
    const gcode_script_type second = gcode_script_register_type(handle, "test.script_type.second");
    REQUIRE(first >= GCODE_SCRIPT_TYPE_CUSTOM_BEGIN);
    CHECK(repeated == first);
    CHECK(second != first);
    CHECK(std::string(gcode_script_type_name(handle, first)) == "test.script_type.first");

    CHECK(gcode_script_register_type(handle, "") == GCODE_SCRIPT_TYPE_INVALID);
    CHECK(gcode_script_type_name(handle, GCODE_SCRIPT_TYPE_INVALID) == nullptr);
    CHECK(gcode_script_type_name(handle, GCODE_SCRIPT_TYPE_CUSTOM_BEGIN - 1) == nullptr);
}
TEST_CASE("G-code script type survives entity copies moves and diagnostics",
          "[plugins][gcode][script-type][diagnostic]") {
    const gcode_script_type script_type = GCODE_SCRIPT_TYPE_CUSTOM_BEGIN + 42;
    ExtrusionNop source(
        ExtrusionPropertyCustomGcodeText(ExtrusionPropertyCustomGcodeText::Code::SCRIPT, "M117 typed", script_type));

    std::unique_ptr<ExtrusionEntity> copied(source.clone());
    const ExtrusionPropertyCustomGcode *copied_property = copied->get_property<ExtrusionPropertyCustomGcode>();
    REQUIRE(copied_property != nullptr);
    CHECK(copied_property->script_type == script_type);

    ExtrusionNop moved(std::move(source));
    const ExtrusionPropertyCustomGcode *moved_property = moved.get_property<ExtrusionPropertyCustomGcode>();
    REQUIRE(moved_property != nullptr);
    CHECK(moved_property->script_type == script_type);

    ExtrusionPrinter printer(/*mult=*/1.0, /*trunc=*/0, /*json=*/true);
    printer.traverse(moved);
    CHECK(printer.str().find("\"script_type\":2147483690") != std::string::npos);
}
TEST_CASE("G-code script processing tool survives stored entity copies moves and diagnostics",
          "[plugins][gcode][script-type][diagnostic]")
{
    PluginStorage plugin_storage;
    storage_handle *storage = reinterpret_cast<storage_handle *>(&plugin_storage);

    slic3r_api::StoredConfig arguments(storage);
    arguments.get_or_add("filament_extruder_id", SLIC3R_CONFIG_OPTION_INT).set_int(2);
    arguments.get_or_add("producer_note", SLIC3R_CONFIG_OPTION_STRING).set_string("owned with event");
    slic3r_api::StoredExtrusionEntity source(storage);
    const slic3r_api::EPropertyCustomGcode &source_property =
        source.script_gcode(
            "M117 stored", GCODE_SCRIPT_TYPE_END_FILAMENT_GCODE, arguments, uint16_t(2));
    CHECK(source_property.processing_extruder_id == 2);
    REQUIRE(source_property.config_id != EXTRUSION_DATA_ID_INVALID);
    slic3r_api::StoredConfig source_config(storage);
    source_config.deserialize_all(source.stored_string(source_property.config_id));
    CHECK(source_config.keys().size() == 2);

    slic3r_api::StoredExtrusionEntity copied(storage, source.readonly());
    const slic3r_api::EPropertyCustomGcode *copied_property =
        copied.get(slic3r_api::EPropertyCustomGcode::key);
    REQUIRE(copied_property != nullptr);
    CHECK(copied_property->processing_extruder_id == 2);
    REQUIRE(copied_property->config_id != EXTRUSION_DATA_ID_INVALID);
    slic3r_api::StoredConfig copied_config(storage);
    copied_config.deserialize_all(copied.stored_string(copied_property->config_id));
    CHECK(copied_config.get("producer_note").get_string() == "owned with event");
    copied.custom_gcode("M117 raw", C_EXTRUSION_CUSTOM_GCODE_GCODE);
    copied_property = copied.get(slic3r_api::EPropertyCustomGcode::key);
    REQUIRE(copied_property != nullptr);
    CHECK(copied_property->processing_extruder_id == GCODE_SCRIPT_PROCESSING_EXTRUDER_INVALID);
    CHECK(copied_property->config_id == EXTRUSION_DATA_ID_INVALID);

    slic3r_api::StoredExtrusionEntity moved(std::move(source));
    const slic3r_api::EPropertyCustomGcode *moved_property =
        moved.get(slic3r_api::EPropertyCustomGcode::key);
    REQUIRE(moved_property != nullptr);
    CHECK(moved_property->processing_extruder_id == 2);
    REQUIRE(moved_property->config_id != EXTRUSION_DATA_ID_INVALID);
    slic3r_api::StoredConfig moved_config(storage);
    moved_config.deserialize_all(moved.stored_string(moved_property->config_id));
    CHECK(moved_config.get("filament_extruder_id").get_int() == 2);

    const ExtrusionEntity &host_entity =
        *reinterpret_cast<const ExtrusionEntity *>(moved.handle());
    ExtrusionPrinter printer(/*mult=*/1.0, /*trunc=*/0, /*json=*/true);
    printer.traverse(host_entity);
    CHECK(printer.str().find("\"processing_extruder_id\":2") != std::string::npos);
    CHECK(printer.str().find("\"config_key_count\":2") != std::string::npos);
}
TEST_CASE("G-code script Config snapshots replace and clear their owned data",
          "[plugins][gcode][firmware][custom-gcode][config]")
{
    PluginStorage plugin_storage;
    storage_handle *storage = reinterpret_cast<storage_handle *>(&plugin_storage);
    slic3r_api::StoredExtrusionEntity event(
        storage);
    slic3r_api::StoredConfig initial_config(storage);
    initial_config.get_or_add("layer_num", SLIC3R_CONFIG_OPTION_INT).set_int(7);
    const slic3r_api::EPropertyCustomGcode &property = event.script_gcode(
        "M117 [layer_num]", GCODE_SCRIPT_TYPE_LAYER_GCODE, initial_config);
    const extrusion_data_id initial_id = property.config_id;
    REQUIRE(initial_id != EXTRUSION_DATA_ID_INVALID);

    slic3r_api::StoredConfig replacement_config(storage);
    replacement_config.get_or_add("layer_num", SLIC3R_CONFIG_OPTION_INT).set_int(9);
    replacement_config.get_or_add("layer_z", SLIC3R_CONFIG_OPTION_FLOAT).set_float(0.4);
    event.script_gcode(
        "M117 [layer_num] [layer_z]", GCODE_SCRIPT_TYPE_LAYER_GCODE, replacement_config);

    // Publishing the replacement snapshot releases the buffer previously
    // owned by the same config_id field.
    const slic3r_api::EPropertyCustomGcode *replaced =
        event.get(slic3r_api::EPropertyCustomGcode::key);
    REQUIRE(replaced != nullptr);
    CHECK(replaced->config_id != EXTRUSION_DATA_ID_INVALID);
    CHECK(event.stored_data(initial_id) == nullptr);
    slic3r_api::StoredConfig decoded_replacement(storage);
    decoded_replacement.deserialize_all(event.stored_string(replaced->config_id));
    CHECK(decoded_replacement.get("layer_num").get_int() == 9);
    CHECK(decoded_replacement.get("layer_z").get_float() == Approx(0.4));

    slic3r_api::StoredConfig empty_config(storage);
    event.script_gcode("M117 empty", GCODE_SCRIPT_TYPE_LAYER_GCODE, empty_config);
    replaced = event.get(slic3r_api::EPropertyCustomGcode::key);
    REQUIRE(replaced != nullptr);
    REQUIRE(replaced->config_id != EXTRUSION_DATA_ID_INVALID);
    slic3r_api::StoredConfig decoded_empty(storage);
    decoded_empty.deserialize_all(event.stored_string(replaced->config_id));
    CHECK(decoded_empty.keys().empty());

    event.script_gcode("M117 plain", GCODE_SCRIPT_TYPE_LAYER_GCODE);
    replaced = event.get(slic3r_api::EPropertyCustomGcode::key);
    REQUIRE(replaced != nullptr);
    CHECK(replaced->config_id == EXTRUSION_DATA_ID_INVALID);
}
TEST_CASE("Firmware rejects invalid script Config snapshots and clears its scratch Config",
          "[plugins][gcode][firmware][custom-gcode][config]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    configure_standard_firmware(print);
    GCodeScriptProcessor host_processor(print, Orchestrator::instance());
    const slic3r_api::GCodeGeneration::GCodeScriptProcessorView scripts(
        host_processor.c_processor());
    PluginStorage plugin_storage;
    storage_handle *storage = reinterpret_cast<storage_handle *>(&plugin_storage);
    ScriptStateFirmwareProbe firmware(scripts, storage);
    const slic3r_api::Print print_view(
        reinterpret_cast<const print_handle *>(&print));
    REQUIRE(firmware.begin_print(print_view).empty());

    slic3r_api::StoredExtrusionEntity invalid_id_event(storage);
    slic3r_api::EPropertyCustomGcode &invalid_id_property = invalid_id_event.script_gcode(
        "M117 invalid", GCODE_SCRIPT_TYPE_EXTRUSION_CUSTOM, uint16_t(0));
    invalid_id_property.config_id = extrusion_data_id(0x7fffffffu);
    CHECK_THROWS_AS(firmware.write_event(invalid_id_event.readonly()), std::invalid_argument);

    slic3r_api::StoredExtrusionEntity corrupt_event(storage);
    slic3r_api::EPropertyCustomGcode &corrupt_property = corrupt_event.script_gcode(
        "M117 corrupt", GCODE_SCRIPT_TYPE_EXTRUSION_CUSTOM, uint16_t(0));
    REQUIRE(corrupt_event.store_property_string(
                slic3r_api::EPropertyCustomGcode::property_type,
                &corrupt_property.config_id,
                "SCFG9\n0\n") != EXTRUSION_DATA_ID_INVALID);
    CHECK_THROWS(firmware.write_event(corrupt_event.readonly()));

    slic3r_api::StoredConfig first_config(storage);
    first_config.get_or_add("first_only", SLIC3R_CONFIG_OPTION_INT).set_int(1);
    slic3r_api::StoredExtrusionEntity first_event(storage);
    first_event.script_gcode(
        "M117 [first_only]", GCODE_SCRIPT_TYPE_EXTRUSION_CUSTOM, first_config, uint16_t(0));
    CHECK(firmware.write_event(first_event.readonly()) == "M117 1\n");

    slic3r_api::StoredConfig second_config(storage);
    second_config.get_or_add("second_only", SLIC3R_CONFIG_OPTION_INT).set_int(2);
    slic3r_api::StoredExtrusionEntity second_event(storage);
    second_event.script_gcode(
        "M117 [first_only]", GCODE_SCRIPT_TYPE_EXTRUSION_CUSTOM, second_config, uint16_t(0));
    CHECK_THROWS(firmware.write_event(second_event.readonly()));
}
TEST_CASE("ExtrusionPrinter names every custom G-code kind",
          "[plugins][gcode][custom-gcode][diagnostic]")
{
    const auto printed_kind = [](ExtrusionPropertyCustomGcodeText::Code code) {
        ExtrusionNop entity(ExtrusionPropertyCustomGcodeText(code, "test"));
        ExtrusionPrinter printer(/*mult=*/1.0, /*trunc=*/0, /*json=*/true);
        printer.traverse(entity);
        return printer.str();
    };

    CHECK(printed_kind(ExtrusionPropertyCustomGcodeText::Code::GCODE).find(
              "\"kind\":\"gcode\"") != std::string::npos);
    CHECK(printed_kind(ExtrusionPropertyCustomGcodeText::Code::COMMENT).find(
              "\"kind\":\"comment\"") != std::string::npos);
    CHECK(printed_kind(ExtrusionPropertyCustomGcodeText::Code::SCRIPT).find(
              "\"kind\":\"script\"") != std::string::npos);
    CHECK(printed_kind(static_cast<ExtrusionPropertyCustomGcodeText::Code>(99)).find(
              "\"kind\":\"unknown\"") != std::string::npos);
}
TEST_CASE("Legacy custom scripts substitute and update machine state",
          "[plugins][gcode][legacy][custom-gcode]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "step_gcode_plugin", "gcode.legacy" },
        { "start_gcode", "" },
        { "end_gcode", "" },
        { "gcode_comments", "0" }
    });

    Print print;
    Model model;
    Test::init_print({Test::TestMesh::cube_20x20x20}, print, model, config);
    print.process();

    GCodeGenerator generator;
    const boost::filesystem::path output_path = temporary_gcode_path();
    remove_output_pair(output_path);
    const std::string output_path_string = output_path.string();
    generator.do_export(&print, output_path_string.c_str());
    remove_output_pair(output_path);

    // This is the same parser entry point and diagnostic name used by the
    // SCRIPT branch of GCodeGenerator::apply_property().
    const std::string move = generator.placeholder_parser_process(
        "extrusion_custom_gcode_script", "G1 X123 Y45", uint16_t(-1));
    CHECK(move.find("G1 X123 Y45") != std::string::npos);
    CHECK(is_approx(generator.writer().get_position().x(), 123.));
    CHECK(is_approx(generator.writer().get_position().y(), 45.));

    const std::string state = generator.placeholder_parser_process(
        "extrusion_custom_gcode_script",
        "M117 X{current_position[0]} Y{current_position[1]}", uint16_t(-1));
    CHECK(state.find("M117 X123 Y45") != std::string::npos);
    CHECK(state.find("{current_position") == std::string::npos);
}
