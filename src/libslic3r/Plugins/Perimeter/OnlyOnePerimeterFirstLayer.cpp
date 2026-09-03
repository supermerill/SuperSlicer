///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "OnlyOnePerimeterFirstLayer.hpp"

#include <cassert>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_perimeter.h"
#include "libslic3r/Api/plugin/cpp/PerimeterStepViews.hpp"

/*
First-layer single-perimeter module
===================================

This plugin registers a PERIMETER_GENERATION_MODULE that limits selected areas
of the first object layer to one perimeter. It does not create or remove
extrusion paths directly. The perimeter generator creates the first ring and
its child areas; this module then changes the requested perimeter count of the
eligible child branches so later generation stops after that ring.

module_start() stores the object-local layer id and segregates the region-local
setting for the current island. module_after() acts only on the root's first
perimeter, after its children exist. With one setting value for the island, it
clamps every child when the option is enabled. With several regional values,
it splits each child against the enabled-area clip and clamps only the inside
parts, leaving outside siblings unchanged.

The normal call flow is:

    OnlyOnePerimeterFirstLayer::run_impl()
    `-- install module_vtable() in the perimeter-generation context
        `-- perimeter generator invokes module_start()
            |-- read layer id and segregate region settings
            `-- perimeter generator creates the first perimeter
                `-- module_after() for generated nodes
                    |-- ignore non-first layers
                    |-- ignore non-root or childless nodes
                    |-- uniform enabled setting?
                    |   `-- set every child perimeter count to one
                    `-- mixed regional settings?
                        |-- split children against each enabled clip
                        `-- set inside-child perimeter counts to one

The module is deliberately limited to the first root perimeter. Later layers,
deeper perimeter nodes, and disabled regions retain the requests made by the
selected perimeter generator or by other modules.
*/

namespace slic3r_api { namespace Perimeter { namespace OnlyOnePerimeterFirstLayerPlugin {

namespace {

const char *k_only_one_perimeter_first_layer_id = "perimeter.module.only_one_perimeter_first_layer";
const char *k_no_dependencies[] = { nullptr };
const raw_used_config_key k_used_config_keys[] = {
    { "only_one_perimeter_first_layer", RAW_CO_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE }
};
const char *k_only_one_perimeter_first_layer_key = "only_one_perimeter_first_layer";

class ModuleState
{
public:
    void initialize_region_settings(const PerimeterGenerationContextView &context, uint32_t layer_id)
    {
        // RegionSettings and the object-local layer id are invariant while the
        // host walks this perimeter tree. Build them once in start(); after()
        // only reads the cached state for each processed node.
        m_layer_id = layer_id;
        m_settings.reset(new RegionSettings(context.storage(), context.island(), {{k_only_one_perimeter_first_layer_key}}));
        m_settings->segregate(context.island().slice());
    }

    bool is_first_layer() const
    {
        return m_layer_id == 0;
    }

    const RegionSettings *settings() const
    {
        return m_settings.get();
    }

private:
    uint32_t m_layer_id = uint32_t(-1);
    std::unique_ptr<RegionSettings> m_settings;
};

void set_children_to_one_perimeter(const PerimeterNodeView &parent)
{
    // In the perimeter tree, children are the areas that would receive the next
    // perimeter ring. Setting their requested count to one stops all branches
    // after the already-generated first perimeter.
    const std::vector<PerimeterNodeView> children = parent.children_snapshot();
    for (const PerimeterNodeView &child : children)
        child.set_perimeter_needed(1);
}

void set_enabled_children_to_one_perimeter(PerimeterGenerationContextView &context,
                                           const PerimeterNodeView &parent,
                                           const RegionSettingsClip &enabled_area)
{
    // Region-local path: only children intersecting the enabled area should be
    // clamped. split_node() keeps the outside siblings alive so they may keep
    // generating the normal number of perimeters.
    const std::vector<PerimeterNodeView> children = parent.children_snapshot();
    for (const PerimeterNodeView &child : children) {
        const std::vector<PerimeterNodeView> inside_nodes = context.split_node(child, enabled_area);
        for (const PerimeterNodeView &inside_node : inside_nodes)
            inside_node.set_perimeter_needed(1);
    }
}

void *module_start(void *, perimeter_generation_context *context)
{
    // One state object per perimeter tree. It is intentionally not stored on the
    // plugin singleton, because multiple objects/layers may be processed in
    // parallel and RegionSettings clips are only meaningful for one island.
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
    return state;
}

void module_after(void *, void *user_context, perimeter_generation_context *context, perimeter_node *node)
{
    // This module acts in after(), because it needs the first generated ring to
    // create child areas first. Those children are then clamped so no second
    // perimeter is generated where the setting is active.
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

    if (!state->is_first_layer())
        return;

    PerimeterNodeView parent(node);
    // Only the root's first generated perimeter is allowed to clamp its
    // children. Deeper nodes already belong to the post-first-perimeter tree.
    if (parent.perimeter_idx() != 0 || parent.child_count() == 0)
        return;

    const RegionSettings *settings = state->settings();
    assert(settings != nullptr);
    if (settings == nullptr)
        return;

    // == solo_config code path ==
    // The whole island has one value. If enabled, every child branch is stopped
    // after the first perimeter. If disabled, this module is inert.
    if (!settings->has_many_config(k_only_one_perimeter_first_layer_key)) {
        if (settings->get_solo_config(k_only_one_perimeter_first_layer_key).get_bool(
                k_only_one_perimeter_first_layer_key))
            set_children_to_one_perimeter(parent);
        return;
    }

    // == many_config code path ==
    // Different regions/modifier areas have different values. Split the child
    // branches and clamp only the parts where the setting is enabled.
    const RegionSettings::AreaMap &areas = settings->get_areas(k_only_one_perimeter_first_layer_key);
    for (const auto &[setting_value, setting_clip] : areas) {
        if (!setting_value.get_bool(k_only_one_perimeter_first_layer_key))
            continue;
        set_enabled_children_to_one_perimeter(context_view, parent, setting_clip);
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

OnlyOnePerimeterFirstLayer &
OnlyOnePerimeterFirstLayer::instance(orchestrator_handle *orch)
{
    static OnlyOnePerimeterFirstLayer s_instance(orch);
    return s_instance;
}

const char *OnlyOnePerimeterFirstLayer::id_impl() const noexcept
{
    return k_only_one_perimeter_first_layer_id;
}

slicing_step_t OnlyOnePerimeterFirstLayer::step_impl() const noexcept
{
    return PERIMETER_GENERATION_MODULE;
}

const char *const *OnlyOnePerimeterFirstLayer::dependencies_impl() const noexcept
{
    return k_no_dependencies;
}

int32_t OnlyOnePerimeterFirstLayer::priority_impl() const noexcept
{
    return 7;
}

int32_t OnlyOnePerimeterFirstLayer::used_config_keys(raw_used_config_key *keys) const noexcept
{
    if (keys != nullptr)
        keys[0] = k_used_config_keys[0];
    return 1;
}

const char *OnlyOnePerimeterFirstLayer::progress_message_format_impl() const noexcept
{
    return "Only one perimeter on first layer: %u / %u";
}

void OnlyOnePerimeterFirstLayer::run_impl(const plugin_run_context *run_ctx) const
{
    run_ctx_perimeter_generation_module *ctx = plugin_ctx_as_perimeter_generation_module(run_ctx);
    if (ctx == nullptr)
        return;

    ctx->module.ctx = const_cast<OnlyOnePerimeterFirstLayer *>(this);
    ctx->module.vt = &module_vtable();
}

void register_only_one_perimeter_first_layer_plugin(orchestrator_handle *orch)
{
    orchestrator_register_plugin(orch, OnlyOnePerimeterFirstLayer::instance(orch).c_instance());
}

}}} // namespace slic3r_api::Perimeter::OnlyOnePerimeterFirstLayerPlugin

#ifdef ONLY_ONE_PERIMETER_FIRST_LAYER_PLUGIN_DLL
extern "C" void register_plugin(orchestrator_handle *orch)
{
    slic3r_api::Perimeter::OnlyOnePerimeterFirstLayerPlugin::register_only_one_perimeter_first_layer_plugin(orch);
}
#endif // ONLY_ONE_PERIMETER_FIRST_LAYER_PLUGIN_DLL
