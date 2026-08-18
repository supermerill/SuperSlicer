
#include "StepLayerExtrusionEdition.hpp"

#include <cassert>
#include <cstdint>
#include <vector>

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/host/Plugin.hpp"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_layer_extrusion_edit.h"
#include "libslic3r/Print.hpp"
#include "libslic3r/Printing/PrintingPlan.hpp"
#include "libslic3r/Steps/StepRunner.hpp"
#include "libslic3r/Thread.hpp"

namespace Slic3r::Steps::StepLayerExtrusionEdition {

namespace {

struct LayerRunLocation
{
    uint32_t group_idx = 0;
    uint32_t layer_group_idx = 0;
};

// Flatten the plan hierarchy into stable numeric locations. Layer runs may be
// parallel, so no child pointer is retained across another plugin call.
std::vector<LayerRunLocation> collect_layer_run_locations(const Printing::PrintingPlan &plan);

// Build one short-lived ABI payload from a stable plan and a numeric location.
run_ctx_layer_extrusion_edition make_layer_run_context(Print &print,
                                                       Printing::PrintingPlan &plan,
                                                       const LayerRunLocation &location);

// Run one plugin over every layer group. setup() sees the first payload so it
// may prepare immutable plan-wide data before per-layer work starts.
void run_plugin(Orchestrator &orchestrator,
                Print &print,
                Printing::PrintingPlan &plan,
                Plugin &plugin,
                const std::vector<LayerRunLocation> &locations);

std::vector<LayerRunLocation> collect_layer_run_locations(const Printing::PrintingPlan &plan)
{
    std::vector<LayerRunLocation> locations;
    for (uint32_t group_idx = 0; group_idx < plan.groups.size(); ++group_idx) {
        const Printing::PrintingGroup &group = plan.groups[group_idx];
        for (uint32_t layer_group_idx = 0; layer_group_idx < group.layers.size(); ++layer_group_idx)
            locations.push_back(LayerRunLocation{group_idx, layer_group_idx});
    }
    return locations;
}

run_ctx_layer_extrusion_edition make_layer_run_context(Print &print,
                                                       Printing::PrintingPlan &plan,
                                                       const LayerRunLocation &location)
{
    assert(location.group_idx < plan.groups.size());
    Printing::PrintingGroup &group = plan.groups[location.group_idx];
    assert(location.layer_group_idx < group.layers.size());

    run_ctx_layer_extrusion_edition payload = {};
    payload.print = reinterpret_cast<const print_handle *>(&print);
    payload.plan = reinterpret_cast<const printing_plan_handle *>(&plan);
    payload.group = reinterpret_cast<const printing_group_handle *>(&group);
    payload.layer_group = reinterpret_cast<printing_layer_group_handle *>(&group.layers[location.layer_group_idx]);
    payload.group_idx = location.group_idx;
    payload.layer_group_idx = location.layer_group_idx;
    return payload;
}

void run_plugin(Orchestrator &orchestrator,
                Print &print,
                Printing::PrintingPlan &plan,
                Plugin &plugin,
                const std::vector<LayerRunLocation> &locations)
{
    if (locations.empty())
        return;

    assert(locations.size() <= UINT32_MAX);
    plugin_host_context host_context =
        orchestrator.prepare_plugin_host_context(STEP_LAYER_EXTRUSION_EDIT, &plugin, &print);
    host_context.object_count = locations.size();
    plugin_run_context run_context =
        orchestrator.prepare_plugin_run_context(STEP_LAYER_EXTRUSION_EDIT, &plugin, &host_context);

    // setup() is serialized and receives a real plan payload. The plugin may
    // scan the complete plan here, but structural edits are forbidden because
    // the numeric work list below must remain valid for all parallel runs.
    run_ctx_layer_extrusion_edition setup_payload = make_layer_run_context(print, plan, locations.front());
    run_context.data = &setup_payload;
    plugin.setup(run_context, uint32_t(locations.size()));

    parallel_for(size_t(0), locations.size(), [&](const size_t idx) {
        plugin_host_context local_host_context = host_context;
        local_host_context.object_idx = idx;
        plugin_run_context local_run_context = run_context;
        local_run_context.host_context = &local_host_context;
        if (local_run_context.is_cancelled != nullptr &&
            local_run_context.is_cancelled(local_run_context.host_context))
            return;

        run_ctx_layer_extrusion_edition payload = make_layer_run_context(print, plan, locations[idx]);
        local_run_context.data = &payload;
        plugin.setup_run(local_run_context);
    });

    parallel_for(size_t(0), locations.size(), [&](const size_t idx) {
        plugin_host_context local_host_context = host_context;
        local_host_context.object_idx = idx;
        plugin_run_context local_run_context = run_context;
        local_run_context.host_context = &local_host_context;
        if (local_run_context.is_cancelled != nullptr &&
            local_run_context.is_cancelled(local_run_context.host_context))
            return;

        run_ctx_layer_extrusion_edition payload = make_layer_run_context(print, plan, locations[idx]);
        local_run_context.data = &payload;
        plugin.run(local_run_context);
    });
}

} // namespace

void clean_and_prepare(Print &) {}

bool validate_pre(const Print &print, std::string *error)
{
    if (print.printing_plan() != nullptr)
        return true;
    if (error != nullptr)
        *error = "STEP_LAYER_EXTRUSION_EDIT requires the PrintingPlan produced by STEP_ORDERING.";
    return false;
}

bool validate_post(const Print &, std::string *)
{
    return true;
}

void run_step(Orchestrator &orchestrator, Print &print)
{
    const Printing::PrintingPlan *existing_plan = print.printing_plan();
    if (existing_plan == nullptr)
        return;

    Printing::PrintingPlan &plan = print.mutable_printing_plan();
    const std::vector<LayerRunLocation> locations = collect_layer_run_locations(plan);
    const std::vector<Plugin *> plugins = selected_or_active_plugins_for_step(
        orchestrator, STEP_LAYER_EXTRUSION_EDIT, &print.full_print_config());

    // Plugins are a deterministic edit chain. A plugin finishes all layer runs
    // before the next plugin observes the resulting extrusion properties.
    for (Plugin *plugin : plugins)
        if (plugin != nullptr)
            run_plugin(orchestrator, print, plan, *plugin, locations);
}

} // namespace Slic3r::Steps::StepLayerExtrusionEdition
