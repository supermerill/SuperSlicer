///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_step_wipetower_h_
#define slic3r_step_wipetower_h_

#define SLIC3R_PLUGIN_API_STEP_WIPETOWER_MAJOR 1u
#define SLIC3R_PLUGIN_API_STEP_WIPETOWER_MINOR 0u

#include "slic3r_step_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
Payload for STEP_WIPETOWER.

Plugins generate or edit wipe-tower data for one object/print context.
*/
typedef struct run_ctx_generate_wipe_tower {
    const print_handle *print;
    const object_handle *object;
} run_ctx_generate_wipe_tower;

static inline const run_ctx_generate_wipe_tower *
plugin_ctx_as_generate_wipe_tower(const plugin_run_context *ctx)
{
    if (!ctx || ctx->step != STEP_WIPETOWER)
        return NULL;
    return (const run_ctx_generate_wipe_tower *)ctx->data;
}

#ifdef __cplusplus
}
#endif

#endif // slic3r_step_wipetower_h_
