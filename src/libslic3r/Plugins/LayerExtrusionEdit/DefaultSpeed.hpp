///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_Plugins_LayerExtrusionEdit_DefaultSpeed_hpp_
#define slic3r_Plugins_LayerExtrusionEdit_DefaultSpeed_hpp_

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"

namespace slic3r_api { namespace LayerExtrusionEdit { namespace DefaultSpeedPlugin {

/*
Register the default STEP_LAYER_EXTRUSION_EDIT speed plugin.

The plugin resolves missing speed on cloned PrintingPlan trees, computes one
autospeed target per printing group and extruder, applies physical speed and
flow limits, and hoists uniform values towards extrusion collection nodes.
*/
void register_default_speed_plugin(orchestrator_handle *orch);

}}} // namespace slic3r_api::LayerExtrusionEdit::DefaultSpeedPlugin

#endif // slic3r_Plugins_LayerExtrusionEdit_DefaultSpeed_hpp_
