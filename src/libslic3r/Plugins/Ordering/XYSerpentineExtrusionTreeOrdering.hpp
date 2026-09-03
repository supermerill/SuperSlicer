///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_Plugins_Ordering_XYSerpentineExtrusionTreeOrdering_hpp_
#define slic3r_Plugins_Ordering_XYSerpentineExtrusionTreeOrdering_hpp_

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"

namespace slic3r_api { namespace Ordering { namespace DefaultOrderingPlugin {

/*
Register the deterministic XY-serpentine extrusion-tree ordering provider.

The provider groups neither geometry nor ownership scopes. It reorders only
PrintingExtrusion vectors and children of nodes that explicitly allow sorting.
Subtree bounding-box centres guide a broad bottom-to-top S traversal. A final
sequential walk then chooses legal orientations and seams and fixes every tree
for the downstream PrintingPlan pipeline.
*/
void register_xy_serpentine_extrusion_tree_ordering_plugin(
    orchestrator_handle *orchestrator);

}}} // namespace slic3r_api::Ordering::DefaultOrderingPlugin

#endif // slic3r_Plugins_Ordering_XYSerpentineExtrusionTreeOrdering_hpp_
