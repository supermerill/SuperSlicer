///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "ExtraPerimeterBelowArea.hpp"

#include <cassert>
#include <cstdint>
#include <memory>
#include <set>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_perimeter.h"
#include "libslic3r/Api/plugin/cpp/PerimeterStepViews.hpp"

/*
ExtraPerimeterBelowArea
=======================

This perimeter-generation module requests additional perimeter passes when a
remaining child area is smaller than `extra_perimeters_below_area`. It does not
construct an extrusion or perform an offset itself; it changes the number of
passes that the selected perimeter generator will attempt on that branch.

The normal execution flow is:

    module_start()
    `-- build one RegionSettings map for the current perimeter island

    module_after()
    |-- wait until the generator has created child areas
    |-- ignore an empty zero-perimeter root used only as a traversal seed
    |-- read the setting for each regional area clip
    |-- test every eligible child area against its threshold
    `-- request a large additional perimeter count for small areas

The module marks a branch after applying the rule and checks its ancestors, so
descendants do not repeatedly receive the same request. A uniform disabled
setting exits immediately; mixed regional settings are handled independently
after RegionSettings splits the island into clips. The threshold is interpreted
in square millimetres, with percentages based on the current perimeter-width
reference, and is converted to the scaled area units used by polygons.

`module_end()` releases the per-tree state after the host finishes its walk.
Because the module runs in `after()`, it can inspect the area produced by the
preceding perimeter pass. The large request is only an upper bound: the host
generator stops earlier when the branch has no remaining printable area. This
module therefore controls branch continuation but does not decide the exact
number of useful rings.
*/

namespace slic3r_api { namespace Perimeter { namespace ExtraPerimeterBelowAreaPlugin {

namespace {

const char *k_extra_perimeter_below_area_id = "perimeter.module.extra_perimeter_below_area";
const char *k_no_dependencies[] = { nullptr };
const raw_used_config_key k_used_config_keys[] = {
    { "extra_perimeters_below_area", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE }
};
const char *k_extra_perimeter_below_area_key = "extra_perimeters_below_area";
// This module does not know how many extra rings will be needed to consume the
// small area. Instead it asks the host loop to keep trying until the generator
// can no longer create inner child areas. A large value is enough because the
// child area normally disappears before reaching this counter.
const uint32_t k_force_many_perimeters = 9999;

double threshold_area_scaled(const PerimeterGenerationContextView &context, const RegionSettingsValue &value)
{
    // extra_perimeters_below_area is expressed as an area in mm2, with support
    // for the same "effective value" rules as normal config options. The
    // reference area is one perimeter width squared, matching the old setting
    // semantics where percentages are relative to the extrusion footprint.
    const c_flow flow = context.perimeter_flow();
    const double perimeter_width_mm = unscaled(flow.width);
    const double reference_area_mm2 = perimeter_width_mm * perimeter_width_mm;
    const double area_mm2 = value.get_effective_value(reference_area_mm2, k_extra_perimeter_below_area_key);
    // Polygon areas are stored in scaled coordinates, so an mm2 threshold must
    // be scaled twice before it can be compared with ExPolygon::area().
    return scale_d(scale_d(area_mm2));
}

void force_extra_perimeters_if_small(const PerimeterNodeView &node, double area_scaled)
{
    // The node represents the area left for the next perimeter pass. If it is
    // below the configured threshold, request enough extra passes for the main
    // host loop to keep processing this branch until the generator exhausts it.
    if (node.area().area() < area_scaled)
        node.add_perimeters(k_force_many_perimeters);
}

class ModuleState
{
public:
    void initialize_region_settings(const PerimeterGenerationContextView &context)
    {
        // RegionSettings only depends on the current island, its regions and
        // the static config values. None of those change while the host walks
        // the perimeter tree, so doing this once in start() avoids rebuilding
        // and reclipping the same setting map for every node.
        m_settings.reset(new RegionSettings(context.storage(), context.island(), {{k_extra_perimeter_below_area_key}}));
        m_settings->segregate(context.island().slice());
    }

    const RegionSettings *settings() const
    {
        return m_settings.get();
    }

    bool check_and_set(const perimeter_node *node)
    {
        // The callback is called once for every processed node. After this
        // module requests a very large number of perimeters, many descendants
        // will be generated. Without this guard, every descendant would request
        // the same "many more" count again and the module would overdrive the
        // branch indefinitely.
        const perimeter_node *current = node;
        while (current != nullptr) {
            // If this node or one of its ancestors already ran the module, the
            // branch is already under the "below area" rule.
            if (seen_nodes.find(current) != seen_nodes.end())
                return true;
            current = current->parent == current ? nullptr : current->parent;
        }
        // Mark only the current node. Descendants are implicitly covered by the
        // ancestor walk above.
        seen_nodes.insert(node);
        return false;
    }

private:
    std::unique_ptr<RegionSettings> m_settings;
    std::set<const perimeter_node *> seen_nodes;
};

void module_after(void *, void *user_context, perimeter_generation_context *context, perimeter_node *node)
{
    // This module acts in after(), not before(), because it needs the generator
    // to create child areas first. Those children are what we test against the
    // area threshold and optionally ask to process again.
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

    PerimeterNodeView parent(node);
    // No children means the generator has no remaining inner area to extend.
    // check_and_set() also prevents applying the same rule twice on one branch.
    if (parent.child_count() == 0 || state->check_and_set(node))
        return;
    // With perimeters=0, generators may still create a root child area as a
    // traversal seed without emitting any root extrusion. This module must not
    // bootstrap perimeters from that seed. If another module requested a real
    // root perimeter, parent.extrusions() is non-empty and the child areas may
    // legitimately be extended below the area threshold.
    if (!parent.parent().valid() && parent.extrusions().empty())
        return;

    // RegionSettings was built once in start(). It gives a per-area view of the
    // option. If all regions agree, the area map contains one "accept all"
    // entry. If overlapping regions have different values, segregate() split
    // the island into clips for each value.
    const RegionSettings *settings = state->settings();
    assert(settings != nullptr);
    if (settings == nullptr)
        return;

    // Fast exit for the common case: one uniform value and that value disables
    // the option. With many configs, some clips may still be enabled even if
    // others are disabled, so the loop below handles them one by one.
    if (!settings->has_many_config(k_extra_perimeter_below_area_key) &&
        settings->get_solo_config(k_extra_perimeter_below_area_key).get_float(k_extra_perimeter_below_area_key) <= 0.)
        return;

    // Take a snapshot because split_node() may rebuild the parent's child array.
    // The PerimeterNodeView snapshot stores node pointers, so it stays usable as
    // long as the pointed nodes themselves are not destroyed by the callback.
    const std::vector<PerimeterNodeView> children = parent.children_snapshot();
    for (const PerimeterNodeView &child : children) {
        // Another module or the generator may have already requested more
        // perimeters for this child. Let that request run first; this module
        // will see the generated descendants in later after() calls.
        if (child.needs_more_perimeters())
            continue;

        const RegionSettings::AreaMap &areas = settings->get_areas(k_extra_perimeter_below_area_key);
        for (const auto &[setting_value, setting_clip] : areas) {
            // Disabled clips explicitly opt out of the rule.
            if (setting_value.get_float(k_extra_perimeter_below_area_key) <= 0.)
                continue;

            const double area_scaled = threshold_area_scaled(context_view, setting_value);
            if (setting_clip.is_accept_all()) {
                // Uniform setting: the whole child is governed by one value, so
                // no geometric split is needed.
                force_extra_perimeters_if_small(child, area_scaled);
                continue;
            }

            // Region-local setting: split the child into the part covered by
            // this clip and the part outside. Only the inside nodes inherit the
            // threshold value from this entry.
            const std::vector<PerimeterNodeView> inside_nodes = context_view.split_node(child, setting_clip);
            for (const PerimeterNodeView &inside_node : inside_nodes)
                force_extra_perimeters_if_small(inside_node, area_scaled);
        }
    }
}

void *module_start(void *, perimeter_generation_context *context)
{
    // One state object per perimeter tree. It is intentionally not stored on the
    // plugin singleton, because multiple objects/layers may be processed in
    // parallel and node pointers are only meaningful inside one tree.
    ModuleState *state = new ModuleState();
    assert(context != nullptr);
    if (context == nullptr)
        return state;

    PerimeterGenerationContextView context_view(context);
    const uint32_t region_count = context_view.island().region_count();
    assert(region_count > 0);
    if (region_count > 0)
        state->initialize_region_settings(context_view);
    return state;
}

void module_end(void *, void *user_context, perimeter_generation_context *)
{
    // The host guarantees that end() is called for every successful start().
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

ExtraPerimeterBelowArea &
ExtraPerimeterBelowArea::instance(orchestrator_handle *orch)
{
    static ExtraPerimeterBelowArea s_instance(orch);
    return s_instance;
}

const char *ExtraPerimeterBelowArea::id_impl() const noexcept
{
    return k_extra_perimeter_below_area_id;
}

slicing_step_t ExtraPerimeterBelowArea::step_impl() const noexcept
{
    return PERIMETER_GENERATION_MODULE;
}

const char *const *ExtraPerimeterBelowArea::dependencies_impl() const noexcept
{
    return k_no_dependencies;
}

int32_t ExtraPerimeterBelowArea::priority_impl() const noexcept
{
    return -10;
}

int32_t ExtraPerimeterBelowArea::used_config_keys(raw_used_config_key *keys) const noexcept
{
    if (keys != nullptr)
        keys[0] = k_used_config_keys[0];
    return 1;
}

const char *ExtraPerimeterBelowArea::progress_message_format_impl() const noexcept
{
    return "Extra perimeter below area: %u / %u";
}

void ExtraPerimeterBelowArea::run_impl(const plugin_run_context *run_ctx) const
{
    run_ctx_perimeter_generation_module *ctx = plugin_ctx_as_perimeter_generation_module(run_ctx);
    if (ctx == nullptr)
        return;

    ctx->module.ctx = const_cast<ExtraPerimeterBelowArea *>(this);
    ctx->module.vt = &module_vtable();
}

void register_extra_perimeter_below_area_plugin(orchestrator_handle *orch)
{
    orchestrator_register_plugin(orch, ExtraPerimeterBelowArea::instance(orch).c_instance());
}

}}} // namespace slic3r_api::Perimeter::ExtraPerimeterBelowAreaPlugin

#ifdef EXTRA_PERIMETER_BELOW_AREA_PLUGIN_DLL
extern "C" void register_plugin(orchestrator_handle *orch)
{
    slic3r_api::Perimeter::ExtraPerimeterBelowAreaPlugin::register_extra_perimeter_below_area_plugin(orch);
}
#endif // EXTRA_PERIMETER_BELOW_AREA_PLUGIN_DLL
