///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "StepGenerateInfill.hpp"

#include <cassert>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/host/Plugin.hpp"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_infill.h"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/LayerRegion.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintObject.hpp"
#include "libslic3r/Steps/StepPipeline.hpp"
#include "libslic3r/Surface.hpp"

namespace Slic3r::Steps::StepGenerateInfill {
namespace {

// STEP_INFILL has three kinds of plugins involved in one run:
// - one exclusive STEP_INFILL generator, selected by the step setting;
// - many INFILL_PATTERN service plugins, one per available fill pattern;
// - optional INFILL_SURFACE_RECIPE_MODIFIER service plugins.
//
// The C ABI stores only opaque pointers and function callbacks, so this struct
// is the host-side state behind those callbacks. It lives for the duration of
// the step run and is reached through run_ctx_generate_infill::host_context.
struct InfillStepHostContext
{
    Orchestrator *orchestrator = nullptr;
    Print *print = nullptr;

    // Runtime pattern ids are compact numbers valid only for this step run.
    // Config still stores stable string plugin ids; the generator resolves
    // those strings through resolve_pattern_id_callback() before filling each
    // surface.
    std::map<std::string, infill_pattern_runtime_id> pattern_runtime_ids_by_plugin_id;
    std::map<infill_pattern_runtime_id, Plugin *> pattern_plugins_by_runtime_id;
    std::map<Plugin *, plugin_host_context> pattern_host_contexts;

    // Recipe modifiers are called as a priority-ordered chain for every fill
    // surface. They share the same mutable raw_infill_pattern_params so each
    // modifier sees the changes made by the previous one.
    std::vector<Plugin *> recipe_modifier_plugins;
    std::map<Plugin *, plugin_host_context> recipe_modifier_host_contexts;
};

// All callbacks stored in run_ctx_generate_infill use this helper to recover
// the C++ state that cannot be captured by a plain C function pointer.
InfillStepHostContext *host_from_context(const run_ctx_generate_infill *ctx)
{
    if (ctx == nullptr || ctx->host_context == nullptr)
        return nullptr;
    return reinterpret_cast<InfillStepHostContext *>(ctx->host_context);
}

// Translate the stable pattern id stored in configuration into the compact
// runtime id used by raw_infill_pattern_params. Keeping this translation in
// the host lets recipe modifiers switch patterns without depending on string
// buffer ownership or active-plugin ordering details.
infill_pattern_runtime_id resolve_pattern_id_callback(const run_ctx_generate_infill *ctx,
                                                      const char *pattern_plugin_id)
{
    InfillStepHostContext *host = host_from_context(ctx);
    if (host == nullptr)
        return INFILL_PATTERN_RUNTIME_ID_INVALID;

    const std::string requested_id = pattern_plugin_id != nullptr ? pattern_plugin_id : "";
    const std::map<std::string, infill_pattern_runtime_id>::const_iterator selected =
        host->pattern_runtime_ids_by_plugin_id.find(requested_id);
    if (selected != host->pattern_runtime_ids_by_plugin_id.end())
        return selected->second;

    // Presets can outlive plugin activation changes. Falling back keeps the
    // slice usable, while the warning tells the user that the selected pattern
    // id is no longer available in the active plugin set.
    if (!host->pattern_plugins_by_runtime_id.empty()) {
        const std::map<infill_pattern_runtime_id, Plugin *>::const_iterator fallback_it =
            host->pattern_plugins_by_runtime_id.begin();
        Plugin *fallback = fallback_it->second;
        const std::string fallback_id = fallback != nullptr ? fallback->get_id() : "";
        const std::string message = "Infill pattern '" + requested_id + "' is not active; using '" +
                                    fallback_id + "' instead.";
        host->orchestrator->add_plugin_message(Orchestrator::PluginMessageLevel::Warning,
                                               fallback,
                                               INFILL_PATTERN,
                                               message.c_str());
        return fallback_it->first;
    }

    return INFILL_PATTERN_RUNTIME_ID_INVALID;
}

// Reverse lookup used mostly by diagnostics and recipe modifiers that want to
// inspect the current recipe. The returned string is owned by Plugin and stays
// valid while the plugin registry exists; callers should treat it as borrowed.
const char *pattern_plugin_id_callback(const run_ctx_generate_infill *ctx,
                                       infill_pattern_runtime_id pattern_id)
{
    InfillStepHostContext *host = host_from_context(ctx);
    if (host == nullptr)
        return "";

    const std::map<infill_pattern_runtime_id, Plugin *>::const_iterator found =
        host->pattern_plugins_by_runtime_id.find(pattern_id);
    if (found == host->pattern_plugins_by_runtime_id.end() || found->second == nullptr)
        return "";

    return found->second->get_id().c_str();
}

// Convert the runtime id present in raw_infill_pattern_params back to the
// active plugin object. This is the last validation gate before executing a
// pattern plugin.
Plugin *pattern_plugin_for_runtime_id(InfillStepHostContext &host,
                                      infill_pattern_runtime_id pattern_id)
{
    if (pattern_id == INFILL_PATTERN_RUNTIME_ID_INVALID)
        return nullptr;

    const std::map<infill_pattern_runtime_id, Plugin *>::const_iterator selected =
        host.pattern_plugins_by_runtime_id.find(pattern_id);
    if (selected != host.pattern_plugins_by_runtime_id.end())
        return selected->second;

    // A runtime id is created only by this host wrapper for the current run. If
    // it cannot be found here, a generator or recipe modifier kept an id past
    // its valid lifetime or wrote an arbitrary value. Do not guess a pattern:
    // emitting no extrusion is safer than silently using the wrong algorithm.
    const std::string message =
        "Unknown runtime infill pattern id '" + std::to_string(pattern_id) + "'.";
    host.orchestrator->add_plugin_message(Orchestrator::PluginMessageLevel::Warning,
                                          nullptr,
                                          INFILL_PATTERN,
                                          message.c_str());
    return nullptr;
}

int32_t generate_pattern_callback(const run_ctx_generate_infill *ctx,
                                  infill_pattern_runtime_id pattern_id,
                                  const layer_handle *layer,
                                  const layer_island_handle *island,
                                  const layer_region_island_handle *region_island,
                                  const layer_region_handle *primary_region,
                                  const surface_handle *surface,
                                  const expolygon_collection_handle *no_overlap_areas,
                                  const raw_infill_pattern_params *params,
                                  extrusion_entity_handle *output)
{
    if (ctx == nullptr || ctx->host_context == nullptr || params == nullptr || output == nullptr)
        return 0;

    InfillStepHostContext &host = *reinterpret_cast<InfillStepHostContext *>(ctx->host_context);
    Plugin *plugin = pattern_plugin_for_runtime_id(host, pattern_id);
    if (plugin == nullptr)
        return 0;

    std::map<Plugin *, plugin_host_context>::iterator host_context = host.pattern_host_contexts.find(plugin);
    assert(host_context != host.pattern_host_contexts.end());
    if (host_context == host.pattern_host_contexts.end())
        return 0;

    plugin_run_context run_context =
        host.orchestrator->prepare_plugin_run_context(INFILL_PATTERN, plugin, &host_context->second);

    // The pattern plugin receives the exact surface recipe and an empty output
    // extrusion root. It does not know how it was selected; that remains the
    // responsibility of STEP_INFILL and the runtime pattern registry above.
    run_ctx_infill_pattern payload = {};
    payload.print = ctx->print;
    payload.object = ctx->object;
    payload.layer = layer;
    payload.island = island;
    payload.region_island = region_island;
    payload.primary_region = primary_region;
    payload.surface = surface;
    payload.no_overlap_areas = no_overlap_areas;
    payload.params = params;
    payload.output = output;
    run_context.data = &payload;

    plugin->setup_run(run_context);
    plugin->run(run_context);
    return 1;
}

int32_t modify_surface_recipe_callback(const run_ctx_generate_infill *ctx,
                                       const layer_handle *layer,
                                       const layer_island_handle *island,
                                       const layer_region_island_handle *region_island,
                                       const layer_region_handle *primary_region,
                                       const surface_handle *surface,
                                       const expolygon_collection_handle *no_overlap_areas,
                                       raw_infill_pattern_params *params)
{
    if (ctx == nullptr || ctx->host_context == nullptr || params == nullptr) {
        return 0;
    }

    InfillStepHostContext &host = *reinterpret_cast<InfillStepHostContext *>(ctx->host_context);
    if (host.recipe_modifier_plugins.empty())
        return 1;

    // Recipe modifiers are a small service chain. Each plugin receives the
    // recipe produced by the generator and any changes made by earlier
    // modifiers. This makes independent tweaks composable: one plugin can
    // adjust density while another changes priority or pattern id.
    for (Plugin *plugin : host.recipe_modifier_plugins) {
        if (plugin == nullptr)
            continue;

        std::map<Plugin *, plugin_host_context>::iterator host_context =
            host.recipe_modifier_host_contexts.find(plugin);
        assert(host_context != host.recipe_modifier_host_contexts.end());
        if (host_context == host.recipe_modifier_host_contexts.end())
            continue;

        plugin_run_context run_context =
            host.orchestrator->prepare_plugin_run_context(
                INFILL_SURFACE_RECIPE_MODIFIER, plugin, &host_context->second);

        // A recipe modifier edits only raw_infill_pattern_params. It gets the
        // same layer/surface context as the generator plus resolver callbacks
        // so it can switch to another active INFILL_PATTERN without touching
        // string buffers owned by the generator.
        run_ctx_infill_surface_recipe_modifier payload = {};
        payload.print = ctx->print;
        payload.object = ctx->object;
        payload.layer = layer;
        payload.island = island;
        payload.region_island = region_island;
        payload.primary_region = primary_region;
        payload.surface = surface;
        payload.no_overlap_areas = no_overlap_areas;
        payload.generate_infill_ctx = ctx;
        payload.resolve_pattern_id = &resolve_pattern_id_callback;
        payload.pattern_plugin_id = &pattern_plugin_id_callback;
        payload.params = params;
        run_context.data = &payload;

        plugin->setup_run(run_context);
        plugin->run(run_context);
    }

    return 1;
}

ExtrusionRole bucket_role_from_raw(raw_extrusion_role role)
{
    if ((role & RAW_EXTRUSION_ROLE_THIN) != 0 || role == RAW_EXTRUSION_ROLE_GAP_FILL)
        return LayerRegionIsland::GAP_FILLS;
    if ((role & RAW_EXTRUSION_ROLE_INFILL) != 0)
        return LayerRegionIsland::INFILLS;
    if ((role & RAW_EXTRUSION_ROLE_IRONING) != 0)
        return LayerRegionIsland::IRONINGS;
    if ((role & RAW_EXTRUSION_ROLE_MILL) != 0)
        return LayerRegionIsland::MILLS;
    if ((role & RAW_EXTRUSION_ROLE_SUPPORT) != 0)
        return (role & RAW_EXTRUSION_ROLE_EXTERNAL) != 0 ? LayerRegionIsland::SUPPORT_INTERFACE :
                                                           LayerRegionIsland::SUPPORT;
    return LayerRegionIsland::INFILLS;
}

void append_extrusion_children(ExtrusionEntityCollection &dst, ExtrusionEntity &src)
{
    if (src.is_nop())
        return;

    if (ExtrusionEntityCollection *collection = dynamic_cast<ExtrusionEntityCollection *>(&src)) {
        dst.append_move_from(*collection);
        return;
    }

    if (src.is_leaf()) {
        dst.append(std::move(src));
        return;
    }

    ExtrusionEntity::Children &children = src.children();
    while (!children.empty()) {
        dst.append(std::move(children.front()));
        children.erase(children.begin());
    }
}

int32_t append_region_island_extrusion_callback(layer_region_island_handle *region_island_handle,
                                                raw_extrusion_role role,
                                                extrusion_entity_handle *extrusion_handle)
{
    LayerRegionIsland *region_island = reinterpret_cast<LayerRegionIsland *>(region_island_handle);
    ExtrusionEntity *extrusion = reinterpret_cast<ExtrusionEntity *>(extrusion_handle);
    if (region_island == nullptr || extrusion == nullptr)
        return 0;

    // Publication is append-only for this step. The generator may call the
    // callback several times for one LayerRegionIsland, once per surface, and
    // all generated paths must stay under the same infill bucket.
    ExtrusionEntityCollection &dst = region_island->mutable_extrusion(bucket_role_from_raw(role));
    append_extrusion_children(dst, *extrusion);
    region_island->remove_empty_extrusions();
    return 1;
}

bool surface_needs_infill(const Surface &surface)
{
    return !surface.empty() && !surface.has_fill_void();
}

size_t count_candidate_surfaces(const Print &print)
{
    size_t count = 0;
    for (const PrintObject &object : print.objects())
        for (const Layer &layer : object.layers())
            for (const LayerSliceIsland &island : layer.islands())
                for (const LayerRegionIsland &region_island : island.regions_islands())
                    for (const Surface &surface : region_island.fill_surfaces())
                        if (surface_needs_infill(surface))
                            ++count;
    return count;
}

void prepare_pattern_plugins(InfillStepHostContext &host)
{
    assert(host.orchestrator != nullptr);
    assert(host.print != nullptr);

    // INFILL_PATTERN plugins are service plugins, not standalone slicing
    // steps. The selected STEP_INFILL generator calls them once per generated
    // surface through generate_pattern_callback(). We still run their setup()
    // here so they can size progress counters and cache per-print state before
    // the generator starts asking for individual patterns.
    const size_t run_count = count_candidate_surfaces(*host.print);
    infill_pattern_runtime_id next_runtime_id = 1;
    for (Plugin *plugin : host.orchestrator->get_active_plugins_for_step(INFILL_PATTERN)) {
        if (plugin == nullptr)
            continue;

        // Runtime ids are one-based so 0 can remain the invalid value exposed
        // by the C ABI. They are deliberately rebuilt for each run; plugin
        // config and UI must keep using plugin->get_id().
        const infill_pattern_runtime_id runtime_id = next_runtime_id++;
        host.pattern_runtime_ids_by_plugin_id.emplace(plugin->get_id(), runtime_id);
        host.pattern_plugins_by_runtime_id.emplace(runtime_id, plugin);
        plugin_host_context plugin_host =
            host.orchestrator->prepare_plugin_host_context(INFILL_PATTERN, plugin, host.print);
        plugin_host.object_count = run_count;
        host.pattern_host_contexts.emplace(plugin, plugin_host);

        plugin_run_context setup_context =
            host.orchestrator->prepare_plugin_run_context(INFILL_PATTERN, plugin, &host.pattern_host_contexts.find(plugin)->second);
        plugin->setup(setup_context, uint32_t(run_count));
    }
}

void prepare_recipe_modifier_plugins(InfillStepHostContext &host)
{
    assert(host.orchestrator != nullptr);
    assert(host.print != nullptr);

    // Recipe modifiers are another service chain owned by STEP_INFILL. They do
    // not traverse the print by themselves: the generator asks the host to run
    // them for each surface after it has built the default recipe and before it
    // calls the selected pattern plugin.
    const size_t run_count = count_candidate_surfaces(*host.print);
    host.recipe_modifier_plugins =
        host.orchestrator->get_active_plugins_for_step(INFILL_SURFACE_RECIPE_MODIFIER);

    for (Plugin *plugin : host.recipe_modifier_plugins) {
        if (plugin == nullptr)
            continue;

        // Use the number of candidate surfaces as the advertised run count
        // because each modifier can be called once for every fillable surface.
        // The actual call order still follows the generator traversal.
        plugin_host_context plugin_host =
            host.orchestrator->prepare_plugin_host_context(
                INFILL_SURFACE_RECIPE_MODIFIER, plugin, host.print);
        plugin_host.object_count = run_count;
        host.recipe_modifier_host_contexts.emplace(plugin, plugin_host);

        plugin_run_context setup_context =
            host.orchestrator->prepare_plugin_run_context(
                INFILL_SURFACE_RECIPE_MODIFIER,
                plugin,
                &host.recipe_modifier_host_contexts.find(plugin)->second);
        plugin->setup(setup_context, uint32_t(run_count));
    }
}

void clear_infill_outputs(Print &print)
{
    for (PrintObject &object : print.objects()) {
        for (Layer &layer : object.layers()) {
            for (LayerSliceIsland &island : layer.islands()) {
                for (LayerRegionIsland &region_island : island.regions_islands()) {
                    if (region_island.has_extrusion(LayerRegionIsland::INFILLS))
                        region_island.mutable_extrusion(LayerRegionIsland::INFILLS).clear();
                    if (region_island.has_extrusion(LayerRegionIsland::GAP_FILLS))
                        region_island.mutable_extrusion(LayerRegionIsland::GAP_FILLS).clear();
                    if (region_island.has_extrusion(LayerRegionIsland::IRONINGS))
                        region_island.mutable_extrusion(LayerRegionIsland::IRONINGS).clear();
                    region_island.remove_empty_extrusions();
                }
            }
        }
    }
}

void run_generator_for_object(Orchestrator &orchestrator,
                              Plugin &plugin,
                              Print &print,
                              PrintObject &object,
                              plugin_host_context &host_context,
                              InfillStepHostContext &infill_context)
{
    plugin_run_context run_context =
        orchestrator.prepare_plugin_run_context(STEP_INFILL, &plugin, &host_context);

    // The generator receives a small callback table instead of direct access to
    // host internals. This keeps pattern selection, recipe modifier ordering
    // and publication into LayerRegionIsland owned by the host step.
    run_ctx_generate_infill payload = {};
    payload.print = reinterpret_cast<const print_handle *>(&print);
    payload.object = reinterpret_cast<const object_handle *>(&object);
    payload.host_context = &infill_context;
    payload.resolve_pattern_id = &resolve_pattern_id_callback;
    payload.pattern_plugin_id = &pattern_plugin_id_callback;
    payload.generate_pattern = &generate_pattern_callback;
    payload.modify_surface_recipe = &modify_surface_recipe_callback;
    payload.append_region_island_extrusion = &append_region_island_extrusion_callback;
    run_context.data = &payload;

    plugin.setup_run(run_context);
    plugin.run(run_context);
}

} // namespace

void clean_and_prepare(Print &print)
{
    clear_infill_outputs(print);
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
    // STEP_INFILL is an exclusive step. The selected generator owns the
    // high-level traversal and parameter preparation, while this host wrapper
    // provides callbacks for pattern-service plugins and data-tree mutation.
    Plugin *plugin = selected_or_active_plugin_for_step(orchestrator, STEP_INFILL, &print.full_print_config());
    if (plugin == nullptr) {
        orchestrator.add_plugin_message(Orchestrator::PluginMessageLevel::Error,
                                        nullptr,
                                        STEP_INFILL,
                                        "No active infill generator plugin is available.");
        return;
    }

    InfillStepHostContext infill_context;
    infill_context.orchestrator = &orchestrator;
    infill_context.print = &print;
    prepare_pattern_plugins(infill_context);
    prepare_recipe_modifier_plugins(infill_context);
    if (infill_context.pattern_plugins_by_runtime_id.empty()) {
        orchestrator.add_plugin_message(Orchestrator::PluginMessageLevel::Error,
                                        plugin,
                                        INFILL_PATTERN,
                                        "No active infill pattern plugin is available.");
        return;
    }

    plugin_host_context host_context = orchestrator.prepare_plugin_host_context(STEP_INFILL, plugin, &print);
    host_context.object_count = print.objects().size();
    plugin_run_context setup_context = orchestrator.prepare_plugin_run_context(STEP_INFILL, plugin, &host_context);
    plugin->setup(setup_context, uint32_t(print.objects().size()));

    for (size_t object_idx = 0; object_idx < print.objects().size(); ++object_idx) {
        host_context.object_idx = object_idx;
        if (setup_context.is_cancelled != nullptr && setup_context.is_cancelled(setup_context.host_context))
            return;
        run_generator_for_object(orchestrator, *plugin, print, print.object(object_idx), host_context, infill_context);
    }
}

} // namespace Slic3r::Steps::StepGenerateInfill
