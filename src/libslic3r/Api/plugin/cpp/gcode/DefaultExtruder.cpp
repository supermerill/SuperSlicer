///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "DefaultExtruder.hpp"

/*
Standard firmware extruder implementation
=========================================

DefaultExtruder is the configuration boundary for its reusable components. It
reads per-extruder settings once, gives each component explicit values, then
coordinates their runtime reset and synchronization as one logical tool.
*/

namespace slic3r_api { namespace GCodeGeneration {

void DefaultExtruder::setup(const Config &config)
{
    Tool::setup(config);

    // Convert configuration units at the owner boundary so the reusable
    // component classes remain independent of setting names and Config views.
    m_fan.setup(config.vector_percent_or_default("extruder_fan_offset", m_id, 0.0) * 100.0);
    m_heater.setup(int16_t(config.vector_float_or_default(
        "extruder_temperature_offset", m_id, 0.0)));

    ExtrusionAxisSettings axis_settings;
    axis_settings.use_relative_e_distances =
        config.bool_or_default("use_relative_e_distances", false);
    axis_settings.use_volumetric_e = config.bool_or_default("use_volumetric_e", false);
    axis_settings.extrusion_multiplier =
        config.vector_float_or_default("extrusion_multiplier", m_id, 1.0);
    axis_settings.filament_diameter =
        config.vector_float_or_default("filament_diameter", m_id, 0.0);
    axis_settings.retract_speed = config.vector_float_or_default("retract_speed", m_id, 0.0);
    axis_settings.deretract_speed =
        config.vector_float_or_default("deretract_speed", m_id, axis_settings.retract_speed);
    axis_settings.xyz_precision = config.int_or_default("gcode_precision_xyz", 3);
    axis_settings.e_precision = config.int_or_default("gcode_precision_e", 5);
    m_extrusion_axis.setup(axis_settings);

    reset_runtime_state();
}

void DefaultExtruder::reset_runtime_state()
{
    Tool::reset_runtime_state();
    m_fan.reset_runtime_state();
    m_heater.reset_runtime_state();
    m_pressure_advance.reset_runtime_state();
    m_extrusion_axis.reset_runtime_state();
}

void DefaultExtruder::synchronize_runtime_from(const DefaultExtruder &source)
{
    // Runtime history is copied component by component. Identity and every
    // setup-derived correction remain authoritative on the destination tool.
    m_fan.synchronize_runtime_from(source.m_fan);
    m_heater.synchronize_runtime_from(source.m_heater);
    m_pressure_advance.synchronize_runtime_from(source.m_pressure_advance);
    m_extrusion_axis.synchronize_runtime_from(source.m_extrusion_axis);
}

}} // namespace slic3r_api::GCodeGeneration
