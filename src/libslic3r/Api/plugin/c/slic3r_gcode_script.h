///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_gcode_script_h_
#define slic3r_gcode_script_h_

#include <stdint.h>

#include "slic3r_utils.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
Host-owned G-code script processor
==================================

The processor keeps the heavy placeholder language and all of its runtime
state inside the host. A script producer stores an immutable serialized Config
with the extrusion event. At execution time, the firmware reconstructs that
Config, the host copies its values into one short-lived mutable context, and
the firmware adds the current machine state before processing the script.

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

/*
Stable semantic identity of one PlaceholderParser script context.

Built-in ids are shared by the host and every plugin. Runtime ids are allocated
by an Orchestrator from the custom range and are valid only for that
Orchestrator. The numeric value therefore belongs in transient PrintingPlan
events, never in a serialized project or plugin configuration.
*/
typedef uint32_t gcode_script_type;

#define GCODE_SCRIPT_TYPE_INVALID ((gcode_script_type) 0u)
#define GCODE_SCRIPT_TYPE_START_GCODE ((gcode_script_type) 1u)
#define GCODE_SCRIPT_TYPE_END_GCODE ((gcode_script_type) 2u)
#define GCODE_SCRIPT_TYPE_EXTRUSION_CUSTOM ((gcode_script_type) 3u)
#define GCODE_SCRIPT_TYPE_START_FILAMENT_GCODE ((gcode_script_type) 4u)
#define GCODE_SCRIPT_TYPE_END_FILAMENT_GCODE ((gcode_script_type) 5u)
#define GCODE_SCRIPT_TYPE_BEFORE_LAYER_GCODE ((gcode_script_type) 6u)
#define GCODE_SCRIPT_TYPE_LAYER_GCODE ((gcode_script_type) 7u)
#define GCODE_SCRIPT_TYPE_TOOLCHANGE_GCODE ((gcode_script_type) 8u)
#define GCODE_SCRIPT_TYPE_BETWEEN_OBJECTS_GCODE ((gcode_script_type) 9u)
#define GCODE_SCRIPT_TYPE_CUSTOM_BEGIN ((gcode_script_type) 0x80000000u)

typedef struct config_handle config_handle;
typedef struct orchestrator_handle orchestrator_handle;

/*
Register a stable namespaced script name and receive its compact runtime id.

Registering the same name repeatedly is idempotent. Built-in names return their
fixed ids. An empty name or an exhausted registry returns
GCODE_SCRIPT_TYPE_INVALID.
*/
SLIC3R_HOST_API gcode_script_type gcode_script_register_type(orchestrator_handle *orch, const char *namespaced_name);

/* Return the stable name of a built-in or registered script type. */
SLIC3R_HOST_API const char *gcode_script_type_name(const orchestrator_handle *orch, gcode_script_type type);

typedef struct raw_gcode_script_result {
    uint32_t struct_size;
    raw_gcode_script_status status;
    const char *text;
    uint64_t text_size;
    const char *error_message;
} raw_gcode_script_result;

typedef config_handle *(*gcode_script_prepare_fn)(void *context,
                                                  gcode_script_type script_type,
                                                  const config_handle *producer_config);

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
