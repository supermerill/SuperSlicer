///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_step_gcode_firmware_h_
#define slic3r_step_gcode_firmware_h_

#include "slic3r_step_common.h"
#include "../slic3r_gcode_firmware.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
Creation payload for a GCODE_FIRMWARE service plugin.

The provider reads the borrowed Print when it needs configuration and returns
a fresh session. Ownership of the returned instance moves to the host, which
destroys it after the STEP_GCODE plugin has finished.
*/
typedef struct run_ctx_gcode_firmware {
    const print_handle *print;
    raw_gcode_firmware_instance instance;
} run_ctx_gcode_firmware;

static inline run_ctx_gcode_firmware *
plugin_ctx_as_gcode_firmware(const plugin_run_context *ctx)
{
    if (!ctx || ctx->step != GCODE_FIRMWARE)
        return NULL;
    return (run_ctx_gcode_firmware *)ctx->data;
}

#ifdef __cplusplus
}
#endif

#endif // slic3r_step_gcode_firmware_h_
