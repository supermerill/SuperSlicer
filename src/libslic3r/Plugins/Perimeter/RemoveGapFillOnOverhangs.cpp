///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "RemoveGapFillOnOverhangs.hpp"

#include <cassert>
#include <cstdint>
#include <memory>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_perimeter.h"
#include "libslic3r/Api/plugin/cpp/ClipperViews.hpp"
#include "libslic3r/Api/plugin/cpp/PerimeterStepViews.hpp"

namespace slic3r_api { namespace Perimeter { namespace RemoveGapFillOnOverhangsPlugin {

namespace {

const char *k_remove_gap_fill_on_overhangs_id = "perimeter.module.remove_gap_fill_on_overhangs";
const char *k_no_dependencies[] = { nullptr };
const raw_used_config_key k_used_config_keys[] = {
    { "gap_fill_no_overhang", RAW_CO_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE }
};
const char *k_gap_fill_no_overhang_key = "gap_fill_no_overhang";

StoredExPolygonCollection lower_slice_coverage(storage_handle *storage, const LayerIsland &island);

class ModuleState
{
public:
    void initialize(const PerimeterGenerationContextView &context)
    {
        // The host calls this module once for every node of the same perimeter
        // tree. The current island, the lower islands and the region settings do
        // not change during that walk, so build the expensive inputs once here.
        // Each after() call then only clips those cached inputs to the current
        // node area.
        const LayerIsland island = context.island();
        m_region_count = island.region_count();
        m_settings.reset(new RegionSettings(context.storage(), island, {{k_gap_fill_no_overhang_key}}));
        m_settings->segregate(island.slice());
        m_lower_slices.reset(new StoredExPolygonCollection(lower_slice_coverage(context.storage(), island)));

        m_disabled =
            !m_settings->has_many_config(k_gap_fill_no_overhang_key) &&
            !m_settings->get_solo_config(k_gap_fill_no_overhang_key).get_bool(k_gap_fill_no_overhang_key);
    }

    bool ready() const
    {
        return m_region_count > 0 && m_settings != nullptr && m_lower_slices != nullptr;
    }

    uint32_t region_count() const
    {
        return m_region_count;
    }

    bool disabled() const
    {
        return m_disabled;
    }

    const RegionSettings &settings() const
    {
        assert(m_settings != nullptr);
        return *m_settings;
    }

    ExPolygonCollection lower_slices() const
    {
        assert(m_lower_slices != nullptr);
        return m_lower_slices->readonly();
    }

private:
    uint32_t m_region_count = 0;
    bool m_disabled = true;
    std::unique_ptr<RegionSettings> m_settings;
    std::unique_ptr<StoredExPolygonCollection> m_lower_slices;
};

StoredExPolygonCollection lower_slice_coverage(storage_handle *storage, const LayerIsland &island)
{
    // For this module, lower geometry is only a yes/no mask: if a gap-fill line
    // is above any lower island, it is considered supported. Union the lower
    // slices once so later node-level tests can use one compact coverage area.
    StoredExPolygonCollection lower_slices(storage);
    const std::vector<LayerIsland> lower_islands = island.lower_islands();
    for (const LayerIsland &lower_island : lower_islands)
        lower_slices.push_back(lower_island.slice());

    if (lower_slices.empty())
        return lower_slices;

    ClipperContext clip(storage);
    return clipper_union(clip(lower_slices)).to_expolygon_collection();
}

StoredExPolygonCollection node_area_collection(storage_handle *storage, const PerimeterNodeView &node)
{
    // The clipper helpers operate on collections. A perimeter node owns exactly
    // one area, so wrap it without changing its meaning.
    StoredExPolygonCollection area(storage);
    area.push_back(node.area());
    return area;
}

// Compute the area where open gap-fill lines must be removed for this node.
//
// The result is:
//   current node area
//   intersected with the regions/modifiers where gap_fill_no_overhang is true
//   minus the union of lower islands.
//
// The function returns an empty collection when the setting is disabled for the
// node, or when every enabled part of the node is already supported below.
StoredExPolygonCollection gap_fill_no_overhang_area(const PerimeterGenerationContextView &context,
                                                    const PerimeterNodeView &node,
                                                    const ModuleState &state)
{
    storage_handle *storage = context.storage();
    StoredExPolygonCollection forbidden_area(storage);
    StoredExPolygonCollection node_area = node_area_collection(storage, node);
    const ExPolygonCollection lower_slices = state.lower_slices();
    const RegionSettings::AreaMap &areas = state.settings().get_areas(k_gap_fill_no_overhang_key);

    for (const auto &[setting_value, setting_clip] : areas) {
        if (!setting_value.get_bool(k_gap_fill_no_overhang_key))
            continue;

        // A uniform true value means "apply to the whole node". A local true
        // value means "apply only to the part of this node covered by this
        // region or modifier". This is what lets the setting vary by region
        // without forcing the generator to split the whole island up front.
        StoredExPolygonCollection enabled_area = setting_clip.intersections(node_area);
        if (enabled_area.empty())
            continue;

        // The setting forbids gap fill only in the unsupported part. If there
        // are no lower islands, every enabled area is unsupported. Otherwise,
        // subtract the cached lower coverage and keep only what remains.
        StoredExPolygonCollection unsupported_area(storage);
        if (lower_slices.empty())
            unsupported_area.move_from(std::move(enabled_area));
        else {
            ClipperContext clip(storage);
            unsupported_area =
                clipper_diff(clip(enabled_area), clip(lower_slices)).to_expolygon_collection();
        }

        if (!unsupported_area.empty())
            forbidden_area.append_move_from(std::move(unsupported_area));
    }

    if (!forbidden_area.empty()) {
        // Several regions may contribute overlapping forbidden fragments. Merge
        // them before clipping polylines so each candidate is clipped once
        // against a clean area.
        ClipperContext clip(storage);
        forbidden_area = clipper_union(clip(forbidden_area)).to_expolygon_collection();
        forbidden_area.ensure_valid();
    }
    return forbidden_area;
}

void append_entity_without_overhang_gap_fill(storage_handle *storage,
                                             StoredExtrusionEntity &dst,
                                             const ExtrusionEntity &entity,
                                             const ExPolygonCollection &forbidden_area)
{
    // Closed local polylines are perimeter walls, and entities without a local
    // polyline are collections or empty nodes. This module only removes open
    // local strokes, which are the gap-fill candidates produced in this part of
    // the perimeter pipeline.
    if (!entity.has_polyline() || entity.local_is_closed() || entity.point_count() < 2) {
        dst.add_child(entity);
        return;
    }

    // Clipping may split one open line into several printable fragments. Each
    // fragment starts as a clone of the original entity so role, flow and other
    // properties survive the operation, then receives its own clipped polyline.
    // If gap fill later starts carrying arc data, this needs an ArcPolyline-
    // aware clipper instead of the plain polyline helper.
    StoredPolyline source_polyline(storage);
    const std::vector<c_point> points = entity.points();
    source_polyline.insert_array(0, points.data(), static_cast<uint32_t>(points.size()));
    StoredPolylineCollection fragments =
        clipper_diff_polyline_expolygons(storage, source_polyline, forbidden_area);

    for (const Polyline fragment : fragments) {
        if (fragment.size() < 2)
            continue;

        StoredExtrusionEntity clipped_entity(storage, entity);
        clipped_entity.set(fragment);
        if (!clipped_entity.empty())
            dst.add_child(clipped_entity.mutable_view());
    }
}

void remove_gap_fill_on_overhangs(storage_handle *storage,
                                  MutableExtrusionEntity extrusions,
                                  const ExPolygonCollection &forbidden_area)
{
    if (forbidden_area.empty() || extrusions.empty() || extrusions.child_count() == 0)
        return;

    // Rebuild the child collection in a temporary entity. This keeps iteration
    // over the original children simple and avoids editing the collection while
    // clipping may add zero, one or many replacement fragments.
    StoredExtrusionEntity clipped_extrusions(storage, extrusions.readonly());
    clipped_extrusions.clear_content();

    const uint32_t child_count = extrusions.child_count();
    for (uint32_t child_idx = 0; child_idx < child_count; ++child_idx)
        append_entity_without_overhang_gap_fill(storage, clipped_extrusions, extrusions.child(child_idx), forbidden_area);

    // Replace the node output only after every child has been processed. If all
    // open strokes are removed, the resulting collection can legitimately be
    // empty; the node area/fill-area data is not changed here.
    const bool moved = extrusion_move_from(extrusions.mutable_handle(), clipped_extrusions.mutable_handle()) != 0;
    assert(moved);
    (void) moved;
}

void *module_start(void *, perimeter_generation_context *context)
{
    // Start is called once before the host walks a perimeter tree. Return a
    // per-run state object instead of storing data on the plugin singleton:
    // multiple islands can be processed in parallel, and each one has different
    // region settings and lower coverage.
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

    state->initialize(context_view);
    return state;
}

void module_after(void *, void *user_context, perimeter_generation_context *context, perimeter_node *node)
{
    // After is called after the selected perimeter generator has filled this
    // node with extrusion entities. At this point the module can remove unsafe
    // open gap-fill fragments without influencing how children are generated.
    ModuleState *state = static_cast<ModuleState *>(user_context);
    assert(context != nullptr);
    assert(node != nullptr);
    assert(state != nullptr);
    if (context == nullptr || node == nullptr || state == nullptr)
        return;

    PerimeterGenerationContextView context_view(context);
    const uint32_t region_count = context_view.island().region_count();
    assert(region_count > 0);
    assert(!state->ready() || state->region_count() == region_count);
    if (region_count == 0 || !state->ready() || state->disabled())
        return;

    PerimeterNodeView parent(node);
    // The host stores node output as a collection of child extrusions. The
    // module rewrites only that output collection after clipping open gap-fill
    // candidates; it does not touch node areas, fill areas or child nodes.
    if (parent.extrusions().child_count() == 0)
        return;

    StoredExPolygonCollection forbidden_area =
        gap_fill_no_overhang_area(context_view, parent, *state);
    remove_gap_fill_on_overhangs(context_view.storage(), parent.extrusions(), forbidden_area);
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

RemoveGapFillOnOverhangs &
RemoveGapFillOnOverhangs::instance(orchestrator_handle *orch)
{
    static RemoveGapFillOnOverhangs s_instance(orch);
    return s_instance;
}

const char *RemoveGapFillOnOverhangs::id_impl() const noexcept
{
    return k_remove_gap_fill_on_overhangs_id;
}

slicing_step_t RemoveGapFillOnOverhangs::step_impl() const noexcept
{
    return PERIMETER_GENERATION_MODULE;
}

const char *const *RemoveGapFillOnOverhangs::dependencies_impl() const noexcept
{
    return k_no_dependencies;
}

int32_t RemoveGapFillOnOverhangs::priority_impl() const noexcept
{
    return 10;
}

int32_t RemoveGapFillOnOverhangs::used_config_keys(raw_used_config_key *keys) const noexcept
{
    if (keys != nullptr)
        for (uint32_t idx = 0; idx < sizeof(k_used_config_keys) / sizeof(k_used_config_keys[0]); ++idx)
            keys[idx] = k_used_config_keys[idx];
    return int32_t(sizeof(k_used_config_keys) / sizeof(k_used_config_keys[0]));
}

const char *RemoveGapFillOnOverhangs::progress_message_format_impl() const noexcept
{
    return "Remove gap fill on overhangs: %u / %u";
}

void RemoveGapFillOnOverhangs::run_impl(const plugin_run_context *run_ctx) const
{
    run_ctx_perimeter_generation_module *ctx = plugin_ctx_as_perimeter_generation_module(run_ctx);
    if (ctx == nullptr)
        return;

    ctx->module.ctx = const_cast<RemoveGapFillOnOverhangs *>(this);
    ctx->module.vt = &module_vtable();
}

void register_remove_gap_fill_on_overhangs_plugin(orchestrator_handle *orch)
{
    orchestrator_register_plugin(orch, RemoveGapFillOnOverhangs::instance(orch).c_instance());
}

}}} // namespace slic3r_api::Perimeter::RemoveGapFillOnOverhangsPlugin

#ifdef REMOVE_GAP_FILL_ON_OVERHANGS_PLUGIN_DLL
extern "C" void register_plugin(orchestrator_handle *orch)
{
    slic3r_api::Perimeter::RemoveGapFillOnOverhangsPlugin::register_remove_gap_fill_on_overhangs_plugin(orch);
}
#endif // REMOVE_GAP_FILL_ON_OVERHANGS_PLUGIN_DLL
