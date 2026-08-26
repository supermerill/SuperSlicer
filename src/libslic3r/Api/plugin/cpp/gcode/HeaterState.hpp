///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_gcode_HeaterState_hpp_
#define slic3r_Api_plugin_cpp_gcode_HeaterState_hpp_

#include <cstdint>

#include "EncodableState.hpp"

/*
Firmware heater state
=====================

HeaterState represents any independently controlled heater: tool, bed,
chamber or a future firmware-specific device. It distinguishes the requested
target, the encoded target and the target encoded with an explicit wait.
*/

namespace slic3r_api { namespace GCodeGeneration {

class HeaterState
{
public:
    void setup(int16_t temperature_offset);
    void reset_runtime_state();
    void synchronize_runtime_from(const HeaterState &source);
    // Import a physical target already emitted by external G-code.
    void synchronize_after_external_gcode(int16_t effective_temperature, bool waited);

    std::optional<int16_t> requested_temperature() const { return m_temperature.requested(); }
    std::optional<int16_t> encoded_temperature() const { return m_temperature.encoded(); }
    std::optional<int16_t> encoded_temperature_with_wait() const
    {
        return m_temperature.encoded_with_wait();
    }

    void request_temperature(std::optional<int16_t> temperature)
    {
        m_temperature.request(temperature);
    }
    int16_t effective_temperature() const;
    bool needs_encoding() const;
    bool needs_wait_encoding() const;
    void mark_encoded();
    void mark_encoded_with_wait();
    void clear_encoded() { m_temperature.clear_encoded(); }
    void clear_encoded_with_wait() { m_temperature.clear_encoded_with_wait(); }

private:
    // --- Runtime requested/encoded/wait-encoded state ---
    AwaitableEncodableState<int16_t> m_temperature;

    // --- Configuration cache populated by setup() ---
    // Per-device correction added before a requested target is encoded.
    int16_t m_temperature_offset = 0;
};

}} // namespace slic3r_api::GCodeGeneration

#endif // slic3r_Api_plugin_cpp_gcode_HeaterState_hpp_
