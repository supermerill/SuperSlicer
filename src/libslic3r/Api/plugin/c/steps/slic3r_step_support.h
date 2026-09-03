///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_step_support_h_
#define slic3r_step_support_h_

#define SLIC3R_PLUGIN_API_STEP_SUPPORT_MAJOR 1u
#define SLIC3R_PLUGIN_API_STEP_SUPPORT_MINOR 0u

#include "slic3r_step_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
Payload for STEP_SUPPORT.

This step generates support layers, support surfaces and/or support extrusion
for one object, depending on the registered plugin.
*/
typedef struct run_ctx_generate_support {
    const print_handle *print;
    const object_handle *object;
} run_ctx_generate_support;

static inline const run_ctx_generate_support *
plugin_ctx_as_generate_support(const plugin_run_context *ctx)
{
    if (!ctx || ctx->step != STEP_SUPPORT)
        return NULL;
    return (const run_ctx_generate_support *)ctx->data;
}

#ifdef __cplusplus
}
#endif

#endif // slic3r_step_support_h_
