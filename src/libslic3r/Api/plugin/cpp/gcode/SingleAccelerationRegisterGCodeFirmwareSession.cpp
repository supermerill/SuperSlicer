///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "SingleAccelerationRegisterGCodeFirmwareSession.hpp"

/*
Single acceleration-register firmware session implementation
==============================================================

The standard session performs the actual encoding and acknowledges the active
logical category. This specialization then clears the opposite acknowledgement
because both categories compete for the same physical firmware register.
*/

namespace slic3r_api { namespace GCodeGeneration {

std::string SingleAccelerationRegisterGCodeFirmwareSession::write_acceleration(
    PreparedMove::Kind kind)
{
    // Remember whether the standard implementation is about to encode a
    // transition. The returned text may legitimately be empty, so its size
    // cannot be used to decide whether the state was acknowledged.
    const bool encodes_travel =
        (kind == PreparedMove::Kind::Travel || kind == PreparedMove::Kind::Wipe) &&
        gantry().needs_travel_acceleration_encoding();
    const bool encodes_print = kind == PreparedMove::Kind::Extrusion &&
                               gantry().needs_print_acceleration_encoding();

    std::string output = DefaultGCodeFirmwareSession::write_acceleration(kind);

    // The command just written replaced the one shared physical register.
    // Preserve both requests, but force the other category to be restored when
    // a later movement needs it again.
    if (encodes_travel)
        gantry().clear_print_acceleration_encoded();
    else if (encodes_print)
        gantry().clear_travel_acceleration_encoded();

    return output;
}

}} // namespace slic3r_api::GCodeGeneration
