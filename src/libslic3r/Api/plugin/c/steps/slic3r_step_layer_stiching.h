///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_step_layer_stiching_h_
#define slic3r_step_layer_stiching_h_

#define SLIC3R_PLUGIN_API_STEP_LAYER_STICHING_MAJOR 1u
#define SLIC3R_PLUGIN_API_STEP_LAYER_STICHING_MINOR 0u

#include "slic3r_step_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
Payload for STEP_LAYER_STICHING.

Plugins may stitch or reconcile layer extrusion after layer-level editing.
The name keeps the existing API spelling.
*/
typedef struct run_ctx_layer_stiching {
    const print_handle *print;
    const object_handle *object;
} run_ctx_layer_stiching;

static inline const run_ctx_layer_stiching *
plugin_ctx_as_layer_stiching(const plugin_run_context *ctx)
{
    if (!ctx || ctx->step != STEP_LAYER_STICHING)
        return NULL;
    return (const run_ctx_layer_stiching *)ctx->data;
}

#ifdef __cplusplus
}
#endif

#endif // slic3r_step_layer_stiching_h_
