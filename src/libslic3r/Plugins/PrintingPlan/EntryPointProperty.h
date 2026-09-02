///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_Plugins_PrintingPlan_EntryPointProperty_h_
#define slic3r_Plugins_PrintingPlan_EntryPointProperty_h_

/*
Shared entry/exit metadata for PrintingExtrusion roots
=======================================================

Ordering plugins use this private dynamic property to publish the best known
XY entry and exit of one complete PrintingExtrusion. The payload is attached
directly to PrintingExtrusion::root(), but describes the whole owned tree. It
must therefore be read directly from that root and never through inherited
property lookup on one of its descendants.

The points may be estimates produced before internal ordering or exact values
published after the tree has been fixed. The payload deliberately carries no
quality flag: plugin priority and dependencies define which producer has run.
A producer replaces both points atomically whenever it improves the estimate.
An absent property means that no usable geometric entry or exit is known.

STEP_LAYER_EXTRUSION_EDIT consumers may copy neighbouring values during their
setup_run() pass. All setup_run() calls finish before any parallel run() starts,
so those copies remain stable even when each worker later updates properties on
the roots owned by its own PrintingLayerGroup.
*/

#include "libslic3r/Api/plugin/c/slic3r_geometry.h"

#define PRINTING_EXTRUSION_ENTRY_POINT_PROPERTY_NAME \
    "slic3r.printing_extrusion.entry_points"

/* Approximate or exact XY endpoints of one complete PrintingExtrusion tree. */
typedef struct c_printing_extrusion_entry_point_property {
    c_point entry;
    c_point exit;
} c_printing_extrusion_entry_point_property;

#ifdef __cplusplus
#include "libslic3r/Api/plugin/cpp/PluginPropertyKey.hpp"

namespace slic3r_api {

/* Typed C++ payload stored directly on a PrintingExtrusion root. */
struct EntryPointProperty : c_printing_extrusion_entry_point_property
{
};

/* Register the shared contract and retain its orchestrator in the typed key. */
inline PluginPropertyKey<EntryPointProperty>
entry_point_property_key(orchestrator_handle *orchestrator)
{
    return PluginPropertyKey<EntryPointProperty>::register_dynamic(
        orchestrator, PRINTING_EXTRUSION_ENTRY_POINT_PROPERTY_NAME);
}

} // namespace slic3r_api
#endif

#endif // slic3r_Plugins_PrintingPlan_EntryPointProperty_h_
