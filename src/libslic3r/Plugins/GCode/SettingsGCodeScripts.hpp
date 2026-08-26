///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Plugins_GCode_SettingsGCodeScripts_hpp_
#define slic3r_Plugins_GCode_SettingsGCodeScripts_hpp_

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"

namespace slic3r_api { namespace GCodeGeneration { namespace SettingsGCodeScriptsPlugin {

/*
Register the plugin which places configured start/end scripts in the global
PrintingPlan event scopes. Placeholder expansion remains the responsibility of
the selected firmware session when those events are finally written.
*/
void register_settings_gcode_scripts_plugin(orchestrator_handle *orchestrator);

}}} // namespace slic3r_api::GCodeGeneration::SettingsGCodeScriptsPlugin

#endif // slic3r_Plugins_GCode_SettingsGCodeScripts_hpp_
