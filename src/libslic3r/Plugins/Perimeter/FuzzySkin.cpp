///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "FuzzySkin.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <utility>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_post_perimeter.h"
#include "libslic3r/Api/plugin/cpp/ClipperViews.hpp"
#include "libslic3r/Api/plugin/cpp/DataTreeViews.hpp"
#include "libslic3r/Api/plugin/cpp/ExtrusionViews.hpp"
#include "libslic3r/Api/plugin/cpp/RegionSettingsViews.hpp"
#include "libslic3r/Api/plugin/cpp/VolumeViews.hpp"

namespace slic3r_api { namespace Perimeter { namespace FuzzySkinPlugin {

namespace {

const char *k_fuzzy_skin_id = "perimeter.post_process.fuzzy_skin";
const char *k_fuzzy_skin_painting_key = "perimeter.post_process.fuzzy_skin.painting";
const char *k_no_dependencies[] = { nullptr };
const raw_used_config_key k_used_config_keys[] = {
    { "fuzzy_skin", RAW_CO_ENUM, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "fuzzy_skin_thickness", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "fuzzy_skin_point_dist", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE }
};
const char *k_fuzzy_skin_key = "fuzzy_skin";
const char *k_fuzzy_skin_thickness_key = "fuzzy_skin_thickness";
const char *k_fuzzy_skin_point_dist_key = "fuzzy_skin_point_dist";
const char *k_fuzzy_skin_icon_svg =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
    "<path d=\"M4 16c2.2-2.7 3.9-2.7 6.1 0s3.9 2.7 6.1 0S20 13.3 22 16\" "
    "fill=\"none\" stroke=\"#222\" stroke-width=\"2\" stroke-linecap=\"round\"/>"
    "<path d=\"M3 11c2.1-2.4 3.8-2.4 5.8 0s3.7 2.4 5.8 0 3.7-2.4 5.4-.3\" "
    "fill=\"none\" stroke=\"#f6c02d\" stroke-width=\"2\" stroke-linecap=\"round\"/>"
    "</svg>";

// These values match the public FFF fuzzy_skin enum option order. Keeping them
// local avoids including host print-config classes from this plugin algorithm.
constexpr int32_t k_fuzzy_none     = 0;
constexpr int32_t k_fuzzy_external = 1;
constexpr int32_t k_fuzzy_shell    = 2;
constexpr int32_t k_fuzzy_all      = 3;

constexpr uint16_t k_loop_role_hole    = 1u << 3;

struct FuzzyParameters
{
    int32_t mode = k_fuzzy_none;
    coordf_t thickness = 0.;
    coordf_t point_distance = 0.;

    bool can_fuzz_perimeters() const
    {
        return mode != k_fuzzy_none && thickness > 0. && point_distance > 0.;
    }

    bool can_fuzz_gap_fill() const
    {
        return mode == k_fuzzy_all && thickness > 0. && point_distance > 0.;
    }
};

struct InheritedExtrusionState
{
    bool has_perimeter = false;
    EPropertyPerimeter perimeter = {};
};

struct FuzzyTarget
{
    MutableExtrusionEntity entity;
    FuzzyParameters params;
};

struct FuzzyClip
{
    FuzzyClip(StoredExPolygonCollection &&area_in, const FuzzyParameters &params_in) :
        area(std::move(area_in)), params(params_in)
    {}

    StoredExPolygonCollection area;
    FuzzyParameters params;
};

struct FuzzyPaintingClip
{
    explicit FuzzyPaintingClip(storage_handle *storage) : enforcers(storage), blockers(storage) {}

    bool has_enforcers() const { return !enforcers.empty(); }
    bool has_blockers() const { return !blockers.empty(); }
    bool has_any() const { return has_enforcers() || has_blockers(); }

    StoredExPolygonCollection enforcers;
    StoredExPolygonCollection blockers;
};

struct SplitFragment
{
    SplitFragment(storage_handle *storage,
                  const ExtrusionEntity &source,
                  const Polyline &polyline,
                  const FuzzyParameters &params_in,
                  double order_in,
                  bool fuzzify_in) :
        entity(storage, source),
        params(params_in),
        order(order_in),
        fuzzify(fuzzify_in)
    {
        entity.set(polyline);
    }

    StoredExtrusionEntity entity;
    FuzzyParameters params;
    double order = 0.;
    bool fuzzify = false;
};

FuzzyParameters fuzzy_parameters_from_value(const RegionSettingsValue &value, double nozzle_diameter)
{
    FuzzyParameters out;
    out.mode = value.get_int(k_fuzzy_skin_key);
    out.thickness = scale_d(value.get_effective_value(nozzle_diameter, k_fuzzy_skin_thickness_key));
    out.point_distance = scale_d(value.get_effective_value(nozzle_diameter, k_fuzzy_skin_point_dist_key));
    return out;
}

double nozzle_diameter_for_fuzzy_skin(const LayerIsland &island)
{
    if (island.region_count() == 0)
        return 0.;
    const c_flow flow = island.region(0).flow(RAW_EXTRUSION_ROLE_EXTERNAL_PERIMETER);
    return unscaled(flow.nozzle_diameter);
}

InheritedExtrusionState state_with_entity_properties(const InheritedExtrusionState &parent_state,
                                                     const ExtrusionEntity &entity)
{
    // Extrusion properties may be stored on an ancestor collection and inherited
    // by leaf paths. Carry the current perimeter metadata down the tree so a
    // split child is still classified as external/shell/hole correctly.
    InheritedExtrusionState state = parent_state;
    const EPropertyPerimeter *perimeter = entity.property<EPropertyPerimeter>();
    if (perimeter != nullptr) {
        state.has_perimeter = true;
        state.perimeter = *perimeter;
    }

    return state;
}

bool perimeter_state_is_hole(const InheritedExtrusionState &state)
{
    return state.has_perimeter && (state.perimeter.perimeter_role() & k_loop_role_hole) != 0;
}

bool perimeter_state_is_shell(const InheritedExtrusionState &state)
{
    return state.has_perimeter && state.perimeter.shell_count() == 0;
}

bool should_fuzzify_perimeter(const FuzzyParameters &params, const InheritedExtrusionState &state)
{
    if (!params.can_fuzz_perimeters())
        return false;
    if (params.mode == k_fuzzy_all)
        return true;
    if (!perimeter_state_is_shell(state))
        return false;
    if (params.mode == k_fuzzy_shell)
        return true;
    if (params.mode == k_fuzzy_external)
        return !perimeter_state_is_hole(state);
    return false;
}

bool should_fuzzify_for_role(raw_extrusion_role role,
                             const FuzzyParameters &params,
                             const InheritedExtrusionState &state)
{
    if (role == RAW_EXTRUSION_ROLE_GAP_FILL)
        return params.can_fuzz_gap_fill();
    return should_fuzzify_perimeter(params, state);
}

FuzzyParameters parameters_for_painting_enforcer(FuzzyParameters params);

bool any_area_can_fuzzify_role(const RegionSettings::AreaMap &areas,
                               raw_extrusion_role role,
                               double nozzle_diameter,
                               const FuzzyPaintingClip &painting)
{
    for (const auto &[setting_value, setting_clip] : areas) {
        const FuzzyParameters params = fuzzy_parameters_from_value(setting_value, nozzle_diameter);
        if (role == RAW_EXTRUSION_ROLE_GAP_FILL ? params.can_fuzz_gap_fill() : params.can_fuzz_perimeters())
            return true;
        const FuzzyParameters painted_params = parameters_for_painting_enforcer(params);
        if (painting.has_enforcers() &&
            (role == RAW_EXTRUSION_ROLE_GAP_FILL ?
                painted_params.can_fuzz_gap_fill() :
                painted_params.can_fuzz_perimeters()))
            return true;
    }
    return false;
}

StoredPolyline polyline_from_points(storage_handle *storage, const std::vector<c_point> &points)
{
    StoredPolyline polyline(storage);
    if (!points.empty())
        polyline.insert_array(0, points.data(), static_cast<uint32_t>(points.size()));
    return polyline;
}

double squared_distance_to_projection(c_point point,
                                      c_point segment_start,
                                      c_point segment_end,
                                      double &projection_ratio)
{
    const double vx = double(segment_end.x) - double(segment_start.x);
    const double vy = double(segment_end.y) - double(segment_start.y);
    const double wx = double(point.x) - double(segment_start.x);
    const double wy = double(point.y) - double(segment_start.y);
    const double segment_length_sq = vx * vx + vy * vy;
    if (segment_length_sq <= 0.) {
        projection_ratio = 0.;
        const double dx = double(point.x) - double(segment_start.x);
        const double dy = double(point.y) - double(segment_start.y);
        return dx * dx + dy * dy;
    }

    projection_ratio = (wx * vx + wy * vy) / segment_length_sq;
    if (projection_ratio < 0.)
        projection_ratio = 0.;
    else if (projection_ratio > 1.)
        projection_ratio = 1.;

    const double px = double(segment_start.x) + vx * projection_ratio;
    const double py = double(segment_start.y) + vy * projection_ratio;
    const double dx = double(point.x) - px;
    const double dy = double(point.y) - py;
    return dx * dx + dy * dy;
}

double distance_along_points(const std::vector<c_point> &source, c_point point)
{
    // Clipper returns fragments in geometric order most of the time, but not as
    // a documented guarantee. Sort fragments by their first point projected on
    // the original polyline so replacement children keep the extrusion order.
    double best_distance_sq = std::numeric_limits<double>::max();
    double best_distance = 0.;
    double accumulated = 0.;

    for (size_t idx = 1; idx < source.size(); ++idx) {
        double projection_ratio = 0.;
        const double distance_sq =
            squared_distance_to_projection(point, source[idx - 1], source[idx], projection_ratio);
        const double segment_length = norm(source[idx] - source[idx - 1]);
        if (distance_sq < best_distance_sq) {
            best_distance_sq = distance_sq;
            best_distance = accumulated + projection_ratio * segment_length;
        }
        accumulated += segment_length;
    }

    return best_distance;
}

FuzzyParameters parameters_for_painting_enforcer(FuzzyParameters params)
{
    // A painted enforcer is an explicit request for fuzzy skin in the painted
    // area. When the region setting is "none", use the least invasive fuzzy
    // mode and keep thickness / point distance from that same region.
    if (params.mode == k_fuzzy_none)
        params.mode = k_fuzzy_external;
    return params;
}

void append_fuzzy_clip(std::vector<FuzzyClip> &clips,
                       StoredExPolygonCollection &&area,
                       const FuzzyParameters &params)
{
    if (area.empty())
        return;
    area.ensure_valid();
    if (!area.empty())
        clips.emplace_back(std::move(area), params);
}

StoredExPolygonCollection area_without_painting_blockers(storage_handle *storage,
                                                         const ExPolygonCollection &area,
                                                         const FuzzyPaintingClip &painting)
{
    if (!painting.has_blockers())
        return area.clone(storage);

    ClipperContext clipper(storage);
    // Expand blockers by a tiny amount so a painted edge reliably cuts a
    // perimeter fragment instead of leaving a nearly coincident fuzzy sliver.
    ClipperOperand blockers = clipper_offset(clipper(painting.blockers.readonly()),
                                             1000. * double(SCALED_EPSILON));
    return clipper_diff(clipper(area), blockers).to_expolygon_collection();
}

StoredExPolygonCollection area_inside_painting_enforcers(storage_handle *storage,
                                                         const ExPolygonCollection &area,
                                                         const FuzzyPaintingClip &painting)
{
    if (!painting.has_enforcers())
        return StoredExPolygonCollection(storage);

    ClipperContext clipper(storage);
    ClipperOperand enforced = clipper_intersection(clipper(area), clipper(painting.enforcers.readonly()));
    if (painting.has_blockers()) {
        ClipperOperand blockers = clipper_offset(clipper(painting.blockers.readonly()),
                                                 1000. * double(SCALED_EPSILON));
        enforced = clipper_diff(enforced, blockers);
    }
    return enforced.to_expolygon_collection();
}

std::vector<FuzzyClip> fuzzy_clips_for_leaf(storage_handle *storage,
                                            const ExPolygon &island_slice,
                                            const RegionSettings::AreaMap &areas,
                                            const FuzzyPaintingClip &painting,
                                            double nozzle_diameter,
                                            raw_extrusion_role role,
                                            const InheritedExtrusionState &state)
{
    // RegionSettings partitions the island by configuration. Generic facet
    // painting is an additional spatial mask layered on top:
    // - normal settings create fuzzy clips where fuzzy_skin is enabled;
    // - enforcer facets create fuzzy clips even where fuzzy_skin is none;
    // - blocker facets are subtracted from both sources.
    std::vector<FuzzyClip> clips;
    for (const auto &[setting_value, setting_clip] : areas) {
        StoredExPolygonCollection region_area = setting_clip.intersections(island_slice);
        if (region_area.empty())
            continue;

        const FuzzyParameters params = fuzzy_parameters_from_value(setting_value, nozzle_diameter);
        if (should_fuzzify_for_role(role, params, state)) {
            StoredExPolygonCollection enabled_area =
                area_without_painting_blockers(storage, region_area.readonly(), painting);
            append_fuzzy_clip(clips, std::move(enabled_area), params);
            continue;
        }

        const FuzzyParameters painted_params = parameters_for_painting_enforcer(params);
        if (should_fuzzify_for_role(role, painted_params, state)) {
            StoredExPolygonCollection painted_area =
                area_inside_painting_enforcers(storage, region_area.readonly(), painting);
            append_fuzzy_clip(clips, std::move(painted_area), painted_params);
        }
    }
    return clips;
}

StoredExPolygonCollection union_fuzzy_clips(storage_handle *storage, const std::vector<FuzzyClip> &clips)
{
    StoredExPolygonCollection accepted_area(storage);
    for (const FuzzyClip &clip_area : clips)
        accepted_area.append_copy_from(clip_area.area.readonly());

    if (!accepted_area.empty()) {
        ClipperContext clipper(storage);
        accepted_area = clipper_union(clipper(accepted_area)).to_expolygon_collection();
        accepted_area.ensure_valid();
    }
    return accepted_area;
}

void append_intersection_fragments(storage_handle *storage,
                                   const MutableExtrusionEntity &source_entity,
                                   const StoredPolyline &source_polyline,
                                   const std::vector<c_point> &source_points,
                                   const FuzzyClip &clip,
                                   std::vector<SplitFragment> &fragments)
{
    StoredPolylineCollection clipped_polylines =
        clipper_intersection_polyline_expolygons(storage, source_polyline, clip.area.readonly());
    for (const Polyline clipped_polyline : clipped_polylines) {
        if (clipped_polyline.size() < 2)
            continue;

        const double order = distance_along_points(source_points, clipped_polyline.front());
        fragments.emplace_back(storage, source_entity.readonly(), clipped_polyline, clip.params, order, true);
    }
}

void append_remainder_fragments(storage_handle *storage,
                                const MutableExtrusionEntity &source_entity,
                                const StoredPolyline &source_polyline,
                                const std::vector<c_point> &source_points,
                                const StoredExPolygonCollection &accepted_area,
                                std::vector<SplitFragment> &fragments)
{
    // The accepted area is the union of all fuzzy regions, including painted
    // enforcers. Everything outside it stays printable but keeps the original
    // smooth centerline.
    StoredPolylineCollection remainders =
        clipper_diff_polyline_expolygons(storage, source_polyline, accepted_area.readonly());

    FuzzyParameters disabled_params;
    for (const Polyline remainder : remainders) {
        if (remainder.size() < 2)
            continue;
        const double order = distance_along_points(source_points, remainder.front());
        fragments.emplace_back(storage, source_entity.readonly(), remainder, disabled_params, order, false);
    }
}

std::vector<SplitFragment> split_leaf_by_region_settings(storage_handle *storage,
                                                         MutableExtrusionEntity entity,
                                                         const ExPolygon &island_slice,
                                                         const RegionSettings::AreaMap &areas,
                                                         const FuzzyPaintingClip &painting,
                                                         double nozzle_diameter,
                                                         raw_extrusion_role role,
                                                         const InheritedExtrusionState &state)
{
    // Convert the region settings and optional painting into explicit fuzzy
    // clips, intersect the source polyline with them, keep the outside
    // remainder as smooth fragments, then sort all pieces back in path order.
    std::vector<SplitFragment> fragments;
    const std::vector<c_point> source_points = entity.points();
    if (source_points.size() < 2)
        return fragments;

    std::vector<FuzzyClip> fuzzy_clips =
        fuzzy_clips_for_leaf(storage, island_slice, areas, painting, nozzle_diameter, role, state);
    if (fuzzy_clips.empty())
        return fragments;

    StoredPolyline source_polyline = polyline_from_points(storage, source_points);
    for (const FuzzyClip &clip : fuzzy_clips)
        append_intersection_fragments(storage, entity, source_polyline, source_points, clip, fragments);

    StoredExPolygonCollection accepted_area = union_fuzzy_clips(storage, fuzzy_clips);
    append_remainder_fragments(storage, entity, source_polyline, source_points, accepted_area, fragments);

    std::sort(fragments.begin(), fragments.end(),
              [](const SplitFragment &lhs, const SplitFragment &rhs) { return lhs.order < rhs.order; });
    return fragments;
}

bool fragments_need_replacement(const std::vector<SplitFragment> &fragments)
{
    for (const SplitFragment &fragment : fragments)
        if (fragment.fuzzify)
            return true;
    return false;
}

void collect_fuzzy_targets_from_fragments(MutableExtrusionEntity owner,
                                          const std::vector<SplitFragment> &fragments,
                                          std::vector<FuzzyTarget> &targets)
{
    // New child handles are only stable after all children have been inserted.
    // Store the handles after replacement, then fuzz them in a separate pass.
    assert(owner.child_count() == fragments.size());
    for (uint32_t idx = 0; idx < owner.child_count() && idx < fragments.size(); ++idx)
        if (fragments[idx].fuzzify)
            targets.push_back({ owner.child_mutable(idx), fragments[idx].params });
}

uint32_t replace_child_with_fragments(MutableExtrusionEntity parent,
                                      uint32_t child_idx,
                                      MutableExtrusionEntity child,
                                      std::vector<SplitFragment> &fragments,
                                      std::vector<FuzzyTarget> &targets)
{
    if (fragments.empty())
        return child_idx + 1;

    if (fragments.size() == 1) {
        const bool moved = extrusion_move_from(child.mutable_handle(), fragments.front().entity.mutable_handle()) != 0;
        assert(moved);
        (void) moved;
        if (fragments.front().fuzzify)
            targets.push_back({ child, fragments.front().params });
        return child_idx + 1;
    }

    if ((parent.flags() & RAW_EXTRUSION_FLAG_SORTABLE) != 0) {
        // Sortable parents may reorder children. A split fuzzy path must stay
        // in its original sequence, so replace the leaf by a non-sortable
        // collection containing the ordered fragments.
        const bool was_reversible = (child.flags() & RAW_EXTRUSION_FLAG_REVERSIBLE) != 0;
        child.clear_content();
        child.set_flags(was_reversible ? RAW_EXTRUSION_FLAG_REVERSIBLE : 0);
        for (SplitFragment &fragment : fragments)
            child.add_child(fragment.entity.mutable_view());
        collect_fuzzy_targets_from_fragments(child, fragments, targets);
        return child_idx + 1;
    }

    // Non-sortable parents already preserve child order. In that case inserting
    // the fragments as siblings keeps the tree shallower and mirrors how a
    // continuous loop stores ordered path pieces.
    const bool removed = parent.remove_child(child_idx);
    assert(removed);
    (void) removed;
    for (uint32_t offset = 0; offset < fragments.size(); ++offset) {
        const uint32_t inserted_idx = parent.insert_child_move(child_idx + offset, fragments[offset].entity.mutable_view());
        assert(!is_invalid_index(inserted_idx));
        if (!is_invalid_index(inserted_idx) && fragments[offset].fuzzify)
            targets.push_back({ parent.child_mutable(inserted_idx), fragments[offset].params });
    }
    return child_idx + static_cast<uint32_t>(fragments.size());
}

void replace_root_leaf_with_fragments(MutableExtrusionEntity root,
                                      std::vector<SplitFragment> &fragments,
                                      std::vector<FuzzyTarget> &targets)
{
    // A root leaf has no parent where sibling fragments could be inserted. Turn
    // it into a non-sortable collection so the fragment order remains explicit.
    if (fragments.empty())
        return;
    if (fragments.size() == 1) {
        const bool moved = extrusion_move_from(root.mutable_handle(), fragments.front().entity.mutable_handle()) != 0;
        assert(moved);
        (void) moved;
        if (fragments.front().fuzzify)
            targets.push_back({ root, fragments.front().params });
        return;
    }

    const bool was_reversible = (root.flags() & RAW_EXTRUSION_FLAG_REVERSIBLE) != 0;
    root.clear_content();
    root.set_flags(was_reversible ? RAW_EXTRUSION_FLAG_REVERSIBLE : 0);
    for (SplitFragment &fragment : fragments)
        root.add_child(fragment.entity.mutable_view());
    collect_fuzzy_targets_from_fragments(root, fragments, targets);
}

void process_entity_children(storage_handle *storage,
                             MutableExtrusionEntity parent,
                             const ExPolygon &island_slice,
                             const RegionSettings::AreaMap *areas,
                             const FuzzyPaintingClip &painting,
                             const FuzzyParameters &solo_params,
                             double nozzle_diameter,
                             raw_extrusion_role role,
                             const InheritedExtrusionState &parent_state,
                             std::vector<FuzzyTarget> &targets);

void process_entity(storage_handle *storage,
                    MutableExtrusionEntity entity,
                    const ExPolygon &island_slice,
                    const RegionSettings::AreaMap *areas,
                    const FuzzyPaintingClip &painting,
                    const FuzzyParameters &solo_params,
                    double nozzle_diameter,
                    raw_extrusion_role role,
                    const InheritedExtrusionState &parent_state,
                    std::vector<FuzzyTarget> &targets)
{
    // Walk the extrusion tree directly instead of flattening it. Keeping the
    // hierarchy lets us replace a single leaf without disturbing unrelated
    // collections, loops, or continuous path groups.
    const InheritedExtrusionState state = state_with_entity_properties(parent_state, entity.readonly());

    if (entity.child_count() > 0) {
        process_entity_children(storage, entity, island_slice, areas, painting, solo_params,
                                nozzle_diameter, role, state, targets);
        return;
    }

    if (!entity.has_polyline() || entity.point_count() < 2)
        return;

    if (areas == nullptr) {
        if (should_fuzzify_for_role(role, solo_params, state))
            targets.push_back({ entity, solo_params });
        return;
    }

    std::vector<SplitFragment> fragments =
        split_leaf_by_region_settings(storage, entity, island_slice, *areas, painting, nozzle_diameter, role, state);
    if (fragments_need_replacement(fragments))
        replace_root_leaf_with_fragments(entity, fragments, targets);
}

void process_entity_children(storage_handle *storage,
                             MutableExtrusionEntity parent,
                             const ExPolygon &island_slice,
                             const RegionSettings::AreaMap *areas,
                             const FuzzyPaintingClip &painting,
                             const FuzzyParameters &solo_params,
                             double nozzle_diameter,
                             raw_extrusion_role role,
                             const InheritedExtrusionState &parent_state,
                             std::vector<FuzzyTarget> &targets)
{
    uint32_t child_idx = 0;
    while (child_idx < parent.child_count()) {
        MutableExtrusionEntity child = parent.child_mutable(child_idx);
        const InheritedExtrusionState child_state = state_with_entity_properties(parent_state, child.readonly());

        if (child.child_count() > 0) {
            process_entity_children(storage, child, island_slice, areas, painting, solo_params,
                                    nozzle_diameter, role, child_state, targets);
            ++child_idx;
            continue;
        }

        if (!child.has_polyline() || child.point_count() < 2) {
            ++child_idx;
            continue;
        }

        if (areas == nullptr) {
            if (should_fuzzify_for_role(role, solo_params, child_state))
                targets.push_back({ child, solo_params });
            ++child_idx;
            continue;
        }

        std::vector<SplitFragment> fragments =
            split_leaf_by_region_settings(storage, child, island_slice, *areas, painting,
                                          nozzle_diameter, role, child_state);
        child_idx = fragments_need_replacement(fragments) ?
            replace_child_with_fragments(parent, child_idx, child, fragments, targets) :
            child_idx + 1;
    }
}

uint32_t seed_from_points(const std::vector<c_point> &points)
{
    // The original host implementation used the global slicer RNG. This plugin
    // keeps the Cura-derived point placement algorithm, but uses a local seed
    // derived from the extrusion geometry so parallel plugin runs do not share
    // mutable random state.
    uint32_t seed = 2166136261u;
    for (const c_point point : points) {
        const uint64_t x = uint64_t(point.x);
        const uint64_t y = uint64_t(point.y);
        seed ^= uint32_t(x);
        seed *= 16777619u;
        seed ^= uint32_t(x >> 32);
        seed *= 16777619u;
        seed ^= uint32_t(y);
        seed *= 16777619u;
        seed ^= uint32_t(y >> 32);
        seed *= 16777619u;
    }
    return seed == 0 ? 1u : seed;
}

double random_between_0_and_1(std::minstd_rand &rng)
{
    return double(rng()) / double(rng.max());
}

void append_point_if_different(std::vector<c_point> &out, c_point point)
{
    if (out.empty() || !points_equal(out.back(), point))
        out.push_back(point);
}

bool points_differ(const std::vector<c_point> &lhs, const std::vector<c_point> &rhs)
{
    if (lhs.size() != rhs.size())
        return true;
    for (size_t idx = 0; idx < lhs.size(); ++idx)
        if (!points_equal(lhs[idx], rhs[idx]))
            return true;
    return false;
}

c_point fuzzy_point_between(c_point p0, c_point p1, double distance, double offset)
{
    const double dx = double(p1.x) - double(p0.x);
    const double dy = double(p1.y) - double(p0.y);
    const double length = std::sqrt(dx * dx + dy * dy);
    if (length <= 0.)
        return p0;

    const double ratio = distance / length;
    const double x = double(p0.x) + dx * ratio - dy * offset / length;
    const double y = double(p0.y) + dy * ratio + dx * offset / length;
    return c_point{ coord_t(x), coord_t(y) };
}

double points_length(const std::vector<c_point> &points)
{
    double length = 0.;
    for (size_t idx = 1; idx < points.size(); ++idx)
        length += norm(points[idx] - points[idx - 1]);
    return length;
}

double orientation_value(c_point a, c_point b, c_point c)
{
    const double ab_x = double(b.x) - double(a.x);
    const double ab_y = double(b.y) - double(a.y);
    const double ac_x = double(c.x) - double(a.x);
    const double ac_y = double(c.y) - double(a.y);
    return ab_x * ac_y - ab_y * ac_x;
}

bool opposite_strict_signs(double lhs, double rhs)
{
    return (lhs < 0. && rhs > 0.) || (lhs > 0. && rhs < 0.);
}

bool proper_segment_crossing(c_point lhs_a, c_point lhs_b, c_point rhs_a, c_point rhs_b)
{
    // Only interior/interior crossings are repaired here. Shared endpoints are
    // valid polyline joints, especially after clipping a perimeter by several
    // fuzzy regions.
    return opposite_strict_signs(orientation_value(lhs_a, lhs_b, rhs_a),
                                 orientation_value(lhs_a, lhs_b, rhs_b)) &&
           opposite_strict_signs(orientation_value(rhs_a, rhs_b, lhs_a),
                                 orientation_value(rhs_a, rhs_b, lhs_b));
}

bool adjacent_fuzzy_segments(size_t lhs, size_t rhs, bool closed, size_t segment_count)
{
    if (lhs + 1 == rhs || rhs + 1 == lhs)
        return true;
    return closed && segment_count > 1 &&
           ((lhs == 0 && rhs + 1 == segment_count) ||
            (rhs == 0 && lhs + 1 == segment_count));
}

c_point average_internal_points(const std::vector<c_point> &points, size_t first, size_t last)
{
    // The middle point keeps the repaired span inside the same local area as
    // the original back-and-forth fuzzy section. Averaging only internal points
    // avoids moving the two preserved endpoints.
    assert(first + 1 < last);
    double sum_x = 0.;
    double sum_y = 0.;
    size_t count = 0;
    for (size_t idx = first + 1; idx < last; ++idx) {
        sum_x += double(points[idx].x);
        sum_y += double(points[idx].y);
        ++count;
    }
    assert(count > 0);
    return c_point{ coord_t(std::llround(sum_x / double(count))),
                    coord_t(std::llround(sum_y / double(count))) };
}

void append_repair_point(std::vector<c_point> &points, c_point point)
{
    if (points.empty() || !points_equal(points.back(), point))
        points.push_back(point);
}

bool collapse_first_self_crossing(std::vector<c_point> &points, bool closed)
{
    // Fuzzy offsets are random enough that a local back-and-forth can sometimes
    // fold over itself. When two non-adjacent segments cross, replace the whole
    // span between them by three points: the first endpoint, the average of the
    // folded interior, and the last endpoint. This preserves the local detour
    // while removing the crossing that would make the centerline invalid.
    if (points.size() < (closed ? 5u : 4u))
        return false;

    const size_t segment_count = points.size() - 1;
    for (size_t first_segment = 0; first_segment < segment_count; ++first_segment)
        for (size_t second_segment = first_segment + 1; second_segment < segment_count; ++second_segment) {
            if (adjacent_fuzzy_segments(first_segment, second_segment, closed, segment_count))
                continue;

            if (!proper_segment_crossing(points[first_segment], points[first_segment + 1],
                                         points[second_segment], points[second_segment + 1]))
                continue;

            const size_t first = first_segment;
            const size_t last = second_segment + 1;
            const c_point start = points[first];
            const c_point middle = average_internal_points(points, first, last);
            const c_point end = points[last];

            std::vector<c_point> repaired;
            repaired.reserve(points.size() - (last - first) + 2);
            for (size_t idx = 0; idx < first; ++idx)
                append_repair_point(repaired, points[idx]);
            append_repair_point(repaired, start);
            append_repair_point(repaired, middle);
            append_repair_point(repaired, end);
            for (size_t idx = last + 1; idx < points.size(); ++idx)
                append_repair_point(repaired, points[idx]);

            if (closed && !repaired.empty() && !points_equal(repaired.back(), repaired.front()))
                append_repair_point(repaired, repaired.front());

            points = std::move(repaired);
            return true;
        }

    return false;
}

std::vector<c_point> remove_fuzzy_self_crossings(std::vector<c_point> points, bool closed)
{
    // Collapse one crossing at a time. Each repair removes at least one point,
    // so the bounded loop prevents an accidental infinite repair cycle if a
    // future generator creates a degenerate path.
    const size_t max_repairs = points.size();
    size_t repair_count = 0;
    while (repair_count < max_repairs && collapse_first_self_crossing(points, closed))
        ++repair_count;
    return points;
}

void append_fuzzy_segment_points(c_point p0,
                                 c_point p1,
                                 coordf_t min_dist_between_points,
                                 coordf_t range_random_point_dist,
                                 coordf_t thickness,
                                 double &dist_left_over,
                                 int &offset_side,
                                 std::minstd_rand &rng,
                                 std::vector<c_point> &out)
{
    const double segment_length = norm(p1 - p0);
    if (segment_length <= 0.)
        return;

    double last_inserted_distance = dist_left_over + segment_length * 2.;
    for (double distance = dist_left_over; distance < segment_length;
         distance += min_dist_between_points + random_between_0_and_1(rng) * range_random_point_dist) {
        // Alternate the side of the inserted points. This keeps the fuzzy line
        // as short strokes crossing the original centerline instead of letting
        // several random offsets drift along the same side of the perimeter.
        const double offset = random_between_0_and_1(rng) * thickness * double(offset_side);
        append_point_if_different(out, fuzzy_point_between(p0, p1, distance, offset));
        offset_side = -offset_side;
        last_inserted_distance = distance;
    }
    dist_left_over = segment_length - last_inserted_distance;
}

std::vector<c_point> fuzzy_open_points(const std::vector<c_point> &points, const FuzzyParameters &params)
{
    // Open paths keep their original endpoints. Only intermediate randomized
    // points are inserted, otherwise travel planning may see a changed start or
    // end position for the extrusion.
    if (points.size() < 2)
        return points;

    const coordf_t min_dist_between_points = params.point_distance * 3. / 4.;
    const coordf_t range_random_point_dist = params.point_distance / 2.;
    if (min_dist_between_points <= SCALED_EPSILON || points_length(points) < min_dist_between_points * 3.)
        return points;

    std::minstd_rand rng(seed_from_points(points));
    double dist_left_over = random_between_0_and_1(rng) * (min_dist_between_points / 2.);
    int offset_side = random_between_0_and_1(rng) < 0.5 ? -1 : 1;
    std::vector<c_point> out;
    out.reserve(points.size());
    append_point_if_different(out, points.front());

    for (size_t idx = 1; idx < points.size(); ++idx)
        append_fuzzy_segment_points(points[idx - 1], points[idx], min_dist_between_points,
                                    range_random_point_dist, params.thickness, dist_left_over,
                                    offset_side, rng, out);

    append_point_if_different(out, points.back());
    if (out.size() < 2)
        return points;
    return remove_fuzzy_self_crossings(std::move(out), false);
}

std::vector<c_point> fuzzy_closed_points(const std::vector<c_point> &points, const FuzzyParameters &params)
{
    // Closed paths are processed as a ring, then explicitly closed again. This
    // keeps loop semantics intact even when the first randomized point moves.
    if (points.size() < 4)
        return points;

    std::vector<c_point> ring(points.begin(), points.end() - 1);
    const coordf_t min_dist_between_points = params.point_distance * 3. / 4.;
    const coordf_t range_random_point_dist = params.point_distance / 2.;
    if (min_dist_between_points <= SCALED_EPSILON || points_length(points) < min_dist_between_points * 3.)
        return points;

    std::minstd_rand rng(seed_from_points(points));
    double dist_left_over = random_between_0_and_1(rng) * (min_dist_between_points / 2.);
    int offset_side = random_between_0_and_1(rng) < 0.5 ? -1 : 1;
    std::vector<c_point> out;
    out.reserve(points.size());
    append_point_if_different(out, ring.back());

    c_point previous = ring.back();
    for (const c_point point : ring) {
        append_fuzzy_segment_points(previous, point, min_dist_between_points, range_random_point_dist,
                                    params.thickness, dist_left_over, offset_side, rng, out);
        previous = point;
    }

    if (out.size() < 3)
        return points;
    append_point_if_different(out, out.front());
    return remove_fuzzy_self_crossings(std::move(out), true);
}

void apply_fuzzy_skin_to_entity(const FuzzyTarget &target)
{
    // This is the plugin-side adaptation of the fuzzy_paths(), fuzzy_polygon()
    // and fuzzy_extrusion_line() algorithms that historically lived in
    // PerimeterGenerator.cpp. The point-placement idea comes from Cura: insert
    // points at randomized distances, offset them along the segment normal,
    // and keep the path endpoints/closure valid.
    MutableExtrusionEntity entity = target.entity;
    const std::vector<c_point> points = entity.points();
    if (points.size() < 2)
        return;

    const std::vector<c_point> fuzzy_points = entity.local_is_closed() ?
        fuzzy_closed_points(points, target.params) :
        fuzzy_open_points(points, target.params);
    if (fuzzy_points.size() >= 2 && points_differ(fuzzy_points, points))
        entity.set_points(fuzzy_points);
}

bool object_has_fuzzy_skin_painting(const Object &object)
{
    for (uint32_t volume_idx = 0; volume_idx < object.volume_count(); ++volume_idx)
        if (object.volume(volume_idx).has_painting(k_fuzzy_skin_painting_key))
            return true;
    return false;
}

bool has_layer_painting(const std::vector<StoredPolygonCollection> *by_layer, uint32_t layer_idx)
{
    return by_layer != nullptr && layer_idx < by_layer->size() && !(*by_layer)[layer_idx].empty();
}

StoredExPolygonCollection painting_polygons_for_island(storage_handle *storage,
                                                       const LayerIsland &island,
                                                       const std::vector<StoredPolygonCollection> *by_layer,
                                                       uint32_t layer_idx,
                                                       coordf_t painting_margin)
{
    StoredExPolygonCollection out(storage);
    if (!has_layer_painting(by_layer, layer_idx))
        return out;

    ClipperContext clipper(storage);
    ClipperOperand painted = clipper((*by_layer)[layer_idx].readonly());
    if (painting_margin > 0.)
        painted = clipper_offset(painted, painting_margin);
    ClipperOperand clipped = clipper_intersection(clipper(island.slice()), painted);
    if (!clipped.empty()) {
        out = clipper_union(clipped).to_expolygon_collection();
        out.ensure_valid();
    }
    return out;
}

FuzzyPaintingClip painting_clip_for_island(storage_handle *storage,
                                           const LayerIsland &island,
                                           const std::vector<StoredPolygonCollection> *enforcers,
                                           const std::vector<StoredPolygonCollection> *blockers,
                                           uint32_t layer_idx,
                                           coordf_t painting_margin)
{
    // Facets are projected per layer for the whole object. Clip them to the
    // current island before mixing them with RegionSettings so nearby islands
    // do not accidentally influence each other. The projected facets represent
    // the model skin, while the perimeter centerline is inset from that skin;
    // expanding by about one nozzle diameter lets a painted skin patch select
    // the extrusion centerline that belongs to it.
    FuzzyPaintingClip painting(storage);
    painting.enforcers = painting_polygons_for_island(storage, island, enforcers, layer_idx, painting_margin);
    painting.blockers = painting_polygons_for_island(storage, island, blockers, layer_idx, painting_margin);
    return painting;
}

void process_region_island_role(const run_ctx_post_perimeter_generation &ctx,
                                storage_handle *storage,
                                const LayerRegionIsland &region_island,
                                const ExPolygon &island_slice,
                                const RegionSettings &settings,
                                const FuzzyPaintingClip &painting,
                                double nozzle_diameter,
                                raw_extrusion_role role,
                                std::vector<FuzzyTarget> &targets)
{
    // Each role bucket has its own extrusion root. Perimeters and gap fill use
    // the same splitter, but fuzzy skin settings only allow gap fill in "all"
    // mode to match the historical behavior.
    extrusion_entity_handle *root_handle = ctx.get_region_island_mutable_extrusion(region_island.handle(), role);
    if (root_handle == nullptr)
        return;

    const RegionSettings::AreaMap &areas = settings.get_areas(k_fuzzy_skin_key);
    if (!any_area_can_fuzzify_role(areas, role, nozzle_diameter, painting))
        return;

    MutableExtrusionEntity root(root_handle);
    const InheritedExtrusionState empty_state;
    if (settings.has_many_config(k_fuzzy_skin_key) || painting.has_any()) {
        process_entity(storage, root, island_slice, &areas, painting, FuzzyParameters{},
                       nozzle_diameter, role, empty_state, targets);
        return;
    }

    const FuzzyParameters solo_params =
        fuzzy_parameters_from_value(settings.get_solo_config(k_fuzzy_skin_key), nozzle_diameter);
    if (role == RAW_EXTRUSION_ROLE_GAP_FILL ? !solo_params.can_fuzz_gap_fill() : !solo_params.can_fuzz_perimeters())
        return;

    process_entity(storage, root, island_slice, nullptr, painting, solo_params,
                   nozzle_diameter, role, empty_state, targets);
}

void process_island(const run_ctx_post_perimeter_generation &ctx,
                    storage_handle *storage,
                    const LayerIsland &island,
                    const std::vector<StoredPolygonCollection> *painted_enforcers,
                    const std::vector<StoredPolygonCollection> *painted_blockers,
                    uint32_t layer_idx)
{
    // Build the region-setting map once per island. Every region island inside
    // that layer island can then reuse the same spatial partitioning.
    if (island.region_count() == 0 || island.region_island_count() == 0)
        return;

    RegionSettings settings(storage, island, {{k_fuzzy_skin_key, k_fuzzy_skin_thickness_key, k_fuzzy_skin_point_dist_key}});
    settings.segregate(island.slice());
    const double nozzle_diameter = nozzle_diameter_for_fuzzy_skin(island);
    FuzzyPaintingClip painting =
        painting_clip_for_island(storage, island, painted_enforcers, painted_blockers,
                                 layer_idx, scale_d(nozzle_diameter));
    std::vector<FuzzyTarget> targets;

    for (uint32_t region_island_idx = 0; region_island_idx < island.region_island_count(); ++region_island_idx) {
        const LayerRegionIsland region_island = island.region_island(region_island_idx);
        process_region_island_role(ctx, storage, region_island, island.slice(), settings, painting, nozzle_diameter,
                                   RAW_EXTRUSION_ROLE_PERIMETER, targets);
        process_region_island_role(ctx, storage, region_island, island.slice(), settings, painting, nozzle_diameter,
                                   RAW_EXTRUSION_ROLE_GAP_FILL, targets);
    }

    // Split first, fuzz afterwards. This avoids invalidating tree positions
    // while the splitter is still walking the children of a parent entity.
    for (const FuzzyTarget &target : targets)
        apply_fuzzy_skin_to_entity(target);
}

} // namespace

FuzzySkin &FuzzySkin::instance(orchestrator_handle *orch)
{
    static FuzzySkin s_instance(orch);
    return s_instance;
}

const char *FuzzySkin::id_impl() const noexcept
{
    return k_fuzzy_skin_id;
}

const char *FuzzySkin::name_impl() const noexcept
{
    return "Fuzzy skin";
}

const char *FuzzySkin::description_impl() const noexcept
{
    return "Perturbs perimeter geometry in regions where fuzzy skin is enabled.";
}

slicing_step_t FuzzySkin::step_impl() const noexcept
{
    return STEP_POST_PERIMETER;
}

const char *const *FuzzySkin::dependencies_impl() const noexcept
{
    return k_no_dependencies;
}

int32_t FuzzySkin::priority_impl() const noexcept
{
    return 0;
}

int32_t FuzzySkin::used_config_keys(raw_used_config_key *keys) const noexcept
{
    if (keys != nullptr)
        for (uint32_t idx = 0; idx < sizeof(k_used_config_keys) / sizeof(k_used_config_keys[0]); ++idx)
            keys[idx] = k_used_config_keys[idx];
    return int32_t(sizeof(k_used_config_keys) / sizeof(k_used_config_keys[0]));
}

const char *FuzzySkin::progress_message_format_impl() const noexcept
{
    return "Fuzzy skin: %u / %u layers";
}

void FuzzySkin::inilialize_impl(storage_handle *) const
{
    raw_generic_facets_annotation_def def = {};
    def.key = k_fuzzy_skin_painting_key;
    def.label = "Fuzzy skin painting";
    def.enforce_label = "Enforce fuzzy skin";
    def.block_label = "Block fuzzy skin";
    def.icon_svg = k_fuzzy_skin_icon_svg;
    orchestrator_register_generic_facets_annotation(m_orchestrator, &def);
}

void FuzzySkin::setup_run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_post_perimeter_generation *ctx = plugin_ctx_as_post_perimeter_generation(run_ctx);
    if (ctx != nullptr && ctx->object != nullptr)
        progress().add_max(Object(ctx->object).layer_count());
}

void FuzzySkin::run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_post_perimeter_generation *ctx = plugin_ctx_as_post_perimeter_generation(run_ctx);
    if (ctx == nullptr || ctx->object == nullptr || run_ctx == nullptr || run_ctx->plugin_storage == nullptr)
        return;

    throw_if_cancelled(run_ctx);

    const Object object(ctx->object);
    const bool has_painting = object_has_fuzzy_skin_painting(object);
    std::vector<StoredPolygonCollection> painted_enforcers;
    std::vector<StoredPolygonCollection> painted_blockers;
    if (has_painting) {
        painted_enforcers = project_painting_to_polygons(
            run_ctx->plugin_storage, object, k_fuzzy_skin_painting_key, RAW_FACET_PAINTING_ENFORCER);
        painted_blockers = project_painting_to_polygons(
            run_ctx->plugin_storage, object, k_fuzzy_skin_painting_key, RAW_FACET_PAINTING_BLOCKER);
    }

    for (uint32_t layer_idx = 0; layer_idx < object.layer_count(); ++layer_idx) {
        const Layer layer = object.layer(layer_idx);
        if (layer_idx > 0) {
            for (uint32_t island_idx = 0; island_idx < layer.island_count(); ++island_idx)
                process_island(*ctx, run_ctx->plugin_storage, layer.island(island_idx),
                               has_painting ? &painted_enforcers : nullptr,
                               has_painting ? &painted_blockers : nullptr,
                               layer_idx);
        }
        progress().increment();
    }
}

void register_fuzzy_skin_plugin(orchestrator_handle *orch)
{
    orchestrator_register_plugin(orch, FuzzySkin::instance(orch).c_instance());
}

}}} // namespace slic3r_api::Perimeter::FuzzySkinPlugin

#ifdef FUZZY_SKIN_PLUGIN_DLL
extern "C" void register_plugin(orchestrator_handle *orch)
{
    slic3r_api::Perimeter::FuzzySkinPlugin::register_fuzzy_skin_plugin(orch);
}
#endif // FUZZY_SKIN_PLUGIN_DLL
