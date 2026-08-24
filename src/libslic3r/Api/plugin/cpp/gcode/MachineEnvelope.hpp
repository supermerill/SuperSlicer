///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_gcode_MachineEnvelope_hpp_
#define slic3r_Api_plugin_cpp_gcode_MachineEnvelope_hpp_

#include <optional>

/*
Firmware-neutral machine envelope
=================================

MachineEnvelope copies the printer limits that a firmware may publish at the
start of a G-code file. It contains no command syntax, unit conversion or
dialect decision. Firmware sessions choose which values they support and how
to encode them.

configured_machine_envelope() is the shared boundary between printer Config
and firmware implementations. It returns no value when the preset does not
request machine-limit commands and validates every copied limit before making
the envelope available to an encoder.
*/

namespace slic3r_api {

class Config;

namespace GCodeGeneration {

struct MachineEnvelope
{
    double max_acceleration_x = 0.0;
    double max_acceleration_y = 0.0;
    double max_acceleration_z = 0.0;
    double max_acceleration_e = 0.0;
    double max_feedrate_x = 0.0;
    double max_feedrate_y = 0.0;
    double max_feedrate_z = 0.0;
    double max_feedrate_e = 0.0;
    double max_print_acceleration = 0.0;
    double max_retract_acceleration = 0.0;
    double max_travel_acceleration = 0.0;
    double max_jerk_x = 0.0;
    double max_jerk_y = 0.0;
    double max_jerk_z = 0.0;
    double max_jerk_e = 0.0;
    double min_travel_feedrate = 0.0;
    double min_extruding_feedrate = 0.0;
};

std::optional<MachineEnvelope> configured_machine_envelope(const Config &config);

}} // namespace slic3r_api::GCodeGeneration

#endif // slic3r_Api_plugin_cpp_gcode_MachineEnvelope_hpp_
