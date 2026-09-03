///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_step_pre_perimeter_h_
#define slic3r_step_pre_perimeter_h_

#define SLIC3R_PLUGIN_API_STEP_PRE_PERIMETER_MAJOR 1u
#define SLIC3R_PLUGIN_API_STEP_PRE_PERIMETER_MINOR 0u

#include "slic3r_step_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
Payload for STEP_PRE_PERIMETER.

This step runs before perimeter generation for one object. Plugins can inspect
or prepare layer/region data before perimeter paths are produced.
*/
typedef struct run_ctx_prepare_for_perimeters {
    const print_handle *print;
    const object_handle *object;
} run_ctx_prepare_for_perimeters;

static inline const run_ctx_prepare_for_perimeters *
plugin_ctx_as_prepare_for_perimeters(const plugin_run_context *ctx)
{
    if (!ctx || ctx->step != STEP_PRE_PERIMETER)
        return NULL;
    return (const run_ctx_prepare_for_perimeters *)ctx->data;
}

#ifdef __cplusplus
}
#endif

#endif // slic3r_step_pre_perimeter_h_
