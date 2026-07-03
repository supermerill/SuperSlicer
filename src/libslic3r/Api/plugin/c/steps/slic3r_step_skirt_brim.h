///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_step_skirt_brim_h_
#define slic3r_step_skirt_brim_h_

#include "slic3r_step_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
Payload for STEP_SKIRT_BRIM.

This step owns first-layer adhesion geometry such as brim and skirt. Plugins
publish that geometry by creating auxiliary layers on either the print auxiliary
object or on a real object. Tag those layers with c_layer_adhesion_property so
later plugins and compatibility readers can classify them without knowing which
generator created them.

The payload stays deliberately small: it gives the plugin a mutable Print
handle, and the generic data-tree API provides the actual layer/extrusion
mutations. The host normalizes directions and rebuilds the final first-layer
convex hull after all active plugins have run.
*/
typedef struct run_ctx_skirt_brim {
    print_handle *print;
} run_ctx_skirt_brim;

static inline const run_ctx_skirt_brim *
plugin_ctx_as_skirt_brim(const plugin_run_context *ctx)
{
    if (!ctx || ctx->step != STEP_SKIRT_BRIM)
        return NULL;
    return (const run_ctx_skirt_brim *)ctx->data;
}

#ifdef __cplusplus
}
#endif

#endif // slic3r_step_skirt_brim_h_
