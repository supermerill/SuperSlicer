///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_step_post_perimeter_h_
#define slic3r_step_post_perimeter_h_

#define SLIC3R_PLUGIN_API_STEP_POST_PERIMETER_MAJOR 1u
#define SLIC3R_PLUGIN_API_STEP_POST_PERIMETER_MINOR 0u

#include "slic3r_step_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
Payload for STEP_POST_PERIMETER.

STEP_POST_PERIMETER runs after STEP_PERIMETER has published perimeter
extrusions and island-level fill areas, but before surface generation turns
those fill areas into Surface objects.

This step is meant for algorithms that need the final perimeter geometry as
input and may rewrite it in place. Typical examples are perimeter cleanup,
bridge/overhang tagging, fuzzy skin, seam/vase annotations, or any operation
that clips/remaps the area that will later be filled.

Normal usage:
- cast plugin_run_context with plugin_ctx_as_post_perimeter_generation();
- iterate object -> layers -> islands -> region islands with the data-tree API;
- call get_region_island_mutable_extrusion() when an extrusion bucket has to be
  edited even though the traversal uses const LayerRegionIsland handles;
- build replacement ExPolygon collections in plugin storage when fill areas
  change, then call set_island_fill_areas() and/or set_island_fill_free_areas().

The print/object/layer/island/region-island handles are borrowed from the host
and are valid only for the current callback. Returned mutable extrusion roots
are also borrowed; do not free them. The fill-area setters copy the source
collection immediately, so the plugin may discard or reuse its source storage
after the callback returns.
*/
typedef extrusion_entity_handle *(*post_perimeter_get_region_island_mutable_extrusion_fn)(
    const layer_region_island_handle *region_island,
    raw_extrusion_role role);

/*
Replace the island fill areas used by later surface/infill steps.

areas == NULL clears the collection. Non-NULL areas are copied into the host
island and the host refreshes the associated bounding boxes.
*/
typedef int32_t (*post_perimeter_set_island_fill_areas_fn)(
    const layer_island_handle *island,
    const expolygon_collection_handle *areas);

/*
Replace the island free/no-encroachment fill areas.

These polygons describe the space available to infill without perimeter
encroachment. areas == NULL clears the collection. Non-NULL areas are copied.
*/
typedef int32_t (*post_perimeter_set_island_fill_free_areas_fn)(
    const layer_island_handle *island,
    const expolygon_collection_handle *areas);

typedef struct run_ctx_post_perimeter_generation {
    const print_handle *print;
    const object_handle *object;

    /*
    Borrow the mutable root extrusion for a region-island bucket.

    The region_island argument is const because the data-tree traversal should
    not let this step modify island topology. Only the selected extrusion bucket
    is made mutable through this explicit post-perimeter capability. Returns
    NULL when the bucket does not exist.
    */
    post_perimeter_get_region_island_mutable_extrusion_fn get_region_island_mutable_extrusion;

    /*
    Replace island-level fill areas. These callbacks are the only supported way
    for post-perimeter plugins to rewrite the areas consumed by
    STEP_SURFACE_GENERATION.
    */
    post_perimeter_set_island_fill_areas_fn set_island_fill_areas;
    post_perimeter_set_island_fill_free_areas_fn set_island_fill_free_areas;
} run_ctx_post_perimeter_generation;

static inline const run_ctx_post_perimeter_generation *
plugin_ctx_as_post_perimeter_generation(const plugin_run_context *ctx)
{
    if (!ctx || ctx->step != STEP_POST_PERIMETER)
        return NULL;
    return (const run_ctx_post_perimeter_generation *)ctx->data;
}

#ifdef __cplusplus
}
#endif

#endif // slic3r_step_post_perimeter_h_
