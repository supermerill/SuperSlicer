///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "ExtraPerimeterOddLayer.hpp"

#include <cassert>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_perimeter.h"
#include "libslic3r/Api/plugin/cpp/PerimeterStepViews.hpp"

/*
Odd-layer extra perimeter module
================================

This plugin registers a PERIMETER_GENERATION_MODULE that requests one
additional perimeter on odd object layers where the region setting is enabled.
It does not generate paths itself; the active perimeter generator consumes the
requests and remains responsible for the resulting geometry.

module_start() creates tree-local state containing the layer parity and the
region-segregated setting. If the setting is uniform across the island, an odd
layer takes the fast path and increments the root perimeter count immediately.
If the setting differs between regions, module_after() waits until the current
branch has reached its last perimeter, splits its children against each
enabled region clip, and requests one more perimeter for each resulting inside
branch. The state map prevents the same branch from receiving the extra pass
twice.

The normal call flow is:

    ExtraPerimeterOddLayer::run_impl()
    `-- install module_vtable() in the perimeter-generation context
        `-- perimeter generator invokes module_start()
            |-- read the object layer id and region settings
            |-- even layer?
            |   `-- leave the base perimeter count unchanged
            |-- uniform enabled setting on odd layer?
            |   `-- add one perimeter to the root
            `-- mixed regional setting on odd layer?
                `-- defer requests to module_after()

    module_after()
    `-- check odd-layer, last-perimeter, and branch-state conditions
        |-- enumerate enabled regional clips
        |-- split_node() for each eligible child
        `-- request the current perimeter and mark each inside branch

    module_end()
    `-- destroy the tree-local ModuleState

Even layers and disabled regions are left unchanged. The module affects only
perimeter requests and tree subdivision; path construction remains in the
selected perimeter generator.
*/

namespace slic3r_api { namespace Perimeter { namespace ExtraPerimeterOddLayerPlugin {

namespace {

const char *k_extra_perimeter_odd_layer_id = "perimeter.module.extra_perimeter_odd_layer";
const char *k_no_dependencies[] = { nullptr };
const raw_used_config_key k_used_config_keys[] = {
    { "extra_perimeters_odd_layers", RAW_CO_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE }
};
const char *k_extra_perimeter_odd_layer_key = "extra_perimeters_odd_layers";

class ModuleState
{
public:
    void initialize_region_settings(const PerimeterGenerationContextView &context, uint32_t layer_id)
    {
        // RegionSettings and layer parity are invariant while the host walks
        // the current perimeter tree. Build them once in start() so after()
        // only reads cached state for each processed node.
        m_layer_id = layer_id;
        m_settings.reset(new RegionSettings(context.storage(), context.island(), {{k_extra_perimeter_odd_layer_key}}));
        m_settings->segregate(context.island().slice());
    }

    const RegionSettings *settings() const
    {
        return m_settings.get();
    }

    bool is_odd_layer() const
    {
        return m_layer_id != uint32_t(-1) && m_layer_id % 2 == 1;
    }

    bool has_extra(const perimeter_node *node) const
    {
        return m_extra_nodes.find(node) != m_extra_nodes.end();
    }

    void mark_extra(const perimeter_node *node)
    {
        m_extra_nodes[node] = true;
    }

private:
    uint32_t m_layer_id = uint32_t(-1);
    std::unique_ptr<RegionSettings> m_settings;
    // One entry per branch that already received the odd-layer extra perimeter.
    // This replaces the old region-island scoped "already seen" map from the
    // PerimeterGenerator2 sketch and keeps the module safe across parallel runs.
    std::map<const perimeter_node *, bool> m_extra_nodes;
};

void request_extra_perimeter_for_children(PerimeterGenerationContextView &context,
                                          const PerimeterNodeView &parent,
                                          const RegionSettingsClip &clip,
                                          ModuleState &state)
{
    const std::vector<PerimeterNodeView> children = parent.children_snapshot();
    for (const PerimeterNodeView &child : children) {
        // Only do it one time per branch. If this child already consumed the
        // odd-layer extra pass, its descendants must not request it again.
        if (state.has_extra(child.handle()))
            continue;

        // Split children against the region-local area where
        // extra_perimeters_odd_layers is enabled. The returned nodes are the
        // inside parts that should receive one more perimeter pass.
        const std::vector<PerimeterNodeView> inside_nodes = context.split_node(child, clip);
        for (const PerimeterNodeView &inside_node : inside_nodes) {
            if (state.has_extra(inside_node.handle()))
                continue;
            inside_node.request_current_perimeter();
            state.mark_extra(inside_node.handle());
        }
    }
}

void *module_start(void *, perimeter_generation_context *context)
{
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

    const uint32_t layer_id = context_view.layer_id_from_object();
    assert(layer_id != uint32_t(-1));
    if (layer_id == uint32_t(-1))
        return state;
    state->initialize_region_settings(context_view, layer_id);

    // == solo_config code path ==
    // Only run on odd layer ids. Even layers keep the base perimeter count.
    // This mirrors the old "solo config" path: when the whole current island
    // uses extra_perimeters_odd_layers, add one perimeter on odd layer ids.
    if (state->is_odd_layer()) {
        const RegionSettings *settings = state->settings();
        assert(settings != nullptr);
        if (settings != nullptr &&
            !settings->has_many_config(k_extra_perimeter_odd_layer_key) &&
            settings->get_solo_config(k_extra_perimeter_odd_layer_key).get_bool())
            context_view.root().add_perimeters(1);
    }
    return state;
}

void module_after(void *, void *user_context, perimeter_generation_context *context, perimeter_node *node)
{
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

    // == many_config code path ==
    // Only run on odd layer ids. Even layers keep the generated children as-is.
    if (!state->is_odd_layer())
        return;

    PerimeterNodeView parent(node);
    // Do it when the last perimeter of the current branch has just been
    // extruded. At that point the generator has created the child areas that
    // may need one region-local extra pass.
    if (!parent.is_last_perimeter() || parent.child_count() == 0)
        return;
    // Only do it one time per branch. If this parent is already marked, the
    // branch was generated from a previously requested odd-layer extra pass.
    if (state->has_extra(node))
        return;

    // Check where the region settings need the extra perimeter. When the
    // setting is not split by region, module_start() already handled it by
    // bumping the root perimeter count.
    const RegionSettings *settings = state->settings();
    assert(settings != nullptr);
    if (settings == nullptr || !settings->has_many_config(k_extra_perimeter_odd_layer_key))
        return;

    const RegionSettings::AreaMap &areas = settings->get_areas(k_extra_perimeter_odd_layer_key);
    for (const auto &[setting_value, setting_clip] : areas)
        if (setting_value.get_bool())
            request_extra_perimeter_for_children(context_view, parent, setting_clip, *state);
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

ExtraPerimeterOddLayer &
ExtraPerimeterOddLayer::instance(orchestrator_handle *orch)
{
    static ExtraPerimeterOddLayer s_instance(orch);
    return s_instance;
}

const char *ExtraPerimeterOddLayer::id_impl() const noexcept
{
    return k_extra_perimeter_odd_layer_id;
}

slicing_step_t ExtraPerimeterOddLayer::step_impl() const noexcept
{
    return PERIMETER_GENERATION_MODULE;
}

const char *const *ExtraPerimeterOddLayer::dependencies_impl() const noexcept
{
    return k_no_dependencies;
}

int32_t ExtraPerimeterOddLayer::priority_impl() const noexcept
{
    return 0;
}

int32_t ExtraPerimeterOddLayer::used_config_keys(raw_used_config_key *keys) const noexcept
{
    if (keys != nullptr)
        keys[0] = k_used_config_keys[0];
    return 1;
}

const char *ExtraPerimeterOddLayer::progress_message_format_impl() const noexcept
{
    return "Extra perimeter odd layer: %u / %u";
}

void ExtraPerimeterOddLayer::run_impl(const plugin_run_context *run_ctx) const
{
    run_ctx_perimeter_generation_module *ctx = plugin_ctx_as_perimeter_generation_module(run_ctx);
    if (ctx == nullptr)
        return;

    ctx->module.ctx = const_cast<ExtraPerimeterOddLayer *>(this);
    ctx->module.vt = &module_vtable();
}

void register_extra_perimeter_odd_layer_plugin(orchestrator_handle *orch)
{
    orchestrator_register_plugin(orch, ExtraPerimeterOddLayer::instance(orch).c_instance());
}

}}} // namespace slic3r_api::Perimeter::ExtraPerimeterOddLayerPlugin

#ifdef EXTRA_PERIMETER_ODD_LAYER_PLUGIN_DLL
extern "C" void register_plugin(orchestrator_handle *orch)
{
    slic3r_api::Perimeter::ExtraPerimeterOddLayerPlugin::register_extra_perimeter_odd_layer_plugin(orch);
}
#endif // EXTRA_PERIMETER_ODD_LAYER_PLUGIN_DLL
