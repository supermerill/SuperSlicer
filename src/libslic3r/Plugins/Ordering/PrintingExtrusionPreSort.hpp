///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_Plugins_Ordering_PrintingExtrusionPreSort_hpp_
#define slic3r_Plugins_Ordering_PrintingExtrusionPreSort_hpp_

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"

namespace slic3r_api { namespace Ordering { namespace DefaultOrderingPlugin {

/*
Register the built-in coarse ordering of PrintingExtrusion objects.

The provider runs after the PrintingPlan and its tool visits have reached their
final STEP_ORDERING order. It reorders only the extrusion vector owned by each
PrintingToolGroup. It neither moves content between owners nor fixes the trees'
internal sortable structure.

For every non-empty root it also publishes the approximate entry and exit
selected by the ordering engine. The following layer-extrusion-edit step can
use those points as an initial estimate before replacing them with exact tree
endpoints.
*/
void register_printing_extrusion_pre_sort_plugin(orchestrator_handle *orchestrator);

}}} // namespace slic3r_api::Ordering::DefaultOrderingPlugin

#endif // slic3r_Plugins_Ordering_PrintingExtrusionPreSort_hpp_
