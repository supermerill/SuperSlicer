///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "PressureAdvanceState.hpp"

/*
Firmware pressure-advance state implementation
==============================================

Pressure advance has no unit correction or waiting state. This component gives
the shared register protocol a clear domain boundary while keeping command
deduplication and error handling identical to other firmware values.
*/

namespace slic3r_api { namespace GCodeGeneration {

void PressureAdvanceState::reset_runtime_state()
{
    m_value.reset_runtime_state();
}

void PressureAdvanceState::synchronize_runtime_from(const PressureAdvanceState &source)
{
    m_value.synchronize_runtime_from(source.m_value);
}

bool PressureAdvanceState::needs_encoding() const
{
    return m_value.needs_encoding();
}

void PressureAdvanceState::mark_encoded()
{
    m_value.mark_encoded();
}

}} // namespace slic3r_api::GCodeGeneration
