///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_extrusion_h_
#define slic3r_extrusion_h_

#include <stddef.h>
#include <stdint.h>

#include "slic3r_def.h"
#include "slic3r_geometry.h"
#include "slic3r_utils.h"

#ifdef __cplusplus
extern "C" {
#endif
    /* ========================= EXTRUSION ROLE ========================= */

/*
Extrusion role bitmask (mapped from ExtrusionRoleModifier / ExtrusionRole).
Multiple flags can be combined.
*/
typedef int32_t raw_extrusion_role;

/* No role */
#define RAW_EXTRUSION_ROLE_NONE                 0

/* Base types */

/* Perimeter (internal / external / overhang) */
#define RAW_EXTRUSION_ROLE_PERIMETER            (1 << 0)

/* Infill */
#define RAW_EXTRUSION_ROLE_INFILL               (1 << 1)

/* Support material */
#define RAW_EXTRUSION_ROLE_SUPPORT              (1 << 2)

/* Skirt / brim */
#define RAW_EXTRUSION_ROLE_SKIRT                (1 << 3)

/* Wipe tower */
#define RAW_EXTRUSION_ROLE_WIPE_TOWER           (1 << 4)

/* Milling */
#define RAW_EXTRUSION_ROLE_MILL                 (1 << 5)

/* Modifiers */

/* External / visible */
#define RAW_EXTRUSION_ROLE_EXTERNAL             (1 << 6)

/* Solid */
#define RAW_EXTRUSION_ROLE_SOLID                (1 << 7)

/* Ironing */
#define RAW_EXTRUSION_ROLE_IRONING              (1 << 8)

/* Bridge / overhang */
#define RAW_EXTRUSION_ROLE_BRIDGE               (1 << 9)

/* Thin / gap fill / thin wall */
#define RAW_EXTRUSION_ROLE_THIN                 (1 << 10)

/* Special */

/* Mixed role */
#define RAW_EXTRUSION_ROLE_MIXED                (1 << 11)

/* Travel */
#define RAW_EXTRUSION_ROLE_TRAVEL               (1 << 12)

/* ----- Exact named combined roles from ExtrusionRole ----- */

#define RAW_EXTRUSION_ROLE_INTERNAL_PERIMETER \
    (RAW_EXTRUSION_ROLE_PERIMETER)

#define RAW_EXTRUSION_ROLE_EXTERNAL_PERIMETER \
    (RAW_EXTRUSION_ROLE_PERIMETER | RAW_EXTRUSION_ROLE_EXTERNAL)

#define RAW_EXTRUSION_ROLE_OVERHANG_PERIMETER \
    (RAW_EXTRUSION_ROLE_PERIMETER | RAW_EXTRUSION_ROLE_BRIDGE)

#define RAW_EXTRUSION_ROLE_OVERHANG_EXTERNAL_PERIMETER \
    (RAW_EXTRUSION_ROLE_PERIMETER | RAW_EXTRUSION_ROLE_EXTERNAL | RAW_EXTRUSION_ROLE_BRIDGE)

#define RAW_EXTRUSION_ROLE_INTERNAL_INFILL \
    (RAW_EXTRUSION_ROLE_INFILL)

#define RAW_EXTRUSION_ROLE_SOLID_INFILL \
    (RAW_EXTRUSION_ROLE_INFILL | RAW_EXTRUSION_ROLE_SOLID)

#define RAW_EXTRUSION_ROLE_TOP_SOLID_INFILL \
    (RAW_EXTRUSION_ROLE_INFILL | RAW_EXTRUSION_ROLE_SOLID | RAW_EXTRUSION_ROLE_EXTERNAL)

#define RAW_EXTRUSION_ROLE_IRONING_INFILL \
    (RAW_EXTRUSION_ROLE_INFILL | RAW_EXTRUSION_ROLE_SOLID | RAW_EXTRUSION_ROLE_IRONING | RAW_EXTRUSION_ROLE_EXTERNAL)

#define RAW_EXTRUSION_ROLE_BRIDGE_INFILL \
    (RAW_EXTRUSION_ROLE_INFILL | RAW_EXTRUSION_ROLE_SOLID | RAW_EXTRUSION_ROLE_BRIDGE | RAW_EXTRUSION_ROLE_EXTERNAL)

#define RAW_EXTRUSION_ROLE_INTERNAL_BRIDGE_INFILL \
    (RAW_EXTRUSION_ROLE_INFILL | RAW_EXTRUSION_ROLE_SOLID | RAW_EXTRUSION_ROLE_BRIDGE)

#define RAW_EXTRUSION_ROLE_GAP_FILL \
    (RAW_EXTRUSION_ROLE_MIXED | RAW_EXTRUSION_ROLE_THIN)

#define RAW_EXTRUSION_ROLE_THIN_WALL \
    (RAW_EXTRUSION_ROLE_PERIMETER | RAW_EXTRUSION_ROLE_THIN | RAW_EXTRUSION_ROLE_EXTERNAL)

#define RAW_EXTRUSION_ROLE_SUPPORT_MATERIAL \
    (RAW_EXTRUSION_ROLE_SUPPORT)

#define RAW_EXTRUSION_ROLE_SUPPORT_MATERIAL_INTERFACE \
    (RAW_EXTRUSION_ROLE_SUPPORT | RAW_EXTRUSION_ROLE_EXTERNAL)

#define RAW_EXTRUSION_ROLE_MILLING \
    (RAW_EXTRUSION_ROLE_MILL)

#define RAW_EXTRUSION_ROLE_WIPE_TOWER_DEFAULT \
    (RAW_EXTRUSION_ROLE_WIPE_TOWER)

#define RAW_EXTRUSION_ROLE_WIPE_TOWER_RAMMING \
    (RAW_EXTRUSION_ROLE_WIPE_TOWER | RAW_EXTRUSION_ROLE_BRIDGE)

#define RAW_EXTRUSION_ROLE_WIPE_TOWER_WIPE \
    (RAW_EXTRUSION_ROLE_WIPE_TOWER | RAW_EXTRUSION_ROLE_SOLID)

/* Check if flag is set */
#define RAW_EXTRUSION_ROLE_HAS(role, flag) (((role) & (flag)) != 0)

/* Add flag */
#define RAW_EXTRUSION_ROLE_ADD(role, flag) ((role) |= (flag))

/* Remove flag */
#define RAW_EXTRUSION_ROLE_REMOVE(role, flag) ((role) &= ~(flag))

/* ----- Helpers matching ExtrusionRole semantics ----- */

#define RAW_EXTRUSION_ROLE_IS_PERIMETER(role) \
    RAW_EXTRUSION_ROLE_HAS((role), RAW_EXTRUSION_ROLE_PERIMETER)

#define RAW_EXTRUSION_ROLE_IS_EXTERNAL(role) \
    RAW_EXTRUSION_ROLE_HAS((role), RAW_EXTRUSION_ROLE_EXTERNAL)

#define RAW_EXTRUSION_ROLE_IS_BRIDGE(role) \
    RAW_EXTRUSION_ROLE_HAS((role), RAW_EXTRUSION_ROLE_BRIDGE)

#define RAW_EXTRUSION_ROLE_IS_EXTERNAL_PERIMETER(role) \
    (RAW_EXTRUSION_ROLE_IS_PERIMETER(role) && RAW_EXTRUSION_ROLE_IS_EXTERNAL(role))

#define RAW_EXTRUSION_ROLE_IS_INFILL(role) \
    RAW_EXTRUSION_ROLE_HAS((role), RAW_EXTRUSION_ROLE_INFILL)

#define RAW_EXTRUSION_ROLE_IS_SOLID_INFILL(role) \
    (RAW_EXTRUSION_ROLE_IS_INFILL(role) && RAW_EXTRUSION_ROLE_HAS((role), RAW_EXTRUSION_ROLE_SOLID))

#define RAW_EXTRUSION_ROLE_IS_SPARSE_INFILL(role) \
    (RAW_EXTRUSION_ROLE_IS_INFILL(role) && !RAW_EXTRUSION_ROLE_HAS((role), RAW_EXTRUSION_ROLE_SOLID))

#define RAW_EXTRUSION_ROLE_IS_SUPPORT(role) \
    RAW_EXTRUSION_ROLE_HAS((role), RAW_EXTRUSION_ROLE_SUPPORT)

#define RAW_EXTRUSION_ROLE_IS_SUPPORT_BASE(role) \
    (RAW_EXTRUSION_ROLE_IS_SUPPORT(role) && !RAW_EXTRUSION_ROLE_IS_EXTERNAL(role))

#define RAW_EXTRUSION_ROLE_IS_SUPPORT_INTERFACE(role) \
    (RAW_EXTRUSION_ROLE_IS_SUPPORT(role) && RAW_EXTRUSION_ROLE_IS_EXTERNAL(role))

#define RAW_EXTRUSION_ROLE_IS_SKIRT(role) \
    RAW_EXTRUSION_ROLE_HAS((role), RAW_EXTRUSION_ROLE_SKIRT)

#define RAW_EXTRUSION_ROLE_IS_MIXED(role) \
    RAW_EXTRUSION_ROLE_HAS((role), RAW_EXTRUSION_ROLE_MIXED)

#define RAW_EXTRUSION_ROLE_IS_TRAVEL(role) \
    RAW_EXTRUSION_ROLE_HAS((role), RAW_EXTRUSION_ROLE_TRAVEL)


/* ========================= HANDLES ========================= */

typedef struct extrusion_entity_handle extrusion_entity_handle;

/* ---- Flow ---- */
/*
contains the caracteristic of the flow, with some cached values for optimization.
*/
typedef struct c_flow
{
    coord_t width;
    coord_t spacing;
    coord_t height;
    coord_t nozzle_diameter;
    int32_t is_bridge;
    float spacing_ratio;
    double mm3_per_mm;
} c_flow;

/*
Flags for c_medial_axis_extrusion_params.

Each flag enables one optional behavior. The C++ and Python factories set a
small set of safe defaults, but a zero-initialized C struct means "no optional
behavior".
*/
#define MEDIAL_AXIS_EXTRUSION_TRIM_THIN_ENDPOINTS ((uint32_t)(1u << 0))
#define MEDIAL_AXIS_EXTRUSION_CAN_REVERSE         ((uint32_t)(1u << 1))
#define MEDIAL_AXIS_EXTRUSION_CONSTANT_WIDTH      ((uint32_t)(1u << 2))
#define MEDIAL_AXIS_EXTRUSION_KEEP_EMPTY_ROOT     ((uint32_t)(1u << 3))

/*
Input for the future medial-axis-to-extrusion helper.

This helper will take one ExPolygon, compute medial-axis centerlines, then turn
those centerlines into extrusion entities using the target flow below. It is a
single parameter block because the geometric medial-axis stage and the extrusion
conversion stage share the same physical assumptions: especially flow.height.

All coord_t fields are scaled distances. The flow fields are also scaled where
c_flow says so. A value of 0 for optional coord_t fields means "use the host
default" or "disable that optional behavior", as documented per field.
*/
typedef struct c_medial_axis_extrusion_params {
    /*
    Optional area where line endpoints may extend to find an anchor. NULL means
    endpoints are constrained to the source ExPolygon itself.
    */
    const expolygon_handle *extension_area;

    /*
    Minimum geometric width accepted by the medial-axis stage. Parts thinner
    than this are removed or trimmed.
    */
    coord_t min_medial_width;

    /*
    Maximum width the medial-axis stage is expected to cover with one variable
    width line. This value guides filtering, simplification, endpoint extension
    and branch cleanup.
    */
    coord_t max_medial_width;

    /*
    Optional minimum extrusion width. When greater than min_medial_width, very
    thin accepted lines may be grown to this width before extrusion conversion.
    A value of 0 means "same as min_medial_width".
    */
    coord_t min_extrusion_width;

    /*
    Optional maximum extrusion width. When smaller than max_medial_width, wide
    accepted lines may be clamped to this width before extrusion conversion. A
    value of 0 means "same as max_medial_width".
    */
    coord_t max_extrusion_width;

    /*
    Optional minimum centerline length kept by the medial-axis cleanup. A value
    of 0 lets the host choose the legacy/default threshold.
    */
    coord_t min_centerline_length;

    /*
    Optional extra endpoint extension length. The extension is clipped by
    extension_area when it is set, otherwise by the source ExPolygon itself. A
    value of 0 disables this extra extension.
    */
    coord_t endpoint_extension_length;

    /*
    Optional taper length at line endpoints. Over this distance, the generated
    width profile is progressively reduced toward the endpoint instead of
    keeping the full local medial width. This applies to the final centerline
    endpoints after optional endpoint extension: it can taper normal endpoints
    even when endpoint_extension_length is 0. A value of 0 disables tapering.
    */
    coord_t endpoint_taper_length;

    /*
    Role and flow assigned to the generated extrusion entities. flow.height is
    the only height used by this helper; there is no separate medial-axis height
    field, to avoid inconsistent geometric and extrusion assumptions.
    */
    raw_extrusion_role role;
    c_flow flow;

    /*
    Maximum segment length used when variable width changes along a centerline.
    It controls how finely a changing width profile is discretized into normal
    extrusion moves.
    */
    coord_t variable_width_resolution;

    /*
    Width variation tolerated before the converter starts a new extrusion with
    a different flow. A value of 0 means "use the host default". If the
    CONSTANT_WIDTH flag is set, this field is ignored and every output path
    uses the target flow unchanged.
    */
    coord_t width_change_tolerance;

    /*
    Optional final safety filter for generated extrusion entities. Output paths
    shorter than this may be dropped or merged by the implementation. A value of
    0 disables the explicit filter.
    */
    coord_t min_extrusion_length;

    uint32_t flags;
} c_medial_axis_extrusion_params;

/*
Compute medial-axis centerlines for src and convert them directly into an
extrusion entity tree owned by storage.

The returned handle is storage-owned. A plugin may inspect it, move it into a
host-owned tree, or release it with storage_free(). When no printable centerline
is produced, the function returns NULL unless KEEP_EMPTY_ROOT is enabled.
*/
SLIC3R_HOST_API extrusion_entity_handle *expolygon_medial_axis_extrusion(
    storage_handle *storage,
    const expolygon_handle *src,
    const c_medial_axis_extrusion_params *params);


#ifdef __cplusplus
}
#endif

#endif // slic3r_extrusion_h_
