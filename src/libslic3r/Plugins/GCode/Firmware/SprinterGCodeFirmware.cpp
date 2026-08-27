///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "SprinterGCodeFirmware.hpp"

/*
Sprinter firmware implementation
================================

The implementation preserves the established SuperSlicer Sprinter command
choices while mapping the two generic acceleration histories to one command.
*/

namespace slic3r_api { namespace GCodeGeneration { namespace Firmware {

std::string SprinterGCodeFirmwareSession::resolve_empty_script(
    gcode_script_type script_type,
    const Config *producer_config) const
{
    if (script_type == GCODE_SCRIPT_TYPE_COLOR_CHANGE_GCODE)
        return encode_unsupported_operation("color change");
    if (script_type == GCODE_SCRIPT_TYPE_PAUSE_PRINT_GCODE)
        return encode_unsupported_operation("pause print");
    return SingleAccelerationRegisterGCodeFirmwareSession::resolve_empty_script(
        script_type, producer_config);
}

std::string SprinterGCodeFirmwareSession::encode_machine_envelope(
    const MachineEnvelope &envelope) const
{
    // Sprinter uses mm/min for M203 and has no matching envelope command for
    // the remaining jerk and minimum-feedrate settings.
    const double unit_factor = 60.0;
    std::string output;
    GCodeFormatter formatter(2, 2);

    formatter.emit_string("M201");
    formatter.emit_integer_parameter('X', envelope.max_acceleration_x);
    formatter.emit_integer_parameter('Y', envelope.max_acceleration_y);
    formatter.emit_integer_parameter('Z', envelope.max_acceleration_z);
    formatter.emit_integer_parameter('E', envelope.max_acceleration_e);
    formatter.emit_comment(true, "sets maximum accelerations, mm/sec^2");
    output += formatter.string();
    formatter.clear();

    formatter.emit_string("M203");
    formatter.emit_integer_parameter('X', envelope.max_feedrate_x * unit_factor);
    formatter.emit_integer_parameter('Y', envelope.max_feedrate_y * unit_factor);
    formatter.emit_integer_parameter('Z', envelope.max_feedrate_z * unit_factor);
    formatter.emit_integer_parameter('E', envelope.max_feedrate_e * unit_factor);
    formatter.emit_comment(true, "sets maximum feedrates, mm/min");
    output += formatter.string();
    formatter.clear();

    formatter.emit_string("M204");
    formatter.emit_integer_parameter('P', envelope.max_print_acceleration);
    formatter.emit_integer_parameter('T', envelope.max_travel_acceleration);
    formatter.emit_comment(true, "sets print and travel acceleration, mm/sec^2");
    output += formatter.string();
    return output;
}

void SprinterGCodeFirmwareSession::synchronize_selected_extruder_state(
    const DefaultExtruder *previous,
    DefaultExtruder &selected)
{
    // Sprinter's M106 command controls one shared part-cooling fan. Its T
    // parameter still keeps temperature encoding history local to each tool.
    if (previous != nullptr)
        selected.fan().synchronize_runtime_from(previous->fan());
}

std::string SprinterGCodeFirmwareSession::encode_chamber_temperature(int16_t, bool) const
{
    return encode_unsupported_operation("chamber temperature");
}

std::string SprinterGCodeFirmwareSession::encode_pressure_advance(
    uint16_t tool_id,
    double pressure_advance) const
{
    return "M572 D" + std::to_string(tool_id) +
           " S" + format_number(pressure_advance) + "\n";
}

std::string SprinterGCodeFirmwareSession::encode_acceleration(uint32_t acceleration, bool) const
{
    return "M204 P" + std::to_string(acceleration) + "\n";
}

std::string SprinterGCodeFirmwareSession::encode_extruder_current(uint16_t, double) const
{
    return encode_unsupported_operation("extruder current");
}

}}} // namespace slic3r_api::GCodeGeneration::Firmware
