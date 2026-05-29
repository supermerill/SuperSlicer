///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_data_tree_h_
#define slic3r_data_tree_h_

#include <stddef.h>
#include <stdint.h>

#include "slic3r_config_option.h"
#include "slic3r_def.h"
#include "slic3r_extrusions.h"
#include "slic3r_geometry.h"
#include "slic3r_utils.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================= HANDLES ========================= */

typedef struct print_handle print_handle;
typedef struct print_region_handle print_region_handle;
typedef struct object_handle object_handle;
typedef struct layer_handle layer_handle;
typedef struct layer_region_handle layer_region_handle;
typedef struct layer_island_handle layer_island_handle;
typedef struct layer_region_island_handle layer_region_island_handle;
typedef struct surface_handle surface_handle;
typedef struct surface_collection_handle surface_collection_handle;
typedef struct config_handle config_handle;

/* ========================= SURFACE TYPE ========================= */

/*
Surface type bitmask (mapped from SurfaceType).
Multiple flags can be combined.
*/
typedef uint16_t raw_surface_type;

/* No surface */
#define RAW_SURFACE_TYPE_NONE 0

/* Position flags */

/* Top horizontal surface, visible from the top */
#define RAW_SURFACE_TYPE_POS_TOP (1 << 0)

/* Bottom horizontal surface, visible from the bottom */
#define RAW_SURFACE_TYPE_POS_BOTTOM (1 << 1)

/* Internal sparse infill */
#define RAW_SURFACE_TYPE_POS_INTERNAL (1 << 2)

/* Inner/outer perimeters (mainly for coloring) */
#define RAW_SURFACE_TYPE_POS_PERIMETER (1 << 3)

/* Density flags */

/* Solid infill (100%) */
#define RAW_SURFACE_TYPE_DENS_SOLID (1 << 4)

/* Sparse infill (>0% & <100%) */
#define RAW_SURFACE_TYPE_DENS_SPARSE (1 << 5)

/* Void / combined sparse layers */
#define RAW_SURFACE_TYPE_DENS_VOID (1 << 6)

/* Modifier flags */

/* First bridging layer */
#define RAW_SURFACE_TYPE_MOD_BRIDGE (1 << 7)

/* Second layer over bridge */
#define RAW_SURFACE_TYPE_MOD_OVERBRIDGE (1 << 8)

/* Check if flag is set */
#define RAW_SURFACE_TYPE_HAS(type, flag) (((type) & (flag)) != 0)

/* Add flag */
#define RAW_SURFACE_TYPE_ADD(type, flag) ((type) |= (flag))

/* Remove flag */
#define RAW_SURFACE_TYPE_REMOVE(type, flag) ((type) &= ~(flag))

/* ========================= SURFACE ========================= */

struct c_surface
{
    const expolygon_handle *expolygon;
    raw_surface_type type;
};
/*
Snapshot one Surface into a tiny C value. Prefer the handle API below when the
Surface may grow new fields, or when you need to mutate it.
*/
SLIC3R_HOST_API c_surface surface_c_view(const surface_handle *me);

/* Borrow the geometry owned by a read-only Surface view. */
SLIC3R_HOST_API const expolygon_handle *surface_get_expolygon(const surface_handle *me);

/* Surface type bitmask access. Surfaces are read-only views in the public API. */
SLIC3R_HOST_API raw_surface_type surface_get_type(const surface_handle *me);

/* Convenience helper for one flag inside the surface type bitmask. */
SLIC3R_HOST_API int32_t surface_get_flag(const surface_handle *me, raw_surface_type flag);

/*
Surface collection storage.

Surface remains a read-only data-tree view. Plugins create temporary
SurfaceCollections in their storage, fill them with copied or moved ExPolygons,
then pass the whole collection to a step callback that moves it into the host
data tree.
*/
SLIC3R_HOST_API surface_collection_handle *storage_new_surface_collection(storage_handle *storage);
SLIC3R_HOST_API void surface_collection_clear(surface_collection_handle *me);
SLIC3R_HOST_API void surface_collection_append_expolygon_copy(surface_collection_handle *me,
                                                              const expolygon_handle *area,
                                                              raw_surface_type surface_type);
SLIC3R_HOST_API void surface_collection_append_expolygon_move(surface_collection_handle *me,
                                                              expolygon_handle *area,
                                                              raw_surface_type surface_type);
SLIC3R_HOST_API void surface_collection_append_expolygons_copy(surface_collection_handle *me,
                                                               const expolygon_collection_handle *areas,
                                                               raw_surface_type surface_type);
SLIC3R_HOST_API void surface_collection_append_expolygons_move(surface_collection_handle *me,
                                                               expolygon_collection_handle *areas,
                                                               raw_surface_type surface_type);
SLIC3R_HOST_API uint32_t surface_collection_size(const surface_collection_handle *me);
SLIC3R_HOST_API const surface_handle *surface_collection_at(const surface_collection_handle *me, uint32_t idx);

/* ========================= LAYER ========================= */

SLIC3R_HOST_API coord_t layer_get_height(const layer_handle *me);
SLIC3R_HOST_API coord_t layer_get_print_z(const layer_handle *me);
/* Center Z of the slicing plane. It is exactly print_z - height / 2. */
SLIC3R_HOST_API coord_t layer_get_slice_z(const layer_handle *me);
/* is == -1 if this layer isn't a support layer. */
SLIC3R_HOST_API coord_t layer_get_support_id(const layer_handle *me);

SLIC3R_HOST_API const expolygon_collection_handle *layer_get_slices(const layer_handle *me);

SLIC3R_HOST_API layer_handle *layer_get_upper_layer_mutable(layer_handle *me);
SLIC3R_HOST_API const layer_handle *layer_get_upper_layer(const layer_handle *me);

SLIC3R_HOST_API layer_handle *layer_get_lower_layer_mutable(layer_handle *me);
SLIC3R_HOST_API const layer_handle *layer_get_lower_layer(const layer_handle *me);

// tag that can be used by processed to store some information
SLIC3R_HOST_API void layer_set_tag(layer_handle *me, const char *tag, double value);
SLIC3R_HOST_API double layer_get_tag(const layer_handle *me, const char *tag);

SLIC3R_HOST_API uint32_t layer_count_region(const layer_handle *me);
SLIC3R_HOST_API layer_region_handle *layer_get_region_mutable(layer_handle *me, uint32_t idx);
SLIC3R_HOST_API const layer_region_handle *layer_get_region(const layer_handle *me, uint32_t idx);

SLIC3R_HOST_API uint32_t layer_count_island(const layer_handle *me);
SLIC3R_HOST_API layer_island_handle *layer_get_island_mutable(layer_handle *me, uint32_t idx);
SLIC3R_HOST_API const layer_island_handle *layer_get_island(const layer_handle *me, uint32_t idx);

/* ========================= LAYER REGION ========================= */

/* tag that can be used by processed to store some information */
SLIC3R_HOST_API void layer_region_set_tag(layer_region_handle *me, const char *tag, double value);
SLIC3R_HOST_API double layer_region_get_tag(const layer_region_handle *me, const char *tag);

SLIC3R_HOST_API c_flow layer_region_get_flow(const layer_region_handle *me, raw_extrusion_role flow_role);
SLIC3R_HOST_API const expolygon_collection_handle *layer_region_get_slices(const layer_region_handle *me);
SLIC3R_HOST_API c_bounding_box layer_region_get_bounding_box(const layer_region_handle *me);

SLIC3R_HOST_API const layer_handle *layer_region_get_layer(const layer_region_handle *me);
SLIC3R_HOST_API const print_region_handle *layer_region_get_print_region(const layer_region_handle *me);

/* ========================= LAYER ISLAND ========================= */

SLIC3R_HOST_API expolygon_handle *layer_island_get_slice_mutable(layer_island_handle *me);
SLIC3R_HOST_API const expolygon_handle *layer_island_get_slice(const layer_island_handle *me);
SLIC3R_HOST_API c_bounding_box layer_island_get_bounding_box(const layer_island_handle *me);
/* give an expolygon included inside get_slice()  where the infill has to be extruded. */
SLIC3R_HOST_API const expolygon_handle *layer_island_get_infill_slice(const layer_island_handle *me);
/* All ExPolygons where infill has to be extruded. Prefer this when a previous step split the island fill area. */
SLIC3R_HOST_API const expolygon_collection_handle *layer_island_get_infill_areas(const layer_island_handle *me);
SLIC3R_HOST_API c_bounding_box layer_island_get_infill_bounding_box(const layer_island_handle *me);
/* give an expolygon included inside get_infill_slice() where the infill may be extruded if there was no
 * infill-perimeter encroachment. */
SLIC3R_HOST_API const expolygon_handle *layer_island_get_infill_no_overlap_slice(const layer_island_handle *me);
/* All no-overlap/free infill ExPolygons. */
SLIC3R_HOST_API const expolygon_collection_handle *layer_island_get_infill_no_overlap_areas(const layer_island_handle *me);

/* tag that can be used by processed to store some information */
SLIC3R_HOST_API void layer_island_set_tag(layer_island_handle *me, const char *tag, double value);
SLIC3R_HOST_API double layer_island_get_tag(const layer_island_handle *me, const char *tag);

SLIC3R_HOST_API uint32_t layer_island_count_region(const layer_island_handle *me);
SLIC3R_HOST_API layer_region_handle *layer_island_get_region_mutable(layer_island_handle *me, uint32_t idx);
SLIC3R_HOST_API const layer_region_handle *layer_island_get_region(const layer_island_handle *me, uint32_t idx);

SLIC3R_HOST_API uint32_t layer_island_count_region_island(const layer_island_handle *me);
SLIC3R_HOST_API layer_region_island_handle *layer_island_get_region_island_mutable(layer_island_handle *me, uint32_t idx);
SLIC3R_HOST_API const layer_region_island_handle *layer_island_get_region_island(const layer_island_handle *me, uint32_t idx);

SLIC3R_HOST_API const layer_handle *layer_island_get_layer(const layer_island_handle *me);

/*
Linked islands on adjacent object layers.
*/
SLIC3R_HOST_API uint32_t layer_island_count_lower_island(const layer_island_handle *me);
SLIC3R_HOST_API const layer_island_handle *layer_island_get_lower_island(const layer_island_handle *me, uint32_t idx);
SLIC3R_HOST_API uint32_t layer_island_count_upper_island(const layer_island_handle *me);
SLIC3R_HOST_API const layer_island_handle *layer_island_get_upper_island(const layer_island_handle *me, uint32_t idx);

/* ========================= LAYER REGION ISLAND ========================= */

SLIC3R_HOST_API int32_t layer_region_island_extruder_id(const layer_region_island_handle *me);
SLIC3R_HOST_API int32_t layer_region_island_has_extrusions(const layer_region_island_handle *me);
SLIC3R_HOST_API int32_t layer_region_island_has_extrusion(const layer_region_island_handle *me, raw_extrusion_role role);
SLIC3R_HOST_API extrusion_entity_handle *layer_region_island_get_mutable_extrusion(layer_region_island_handle *me, raw_extrusion_role role);
SLIC3R_HOST_API const extrusion_entity_handle *layer_region_island_get_extrusion(const layer_region_island_handle *me,
                                                          raw_extrusion_role role);

/*
Island-level fill surfaces produced by STEP_SURFACE_GENERATION and refined by
later surface-processing steps. LayerRegion no longer exposes these caches in
the plugin ABI; plugins should read/write fill surfaces through the
LayerRegionIsland that owns the infill work.
*/
SLIC3R_HOST_API const surface_collection_handle *layer_region_island_get_fill_surfaces(const layer_region_island_handle *me);
SLIC3R_HOST_API uint32_t layer_region_island_count_fill_surface(const layer_region_island_handle *me);
SLIC3R_HOST_API const surface_handle *layer_region_island_get_fill_surface(const layer_region_island_handle *me, uint32_t idx);

/* tag that can be used by processed to store some information */
SLIC3R_HOST_API void layer_region_island_set_tag(layer_region_island_handle *me, const char *tag, double value);
SLIC3R_HOST_API double layer_region_island_get_tag(const layer_region_island_handle *me, const char *tag);

/*
Regions owned by this LayerRegionIsland.

Surface-generation plugins use this to refine an existing group without
falling back to the whole LayerIsland. The returned region handles are borrowed
from the host data tree and remain valid during the current step run.
*/
SLIC3R_HOST_API uint32_t layer_region_island_count_region(const layer_region_island_handle *me);
SLIC3R_HOST_API const layer_region_handle *layer_region_island_get_region(const layer_region_island_handle *me,
                                                                          uint32_t idx);

/* ========================= PRINT REGION ========================= */

SLIC3R_HOST_API config_handle *print_region_get_config_mutable(print_region_handle *me);
SLIC3R_HOST_API const config_handle *print_region_get_config(const print_region_handle *me);

/* ========================= OBJECT ========================= */

SLIC3R_HOST_API config_handle *object_get_config_mutable(object_handle *me);
SLIC3R_HOST_API const config_handle *object_get_config(const object_handle *me);

// Deprecated: if not useful, it will be deleted
SLIC3R_HOST_API coord_t object_get_max_z(const object_handle *me);

/*
Transformation from the source model object into this printable Object.

This is the same transform as native PrintObject::trafo(): rotation / scaling /
mirroring / Z translation are included, but the temporary XY centering offset
used to keep Clipper coordinates small is not. To reproduce trafo_centered(),
apply object_get_center_offset() as a pre-translation:
    centered = translate(-unscaled(center.x), -unscaled(center.y), 0) * transform
*/
SLIC3R_HOST_API c_matrix4d object_get_transform(const object_handle *me);

/*
Scaled XY offset used by the host while slicing to center the mesh before it is
sent to Clipper. This is a slicer-space 2D value, so it uses coord_t through
c_point. Convert with unscaled() before composing it with c_matrix4d.
*/
SLIC3R_HOST_API c_point object_get_center_offset(const object_handle *me);

SLIC3R_HOST_API uint32_t object_count_layer(const object_handle *me);
SLIC3R_HOST_API layer_handle *object_get_layer_mutable(object_handle *me, uint32_t idx);
SLIC3R_HOST_API const layer_handle *object_get_layer(const object_handle *me, uint32_t idx);

SLIC3R_HOST_API uint32_t object_count_region(const object_handle *me);
SLIC3R_HOST_API print_region_handle *object_get_print_region_mutable(object_handle *me, uint32_t idx);
SLIC3R_HOST_API const print_region_handle *object_get_print_region(const object_handle *me, uint32_t idx);

/* ========================= PRINT ========================= */

SLIC3R_HOST_API config_handle *print_get_config_mutable(print_handle *me);
SLIC3R_HOST_API const config_handle *print_get_config(const print_handle *me);

SLIC3R_HOST_API uint32_t print_count_object(const print_handle *me);

SLIC3R_HOST_API object_handle *print_get_object_mutable(print_handle *me, uint32_t idx);
SLIC3R_HOST_API const object_handle *print_get_object(const print_handle *me, uint32_t idx);

/* ========================= CONFIG ========================= */

/*
Returns keys as a borrowed array of strings.
*/
SLIC3R_HOST_API const_strings_t config_keys(const config_handle *me);

/*
Get option by key (string must be null-terminated).
Returns NULL if not found.
*/
SLIC3R_HOST_API const config_option_handle *config_get(const config_handle *me, const char *key);
SLIC3R_HOST_API config_option_handle *config_get_mutable(config_handle *me, const char *key);

#ifdef __cplusplus
}
#endif

#endif // slic3r_data_tree_h_
