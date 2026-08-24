///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "Marlin1GCodeFirmware.hpp"

/*
Marlin 1 firmware implementation
================================

Only commands whose syntax differs from the standard Marlin 2 session are
implemented here. Traversal, E accounting and machine state remain inherited.
*/

namespace slic3r_api { namespace GCodeGeneration { namespace Firmware {

std::string Marlin1GCodeFirmwareSession::encode_machine_envelope(
    const MachineEnvelope &envelope) const
{
    std::string output;
    GCodeFormatter formatter(2, 2);

    // Marlin 1 publishes axis acceleration limits as integer parameters.
    formatter.emit_string("M201");
    formatter.emit_integer_parameter('X', envelope.max_acceleration_x);
    formatter.emit_integer_parameter('Y', envelope.max_acceleration_y);
    formatter.emit_integer_parameter('Z', envelope.max_acceleration_z);
    formatter.emit_integer_parameter('E', envelope.max_acceleration_e);
    formatter.emit_comment(true, "sets maximum accelerations, mm/sec^2");
    output += formatter.string();
    formatter.clear();

    // Feedrate limits remain in mm/sec for this dialect.
    formatter.emit_string("M203");
    formatter.emit_integer_parameter('X', envelope.max_feedrate_x);
    formatter.emit_integer_parameter('Y', envelope.max_feedrate_y);
    formatter.emit_integer_parameter('Z', envelope.max_feedrate_z);
    formatter.emit_integer_parameter('E', envelope.max_feedrate_e);
    formatter.emit_comment(true, "sets maximum feedrates, mm/sec");
    output += formatter.string();
    formatter.clear();

    // M204 S configures the shared print acceleration, while T retains the
    // legacy retract acceleration field.
    formatter.emit_string("M204");
    formatter.emit_integer_parameter('S', envelope.max_print_acceleration);
    formatter.emit_integer_parameter('T', envelope.max_retract_acceleration);
    formatter.emit_comment(true, "sets print and retract acceleration, mm/sec^2");
    output += formatter.string();
    formatter.clear();

    // Fixed precision keeps small jerk limits visible in the preamble.
    formatter.emit_string("M205");
    formatter.emit_fixed_parameter('X', envelope.max_jerk_x, 2);
    formatter.emit_fixed_parameter('Y', envelope.max_jerk_y, 2);
    formatter.emit_fixed_parameter('Z', envelope.max_jerk_z, 2);
    formatter.emit_fixed_parameter('E', envelope.max_jerk_e, 2);
    formatter.emit_comment(true, "sets the jerk limits, mm/sec");
    output += formatter.string();
    formatter.clear();

    formatter.emit_string("M205");
    formatter.emit_integer_parameter('S', envelope.min_extruding_feedrate);
    formatter.emit_integer_parameter('T', envelope.min_travel_feedrate);
    formatter.emit_comment(true, "sets the minimum extruding and travel feed rate, mm/sec");
    output += formatter.string();
    return output;
}

void Marlin1GCodeFirmwareSession::synchronize_selected_extruder_state(
    const DefaultExtruder *previous,
    DefaultExtruder &selected)
{
    // M106 addresses the one part-cooling fan used by this dialect, while the
    // T parameter keeps each extruder heater history independent.
    if (previous != nullptr)
        selected.fan().synchronize_runtime_from(previous->fan());
}

std::string Marlin1GCodeFirmwareSession::encode_tool_temperature(
    uint16_t tool_id,
    int16_t temperature,
    bool wait) const
{
    const std::optional<int16_t> encoded = tool_id < extruders().size() ?
        extruders()[tool_id].heater().encoded_temperature() : std::optional<int16_t>();
    // [2026-08-25] Marlin M109 with S does not wait while cooling. R waits in
    // both directions: https://marlinfw.org/docs/gcode/M109.html
    const bool cooling_wait = wait && encoded && temperature < *encoded;
    return std::string(wait ? "M109" : "M104") +
           (cooling_wait ? " R" : " S") + std::to_string(temperature) +
           " T" + std::to_string(tool_id) + "\n";
}

std::string Marlin1GCodeFirmwareSession::encode_acceleration(uint32_t acceleration, bool) const
{
    // [2026-08-25] Marlin's legacy M204 S parameter controls the one default
    // acceleration shared by movement categories:
    // https://marlinfw.org/docs/gcode/M204.html
    return "M204 S" + std::to_string(acceleration) + "\n";
}

}}} // namespace slic3r_api::GCodeGeneration::Firmware
