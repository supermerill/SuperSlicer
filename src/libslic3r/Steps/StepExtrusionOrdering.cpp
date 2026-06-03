
#include "StepExtrusionOrdering.hpp"

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/host/Plugin.hpp"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_ordering.h"
#include "libslic3r/Print.hpp"
#include "libslic3r/Printing/PrintingPlan.hpp"
#include "libslic3r/Steps/StepRunner.hpp"

namespace Slic3r::Steps::StepExtrusionOrdering {

void clean_and_prepare(Print &print)
{
    print.reset_printing_plan();
}

bool validate_pre(const Print &, std::string *)
{
    return true;
}

bool validate_post(const Print &, std::string *)
{
    return true;
}

void run_step(Orchestrator &orchestrator, Print &print)
{
    // STEP_ORDERING is a small sequential pipeline. The first plugin normally
    // builds the PrintingPlan, and later plugins refine that same mutable plan.
    // Running these plugins in parallel would make the plan order depend on
    // scheduler timing, so every selected plugin receives the same payload and
    // runs to completion before the next plugin starts.
    Printing::PrintingPlan &plan = print.mutable_printing_plan();
    run_ctx_extrusion_ordering payload = {};
    payload.print = reinterpret_cast<const print_handle *>(&print);
    payload.plan = reinterpret_cast<printing_plan_handle *>(&plan);

    std::vector<Plugin *> plugins =
        selected_or_active_plugins_for_step(orchestrator, STEP_ORDERING, &print.full_print_config());
    for (Plugin *plugin : plugins) {
        if (plugin == nullptr)
            continue;

        plugin_host_context host_context = orchestrator.prepare_plugin_host_context(STEP_ORDERING, plugin, &print);
        plugin_run_context run_context = orchestrator.prepare_plugin_run_context(STEP_ORDERING, plugin, &host_context);
        run_context.data = &payload;

        plugin->setup(run_context, 1);
        plugin->setup_run(run_context);
        plugin->run(run_context);
    }
}

} // namespace Slic3r::Steps::StepExtrusionOrdering
