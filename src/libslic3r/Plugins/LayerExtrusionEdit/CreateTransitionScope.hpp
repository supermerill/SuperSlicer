///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_Plugins_LayerExtrusionEdit_CreateTransitionScope_hpp_
#define slic3r_Plugins_LayerExtrusionEdit_CreateTransitionScope_hpp_

/*
Register the built-in compact transition-scope producer.

Purpose
-------

The provider prepares the ordered extrusion trees consumed by retraction,
wipe, travel and lift plugins. It partitions printable geometry into maximal
continuous scopes and marks each scope root with
PrintingExtrusionScopeProperty. A scope never crosses a PrintingExtrusion
ownership boundary, even when geometry on both sides is continuous.

When a real transition exists, the provider restructures the scope root and
creates empty ExtrusionEntity phases around its printable content. It does not
create another PrintingExtrusion and does not generate process movements. The
later plugins fill the reserved phases with retract, wipe, travel, unretract or
lift geometry.

Input requirements
------------------

- PrintingGroup, PrintingLayerGroup, PrintingToolGroup and PrintingExtrusion
  order must already be final.
- Printable geometry and inherited extrusion properties must remain unchanged
  while this provider executes.
- Extrusion trees must already be in their final execution order. Their
  entities should be non-sortable and non-reversible at this stage.
- Existing Travel, Wipe, Retract and Unretract entities may already be present;
  they are treated as process boundaries rather than printable scope content.

Output contract
---------------

- Every printable polyline belongs to exactly one marked scope, either because
  its own entity is the scope root or because it is below that root's content
  phase.
- PrintingExtrusionScopeProperty exists directly on the scope root only. A
  marked scope cannot contain another marked scope.
- No printable polyline remains outside a marked scope. Existing process
  geometry is intentionally left outside printable content.
- Every scope root is non-sortable and non-reversible. Its property flags and
  optional children follow the layouts documented by OrderedExtrusionScope.
- Structurally separate but geometrically continuous scopes remain separate and
  receive no transition phases. Therefore an outgoing transition on one scope
  always agrees with the incoming transition on the next scope.
- START marks the first printable scope and TERMINAL marks the last. TERMINAL
  alone does not create an after phase.

Downstream plugins must modify only the reserved phase subtrees and preserve
the marked roots, their order and their PrintingExtrusion ownership.
*/

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"

namespace slic3r_api { namespace LayerExtrusionEdit { namespace CreateTransitionScopePlugin {

/* Register the built-in provider in one orchestrator. */
void register_create_transition_scope_plugin(orchestrator_handle *orchestrator);

}}} // namespace slic3r_api::LayerExtrusionEdit::CreateTransitionScopePlugin

#endif // slic3r_Plugins_LayerExtrusionEdit_CreateTransitionScope_hpp_
