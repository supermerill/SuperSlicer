///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "SeparateHoleContour.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <map>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_perimeter.h"
#include "libslic3r/Api/plugin/cpp/ClipperViews.hpp"
#include "libslic3r/Api/plugin/cpp/PerimeterStepViews.hpp"

namespace slic3r_api { namespace Perimeter { namespace SeparateHoleContourPlugin {

namespace {

const char *k_separate_hole_contour_id = "perimeter.module.separate_hole_contour";
const char *k_no_dependencies[] = { nullptr };
const raw_used_config_key k_used_config_keys[] = {
    { "perimeters_hole", RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "perimeters", RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE }
};
const char *k_perimeters_hole_key = "perimeters_hole";
const char *k_perimeters_key = "perimeters";

// Same bit value as Slic3r::ExtrusionLoopRole::elrHole. The perimeter property
// is a C payload, so the module only needs the ABI bit, not the C++ enum type.
const int32_t k_perimeter_loop_role_hole = 1 << 3;

/*
SeparateHoleContour is a perimeter-generation module, not a generator.

The active STEP_PERIMETER generator first creates a normal perimeter tree. This
module then edits each generated node so contours and holes can stop at
different shell counts:

  - perimeters controls contour loops.
  - perimeters_hole controls hole loops when enabled.
  - the root asks the generator for max(perimeters, perimeters_hole) shells.
  - after each generated node, this module removes the class that is past its
    configured count and rebuilds child areas from the loops that remain.

That last rebuild is the important bit. Children are created by the generator
before modules run. If a module removes a loop, the previous child areas still
assume that removed loop exists. Rebuilding keeps the next perimeter level and
the later fill areas consistent with the edited extrusion tree.
*/
struct HoleContourCount
{
    // Effective class limits for this branch. They preserve the configured
    // contour/hole difference, but start from the current root perimeter count
    // so modules that already added or removed shell levels are respected.
    int32_t max_hole_count = 0;
    int32_t max_contour_count = 0;

    // Number of shell levels where this module actually removed that class.
    // These counters are propagated to child nodes, so they describe the current
    // branch, not the whole island globally.
    int32_t hole_deleted = 0;
    int32_t contour_deleted = 0;
};

struct EraseDecision
{
    bool holes = false;
    bool contours = false;
};

class ModuleState
{
public:
    bool get(const perimeter_node *node, HoleContourCount &out) const
    {
        assert(node != nullptr);
        if (node == nullptr)
            return false;

        const std::map<const perimeter_node *, HoleContourCount>::const_iterator it = counts.find(node);
        if (it == counts.end())
            return false;

        out = it->second;
        return true;
    }

    void set(const perimeter_node *node, const HoleContourCount &count)
    {
        assert(node != nullptr);
        if (node == nullptr)
            return;

        counts[node] = count;
    }

private:
    // Host perimeter nodes have stable addresses during one run_region_group()
    // traversal. The state is local to that traversal and is deleted in end().
    std::map<const perimeter_node *, HoleContourCount> counts;
};

uint32_t count_from_config(int32_t value)
{
    return value <= 0 ? 0 : uint32_t(value);
}

int32_t count_from_node(const PerimeterNodeView &node)
{
    const uint32_t count = node.perimeter_needed();
    const uint32_t max_int32 = uint32_t(std::numeric_limits<int32_t>::max());
    return int32_t(std::min(count, max_int32));
}

bool extrusion_is_hole_perimeter(const ExtrusionEntity &entity)
{
    const EPropertyPerimeter *perimeter = entity.property<EPropertyPerimeter>();
    return perimeter != nullptr && (perimeter->perimeter_role() & k_perimeter_loop_role_hole) != 0;
}

bool extrusion_is_perimeter_loop_candidate(const ExtrusionEntity &entity)
{
    // Gap fill and other open extrusions may live in the same node. Only closed
    // generated perimeter loops participate in contour/hole filtering.
    return !entity.empty() && entity.is_closed();
}

size_t erase_perimeter_class(MutableExtrusionEntity extrusions, bool erase_holes)
{
    // Iterate backwards because remove_child() shifts later indexes.
    size_t erased_count = 0;
    for (uint32_t idx = extrusions.child_count(); idx > 0; --idx) {
        const uint32_t child_idx = idx - 1;
        const ExtrusionEntity child = extrusions.child(child_idx);
        if (!extrusion_is_perimeter_loop_candidate(child))
            continue;
        if (extrusion_is_hole_perimeter(child) != erase_holes)
            continue;

        if (extrusions.remove_child(child_idx))
            ++erased_count;
    }
    return erased_count;
}

double cleanup_distance_from_flow(const c_flow &flow)
{
    assert(flow.spacing >= 0);
    return std::max<double>(double(SCALED_EPSILON), 0.1 * double(flow.spacing));
}

c_flow external_perimeter_flow(const PerimeterGenerationContextView &context)
{
    const LayerIsland island = context.island();
    assert(island.region_count() > 0);
    return island.region_count() > 0 ? island.region(0).flow(RAW_EXTRUSION_ROLE_EXTERNAL_PERIMETER) : c_flow{};
}

void append_closed_entity_polygons(storage_handle *storage,
                                   StoredPolygonCollection &polygons,
                                   const ExtrusionEntity &entity)
{
    assert(storage != nullptr);

    if (entity.has_polyline() && entity.local_is_closed()) {
        std::vector<c_point> points = entity.points();
        if (points.size() > 1 && points_equal(points.front(), points.back()))
            points.pop_back();

        if (points.size() >= 3) {
            StoredPolygon polygon(storage);
            polygon.insert_array(0, points.data(), uint32_t(points.size()));
            polygon.make_counter_clockwise();
            // Clipper treats polygons as filled areas. Normalize orientation so
            // a closed perimeter loop can be converted to a robust coverage
            // polygon regardless of the path direction chosen by the generator.
            if (polygon.valid_polygon())
                polygons.push_back(polygon.readonly());
        }
    }

    for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        append_closed_entity_polygons(storage, polygons, entity.child(child_idx));
}

StoredExPolygonCollection collection_from_expolygon(storage_handle *storage,
                                                    const ExPolygon &expolygon)
{
    assert(storage != nullptr);
    StoredExPolygonCollection collection(storage);
    collection.push_back(expolygon);
    return collection;
}

StoredExPolygonCollection offset_collection(storage_handle *storage,
                                            const ExPolygonCollection &subject,
                                            double delta)
{
    assert(storage != nullptr);
    if (subject.empty())
        return StoredExPolygonCollection(storage);

    ClipperContext clipper(storage);
    return clipper_offset(clipper(subject), delta).to_expolygon_collection();
}

StoredExPolygonCollection offset2_collection(storage_handle *storage,
                                             const ExPolygonCollection &subject,
                                             double delta1,
                                             double delta2)
{
    assert(storage != nullptr);
    if (subject.empty())
        return StoredExPolygonCollection(storage);

    ClipperContext clipper(storage);
    return clipper_offset2(clipper(subject), delta1, delta2).to_expolygon_collection();
}

StoredExPolygonCollection diff_collection(storage_handle *storage,
                                          const ExPolygonCollection &subject,
                                          const ExPolygonCollection &clip_area)
{
    assert(storage != nullptr);
    if (subject.empty())
        return StoredExPolygonCollection(storage);
    if (clip_area.empty())
        return subject.clone(storage);

    ClipperContext clipper(storage);
    return clipper_diff(clipper(subject), clipper(clip_area)).to_expolygon_collection();
}

StoredExPolygonCollection extrusion_coverage_area(storage_handle *storage,
                                                  const ExtrusionEntity &extrusions,
                                                  double radius,
                                                  double cleanup_distance)
{
    assert(storage != nullptr);
    assert(radius >= 0.);
    assert(cleanup_distance >= 0.);

    StoredPolygonCollection polygons(storage);
    append_closed_entity_polygons(storage, polygons, extrusions);
    if (polygons.empty())
        return StoredExPolygonCollection(storage);

    // Closed perimeter loops are centerlines. To know which area remains
    // available for the next child node, turn kept centerlines into a physical
    // coverage area. CLOSED_LINE is intentional: a loop centerline covers a
    // stroke around the line, not the whole polygon interior.
    ClipperContext clipper(storage);
    StoredExPolygonCollection area =
        clipper_union(clipper_offset(clipper(polygons),
                                     std::max(radius, cleanup_distance),
                                     CLIPPER_JOIN_MITER,
                                     3.0,
                                     CLIPPER_END_CLOSED_LINE)).to_expolygon_collection();
    if (!area.empty())
        area = offset2_collection(storage, area.readonly(), cleanup_distance, -cleanup_distance);
    return area;
}

StoredExPolygonCollection build_child_areas(storage_handle *storage,
                                            const PerimeterNodeView &parent,
                                            const ExPolygonCollection &kept_extrusion_area,
                                            double cleanup_distance)
{
    assert(storage != nullptr);

    // Child areas are the strict no-overlap domains for the next perimeter
    // level. They are rebuilt from the parent area minus the physical coverage
    // of the perimeter class that stayed in this node.
    StoredExPolygonCollection parent_area = collection_from_expolygon(storage, parent.area());
    StoredExPolygonCollection child_areas = kept_extrusion_area.empty() ?
        parent_area.readonly().clone(storage) :
        diff_collection(storage, parent_area, kept_extrusion_area);

    if (!child_areas.empty())
        child_areas = offset2_collection(storage, child_areas.readonly(), -cleanup_distance, cleanup_distance);
    return child_areas;
}

StoredExPolygonCollection build_fill_areas(storage_handle *storage,
                                           const PerimeterNodeView &parent,
                                           const ExPolygonCollection &kept_extrusion_area,
                                           double cleanup_distance)
{
    assert(storage != nullptr);

    // Fill areas intentionally remain a little larger than child areas. They
    // are anchoring domains for later infill; if they collapse to child areas,
    // infill loses the controlled encroachment into perimeter material.
    StoredExPolygonCollection base_fill_area = collection_from_expolygon(storage, parent.fill_area());
    StoredExPolygonCollection fill_areas = kept_extrusion_area.empty() ?
        base_fill_area.readonly().clone(storage) :
        diff_collection(storage, base_fill_area, kept_extrusion_area);

    if (!fill_areas.empty())
        fill_areas = offset2_collection(storage, fill_areas.readonly(), -cleanup_distance, cleanup_distance);
    return fill_areas;
}

void store_for_children(ModuleState &state,
                        const PerimeterNodeView &parent,
                        const HoleContourCount &data)
{
    // After a rebuild, old child handles are invalid. Take a fresh snapshot and
    // associate the branch counters with the newly created children.
    const std::vector<PerimeterNodeView> children = parent.children_snapshot();
    for (const PerimeterNodeView &child : children)
        state.set(child.handle(), data);
}

void set_if_child_needs_one_less_perimeter(const PerimeterNodeView &child)
{
    // When both classes were removed from the parent, the child should not
    // spend one more generator pass recreating the same now-deleted shell.
    if (child.perimeter_needed() > child.perimeter_idx())
        child.set_perimeter_needed(child.perimeter_needed() - 1);
}

EraseDecision erase_decision_for_node(const PerimeterNodeView &node,
                                      const HoleContourCount &data)
{
    const int32_t perimeter_idx = int32_t(node.perimeter_idx());
    const int32_t diff_contour_hole = data.max_contour_count - data.max_hole_count;

    EraseDecision decision;
    decision.holes = data.max_hole_count == 0;
    decision.contours = data.max_contour_count == 0;

    // Only the class with the smaller configured count is capped here. The
    // larger class may have been extended by another module, such as
    // ExtraPerimeterCount, which increases node.perimeter_needed before this
    // module sees the generated tree.
    if (!decision.contours && diff_contour_hole < 0) {
        if (perimeter_idx >= data.max_contour_count)
            decision.contours = true;
    }

    if (!decision.holes && diff_contour_hole > 0) {
        if (perimeter_idx >= data.max_hole_count)
            decision.holes = true;
    }

    return decision;
}

void *module_start(void *, perimeter_generation_context *context)
{
    // Always allocate the per-run state here, even for no-op cases. The host
    // will later call end(), and end() can then use the same simple delete path
    // for active and inactive runs.
    ModuleState *state = new ModuleState();
    if (context == nullptr || context->root == nullptr)
        return state;

    PerimeterGenerationContextView context_view(context);
    if (context_view.island().region_count() == 0)
        return state;

    // Build RegionSettings once per perimeter tree. The current module supports
    // one effective value pair for the whole island; mixed per-region values
    // require splitting the tree by setting area and are intentionally left as a
    // no-op until that design is implemented.
    RegionSettings settings = context_view.region_settings({{k_perimeters_hole_key, k_perimeters_key}});
    settings.segregate(context_view.island().slice());

    // Region-varying perimeter counts need node splitting by the active areas.
    // The old in-core implementation only supported one value pair per island
    // here, so keep that restriction until the generator tree is complete.
    if (settings.has_many_config(k_perimeters_hole_key))
        return state;

    const RegionSettingsValue &values = settings.get_solo_config(k_perimeters_hole_key);
    if (!values.is_enabled(k_perimeters_hole_key))
        return state;

    const int32_t configured_hole_count = values.get_int(k_perimeters_hole_key);
    const int32_t configured_contour_count = values.get_int(k_perimeters_key);

    // Equal counts mean there is no separate contour/hole policy to apply.
    // This includes the enabled 0/0 case: the generator owns "no perimeters",
    // this module only owns differences between the two counts.
    if (configured_hole_count == configured_contour_count)
        return state;

    HoleContourCount data;
    const int32_t diff_contour_hole = configured_contour_count - configured_hole_count;
    data.max_contour_count = count_from_node(context_view.root());
    data.max_hole_count = std::max<int32_t>(0, data.max_contour_count - diff_contour_hole);

    // The generator must create enough levels for the larger effective count.
    // If a previous module already increased the root, the differential is kept:
    // perimeters=2, perimeters_hole=5, extra=2 becomes contour=4, hole=7.
    const uint32_t requested_perimeter_count = count_from_config(
        std::max(data.max_hole_count, data.max_contour_count));
    if (requested_perimeter_count > context_view.root().perimeter_needed())
        context_view.root().set_perimeter_needed(requested_perimeter_count);

    state->set(context->root, data);
    return state;
}

void module_after(void *, void *user_context, perimeter_generation_context *context, perimeter_node *node)
{
    ModuleState *state = static_cast<ModuleState *>(user_context);
    if (context == nullptr || node == nullptr || state == nullptr)
        return;

    PerimeterGenerationContextView context_view(context);
    if (context_view.island().region_count() == 0)
        return;

    HoleContourCount data;
    if (!state->get(node, data))
        return;

    PerimeterNodeView parent(node);
    const EraseDecision decision = erase_decision_for_node(parent, data);

    if (!decision.holes && !decision.contours) {
        // This node is still within both requested counts. Nothing structural
        // changed, so keep the generator children and simply propagate state.
        store_for_children(*state, parent, data);
        state->set(node, data);
        return;
    }

    MutableExtrusionEntity extrusions = parent.extrusions();
    if (decision.contours && decision.holes) {
        // Both classes are beyond their requested count. This is unusual in the
        // normal pipeline because equal counts are a no-op, but it can happen
        // on child branches after topology changes. Remove both classes and
        // shrink child work so the generator does not recreate this shell.
        const size_t erased_holes = erase_perimeter_class(extrusions, true);
        const size_t erased_contours = erase_perimeter_class(extrusions, false);
        if (erased_contours > 0)
            ++data.contour_deleted;
        if (erased_holes > 0)
            ++data.hole_deleted;

        bool child_needs_more_perimeters = false;
        const std::vector<PerimeterNodeView> children = parent.children_snapshot();
        for (const PerimeterNodeView &child : children)
            child_needs_more_perimeters |= child.needs_more_perimeters();

        if (parent.is_last_perimeter() && !child_needs_more_perimeters) {
            if (parent.perimeter_needed() > 0)
                parent.set_perimeter_needed(parent.perimeter_needed() - 1);
        } else {
            for (const PerimeterNodeView &child : children) {
                set_if_child_needs_one_less_perimeter(child);
                state->set(child.handle(), data);
            }
        }

        state->set(node, data);
        return;
    }

    // Only one class is past its limit. Keep the other class in this node, then
    // rebuild child/fill areas from the physical footprint of the kept loops.
    // Without this rebuild, child nodes would still be based on the pre-edit
    // generator output and may overlap or miss material.
    const size_t erased_count = erase_perimeter_class(extrusions, decision.holes);
    if (erased_count == 0) {
        state->set(node, data);
        return;
    }

    if (decision.contours)
        ++data.contour_deleted;
    if (decision.holes)
        ++data.hole_deleted;

    const c_flow flow = external_perimeter_flow(context_view);
    const double cleanup_distance = cleanup_distance_from_flow(flow);
    StoredExPolygonCollection kept_extrusion_area =
        extrusion_coverage_area(context_view.storage(), extrusions.readonly(), 0.5 * double(flow.spacing), cleanup_distance);
    StoredExPolygonCollection kept_extrusion_fill_area =
        extrusion_coverage_area(context_view.storage(), extrusions.readonly(), 0.25 * double(flow.spacing), cleanup_distance);
    StoredExPolygonCollection child_areas =
        build_child_areas(context_view.storage(), parent, kept_extrusion_area, cleanup_distance);
    StoredExPolygonCollection fill_areas =
        build_fill_areas(context_view.storage(), parent, kept_extrusion_fill_area, cleanup_distance);

    if (context_view.rebuild_children(parent, child_areas, fill_areas))
        store_for_children(*state, parent, data);

    state->set(node, data);
}

void module_end(void *, void *user_context, perimeter_generation_context *)
{
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

SeparateHoleContour &
SeparateHoleContour::instance(orchestrator_handle *orch)
{
    static SeparateHoleContour s_instance(orch);
    return s_instance;
}

const char *SeparateHoleContour::id_impl() const noexcept
{
    return k_separate_hole_contour_id;
}

slicing_step_t SeparateHoleContour::step_impl() const noexcept
{
    return PERIMETER_GENERATION_MODULE;
}

const char *const *SeparateHoleContour::dependencies_impl() const noexcept
{
    return k_no_dependencies;
}

int32_t SeparateHoleContour::priority_impl() const noexcept
{
    return 6;
}

int32_t SeparateHoleContour::used_config_keys(raw_used_config_key *keys) const noexcept
{
    if (keys != nullptr)
        for (uint32_t idx = 0; idx < sizeof(k_used_config_keys) / sizeof(k_used_config_keys[0]); ++idx)
            keys[idx] = k_used_config_keys[idx];
    return int32_t(sizeof(k_used_config_keys) / sizeof(k_used_config_keys[0]));
}

const char *SeparateHoleContour::progress_message_format_impl() const noexcept
{
    return "Separate hole/contour perimeters: %u / %u";
}

void SeparateHoleContour::run_impl(const plugin_run_context *run_ctx) const
{
    run_ctx_perimeter_generation_module *ctx = plugin_ctx_as_perimeter_generation_module(run_ctx);
    if (ctx == nullptr)
        return;

    ctx->module.ctx = const_cast<SeparateHoleContour *>(this);
    ctx->module.vt = &module_vtable();
}

void register_separate_hole_contour_plugin(orchestrator_handle *orch)
{
    orchestrator_register_plugin(orch, SeparateHoleContour::instance(orch).c_instance());
}

}}} // namespace slic3r_api::Perimeter::SeparateHoleContourPlugin

#ifdef SEPARATE_HOLE_CONTOUR_PLUGIN_DLL
extern "C" void register_plugin(orchestrator_handle *orch)
{
    slic3r_api::Perimeter::SeparateHoleContourPlugin::register_separate_hole_contour_plugin(orch);
}
#endif // SEPARATE_HOLE_CONTOUR_PLUGIN_DLL
