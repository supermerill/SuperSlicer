///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_Plugins_LayerExtrusionEdit_DefaultLayerEntryState_hpp_
#define slic3r_Plugins_LayerExtrusionEdit_DefaultLayerEntryState_hpp_

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"

namespace slic3r_api { namespace LayerExtrusionEdit { namespace DefaultLayerEntryStatePlugin {

/*
Register the default PrintingLayerGroup entry-state producer.

The plugin performs one ordered scan before parallel layer editing begins. It
then publishes the precomputed position and active tool on each layer-group so
later plugins never need to inspect a concurrently processed predecessor.
*/
void register_default_layer_entry_state_plugin(orchestrator_handle *orchestrator);

}}} // namespace slic3r_api::LayerExtrusionEdit::DefaultLayerEntryStatePlugin

#endif // slic3r_Plugins_LayerExtrusionEdit_DefaultLayerEntryState_hpp_
