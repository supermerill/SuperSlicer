///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "Printer.hpp"

/*
Firmware printer state implementation
=====================================

Printer coordinates reusable device components. It gives machine-wide devices
neutral offsets and delegates runtime reset and synchronization to them.
*/

namespace slic3r_api { namespace GCodeGeneration {

void Printer::setup(const Config &config)
{
    // Bed and chamber devices have no correction setting in the current public
    // model. Explicit zero setup keeps their configuration boundary visible.
    static_cast<void>(config);
    m_bed_heater.setup(0);
    m_chamber_heater.setup(0);
    m_chamber_fan.setup(0.0);
    reset_runtime_state();
}

void Printer::reset_runtime_state()
{
    m_bed_heater.reset_runtime_state();
    m_chamber_heater.reset_runtime_state();
    m_chamber_fan.reset_runtime_state();
    m_preview_enabled = true;
}

void Printer::synchronize_runtime_from(const Printer &source)
{
    // Copy each runtime component while retaining this Printer's setup values.
    m_bed_heater.synchronize_runtime_from(source.m_bed_heater);
    m_chamber_heater.synchronize_runtime_from(source.m_chamber_heater);
    m_chamber_fan.synchronize_runtime_from(source.m_chamber_fan);
    m_preview_enabled = source.m_preview_enabled;
}

}} // namespace slic3r_api::GCodeGeneration
