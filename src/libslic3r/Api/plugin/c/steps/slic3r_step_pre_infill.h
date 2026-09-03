///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_step_pre_infill_h_
#define slic3r_step_pre_infill_h_

#define SLIC3R_PLUGIN_API_STEP_PRE_INFILL_MAJOR 1u
#define SLIC3R_PLUGIN_API_STEP_PRE_INFILL_MINOR 0u

#include "slic3r_step_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
Payload for STEP_PRE_INFILL.

Runs before infill preparation/generation for one object.
*/
typedef struct run_ctx_prepare_infill {
    const print_handle *print;
    const object_handle *object;
} run_ctx_prepare_infill;

static inline const run_ctx_prepare_infill *
plugin_ctx_as_prepare_infill(const plugin_run_context *ctx)
{
    if (!ctx || ctx->step != STEP_PRE_INFILL)
        return NULL;
    return (const run_ctx_prepare_infill *)ctx->data;
}

#ifdef __cplusplus
}
#endif

#endif // slic3r_step_pre_infill_h_
