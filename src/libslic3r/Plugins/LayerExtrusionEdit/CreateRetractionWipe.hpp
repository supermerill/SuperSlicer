///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_Plugins_LayerExtrusionEdit_CreateRetractionWipe_hpp_
#define slic3r_Plugins_LayerExtrusionEdit_CreateRetractionWipe_hpp_

/*
Retraction-wipe creation for compact PrintingPlan scopes
========================================================

CreateRetractionWipe runs after semantic retractions have been placed and
before layer entry state and travels are generated. It converts part of an
existing Retract request into a fast movement over the source scope's freshly
printed path. Open paths are retraced; a final perimeter loop continues from
its seam in the loop direction.

Prerequisites
-------------

- CreateTransitionScope has marked non-nested, ordered printable scopes.
- CreateRetraction has placed any eligible Retract in source.after and the
  matching Unretract in target.before.
- Scope content is final, non-sortable and non-reversible.

Mutations and guarantees
------------------------

The plugin reads printable geometry only from source.content. It inserts Wipe
and any remaining E-only Retract into source.after. An optional inside-start
approach turns the existing target Unretract into geometry inside
target.before. It never changes the marked scope root, the phase order, or the
printable content.

Layer workers are independent. A worker may inspect the next scope only when
it belongs to the same PrintingLayerGroup. Consequently wipe_only_crossing is
conservative at layer boundaries: the wipe is omitted when the target cannot
be inspected safely, while the original E-only retraction remains intact.

After this provider, DefaultLayerEntryState observes the position left by the
wipe. The selected travel provider starts there and aims at an inside-start
approach when one exists. A later lift provider may add Z motion without
changing the semantic E-axis request.
*/

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"

namespace slic3r_api { namespace LayerExtrusionEdit { namespace CreateRetractionWipePlugin {

/* Register the built-in provider which distributes retraction over wipe motion. */
void register_create_retraction_wipe_plugin(orchestrator_handle *orchestrator);

}}} // namespace slic3r_api::LayerExtrusionEdit::CreateRetractionWipePlugin

#endif // slic3r_Plugins_LayerExtrusionEdit_CreateRetractionWipe_hpp_
