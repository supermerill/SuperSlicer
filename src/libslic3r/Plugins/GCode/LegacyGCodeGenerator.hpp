///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Plugins_GCode_LegacyGCodeGenerator_hpp_
#define slic3r_Plugins_GCode_LegacyGCodeGenerator_hpp_

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"

namespace slic3r_api { namespace GCodeGeneration { namespace LegacyGCodeGeneratorPlugin {

/*
Register the legacy STEP_GCODE selector plugin.

This plugin is intentionally a selector token, not a normal STEP_GCODE writer.
The real legacy export path still lives in the host GCodeGenerator because it
uses many host-only classes that are not part of the plugin API. The plugin
exists so the GUI can show "Legacy G-code generator" in the same exclusive
selector as experimental plugin writers.
*/
void register_legacy_gcode_generator_plugin(orchestrator_handle *orch);

}}} // namespace slic3r_api::GCodeGeneration::LegacyGCodeGeneratorPlugin

#endif // slic3r_Plugins_GCode_LegacyGCodeGenerator_hpp_
