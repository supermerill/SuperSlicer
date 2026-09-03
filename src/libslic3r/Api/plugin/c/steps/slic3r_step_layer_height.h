///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_step_layer_height_h_
#define slic3r_step_layer_height_h_

#define SLIC3R_PLUGIN_API_STEP_LAYER_HEIGHT_MAJOR 1u
#define SLIC3R_PLUGIN_API_STEP_LAYER_HEIGHT_MINOR 0u

#include "slic3r_step_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
Payload for STEP_LAYER_HEIGHT.

The plugin receives one object and returns explicit object-local layer
descriptors through set_layer_height_profile(). The descriptor array is encoded
as [layer_top_z, layer_height, ...]. Later steps create one Layer for every
pair. Keeping height explicit allows a plugin to leave a deliberate empty Z
interval below a layer. Z values are scaled coordinates and do not include
raft/support layers.
*/

/*
Borrowed view over one Object layer config range entry.

z_min and z_max are unscaled object-local Z coordinates, matching the native
t_layer_height_range convention. config is a borrowed read-only ConfigBase view
over the range's ModelConfig content; it stays valid only during the current
setup_run()/run() call.
*/
typedef struct c_layer_config_range {
    coord_t z_min;
    coord_t z_max;
    const config_handle *config;
} c_layer_config_range;

// Set object layers as [layer_top_z, layer_height, ...] pairs.
typedef void (*set_layer_height_profile_fn)(const object_handle *object,
                                            coord_t *layer_descriptors,
                                            uint32_t descriptor_count);

typedef struct run_ctx_layer_height_generation {
    const print_handle *print;
    const object_handle *object;
    // Input from the GUI, containing [layer_top_z, layer_height] pairs set by
    // the variable layer height feature. Empty when the feature is not used.
    coord_t *enforce_layer_zs;
    uint32_t enforce_layer_zs_size;
    // Object layer-specific config overrides, borrowed from the host object layer ranges.
    // The array and its config handles are read-only and valid only for this setup_run()/run() call.
    const c_layer_config_range *layer_config_ranges;
    uint32_t layer_config_ranges_size;
    // You have to call this to give back the final explicit layer descriptors.
    set_layer_height_profile_fn set_layer_height_profile;
    // Maximum z of the object (from the platter, in the 3D view).
    coord_t max_z;
} run_ctx_layer_height_generation;

static inline const run_ctx_layer_height_generation *
plugin_ctx_as_layer_height_generation(const plugin_run_context *ctx)
{
    if (!ctx || ctx->step != STEP_LAYER_HEIGHT)
        return NULL;
    return (const run_ctx_layer_height_generation *)ctx->data;
}

#ifdef __cplusplus
}
#endif

#endif // slic3r_step_layer_height_h_
