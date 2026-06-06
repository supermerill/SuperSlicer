///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Plugins_SkirtBrim_DefaultSkirtGenerator_hpp_
#define slic3r_Plugins_SkirtBrim_DefaultSkirtGenerator_hpp_

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"

namespace slic3r_api { namespace SkirtBrim { namespace DefaultSkirtGeneratorPlugin {

/*
Default skirt generator
=======================

This built-in STEP_SKIRT_BRIM plugin ports the classic skirt generation path to
the plugin API. Unlike DefaultBrimGenerator, it does not include Print, Flow or
ClipperUtils host classes: all geometry and config access goes through the
public C/C++ plugin helpers.
*/
void register_default_skirt_generator_plugin(orchestrator_handle *orchestrator);

}}} // namespace slic3r_api::SkirtBrim::DefaultSkirtGeneratorPlugin

#endif // slic3r_Plugins_SkirtBrim_DefaultSkirtGenerator_hpp_
