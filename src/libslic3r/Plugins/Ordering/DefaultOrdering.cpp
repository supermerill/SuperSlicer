///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "DefaultOrdering.hpp"

namespace slic3r_api { namespace Ordering { namespace DefaultOrderingPlugin {

/*
Default ordering is a chain of two independent plugins.

This file intentionally contains only the aggregate registration entry point
used by the built-in plugin loader. Each phase lives in its own source file so
an external developer can read or replace one phase without wading through the
others:
  - DefaultPlanBuilder.cpp creates the PrintingPlan;
  - DefaultToolGroupOrdering.cpp orders extruder/tool visits.

Sortable content inside each PrintingExtrusion is fixed at the beginning of
STEP_LAYER_EXTRUSION_EDIT. Keeping that operation out of this aggregate leaves
room for a future provider to pre-order complete PrintingExtrusion objects
before their internal trees become immutable.
*/
void register_default_ordering_plugins(orchestrator_handle *orch)
{
    register_default_plan_builder_plugin(orch);
    register_default_tool_group_ordering_plugin(orch);
}

}}} // namespace slic3r_api::Ordering::DefaultOrderingPlugin
