///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_Plugins_Ordering_DefaultExtrusionTreeOrdering_hpp_
#define slic3r_Plugins_Ordering_DefaultExtrusionTreeOrdering_hpp_

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"

namespace slic3r_api { namespace Ordering { namespace DefaultOrderingPlugin {

/*
Register the fallback extrusion-tree ordering provider.

The provider runs after the coarse PrintingExtrusion pre-sort. It fixes every
sortable tree in the already ordered plan and replaces each approximate
EntryPointProperty with the resulting exact endpoints. A later provider may
replace this implementation through the same exclusive group.
*/
void register_default_extrusion_tree_ordering_plugin(orchestrator_handle *orchestrator);

}}} // namespace slic3r_api::Ordering::DefaultOrderingPlugin

#endif // slic3r_Plugins_Ordering_DefaultExtrusionTreeOrdering_hpp_
