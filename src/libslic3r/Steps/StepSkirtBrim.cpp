
///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "StepSkirtBrim.hpp"

namespace Slic3r::Steps::StepSkirtBrim {

void clean_and_prepare(Print &) {}

bool validate_pre(const Print &, std::string *)
{
    return true;
}

bool validate_post(const Print &, std::string *)
{
    return true;
}

void run_step(Orchestrator &, Print &) {}

} // namespace Slic3r::Steps::StepSkirtBrim
