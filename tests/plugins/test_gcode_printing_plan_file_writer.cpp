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

#include "libslic3r/Api/host/ApiHostUtils.hpp"
#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/host/Plugin.hpp"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_gcode.h"
#include "libslic3r/Api/plugin/cpp/gcode/DefaultGCodeFirmwareSession.hpp"
#include "libslic3r/Api/plugin/cpp/gcode/GCodeFirmwareViews.hpp"
#include "libslic3r/Api/plugin/cpp/gcode/MachineEnvelope.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Geometry/ArcWelder.hpp"
#include "libslic3r/Plugins/GCode/Firmware/BuiltinGCodeFirmwares.hpp"
#include "libslic3r/Plugins/GCode/Firmware/KlipperGCodeFirmware.hpp"
#include "libslic3r/Plugins/GCode/Firmware/Marlin1GCodeFirmware.hpp"
#include "libslic3r/Plugins/GCode/Firmware/Marlin2GCodeFirmware.hpp"
#include "libslic3r/Plugins/GCode/Firmware/PrusaGCodeFirmware.hpp"
#include "libslic3r/Plugins/GCode/Firmware/RepRapGCodeFirmware.hpp"
#include "libslic3r/Plugins/GCode/Firmware/SprinterGCodeFirmware.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Printing/PrintingPlan.hpp"
#include "libslic3r/Steps/StepGenerateGcode.hpp"
#include "libslic3r/Steps/StepPipeline.hpp"

/*
PrintingPlan G-code firmware tests
==================================

The production default firmware owns machine state and emits Marlin commands.
A local recording session still supplies explicit markers for tests which need
to observe only the file-writer boundary order. This separates the two
contracts: the provider serializes one plan element, while
PrintingPlanFileWriter preserves order and publishes the returned chunks.
*/

namespace {
using namespace Slic3r;
using namespace Slic3r::Printing;

struct FirmwareRecorder
{
    std::vector<std::string> calls;
    uint32_t destruction_count = 0;
    bool throw_on_extrusion = false;
    std::vector<bool> roots;
    std::vector<bool> region_islands;
    std::vector<raw_extrusion_role> roles;
    std::vector<uint16_t> object_instance_indices;
};

class RecordingFirmwareSession final : public slic3r_api::GCodeFirmwareSession
{
public:
    explicit RecordingFirmwareSession(FirmwareRecorder &recorder) : m_recorder(recorder) {}
    ~RecordingFirmwareSession() override { ++m_recorder.destruction_count; }

    std::string begin_print(const slic3r_api::Print &) override
    {
        m_recorder.calls.emplace_back("begin_print");
        return "begin_print\n";
    }

    std::string begin_group(const slic3r_api::PrintingGroup &) override
    {
        m_recorder.calls.emplace_back("begin_group");
        return "begin_group\n";
    }

    std::string begin_layer(const slic3r_api::PrintingLayerGroup &) override
    {
        m_recorder.calls.emplace_back("begin_layer");
        return "begin_layer\n";
    }

    std::string begin_tool_group(const slic3r_api::PrintingToolGroup &) override
    {
        m_recorder.calls.emplace_back("begin_tool_group");
        return "begin_tool_group\n";
    }

    std::string write_extrusion(const slic3r_api::PrintingExtrusion &extrusion) override
    {
        if (m_recorder.throw_on_extrusion)
            throw std::runtime_error("test firmware extrusion failure");

        m_recorder.calls.emplace_back("write_extrusion");
        m_recorder.roots.push_back(extrusion.root().valid());
        m_recorder.region_islands.push_back(extrusion.region_island().valid());
        m_recorder.roles.push_back(extrusion.role());
        m_recorder.object_instance_indices.push_back(extrusion.object_instance_idx());
        return "write_extrusion\n";
    }

    std::string end_tool_group() override
    {
        m_recorder.calls.emplace_back("end_tool_group");
        return "end_tool_group\n";
    }

    std::string end_layer() override
    {
        m_recorder.calls.emplace_back("end_layer");
        return "end_layer\n";
    }

    std::string end_group() override
    {
        m_recorder.calls.emplace_back("end_group");
        return "end_group\n";
    }

    std::string end_print() override
    {
        m_recorder.calls.emplace_back("end_print");
        return "end_print\n";
    }

private:
    FirmwareRecorder &m_recorder;
};

class FirmwareInstanceOwner
{
public:
    explicit FirmwareInstanceOwner(raw_gcode_firmware_instance instance) : m_instance(instance) {}

    ~FirmwareInstanceOwner()
    {
        if (m_instance.session != nullptr && m_instance.vtable != nullptr &&
            m_instance.vtable->destroy != nullptr)
            m_instance.vtable->destroy(m_instance.session);
    }

    FirmwareInstanceOwner(const FirmwareInstanceOwner &) = delete;
    FirmwareInstanceOwner &operator=(const FirmwareInstanceOwner &) = delete;

    raw_gcode_firmware_instance &instance() { return m_instance; }

private:
    raw_gcode_firmware_instance m_instance = {};
};

class CustomFanFirmwareSession final :
    public slic3r_api::GCodeGeneration::DefaultGCodeFirmwareSession
{
protected:
    std::string encode_fan(uint16_t tool_id, double speed_percent) const override
    {
        return "CUSTOM_FAN T" + std::to_string(tool_id) +
               " S" + std::to_string(int32_t(std::lround(speed_percent))) + "\n";
    }
};

// Exposes the parent begin-print orchestration without adding any dialect
// behavior beyond a visible marker for the machine-envelope hook.
class MachineEnvelopeFirmwareSession final :
    public slic3r_api::GCodeGeneration::DefaultGCodeFirmwareSession
{
public:
    uint32_t encode_count() const { return m_encode_count; }
    bool initialized_during_encoding() const { return m_initialized_during_encoding; }
    const std::optional<slic3r_api::GCodeGeneration::MachineEnvelope> &encoded_envelope() const
    {
        return m_encoded_envelope;
    }
    bool formatter_initialized() const
    {
        (void)gcode_formatter();
        return true;
    }

protected:
    std::string encode_machine_envelope(
        const slic3r_api::GCodeGeneration::MachineEnvelope &envelope) const override
    {
        // Accessing the formatter proves that setup() completed before the
        // virtual dialect hook was entered.
        (void)gcode_formatter();
        m_initialized_during_encoding = true;
        m_encoded_envelope = envelope;
        ++m_encode_count;
        return "machine-envelope\n";
    }

private:
    mutable uint32_t m_encode_count = 0;
    mutable bool m_initialized_during_encoding = false;
    mutable std::optional<slic3r_api::GCodeGeneration::MachineEnvelope> m_encoded_envelope;
};

class PreviewInspectingFirmwareSession final :
    public slic3r_api::GCodeGeneration::DefaultGCodeFirmwareSession
{
public:
    std::string end_print() override
    {
        return printer().preview_enabled() ? "PREVIEW_ON\n" : "PREVIEW_OFF\n";
    }
};

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

class TraversalHookFirmwareSession final :
    public slic3r_api::GCodeGeneration::DefaultGCodeFirmwareSession
{
public:
    explicit TraversalHookFirmwareSession(std::vector<std::string> &calls) : m_calls(calls) {}

protected:
    std::string enter_extrusion_node(const slic3r_api::ExtrusionEntity &) override
    {
        m_calls.emplace_back("enter");
        return {};
    }

    std::string visit_extrusion_leaf(const slic3r_api::ExtrusionEntity &) override
    {
        m_calls.emplace_back("leaf");
        return {};
    }

    std::string leave_extrusion_node(const slic3r_api::ExtrusionEntity &) override
    {
        m_calls.emplace_back("leave");
        return {};
    }

private:
    std::vector<std::string> &m_calls;
};

boost::filesystem::path temporary_gcode_path()
{
    return boost::filesystem::temp_directory_path() /
           boost::filesystem::unique_path("slic3r-printing-plan-writer-%%%%-%%%%.gcode");
}

std::string read_text_file(const boost::filesystem::path &path)
{
    std::ifstream file(path.string(), std::ios::in | std::ios::binary);
    REQUIRE(file.good());
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void write_text_file(const boost::filesystem::path &path, const std::string &contents)
{
    // Error-path tests begin with a valid published artifact so they can prove
    // that a failed staging run never replaces the previous file.
    std::ofstream file(path.string(), std::ios::out | std::ios::binary | std::ios::trunc);
    REQUIRE(file.good());
    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    file.close();
    REQUIRE(file.good());
}

void remove_output_pair(const boost::filesystem::path &path)
{
    boost::system::error_code ignored;
    boost::filesystem::remove(path, ignored);
    boost::filesystem::remove(path.string() + ".tmp", ignored);
}

void append_empty_extrusion(PrintingToolGroup &tool_group,
                            ExtrusionRole role = ExtrusionRole::None,
                            uint16_t object_instance_idx = 0,
                            const LayerRegionIsland *region_island = nullptr)
{
    PrintingExtrusion extrusion;
    extrusion.root = std::make_unique<ExtrusionEntity>(true);
    extrusion.sregion_island_role = role;
    extrusion.object_instance_idx = object_instance_idx;
    extrusion.region_island = region_island;
    tool_group.extrusions.push_back(std::move(extrusion));
}

std::unique_ptr<ExtrusionPath> make_firmware_path(const ArcPolyline &polyline,
                                                  float speed,
                                                  float acceleration,
                                                  float pressure_advance = -1.f,
                                                  float fan_speed = -1.f,
                                                  float temperature = -1.f)
{
    const ExtrusionAttributes attributes(
        ExtrusionRole::Perimeter,
        ExtrusionFlow(0.08, 0.4f, 0.2f));
    std::unique_ptr<ExtrusionPath> path(
        new ExtrusionPath(polyline, attributes, nullptr, true));
    path->add_property(ExtrusionPropertySpeed(
        speed, acceleration, pressure_advance, fan_speed, temperature));
    return path;
}

std::unique_ptr<ExtrusionPath> make_firmware_travel(const ArcPolyline &polyline,
                                                    float speed,
                                                    float acceleration)
{
    // Travel leaves still carry attributes so the firmware can classify the
    // movement, but their flow is intentionally zero because they emit no E.
    std::unique_ptr<ExtrusionPath> path(
        new ExtrusionPath(polyline, ExtrusionAttributes(ExtrusionRole::Travel), nullptr, true));
    path->add_property(ExtrusionPropertySpeed(speed, acceleration));
    return path;
}

void append_path_extrusion(PrintingToolGroup &tool_group,
                           std::unique_ptr<ExtrusionEntity> root)
{
    PrintingExtrusion extrusion;
    extrusion.root = std::move(root);
    extrusion.sregion_island_role = ExtrusionRole::Perimeter;
    tool_group.extrusions.push_back(std::move(extrusion));
}

void append_multi_tool_process_sequence(PrintingLayerGroup &layer)
{
    const uint16_t tool_ids[] = {0, 1, 0};
    const float temperatures[] = {200.f, 210.f, 200.f};
    const float fan_speeds[] = {20.f, 30.f, 20.f};
    const float pressure_advances[] = {0.01f, 0.02f, 0.01f};
    for (uint32_t idx = 0; idx < 3; ++idx) {
        layer.tool_groups.emplace_back();
        PrintingToolGroup &tool_group = layer.tool_groups.back();
        tool_group.extruder_id = tool_ids[idx];
        append_path_extrusion(
            tool_group,
            make_firmware_path(
                ArcPolyline(Points{
                    Point(scale_i(double(idx)), scale_i(0.0)),
                    Point(scale_i(double(idx + 1)), scale_i(0.0))
                }),
                15.f,
                400.f,
                pressure_advances[idx],
                fan_speeds[idx],
                temperatures[idx]));
    }
}

void configure_standard_firmware(Print &print, uint16_t extruder_count = 1)
{
    PrintConfig &print_config = const_cast<PrintConfig &>(print.config());
    print_config.travel_speed.value = 100.0;
    print_config.gcode_precision_xyz.value = 3;
    print_config.gcode_precision_e.value = 5;
    if (extruder_count == 2) {
        print_config.nozzle_diameter.set(std::vector<double>{0.4, 0.4});
        print_config.filament_diameter.set(std::vector<double>{1.75, 1.75});
        print_config.extrusion_multiplier.set(std::vector<double>{1.0, 1.0});
        print_config.retract_speed.set(std::vector<double>{40.0, 40.0});
        print_config.deretract_speed.set(std::vector<double>{30.0, 30.0});
    }
}

void configure_machine_envelope(Print &print)
{
    // Distinct values make unit conversions and axis ordering visible in the
    // generated preamble instead of letting equal defaults hide mistakes.
    PrintConfig &config = const_cast<PrintConfig &>(print.config());
    config.machine_limits_usage.value = MachineLimitsUsage::EmitToGCode;
    config.machine_max_acceleration_x.set(std::vector<double>{1000.0});
    config.machine_max_acceleration_y.set(std::vector<double>{1100.0});
    config.machine_max_acceleration_z.set(std::vector<double>{120.0});
    config.machine_max_acceleration_e.set(std::vector<double>{1300.0});
    config.machine_max_feedrate_x.set(std::vector<double>{200.0});
    config.machine_max_feedrate_y.set(std::vector<double>{210.0});
    config.machine_max_feedrate_z.set(std::vector<double>{12.0});
    config.machine_max_feedrate_e.set(std::vector<double>{25.0});
    config.machine_max_acceleration_extruding.set(std::vector<double>{500.0});
    config.machine_max_acceleration_retracting.set(std::vector<double>{800.0});
    config.machine_max_acceleration_travel.set(std::vector<double>{900.0});
    config.machine_max_jerk_x.set(std::vector<double>{8.0});
    config.machine_max_jerk_y.set(std::vector<double>{9.0});
    config.machine_max_jerk_z.set(std::vector<double>{0.4});
    config.machine_max_jerk_e.set(std::vector<double>{2.5});
    config.machine_min_extruding_rate.set(std::vector<double>{3.0});
    config.machine_min_travel_rate.set(std::vector<double>{4.0});
}

size_t count_occurrences(const std::string &text, const std::string &needle)
{
    size_t count = 0;
    size_t offset = 0;
    while ((offset = text.find(needle, offset)) != std::string::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

size_t count_command_lines(const std::string &text, const std::string &command)
{
    // Firmware commands are line-oriented. Comparing complete lines avoids
    // counting a tool suffix such as "T0" in an unrelated M104 command.
    size_t count = 0;
    size_t begin = 0;
    while (begin < text.size()) {
        const size_t end = text.find('\n', begin);
        const size_t length = end == std::string::npos ? text.size() - begin : end - begin;
        if (text.compare(begin, length, command) == 0)
            ++count;
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return count;
}

std::unique_ptr<ExtrusionEntity> special_command_entity(
    ExtrusionPropertySpecialCommand::Code code,
    double value = 0.0)
{
    return std::unique_ptr<ExtrusionEntity>(
        new ExtrusionNop(ExtrusionPropertySpecialCommand(code, value)));
}

void select_printing_plan_writer(
    Print &print,
    const std::string &firmware_id = "gcode.firmware.marlin2")
{
    // Legacy remains the global default. Tests which exercise the new artifact
    // writer select it through the same printer option used by normal presets.
    DynamicPrintConfig &config = const_cast<DynamicPrintConfig &>(print.full_print_config());
    config.set_deserialize("step_gcode_plugin", "gcode.printing_plan_file_writer");
    config.set_deserialize("gcode_firmware_plugin", firmware_id);
}

std::string export_with_firmware(Print &print, const std::string &firmware_id)
{
    // Exercise the provider, C ABI session and file writer together. Keeping
    // this path shared prevents dialect tests from accidentally bypassing the
    // same selection mechanism used by printer presets.
    select_printing_plan_writer(print, firmware_id);
    const boost::filesystem::path output_path = temporary_gcode_path();
    remove_output_pair(output_path);
    Orchestrator &orchestrator = Orchestrator::instance();
    orchestrator.reset_plugin_cancel();
    try {
        Steps::StepGenerateGcode::run_step(
            orchestrator, print, output_path.string());
        const std::string output = read_text_file(output_path);
        remove_output_pair(output_path);
        orchestrator.reset_plugin_cancel();
        return output;
    } catch (...) {
        remove_output_pair(output_path);
        orchestrator.reset_plugin_cancel();
        throw;
    }
}

void run_file_writer_with_firmware(Print &print,
                                   const boost::filesystem::path &output_path,
                                   const raw_gcode_firmware_instance &firmware)
{
    Orchestrator &orchestrator = Orchestrator::instance();
    orchestrator.reset_plugin_cancel();
    Plugin *writer = orchestrator.get_plugin("gcode.printing_plan_file_writer");
    REQUIRE(writer != nullptr);

    PrintingPlan &plan = print.mutable_printing_plan();
    run_ctx_generate_gcode payload = {};
    payload.print = reinterpret_cast<const print_handle *>(&print);
    payload.plan = reinterpret_cast<printing_plan_handle *>(&plan);
    payload.firmware = &firmware;

    // Keep the path storage alive while the plugin consumes its borrowed C
    // string; assigning a temporary path string would leave a dangling pointer.
    const std::string output_path_string = output_path.string();
    payload.output_path = output_path_string.c_str();
    plugin_host_context host_context = orchestrator.prepare_plugin_host_context(STEP_GCODE, writer, &print);
    plugin_run_context run_context = orchestrator.prepare_plugin_run_context(STEP_GCODE, writer, &host_context);
    run_context.data = &payload;

    writer->setup(run_context, 1);
    writer->setup_run(run_context);
    writer->run(run_context);
    if (run_context.is_cancelled != nullptr && run_context.is_cancelled(run_context.host_context)) {
        orchestrator.reset_plugin_cancel();
        throw RuntimeError("Direct PrintingPlan writer test failed.");
    }
}

} // namespace

TEST_CASE("STEP_GCODE legacy selector is the default and owns the printer UI slot", "[plugins][gcode]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Orchestrator &orchestrator = Orchestrator::instance();
    Print print;

    const std::vector<Plugin *> gcode_plugins = orchestrator.get_active_plugins_for_step(STEP_GCODE);
    REQUIRE(gcode_plugins.size() >= 2);
    CHECK(gcode_plugins.front()->get_id() == "gcode.legacy");

    Plugin *selected = Steps::selected_or_active_plugin_for_step(
        orchestrator, STEP_GCODE, &print.full_print_config());
    REQUIRE(selected != nullptr);
    CHECK(selected->get_id() == "gcode.legacy");
    const ConfigOptionDef *definition = PrintConfigDef::instance().get("step_gcode_plugin");
    REQUIRE(definition != nullptr);
    CHECK(definition->option_preset_type == RAW_PRESET_TYPE_FFF_PRINTER);
    CHECK(PrintConfigDef::instance().option_keys(RAW_PRESET_TYPE_FFF_PRINTER).count(
              "step_gcode_plugin") == 1);

    const std::vector<Orchestrator::PluginUiFragment> printer_fragments =
        orchestrator.ui_fragments_for_file("printer_fff.ui");
    bool found_printer_fragment = false;
    for (const Orchestrator::PluginUiFragment &fragment : printer_fragments) {
        if (fragment.fragment_id != "step_gcode_plugin")
            continue;
        found_printer_fragment = true;
        CHECK(fragment.content.find("setting:insert$beforesetting$gcode_flavor:step_gcode_plugin") !=
              std::string::npos);
    }
    CHECK(found_printer_fragment);

    // Merge the insertion rule against a minimal printer page to verify the
    // user-visible order, not only the fragment registration.
    const std::string merged_printer_layout = orchestrator.merged_ui_layout(
        "printer_fff.ui",
        "page:General:printer\n"
        "group:Firmware\n"
        "\tsetting:gcode_flavor\n");
    const size_t gcode_selector_pos = merged_printer_layout.find("setting:step_gcode_plugin");
    const size_t flavor_pos = merged_printer_layout.find("setting:gcode_flavor");
    REQUIRE(gcode_selector_pos != std::string::npos);
    REQUIRE(flavor_pos != std::string::npos);
    CHECK(gcode_selector_pos < flavor_pos);

    const std::vector<Orchestrator::PluginUiFragment> print_fragments =
        orchestrator.ui_fragments_for_file("print.ui");
    for (const Orchestrator::PluginUiFragment &fragment : print_fragments)
        CHECK(fragment.fragment_id != "step_gcode_plugin");
}

TEST_CASE("STEP_GCODE legacy selector is not a direct plugin writer", "[plugins][gcode]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Orchestrator &orchestrator = Orchestrator::instance();
    orchestrator.reset_plugin_cancel();
    Print print;
    print.mutable_printing_plan();
    const boost::filesystem::path output_path = temporary_gcode_path();
    remove_output_pair(output_path);

    // The orchestrator owns the legacy export path. Running the STEP_GCODE
    // plugin bridge directly must not turn that marker plugin into a writer.
    CHECK_THROWS_AS(Steps::StepGenerateGcode::run_step(
                        orchestrator, print, output_path.string()), RuntimeError);
    orchestrator.reset_plugin_cancel();
    CHECK_FALSE(boost::filesystem::exists(output_path));
    CHECK_FALSE(boost::filesystem::exists(output_path.string() + ".tmp"));
}

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

TEST_CASE("G-code formatter builds checked command parameters",
          "[plugins][gcode][formatter]")
{
    SECTION("integer and fixed parameters preserve their requested representation") {
        slic3r_api::GCodeGeneration::GCodeFormatter formatter(3, 5);
        formatter.emit_string("M204");
        formatter.emit_integer_parameter('P', 499.5);
        formatter.emit_fixed_parameter('T', 8.0, 2);
        formatter.emit_comment(true, "test command");
        CHECK(formatter.string() == "M204 P500 T8.00 ; test command\n");
    }

    SECTION("invalid values are rejected before encoding") {
        slic3r_api::GCodeGeneration::GCodeFormatter formatter(3, 5);
        CHECK_THROWS_AS(formatter.emit_integer_parameter('X', -1.0), std::invalid_argument);
        CHECK_THROWS_AS(
            formatter.emit_integer_parameter('X', std::numeric_limits<double>::quiet_NaN()),
            std::invalid_argument);
        CHECK_THROWS_AS(
            formatter.emit_fixed_parameter('X', std::numeric_limits<double>::infinity(), 2),
            std::invalid_argument);
        CHECK_THROWS_AS(
            formatter.emit_integer_parameter('X', std::numeric_limits<double>::max()),
            std::invalid_argument);
        CHECK_THROWS_AS(formatter.emit_fixed_parameter('X', 1.0, 10), std::invalid_argument);
    }
}

TEST_CASE("Default firmware owns machine envelope initialization",
          "[plugins][gcode][firmware][machine-envelope]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    SECTION("disabled limits initialize the session without calling the dialect hook") {
        Print print;
        configure_standard_firmware(print);
        const slic3r_api::Print print_view(
            reinterpret_cast<const print_handle *>(&print));
        MachineEnvelopeFirmwareSession session;
        CHECK(session.begin_print(print_view).empty());
        CHECK(session.formatter_initialized());
        CHECK(session.encode_count() == 0);
    }

    SECTION("enabled limits reach the dialect hook after setup") {
        Print print;
        configure_standard_firmware(print);
        configure_machine_envelope(print);
        const slic3r_api::Print print_view(
            reinterpret_cast<const print_handle *>(&print));
        MachineEnvelopeFirmwareSession session;
        CHECK(session.begin_print(print_view) == "machine-envelope\n");
        CHECK(session.encode_count() == 1);
        CHECK(session.initialized_during_encoding());
        REQUIRE(session.encoded_envelope());
        CHECK(session.encoded_envelope()->max_acceleration_x == 1000.0);

        // The neutral base remains directly usable and deliberately has no
        // firmware-specific envelope syntax.
        slic3r_api::GCodeGeneration::DefaultGCodeFirmwareSession neutral_session;
        CHECK(neutral_session.begin_print(print_view).empty());
    }

    SECTION("invalid limits fail before entering the dialect hook") {
        Print print;
        configure_standard_firmware(print);
        configure_machine_envelope(print);
        PrintConfig &config = const_cast<PrintConfig &>(print.config());
        config.machine_max_feedrate_x.set(std::vector<double>{-1.0});
        const slic3r_api::Print print_view(
            reinterpret_cast<const print_handle *>(&print));
        MachineEnvelopeFirmwareSession session;
        CHECK_THROWS_AS(session.begin_print(print_view), std::invalid_argument);
        CHECK(session.formatter_initialized());
        CHECK(session.encode_count() == 0);
    }
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

TEST_CASE("Machine envelope reads and validates neutral printer limits",
          "[plugins][gcode][firmware][dialects][machine-envelope]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    // Presets may retain limits for estimates without asking firmware sessions
    // to publish those limits in the generated file.
    Print disabled_print;
    configure_standard_firmware(disabled_print);
    const slic3r_api::Print disabled_view(
        reinterpret_cast<const print_handle *>(&disabled_print));
    CHECK_FALSE(slic3r_api::GCodeGeneration::configured_machine_envelope(
        disabled_view.config()));

    // The public reader copies printer data without applying any dialect unit
    // conversion, leaving that decision to the selected firmware session.
    Print enabled_print;
    configure_standard_firmware(enabled_print);
    configure_machine_envelope(enabled_print);
    const slic3r_api::Print enabled_view(
        reinterpret_cast<const print_handle *>(&enabled_print));
    const std::optional<slic3r_api::GCodeGeneration::MachineEnvelope> envelope =
        slic3r_api::GCodeGeneration::configured_machine_envelope(enabled_view.config());
    REQUIRE(envelope);
    CHECK(envelope->max_acceleration_x == 1000.0);
    CHECK(envelope->max_acceleration_y == 1100.0);
    CHECK(envelope->max_acceleration_z == 120.0);
    CHECK(envelope->max_acceleration_e == 1300.0);
    CHECK(envelope->max_feedrate_x == 200.0);
    CHECK(envelope->max_feedrate_y == 210.0);
    CHECK(envelope->max_feedrate_z == 12.0);
    CHECK(envelope->max_feedrate_e == 25.0);
    CHECK(envelope->max_print_acceleration == 500.0);
    CHECK(envelope->max_retract_acceleration == 800.0);
    CHECK(envelope->max_travel_acceleration == 900.0);
    CHECK(envelope->max_jerk_x == 8.0);
    CHECK(envelope->max_jerk_y == 9.0);
    CHECK(envelope->max_jerk_z == 0.4);
    CHECK(envelope->max_jerk_e == 2.5);
    CHECK(envelope->min_extruding_feedrate == 3.0);
    CHECK(envelope->min_travel_feedrate == 4.0);

    SECTION("negative limits are rejected") {
        Print invalid_print;
        configure_standard_firmware(invalid_print);
        configure_machine_envelope(invalid_print);
        PrintConfig &config = const_cast<PrintConfig &>(invalid_print.config());
        config.machine_max_feedrate_x.set(std::vector<double>{-1.0});
        const slic3r_api::Print invalid_view(
            reinterpret_cast<const print_handle *>(&invalid_print));
        CHECK_THROWS_AS(
            slic3r_api::GCodeGeneration::configured_machine_envelope(invalid_view.config()),
            std::invalid_argument);
    }

    SECTION("non-finite limits are rejected") {
        Print invalid_print;
        configure_standard_firmware(invalid_print);
        configure_machine_envelope(invalid_print);
        PrintConfig &config = const_cast<PrintConfig &>(invalid_print.config());
        config.machine_max_feedrate_x.set(
            std::vector<double>{std::numeric_limits<double>::quiet_NaN()});
        const slic3r_api::Print invalid_view(
            reinterpret_cast<const print_handle *>(&invalid_print));
        CHECK_THROWS_AS(
            slic3r_api::GCodeGeneration::configured_machine_envelope(invalid_view.config()),
            std::invalid_argument);
    }
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

TEST_CASE("Firmware C++ adapter validates tables and destroys its session once",
          "[plugins][gcode][firmware]")
{
    FirmwareRecorder recorder;
    {
        FirmwareInstanceOwner owner(slic3r_api::make_gcode_firmware_instance(
            std::unique_ptr<slic3r_api::GCodeFirmwareSession>(new RecordingFirmwareSession(recorder))));
        CHECK_NOTHROW(slic3r_api::GCodeFirmwareView(&owner.instance()));

        raw_gcode_firmware_instance truncated_instance = owner.instance();
        truncated_instance.struct_size = 0;
        CHECK_THROWS_AS(slic3r_api::GCodeFirmwareView(&truncated_instance), std::invalid_argument);

        raw_gcode_firmware_vtable truncated = *owner.instance().vtable;
        truncated.struct_size = 0;
        raw_gcode_firmware_instance invalid_size = owner.instance();
        invalid_size.vtable = &truncated;
        CHECK_THROWS_AS(slic3r_api::GCodeFirmwareView(&invalid_size), std::invalid_argument);

        raw_gcode_firmware_vtable missing_callback = *owner.instance().vtable;
        missing_callback.write_extrusion = nullptr;
        raw_gcode_firmware_instance invalid_callback = owner.instance();
        invalid_callback.vtable = &missing_callback;
        CHECK_THROWS_AS(slic3r_api::GCodeFirmwareView(&invalid_callback), std::invalid_argument);
    }
    CHECK(recorder.destruction_count == 1);
}

TEST_CASE("PrintingPlan file writer serializes every firmware boundary in order",
          "[plugins][gcode][firmware]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    FirmwareRecorder recorder;
    FirmwareInstanceOwner firmware(slic3r_api::make_gcode_firmware_instance(
        std::unique_ptr<slic3r_api::GCodeFirmwareSession>(new RecordingFirmwareSession(recorder))));
    Print print;
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    plan.groups.front().layers.emplace_back();
    PrintingLayerGroup &layer = plan.groups.front().layers.front();
    layer.tool_groups.emplace_back();
    layer.tool_groups.front().extruder_id = 2;
    const LayerRegionIsland *fake_region =
        reinterpret_cast<const LayerRegionIsland *>(uintptr_t(0x2345));
    append_empty_extrusion(
        layer.tool_groups.front(), ExtrusionRole::Perimeter, 7, fake_region);
    append_empty_extrusion(
        layer.tool_groups.front(), ExtrusionRole::None, 0, fake_region);

    const boost::filesystem::path output_path = temporary_gcode_path();
    remove_output_pair(output_path);
    try {
        run_file_writer_with_firmware(print, output_path, firmware.instance());

        const std::vector<std::string> expected_calls{
            "begin_print",
            "begin_group",
            "begin_layer",
            "begin_tool_group",
            "write_extrusion",
            "write_extrusion",
            "end_tool_group",
            "end_layer",
            "end_group",
            "end_print"
        };
        CHECK(recorder.calls == expected_calls);
        CHECK(read_text_file(output_path) ==
              "begin_print\n"
              "begin_group\n"
              "begin_layer\n"
              "begin_tool_group\n"
              "write_extrusion\n"
              "write_extrusion\n"
              "end_tool_group\n"
              "end_layer\n"
              "end_group\n"
              "end_print\n");
        REQUIRE(recorder.roots.size() == 2);
        CHECK(recorder.roots[0]);
        CHECK(recorder.roots[1]);
        REQUIRE(recorder.region_islands.size() == 2);
        CHECK(recorder.region_islands[0]);
        CHECK(recorder.region_islands[1]);
        REQUIRE(recorder.roles.size() == 2);
        CHECK(recorder.roles[0] == RAW_EXTRUSION_ROLE_PERIMETER);
        CHECK(recorder.roles[1] == RAW_EXTRUSION_ROLE_NONE);
        REQUIRE(recorder.object_instance_indices.size() == 2);
        CHECK(recorder.object_instance_indices[0] == 7);
        CHECK(recorder.object_instance_indices[1] == 0);
        CHECK_FALSE(boost::filesystem::exists(output_path.string() + ".tmp"));
    } catch (...) {
        remove_output_pair(output_path);
        throw;
    }
    remove_output_pair(output_path);
}

TEST_CASE("Default firmware selects the tool of an otherwise empty PrintingPlan", "[plugins][gcode]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    select_printing_plan_writer(print);
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    plan.groups.front().layers.emplace_back();
    plan.groups.front().layers.front().tool_groups.emplace_back();
    plan.groups.front().layers.front().tool_groups.front().extruder_id = 0;
    append_empty_extrusion(plan.groups.front().layers.front().tool_groups.front());
    const boost::filesystem::path output_path = temporary_gcode_path();
    remove_output_pair(output_path);

    try {
        Steps::StepGenerateGcode::run_step(Orchestrator::instance(), print, output_path.string());
        REQUIRE(boost::filesystem::exists(output_path));
        CHECK(read_text_file(output_path) == "T0\n");
        CHECK_FALSE(boost::filesystem::exists(output_path.string() + ".tmp"));
    } catch (...) {
        remove_output_pair(output_path);
        throw;
    }
    remove_output_pair(output_path);
}

TEST_CASE("Default firmware publishes an empty file for an empty PrintingPlan", "[plugins][gcode]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    select_printing_plan_writer(print);
    print.mutable_printing_plan();
    const boost::filesystem::path output_path = temporary_gcode_path();
    remove_output_pair(output_path);

    try {
        Steps::StepGenerateGcode::run_step(Orchestrator::instance(), print, output_path.string());
        REQUIRE(boost::filesystem::exists(output_path));
        CHECK(read_text_file(output_path).empty());
        CHECK_FALSE(boost::filesystem::exists(output_path.string() + ".tmp"));
    } catch (...) {
        remove_output_pair(output_path);
        throw;
    }
    remove_output_pair(output_path);
}

TEST_CASE("Default firmware emits inherited process state and ordered linear moves",
          "[plugins][gcode][firmware]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    select_printing_plan_writer(print);
    configure_standard_firmware(print);
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    plan.groups.front().layers.emplace_back();
    PrintingLayerGroup &layer = plan.groups.front().layers.front();
    layer.print_z = scale_i(0.2);
    layer.tool_groups.emplace_back();
    PrintingToolGroup &tool_group = layer.tool_groups.front();
    tool_group.extruder_id = 0;

    ArcPolyline polyline(Points{
        Point(scale_i(0.0), scale_i(0.0)),
        Point(scale_i(10.0), scale_i(0.0)),
        Point(scale_i(20.0), scale_i(0.0))
    });
    polyline.set_z_offset(0, scale_i(0.1));
    polyline.set_z_offset(1, scale_i(0.1));
    polyline.set_z_offset(2, scale_i(0.2));
    append_path_extrusion(tool_group, make_firmware_path(polyline, 20.f, 500.f, 0.04f, 50.f, 210.f));

    const boost::filesystem::path output_path = temporary_gcode_path();
    remove_output_pair(output_path);
    try {
        Steps::StepGenerateGcode::run_step(Orchestrator::instance(), print, output_path.string());
        const std::string output = read_text_file(output_path);
        INFO(output);

        const size_t temperature_pos = output.find("M104 S210 T0\n");
        const size_t fan_pos = output.find("M106 S128\n");
        const size_t pressure_pos = output.find("M900 K0.04\n");
        const size_t travel_pos = output.find("G0 X0 Y0 Z.3 F6000\n");
        const size_t print_acceleration_pos = output.find("M204 P500\n");
        const size_t first_extrusion_pos = output.find("G1 X10 E");
        const size_t second_extrusion_pos = output.find("G1 X20 Z.4 E");
        REQUIRE(temperature_pos != std::string::npos);
        REQUIRE(fan_pos != std::string::npos);
        REQUIRE(pressure_pos != std::string::npos);
        REQUIRE(travel_pos != std::string::npos);
        REQUIRE(print_acceleration_pos != std::string::npos);
        REQUIRE(first_extrusion_pos != std::string::npos);
        REQUIRE(second_extrusion_pos != std::string::npos);
        CHECK(temperature_pos < fan_pos);
        CHECK(fan_pos < pressure_pos);
        CHECK(pressure_pos < travel_pos);
        CHECK(travel_pos < print_acceleration_pos);
        CHECK(print_acceleration_pos < first_extrusion_pos);
        CHECK(first_extrusion_pos < second_extrusion_pos);

        // State remains emitted across both segments. Speed is attached only
        // to the first movement that needs the new value.
        CHECK(count_occurrences(output, "M104 S210 T0\n") == 1);
        CHECK(count_occurrences(output, "M106 S128\n") == 1);
        CHECK(count_occurrences(output, "M900 K0.04\n") == 1);
        CHECK(count_occurrences(output, "M204 P500\n") == 1);
        CHECK(output.find("M204 T") == std::string::npos);
        CHECK(count_occurrences(output, " F1200") == 1);
        CHECK_FALSE(boost::filesystem::exists(output_path.string() + ".tmp"));
    } catch (...) {
        remove_output_pair(output_path);
        throw;
    }
    remove_output_pair(output_path);
}

TEST_CASE("Default firmware preserves arcs and computes their extrusion length",
          "[plugins][gcode][firmware]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    select_printing_plan_writer(print);
    configure_standard_firmware(print);
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    plan.groups.front().layers.emplace_back();
    PrintingLayerGroup &layer = plan.groups.front().layers.front();
    layer.print_z = scale_i(0.2);
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

    const boost::filesystem::path output_path = temporary_gcode_path();
    remove_output_pair(output_path);
    try {
        Steps::StepGenerateGcode::run_step(Orchestrator::instance(), print, output_path.string());
        const std::string output = read_text_file(output_path);
        const size_t arc_pos = output.find("G3 X10 R5 E");
        REQUIRE(arc_pos != std::string::npos);
        CHECK(output.find("G1 X10") == std::string::npos);

        // A 5 mm radius semicircle is longer than the 10 mm chord, so its E
        // output must exceed the straight-path value of about 0.333 mm.
        const size_t e_pos = output.find(" E", arc_pos);
        REQUIRE(e_pos != std::string::npos);
        CHECK(std::stod(output.substr(e_pos + 2)) > 0.5);
    } catch (...) {
        remove_output_pair(output_path);
        throw;
    }
    remove_output_pair(output_path);
}

TEST_CASE("Default firmware interprets one acceleration field from the movement role",
          "[plugins][gcode][firmware]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    select_printing_plan_writer(print);
    configure_standard_firmware(print);
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    plan.groups.front().layers.emplace_back();
    PrintingLayerGroup &layer = plan.groups.front().layers.front();
    layer.tool_groups.emplace_back();
    PrintingToolGroup &tool_group = layer.tool_groups.front();
    tool_group.extruder_id = 0;

    // The travel and the following print path carry the same property type.
    // Their effective roles decide whether Marlin receives M204 T or M204 P.
    append_path_extrusion(
        tool_group,
        make_firmware_travel(
            ArcPolyline(Points{Point(scale_i(0.0), scale_i(0.0)), Point(scale_i(10.0), scale_i(0.0))}),
            80.f,
            1200.f));
    append_path_extrusion(
        tool_group,
        make_firmware_path(
            ArcPolyline(Points{Point(scale_i(10.0), scale_i(0.0)), Point(scale_i(11.0), scale_i(0.0))}),
            20.f,
            400.f));

    const boost::filesystem::path output_path = temporary_gcode_path();
    remove_output_pair(output_path);
    try {
        Steps::StepGenerateGcode::run_step(Orchestrator::instance(), print, output_path.string());
        const std::string output = read_text_file(output_path);
        INFO(output);
        CHECK(count_occurrences(output, "M204 T1200\n") == 1);
        CHECK(count_occurrences(output, "M204 P400\n") == 1);
        CHECK(output.find("G0 X10 F4800\n") != std::string::npos);
        CHECK(output.find("G0 X10 E") == std::string::npos);
        CHECK(output.find("G1 X11 E") != std::string::npos);
    } catch (...) {
        remove_output_pair(output_path);
        throw;
    }
    remove_output_pair(output_path);
}

TEST_CASE("Default firmware writes the E value prepared by the extruder state",
          "[plugins][gcode][firmware]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    select_printing_plan_writer(print);
    configure_standard_firmware(print);
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    plan.groups.front().layers.emplace_back();
    PrintingLayerGroup &layer = plan.groups.front().layers.front();
    layer.tool_groups.emplace_back();
    PrintingToolGroup &tool_group = layer.tool_groups.front();
    tool_group.extruder_id = 0;

    // Two invisible deltas cross the output precision together. The last
    // command then returns the absolute coordinate to zero, which must remain
    // distinguishable from an absent E word.
    ExtrusionEntity::Children children;
    children.push_back(special_command_entity(ExtrusionPropertySpecialCommand::Code::EXTRUSION, 0.000004));
    children.push_back(special_command_entity(ExtrusionPropertySpecialCommand::Code::EXTRUSION, 0.000004));
    children.push_back(special_command_entity(ExtrusionPropertySpecialCommand::Code::EXTRUSION, -0.000008));
    std::unique_ptr<ExtrusionEntity> root(
        new ExtrusionEntity(std::move(children), false, true, true));
    root->add_property(ExtrusionPropertySpeed(12.f, 300.f));
    append_path_extrusion(tool_group, std::move(root));

    const boost::filesystem::path output_path = temporary_gcode_path();
    remove_output_pair(output_path);
    try {
        Steps::StepGenerateGcode::run_step(Orchestrator::instance(), print, output_path.string());
        const std::string output = read_text_file(output_path);
        INFO(output);
        CHECK(output.find("G1 E.00001") != std::string::npos);
        CHECK(output.find("G1 E0") != std::string::npos);
        CHECK(count_occurrences(output, "G1 E") == 2);
    } catch (...) {
        remove_output_pair(output_path);
        throw;
    }
    remove_output_pair(output_path);
}

TEST_CASE("A derived firmware may replace only the fan encoder",
          "[plugins][gcode][firmware]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    configure_standard_firmware(print);
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    plan.groups.front().layers.emplace_back();
    PrintingLayerGroup &layer = plan.groups.front().layers.front();
    layer.tool_groups.emplace_back();
    PrintingToolGroup &tool_group = layer.tool_groups.front();
    tool_group.extruder_id = 0;
    append_path_extrusion(
        tool_group,
        make_firmware_path(
            ArcPolyline(Points{Point(scale_i(0.0), scale_i(0.0)), Point(scale_i(1.0), scale_i(0.0))}),
            15.f,
            400.f,
            -1.f,
            40.f));

    FirmwareInstanceOwner firmware(slic3r_api::make_gcode_firmware_instance(
        std::unique_ptr<slic3r_api::GCodeFirmwareSession>(new CustomFanFirmwareSession())));
    const boost::filesystem::path output_path = temporary_gcode_path();
    remove_output_pair(output_path);
    try {
        run_file_writer_with_firmware(print, output_path, firmware.instance());
        const std::string output = read_text_file(output_path);
        CHECK(output.find("CUSTOM_FAN T0 S40\n") != std::string::npos);
        CHECK(output.find("M106") == std::string::npos);
        CHECK(output.find("M204 P400\n") != std::string::npos);
        CHECK(output.find("G1 X1 E") != std::string::npos);
    } catch (...) {
        remove_output_pair(output_path);
        throw;
    }
    remove_output_pair(output_path);
}

TEST_CASE("A derived firmware may extend extrusion traversal without replacing it",
          "[plugins][gcode][firmware]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    configure_standard_firmware(print);
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    plan.groups.front().layers.emplace_back();
    PrintingLayerGroup &layer = plan.groups.front().layers.front();
    layer.tool_groups.emplace_back();
    PrintingToolGroup &tool_group = layer.tool_groups.front();
    tool_group.extruder_id = 0;

    // The collection and its path form two nested nodes. The derived session
    // observes both scopes and the leaf while the base visitor still performs
    // all standard state resolution and geometry serialization.
    ExtrusionEntity::Children children;
    children.push_back(make_firmware_path(
        ArcPolyline(Points{Point(scale_i(0.0), scale_i(0.0)), Point(scale_i(1.0), scale_i(0.0))}),
        15.f,
        400.f));
    append_path_extrusion(
        tool_group,
        std::unique_ptr<ExtrusionEntity>(new ExtrusionEntity(std::move(children), false, true, true)));

    std::vector<std::string> calls;
    FirmwareInstanceOwner firmware(slic3r_api::make_gcode_firmware_instance(
        std::unique_ptr<slic3r_api::GCodeFirmwareSession>(new TraversalHookFirmwareSession(calls))));
    const boost::filesystem::path output_path = temporary_gcode_path();
    remove_output_pair(output_path);
    try {
        run_file_writer_with_firmware(print, output_path, firmware.instance());
        const std::vector<std::string> expected{"enter", "enter", "leaf", "leave", "leave"};
        CHECK(calls == expected);
    } catch (...) {
        remove_output_pair(output_path);
        throw;
    }
    remove_output_pair(output_path);
}

TEST_CASE("Default firmware restores inherited requests between sibling nodes",
          "[plugins][gcode][firmware]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    select_printing_plan_writer(print);
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
        ArcPolyline(Points{Point(scale_i(0.0), scale_i(0.0)), Point(scale_i(1.0), scale_i(0.0))}),
        -1.f,
        -1.f));
    children.push_back(make_firmware_path(
        ArcPolyline(Points{Point(scale_i(1.0), scale_i(0.0)), Point(scale_i(2.0), scale_i(0.0))}),
        30.f,
        700.f,
        0.08f,
        70.f,
        220.f));
    children.push_back(make_firmware_path(
        ArcPolyline(Points{Point(scale_i(2.0), scale_i(0.0)), Point(scale_i(3.0), scale_i(0.0))}),
        -1.f,
        -1.f));
    std::unique_ptr<ExtrusionEntity> root(
        new ExtrusionEntity(std::move(children), false, true, true));
    root->add_property(ExtrusionPropertySpeed(20.f, 500.f, 0.04f, 50.f, 210.f));
    append_path_extrusion(tool_group, std::move(root));

    const boost::filesystem::path output_path = temporary_gcode_path();
    remove_output_pair(output_path);
    try {
        Steps::StepGenerateGcode::run_step(Orchestrator::instance(), print, output_path.string());
        const std::string output = read_text_file(output_path);
        INFO(output);

        const size_t first_temperature = output.find("M104 S210 T0\n");
        const size_t child_temperature = output.find("M104 S220 T0\n");
        const size_t restored_temperature = output.find("M104 S210 T0\n", first_temperature + 1);
        REQUIRE(first_temperature != std::string::npos);
        REQUIRE(child_temperature != std::string::npos);
        REQUIRE(restored_temperature != std::string::npos);
        CHECK(first_temperature < child_temperature);
        CHECK(child_temperature < restored_temperature);
        CHECK(count_occurrences(output, "M204 P500\n") == 2);
        CHECK(count_occurrences(output, "M204 P700\n") == 1);
        CHECK(count_occurrences(output, " F1200") == 2);
        CHECK(count_occurrences(output, " F1800") == 1);
        CHECK(count_occurrences(output, "M106 S128\n") == 2);
        CHECK(count_occurrences(output, "M106 S179\n") == 1);
    } catch (...) {
        remove_output_pair(output_path);
        throw;
    }
    remove_output_pair(output_path);
}

TEST_CASE("Default firmware handles ordered special commands and custom G-code",
          "[plugins][gcode][firmware]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    configure_standard_firmware(print, 2);
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    plan.groups.front().layers.emplace_back();
    PrintingLayerGroup &layer = plan.groups.front().layers.front();
    layer.tool_groups.emplace_back();
    PrintingToolGroup &tool_group = layer.tool_groups.front();
    tool_group.extruder_id = 0;

    ExtrusionEntity::Children children;
    children.push_back(special_command_entity(ExtrusionPropertySpecialCommand::Code::TOOLCHANGE, 1.0));
    children.push_back(special_command_entity(
        ExtrusionPropertySpecialCommand::Code::SAVE_AND_RESET_SPEED_RATIO, 0.8));
    children.push_back(special_command_entity(
        ExtrusionPropertySpecialCommand::Code::RESTORE_SPEED_RATIO));
    children.push_back(special_command_entity(
        ExtrusionPropertySpecialCommand::Code::FLUSH_PLANNER_QUEUE));
    children.push_back(special_command_entity(
        ExtrusionPropertySpecialCommand::Code::EXTRUSION, 0.5));
    children.push_back(special_command_entity(
        ExtrusionPropertySpecialCommand::Code::RETRACT, -0.2));
    children.push_back(special_command_entity(
        ExtrusionPropertySpecialCommand::Code::PAUSE, 25.0));
    children.push_back(special_command_entity(
        ExtrusionPropertySpecialCommand::Code::WAIT_FOR_TEMP));
    children.push_back(special_command_entity(
        ExtrusionPropertySpecialCommand::Code::WAIT_FOR_TEMP));
    children.push_back(special_command_entity(
        ExtrusionPropertySpecialCommand::Code::DISABLE_PREVIEW));
    children.push_back(special_command_entity(
        ExtrusionPropertySpecialCommand::Code::ENABLE_PREVIEW));
    children.push_back(special_command_entity(
        ExtrusionPropertySpecialCommand::Code::EXTRUDER_CURRENT, 650.0));
    std::unique_ptr<ExtrusionNop> custom(new ExtrusionNop());
    custom->add_property(ExtrusionPropertyCustomGcodeText("M117 ready"));
    children.push_back(std::move(custom));

    std::unique_ptr<ExtrusionEntity> root(
        new ExtrusionEntity(std::move(children), false, true, true));
    root->add_property(ExtrusionPropertySpeed(12.f, 300.f, 0.02f, 30.f, 205.f));
    append_path_extrusion(tool_group, std::move(root));

    FirmwareInstanceOwner firmware(slic3r_api::make_gcode_firmware_instance(
        std::unique_ptr<slic3r_api::GCodeFirmwareSession>(new PreviewInspectingFirmwareSession())));
    const boost::filesystem::path output_path = temporary_gcode_path();
    remove_output_pair(output_path);
    try {
        run_file_writer_with_firmware(print, output_path, firmware.instance());
        const std::string output = read_text_file(output_path);
        INFO(output);
        CHECK(output.find("T0\nT1\n") != std::string::npos);
        CHECK(output.find("M220 B\nM220 S80\n") != std::string::npos);
        CHECK(output.find("M220 R\n") != std::string::npos);
        CHECK(output.find("G4 S0\n") != std::string::npos);
        CHECK(output.find("G1 E") != std::string::npos);
        CHECK(output.find(" F720\n") != std::string::npos);
        CHECK(output.find(" F2400\n") != std::string::npos);
        CHECK(output.find("G4 P25\n") != std::string::npos);
        CHECK(output.find("M109 S205 T1\n") != std::string::npos);
        CHECK(count_occurrences(output, "M109 S205 T1\n") == 1);
        CHECK(output.find("M906 T1 E650\n") != std::string::npos);
        CHECK(output.find("M117 ready\n") != std::string::npos);
        CHECK(output.find("PREVIEW_ON\n") != std::string::npos);
    } catch (...) {
        remove_output_pair(output_path);
        throw;
    }
    remove_output_pair(output_path);
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

TEST_CASE("The generic firmware session keeps every tool environment independent",
          "[plugins][gcode][firmware]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    configure_standard_firmware(print, 2);
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    plan.groups.front().layers.emplace_back();
    append_multi_tool_process_sequence(plan.groups.front().layers.front());

    FirmwareInstanceOwner firmware(slic3r_api::make_gcode_firmware_instance(
        std::unique_ptr<slic3r_api::GCodeFirmwareSession>(
            new slic3r_api::GCodeGeneration::DefaultGCodeFirmwareSession())));
    const boost::filesystem::path output_path = temporary_gcode_path();
    remove_output_pair(output_path);
    try {
        run_file_writer_with_firmware(print, output_path, firmware.instance());
        const std::string output = read_text_file(output_path);
        INFO(output);
        CHECK(count_occurrences(output, "M104 S200 T0\n") == 1);
        CHECK(count_occurrences(output, "M104 S210 T1\n") == 1);
        CHECK(count_occurrences(output, "M106 S51\n") == 1);
        CHECK(count_occurrences(output, "M106 S77\n") == 1);
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

TEST_CASE("Default firmware rejects missing process properties and unknown tools",
          "[plugins][gcode][firmware]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    SECTION("printable leaf without valid flow attributes") {
        Print print;
        select_printing_plan_writer(print);
        configure_standard_firmware(print);
        PrintingPlan &plan = print.mutable_printing_plan();
        plan.groups.emplace_back();
        plan.groups.front().layers.emplace_back();
        PrintingLayerGroup &layer = plan.groups.front().layers.front();
        layer.tool_groups.emplace_back();
        PrintingToolGroup &tool_group = layer.tool_groups.front();
        tool_group.extruder_id = 0;

        // A printable role with default attributes has no usable mm3_per_mm,
        // even though its process speed and acceleration are otherwise valid.
        std::unique_ptr<ExtrusionPath> path(new ExtrusionPath(
            ArcPolyline(Points{Point(scale_i(0.0), scale_i(0.0)), Point(scale_i(1.0), scale_i(0.0))}),
            ExtrusionAttributes(ExtrusionRole::Perimeter),
            nullptr,
            true));
        path->add_property(ExtrusionPropertySpeed(20.f, 500.f));
        append_path_extrusion(tool_group, std::move(path));

        const boost::filesystem::path output_path = temporary_gcode_path();
        remove_output_pair(output_path);
        CHECK_THROWS_AS(
            Steps::StepGenerateGcode::run_step(Orchestrator::instance(), print, output_path.string()),
            RuntimeError);
        Orchestrator::instance().reset_plugin_cancel();
        CHECK_FALSE(boost::filesystem::exists(output_path));
        CHECK_FALSE(boost::filesystem::exists(output_path.string() + ".tmp"));
    }

    SECTION("printable leaf without speed and acceleration") {
        Print print;
        select_printing_plan_writer(print);
        configure_standard_firmware(print);
        PrintingPlan &plan = print.mutable_printing_plan();
        plan.groups.emplace_back();
        plan.groups.front().layers.emplace_back();
        PrintingLayerGroup &layer = plan.groups.front().layers.front();
        layer.tool_groups.emplace_back();
        PrintingToolGroup &tool_group = layer.tool_groups.front();
        tool_group.extruder_id = 0;
        append_path_extrusion(
            tool_group,
            make_firmware_path(
                ArcPolyline(Points{Point(scale_i(0.0), scale_i(0.0)), Point(scale_i(1.0), scale_i(0.0))}),
                -1.f,
                -1.f));

        const boost::filesystem::path output_path = temporary_gcode_path();
        remove_output_pair(output_path);
        write_text_file(output_path, "previous output\n");
        CHECK_THROWS_AS(
            Steps::StepGenerateGcode::run_step(Orchestrator::instance(), print, output_path.string()),
            RuntimeError);
        Orchestrator::instance().reset_plugin_cancel();
        CHECK(read_text_file(output_path) == "previous output\n");
        CHECK_FALSE(boost::filesystem::exists(output_path.string() + ".tmp"));
        remove_output_pair(output_path);
    }

    SECTION("tool group outside the configured range") {
        Print print;
        select_printing_plan_writer(print);
        configure_standard_firmware(print);
        PrintingPlan &plan = print.mutable_printing_plan();
        plan.groups.emplace_back();
        plan.groups.front().layers.emplace_back();
        PrintingLayerGroup &layer = plan.groups.front().layers.front();
        layer.tool_groups.emplace_back();
        PrintingToolGroup &tool_group = layer.tool_groups.front();
        tool_group.extruder_id = 4;
        append_empty_extrusion(tool_group);

        const boost::filesystem::path output_path = temporary_gcode_path();
        remove_output_pair(output_path);
        CHECK_THROWS_AS(
            Steps::StepGenerateGcode::run_step(Orchestrator::instance(), print, output_path.string()),
            RuntimeError);
        Orchestrator::instance().reset_plugin_cancel();
        CHECK_FALSE(boost::filesystem::exists(output_path));
        CHECK_FALSE(boost::filesystem::exists(output_path.string() + ".tmp"));
    }
}

TEST_CASE("Firmware exceptions prevent final output publication", "[plugins][gcode][firmware]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    FirmwareRecorder recorder;
    recorder.throw_on_extrusion = true;
    FirmwareInstanceOwner firmware(slic3r_api::make_gcode_firmware_instance(
        std::unique_ptr<slic3r_api::GCodeFirmwareSession>(new RecordingFirmwareSession(recorder))));
    Print print;
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    plan.groups.front().layers.emplace_back();
    plan.groups.front().layers.front().tool_groups.emplace_back();
    append_empty_extrusion(plan.groups.front().layers.front().tool_groups.front());

    const boost::filesystem::path output_path = temporary_gcode_path();
    remove_output_pair(output_path);
    write_text_file(output_path, "previous output\n");
    CHECK_THROWS_AS(run_file_writer_with_firmware(print, output_path, firmware.instance()), RuntimeError);
    REQUIRE(boost::filesystem::exists(output_path));
    CHECK(read_text_file(output_path) == "previous output\n");
    CHECK_FALSE(boost::filesystem::exists(output_path.string() + ".tmp"));
    remove_output_pair(output_path);
}

TEST_CASE("STEP_GCODE fails when the output directory is unavailable", "[plugins][gcode]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Orchestrator &orchestrator = Orchestrator::instance();
    orchestrator.reset_plugin_cancel();
    Print print;
    select_printing_plan_writer(print);
    print.mutable_printing_plan();
    const boost::filesystem::path missing_directory =
        boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("slic3r-missing-output-dir-%%%%-%%%%");
    const boost::filesystem::path output_path = missing_directory / "out.gcode";

    CHECK_THROWS_AS(Steps::StepGenerateGcode::run_step(orchestrator, print, output_path.string()),
                    RuntimeError);
    orchestrator.reset_plugin_cancel();
    CHECK_FALSE(boost::filesystem::exists(output_path));
    CHECK_FALSE(boost::filesystem::exists(output_path.string() + ".tmp"));
}

TEST_CASE("Orchestrator export_gcode still routes through ordering and STEP_GCODE", "[plugins][gcode]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    select_printing_plan_writer(print);
    for (slicing_step_t step : Steps::execution_order())
        print.mark_step_executed(step);

    GCodeProcessorResult result;
    const boost::filesystem::path output_path = temporary_gcode_path();
    remove_output_pair(output_path);
    std::vector<PrintBase::SlicingStatus> statuses;
    print.set_status_callback([&statuses](const PrintBase::SlicingStatus &status) {
        statuses.push_back(status);
    });

    try {
        const std::string generated_path =
            Orchestrator::instance().export_gcode(print, output_path.string(), &result, nullptr);
        CHECK(generated_path == output_path.string());
        CHECK(result.filename == output_path.string());
        REQUIRE(boost::filesystem::exists(output_path));
        CHECK(read_text_file(output_path).empty());
        REQUIRE(print.printing_plan() != nullptr);
        CHECK(print.printing_plan()->groups.size() == 1);

        // Step labels are not boost::format templates. A leftover path argument
        // would make the GUI throw boost::io::too_many_args while showing this
        // otherwise successful export.
        bool saw_ordering_status = false;
        for (const PrintBase::SlicingStatus &status : statuses) {
            if (status.main_text != "Ordering extrusions")
                continue;
            saw_ordering_status = true;
            CHECK(status.args.empty());
        }
        CHECK(saw_ordering_status);
    } catch (...) {
        remove_output_pair(output_path);
        throw;
    }
    remove_output_pair(output_path);
}
