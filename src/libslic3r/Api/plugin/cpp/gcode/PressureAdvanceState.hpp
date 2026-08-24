///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_gcode_PressureAdvanceState_hpp_
#define slic3r_Api_plugin_cpp_gcode_PressureAdvanceState_hpp_

#include "EncodableState.hpp"

/*
Firmware pressure-advance state
===============================

PressureAdvanceState gives the generic requested/encoded register a domain
name and API. It intentionally owns no configuration: the requested value is
already the value that a firmware encoder must receive.
*/

namespace slic3r_api { namespace GCodeGeneration {

class PressureAdvanceState
{
public:
    void reset_runtime_state();
    void synchronize_runtime_from(const PressureAdvanceState &source);

    std::optional<double> requested() const { return m_value.requested(); }
    std::optional<double> encoded() const { return m_value.encoded(); }
    void request(std::optional<double> value) { m_value.request(value); }
    bool needs_encoding() const;
    void mark_encoded();
    void clear_encoded() { m_value.clear_encoded(); }

private:
    EncodableState<double> m_value;
};

}} // namespace slic3r_api::GCodeGeneration

#endif // slic3r_Api_plugin_cpp_gcode_PressureAdvanceState_hpp_
