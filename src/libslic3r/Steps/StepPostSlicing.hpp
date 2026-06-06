///|/ Copyright (c) SuperSlicer 2026 Durand R?mi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef steps_steppostslicing_hpp_
#define steps_steppostslicing_hpp_

#include <string>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_plugin_types.h"

namespace Slic3r {

class Orchestrator;
class Plugin;
class Print;

namespace Steps::StepPostSlicing {

/*
Reset or prepare native data needed by STEP_POST_SLICING before plugins run.
Currently empty; keep step-specific cleanup here instead of Orchestrator.
*/
void clean_and_prepare(Print &print);

/*
Check that the data tree is in the shape expected before running
STEP_POST_SLICING plugins.

At this point slicing has produced per-layer geometry, but surface generation
has not run yet. The expected state is:
- LayerRegion raw slices may be empty when their PrintRegion is available but
  unused on that layer. This is valid for the fallback default region and for
  modifier regions whose masks do not touch every layer.
- LayerRegion raw slices are mutually disjoint, except for tiny numeric slivers
  around shared borders.
- Layer slices exist and contain at least one non-empty ExPolygon.
- Layer islands mirror Layer slices: one island per layer slice.
- LayerSliceIsland has no LayerRegionIsland yet.
- LayerRegion processed surfaces and fill surfaces are still empty.

Call this before exposing mutable post-slicing callbacks to plugins, or from
tests that want to assert that the previous slicing step produced a valid input
for post-slicing.
*/
bool validate_pre(const Print &print, std::string &error);

/*
Check that the data tree is still valid after a STEP_POST_SLICING plugin has
run.

Plugins may edit layer slices, layer-region raw slices, and layer-island slices,
but they must leave those caches mutually consistent and must not create
later-step data such as LayerRegionIsland or processed surfaces. Empty
LayerRegion raw slices remain legal after plugins for the same reason as before
plugins: a region can exist as a settings choice without being used on every
layer.

Call this after each post-slicing plugin, and once again at the end of the step.
This makes it easier to identify the first plugin that broke the expected
post-slicing contract.
*/
bool validate_post(const Print &print, std::string &error);

/*
Run all plugins registered for STEP_POST_SLICING.

This method owns the step-specific orchestration:
- validate the input tree before plugins run;
- build context for each object+plugin execution;
- expose only the callbacks that are legal during this step;
- validate the tree after each plugin and after the full step.

Orchestrator::slice() should call this for STEP_POST_SLICING instead of
duplicating the post-slicing callback setup.
*/
void run_step(Orchestrator &orchestrator, Print &print);

} // namespace Steps::StepPostSlicing

} // namespace Slic3r

#endif // steps_steppostslicing_hpp_
