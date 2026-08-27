///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Plugins_GCode_FeatureGCode_hpp_
#define slic3r_Plugins_GCode_FeatureGCode_hpp_

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"

namespace slic3r_api { namespace GCodeGeneration { namespace FeatureGCodePlugin {

/*
Register the STEP_EXTRUSION_EDIT plugin which annotates every effective
extrusion-role transition with the configured feature_gcode script. Each
annotation stores its immutable PlaceholderParser role values as a serialized
Config; the selected firmware adds current machine state when it runs later.
*/
void register_feature_gcode_plugin(orchestrator_handle *orchestrator);

}}} // namespace slic3r_api::GCodeGeneration::FeatureGCodePlugin

#endif // slic3r_Plugins_GCode_FeatureGCode_hpp_
