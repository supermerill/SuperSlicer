///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "HeaterState.hpp"

#include <stdexcept>

/*
Firmware heater state implementation
====================================

Requests remain uncorrected so inherited extrusion properties can be copied
between devices. Normal and wait encodings use the effective corrected target
because they describe output already generated for the physical machine.
*/

namespace slic3r_api { namespace GCodeGeneration {

void HeaterState::setup(int16_t temperature_offset)
{
    m_temperature_offset = temperature_offset;
}

void HeaterState::reset_runtime_state()
{
    m_temperature.reset_runtime_state();
}

void HeaterState::synchronize_runtime_from(const HeaterState &source)
{
    m_temperature.synchronize_runtime_from(source.m_temperature);
}

int16_t HeaterState::effective_temperature() const
{
    const std::optional<int16_t> requested = m_temperature.requested();
    if (!requested)
        throw std::logic_error("No heater temperature is requested.");
    return int16_t(*requested + m_temperature_offset);
}

bool HeaterState::needs_encoding() const
{
    return m_temperature.requested() &&
           m_temperature.needs_encoding_for(effective_temperature());
}

bool HeaterState::needs_wait_encoding() const
{
    return m_temperature.requested() &&
           m_temperature.needs_wait_encoding_for(effective_temperature());
}

void HeaterState::mark_encoded()
{
    m_temperature.mark_encoded_as(effective_temperature());
}

void HeaterState::mark_encoded_with_wait()
{
    m_temperature.mark_encoded_with_wait_as(effective_temperature());
}

}} // namespace slic3r_api::GCodeGeneration
