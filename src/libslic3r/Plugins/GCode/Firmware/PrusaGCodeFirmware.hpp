///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Plugins_GCode_Firmware_PrusaGCodeFirmware_hpp_
#define slic3r_Plugins_GCode_Firmware_PrusaGCodeFirmware_hpp_

#include "Marlin2GCodeFirmware.hpp"

/*
Prusa firmware session
======================

The initial Prusa dialect intentionally follows Marlin 2 exactly. A distinct
type and provider create a stable extension point for future Prusa-specific
commands without adding conditions to the Marlin implementation.
*/

namespace slic3r_api { namespace GCodeGeneration { namespace Firmware {

class PrusaGCodeFirmwareSession : public Marlin2GCodeFirmwareSession
{
public:
    using Marlin2GCodeFirmwareSession::Marlin2GCodeFirmwareSession;
};

}}} // namespace slic3r_api::GCodeGeneration::Firmware

#endif // slic3r_Plugins_GCode_Firmware_PrusaGCodeFirmware_hpp_
