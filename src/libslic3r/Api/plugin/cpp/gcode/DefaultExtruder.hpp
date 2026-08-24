///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_gcode_DefaultExtruder_hpp_
#define slic3r_Api_plugin_cpp_gcode_DefaultExtruder_hpp_

#include "ExtrusionAxisState.hpp"
#include "FanState.hpp"
#include "HeaterState.hpp"
#include "PressureAdvanceState.hpp"
#include "Tool.hpp"

/*
Standard firmware extruder composition
======================================

DefaultExtruder is an identified machine tool composed from independent
firmware states. It owns one fan, heater, pressure-advance register and E axis,
but does not inherit from those capabilities. This makes the physical model
visible and lets Printer reuse the same heater and fan components.

The standard firmware may copy heater and fan runtime state between logical
extruders when they refer to shared hardware. E-axis and pressure-advance state
always remain attached to their logical extruder.
*/

namespace slic3r_api { namespace GCodeGeneration {

class DefaultGCodeFirmwareSession;

class DefaultExtruder : public Tool
{
public:
    explicit DefaultExtruder(uint16_t id) : Tool(id) {}

    void setup(const Config &config) override;
    void reset_runtime_state() override;
    void synchronize_runtime_from(const DefaultExtruder &source);

    FanState &fan() { return m_fan; }
    const FanState &fan() const { return m_fan; }
    HeaterState &heater() { return m_heater; }
    const HeaterState &heater() const { return m_heater; }
    PressureAdvanceState &pressure_advance() { return m_pressure_advance; }
    const PressureAdvanceState &pressure_advance() const { return m_pressure_advance; }
    ExtrusionAxisState &extrusion_axis() { return m_extrusion_axis; }
    const ExtrusionAxisState &extrusion_axis() const { return m_extrusion_axis; }

private:
    friend class DefaultGCodeFirmwareSession;

    FanState m_fan;
    HeaterState m_heater;
    PressureAdvanceState m_pressure_advance;
    ExtrusionAxisState m_extrusion_axis;
};

}} // namespace slic3r_api::GCodeGeneration

#endif // slic3r_Api_plugin_cpp_gcode_DefaultExtruder_hpp_
