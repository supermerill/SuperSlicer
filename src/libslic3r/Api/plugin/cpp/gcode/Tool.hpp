///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/ Copyright (c) Prusa Research 2017 - 2023 Vojtech Bubnik @bubnikv, Lukas Matena @lukasmatena
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_gcode_Tool_hpp_
#define slic3r_Api_plugin_cpp_gcode_Tool_hpp_

#include <cstdint>
#include "../ConfigViews.hpp"

/*
Firmware tool capabilities
==========================

Tool provides the physical identity and configuration shared by machine tools.
Independent fan, heater, pressure-advance and extrusion-axis states live in
their own modules and are composed by concrete tools.

Extrusion-axis state, gantry state and printer-wide state live in their own
headers. Consumers include only the machine-state modules they use.
*/

namespace slic3r_api { namespace GCodeGeneration {

class Tool
{
public:
    explicit Tool(uint16_t id);
    virtual ~Tool() = default;

    uint16_t id() const { return m_id; }
    virtual void setup(const Config &config);
    virtual void reset_runtime_state();

    c_vec2d xy_offset() const { return m_xy_offset; }
    double z_offset() const { return m_z_offset; }

protected:
    // --- Immutable tool identity ---
    // Print-wide stable identifier of this physical tool.
    const uint16_t m_id;

    // --- Configuration cache populated by setup() ---
    // These values are copied from Config and do not represent commands sent
    // to the machine. reset_runtime_state() therefore leaves them unchanged.
    // XY correction applied while this tool is held by the gantry.
    c_vec2d m_xy_offset{0.0, 0.0};
    // Z correction applied while this tool is held by the gantry.
    double m_z_offset = 0.0;
};

inline bool operator==(const Tool &lhs, const Tool &rhs) { return lhs.id() == rhs.id(); }
inline bool operator!=(const Tool &lhs, const Tool &rhs) { return lhs.id() != rhs.id(); }
inline bool operator<(const Tool &lhs, const Tool &rhs) { return lhs.id() < rhs.id(); }
inline bool operator>(const Tool &lhs, const Tool &rhs) { return lhs.id() > rhs.id(); }

class Mill : public Tool
{
public:
    explicit Mill(uint16_t tool_id) : Tool(tool_id) {}

    void setup(const Config &config) override;
    void reset_runtime_state() override;
    uint16_t mill_id() const { return m_mill_id; }

private:
    // --- Runtime state ---
    // Current spindle command tracked for this mill tool.
    int16_t m_current_spindle_speed = 0;

    // --- Configuration cache populated by setup() ---
    // Index used to read this mill's values from vector configuration options.
    uint16_t m_mill_id = 0;
};

}} // namespace slic3r_api::GCodeGeneration

#endif // slic3r_Api_plugin_cpp_gcode_Tool_hpp_
