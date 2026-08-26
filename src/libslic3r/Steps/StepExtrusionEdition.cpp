#include "StepExtrusionEdition.hpp"

#include <vector>

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/host/Plugin.hpp"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_extrusion_edit.h"
#include "libslic3r/Print.hpp"
#include "libslic3r/Printing/PrintingPlan.hpp"
#include "libslic3r/Steps/StepRunner.hpp"

/*
Global PrintingPlan event edition
=================================

STEP_EXTRUSION_EDIT is the sequential plan-wide editing stage. It rebuilds all
scope events, then lets each active plugin append its event trees in priority
order. Unlike STEP_LAYER_EXTRUSION_EDIT, this stage never runs layer groups in
parallel because plan, group, layer, and tool boundaries share one hierarchy.
*/

namespace Slic3r::Steps::StepExtrusionEdition {
namespace {

// Remove every event produced by an earlier execution while preserving the
// ordered extrusion work and the fixed event roots themselves.
void clear_scope_events(Printing::PrintingPlan &plan);

void clear_scope_events(Printing::PrintingPlan &plan)
{
    plan.events.clear();
    for (Printing::PrintingGroup &group : plan.groups) {
        group.events.clear();
        for (Printing::PrintingLayerGroup &layer_group : group.layers) {
            layer_group.events.clear();
            for (Printing::PrintingToolGroup &tool_group : layer_group.tool_groups)
                tool_group.events.clear();
        }
    }
}

} // namespace

void clean_and_prepare(Print &print)
{
    if (print.printing_plan() != nullptr)
        clear_scope_events(print.mutable_printing_plan());
}

bool validate_pre(const Print &print, std::string *error)
{
    if (print.printing_plan() != nullptr)
        return true;
    if (error != nullptr)
        *error = "STEP_EXTRUSION_EDIT requires the PrintingPlan produced by STEP_ORDERING.";
    return false;
}

bool validate_post(const Print &, std::string *)
{
    return true;
}

void run_step(Orchestrator &orchestrator, Print &print)
{
    if (print.printing_plan() == nullptr)
        return;

    Printing::PrintingPlan &plan = print.mutable_printing_plan();
    run_ctx_extrusion_edition payload = {};
    payload.print = reinterpret_cast<const print_handle *>(&print);
    payload.plan = reinterpret_cast<printing_plan_handle *>(&plan);

    // Every plugin observes all events appended by lower-priority plugins.
    // Completing one plugin before starting the next makes event order stable
    // and lets later plugins intentionally add work around earlier events.
    const std::vector<Plugin *> plugins = selected_or_active_plugins_for_step(
        orchestrator, STEP_EXTRUSION_EDIT, &print.full_print_config());
    for (Plugin *plugin : plugins) {
        if (plugin == nullptr)
            continue;

        plugin_host_context host_context =
            orchestrator.prepare_plugin_host_context(STEP_EXTRUSION_EDIT, plugin, &print);
        plugin_run_context run_context =
            orchestrator.prepare_plugin_run_context(STEP_EXTRUSION_EDIT, plugin, &host_context);
        run_context.data = &payload;

        plugin->setup(run_context, 1);
        plugin->setup_run(run_context);
        plugin->run(run_context);
    }
}

} // namespace Slic3r::Steps::StepExtrusionEdition
