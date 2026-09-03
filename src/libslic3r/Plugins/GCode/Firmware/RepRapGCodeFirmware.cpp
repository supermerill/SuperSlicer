///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "RepRapGCodeFirmware.hpp"

#include <algorithm>

/*
RepRapFirmware implementation
=============================

`RepRapGCodeFirmwareSession` keeps the common PrintingPlan traversal and
machine-state bookkeeping, then translates the resulting state changes to
RepRapFirmware's command vocabulary.

The specialization flow is:

    resolve_empty_script()
    |-- emit `M600` for a color-change event
    |-- emit `M226` for a pause-print event
    `-- delegate other script types to the common session

    encode_machine_envelope()
    |-- emit acceleration limits with `M201`
    |-- convert feedrates and minimum extruding feedrate to mm/min for `M203`
    |-- emit print/travel acceleration with `M204`
    `-- emit jerk limits with `M566`, also in mm/min

    encode fan and pressure-advance changes
    `-- use RepRapFirmware's tool-oriented command parameters

The neutral `MachineEnvelope` remains in mm/sec-based units until this class
performs the dialect-boundary conversion. State transitions, extrusion values,
and script processing stay in the inherited session. This class only supplies
RepRapFirmware encodings and is instantiated by the common firmware provider.
*/

namespace slic3r_api { namespace GCodeGeneration { namespace Firmware {

std::string RepRapGCodeFirmwareSession::resolve_empty_script(
    gcode_script_type script_type,
    const Config *producer_config) const
{
    if (script_type == GCODE_SCRIPT_TYPE_COLOR_CHANGE_GCODE)
        return "M600\n";
    if (script_type == GCODE_SCRIPT_TYPE_PAUSE_PRINT_GCODE)
        return "M226\n";
    return DefaultGCodeFirmwareSession::resolve_empty_script(script_type, producer_config);
}

std::string RepRapGCodeFirmwareSession::encode_machine_envelope(
    const MachineEnvelope &envelope) const
{
    // RepRapFirmware expresses feedrate and jerk envelope values in mm/min,
    // while acceleration values remain in mm/sec^2.
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

    // Convert feedrates only at the dialect boundary; MachineEnvelope remains
    // expressed in its neutral mm/sec units.
    formatter.emit_string("M203");
    formatter.emit_integer_parameter('X', envelope.max_feedrate_x * unit_factor);
    formatter.emit_integer_parameter('Y', envelope.max_feedrate_y * unit_factor);
    formatter.emit_integer_parameter('Z', envelope.max_feedrate_z * unit_factor);
    formatter.emit_integer_parameter('E', envelope.max_feedrate_e * unit_factor);
    formatter.emit_integer_parameter('I', envelope.min_extruding_feedrate * unit_factor);
    formatter.emit_comment(true, "sets maximum feedrates, mm/min");
    output += formatter.string();
    formatter.clear();

    formatter.emit_string("M204");
    formatter.emit_integer_parameter('P', envelope.max_print_acceleration);
    formatter.emit_integer_parameter('T', envelope.max_travel_acceleration);
    formatter.emit_comment(true, "sets print and travel acceleration, mm/sec^2");
    output += formatter.string();
    formatter.clear();

    formatter.emit_string("M566");
    formatter.emit_fixed_parameter('X', envelope.max_jerk_x * unit_factor, 2);
    formatter.emit_fixed_parameter('Y', envelope.max_jerk_y * unit_factor, 2);
    formatter.emit_fixed_parameter('Z', envelope.max_jerk_z * unit_factor, 2);
    formatter.emit_fixed_parameter('E', envelope.max_jerk_e * unit_factor, 2);
    formatter.emit_comment(true, "sets the jerk limits, mm/min");
    output += formatter.string();
    return output;
}

std::string RepRapGCodeFirmwareSession::encode_tool_temperature(
    uint16_t tool_id,
    int16_t temperature,
    bool wait) const
{
    // [2026-08-25] RepRapFirmware uses G10 to set a tool temperature and M116
    // to wait: https://docs.duet3d.com/User_manual/Reference/Gcodes
    std::string output;
    const std::optional<int16_t> encoded = tool_id < extruders().size() ?
        extruders()[tool_id].heater().encoded_temperature() : std::optional<int16_t>();
    if (!wait || !encoded || *encoded != temperature)
        output = "G10 P" + std::to_string(tool_id) + " S" + std::to_string(temperature) + "\n";
    if (wait)
        output += "M116 P" + std::to_string(tool_id) + "\n";
    return output;
}

std::string RepRapGCodeFirmwareSession::encode_fan(uint16_t tool_id,
                                                    double speed_percent) const
{
    const double normalized = std::clamp(speed_percent, 0.0, 100.0) / 100.0;
    return "M106 P" + std::to_string(tool_id) + " S" + format_number(normalized) + "\n";
}

std::string RepRapGCodeFirmwareSession::encode_pressure_advance(
    uint16_t tool_id,
    double pressure_advance) const
{
    // [2026-08-25] RepRapFirmware assigns pressure advance per drive with
    // M572: https://docs.duet3d.com/User_manual/Reference/Gcodes
    return "M572 D" + std::to_string(tool_id) +
           " S" + format_number(pressure_advance) + "\n";
}

}}} // namespace slic3r_api::GCodeGeneration::Firmware
