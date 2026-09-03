///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_step_post_slicing_h_
#define slic3r_step_post_slicing_h_

#define SLIC3R_PLUGIN_API_STEP_POST_SLICING_MAJOR 1u
#define SLIC3R_PLUGIN_API_STEP_POST_SLICING_MINOR 0u

#include "slic3r_step_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
Payload for STEP_POST_SLICING.

At this step, plugins may modify the geometry stored in a PrintObject's layers:
layer slices, island slices, and LayerRegion slices. LayerRegion slices are
mutable directly. Layer slices and island slices require the callbacks below so
the host can keep its caches consistent.
*/
// get mutable layer from const object to be able to do stuff in this step
typedef layer_handle *(*object_borrow_mutable_layer_fn)(const object_handle *me, uint32_t idx);
// clear previous layer's islands, move expolygon_collection_handle into the layer slices and construct islands from it.
typedef void (*layer_assign_islands_by_moving_contents_fn)(layer_handle *me, expolygon_collection_handle *in_out_islands);
// this layer islans  has been modified, recompute the slices (cache of islands)
typedef void (*layer_recompute_slices_from_islands_fn)(layer_handle *me);
// this layer region has been modified, clear all islands and slices from the layer and recompute them from this layer's layer_region;
typedef void (*layer_recompute_slices_and_islands_from_layer_region_fn)(layer_handle *me);
// if don't want ot reconstruct all islands from scratch, you can also modify the layer slices & islands slice &
// region slices, but be careful to keep them consistent. You can use other methodds to recompute one from the other.
typedef expolygon_collection_handle *(*layer_borrow_mutable_slices_fn)(layer_handle *me);
typedef expolygon_handle *(*layer_island_borrow_mutable_slice_fn)(layer_island_handle *me);

typedef struct run_ctx_post_slicing {
    // mutable handles
    const print_handle *print;
    const object_handle *object;

    // Takes ownership / moves contents into Layer islands.
    layer_assign_islands_by_moving_contents_fn layer_assign_islands_by_moving_contents;

    // Rebuilds Layer slices from Layer islands after island mutation.
    layer_recompute_slices_from_islands_fn layer_recompute_slices_from_islands;

    // Rebuilds Layer slices & islands from LayerRegion slices.
    layer_recompute_slices_and_islands_from_layer_region_fn layer_recompute_slices_and_islands_from_layer_region;

    // Mutable post-slicing accessors. Caller must keep Layer / LayerRegion / LayerIsland
    // slices consistent if several caches are edited manually.
    object_borrow_mutable_layer_fn object_borrow_mutable_layer;
    layer_borrow_mutable_slices_fn layer_borrow_mutable_slices;
    layer_region_borrow_mutable_slices_fn layer_region_borrow_mutable_slices;
    layer_island_borrow_mutable_slice_fn layer_island_borrow_mutable_slice;
} run_ctx_post_slicing;

static inline const run_ctx_post_slicing *
plugin_ctx_as_post_slicing(const plugin_run_context *ctx)
{
    if (!ctx || ctx->step != STEP_POST_SLICING)
        return NULL;

    return (const run_ctx_post_slicing *)ctx->data;
}

#ifdef __cplusplus
}
#endif

#endif // slic3r_step_post_slicing_h_
