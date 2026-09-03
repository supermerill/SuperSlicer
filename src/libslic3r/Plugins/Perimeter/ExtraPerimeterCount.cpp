///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "ExtraPerimeterCount.hpp"

#include <cassert>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_perimeter.h"
#include "libslic3r/Api/plugin/cpp/PerimeterStepViews.hpp"

/*
Region-dependent extra perimeter module
========================================

This plugin does not generate perimeter geometry itself. It registers a
PERIMETER_GENERATION_MODULE vtable so the active perimeter generator can ask
it how many additional perimeters are required for each part of a perimeter
tree. The setting is region-local, therefore different branches of one island
may require different counts.

module_start() allocates one ModuleState for the current perimeter tree and
pre-segregates the extra-perimeter setting. When the whole island uses one
value, it takes the fast path and requests all extra perimeters at the root.
When values differ between regions, it leaves the root unchanged and lets
module_after() process the child areas created by the generator. That callback
builds a clip containing regions that still allow another perimeter, splits
eligible children, and records how many region-local extras each new branch
has already consumed. module_end() releases the tree-local state.

The normal call flow is:

    ExtraPerimeterCount::run_impl()
    `-- install module_vtable() in the perimeter-generation context
        `-- perimeter generator invokes module_start()
            |-- create ModuleState and segregate region settings
            |-- uniform setting?
            |   `-- request all extra perimeters at the root
            `-- mixed region settings?
                `-- defer requests to module_after()

    module_after()
    `-- inspect generated child nodes
        |-- wait if another module still requests more perimeters
        |-- build_extra_clip() for still-eligible regions
        |-- split_node() along that clip
        `-- request_extra_for_inside_nodes() and update branch counts

    module_end()
    `-- destroy the tree-local ModuleState

The module changes perimeter requests and tree subdivision only. The selected
perimeter generator remains responsible for converting each resulting area
into extrusion paths.
*/

namespace slic3r_api { namespace Perimeter { namespace ExtraPerimeterCountPlugin {

namespace {

const char *k_extra_perimeter_count_id = "perimeter.module.extra_perimeter_count";
const char *k_no_dependencies[] = { nullptr };
const raw_used_config_key k_used_config_keys[] = {
    { "extra_perimeters_count", RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE }
};
const char *k_extra_perimeter_count_key = "extra_perimeters_count";

class ModuleState
{
public:
    void initialize_region_settings(const PerimeterGenerationContextView &context)
    {
        // RegionSettings only depends on the current island, its regions and
        // the static config values. The host may call after() for many nodes in
        // the same perimeter tree, so build and segregate this once in start().
        m_settings.reset(new RegionSettings(context.storage(), context.island(), {{k_extra_perimeter_count_key}}));
        m_settings->segregate(context.island().slice());
    }

    const RegionSettings *settings() const
    {
        return m_settings.get();
    }

    int32_t get(const perimeter_node *node) const
    {
        // For region-local settings, each split branch needs to know how many
        // extra perimeters it has already consumed. Missing means this branch
        // has not consumed any extra count yet.
        const std::map<const perimeter_node *, int32_t>::const_iterator it = m_counts.find(node);
        return it == m_counts.end() ? 0 : it->second;
    }

    void set(const perimeter_node *node, int32_t count)
    {
        m_counts[node] = count;
    }

private:
    std::unique_ptr<RegionSettings> m_settings;
    std::map<const perimeter_node *, int32_t> m_counts;
};

bool build_extra_clip(const RegionSettings::AreaMap &areas,
                      int32_t already_extruded_extra,
                      RegionSettingsClip &clip_out)
{
    // Merge every area whose configured extra count is still greater than the
    // number already consumed by the current branch. The result is one clip:
    // either "accept all" for the uniform case, or a union of all still-eligible
    // region-local areas.
    bool has_clip = false;
    for (const auto &[setting_value, setting_clip] : areas) {
        const int32_t extra_perimeters_count = setting_value.get_int(k_extra_perimeter_count_key);
        if (already_extruded_extra >= extra_perimeters_count)
            continue;

        if (setting_clip.is_accept_all()) {
            clip_out.make_accept_all();
            return true;
        }

        clip_out.append_copy_from(setting_clip.expolygons());
        has_clip = true;
    }

    if (has_clip)
        clip_out.union_self();
    return has_clip;
}

void request_extra_for_inside_nodes(const std::vector<PerimeterNodeView> &inside_nodes,
                                    ModuleState &state,
                                    int32_t next_extra_count)
{
    for (const PerimeterNodeView &inside_node : inside_nodes) {
        // Split nodes may already be complete at their current depth. Asking
        // for one current perimeter makes the host process the inside branch
        // once more, then the stored count tells the next after() call how many
        // region-local extras this branch has consumed.
        inside_node.request_current_perimeter();
        state.set(inside_node.handle(), next_extra_count);
    }
}

void *module_start(void *, perimeter_generation_context *context)
{
    // One state object per perimeter tree. It owns the pre-segregated settings
    // and the per-branch counters, so it must not live on the plugin singleton.
    ModuleState *state = new ModuleState();
    assert(context != nullptr);
    assert(context == nullptr || context->root != nullptr);
    if (context == nullptr || context->root == nullptr)
        return state;

    PerimeterGenerationContextView context_view(context);
    const uint32_t region_count = context_view.island().region_count();
    assert(region_count > 0);
    if (region_count == 0)
        return state;

    state->initialize_region_settings(context_view);
    const RegionSettings *settings = state->settings();
    assert(settings != nullptr);

    // == many_config code path ==
    // Region-local values cannot be resolved at the root: the generator has to
    // create child areas first, then after() will split those children against
    // the clips where another extra perimeter is still allowed.
    if (settings == nullptr || settings->has_many_config(k_extra_perimeter_count_key))
        return state;

    // == solo_config code path ==
    // Fast uniform path. If the whole island uses one value, the root branch can
    // simply request the configured number of extra perimeters up front. The
    // after() path is only needed when different regions require different
    // counts and child areas have to be split.
    const int32_t extra_perimeters_count =
        settings->get_solo_config(k_extra_perimeter_count_key).get_int(k_extra_perimeter_count_key);
    if (extra_perimeters_count > 0)
        context_view.root().add_perimeters(uint32_t(extra_perimeters_count));
    return state;
}

void module_after(void *, void *user_context, perimeter_generation_context *context, perimeter_node *node)
{
    // Region-local extra counts are applied in after(), once the generator has
    // created the child areas that may need one more perimeter.
    ModuleState *state = static_cast<ModuleState *>(user_context);
    assert(context != nullptr);
    assert(node != nullptr);
    assert(state != nullptr);
    if (context == nullptr || node == nullptr || state == nullptr)
        return;

    PerimeterGenerationContextView context_view(context);
    const uint32_t region_count = context_view.island().region_count();
    assert(region_count > 0);
    if (region_count == 0)
        return;

    const RegionSettings *settings = state->settings();
    assert(settings != nullptr);
    if (settings == nullptr || !settings->has_many_config(k_extra_perimeter_count_key))
        return;

    // == many_config code path ==
    // The solo_config case was fully handled in start(), so after() only deals
    // with per-region values. At this point the generator has created child
    // areas, which can be split by RegionSettings clips.
    PerimeterNodeView parent(node);
    // No children means there is no remaining inner area to split or extend.
    if (parent.child_count() == 0)
        return;

    const RegionSettings::AreaMap &areas = settings->get_areas(k_extra_perimeter_count_key);
    const int32_t already_extruded_extra = state->get(node);
    // Take a snapshot because split_node() may rebuild the parent's child array.
    // Views store node pointers, not array-slot addresses, so they remain valid
    // under the current split contract.
    const std::vector<PerimeterNodeView> children = parent.children_snapshot();

    for (const PerimeterNodeView &child : children) {
        // If something else already requested this child, let that request run
        // first. This module will see the generated descendants later with the
        // per-branch extra count preserved in ModuleState.
        if (child.needs_more_perimeters())
            return;

        RegionSettingsClip eligible_clip(context_view.storage());
        if (!build_extra_clip(areas, already_extruded_extra, eligible_clip))
            continue;

        // Only the part of the child falling inside the still-eligible clip
        // should receive the next extra perimeter. The outside siblings keep
        // their current count and will stop if no other module touches them.
        const std::vector<PerimeterNodeView> inside_nodes = context_view.split_node(child, eligible_clip);
        request_extra_for_inside_nodes(inside_nodes, *state, already_extruded_extra + 1);
    }
}

void module_end(void *, void *user_context, perimeter_generation_context *)
{
    assert(user_context != nullptr);
    if (user_context == nullptr)
        return;
    delete static_cast<ModuleState *>(user_context);
}

const perimeter_generation_module_vtable &module_vtable()
{
    static const perimeter_generation_module_vtable vt = {
        &module_start,
        nullptr,
        &module_after,
        &module_end
    };
    return vt;
}

} // namespace

ExtraPerimeterCount &
ExtraPerimeterCount::instance(orchestrator_handle *orch)
{
    static ExtraPerimeterCount s_instance(orch);
    return s_instance;
}

const char *ExtraPerimeterCount::id_impl() const noexcept
{
    return k_extra_perimeter_count_id;
}

slicing_step_t ExtraPerimeterCount::step_impl() const noexcept
{
    return PERIMETER_GENERATION_MODULE;
}

const char *const *ExtraPerimeterCount::dependencies_impl() const noexcept
{
    return k_no_dependencies;
}

int32_t ExtraPerimeterCount::priority_impl() const noexcept
{
    return -20;
}

int32_t ExtraPerimeterCount::used_config_keys(raw_used_config_key *keys) const noexcept
{
    if (keys != nullptr)
        keys[0] = k_used_config_keys[0];
    return 1;
}

const char *ExtraPerimeterCount::progress_message_format_impl() const noexcept
{
    return "Extra perimeter count: %u / %u";
}

void ExtraPerimeterCount::run_impl(const plugin_run_context *run_ctx) const
{
    run_ctx_perimeter_generation_module *ctx = plugin_ctx_as_perimeter_generation_module(run_ctx);
    if (ctx == nullptr)
        return;

    ctx->module.ctx = const_cast<ExtraPerimeterCount *>(this);
    ctx->module.vt = &module_vtable();
}

void register_extra_perimeter_count_plugin(orchestrator_handle *orch)
{
    orchestrator_register_plugin(orch, ExtraPerimeterCount::instance(orch).c_instance());
}

}}} // namespace slic3r_api::Perimeter::ExtraPerimeterCountPlugin

#ifdef EXTRA_PERIMETER_COUNT_PLUGIN_DLL
extern "C" void register_plugin(orchestrator_handle *orch)
{
    slic3r_api::Perimeter::ExtraPerimeterCountPlugin::register_extra_perimeter_count_plugin(orch);
}
#endif // EXTRA_PERIMETER_COUNT_PLUGIN_DLL
