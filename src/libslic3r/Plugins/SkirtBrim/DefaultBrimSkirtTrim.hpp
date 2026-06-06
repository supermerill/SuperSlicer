///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Plugins_SkirtBrim_DefaultBrimSkirtTrim_hpp_
#define slic3r_Plugins_SkirtBrim_DefaultBrimSkirtTrim_hpp_

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"

namespace slic3r_api { namespace SkirtBrim { namespace DefaultBrimSkirtTrimPlugin {

/*
Default brim/skirt trim
=======================

This STEP_SKIRT_BRIM plugin runs after brim and skirt generation. It is the
extension point that owns post-processing where the final skirt geometry may
force brim to be reduced.
*/
void register_default_brim_skirt_trim_plugin(orchestrator_handle *orchestrator);

}}} // namespace slic3r_api::SkirtBrim::DefaultBrimSkirtTrimPlugin

#endif // slic3r_Plugins_SkirtBrim_DefaultBrimSkirtTrim_hpp_
