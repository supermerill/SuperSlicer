///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "FanState.hpp"

#include <algorithm>
#include <stdexcept>

/*
Firmware fan state implementation
=================================

The configured offset is applied only when a command value is evaluated.
Runtime synchronization therefore copies request/output history while keeping
the destination fan's own setup-derived correction.
*/

namespace slic3r_api { namespace GCodeGeneration {

void FanState::setup(double speed_offset_percent)
{
    m_speed_offset_percent = speed_offset_percent;
}

void FanState::reset_runtime_state()
{
    m_speed.reset_runtime_state();
}

void FanState::synchronize_runtime_from(const FanState &source)
{
    m_speed.synchronize_runtime_from(source.m_speed);
}

double FanState::effective_speed_percent() const
{
    const std::optional<double> requested = m_speed.requested();
    if (!requested)
        throw std::logic_error("No fan speed is requested.");
    return std::clamp(*requested + m_speed_offset_percent, 0.0, 100.0);
}

bool FanState::needs_encoding() const
{
    return m_speed.requested() && m_speed.needs_encoding_for(effective_speed_percent());
}

void FanState::mark_encoded()
{
    m_speed.mark_encoded_as(effective_speed_percent());
}

}} // namespace slic3r_api::GCodeGeneration
