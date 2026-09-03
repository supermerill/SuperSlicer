///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_step_extrusion_simplification_h_
#define slic3r_step_extrusion_simplification_h_

#define SLIC3R_PLUGIN_API_STEP_EXTRUSION_SIMPLIFICATION_MAJOR 1u
#define SLIC3R_PLUGIN_API_STEP_EXTRUSION_SIMPLIFICATION_MINOR 0u

#include "slic3r_step_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
Payload for STEP_EXTRUSION_SIMPLIFICATION.

Plugins may simplify or normalize extrusion geometry after extrusion editing.
*/
typedef struct run_ctx_extrusion_simplification {
    const print_handle *print;
    const object_handle *object;
} run_ctx_extrusion_simplification;

static inline const run_ctx_extrusion_simplification *
plugin_ctx_as_extrusion_simplification(const plugin_run_context *ctx)
{
    if (!ctx || ctx->step != STEP_EXTRUSION_SIMPLIFICATION)
        return NULL;
    return (const run_ctx_extrusion_simplification *)ctx->data;
}

#ifdef __cplusplus
}
#endif

#endif // slic3r_step_extrusion_simplification_h_
