///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/ Copyright (c) Prusa Research 2017 - 2023 Vojtech Bubnik @bubnikv, Lukas Matena @lukasmatena
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "Tool.hpp"

#include <stdexcept>

/*
Firmware tool-capability implementation
=======================================

Setup copies identity-related configuration values needed after begin_print(),
because Config views are borrowed and cannot be retained. Independent machine
capabilities are configured by the concrete tool that composes them.
*/

namespace slic3r_api { namespace GCodeGeneration {

Tool::Tool(uint16_t id) : m_id(id) {}

void Tool::reset_runtime_state()
{
    // The base tool has no dynamic state; specialized capabilities reset their
    // own requested and encoded values.
}

void Tool::setup(const Config &config)
{
    const ConfigPoint xy_offset = config.vector_point_or_default("extruder_offset", m_id, ConfigPoint{});
    m_xy_offset = {xy_offset.x, xy_offset.y};
    m_z_offset = 0.0;
}

void Mill::reset_runtime_state()
{
    Tool::reset_runtime_state();
    m_current_spindle_speed = 0;
}

void Mill::setup(const Config &config)
{
    Tool::setup(config);
    const uint32_t extruder_count = config.get("retract_length").size();
    if (m_id < extruder_count)
        throw std::invalid_argument("A mill tool id overlaps the configured extruders.");
    m_mill_id = uint16_t(m_id - extruder_count);
}

}} // namespace slic3r_api::GCodeGeneration
