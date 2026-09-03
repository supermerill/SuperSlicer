///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_step_infill_group_h_
#define slic3r_step_infill_group_h_

#define SLIC3R_PLUGIN_API_STEP_INFILL_GROUP_MAJOR 1u
#define SLIC3R_PLUGIN_API_STEP_INFILL_GROUP_MINOR 0u

#include "slic3r_step_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
Payload for STEP_INFILL_GROUP.

Plugins group compatible infill regions for one object before infill paths are
generated.
*/
typedef struct run_ctx_group_infill_regions {
    const print_handle *print;
    const object_handle *object;
} run_ctx_group_infill_regions;

static inline const run_ctx_group_infill_regions *
plugin_ctx_as_group_infill_regions(const plugin_run_context *ctx)
{
    if (!ctx || ctx->step != STEP_INFILL_GROUP)
        return NULL;
    return (const run_ctx_group_infill_regions *)ctx->data;
}

#ifdef __cplusplus
}
#endif

#endif // slic3r_step_infill_group_h_
