///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_gcode_Gantry_hpp_
#define slic3r_Api_plugin_cpp_gcode_Gantry_hpp_

#include <cstdint>
#include <optional>

#include "../ConfigViews.hpp"
#include "EncodableState.hpp"

/*
Firmware gantry state
=====================

Gantry stores the physical XYZ position and the motion settings interpreted by
a firmware session. Requested values describe what future moves need, while
encoded values describe settings already represented in generated output.
Print and travel acceleration histories remain logically independent even
when a firmware maps both values to one physical command register.

Configuration values are copied during setup(). Runtime state is reset for
each export without discarding those cached settings.
*/

namespace slic3r_api { namespace GCodeGeneration {

class Gantry
{
public:
    void setup(const Config &config);
    void reset_runtime_state();
    // Replace every runtime register with another gantry's exact state while
    // retaining this instance's setup-derived travel speed and Z correction.
    void synchronize_runtime_from(const Gantry &source);

    std::optional<c_vec3d> position() const { return m_position; }
    void set_position(c_vec3d position) { m_position = position; }
    void invalidate_position() { m_position.reset(); }

    std::optional<double> requested_speed() const { return m_speed.requested(); }
    std::optional<double> encoded_speed() const { return m_speed.encoded(); }
    void request_speed(std::optional<double> speed) { m_speed.request(speed); }
    bool needs_speed_encoding() const;
    void mark_speed_encoded();

    std::optional<uint32_t> requested_print_acceleration() const { return m_print_acceleration.requested(); }
    std::optional<uint32_t> requested_travel_acceleration() const { return m_travel_acceleration.requested(); }
    std::optional<uint32_t> encoded_print_acceleration() const { return m_print_acceleration.encoded(); }
    std::optional<uint32_t> encoded_travel_acceleration() const { return m_travel_acceleration.encoded(); }
    void request_print_acceleration(std::optional<uint32_t> acceleration)
    {
        m_print_acceleration.request(acceleration);
    }
    void request_travel_acceleration(std::optional<uint32_t> acceleration)
    {
        m_travel_acceleration.request(acceleration);
    }
    bool needs_print_acceleration_encoding() const;
    bool needs_travel_acceleration_encoding() const;
    void mark_print_acceleration_encoded();
    void mark_travel_acceleration_encoded();
    void clear_print_acceleration_encoded();
    void clear_travel_acceleration_encoded();

    double travel_speed() const { return m_travel_speed; }
    double z_offset() const { return m_z_offset; }

private:
    // --- Persistent physical/runtime state ---
    // Current physical machine position. An empty value forces the next
    // movement to establish all three coordinates explicitly.
    std::optional<c_vec3d> m_position;

    // --- Runtime requested/encoded state ---
    // Speed follows extrusion-tree inheritance. The two acceleration requests
    // are independent logical states: each remains available until a later
    // movement of the same category replaces it or the session is reset.
    // Encoded values track what was represented in generated output and are
    // never restored when leaving an extrusion-tree node.
    // Speed requested by the active extrusion-tree scope and represented by
    // the most recently encoded F word, in mm/s.
    EncodableState<double> m_speed;
    // Independent requested/encoded acceleration histories, in mm/s2.
    EncodableState<uint32_t> m_print_acceleration;
    EncodableState<uint32_t> m_travel_acceleration;

    // --- Configuration cache populated by setup() ---
    // Printer-wide speed used only for synthesized positioning travels.
    double m_travel_speed = 0.0;
    // Global Z correction applied to every generated machine position.
    double m_z_offset = 0.0;
};

}} // namespace slic3r_api::GCodeGeneration

#endif // slic3r_Api_plugin_cpp_gcode_Gantry_hpp_
