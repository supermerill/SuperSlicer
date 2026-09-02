///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Plugins_Ordering_DefaultOrdering_hpp_
#define slic3r_Plugins_Ordering_DefaultOrdering_hpp_

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"

namespace slic3r_api { namespace Ordering { namespace DefaultOrderingPlugin {

/*
Register the built-in STEP_ORDERING plugins one by one.

Each function registers one phase of the ordering chain. Tests or experiments
may call a single registration helper when they want to replace only one phase
with another plugin. The aggregate helper is what the normal plugin loader uses:
it registers the default plan builder and then the tool-group sorter. Internal
tree ordering now belongs to the following layer-extrusion-edit step, where it
may be replaced independently from the high-level PrintingPlan ordering.
*/
void register_default_plan_builder_plugin(orchestrator_handle *orch);
void register_default_tool_group_ordering_plugin(orchestrator_handle *orch);
void register_default_ordering_plugins(orchestrator_handle *orch);

}}} // namespace slic3r_api::Ordering::DefaultOrderingPlugin

#endif // slic3r_Plugins_Ordering_DefaultOrdering_hpp_
