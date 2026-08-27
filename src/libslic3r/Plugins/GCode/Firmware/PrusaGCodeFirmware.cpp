///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "PrusaGCodeFirmware.hpp"

/* Prusa shares Marlin 2 encoding except for its native pause fallback. */

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
