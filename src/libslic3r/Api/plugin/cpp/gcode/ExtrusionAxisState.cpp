///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/ Copyright (c) Prusa Research 2017 - 2023 Vojtech Bubnik @bubnikv, Lukas Matena @lukasmatena
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "ExtrusionAxisState.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "libslic3r/Api/plugin/c/slic3r_def.h"

/*
Firmware extrusion-axis state implementation
============================================

The extrusion state accumulates exact requested distances and retains the
sub-precision difference left by formatting. Firmware code receives only a
quantized number to display; it never has to reconstruct or commit physical E
state itself.
*/

namespace slic3r_api { namespace GCodeGeneration {

void ExtrusionAxisState::setup(const ExtrusionAxisSettings &settings)
{
    m_configured_use_relative_e_distances = settings.use_relative_e_distances;
    m_use_relative_e_distances = m_configured_use_relative_e_distances;
    m_use_volumetric_e = settings.use_volumetric_e;
    m_filament_diameter = settings.filament_diameter;
    m_retract_speed = settings.retract_speed;
    m_deretract_speed = settings.deretract_speed;

    // Precision is validated again here because this reusable component may
    // be configured by a firmware without going through DefaultExtruder.
    const int32_t max_precision = int32_t(GCodeFormatter::pow_10.size()) - 1;
    const int32_t xyz_precision = std::clamp(int32_t(settings.xyz_precision), 0, max_precision);
    const int32_t e_precision = std::clamp(int32_t(settings.e_precision), 0, max_precision);
    m_formatter = GCodeFormatter(uint16_t(xyz_precision), uint16_t(e_precision));

    // mm3_per_mm becomes either filament millimetres per path millimetre or
    // volumetric E per path millimetre, depending on the configured E mode.
    m_e_per_mm3 = settings.extrusion_multiplier;
    if (!m_use_volumetric_e) {
        if (m_filament_diameter <= 0.0)
            throw std::invalid_argument("A positive filament diameter is required for non-volumetric extrusion.");
        m_e_per_mm3 /= filament_crossection();
    }
}

void ExtrusionAxisState::reset_runtime_state()
{
    m_use_relative_e_distances = m_configured_use_relative_e_distances;
    m_E = 0.0;
    m_machine_E = 0.0;
    m_dE_left = 0.0;
    m_absolute_E = 0.0;
    m_retracted = 0.0;
    m_restart_extra = 0.0;
    m_restart_extra_toolchange = 0.0;
}

void ExtrusionAxisState::set_relative_mode(bool relative_mode)
{
    if (m_use_relative_e_distances == relative_mode)
        return;
    m_use_relative_e_distances = relative_mode;
    // The formatting remainder belongs to the previous addressing mode.
    // Relative output starts from a local zero. Absolute output resumes from
    // the logical E coordinate retained by the printer across M82/M83.
    m_E = relative_mode ? 0.0 : m_machine_E;
    m_dE_left = 0.0;
}

void ExtrusionAxisState::observe_external_move(double e_value, bool relative_mode)
{
    if (!std::isfinite(e_value))
        throw std::invalid_argument("External G-code contains a non-finite E value.");
    set_relative_mode(relative_mode);
    const double delta = relative_mode ? e_value : e_value - m_machine_E;
    (void)extrude(delta);
    // Text already contains the exact displayed value. No rounding remainder
    // may be carried into the next host-generated command.
    m_dE_left = 0.0;
}

void ExtrusionAxisState::synchronize_runtime_from(const ExtrusionAxisState &source)
{
    // Transfer the exact physical and accounting snapshot. Conversion and
    // formatting caches remain those configured for the destination tool.
    m_E = source.m_E;
    m_machine_E = source.m_machine_E;
    m_dE_left = source.m_dE_left;
    m_absolute_E = source.m_absolute_E;
    m_retracted = source.m_retracted;
    m_restart_extra = source.m_restart_extra;
    m_restart_extra_toolchange = source.m_restart_extra_toolchange;
}

std::optional<double> ExtrusionAxisState::extrude(double delta_e)
{
    if (!std::isfinite(delta_e) || std::abs(delta_e) >= std::numeric_limits<int32_t>::max())
        throw std::invalid_argument("The requested extrusion delta is invalid.");

    double previous_number = 0.0;
    double extrusion_number = 0.0;
    if (m_use_relative_e_distances) {
        // Relative commands emit only this call's visible delta. Carry the
        // truncated part forward so repeated small requests are not lost.
        const double accumulated = m_dE_left + delta_e;
        extrusion_number = m_formatter.quantize_e(accumulated);
        m_dE_left = accumulated - extrusion_number;
        m_E = 0.0;
    } else {
        // The exact position remains authoritative. The last displayed value
        // is always derivable from the exact position minus its remainder.
        previous_number = m_E - m_dE_left;
        m_E += delta_e;
        extrusion_number = m_formatter.quantize_e(m_E);
        m_dE_left = m_E - extrusion_number;
    }

    // Physical statistics and retraction follow the requested distance even
    // when no visible E word is produced at the configured precision.
    m_machine_E += delta_e;
    m_absolute_E += delta_e;
    if (delta_e < 0.0)
        m_retracted -= delta_e;
    else
        m_retracted = std::max(0.0, m_retracted - delta_e);

    return extrusion_number != previous_number ?
        std::optional<double>(extrusion_number) : std::optional<double>();
}

double ExtrusionAxisState::retract(double retract_length,
                             std::optional<double> restart_extra,
                             std::optional<double> restart_extra_from_toolchange)
{
    if (!std::isfinite(retract_length) || retract_length < 0.0)
        throw std::invalid_argument("The requested retraction length is invalid.");

    const double amount = retract_to_go(retract_length);
    const std::optional<double> extrusion_number = extrude(-amount);
    if (restart_extra)
        m_restart_extra = *restart_extra;
    if (restart_extra_from_toolchange)
        m_restart_extra_toolchange = *restart_extra_from_toolchange;
    return extrusion_number.value_or(0.0);
}

double ExtrusionAxisState::unretract()
{
    const std::optional<double> extrusion_number =
        extrude(m_retracted + m_restart_extra + m_restart_extra_toolchange);
    m_retracted = 0.0;
    m_restart_extra = 0.0;
    m_restart_extra_toolchange = 0.0;
    return extrusion_number.value_or(0.0);
}

void ExtrusionAxisState::reset_retract()
{
    m_retracted = 0.0;
    m_restart_extra = 0.0;
    m_restart_extra_toolchange = 0.0;
}

bool ExtrusionAxisState::need_unretract() const
{
    return m_retracted + m_restart_extra + m_restart_extra_toolchange != 0.0;
}

double ExtrusionAxisState::retract_to_go(double retract_length) const
{
    return std::max(0.0, m_formatter.quantize_e(retract_length - m_retracted));
}

bool ExtrusionAxisState::reset_E()
{
    const bool modified = m_machine_E != 0.0;
    m_E = 0.0;
    m_machine_E = 0.0;
    m_dE_left = 0.0;
    return modified;
}

void ExtrusionAxisState::set_position(double e)
{
    if (!std::isfinite(e))
        throw std::invalid_argument("The extrusion position must be finite.");
    m_machine_E = e;
    m_E = m_use_relative_e_distances ? 0.0 : e;
    m_dE_left = 0.0;
}

void ExtrusionAxisState::set_retracted(double retracted, double restart_extra)
{
    if (retracted < -EPSILON || restart_extra < -EPSILON)
        throw std::invalid_argument("Retraction state cannot be negative.");
    m_retracted = retracted > EPSILON ? retracted : 0.0;
    m_restart_extra = m_retracted > 0.0 && restart_extra > EPSILON ? restart_extra : 0.0;
}

bool ExtrusionAxisState::synchronize_after_external_gcode(
    std::optional<double> e_position,
    double retracted,
    double restart_extra)
{
    if ((e_position && !std::isfinite(*e_position)) || !std::isfinite(retracted) ||
        !std::isfinite(restart_extra))
        throw std::invalid_argument("External G-code returned a non-finite extrusion state.");
    if (retracted < -EPSILON || restart_extra < -EPSILON)
        throw std::invalid_argument("External G-code returned a negative retraction state.");

    const double normalized_retracted = retracted > EPSILON ? retracted : 0.0;
    const double normalized_restart =
        normalized_retracted > 0.0 && restart_extra > EPSILON ? restart_extra : 0.0;
    const bool changed = (e_position && m_E != *e_position) ||
        m_retracted != normalized_retracted || m_restart_extra != normalized_restart;
    if (!changed)
        return false;

    // External state becomes authoritative only when one of its outputs changed.
    // Preserve usage statistics, but discard the old quantization remainder.
    if (e_position)
        set_position(*e_position);
    m_retracted = normalized_retracted;
    m_restart_extra = normalized_restart;
    m_dE_left = 0.0;
    return true;
}

double ExtrusionAxisState::filament_crossection() const
{
    return m_filament_diameter * m_filament_diameter * 0.25 * PI;
}

double ExtrusionAxisState::extruded_volume() const
{
    return m_use_volumetric_e ? m_absolute_E + m_retracted : used_filament() * filament_crossection();
}

double ExtrusionAxisState::used_filament() const
{
    return m_use_volumetric_e ? extruded_volume() / filament_crossection() : m_absolute_E + m_retracted;
}

}} // namespace slic3r_api::GCodeGeneration
