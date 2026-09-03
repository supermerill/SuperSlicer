///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Plugins_Ordering_AdvancedExtrusionTreeOrdering_hpp_
#define slic3r_Plugins_Ordering_AdvancedExtrusionTreeOrdering_hpp_

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"

namespace slic3r_api { namespace Ordering { namespace DefaultOrderingPlugin {

/*
Register the seam-aware extrusion-tree ordering provider.

The provider consumes the coarse order and EntryPointProperty values produced
by PrintingExtrusionPreSort. It creates one shared SeamPlacer session, then
processes independent PrintingToolGroup objects in parallel around a sequential
barrier that refines the order of complete PrintingExtrusion values.
*/
void register_advanced_extrusion_tree_ordering_plugin(orchestrator_handle *orchestrator);

}}} // namespace slic3r_api::Ordering::DefaultOrderingPlugin

#endif // slic3r_Plugins_Ordering_AdvancedExtrusionTreeOrdering_hpp_
