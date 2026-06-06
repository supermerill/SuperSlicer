///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_volume_h_
#define slic3r_volume_h_

#include <stdint.h>

#include "slic3r_data_tree.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================= HANDLES ========================= */

typedef struct volume_handle volume_handle;
typedef struct triangle_mesh_handle triangle_mesh_handle;
typedef struct slicing_layer_range_handle slicing_layer_range_handle;
typedef struct slicing_volume_region_handle slicing_volume_region_handle;

/* ========================= BASIC VALUE TYPES ========================= */

typedef struct c_triangle_indices {
    uint32_t a;
    uint32_t b;
    uint32_t c;
} c_triangle_indices;

/*
Low-level mesh slicing mode, matching MeshSlicingParams::SlicingMode.
Plugins should normally derive this from the Object's "slicing_mode" config:
Regular -> REGULAR, EvenOdd -> EVEN_ODD, CloseHoles -> POSITIVE.
*/
typedef enum raw_mesh_slicing_mode {
    RAW_MESH_SLICING_MODE_REGULAR                  = 0,
    RAW_MESH_SLICING_MODE_EVEN_ODD                 = 1,
    RAW_MESH_SLICING_MODE_POSITIVE                 = 2,
    RAW_MESH_SLICING_MODE_POSITIVE_LARGEST_CONTOUR = 3
} raw_mesh_slicing_mode;

/*
C ABI mirror of MeshSlicingParamsEx.

All distances are unscaled millimeters because TriangleMesh vertices and the
low-level slicer work in float/double model coordinates. The transform is also
unscaled and is applied before slicing.
*/
typedef struct c_mesh_slicing_params {
    raw_mesh_slicing_mode mode;
    uint32_t slicing_mode_normal_below_layer;
    raw_mesh_slicing_mode mode_below;
    c_matrix4d transform;
    float closing_radius;
    float extra_offset;
    double resolution;
    double model_resolution;
} c_mesh_slicing_params;

/* ========================= VOLUME TYPE ========================= */

/*
Stable C mirror of the host volume type enum.
Keep the numeric values aligned with the host enum because plugins may persist
or compare them without including C++ headers.
*/
typedef enum raw_volume_type {
    RAW_VOLUME_TYPE_INVALID                     = -1,
    RAW_VOLUME_TYPE_MODEL_PART                  = 0,
    RAW_VOLUME_TYPE_NEGATIVE_VOLUME             = 1,
    RAW_VOLUME_TYPE_PARAMETER_MODIFIER          = 2,
    RAW_VOLUME_TYPE_SUPPORT_BLOCKER             = 3,
    RAW_VOLUME_TYPE_SUPPORT_ENFORCER            = 4,
    RAW_VOLUME_TYPE_SEAM_POSITION_CENTER        = 5,
    RAW_VOLUME_TYPE_SEAM_POSITION_CENTER_Z      = 6,
    RAW_VOLUME_TYPE_SEAM_POSITION_INSIDE_CENTER = 7,
    RAW_VOLUME_TYPE_SEAM_POSITION_INSIDE        = 8,
    RAW_VOLUME_TYPE_BRIM_PATCH                  = 9,
    RAW_VOLUME_TYPE_BRIM_NEGATIVE               = 10
} raw_volume_type;

/* ========================= FACET PAINTING ========================= */

/*
Painting stored on a model volume facet selection.

Every painting kind is identified by a stable UTF-8 key. Built-in paintings
use the three keys below; plugin FacetsAnnotation kinds use the key passed to
orchestrator_register_generic_facets_annotation().

The host decides internally whether a key maps to older hardcoded storage
(supports / MMU) or to generic plugin-style storage. Plugin code does not need
separate APIs for those cases.

FDM support painting, seam painting and plugin FacetsAnnotation kinds use
RAW_FACET_PAINTING_ENFORCER / RAW_FACET_PAINTING_BLOCKER as values.

MMU painting uses the value as a 1-based extruder selector:
1 means the first extruder, 2 means the second extruder, and so on. Passing
RAW_FACET_PAINTING_NONE returns no painted facets.
*/
#define RAW_FACET_PAINTING_FDM_SUPPORT      "builtin:fdm_support"
#define RAW_FACET_PAINTING_SEAM             "builtin:seam"
#define RAW_FACET_PAINTING_MMU_SEGMENTATION "builtin:mmu_segmentation"

typedef enum raw_facet_painting_value {
    RAW_FACET_PAINTING_NONE     = 0,
    RAW_FACET_PAINTING_ENFORCER = 1,
    RAW_FACET_PAINTING_BLOCKER  = 2
} raw_facet_painting_value;

/* ========================= PRINTOBJECT -> VOLUMES ========================= */

/* Returns the number of volumes borrowed from the Object's source model data. */
SLIC3R_HOST_API uint32_t object_volume_count(const object_handle *object);

/*
Returns a borrowed read-only Volume handle.
The handle stays valid only while the underlying Object stays valid.
*/
SLIC3R_HOST_API const volume_handle *object_volume_at(const object_handle *object, uint32_t idx);

/*
Read-only access to the Object's volume-to-region assignment.

These views expose the same table used by STEP_SLICING. They are useful for
helper geometry that already has a 2D subject and only wants to know which
model-part or modifier volumes provide alternate PrintRegion settings at a Z
height. Negative volumes are still visible in this table, but helpers that do
not build object material should ignore them unless they explicitly want
negative-volume behavior.

note: this part of the api may change in the future to support more flexible volume-to-region assignment
*/
SLIC3R_HOST_API uint32_t object_count_slicing_layer_range(const object_handle *object);
SLIC3R_HOST_API const slicing_layer_range_handle *object_get_slicing_layer_range(const object_handle *object,
                                                                                 uint32_t idx);
SLIC3R_HOST_API coord_t slicing_layer_range_get_z_min(const slicing_layer_range_handle *range);
SLIC3R_HOST_API coord_t slicing_layer_range_get_z_max(const slicing_layer_range_handle *range);
SLIC3R_HOST_API const config_handle *slicing_layer_range_get_config(const slicing_layer_range_handle *range);

SLIC3R_HOST_API uint32_t slicing_layer_range_count_volume_region(const slicing_layer_range_handle *range);
SLIC3R_HOST_API const slicing_volume_region_handle *slicing_layer_range_get_volume_region(
    const slicing_layer_range_handle *range,
    uint32_t idx);

SLIC3R_HOST_API const volume_handle *slicing_volume_region_get_volume(const slicing_volume_region_handle *volume_region);
SLIC3R_HOST_API int32_t slicing_volume_region_get_parent(const slicing_volume_region_handle *volume_region);
SLIC3R_HOST_API int32_t slicing_volume_region_get_layer_region_idx(const slicing_volume_region_handle *volume_region);
SLIC3R_HOST_API c_bounding_box3f slicing_volume_region_get_bbox(const slicing_volume_region_handle *volume_region);

/* ========================= VOLUME ========================= */

SLIC3R_HOST_API raw_volume_type volume_get_type(const volume_handle *volume);
SLIC3R_HOST_API uint64_t volume_get_id(const volume_handle *volume);
SLIC3R_HOST_API const config_handle *volume_get_config(const volume_handle *volume);

/* Returns -1 if the volume has no FFF extruder assignment. */
SLIC3R_HOST_API int32_t volume_get_extruder_id(const volume_handle *volume);

SLIC3R_HOST_API c_matrix4d volume_get_matrix(const volume_handle *volume);
SLIC3R_HOST_API c_matrix4d volume_get_matrix_no_offset(const volume_handle *volume);

/*
Returns non-zero if the volume has any painted facets for paint_key.

paint_key may be one of RAW_FACET_PAINTING_* or a plugin key registered with
orchestrator_register_generic_facets_annotation().
*/
SLIC3R_HOST_API int volume_has_painting(const volume_handle *volume, const char *paint_key);

/*
Project painted facets from all model-part volumes of an Object to its layers.

out_by_layer must point to layer_count mutable polygon_collection_handle
objects, usually created by the plugin in its storage. Every destination
collection is cleared first, then replaced by the projected polygons for the
matching layer.

For paint_key RAW_FACET_PAINTING_FDM_SUPPORT:
    painting_value is RAW_FACET_PAINTING_ENFORCER or RAW_FACET_PAINTING_BLOCKER.
    Downward facing painted facets are projected upward to the slicing planes,
    matching the native support painting behavior.

For paint_key RAW_FACET_PAINTING_SEAM or a plugin generic painting key:
    painting_value is RAW_FACET_PAINTING_ENFORCER or RAW_FACET_PAINTING_BLOCKER.
    Painted facets are projected through the touched layer slabs, matching the
    native seam painting behavior.

For paint_key RAW_FACET_PAINTING_MMU_SEGMENTATION:
    painting_value is a 1-based extruder index. The host projects both top and
    bottom painted facet slabs into the output. This is a raw geometric helper;
    it does not perform the full native MMU segmentation refinement.
*/
SLIC3R_HOST_API void object_project_painting_to_polygons(const object_handle *object,
                                                         const char *paint_key,
                                                         int32_t painting_value,
                                                         polygon_collection_handle **out_by_layer,
                                                         uint32_t layer_count);

SLIC3R_HOST_API const triangle_mesh_handle *volume_get_mesh(const volume_handle *volume);

/* ========================= TRIANGLE MESH ========================= */

SLIC3R_HOST_API uint32_t triangle_mesh_vertex_count(const triangle_mesh_handle *mesh);
SLIC3R_HOST_API uint32_t triangle_mesh_triangle_count(const triangle_mesh_handle *mesh);
SLIC3R_HOST_API c_vec3f triangle_mesh_vertex_at(const triangle_mesh_handle *mesh, uint32_t idx);
SLIC3R_HOST_API c_triangle_indices triangle_mesh_triangle_at(const triangle_mesh_handle *mesh, uint32_t idx);

/*
Slice a TriangleMesh into raw polygon loops at object-local Z positions.

transform is an unscaled 3D transform matrix. slice_zs are also unscaled float
values because TriangleMesh vertices live in float 3D coordinates. If the Z
comes from a Layer view, convert Layer::slice_z() with unscaled() before calling
this function.

out_by_layer must point to slice_count mutable polygon_collection_handle objects,
usually created in plugin storage by the plugin.
Each destination collection is cleared and replaced by the polygons generated at
the matching Z.
*/
SLIC3R_HOST_API void triangle_mesh_slice_to_polygons(const triangle_mesh_handle *mesh,
                                                     c_matrix4d transform,
                                                     const float *z_mm_by_layer,
                                                     polygon_collection_handle **slices_by_layer,
                                                     uint32_t layer_count);

/*
Same as triangle_mesh_slice_to_polygons(), but exposes MeshSlicingParamsEx so a
plugin can reproduce the native PrintObject::slice_volumes() behavior.
*/
SLIC3R_HOST_API void triangle_mesh_slice_to_polygons_with_params(const triangle_mesh_handle *mesh,
                                                                 const c_mesh_slicing_params *params,
                                                                 const float *z_mm_by_layer,
                                                                 polygon_collection_handle **slices_by_layer,
                                                                 uint32_t layer_count);

SLIC3R_HOST_API polygon_collection_handle *triangle_mesh_slice_to_polygon(storage_handle *storage,
                                                                         const triangle_mesh_handle *mesh,
                                                                         c_matrix4d transform,
                                                                         float layer_z_mm);

/*
Slice a TriangleMesh into raw expolygon at object-local Z positions.

transform is an unscaled 3D transform matrix. slice_zs are also unscaled float
values because TriangleMesh vertices live in float 3D coordinates. If the Z
comes from a Layer view, convert Layer::slice_z() with unscaled() before calling
this function.

out_by_layer must point to slice_count mutable polygon_collection_handle objects,
usually created in plugin storage by the plugin.
Each destination collection is cleared and replaced by the polygons generated at
the matching Z.
*/
SLIC3R_HOST_API void triangle_mesh_slice_to_expolygons(const triangle_mesh_handle *mesh,
                                                       c_matrix4d transform,
                                                       const float *z_mm_by_layer,
                                                       expolygon_collection_handle **slices_by_layer,
                                                       uint32_t layer_count);

/*
Slice directly to ExPolygons using MeshSlicingParamsEx. This is the preferred
entry point for STEP_SLICING plugins because it follows the same low-level path
as PrintObjectSlice.cpp::slice_volumes_inner().
*/
SLIC3R_HOST_API void triangle_mesh_slice_to_expolygons_with_params(const triangle_mesh_handle *mesh,
                                                                   const c_mesh_slicing_params *params,
                                                                   const float *z_mm_by_layer,
                                                                   expolygon_collection_handle **slices_by_layer,
                                                                   uint32_t layer_count);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* slic3r_volume_h_ */
