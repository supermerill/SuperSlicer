///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_PluginContext_hpp_
#define slic3r_Api_plugin_cpp_PluginContext_hpp_

/*
Plugin run-context helpers
==========================

The `plugin_run_context` is a host-owned, borrowed context supplied for one
plugin callback. It contains the current step data and optional host callbacks
for cancellation, diagnostics, and progress. Do not retain the pointer or any
data reachable only through it after the callback returns.

`PluginCancelled` is a local C++ control-flow exception used for cooperative
cancellation. `throw_if_cancelled()` checks the host flag and throws it when
the current operation should stop; the `PluginBase` ABI wrappers catch it
without reporting an error. This differs from a normal `std::exception`, which
represents a failure and is reported through `report_error()` by those wrappers.

The reporting helpers are deliberately non-throwing and tolerate a null
context or an absent callback. A warning records a diagnostic without marking
the operation as failed, an error reports a failure, and progress forwards the
plugin's current value to the host. Reporting an error does not itself cancel
the operation.

Typical use inside a plugin callback:

    throw_if_cancelled(ctx);
    report_warning(ctx, "A non-fatal fallback was used.");
    report_progress(ctx, 0.5, "Processing geometry");

Code that implements a raw C plugin entry point must catch C++ exceptions
before returning through the C ABI. `PluginBase` does this automatically for
C++ plugin classes.
*/

#include <exception>

#include "libslic3r/Api/plugin/c/slic3r_plugin_types.h"

namespace slic3r_api {

// Local C++ exception used by plugin code to unwind quickly when the host asks
// the plugin to stop. It must be caught before returning through the C ABI.
class PluginCancelled : public std::exception
{
public:
    const char *what() const noexcept override { return "Plugin execution cancelled"; }
};

inline bool is_cancelled(const plugin_run_context *ctx)
{
    return ctx != nullptr && ctx->is_cancelled != nullptr && ctx->is_cancelled(ctx->host_context) != 0;
}

inline void throw_if_cancelled(const plugin_run_context *ctx)
{
    if (is_cancelled(ctx))
        throw PluginCancelled();
}

inline void report_warning(const plugin_run_context *ctx, const char *message)
{
    if (ctx != nullptr && ctx->report_warning != nullptr)
        ctx->report_warning(ctx->host_context, message);
}

inline void report_error(const plugin_run_context *ctx, const char *message)
{
    if (ctx != nullptr && ctx->report_error != nullptr)
        ctx->report_error(ctx->host_context, message);
}

inline void report_progress(const plugin_run_context *ctx, double progress, const char *message = nullptr)
{
    if (ctx != nullptr && ctx->report_progress != nullptr)
        ctx->report_progress(ctx->host_context, progress, message);
}

} // namespace slic3r_api


#endif // slic3r_Api_plugin_cpp_PluginContext_hpp_
