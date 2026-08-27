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

    std::string write_event(const slic3r_api::ExtrusionEntity &) override
    {
        m_recorder.calls.emplace_back("write_event");
        return "write_event\n";
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

// Exposes the protected syntax encoder so kind dispatch can be tested without
// involving extrusion traversal or filesystem publication.
class CustomGCodeFirmwareProbe final :
    public slic3r_api::GCodeGeneration::DefaultGCodeFirmwareSession
{
public:
    using DefaultGCodeFirmwareSession::encode_custom_gcode;
};

// Exposes the protected script bridge and machine state so tests can verify
// that validated host outputs are imported only after the complete script.
class ScriptStateFirmwareProbe final :
    public slic3r_api::GCodeGeneration::DefaultGCodeFirmwareSession
{
public:
    ScriptStateFirmwareProbe() = default;
    explicit ScriptStateFirmwareProbe(
        slic3r_api::GCodeGeneration::GCodeScriptProcessorView scripts,
        storage_handle *storage = nullptr) :
        DefaultGCodeFirmwareSession(scripts, storage)
    {}

    using DefaultGCodeFirmwareSession::process_script;

    const slic3r_api::GCodeGeneration::Gantry &machine_gantry() const
    {
        return gantry();
    }

    slic3r_api::GCodeGeneration::DefaultExtruder &machine_extruder(size_t idx)
    {
        return extruders().at(idx);
    }

    const slic3r_api::GCodeGeneration::Printer &machine_printer() const
    {
        return printer();
    }
};

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
                                                  float temperature = -1.f,
                                                  ExtrusionRole role = ExtrusionRole::Perimeter)
{
    const ExtrusionAttributes attributes(
        role,
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

gcode_script_type registered_script_type(Orchestrator &orchestrator, const std::string &name) {
    const gcode_script_type type = gcode_script_register_type(reinterpret_cast<orchestrator_handle *>(&orchestrator),
                                                              name.c_str());
    if (type == GCODE_SCRIPT_TYPE_INVALID)
        throw std::runtime_error("The test could not register G-code script type '" + name + "'.");
    return type;
}

DynamicConfig stored_script_config(
    const ExtrusionEntity &entity,
    const ExtrusionPropertyCustomGcode &property)
{
    if (property.config_id == EXTRUSION_DATA_ID_INVALID)
        throw std::runtime_error("The scripted event has no stored Config snapshot.");

    const slic3r_api::ExtrusionEntity view(
        reinterpret_cast<const extrusion_entity_handle *>(&entity));
    uint32_t byte_size = 0;
    const char *serialized = static_cast<const char *>(
        view.stored_data(property.config_id, &byte_size));
    if (serialized == nullptr || byte_size == 0 || serialized[byte_size - 1] != '\0')
        throw std::runtime_error("The scripted event has an invalid Config buffer.");

    DynamicConfig config;
    if (!ConfigSnapshotSerialization::deserialize_all(
            std::string(serialized, serialized + byte_size - 1), config))
        throw std::runtime_error("The scripted event has an invalid Config snapshot.");
    return config;
}

int32_t stored_script_int(
    const ExtrusionEntity &entity,
    const ExtrusionPropertyCustomGcode &property,
    const char *key)
{
    DynamicConfig config = stored_script_config(entity, property);
    const ConfigOptionInt *option = config.option<ConfigOptionInt>(key);
    if (option == nullptr)
        throw std::runtime_error(std::string("Missing stored integer option: ") + key);
    return option->value;
}

double stored_script_float(
    const ExtrusionEntity &entity,
    const ExtrusionPropertyCustomGcode &property,
    const char *key)
{
    DynamicConfig config = stored_script_config(entity, property);
    const ConfigOptionFloat *option = config.option<ConfigOptionFloat>(key);
    if (option == nullptr)
        throw std::runtime_error(std::string("Missing stored float option: ") + key);
    return option->value;
}

std::string stored_script_string(
    const ExtrusionEntity &entity,
    const ExtrusionPropertyCustomGcode &property,
    const char *key)
{
    DynamicConfig config = stored_script_config(entity, property);
    const ConfigOptionString *option = config.option<ConfigOptionString>(key);
    if (option == nullptr)
        throw std::runtime_error(std::string("Missing stored string option: ") + key);
    return option->value;
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

slic3r_api::PrintingPlan printing_plan_view(Print &print)
{
    return slic3r_api::PrintingPlan(
        reinterpret_cast<printing_plan_handle *>(&print.mutable_printing_plan()));
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
        CHECK_THROWS_AS(
            session.begin_print(print_view), std::invalid_argument);
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
        missing_callback.write_event = nullptr;
        raw_gcode_firmware_instance invalid_callback = owner.instance();
        invalid_callback.vtable = &missing_callback;
        CHECK_THROWS_AS(slic3r_api::GCodeFirmwareView(&invalid_callback), std::invalid_argument);
    }
    CHECK(recorder.destruction_count == 1);
}

TEST_CASE("PrintingPlan file writer serializes populated scope events inside their boundaries",
          "[plugins][gcode][firmware][printing][plan][events]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    FirmwareRecorder recorder;
    FirmwareInstanceOwner firmware(slic3r_api::make_gcode_firmware_instance(
        std::unique_ptr<slic3r_api::GCodeFirmwareSession>(new RecordingFirmwareSession(recorder))));
    Print print;
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    PrintingGroup &group = plan.groups.front();
    group.layers.emplace_back();
    PrintingLayerGroup &layer = group.layers.front();
    layer.tool_groups.emplace_back();
    PrintingToolGroup &tool_group = layer.tool_groups.front();
    const LayerRegionIsland *fake_region =
        reinterpret_cast<const LayerRegionIsland *>(uintptr_t(0x3456));
    append_empty_extrusion(tool_group, ExtrusionRole::None, 0, fake_region);

    // One child makes each fixed event root observable without depending on a
    // specific event property. The recording firmware marks every callback.
    ExtrusionEntity event(true);
    plan.events.append_before(event);
    plan.events.append_after(event);
    group.events.append_before(event);
    group.events.append_after(event);
    layer.events.append_before(event);
    layer.events.append_after(event);
    tool_group.events.append_before(event);
    tool_group.events.append_after(event);

    const boost::filesystem::path output_path = temporary_gcode_path();
    remove_output_pair(output_path);
    try {
        run_file_writer_with_firmware(print, output_path, firmware.instance());

        const std::vector<std::string> expected_calls{
            "begin_print", "write_event",
            "begin_group", "write_event",
            "begin_layer", "write_event",
            "begin_tool_group", "write_event",
            "write_extrusion",
            "write_event", "end_tool_group",
            "write_event", "end_layer",
            "write_event", "end_group",
            "write_event", "end_print"
        };
        CHECK(recorder.calls == expected_calls);
        CHECK(read_text_file(output_path) ==
              "begin_print\nwrite_event\n"
              "begin_group\nwrite_event\n"
              "begin_layer\nwrite_event\n"
              "begin_tool_group\nwrite_event\n"
              "write_extrusion\n"
              "write_event\nend_tool_group\n"
              "write_event\nend_layer\n"
              "write_event\nend_group\n"
              "write_event\nend_print\n");
    } catch (...) {
        remove_output_pair(output_path);
        throw;
    }
    remove_output_pair(output_path);
}

TEST_CASE("Default firmware interprets scope events with its extrusion visitor",
          "[plugins][gcode][firmware][printing][plan][events]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    select_printing_plan_writer(print);
    PrintingPlan &plan = print.mutable_printing_plan();
    ExtrusionNop event;
    event.add_property(ExtrusionPropertyCustomGcodeText("M117 scope event"));
    plan.events.append_before(event);

    FirmwareInstanceOwner firmware(slic3r_api::make_gcode_firmware_instance(
        std::unique_ptr<slic3r_api::GCodeFirmwareSession>(
            new slic3r_api::GCodeGeneration::DefaultGCodeFirmwareSession())));
    const boost::filesystem::path output_path = temporary_gcode_path();
    remove_output_pair(output_path);
    try {
        run_file_writer_with_firmware(print, output_path, firmware.instance());
        CHECK(read_text_file(output_path) == "M117 scope event\n");
    } catch (...) {
        remove_output_pair(output_path);
        throw;
    }
    remove_output_pair(output_path);
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
    configure_standard_firmware(print, 2);
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

TEST_CASE("Default firmware distinguishes raw G-code, comments and scripts",
          "[plugins][gcode][firmware][custom-gcode]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    CustomGCodeFirmwareProbe firmware;

    CHECK(firmware.encode_custom_gcode(
              C_EXTRUSION_CUSTOM_GCODE_GCODE, "M117 raw") == "M117 raw\n");
    CHECK(firmware.encode_custom_gcode(
              C_EXTRUSION_CUSTOM_GCODE_COMMENT, "first\nsecond") ==
          "; first\n; second\n");
    CHECK_THROWS_AS(
        firmware.encode_custom_gcode(C_EXTRUSION_CUSTOM_GCODE_SCRIPT, "M117 {layer_num}"),
        std::invalid_argument);
    CHECK_THROWS_AS(
        firmware.encode_custom_gcode(
            static_cast<c_extrusion_custom_gcode_kind>(99), "M117 unknown"),
        std::invalid_argument);

    Print print;
    configure_standard_firmware(print);
    const slic3r_api::Print print_view(reinterpret_cast<const print_handle *>(&print));
    CHECK(firmware.begin_print(print_view).empty());

    ExtrusionNop typed_raw(ExtrusionPropertyCustomGcodeText("M117 raw"));
    ExtrusionPropertyCustomGcode *typed_raw_property = typed_raw.get_property<ExtrusionPropertyCustomGcode>();
    REQUIRE(typed_raw_property != nullptr);
    typed_raw_property->script_type = GCODE_SCRIPT_TYPE_START_GCODE;
    CHECK_THROWS_AS(firmware.write_event(
                        slic3r_api::ExtrusionEntity(reinterpret_cast<const extrusion_entity_handle *>(&typed_raw))),
                    std::invalid_argument);

    ExtrusionNop untyped_script(
        ExtrusionPropertyCustomGcodeText(ExtrusionPropertyCustomGcodeText::Code::SCRIPT, "M117 script"));
    ExtrusionPropertyCustomGcode *untyped_script_property = untyped_script.get_property<ExtrusionPropertyCustomGcode>();
    REQUIRE(untyped_script_property != nullptr);
    untyped_script_property->script_type = GCODE_SCRIPT_TYPE_INVALID;
    CHECK_THROWS_AS(firmware.write_event(slic3r_api::ExtrusionEntity(
                        reinterpret_cast<const extrusion_entity_handle *>(&untyped_script))),
                    std::invalid_argument);
}

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

TEST_CASE("PrintingPlan firmware executes host-owned scripts",
          "[plugins][gcode][firmware][custom-gcode]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    select_printing_plan_writer(print);
    configure_standard_firmware(print);
    PrintingPlan &plan = print.mutable_printing_plan();
    ExtrusionNop script(ExtrusionPropertyCustomGcodeText(
        ExtrusionPropertyCustomGcodeText::Code::SCRIPT,
        "M117 X{position[0]}"));
    plan.events.append_before(script);

    const boost::filesystem::path output_path = temporary_gcode_path();
    remove_output_pair(output_path);
    try {
        Steps::StepGenerateGcode::run_step(
            Orchestrator::instance(), print, output_path.string());
        REQUIRE(boost::filesystem::exists(output_path));
        const std::string output = read_text_file(output_path);
        CHECK(output.find("M117 X0") != std::string::npos);
        CHECK(output.find("{position") == std::string::npos);
    } catch (...) {
        remove_output_pair(output_path);
        throw;
    }
    remove_output_pair(output_path);
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
    REQUIRE(travel_root.child_count() == 1);
    const ExtrusionPropertyCustomGcode *travel_property =
        travel_root.get_property<ExtrusionPropertyCustomGcode>();
    REQUIRE(travel_property != nullptr);
    CHECK(travel_property->script_type == GCODE_SCRIPT_TYPE_FEATURE_GCODE);
    CHECK(stored_script_string(travel_root, *travel_property, "previous_extrusion_role") == "Perimeter");
    CHECK(stored_script_string(travel_root, *travel_property, "next_extrusion_role") == "Travel");
    const ExtrusionPropertyCustomGcode *preserved_property =
        travel_root.child(0).get_property<ExtrusionPropertyCustomGcode>();
    REQUIRE(preserved_property != nullptr);
    CHECK(travel_root.child(0).custom_gcode_string(*preserved_property) == "M117 existing travel");

    CHECK(same_role_next_layer.extrusions[0].root->get_property<ExtrusionPropertyCustomGcode>() == nullptr);
    CHECK(same_role_next_tool.extrusions[0].root->get_property<ExtrusionPropertyCustomGcode>() == nullptr);

    const ExtrusionEntity &infill_root = *second_tool.extrusions[0].root;
    const ExtrusionPropertyCustomGcode *infill_property =
        infill_root.get_property<ExtrusionPropertyCustomGcode>();
    REQUIRE(infill_property != nullptr);
    CHECK(stored_script_string(infill_root, *infill_property, "previous_extrusion_role") == "Travel");
    CHECK(stored_script_string(infill_root, *infill_property, "next_extrusion_role") == "Internal infill");

    // A second execution removes and recreates only this plugin's annotations.
    // The custom travel event remains one child below exactly one wrapper.
    Steps::StepExtrusionEdition::clean_and_prepare(print);
    Steps::StepExtrusionEdition::run_step(Orchestrator::instance(), print);
    CHECK(first_root.child_count() == 0);
    REQUIRE(travel_root.child_count() == 1);
    CHECK(travel_root.child(0).child_count() == 0);

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

TEST_CASE("Configured layer and tool scripts become scoped plan events",
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
        second_layer_event.property<slic3r_api::EPropertyCustomGcode>();
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
    REQUIRE(changed_tool.events.before().child_count() == 2);
    const ExtrusionPropertyCustomGcode *end_filament =
        unchanged_tool.events.after().child(0).get_property<ExtrusionPropertyCustomGcode>();
    const ExtrusionPropertyCustomGcode *toolchange =
        changed_tool.events.before().child(0).get_property<ExtrusionPropertyCustomGcode>();
    const ExtrusionPropertyCustomGcode *start_filament =
        changed_tool.events.before().child(1).get_property<ExtrusionPropertyCustomGcode>();
    REQUIRE(end_filament != nullptr);
    REQUIRE(toolchange != nullptr);
    REQUIRE(start_filament != nullptr);
    CHECK(end_filament->script_type == GCODE_SCRIPT_TYPE_END_FILAMENT_GCODE);
    CHECK(end_filament->processing_extruder_id == 1);
    CHECK(toolchange->script_type == GCODE_SCRIPT_TYPE_TOOLCHANGE_GCODE);
    CHECK(toolchange->processing_extruder_id == 0);
    CHECK(stored_script_int(
              changed_tool.events.before().child(0), *toolchange, "previous_extruder") == 1);
    CHECK(stored_script_int(
              changed_tool.events.before().child(0), *toolchange, "next_extruder") == 0);
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
        event_view.property<slic3r_api::EPropertyCustomGcode>();
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
        event_view.property<slic3r_api::EPropertyCustomGcode>();
    REQUIRE(property != nullptr);
    CHECK(property->processing_extruder_id == 1);
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
        copied.property<slic3r_api::EPropertyCustomGcode>();
    REQUIRE(copied_property != nullptr);
    CHECK(copied_property->processing_extruder_id == 2);
    REQUIRE(copied_property->config_id != EXTRUSION_DATA_ID_INVALID);
    slic3r_api::StoredConfig copied_config(storage);
    copied_config.deserialize_all(copied.stored_string(copied_property->config_id));
    CHECK(copied_config.get("producer_note").get_string() == "owned with event");
    copied.custom_gcode("M117 raw", C_EXTRUSION_CUSTOM_GCODE_GCODE);
    copied_property = copied.property<slic3r_api::EPropertyCustomGcode>();
    REQUIRE(copied_property != nullptr);
    CHECK(copied_property->processing_extruder_id == GCODE_SCRIPT_PROCESSING_EXTRUDER_INVALID);
    CHECK(copied_property->config_id == EXTRUSION_DATA_ID_INVALID);

    slic3r_api::StoredExtrusionEntity moved(std::move(source));
    const slic3r_api::EPropertyCustomGcode *moved_property =
        moved.property<slic3r_api::EPropertyCustomGcode>();
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
        event.property<slic3r_api::EPropertyCustomGcode>();
    REQUIRE(replaced != nullptr);
    CHECK(replaced->config_id != EXTRUSION_DATA_ID_INVALID);
    CHECK(event.stored_data(initial_id) == nullptr);
    slic3r_api::StoredConfig decoded_replacement(storage);
    decoded_replacement.deserialize_all(event.stored_string(replaced->config_id));
    CHECK(decoded_replacement.get("layer_num").get_int() == 9);
    CHECK(decoded_replacement.get("layer_z").get_float() == Approx(0.4));

    slic3r_api::StoredConfig empty_config(storage);
    event.script_gcode("M117 empty", GCODE_SCRIPT_TYPE_LAYER_GCODE, empty_config);
    replaced = event.property<slic3r_api::EPropertyCustomGcode>();
    REQUIRE(replaced != nullptr);
    REQUIRE(replaced->config_id != EXTRUSION_DATA_ID_INVALID);
    slic3r_api::StoredConfig decoded_empty(storage);
    decoded_empty.deserialize_all(event.stored_string(replaced->config_id));
    CHECK(decoded_empty.keys().empty());

    event.script_gcode("M117 plain", GCODE_SCRIPT_TYPE_LAYER_GCODE);
    replaced = event.property<slic3r_api::EPropertyCustomGcode>();
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

TEST_CASE("PrintingPlan scripts cannot replace a previously published file",
          "[plugins][gcode][firmware][custom-gcode]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    select_printing_plan_writer(print);
    PrintingPlan &plan = print.mutable_printing_plan();
    ExtrusionNop script(ExtrusionPropertyCustomGcodeText(
        ExtrusionPropertyCustomGcodeText::Code::SCRIPT,
        "M117 {layer_num}"));
    plan.events.append_before(script);

    const boost::filesystem::path output_path = temporary_gcode_path();
    remove_output_pair(output_path);
    write_text_file(output_path, "previous output\n");
    CHECK_THROWS_AS(
        Steps::StepGenerateGcode::run_step(
            Orchestrator::instance(), print, output_path.string()),
        RuntimeError);
    Orchestrator::instance().reset_plugin_cancel();
    REQUIRE(boost::filesystem::exists(output_path));
    CHECK(read_text_file(output_path) == "previous output\n");
    CHECK_FALSE(boost::filesystem::exists(output_path.string() + ".tmp"));
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
    PrintConfig &config = const_cast<PrintConfig &>(print.config());
    config.start_gcode.value.clear();
    config.end_gcode.value.clear();
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
