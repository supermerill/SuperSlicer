///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Plugins_GCode_PrintingPlanFileWriter_hpp_
#define slic3r_Plugins_GCode_PrintingPlanFileWriter_hpp_

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"

namespace slic3r_api { namespace GCodeGeneration { namespace PrintingPlanFileWriterPlugin {

/*
Register the prototype STEP_GCODE writer.

This built-in plugin is the first consumer of PrintingPlan at the G-code step.
It is intentionally a small file writer, not a printer motion generator: the
output helps developers verify that STEP_ORDERING produced a stable ordered
plan and that STEP_GCODE receives the final destination path.
*/
void register_printing_plan_file_writer_plugin(orchestrator_handle *orch);

}}} // namespace slic3r_api::GCodeGeneration::PrintingPlanFileWriterPlugin

#endif // slic3r_Plugins_GCode_PrintingPlanFileWriter_hpp_
