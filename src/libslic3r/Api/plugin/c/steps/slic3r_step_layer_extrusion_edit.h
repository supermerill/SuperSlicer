///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_step_layer_extrusion_edit_h_
#define slic3r_step_layer_extrusion_edit_h_

#include "slic3r_step_common.h"
#include "../slic3r_printing_plan.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
Payload for STEP_LAYER_EXTRUSION_EDIT.

The host creates one payload per PrintingLayerGroup after STEP_ORDERING. The
plan and group are read-only structural context: plugins must not append,
remove, or reorder their vectors while layer runs may execute in parallel.
Only the cloned extrusion roots owned by layer_group may be edited.
*/
typedef struct run_ctx_layer_extrusion_edition {
    const print_handle *print;
    const printing_plan_handle *plan;
    const printing_group_handle *group;
    printing_layer_group_handle *layer_group;
    uint32_t group_idx;
    uint32_t layer_group_idx;
} run_ctx_layer_extrusion_edition;

static inline const run_ctx_layer_extrusion_edition *
plugin_ctx_as_layer_extrusion_edition(const plugin_run_context *ctx)
{
    if (!ctx || ctx->step != STEP_LAYER_EXTRUSION_EDIT)
        return NULL;
    return (const run_ctx_layer_extrusion_edition *)ctx->data;
}

#ifdef __cplusplus
}
#endif

#endif // slic3r_step_layer_extrusion_edit_h_
