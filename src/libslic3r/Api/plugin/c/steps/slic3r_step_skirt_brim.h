///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_step_skirt_brim_h_
#define slic3r_step_skirt_brim_h_

#include "slic3r_step_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
Payload for STEP_SKIRT_BRIM.

This step owns first-layer adhesion geometry that is stored directly on Print
or PrintObject, such as brim and, later, skirt. Plugins may use host geometry
helpers to build extrusion trees, but they should publish the final trees
through this callback table instead of writing Print internals directly.

The clear callbacks remove previously generated brim. The append callbacks
move one storage-owned extrusion tree into the destination brim collection.
After a successful append, the source handle is still valid but its content may
be empty. The host recomputes the first-layer convex hull after all active
STEP_SKIRT_BRIM plugins have run, so plugins do not need to report hull points.
*/
typedef int32_t (*skirt_brim_clear_brim_fn)(print_handle *print);
typedef int32_t (*skirt_brim_clear_object_brim_fn)(object_handle *object);
typedef int32_t (*skirt_brim_append_brim_move_fn)(print_handle *print,
                                                  extrusion_entity_handle *extrusion);
typedef int32_t (*skirt_brim_append_object_brim_move_fn)(object_handle *object,
                                                         extrusion_entity_handle *extrusion);

typedef struct run_ctx_skirt_brim {
    print_handle *print;

    skirt_brim_clear_brim_fn clear_brim;
    skirt_brim_clear_object_brim_fn clear_object_brim;
    skirt_brim_append_brim_move_fn append_brim_move;
    skirt_brim_append_object_brim_move_fn append_object_brim_move;
} run_ctx_skirt_brim;

static inline const run_ctx_skirt_brim *
plugin_ctx_as_skirt_brim(const plugin_run_context *ctx)
{
    if (!ctx || ctx->step != STEP_SKIRT_BRIM)
        return NULL;
    return (const run_ctx_skirt_brim *)ctx->data;
}

#ifdef __cplusplus
}
#endif

#endif // slic3r_step_skirt_brim_h_
