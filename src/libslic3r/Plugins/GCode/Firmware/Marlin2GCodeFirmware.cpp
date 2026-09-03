///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "Marlin2GCodeFirmware.hpp"

/*
Marlin 2 firmware implementation
================================

`Marlin2GCodeFirmwareSession` is the standard Marlin dialect session used by
the built-in firmware provider. Most traversal and state-transition behavior
comes from the common firmware session; this file supplies Marlin 2 command
encoding and the script fallbacks needed by the host.

The specialization flow is:

    resolve_empty_script()
    |-- emit `M600` for a color-change event
    |-- emit `M0` for a pause, optionally with a sanitized one-line message
    `-- delegate unrelated script types to the common session

    encode_machine_envelope()
    |-- emit axis acceleration limits with `M201`
    |-- emit axis and extruder feedrate limits with `M203`
    `-- emit print/travel acceleration with `M204`

    encode temperature, fan, pressure, and movement state
    `-- format the Marlin 2 command variants while the base session retains
        neutral machine state and extrusion accounting

Pause messages are copied from the script producer Config and flattened to one
line before being inserted into `M0`. The previous encoded temperature target
is kept available where Marlin 2 needs it to decide whether a command should
wait. This class does not traverse the PrintingPlan or select the active
firmware; it is instantiated by the common built-in firmware adapter.
*/

namespace slic3r_api { namespace GCodeGeneration { namespace Firmware {

namespace {

/*
Read the optional pause message supplied by the script producer and make it
safe for Marlin's single-line M0 parameter. Newlines are flattened rather than
allowed to become unintended G-code commands.
*/
std::string marlin_pause_message(const Config *config);

std::string marlin_pause_message(const Config *config)
{
    if (config == nullptr || !config->has("pause_message") ||
        config->get("pause_message").type() != SLIC3R_CONFIG_OPTION_STRING)
        return {};
    std::string message = config->get("pause_message").get_string();
    for (char &character : message)
        if (character == '\r' || character == '\n')
            character = ' ';
    return message;
}

} // namespace

std::string Marlin2GCodeFirmwareSession::resolve_empty_script(
    gcode_script_type script_type,
    const Config *producer_config) const
{
    if (script_type == GCODE_SCRIPT_TYPE_COLOR_CHANGE_GCODE)
        return "M600\n";
    if (script_type == GCODE_SCRIPT_TYPE_PAUSE_PRINT_GCODE) {
        const std::string message = marlin_pause_message(producer_config);
        return message.empty() ? "M0\n" : "M0 " + message + "\n";
    }
    return DefaultGCodeFirmwareSession::resolve_empty_script(script_type, producer_config);
}

std::string Marlin2GCodeFirmwareSession::encode_machine_envelope(
    const MachineEnvelope &envelope) const
{
    std::string output;
    GCodeFormatter formatter(2, 2);

    // Marlin 2 publishes axis acceleration limits as integer parameters.
    formatter.emit_string("M201");
    formatter.emit_integer_parameter('X', envelope.max_acceleration_x);
    formatter.emit_integer_parameter('Y', envelope.max_acceleration_y);
    formatter.emit_integer_parameter('Z', envelope.max_acceleration_z);
    formatter.emit_integer_parameter('E', envelope.max_acceleration_e);
    formatter.emit_comment(true, "sets maximum accelerations, mm/sec^2");
    output += formatter.string();
    formatter.clear();

    formatter.emit_string("M203");
    formatter.emit_integer_parameter('X', envelope.max_feedrate_x);
    formatter.emit_integer_parameter('Y', envelope.max_feedrate_y);
    formatter.emit_integer_parameter('Z', envelope.max_feedrate_z);
    formatter.emit_integer_parameter('E', envelope.max_feedrate_e);
    formatter.emit_comment(true, "sets maximum feedrates, mm/sec");
    output += formatter.string();
    formatter.clear();

    // P, R and T preserve the three independently configured acceleration
    // limits exposed by Marlin 2.
    formatter.emit_string("M204");
    formatter.emit_integer_parameter('P', envelope.max_print_acceleration);
    formatter.emit_integer_parameter('R', envelope.max_retract_acceleration);
    formatter.emit_integer_parameter('T', envelope.max_travel_acceleration);
    formatter.emit_comment(true, "sets print, retract and travel acceleration, mm/sec^2");
    output += formatter.string();
    formatter.clear();

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

void Marlin2GCodeFirmwareSession::synchronize_selected_extruder_state(
    const DefaultExtruder *previous,
    DefaultExtruder &selected)
{
    // M106 addresses the one part-cooling fan used by this dialect, while the
    // T parameter keeps each extruder heater history independent.
    if (previous != nullptr)
        selected.fan().synchronize_runtime_from(previous->fan());
}

std::string Marlin2GCodeFirmwareSession::encode_tool_temperature(
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

}}} // namespace slic3r_api::GCodeGeneration::Firmware
