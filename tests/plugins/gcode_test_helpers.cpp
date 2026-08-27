#include "gcode_test_helpers.hpp"

#include <catch2/catch.hpp>

#include <fstream>
#include <stdexcept>

#include <boost/filesystem.hpp>

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/host/Plugin.hpp"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_gcode.h"
#include "libslic3r/Api/plugin/cpp/ExtrusionViews.hpp"
#include "libslic3r/Config/ConfigSnapshotSerialization.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Steps/StepGenerateGcode.hpp"

namespace Slic3r::Test::GCode {

FirmwareInstanceOwner::FirmwareInstanceOwner(raw_gcode_firmware_instance instance) :
    m_instance(instance)
{}

FirmwareInstanceOwner::~FirmwareInstanceOwner()
{
    if (m_instance.session != nullptr && m_instance.vtable != nullptr &&
        m_instance.vtable->destroy != nullptr)
        m_instance.vtable->destroy(m_instance.session);
}

ScriptStateFirmwareProbe::ScriptStateFirmwareProbe(
    slic3r_api::GCodeGeneration::GCodeScriptProcessorView scripts,
    storage_handle *storage) :
    DefaultGCodeFirmwareSession(scripts, storage)
{}

const slic3r_api::GCodeGeneration::Gantry &ScriptStateFirmwareProbe::machine_gantry() const
{
    return gantry();
}

slic3r_api::GCodeGeneration::DefaultExtruder &
ScriptStateFirmwareProbe::machine_extruder(size_t idx)
{
    return extruders().at(idx);
}

const slic3r_api::GCodeGeneration::Printer &ScriptStateFirmwareProbe::machine_printer() const
{
    return printer();
}

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
    // Start error-path tests with a valid artifact so publication failures can
    // prove that staging never replaces the previous file.
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

void append_empty_extrusion(Printing::PrintingToolGroup &tool_group,
                            ExtrusionRole role,
                            uint16_t object_instance_idx,
                            const LayerRegionIsland *region_island)
{
    Printing::PrintingExtrusion extrusion;
    extrusion.root = std::make_unique<ExtrusionEntity>(true);
    extrusion.sregion_island_role = role;
    extrusion.object_instance_idx = object_instance_idx;
    extrusion.region_island = region_island;
    tool_group.extrusions.push_back(std::move(extrusion));
}

std::unique_ptr<ExtrusionPath> make_firmware_path(const ArcPolyline &polyline,
                                                  float speed,
                                                  float acceleration,
                                                  float pressure_advance,
                                                  float fan_speed,
                                                  float temperature,
                                                  ExtrusionRole role)
{
    const ExtrusionAttributes attributes(role, ExtrusionFlow(0.08, 0.4f, 0.2f));
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
    // Travels retain their role attributes for classification but use zero
    // flow because they never advance the extrusion axis.
    std::unique_ptr<ExtrusionPath> path(
        new ExtrusionPath(polyline, ExtrusionAttributes(ExtrusionRole::Travel), nullptr, true));
    path->add_property(ExtrusionPropertySpeed(speed, acceleration));
    return path;
}

void append_path_extrusion(Printing::PrintingToolGroup &tool_group,
                           std::unique_ptr<ExtrusionEntity> root)
{
    Printing::PrintingExtrusion extrusion;
    extrusion.root = std::move(root);
    extrusion.sregion_island_role = ExtrusionRole::Perimeter;
    tool_group.extrusions.push_back(std::move(extrusion));
}

void append_multi_tool_process_sequence(Printing::PrintingLayerGroup &layer)
{
    const uint16_t tool_ids[] = {0, 1, 0};
    const float temperatures[] = {200.f, 210.f, 200.f};
    const float fan_speeds[] = {20.f, 30.f, 20.f};
    const float pressure_advances[] = {0.01f, 0.02f, 0.01f};
    for (uint32_t idx = 0; idx < 3; ++idx) {
        layer.tool_groups.emplace_back();
        Printing::PrintingToolGroup &tool_group = layer.tool_groups.back();
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

void configure_standard_firmware(Print &print, uint16_t extruder_count)
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
    // Distinct values expose unit conversions and axis ordering in generated
    // preambles instead of allowing equal defaults to hide mistakes.
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
    // Compare complete lines so a tool suffix such as T0 is not counted as an
    // independent command when it appears on a temperature line.
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

gcode_script_type registered_script_type(Orchestrator &orchestrator, const std::string &name)
{
    const gcode_script_type type = gcode_script_register_type(
        reinterpret_cast<orchestrator_handle *>(&orchestrator), name.c_str());
    if (type == GCODE_SCRIPT_TYPE_INVALID)
        throw std::runtime_error("The test could not register G-code script type '" + name + "'.");
    return type;
}

DynamicConfig stored_script_config(const ExtrusionEntity &entity,
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

int32_t stored_script_int(const ExtrusionEntity &entity,
                          const ExtrusionPropertyCustomGcode &property,
                          const char *key)
{
    DynamicConfig config = stored_script_config(entity, property);
    const ConfigOptionInt *option = config.option<ConfigOptionInt>(key);
    if (option == nullptr)
        throw std::runtime_error(std::string("Missing stored integer option: ") + key);
    return option->value;
}

double stored_script_float(const ExtrusionEntity &entity,
                           const ExtrusionPropertyCustomGcode &property,
                           const char *key)
{
    DynamicConfig config = stored_script_config(entity, property);
    const ConfigOptionFloat *option = config.option<ConfigOptionFloat>(key);
    if (option == nullptr)
        throw std::runtime_error(std::string("Missing stored float option: ") + key);
    return option->value;
}

std::string stored_script_string(const ExtrusionEntity &entity,
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
    double value)
{
    return std::unique_ptr<ExtrusionEntity>(
        new ExtrusionNop(ExtrusionPropertySpecialCommand(code, value)));
}

void select_printing_plan_writer(Print &print, const std::string &firmware_id)
{
    // Tests opt into the PrintingPlan writer through the same printer options
    // used by presets; the legacy generator remains the application default.
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
    // Exercise provider selection, the C ABI session and atomic file writer as
    // one path so dialect tests cannot bypass normal printer configuration.
    select_printing_plan_writer(print, firmware_id);
    const boost::filesystem::path output_path = temporary_gcode_path();
    remove_output_pair(output_path);
    Orchestrator &orchestrator = Orchestrator::instance();
    orchestrator.reset_plugin_cancel();
    try {
        Steps::StepGenerateGcode::run_step(orchestrator, print, output_path.string());
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

    Printing::PrintingPlan &plan = print.mutable_printing_plan();
    run_ctx_generate_gcode payload = {};
    payload.print = reinterpret_cast<const print_handle *>(&print);
    payload.plan = reinterpret_cast<printing_plan_handle *>(&plan);
    payload.firmware = &firmware;

    // Keep path storage alive while the plugin consumes the borrowed C string.
    const std::string output_path_string = output_path.string();
    payload.output_path = output_path_string.c_str();
    plugin_host_context host_context =
        orchestrator.prepare_plugin_host_context(STEP_GCODE, writer, &print);
    plugin_run_context run_context =
        orchestrator.prepare_plugin_run_context(STEP_GCODE, writer, &host_context);
    run_context.data = &payload;

    writer->setup(run_context, 1);
    writer->setup_run(run_context);
    writer->run(run_context);
    if (run_context.is_cancelled != nullptr &&
        run_context.is_cancelled(run_context.host_context)) {
        orchestrator.reset_plugin_cancel();
        throw RuntimeError("Direct PrintingPlan writer test failed.");
    }
}

} // namespace Slic3r::Test::GCode
