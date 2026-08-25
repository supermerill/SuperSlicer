///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_step_extrusion_edit_h_
#define slic3r_step_extrusion_edit_h_

#include "slic3r_step_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
Payload for STEP_EXTRUSION_EDIT.

Plugins run sequentially after layer-local extrusion editing. They may inspect
the complete ordered PrintingPlan and append scope events before final
simplification and G-code generation.
*/
typedef struct run_ctx_extrusion_edition {
    const print_handle *print;
    printing_plan_handle *plan;
} run_ctx_extrusion_edition;

static inline const run_ctx_extrusion_edition *
plugin_ctx_as_extrusion_edition(const plugin_run_context *ctx)
{
    if (!ctx || ctx->step != STEP_EXTRUSION_EDIT)
        return NULL;
    return (const run_ctx_extrusion_edition *)ctx->data;
}

#ifdef __cplusplus
}
#endif

#endif // slic3r_step_extrusion_edit_h_
