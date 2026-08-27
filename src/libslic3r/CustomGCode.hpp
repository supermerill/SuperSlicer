///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/ Copyright (c) Prusa Research 2020 - 2022 Oleksandra Iushchenko @YuSanka, Vojtěch Bubník @bubnikv
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_CustomGCode_hpp_
#define slic3r_CustomGCode_hpp_

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "Api/plugin/c/slic3r_def.h"

namespace Slic3r {

class DynamicConfig;
class DynamicPrintConfig;

namespace CustomGCode {

enum Type
{
    ColorChange,
    PausePrint,
    ToolChange,
    Template,
    Custom
};

struct Item
{
    bool operator<(const Item& rhs) const { return this->print_z_ < rhs.print_z_; }
    bool operator==(const Item& rhs) const
    {
        return (rhs.print_z_     == this->print_z_    ) &&
               (rhs.type        == this->type       ) &&
               (rhs.extruder    == this->extruder   ) &&
               (rhs.color       == this->color      ) &&
               (rhs.extra       == this->extra      );
    }
    bool operator!=(const Item& rhs) const { return ! (*this == rhs); }

    coord_t     print_z_;
    Type        type;
    int         extruder;   // Informative value for ColorChangeCode and ToolChangeCode
                            // "gcode" == ColorChangeCode   => M600 will be applied for "extruder" extruder
                            // "gcode" == ToolChangeCode    => for whole print tool will be switched to "extruder" extruder
    std::string color;      // if gcode is equal to PausePrintCode, 
                            // this field is used for save a short message shown on Printer display 
    std::string extra;      // this field is used for the extra data like :
                            // - G-code text for the Type::Custom 
                            // - message text for the Type::PausePrint
};

enum Mode
{
    Undef,
    SingleExtruder,   // Single extruder printer preset is selected
    MultiAsSingle,    // Multiple extruder printer preset is selected, but 
                      // this mode works just for Single extruder print 
                      // (The same extruder is assigned to all ModelObjects and ModelVolumes).
    MultiExtruder     // Multiple extruder printer preset is selected
};

// string anlogue of custom_code_per_height mode
static constexpr char SingleExtruderMode[] = "SingleExtruder";
static constexpr char MultiAsSingleMode [] = "MultiAsSingle";
static constexpr char MultiExtruderMode [] = "MultiExtruder";

struct Info
{
    Mode mode = Undef;
    std::vector<Item> gcodes;

    bool operator==(const Info& rhs) const
    {
        if (rhs.gcodes.empty() && this->gcodes.empty())
            return true; // don't respect to the comparison of the mode, when g_codes are empty
        return  (rhs.mode   == this->mode   ) &&
                (rhs.gcodes == this->gcodes );
    }
    bool operator!=(const Info& rhs) const { return !(*this == rhs); }
};

/*
Names used by the host-owned PrintRecord table that exposes the model's custom
G-code markers to plugins. The channel is a read-only snapshot for consumers:
the Model remains the editable and persistent source of these values.
*/
inline constexpr char PrintRecordChannel[]        = "custom_gcode_per_print_z";
inline constexpr char PrintRecordSizeKey[]        = "size";
inline constexpr char PrintRecordModeKey[]        = "mode";
inline constexpr char PrintRecordPrintZKey[]      = "print_z";
inline constexpr char PrintRecordTypeKey[]        = "type";
inline constexpr char PrintRecordExtruderKey[]    = "extruder";
inline constexpr char PrintRecordColorKey[]       = "color";
inline constexpr char PrintRecordExtraKey[]       = "extra";

/*
Build the complete column-oriented snapshot published in PrintRecords.

Every vector column has exactly `size` entries in Model order. An empty source
still produces all columns so consumers never need a second schema for the
zero-row case.
*/
DynamicConfig make_print_record(const Info &info);

// If loaded configuration has a "colorprint_heights" option (if it was imported from older Slicer), 
// and if CustomGCode::Info.gcodes is empty (there is no color print data available in a new format
// then CustomGCode::Info.gcodes should be updated considering this option.
extern void update_custom_gcode_per_print_z_from_config(Info& info, DynamicPrintConfig* config);

// If information for custom Gcode per print Z was imported from older Slicer, mode will be undefined.
// So, we should set CustomGCode::Info.mode should be updated considering code values from items.
extern void check_mode_for_custom_gcode_per_print_z(Info& info);

// Return pairs of <print_z, 1-based extruder ID> sorted by increasing print_z from custom_gcode_per_print_z.
// print_z corresponds to the first layer printed with the new extruder.
std::vector<std::pair<coord_t, uint16_t>> custom_tool_changes(const Info& custom_gcode_per_print_z, size_t num_extruders);

// Return pairs of <print_z, 1-based extruder ID> sorted by increasing print_z from custom_gcode_per_print_z.
// Where print_z corresponds to the layer on which we perform a color change for the specified extruder.
std::vector<std::pair<coord_t, uint16_t>> custom_color_changes(const Info& custom_gcode_per_print_z, size_t num_extruders);

} // namespace CustomGCode

} // namespace Slic3r



#endif /* slic3r_CustomGCode_hpp_ */
