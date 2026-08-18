///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_Plugins_LayerExtrusionEdit_DefaultAcceleration_hpp_
#define slic3r_Plugins_LayerExtrusionEdit_DefaultAcceleration_hpp_

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"

namespace slic3r_api { namespace LayerExtrusionEdit { namespace DefaultAccelerationPlugin {

/*
Register the default STEP_LAYER_EXTRUSION_EDIT acceleration plugin.

The plugin resolves missing acceleration from role, object, and machine-limit
settings, then hoists uniform values without reading or changing speed.
*/
void register_default_acceleration_plugin(orchestrator_handle *orch);

}}} // namespace slic3r_api::LayerExtrusionEdit::DefaultAccelerationPlugin

#endif // slic3r_Plugins_LayerExtrusionEdit_DefaultAcceleration_hpp_
