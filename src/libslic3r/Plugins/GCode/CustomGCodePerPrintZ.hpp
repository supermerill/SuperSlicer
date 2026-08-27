///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Plugins_GCode_CustomGCodePerPrintZ_hpp_
#define slic3r_Plugins_GCode_CustomGCodePerPrintZ_hpp_

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"

namespace slic3r_api::GCodeGeneration::CustomGCodePerPrintZPlugin {

void register_custom_gcode_per_print_z_ordering_plugins(orchestrator_handle *orchestrator);

/* Register the two ordering passes and the later script-insertion pass. */
void register_custom_gcode_per_print_z_plugins(orchestrator_handle *orchestrator);

} // namespace slic3r_api::GCodeGeneration::CustomGCodePerPrintZPlugin

#endif // slic3r_Plugins_GCode_CustomGCodePerPrintZ_hpp_
