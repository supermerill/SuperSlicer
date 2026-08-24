///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Plugins_GCode_Firmware_BuiltinGCodeFirmwares_hpp_
#define slic3r_Plugins_GCode_Firmware_BuiltinGCodeFirmwares_hpp_

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"

/*
Built-in G-code firmware providers
==================================

These registration functions expose the standard firmware session subclasses
as exclusive GCODE_FIRMWARE plugins. The aggregate function is used by normal
startup, while individual functions keep focused tests and future replacement
experiments possible.
*/

namespace slic3r_api { namespace GCodeGeneration { namespace Firmware {

const char *printer_ui_fragment() noexcept;

void register_marlin1_gcode_firmware_plugin(orchestrator_handle *orchestrator);
void register_marlin2_gcode_firmware_plugin(orchestrator_handle *orchestrator);
void register_prusa_gcode_firmware_plugin(orchestrator_handle *orchestrator);
void register_reprap_gcode_firmware_plugin(orchestrator_handle *orchestrator);
void register_sprinter_gcode_firmware_plugin(orchestrator_handle *orchestrator);
void register_klipper_gcode_firmware_plugin(orchestrator_handle *orchestrator);
void register_builtin_gcode_firmware_plugins(orchestrator_handle *orchestrator);

}}} // namespace slic3r_api::GCodeGeneration::Firmware

#endif // slic3r_Plugins_GCode_Firmware_BuiltinGCodeFirmwares_hpp_
