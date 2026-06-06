///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Plugins_SkirtBrim_DefaultBrimGenerator_hpp_
#define slic3r_Plugins_SkirtBrim_DefaultBrimGenerator_hpp_

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"

namespace slic3r_api { namespace SkirtBrim { namespace DefaultBrimGeneratorPlugin {

/*
Default brim generator
======================

This built-in STEP_SKIRT_BRIM plugin owns the classic brim behavior while the
legacy host path still owns skirt generation. The plugin uses host brim helpers
for the heavy geometry work, then publishes the resulting extrusion trees
through the STEP_SKIRT_BRIM callback table just like an external plugin would.
*/
void register_default_brim_generator_plugin(orchestrator_handle *orchestrator);

}}} // namespace slic3r_api::SkirtBrim::DefaultBrimGeneratorPlugin

#endif // slic3r_Plugins_SkirtBrim_DefaultBrimGenerator_hpp_
