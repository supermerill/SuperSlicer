///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "Gantry.hpp"

/*
Firmware gantry state implementation
====================================

Setup retains the small configuration cache used by every movement. Runtime
operations then compare requested settings with encoded settings so a firmware
session writes only the state transitions required by the next move.
*/

namespace slic3r_api { namespace GCodeGeneration {

void Gantry::setup(const Config &config)
{
    m_travel_speed = config.float_or_default("travel_speed", 0.0);
    m_z_offset = config.float_or_default("z_offset", 0.0);
    reset_runtime_state();
}

void Gantry::reset_runtime_state()
{
    m_position.reset();
    m_speed.reset_runtime_state();
    m_print_acceleration.reset_runtime_state();
    m_travel_acceleration.reset_runtime_state();
}

void Gantry::synchronize_runtime_from(const Gantry &source)
{
    // Copy the complete motion history as one snapshot. Configuration caches
    // are intentionally omitted because they describe this destination
    // machine rather than values already requested or encoded.
    m_position = source.m_position;
    m_speed.synchronize_runtime_from(source.m_speed);
    m_print_acceleration.synchronize_runtime_from(source.m_print_acceleration);
    m_travel_acceleration.synchronize_runtime_from(source.m_travel_acceleration);
}

bool Gantry::needs_speed_encoding() const
{
    return m_speed.needs_encoding();
}

void Gantry::mark_speed_encoded()
{
    m_speed.mark_encoded();
}

bool Gantry::needs_print_acceleration_encoding() const
{
    return m_print_acceleration.needs_encoding();
}

void Gantry::mark_print_acceleration_encoded()
{
    m_print_acceleration.mark_encoded();
}

bool Gantry::needs_travel_acceleration_encoding() const
{
    return m_travel_acceleration.needs_encoding();
}

void Gantry::mark_travel_acceleration_encoded()
{
    m_travel_acceleration.mark_encoded();
}

void Gantry::clear_print_acceleration_encoded()
{
    m_print_acceleration.clear_encoded();
}

void Gantry::clear_travel_acceleration_encoded()
{
    m_travel_acceleration.clear_encoded();
}

}} // namespace slic3r_api::GCodeGeneration
