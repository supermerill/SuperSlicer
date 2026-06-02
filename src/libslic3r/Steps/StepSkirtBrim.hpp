///|/ Copyright (c) SuperSlicer 2026 Durand R?mi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef steps_stepskirtbrim_hpp_
#define steps_stepskirtbrim_hpp_

#include <string>

#include "libslic3r/Api/plugin/c/slic3r_plugin_types.h"

namespace Slic3r {
class Orchestrator;
class Print;

namespace Steps::StepSkirtBrim {

/*
STEP_SKIRT_BRIM is the future plugin slot for first-layer adhesion geometry.

The legacy skirt/brim generator still lives outside this step, so the current
implementation is intentionally empty. Keeping a real step object in the
pipeline lets config invalidation and future plugins target skirt/brim without
pretending they depend on perimeter, infill or support generation.
*/

void clean_and_prepare(Print &print);
bool validate_pre(const Print &print, std::string *error = nullptr);
bool validate_post(const Print &print, std::string *error = nullptr);
void run_step(Orchestrator &orchestrator, Print &print);

} // namespace Steps::StepSkirtBrim
} // namespace Slic3r

#endif // steps_stepskirtbrim_hpp_
