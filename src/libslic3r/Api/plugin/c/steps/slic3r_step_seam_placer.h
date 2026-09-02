///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_step_seam_placer_h_
#define slic3r_step_seam_placer_h_

#include <stdint.h>

#include "../slic3r_extrusion_entity.h"
#include "../slic3r_printing_plan.h"
#include "slic3r_step_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
Seam placer service contract
============================

A SEAM_PLACER plugin creates one initialized session for the PrintingPlan in
run_ctx_seam_placer. The consumer selects and executes the provider through the
generic orchestrator API, then takes ownership of the published instance.

The session callback may be called concurrently. Its result and diagnostic
pointers are borrowed until the next callback on the same thread or until the
session is destroyed. Providers must not allow C++ exceptions to cross this
interface.
*/

typedef enum raw_seam_placer_status {
    /* The callback did not publish a result. */
    RAW_SEAM_PLACER_STATUS_UNSET = 0,
    /* point contains a valid proposed seam. */
    RAW_SEAM_PLACER_STATUS_SUCCESS,
    /* The instance, loop, or output structure was invalid. */
    RAW_SEAM_PLACER_STATUS_INVALID_ARGUMENT,
    /* The provider failed; error_message may describe the failure. */
    RAW_SEAM_PLACER_STATUS_ERROR
} raw_seam_placer_status;

/* Output storage initialized by the caller before invoking place_seam. */
typedef struct raw_seam_placer_result {
    /* Set to sizeof(raw_seam_placer_result) by the caller. */
    uint32_t struct_size;
    raw_seam_placer_status status;
    c_point point;
    /* Borrowed diagnostic; see the lifetime rule in the file introduction. */
    const char *error_message;
} raw_seam_placer_result;

/* Release one provider-owned session. */
typedef void (*seam_placer_destroy_fn)(void *session);

/* Ask one initialized session to choose a point on loop. */
typedef void (*seam_placer_place_fn)(
    const void *session,
    const extrusion_entity_handle *loop,
    c_point start_position,
    raw_seam_placer_result *result);

typedef struct raw_seam_placer_vtable {
    uint32_t struct_size;
    seam_placer_destroy_fn destroy;
    seam_placer_place_fn place_seam;
} raw_seam_placer_vtable;

typedef struct raw_seam_placer_instance {
    uint32_t struct_size;
    /* Provider-owned state transferred to the consumer. */
    void *session;
    /* Static callback table that must outlive session. */
    const raw_seam_placer_vtable *vtable;
} raw_seam_placer_instance;

/*
Single payload used to create a seam placer session.

plan is borrowed and remains owned by the caller. A successful provider writes
a complete instance into instance during run(). Ownership then transfers to the
consumer, which must eventually invoke instance.vtable->destroy(instance.session).
*/
typedef struct run_ctx_seam_placer {
    const printing_plan_handle *plan;
    raw_seam_placer_instance instance;
} run_ctx_seam_placer;

/* Return the service payload only while a SEAM_PLACER plugin is executing. */
static inline run_ctx_seam_placer *
plugin_ctx_as_seam_placer(const plugin_run_context *ctx)
{
    if (ctx == NULL || ctx->step != SEAM_PLACER)
        return NULL;
    return (run_ctx_seam_placer *)ctx->data;
}

#ifdef __cplusplus
}
#endif

#endif // slic3r_step_seam_placer_h_
