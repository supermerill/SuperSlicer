///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_step_bridge_detector_h_
#define slic3r_step_bridge_detector_h_

#define SLIC3R_PLUGIN_API_STEP_BRIDGE_DETECTOR_MAJOR 1u
#define SLIC3R_PLUGIN_API_STEP_BRIDGE_DETECTOR_MINOR 0u

#include "../slic3r_bridge_detector.h"
#include "../slic3r_plugin_run_context.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
Payload for BRIDGE_DETECTOR.

BRIDGE_DETECTOR is a service plugin type. It creates or configures a bridge
detector instance from the input geometry supplied by the host.
*/
typedef struct run_ctx_bridge_detector {
    bridge_detector_create_input input;
    bridge_detector_instance detector;
} run_ctx_bridge_detector;

static inline run_ctx_bridge_detector *
plugin_ctx_as_bridge_detector(const plugin_run_context *ctx)
{
    if (!ctx || ctx->step != BRIDGE_DETECTOR)
        return NULL;

    return (run_ctx_bridge_detector *)ctx->data;
}

#ifdef __cplusplus
}
#endif

#endif // slic3r_step_bridge_detector_h_
