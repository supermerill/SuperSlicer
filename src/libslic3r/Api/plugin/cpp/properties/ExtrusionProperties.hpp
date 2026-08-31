///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_properties_ExtrusionProperties_hpp_
#define slic3r_Api_plugin_cpp_properties_ExtrusionProperties_hpp_

/*
Built-in extrusion properties
=============================

Extrusion entities store small C payloads describing their physical process,
geometry classification, or generated commands. This header gives every fixed
payload a typed PluginPropertyKey and concise C++ accessors while preserving the
exact layout consumed by the C plugin ABI.

These helpers describe only one directly stored payload. Inherited lookup over
an extrusion tree remains the responsibility of ExtrusionTreeVisitors.

Developer guide: [Using Plugin Properties](../../../../../../doc/plugins/properties.md)

Entity guide: [Using Unified Extrusion Entities](../../../../../../doc/plugins/extrusions.md)
*/

#include <cstdint>

#include "libslic3r/Api/plugin/c/slic3r_extrusion_property.h"
#include "libslic3r/Api/plugin/cpp/PluginPropertyKey.hpp"

namespace slic3r_api {

template<class Derived, class Payload, extrusion_property_type TypeValue>
using EPropertyPayload = BuiltInPluginPropertyPayload<Derived, Payload, TypeValue>;

/* Physical attributes inherited by printable descendants. */
struct EPropertyAttributes :
    EPropertyPayload<EPropertyAttributes, c_extrusion_property_attributes, EXTRUSION_PROPERTY_TYPE_ATTRIBUTES>
{
    EPropertyAttributes &extrusion_role(int32_t value) { role = value; return *this; }
    int32_t extrusion_role() const { return role; }
    EPropertyAttributes &no_seam_enabled(bool enabled = true) { no_seam = enabled ? 1 : 0; return *this; }
    bool no_seam_enabled() const { return no_seam != 0; }
    EPropertyAttributes &mm3_per_mm(double value) { c_extrusion_property_attributes::mm3_per_mm = value; return *this; }
    EPropertyAttributes &width(float value) { c_extrusion_property_attributes::width = value; return *this; }
    EPropertyAttributes &height(float value) { c_extrusion_property_attributes::height = value; return *this; }
};

/* Optional process values selected by earlier printing-plan steps. */
struct EPropertySpeed :
    EPropertyPayload<EPropertySpeed, c_extrusion_property_speed, EXTRUSION_PROPERTY_TYPE_SPEED>
{
    EPropertySpeed &speed(float value) { speed_mm_per_s = value; return *this; }
    EPropertySpeed &acceleration(float value) { accel_mm_per_s2 = value; return *this; }
    EPropertySpeed &pressure_advance(float value) { pressure_adv = value; return *this; }
    EPropertySpeed &fan_speed(float value) { fan_speed_percent = value; return *this; }
    EPropertySpeed &temperature(float value) { temperature_C = value; return *this; }
};

/* Travel, retraction, lift, and tool-change movement modifiers. */
struct EPropertyModifier :
    EPropertyPayload<EPropertyModifier, c_extrusion_property_modifier, EXTRUSION_PROPERTY_TYPE_MODIFIER>
{
    EPropertyModifier &set_enforce_travel(bool enabled = true) { enforce_travel = enabled ? 1 : 0; return *this; }
    EPropertyModifier &set_enforce_retraction(bool enabled = true) { enforce_retraction = enabled ? 1 : 0; return *this; }
    EPropertyModifier &set_enforce_unlift(bool enabled = true) { enforce_unlift = enabled ? 1 : 0; return *this; }
    EPropertyModifier &set_disable_retraction(bool enabled = true) { disable_retraction = enabled ? 1 : 0; return *this; }
    EPropertyModifier &set_disable_lift(bool enabled = true) { disable_lift = enabled ? 1 : 0; return *this; }
    EPropertyModifier &set_toolchange_retraction(bool enabled = true) { toolchange_retraction = enabled ? 1 : 0; return *this; }
};

/* Stored text, script metadata, and optional serialized script configuration. */
struct EPropertyCustomGcode :
    EPropertyPayload<EPropertyCustomGcode, c_extrusion_property_custom_gcode, EXTRUSION_PROPERTY_TYPE_CUSTOM_GCODE>
{
    EPropertyCustomGcode &set_kind(c_extrusion_custom_gcode_kind value) {
        kind = value;
        script_type = value == C_EXTRUSION_CUSTOM_GCODE_SCRIPT ? GCODE_SCRIPT_TYPE_EXTRUSION_CUSTOM :
                                                                 GCODE_SCRIPT_TYPE_INVALID;
        return *this;
    }
    EPropertyCustomGcode &set_script_type(gcode_script_type value) {
        script_type = value;
        return *this;
    }
    EPropertyCustomGcode &text(extrusion_data_id value) { text_id = value; return *this; }
    EPropertyCustomGcode &processing_extruder(uint16_t value) { processing_extruder_id = value; return *this; }
};

/* A non-geometric command embedded in the ordered extrusion stream. */
struct EPropertySpecialCommand :
    EPropertyPayload<EPropertySpecialCommand, c_extrusion_property_special_command, EXTRUSION_PROPERTY_TYPE_SPECIAL_COMMAND>
{
    EPropertySpecialCommand &set(c_extrusion_special_command value, double data = 0.) {
        code = value;
        extra_data = data;
        return *this;
    }
};

/* Support-distance and dynamic overhang processing metadata. */
struct EPropertyOverhang :
    EPropertyPayload<EPropertyOverhang, c_extrusion_property_overhang, EXTRUSION_PROPERTY_TYPE_OVERHANG>
{
    EPropertyOverhang &distance(float start, float end) {
        start_distance_from_prev_layer = start;
        end_distance_from_prev_layer = end;
        return *this;
    }
    EPropertyOverhang &curled_proximity(float value) { proximity_to_curled_lines = value; return *this; }
    EPropertyOverhang &full_overhangs_flow(bool enabled = true) { has_full_overhangs_flow = enabled ? 1 : 0; return *this; }
    EPropertyOverhang &full_overhangs_speed(bool enabled = true) { has_full_overhangs_speed = enabled ? 1 : 0; return *this; }
    EPropertyOverhang &dynamic_overhangs_flow(bool enabled = true) { has_dynamic_overhangs_flow = enabled ? 1 : 0; return *this; }
    EPropertyOverhang &dynamic_overhangs_speed(bool enabled = true) { has_dynamic_overhangs_speed = enabled ? 1 : 0; return *this; }
};

/* Constant Z offset applied while generating this entity. */
struct EPropertyZOffset :
    EPropertyPayload<EPropertyZOffset, c_extrusion_property_z_offset, EXTRUSION_PROPERTY_TYPE_Z_OFFSET>
{
    EPropertyZOffset &set(coord_t value) { z_offset = value; return *this; }
    coord_t get() const { return z_offset; }
};

/* Shell index and loop classification assigned by perimeter generators. */
struct EPropertyPerimeter :
    EPropertyPayload<EPropertyPerimeter, c_extrusion_property_perimeter, EXTRUSION_PROPERTY_TYPE_PERIMETER>
{
    EPropertyPerimeter &shell_count(int16_t value) { perimeter_idx = value; return *this; }
    int16_t shell_count() const { return perimeter_idx; }
    EPropertyPerimeter &perimeter_flags(uint16_t value) { c_extrusion_property_perimeter::perimeter_flags = value; return *this; }
    uint16_t perimeter_flags() const { return c_extrusion_property_perimeter::perimeter_flags; }
};

/* Source-surface provenance retained by generated infill trees. */
struct EPropertyInfill :
    EPropertyPayload<EPropertyInfill, c_extrusion_property_infill, EXTRUSION_PROPERTY_TYPE_INFILL>
{
    EPropertyInfill &surface_id(uint64_t value) { source_surface_id = value; return *this; }
    uint64_t surface_id() const { return source_surface_id; }
};

// Each helper is a direct typed view over the corresponding host-owned C bytes.
static_assert(sizeof(EPropertyAttributes) == sizeof(c_extrusion_property_attributes), "ABI payload mismatch");
static_assert(sizeof(EPropertySpeed) == sizeof(c_extrusion_property_speed), "ABI payload mismatch");
static_assert(sizeof(EPropertyModifier) == sizeof(c_extrusion_property_modifier), "ABI payload mismatch");
static_assert(sizeof(EPropertyCustomGcode) == sizeof(c_extrusion_property_custom_gcode), "ABI payload mismatch");
static_assert(sizeof(EPropertySpecialCommand) == sizeof(c_extrusion_property_special_command), "ABI payload mismatch");
static_assert(sizeof(EPropertyOverhang) == sizeof(c_extrusion_property_overhang), "ABI payload mismatch");
static_assert(sizeof(EPropertyZOffset) == sizeof(c_extrusion_property_z_offset), "ABI payload mismatch");
static_assert(sizeof(EPropertyPerimeter) == sizeof(c_extrusion_property_perimeter), "ABI payload mismatch");
static_assert(sizeof(EPropertyInfill) == sizeof(c_extrusion_property_infill), "ABI payload mismatch");

} // namespace slic3r_api

#endif // slic3r_Api_plugin_cpp_properties_ExtrusionProperties_hpp_
