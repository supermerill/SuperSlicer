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
STEP_SKIRT_BRIM runs first-layer adhesion plugins.

The step is a plugin chain, not one exclusive generator. A brim plugin and a
future skirt plugin can both run in the same step, while each sub-feature may
declare its own exclusive group for alternative implementations. The host owns
the final Print/Object storage and exposes narrow callbacks so plugins can
publish extrusion trees without reaching into private Print fields.
*/

void clean_and_prepare(Print &print);
bool validate_pre(const Print &print, std::string *error = nullptr);
bool validate_post(const Print &print, std::string *error = nullptr);
void run_step(Orchestrator &orchestrator, Print &print);

} // namespace Steps::StepSkirtBrim
} // namespace Slic3r

#endif // steps_stepskirtbrim_hpp_
