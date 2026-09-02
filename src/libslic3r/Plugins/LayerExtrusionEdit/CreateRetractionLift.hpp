///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_Plugins_LayerExtrusionEdit_CreateRetractionLift_hpp_
#define slic3r_Plugins_LayerExtrusionEdit_CreateRetractionLift_hpp_

/*
Retraction-lift creation for compact PrintingPlan scopes
========================================================

CreateRetractionLift runs after semantic retraction, wipe creation, layer entry
state and travel routing. It turns the Z lift requested by the source settings
into explicit geometry inside the already reserved transition phases.

Prerequisites
-------------

- CreateTransitionScope has produced non-nested compact scopes.
- A retraction provider has placed Retract requests in source.after.
- A travel provider has filled target.travel when movement is required.
- Scope roots and printable content are fixed, non-sortable and non-reversible.

Mutations and guarantees
------------------------

During setup_run(), each worker raises only retracting Wipe geometry owned by
its layer. The host barrier then makes the final wipe Z available before run()
shapes incoming travels. During run(), a worker modifies only target.travel in
its own layer; it never reads or mutates a neighboring layer's extrusion tree.

The travel always returns to its original destination Z. Layer entry snapshots
therefore remain valid even though the nozzle may start the travel at the Z
left by a progressive wipe lift.
*/

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"

namespace slic3r_api { namespace LayerExtrusionEdit { namespace CreateRetractionLiftPlugin {

/* Register the built-in provider which materializes retract-related Z lift. */
void register_create_retraction_lift_plugin(orchestrator_handle *orchestrator);

}}} // namespace slic3r_api::LayerExtrusionEdit::CreateRetractionLiftPlugin

#endif // slic3r_Plugins_LayerExtrusionEdit_CreateRetractionLift_hpp_
