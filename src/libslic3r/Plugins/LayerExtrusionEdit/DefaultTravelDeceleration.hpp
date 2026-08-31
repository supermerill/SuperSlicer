///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_Plugins_LayerExtrusionEdit_DefaultTravelDeceleration_hpp_
#define slic3r_Plugins_LayerExtrusionEdit_DefaultTravelDeceleration_hpp_

/*
Default travel deceleration plugin
==================================

This provider runs after speed and acceleration selection. It may split the
end of a Travel leaf so firmware decelerates with the acceleration requested
by the following printable movement.
*/

struct orchestrator_handle;

namespace slic3r_api { namespace LayerExtrusionEdit { namespace DefaultTravelDecelerationPlugin {

/* Register the default STEP_LAYER_EXTRUSION_EDIT travel-deceleration provider. */
void register_default_travel_deceleration_plugin(orchestrator_handle *orchestrator);

}}} // namespace slic3r_api::LayerExtrusionEdit::DefaultTravelDecelerationPlugin

#endif // slic3r_Plugins_LayerExtrusionEdit_DefaultTravelDeceleration_hpp_
