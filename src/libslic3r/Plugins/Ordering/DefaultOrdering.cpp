///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "DefaultOrdering.hpp"
#include "DefaultExtrusionTreeOrdering.hpp"
#include "PrintingExtrusionPreSort.hpp"

namespace slic3r_api { namespace Ordering { namespace DefaultOrderingPlugin {

/*
Default ordering is a chain of four independent plugins.

This file intentionally contains only the aggregate registration entry point
used by the built-in plugin loader. Each phase lives in its own source file so
an external developer can read or replace one phase without wading through the
others:
  - DefaultPlanBuilder.cpp creates the PrintingPlan;
  - DefaultToolGroupOrdering.cpp orders extruder/tool visits;
  - PrintingExtrusionPreSort.cpp coarsely orders each visit's extrusions and
    publishes their estimated entry and exit points;
  - DefaultExtrusionTreeOrdering.cpp fixes the internal trees in that coarse
    order and replaces the estimates with exact endpoints.
*/
void register_default_ordering_plugins(orchestrator_handle *orch)
{
    register_default_plan_builder_plugin(orch);
    register_default_tool_group_ordering_plugin(orch);
    register_printing_extrusion_pre_sort_plugin(orch);
    register_default_extrusion_tree_ordering_plugin(orch);
}

}}} // namespace slic3r_api::Ordering::DefaultOrderingPlugin
