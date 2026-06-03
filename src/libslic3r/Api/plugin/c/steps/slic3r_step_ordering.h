///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_step_ordering_h_
#define slic3r_step_ordering_h_

#include "../slic3r_printing_plan.h"
#include "slic3r_step_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
Payload for STEP_ORDERING.

Ordering plugins run sequentially on one mutable PrintingPlan. The Print is
read-only source context; the plan is the work copy that builder and ordering
plugins fill or reorder before later pipeline steps consume it.
*/
typedef struct run_ctx_extrusion_ordering {
    const print_handle *print;
    printing_plan_handle *plan;
} run_ctx_extrusion_ordering;

static inline const run_ctx_extrusion_ordering *
plugin_ctx_as_extrusion_ordering(const plugin_run_context *ctx)
{
    if (!ctx || ctx->step != STEP_ORDERING)
        return NULL;
    return (const run_ctx_extrusion_ordering *)ctx->data;
}

#ifdef __cplusplus
}
#endif

#endif // slic3r_step_ordering_h_
