///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_Plugins_LayerExtrusionEdit_AvoidCrossingPerimetersTravel_hpp_
#define slic3r_Plugins_LayerExtrusionEdit_AvoidCrossingPerimetersTravel_hpp_

/*
Avoid-crossing travel provider
==============================

This provider is an alternative to the simple straight-travel plugin. It
connects the same ordered PrintingPlan leaves, but asks the optimized core
AvoidCrossingPerimeters router for a detour when both endpoint regions enable
the feature and the direct segment really crosses a perimeter.

The provider is not selected by default. Registering it makes it available in
the same exclusive travel-generation group as the straight implementation.
*/

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"

namespace slic3r_api { namespace LayerExtrusionEdit { namespace AvoidCrossingPerimetersTravelPlugin {

/* Register the optional perimeter-avoiding travel producer. */
void register_avoid_crossing_perimeters_travel_plugin(orchestrator_handle *orchestrator);

}}} // namespace slic3r_api::LayerExtrusionEdit::AvoidCrossingPerimetersTravelPlugin

#endif // slic3r_Plugins_LayerExtrusionEdit_AvoidCrossingPerimetersTravel_hpp_
