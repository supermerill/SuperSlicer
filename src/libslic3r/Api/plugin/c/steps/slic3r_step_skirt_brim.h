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

Plugins in this step create first-layer adhesion geometry such as skirt and
brim. The payload is intentionally small for now because the built-in legacy
skirt/brim code still owns the actual generation; future plugins should add
explicit callbacks here for host-owned geometry they need to publish.
*/
typedef struct run_ctx_skirt_brim {
    const print_handle *print;
    const object_handle *object;
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
