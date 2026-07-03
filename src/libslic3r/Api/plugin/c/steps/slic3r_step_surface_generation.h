///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_step_surface_generation_h_
#define slic3r_step_surface_generation_h_

#include "slic3r_step_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
Payload for STEP_SURFACE_GENERATION.

The perimeter step has already published island-level infill areas. A surface
generation plugin converts those areas into LayerRegionIsland fill surfaces.

Normal usage:
- iterate object -> layers -> islands with the data-tree API;
- group island regions that can share the same fill surfaces;
- resolve the effective extruder for each group, then call
  layer_island_get_or_create_region_island() from the data-tree API;
- build a storage-owned SurfaceCollection;
- call set_region_island_fill_surfaces() to move that collection into the
  LayerRegionIsland.

The plugin should not write deprecated LayerRegion fill surface caches here.
The new infill pipeline reads LayerRegionIsland surfaces.
*/

/*
Replace the fill surfaces of a LayerRegionIsland by moving a complete
SurfaceCollection into it.

surfaces must be a storage-owned collection built with
storage_new_surface_collection() and filled before the callback is called. The
host moves the collection content into the LayerRegionIsland; after a
successful call, the source collection is valid but empty. Passing
surfaces == NULL clears the destination collection. This keeps ownership
transfer explicit and avoids exposing mutable LayerRegionIsland internals in
the general data-tree API.
*/
typedef int32_t (*surface_generation_set_region_island_fill_surfaces_fn)(
    layer_region_island_handle *region_island,
    surface_collection_handle *surfaces);

/*
Append one Surface per ExPolygon into a temporary SurfaceCollection, copying
all non-geometry fields from source.

This helper belongs to STEP_SURFACE_GENERATION because it is mainly useful
when a surface plugin clips an existing Surface and must preserve its host-side
metadata: bridge angle, thickness, priority, dense-infill hints, and any future
fields that are not represented by the small C surface type bitmask. Newly
created surfaces can still use the surface_collection_append_expolygon*()
helpers.
*/
typedef void (*surface_generation_append_surface_like_fn)(
    surface_collection_handle *dst,
    const surface_handle *source,
    const expolygon_collection_handle *areas);

typedef struct run_ctx_surface_generation {
    const print_handle *print;
    const object_handle *object;

    surface_generation_set_region_island_fill_surfaces_fn set_region_island_fill_surfaces;
    surface_generation_append_surface_like_fn append_surface_like;
} run_ctx_surface_generation;

static inline const run_ctx_surface_generation *
plugin_ctx_as_surface_generation(const plugin_run_context *ctx)
{
    if (!ctx || ctx->step != STEP_SURFACE_GENERATION)
        return NULL;
    return (const run_ctx_surface_generation *)ctx->data;
}

#ifdef __cplusplus
}
#endif

#endif // slic3r_step_surface_generation_h_
