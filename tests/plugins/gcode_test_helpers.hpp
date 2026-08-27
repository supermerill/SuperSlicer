#ifndef test_plugins_gcode_test_helpers_hpp_
#define test_plugins_gcode_test_helpers_hpp_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <boost/filesystem/path.hpp>

#include "libslic3r/Api/plugin/cpp/PrintingPlanViews.hpp"
#include "libslic3r/Api/plugin/cpp/gcode/DefaultGCodeFirmwareSession.hpp"
#include "libslic3r/Config/PrintConfig.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Printing/PrintingPlan.hpp"

/*
Shared G-code plugin test fixtures
==================================

The G-code test files exercise separate production responsibilities, but they
all need the same small plans, temporary output files and firmware execution
path. This module keeps those neutral fixtures in one place so each test file
contains only the probes and assertions specific to the component it covers.
*/

namespace Slic3r {

class LayerRegionIsland;
class Orchestrator;
class Print;

namespace Test::GCode {

// Owns one raw firmware instance and invokes its ABI destructor exactly once.
class FirmwareInstanceOwner
{
public:
    explicit FirmwareInstanceOwner(raw_gcode_firmware_instance instance);
    ~FirmwareInstanceOwner();

    FirmwareInstanceOwner(const FirmwareInstanceOwner &) = delete;
    FirmwareInstanceOwner &operator=(const FirmwareInstanceOwner &) = delete;

    raw_gcode_firmware_instance &instance() { return m_instance; }

private:
    raw_gcode_firmware_instance m_instance = {};
};

// Exposes the script bridge and resulting machine state to processor tests.
class ScriptStateFirmwareProbe final :
    public slic3r_api::GCodeGeneration::DefaultGCodeFirmwareSession
{
public:
    ScriptStateFirmwareProbe() = default;
    explicit ScriptStateFirmwareProbe(
        slic3r_api::GCodeGeneration::GCodeScriptProcessorView scripts,
        storage_handle *storage = nullptr);

    using DefaultGCodeFirmwareSession::process_script;

    const slic3r_api::GCodeGeneration::Gantry &machine_gantry() const;
    slic3r_api::GCodeGeneration::DefaultExtruder &machine_extruder(size_t idx);
    const slic3r_api::GCodeGeneration::Printer &machine_printer() const;
};

boost::filesystem::path temporary_gcode_path();
std::string read_text_file(const boost::filesystem::path &path);
void write_text_file(const boost::filesystem::path &path, const std::string &contents);
void remove_output_pair(const boost::filesystem::path &path);

void append_empty_extrusion(
    Printing::PrintingToolGroup &tool_group,
    ExtrusionRole role = ExtrusionRole::None,
    uint16_t object_instance_idx = 0,
    const LayerRegionIsland *region_island = nullptr);
std::unique_ptr<ExtrusionPath> make_firmware_path(
    const ArcPolyline &polyline,
    float speed,
    float acceleration,
    float pressure_advance = -1.f,
    float fan_speed = -1.f,
    float temperature = -1.f,
    ExtrusionRole role = ExtrusionRole::Perimeter);
std::unique_ptr<ExtrusionPath> make_firmware_travel(
    const ArcPolyline &polyline,
    float speed,
    float acceleration);
void append_path_extrusion(
    Printing::PrintingToolGroup &tool_group,
    std::unique_ptr<ExtrusionEntity> root);
void append_multi_tool_process_sequence(Printing::PrintingLayerGroup &layer);

void configure_standard_firmware(Print &print, uint16_t extruder_count = 1);
void configure_machine_envelope(Print &print);
size_t count_occurrences(const std::string &text, const std::string &needle);
size_t count_command_lines(const std::string &text, const std::string &command);

gcode_script_type registered_script_type(Orchestrator &orchestrator, const std::string &name);
DynamicConfig stored_script_config(
    const ExtrusionEntity &entity,
    const ExtrusionPropertyCustomGcode &property);
int32_t stored_script_int(
    const ExtrusionEntity &entity,
    const ExtrusionPropertyCustomGcode &property,
    const char *key);
double stored_script_float(
    const ExtrusionEntity &entity,
    const ExtrusionPropertyCustomGcode &property,
    const char *key);
std::string stored_script_string(
    const ExtrusionEntity &entity,
    const ExtrusionPropertyCustomGcode &property,
    const char *key);

std::unique_ptr<ExtrusionEntity> special_command_entity(
    ExtrusionPropertySpecialCommand::Code code,
    double value = 0.0);
void select_printing_plan_writer(
    Print &print,
    const std::string &firmware_id = "gcode.firmware.marlin2");
slic3r_api::PrintingPlan printing_plan_view(Print &print);
std::string export_with_firmware(Print &print, const std::string &firmware_id);
void run_file_writer_with_firmware(
    Print &print,
    const boost::filesystem::path &output_path,
    const raw_gcode_firmware_instance &firmware);

} // namespace Test::GCode
} // namespace Slic3r

#endif // test_plugins_gcode_test_helpers_hpp_
