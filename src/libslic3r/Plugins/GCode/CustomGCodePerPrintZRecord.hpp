///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Plugins_GCode_CustomGCodePerPrintZRecord_hpp_
#define slic3r_Plugins_GCode_CustomGCodePerPrintZRecord_hpp_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "libslic3r/CustomGCode.hpp"

namespace slic3r_api {
class Print;
class PrintingExtrusion;
class PrintingGroup;
class PrintingPlan;
}

namespace slic3r_api::GCodeGeneration::CustomGCodePerPrintZPlugin {

/*
Validated row copied from the host-owned custom_gcode_per_print_z PrintRecord.

The record remains a generic Config table at the ABI boundary. Converting it
once into this value type gives every consumer the same enum checks, column
length checks and model-order semantics without retaining borrowed Config
handles while the PrintingPlan is mutated.
*/
struct CustomGCodeRow
{
    double print_z_mm = 0.0;
    Slic3r::CustomGCode::Type type = Slic3r::CustomGCode::Custom;
    int32_t extruder = 0;
    std::string color;
    std::string extra;
    uint32_t source_row = 0;
};

struct CustomGCodeTable
{
    Slic3r::CustomGCode::Mode mode = Slic3r::CustomGCode::Undef;
    std::vector<CustomGCodeRow> rows;
};

/*
Read and validate the complete record before any plan mutation begins.

An absent channel is accepted as an empty optional so older hosts or tests may
run the plugins without publishing this host-owned table. A present but invalid
channel throws std::runtime_error and must abort the current plugin pass.
*/
std::optional<CustomGCodeTable> read_custom_gcode_per_print_z(const Print &print);

/* Resolve the old Undef mode with the same semantic rules as the Model helper. */
Slic3r::CustomGCode::Mode effective_record_mode(const CustomGCodeTable &table);

/* Infer the active print mode from the final object extrusions in the plan. */
Slic3r::CustomGCode::Mode printing_plan_mode(const Print &print, const PrintingPlan &plan);

/* Return true only for extrusion roots sourced from a normal object layer. */
bool printing_extrusion_is_object(const PrintingGroup &group, const PrintingExtrusion &extrusion);

/* Map every row to the first layer at or above its Z, preserving row order. */
std::vector<std::vector<const CustomGCodeRow *>> rows_by_layer(
    const CustomGCodeTable &table,
    const PrintingGroup &group);

} // namespace slic3r_api::GCodeGeneration::CustomGCodePerPrintZPlugin

#endif // slic3r_Plugins_GCode_CustomGCodePerPrintZRecord_hpp_
