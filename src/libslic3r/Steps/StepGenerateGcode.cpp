#include "StepGenerateGcode.hpp"

#include <cassert>

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/host/Plugin.hpp"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_gcode.h"
#include "libslic3r/libslic3r.h"
#include "libslic3r/Print.hpp"
#include "libslic3r/Printing/PrintingPlan.hpp"
#include "libslic3r/Steps/StepPipeline.hpp"

/*
STEP_GCODE bridge
=================

The G-code step is now a plugin boundary. Earlier pipeline steps build a
PrintingPlan, then this step selects the active STEP_GCODE plugin and gives it
three things: the source Print for context, the mutable PrintingPlan work copy,
and the final output path already resolved by Orchestrator::export_gcode().

The step itself intentionally stays small. File format decisions belong to the
selected plugin, so external writers can replace the prototype implementation
without changing the pipeline runner.
*/

namespace Slic3r::Steps::StepGenerateGcode {

void clean_and_prepare(Print &) {}

bool validate_pre(const Print &, std::string *)
{
    return true;
}

bool validate_post(const Print &, std::string *)
{
    return true;
}

void run_step(Orchestrator &orchestrator, Print &print, const std::string &path)
{
    if (path.empty())
        throw RuntimeError("G-code generation plugin needs a non-empty output path.");

    Plugin *plugin = selected_or_active_plugin_for_step(orchestrator, STEP_GCODE, &print.full_print_config());
    if (plugin == nullptr)
        throw RuntimeError("No active G-code generation plugin is available.");

    // The G-code step consumes the PrintingPlan created by STEP_ORDERING. If a
    // caller runs this step directly in a test, mutable_printing_plan() still
    // gives the plugin a valid empty plan instead of a null handle.
    Printing::PrintingPlan &plan = print.mutable_printing_plan();
    run_ctx_generate_gcode payload = {};
    payload.print = reinterpret_cast<const print_handle *>(&print);
    payload.plan = reinterpret_cast<printing_plan_handle *>(&plan);
    payload.output_path = path.c_str();

    plugin_host_context host_context = orchestrator.prepare_plugin_host_context(STEP_GCODE, plugin, &print);
    plugin_run_context run_context = orchestrator.prepare_plugin_run_context(STEP_GCODE, plugin, &host_context);
    run_context.data = &payload;

    plugin->setup(run_context, 1);
    plugin->setup_run(run_context);
    plugin->run(run_context);

    // PluginBase reports C++ exceptions through report_error() instead of
    // letting them cross the C ABI. report_error() requests cancellation; the
    // host step turns that request back into a normal export failure.
    if (run_context.is_cancelled != nullptr && run_context.is_cancelled(run_context.host_context))
        throw RuntimeError("G-code generation plugin failed.");
}

} // namespace Slic3r::Steps::StepGenerateGcode
