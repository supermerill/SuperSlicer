///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_step_support_spot_h_
#define slic3r_step_support_spot_h_

#define SLIC3R_PLUGIN_API_STEP_SUPPORT_SPOT_MAJOR 1u
#define SLIC3R_PLUGIN_API_STEP_SUPPORT_SPOT_MINOR 0u

#include "slic3r_step_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
Payload for STEP_SUPPORT_SPOT.

Plugins detect or edit support spots for one object before support material is
generated.
*/
typedef struct run_ctx_detect_support_spots {
    const print_handle *print;
    const object_handle *object;
} run_ctx_detect_support_spots;

static inline const run_ctx_detect_support_spots *
plugin_ctx_as_detect_support_spots(const plugin_run_context *ctx)
{
    if (!ctx || ctx->step != STEP_SUPPORT_SPOT)
        return NULL;
    return (const run_ctx_detect_support_spots *)ctx->data;
}

#ifdef __cplusplus
}
#endif

#endif // slic3r_step_support_spot_h_
