///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_gcode_Printer_hpp_
#define slic3r_Api_plugin_cpp_gcode_Printer_hpp_

#include "../ConfigViews.hpp"
#include "FanState.hpp"
#include "HeaterState.hpp"

/*
Firmware printer state
======================

Printer composes machine-wide states that do not belong to one tool or to the
gantry. The same HeaterState and FanState components used by an extruder are
reused for the bed, chamber and chamber fan. Preview remains software-only.

The object does not choose a firmware command. It only exposes generic state
transitions for a firmware session to encode.
*/

namespace slic3r_api { namespace GCodeGeneration {

class Printer
{
public:
    void setup(const Config &config);
    void reset_runtime_state();
    // Replace all machine-wide runtime registers with another printer state.
    // Configuration cached by this instance remains authoritative.
    void synchronize_runtime_from(const Printer &source);

    HeaterState &bed_heater() { return m_bed_heater; }
    const HeaterState &bed_heater() const { return m_bed_heater; }
    HeaterState &chamber_heater() { return m_chamber_heater; }
    const HeaterState &chamber_heater() const { return m_chamber_heater; }
    FanState &chamber_fan() { return m_chamber_fan; }
    const FanState &chamber_fan() const { return m_chamber_fan; }

    bool preview_enabled() const { return m_preview_enabled; }
    void set_preview_enabled(bool enabled) { m_preview_enabled = enabled; }

private:
    // --- Composed machine-wide device states ---
    HeaterState m_bed_heater;
    HeaterState m_chamber_heater;
    FanState m_chamber_fan;

    // --- Runtime software-only state ---
    // Preview markers change this software-only state without machine output.
    bool m_preview_enabled = true;
};

}} // namespace slic3r_api::GCodeGeneration

#endif // slic3r_Api_plugin_cpp_gcode_Printer_hpp_
