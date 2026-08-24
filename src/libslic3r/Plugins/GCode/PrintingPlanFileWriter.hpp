///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Plugins_GCode_PrintingPlanFileWriter_hpp_
#define slic3r_Plugins_GCode_PrintingPlanFileWriter_hpp_

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"

namespace slic3r_api { namespace GCodeGeneration { namespace PrintingPlanFileWriterPlugin {

/*
Register the sequential STEP_GCODE file writer.

This built-in plugin writes every chunk returned by the selected firmware
session and publishes the resulting file atomically. It never interprets an
extrusion or assembles a G-code command itself.
*/
void register_printing_plan_file_writer_plugin(orchestrator_handle *orchestrator);

}}} // namespace slic3r_api::GCodeGeneration::PrintingPlanFileWriterPlugin

#endif // slic3r_Plugins_GCode_PrintingPlanFileWriter_hpp_
