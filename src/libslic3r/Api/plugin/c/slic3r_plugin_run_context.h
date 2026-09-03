///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_plugin_run_context_h_
#define slic3r_plugin_run_context_h_

#define SLIC3R_PLUGIN_API_PLUGIN_RUN_CONTEXT_MAJOR 1u
#define SLIC3R_PLUGIN_API_PLUGIN_RUN_CONTEXT_MINOR 0u

#include <stddef.h>

#include "slic3r_slicing_step.h"
#include "slic3r_utils.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================= RUN CONTEXT ========================= */

typedef struct plugin_host_context plugin_host_context;

/*
Return non-zero if the host requested this plugin execution to stop.
Plugins should check this regularly in long loops and return as soon as
possible when cancellation is requested.
*/
typedef int (*plugin_is_cancelled_fn)(plugin_host_context *host_context);

/*
Report a message to the host.
Warnings are non-blocking. Errors are fatal for the current slicing operation:
the host will request cancellation for the slicing operation.
*/
typedef void (*plugin_report_fn)(plugin_host_context *host_context, const char *message);

/*
Report progress inside the currently running step.
progress is clamped by the host to the [0.0, 1.0] range. message is optional
UTF-8 text and may be NULL to let the host use a default step/plugin message.
*/
typedef void (*plugin_report_progress_fn)(plugin_host_context *host_context, double progress, const char *message);

/*
Common context passed to setup_run() and run().

step tells which slicing step is currently executed. data points to the
step-specific payload documented in Api/plugin/c/steps/slic3r_step_*.h.
Use the plugin_ctx_as_* helper from the corresponding step header to cast data
safely.
*/
typedef struct plugin_run_context {
    slicing_step_t step;
    storage_handle *plugin_storage;
    void *data;

    plugin_host_context *host_context;
    plugin_is_cancelled_fn is_cancelled;
    plugin_report_fn report_warning;
    plugin_report_fn report_error;
    plugin_report_progress_fn report_progress;
} plugin_run_context;

#ifdef __cplusplus
}
#endif

#endif // slic3r_plugin_run_context_h_
