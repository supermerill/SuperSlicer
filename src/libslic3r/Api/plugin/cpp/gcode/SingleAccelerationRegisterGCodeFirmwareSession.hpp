///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_gcode_SingleAccelerationRegisterGCodeFirmwareSession_hpp_
#define slic3r_Api_plugin_cpp_gcode_SingleAccelerationRegisterGCodeFirmwareSession_hpp_

#include "DefaultGCodeFirmwareSession.hpp"

/*
Single acceleration-register firmware session
==============================================

Some firmware dialects expose one physical acceleration command for both
print and travel moves. This specialization keeps Gantry's two logical
requests, but acknowledges only the category represented by the latest
command. Switching movement category therefore restores that category's
requested acceleration even when both requested values happen to be equal.
*/

namespace slic3r_api { namespace GCodeGeneration {

// Reuses the standard PrintingPlan traversal while adapting only the relation
// between the independent print and travel acceleration histories.
class SingleAccelerationRegisterGCodeFirmwareSession :
    public DefaultGCodeFirmwareSession
{
public:
    using DefaultGCodeFirmwareSession::DefaultGCodeFirmwareSession;

protected:
    std::string write_acceleration(PreparedMove::Kind kind) override;
};

}} // namespace slic3r_api::GCodeGeneration

#endif // slic3r_Api_plugin_cpp_gcode_SingleAccelerationRegisterGCodeFirmwareSession_hpp_
