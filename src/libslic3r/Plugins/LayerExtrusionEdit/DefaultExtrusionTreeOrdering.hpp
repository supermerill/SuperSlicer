///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_Plugins_LayerExtrusionEdit_DefaultExtrusionTreeOrdering_hpp_
#define slic3r_Plugins_LayerExtrusionEdit_DefaultExtrusionTreeOrdering_hpp_

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"

namespace slic3r_api { namespace LayerExtrusionEdit { namespace DefaultExtrusionTreeOrderingPlugin {

/*
Register the fallback extrusion-tree ordering provider.

The provider runs before transition processing. It fixes every sortable tree
inside one PrintingLayerGroup and publishes its exact EntryPointProperty. A
future ordering implementation may replace it through the same exclusive
group without changing the later layer-edit plugins.
*/
void register_default_extrusion_tree_ordering_plugin(orchestrator_handle *orchestrator);

}}} // namespace slic3r_api::LayerExtrusionEdit::DefaultExtrusionTreeOrderingPlugin

#endif // slic3r_Plugins_LayerExtrusionEdit_DefaultExtrusionTreeOrdering_hpp_
