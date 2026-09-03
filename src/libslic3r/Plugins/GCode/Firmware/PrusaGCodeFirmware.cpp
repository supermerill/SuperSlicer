///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "PrusaGCodeFirmware.hpp"

/*
Prusa firmware implementation
=============================

`PrusaGCodeFirmwareSession` reuses the Marlin 2 encoder for movement,
temperatures, acceleration, and machine-state transitions. Its only dialect
specific behavior is the native pause command used when a configured pause
script has no explicit replacement.

The specialization flow is:

    resolve_empty_script()
    |-- emit `M601` for a pause-print event
    `-- delegate every other script type to Marlin 2

The inherited session still traverses the PrintingPlan, tracks extrusion and
tool state, and formats all commands not overridden here. This class therefore
does not own a plugin step or firmware selection; it is one concrete session
created by the built-in firmware provider.
*/

namespace slic3r_api { namespace GCodeGeneration { namespace Firmware {

std::string PrusaGCodeFirmwareSession::resolve_empty_script(
    gcode_script_type script_type,
    const Config *producer_config) const
{
    if (script_type == GCODE_SCRIPT_TYPE_PAUSE_PRINT_GCODE)
        return "M601\n";
    return Marlin2GCodeFirmwareSession::resolve_empty_script(script_type, producer_config);
}

}}} // namespace slic3r_api::GCodeGeneration::Firmware
