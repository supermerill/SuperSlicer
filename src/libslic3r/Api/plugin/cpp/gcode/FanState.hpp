///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_gcode_FanState_hpp_
#define slic3r_Api_plugin_cpp_gcode_FanState_hpp_

#include "EncodableState.hpp"

/*
Firmware fan state
==================

FanState represents one independently controlled fan. It keeps the process
request separate from the effective value sent to firmware so a configured
correction can be applied without becoming part of inherited extrusion state.
The owner decides whether this is a tool fan, chamber fan or another device.
*/

namespace slic3r_api { namespace GCodeGeneration {

class FanState
{
public:
    void setup(double speed_offset_percent);
    void reset_runtime_state();
    void synchronize_runtime_from(const FanState &source);

    std::optional<double> requested_speed_percent() const { return m_speed.requested(); }
    std::optional<double> encoded_speed_percent() const { return m_speed.encoded(); }
    void request_speed_percent(std::optional<double> speed) { m_speed.request(speed); }
    double effective_speed_percent() const;
    bool needs_encoding() const;
    void mark_encoded();
    void clear_encoded() { m_speed.clear_encoded(); }

private:
    // --- Runtime requested/encoded state ---
    EncodableState<double> m_speed;

    // --- Configuration cache populated by setup() ---
    // Per-device correction added before a requested percentage is encoded.
    double m_speed_offset_percent = 0.0;
};

}} // namespace slic3r_api::GCodeGeneration

#endif // slic3r_Api_plugin_cpp_gcode_FanState_hpp_
