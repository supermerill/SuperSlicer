///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_Plugins_LayerExtrusionEdit_DefaultFan_hpp_
#define slic3r_Plugins_LayerExtrusionEdit_DefaultFan_hpp_

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"

namespace slic3r_api { namespace LayerExtrusionEdit { namespace DefaultFanPlugin {

/*
Register the default STEP_LAYER_EXTRUSION_EDIT fan plugin.

The plugin converts per-extruder cooling settings and layer duration into
effective fan percentages on the cloned PrintingPlan extrusion trees. It does
not emit G-code or slow paths down; those remain separate responsibilities.
*/
void register_default_fan_plugin(orchestrator_handle *orch);

}}} // namespace slic3r_api::LayerExtrusionEdit::DefaultFanPlugin

#endif // slic3r_Plugins_LayerExtrusionEdit_DefaultFan_hpp_
