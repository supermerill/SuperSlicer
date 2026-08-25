///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_gcode_script_h_
#define slic3r_gcode_script_h_

#include <stdint.h>

#include "slic3r_data_tree.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
Host-owned G-code script processor
==================================

The processor keeps the heavy placeholder language and all of its runtime
state inside the host. A firmware prepares one short-lived mutable Config,
fills the script-specific values it knows, then processes one script.

Calls are synchronous, sequential and non-reentrant. The Config returned by
prepare() is borrowed until the next prepare(). Result strings are borrowed
until the next prepare() or process() call on the same processor.
*/

typedef enum raw_gcode_script_status {
    RAW_GCODE_SCRIPT_STATUS_UNSET = 0,
    RAW_GCODE_SCRIPT_STATUS_SUCCESS,
    RAW_GCODE_SCRIPT_STATUS_INVALID_ARGUMENT,
    RAW_GCODE_SCRIPT_STATUS_ERROR
} raw_gcode_script_status;

typedef struct raw_gcode_script_result {
    uint32_t struct_size;
    raw_gcode_script_status status;
    const char *text;
    uint64_t text_size;
    const char *error_message;
} raw_gcode_script_result;

typedef config_handle *(*gcode_script_prepare_fn)(
    void *context,
    const char *script_name);

typedef void (*gcode_script_process_fn)(
    void *context,
    const char *script,
    uint16_t current_extruder,
    raw_gcode_script_result *result);

typedef struct raw_gcode_script_processor {
    uint32_t struct_size;
    void *context;
    gcode_script_prepare_fn prepare;
    gcode_script_process_fn process;
} raw_gcode_script_processor;

#ifdef __cplusplus
}
#endif

#endif // slic3r_gcode_script_h_
