///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_gcode_firmware_h_
#define slic3r_gcode_firmware_h_

#include <stdint.h>

#include "slic3r_data_tree.h"
#include "slic3r_printing_plan.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
G-code firmware session API
===========================

A GCODE_FIRMWARE service creates one session for one export. The session owns
all state needed to convert an ordered PrintingPlan into text. The STEP_GCODE
writer calls the functions below in strict nesting order and writes each text
chunk before asking the session for the next one.

The output and error pointers in raw_gcode_firmware_result are borrowed from
the session. They remain valid until the next callback on that session or until
the session is destroyed. This short lifetime avoids transferring allocations
across the plugin ABI.
*/

typedef enum raw_gcode_firmware_status {
    RAW_GCODE_FIRMWARE_STATUS_UNSET = 0,
    RAW_GCODE_FIRMWARE_STATUS_SUCCESS,
    RAW_GCODE_FIRMWARE_STATUS_INVALID_ARGUMENT,
    RAW_GCODE_FIRMWARE_STATUS_ERROR
} raw_gcode_firmware_status;

typedef struct raw_gcode_firmware_result {
    uint32_t struct_size;
    raw_gcode_firmware_status status;
    const char *text;
    uint64_t text_size;
    const char *error_message;
} raw_gcode_firmware_result;

typedef void (*gcode_firmware_destroy_fn)(void *session);
typedef void (*gcode_firmware_begin_print_fn)(
    void *session,
    const print_handle *print,
    raw_gcode_firmware_result *result);
typedef void (*gcode_firmware_begin_group_fn)(
    void *session,
    const printing_group_handle *group,
    raw_gcode_firmware_result *result);
typedef void (*gcode_firmware_begin_layer_fn)(
    void *session,
    const printing_layer_group_handle *layer_group,
    raw_gcode_firmware_result *result);
typedef void (*gcode_firmware_begin_tool_group_fn)(
    void *session,
    const printing_tool_group_handle *tool_group,
    raw_gcode_firmware_result *result);
typedef void (*gcode_firmware_write_extrusion_fn)(
    void *session,
    const printing_extrusion_handle *extrusion,
    raw_gcode_firmware_result *result);
typedef void (*gcode_firmware_write_event_fn)(
    void *session,
    const extrusion_entity_handle *event_root,
    raw_gcode_firmware_result *result);
typedef void (*gcode_firmware_end_scope_fn)(
    void *session,
    raw_gcode_firmware_result *result);

typedef struct raw_gcode_firmware_vtable {
    uint32_t struct_size;
    gcode_firmware_destroy_fn destroy;
    gcode_firmware_begin_print_fn begin_print;
    gcode_firmware_begin_group_fn begin_group;
    gcode_firmware_begin_layer_fn begin_layer;
    gcode_firmware_begin_tool_group_fn begin_tool_group;
    gcode_firmware_write_extrusion_fn write_extrusion;
    gcode_firmware_write_event_fn write_event;
    gcode_firmware_end_scope_fn end_tool_group;
    gcode_firmware_end_scope_fn end_layer;
    gcode_firmware_end_scope_fn end_group;
    gcode_firmware_end_scope_fn end_print;
} raw_gcode_firmware_vtable;

typedef struct raw_gcode_firmware_instance {
    uint32_t struct_size;
    void *session;
    const raw_gcode_firmware_vtable *vtable;
} raw_gcode_firmware_instance;

#ifdef __cplusplus
}
#endif

#endif // slic3r_gcode_firmware_h_
