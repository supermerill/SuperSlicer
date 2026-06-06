///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_step_slicing_h_
#define slic3r_step_slicing_h_

#include "../slic3r_volume.h"
#include "slic3r_step_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
Payload for STEP_SLICING.

Plugins create or edit raw LayerRegion slices for one object. The layer range
and volume-region callbacks expose the object slicing assignment without
exposing native C++ containers.
*/

typedef uint32_t (*slicing_layer_range_count_fn)(const object_handle *object);
typedef const slicing_layer_range_handle *(*slicing_layer_range_at_fn)(const object_handle *object, uint32_t idx);
typedef coord_t (*slicing_layer_range_z_min_fn)(const slicing_layer_range_handle *range);
typedef coord_t (*slicing_layer_range_z_max_fn)(const slicing_layer_range_handle *range);
typedef const config_handle *(*slicing_layer_range_config_fn)(const slicing_layer_range_handle *range);

typedef uint32_t (*slicing_layer_range_volume_region_count_fn)(const slicing_layer_range_handle *range);
typedef const slicing_volume_region_handle *(*slicing_layer_range_volume_region_at_fn)(
    const slicing_layer_range_handle *range,
    uint32_t idx);

typedef const volume_handle *(*slicing_volume_region_volume_fn)(const slicing_volume_region_handle *volume_region);
typedef int32_t (*slicing_volume_region_parent_fn)(const slicing_volume_region_handle *volume_region);
/*
Return the LayerRegion index to write into, or -1 for volume regions that do not
produce printable material directly (for example negative volumes).
*/
typedef int32_t (*slicing_volume_region_layer_region_idx_fn)(const slicing_volume_region_handle *volume_region);
typedef c_bounding_box3f (*slicing_volume_region_bbox_fn)(const slicing_volume_region_handle *volume_region);

typedef struct run_ctx_slicing {
    const print_handle *print;
    const object_handle *object;

    // Mutable access to LayerRegion raw slices created/filled by slicing plugins.
    layer_region_borrow_mutable_slices_fn layer_region_borrow_mutable_slices;

    // Step-local read API over layer ranges and their volume/region entries.
    slicing_layer_range_count_fn layer_range_count;
    slicing_layer_range_at_fn layer_range_at;
    slicing_layer_range_z_min_fn layer_range_z_min;
    slicing_layer_range_z_max_fn layer_range_z_max;
    slicing_layer_range_config_fn layer_range_config;
    slicing_layer_range_volume_region_count_fn layer_range_volume_region_count;
    slicing_layer_range_volume_region_at_fn layer_range_volume_region_at;
    slicing_volume_region_volume_fn volume_region_volume;
    slicing_volume_region_parent_fn volume_region_parent;
    slicing_volume_region_layer_region_idx_fn volume_region_layer_region_idx;
    slicing_volume_region_bbox_fn volume_region_bbox;
} run_ctx_slicing;

static inline const run_ctx_slicing *
plugin_ctx_as_slicing(const plugin_run_context *ctx)
{
    if (!ctx || ctx->step != STEP_SLICING)
        return NULL;
    return (const run_ctx_slicing *)ctx->data;
}

#ifdef __cplusplus
}
#endif

#endif // slic3r_step_slicing_h_
