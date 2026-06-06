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
or PrintObject: brim, skirt, first-layer-only skirt loops, and the skirt convex
hull used by the final print envelope.

Plugins should build extrusion trees in their own storage, then move them into
the host through this callback table. Moving means the source handle remains a
valid plugin object, but its tree may be empty afterwards.

The read callbacks expose geometry already published by earlier plugins in the
same STEP_SKIRT_BRIM chain. For example, the default skirt generator runs after
the brim generator and reads the existing brim points when it decides how far
the skirt must stand from the object.

The host normalizes directions and rebuilds the final first-layer convex hull
after all active plugins have run. A plugin only appends `skirt_convex_hull`
points when it creates skirt geometry; it does not need to rebuild the whole
print hull itself.
*/
typedef int32_t (*skirt_brim_clear_brim_fn)(print_handle *print);
typedef int32_t (*skirt_brim_clear_object_brim_fn)(object_handle *object);
typedef int32_t (*skirt_brim_clear_skirt_fn)(print_handle *print);
typedef int32_t (*skirt_brim_clear_object_skirt_fn)(object_handle *object);
typedef int32_t (*skirt_brim_append_brim_move_fn)(print_handle *print,
                                                  extrusion_entity_handle *extrusion);
typedef int32_t (*skirt_brim_append_object_brim_move_fn)(object_handle *object,
                                                         extrusion_entity_handle *extrusion);
typedef int32_t (*skirt_brim_append_skirt_move_fn)(print_handle *print,
                                                   extrusion_entity_handle *extrusion);
typedef int32_t (*skirt_brim_append_object_skirt_move_fn)(object_handle *object,
                                                          extrusion_entity_handle *extrusion);
typedef int32_t (*skirt_brim_append_skirt_first_layer_move_fn)(print_handle *print,
                                                               extrusion_entity_handle *extrusion);
typedef int32_t (*skirt_brim_append_object_skirt_first_layer_move_fn)(object_handle *object,
                                                                      extrusion_entity_handle *extrusion);
typedef int32_t (*skirt_brim_append_skirt_convex_hull_move_fn)(print_handle *print,
                                                               polygon_collection_handle *polygons);

typedef const extrusion_entity_handle *(*skirt_brim_get_print_extrusion_fn)(const print_handle *print);
typedef const extrusion_entity_handle *(*skirt_brim_get_object_extrusion_fn)(const object_handle *object);

typedef struct run_ctx_skirt_brim {
    print_handle *print;

    skirt_brim_clear_brim_fn clear_brim;
    skirt_brim_clear_object_brim_fn clear_object_brim;
    skirt_brim_clear_skirt_fn clear_skirt;
    skirt_brim_clear_object_skirt_fn clear_object_skirt;
    skirt_brim_append_brim_move_fn append_brim_move;
    skirt_brim_append_object_brim_move_fn append_object_brim_move;
    skirt_brim_append_skirt_move_fn append_skirt_move;
    skirt_brim_append_object_skirt_move_fn append_object_skirt_move;
    skirt_brim_append_skirt_first_layer_move_fn append_skirt_first_layer_move;
    skirt_brim_append_object_skirt_first_layer_move_fn append_object_skirt_first_layer_move;
    skirt_brim_append_skirt_convex_hull_move_fn append_skirt_convex_hull_move;

    skirt_brim_get_print_extrusion_fn get_brim;
    skirt_brim_get_object_extrusion_fn get_object_brim;
    skirt_brim_get_print_extrusion_fn get_skirt;
    skirt_brim_get_object_extrusion_fn get_object_skirt;
    skirt_brim_get_print_extrusion_fn get_skirt_first_layer;
    skirt_brim_get_object_extrusion_fn get_object_skirt_first_layer;
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
