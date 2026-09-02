///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_Plugins_LayerExtrusionEdit_CreateRetraction_hpp_
#define slic3r_Plugins_LayerExtrusionEdit_CreateRetraction_hpp_

/*
Register the built-in semantic retraction providers.

CreateRetraction consumes the compact roots produced by CreateTransitionScope.
For every real boundary it writes a semantic Retract event into the source
scope's after phase and writes an Unretract event into the target scope's
before phase. Tool-selection flags choose the E-axis settings but do not make
this provider emit a physical selection command. It never creates scopes,
phase children or travel geometry.

Input requirements
------------------

- PrintingPlan order and extrusion-tree order are final.
- CreateTransitionScope has marked every printable scope and created all phase
  nodes required by its incoming and outgoing transition flags.
- Scope roots and their phase order remain stable during the plugin run.
- No other active retraction provider has already populated these phases.

Parallel behavior
-----------------

Each layer worker reads its local scope owners directly. Only the first and
last scalar boundary facts are copied during setup_run so adjacent non-empty
layers can make the same decision after the host barrier. A worker mutates only
the scopes owned by its PrintingLayerGroup.

Output contract
---------------

- Retract is appended to source.after.
- Unretract is appended to target.before. Tool selection is materialized by
  CreateToolChange in the target PrintingToolGroup events before scopes exist.
- Later wipe, travel and lift providers may edit these phase subtrees, but the
  marked scope roots and printable content remain unchanged.
- A companion sequential provider stores the final normal retraction in
  PrintingPlan.after because a terminal compact scope deliberately has no
  artificial after phase.
*/

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"

namespace slic3r_api { namespace LayerExtrusionEdit { namespace CreateRetractionPlugin {

/* Register the parallel transition provider and its plan-terminal companion. */
void register_create_retraction_plugins(orchestrator_handle *orchestrator);

}}} // namespace slic3r_api::LayerExtrusionEdit::CreateRetractionPlugin

#endif // slic3r_Plugins_LayerExtrusionEdit_CreateRetraction_hpp_
