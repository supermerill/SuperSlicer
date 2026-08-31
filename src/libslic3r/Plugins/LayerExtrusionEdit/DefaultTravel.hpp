///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_Plugins_LayerExtrusionEdit_DefaultTravel_hpp_
#define slic3r_Plugins_LayerExtrusionEdit_DefaultTravel_hpp_

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"

namespace slic3r_api { namespace LayerExtrusionEdit { namespace DefaultTravelPlugin {

/*
Register the simple straight-travel producer.

The plugin connects consecutive geometric leaves after ordering has fixed their
execution order. It deliberately performs no obstacle avoidance, lift,
retraction, or wipe calculation; a more advanced provider may later replace it
under the same plugin and exclusive-group identifiers.
*/
void register_default_travel_plugin(orchestrator_handle *orchestrator);

}}} // namespace slic3r_api::LayerExtrusionEdit::DefaultTravelPlugin

#endif // slic3r_Plugins_LayerExtrusionEdit_DefaultTravel_hpp_
