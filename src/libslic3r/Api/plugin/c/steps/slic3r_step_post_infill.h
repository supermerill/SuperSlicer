///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_step_post_infill_h_
#define slic3r_step_post_infill_h_

#include "slic3r_step_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
Payload for STEP_POST_INFILL.

STEP_POST_INFILL runs once per object after STEP_INFILL has generated the
normal infill extrusion trees. At this point the plugin can inspect the object,
its layers, islands, and LayerRegionIslands, then adjust the extrusion buckets
that belong to infill work.

Normal usage:
- cast plugin_run_context with plugin_ctx_as_post_infill_generation();
- iterate object -> layers -> islands -> region islands with the data-tree API;
- call layer_island_get_or_create_region_island() from the data-tree API when
  the plugin has to publish infill into a LayerRegionIsland matching a
  specific set of LayerRegions and an already resolved extruder;
- call get_region_island_mutable_extrusion() when an existing infill, gap-fill,
  or ironing extrusion tree has to be edited or extended;
- leave the object unchanged when the plugin has no work to do.

The print and object handles are read-only. This is deliberate: a post-infill
plugin should not change layer topology, region membership, surfaces, perimeter
extrusions, or support extrusions. If it needs to modify generated infill, it
must ask the host for exactly that bucket through the callback below. This keeps
the ownership rule simple for plugin authors: normal traversal is read-only,
and every writable part of the host data tree is exposed by an explicit function
in the step payload.
*/
typedef extrusion_entity_handle *(*post_infill_get_region_island_mutable_extrusion_fn)(
    const layer_region_island_handle *region_island,
    raw_extrusion_role role);

typedef struct run_ctx_post_infill_generation {
    const print_handle *print;
    const object_handle *object;

    /*
    Borrow the mutable root extrusion for an infill-owned region-island bucket.

    Use this after finding a LayerRegionIsland through the read-only object
    traversal. The role selects which bucket is requested:
    - RAW_EXTRUSION_ROLE_INTERNAL_INFILL, RAW_EXTRUSION_ROLE_SOLID_INFILL,
      RAW_EXTRUSION_ROLE_TOP_SOLID_INFILL, and other infill roles all return
      the infill bucket;
    - RAW_EXTRUSION_ROLE_GAP_FILL returns the gap-fill bucket;
    - RAW_EXTRUSION_ROLE_IRONING_INFILL returns the ironing bucket.

    Roles owned by other steps, such as perimeter or support roles, return
    NULL. For a valid post-infill role, the callback returns the root collection
    stored in the LayerRegionIsland, creating that empty root if needed. The
    root itself is sortable and should not carry semantic properties; generated
    infill subtrees are stored as its children.

    The returned handle is borrowed from the host and is valid only during the
    current plugin callback. Do not store it for a later run and do not free it.
    */
    post_infill_get_region_island_mutable_extrusion_fn get_region_island_mutable_extrusion;
} run_ctx_post_infill_generation;

static inline const run_ctx_post_infill_generation *
plugin_ctx_as_post_infill_generation(const plugin_run_context *ctx)
{
    if (!ctx || ctx->step != STEP_POST_INFILL)
        return NULL;
    return (const run_ctx_post_infill_generation *)ctx->data;
}

#ifdef __cplusplus
}
#endif

#endif // slic3r_step_post_infill_h_
