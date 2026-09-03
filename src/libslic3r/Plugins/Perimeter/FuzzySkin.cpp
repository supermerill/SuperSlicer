///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "FuzzySkin.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
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

/*
Fuzzy-skin perimeter post-processing
====================================

This plugin provides the STEP_POST_PERIMETER pass that perturbs selected
perimeter paths to create a textured outer surface. It processes perimeter and
gap-fill roots separately. RegionSettings determines the mode, thickness, and
point spacing; painted enforcer and blocker areas may further enable or prevent
the effect on selected parts of an island.

The plugin uses two phases. First, process_island() builds the region and
painting partitions, visits every relevant extrusion tree, and collects target
leaves. process_entity() carries perimeter properties inherited from ancestor
nodes so a leaf can be classified correctly even when its metadata is stored
on a wrapper. It may split a leaf by regional or painted areas, but does not
fuzz the paths during that traversal because changing the tree would invalidate
the visitor's positions.

Second, fuzzy_paths() modifies each collected target. Closed paths are treated
as rings and reopened explicitly after randomized point placement. Open paths
keep their original first and last points so travel planning sees the same
endpoints. A deterministic seed based on the source points makes the texture
repeatable, and self-crossing or tiny results are repaired before the points
are written back.

The normal call flow is:

    FuzzySkin::run_impl()
    `-- walk object layers and layer islands
        `-- process_island()
            |-- segregate fuzzy-skin settings by region
            |-- build painted enforcer/blocker clips
            `-- process_region_island_role() for perimeter and gap fill
                |-- check whether the mode applies to this role and area
                `-- process_entity()
                    |-- carry inherited perimeter properties
                    |-- split leaves at regional/painting boundaries
                    `-- collect FuzzyTarget entries
            `-- fuzzy_paths() for every collected target
                |-- fuzzy_polygon() for closed paths
                |-- fuzzy_extrusion_line() for open paths
                |-- remove_fuzzy_self_crossings()
                `-- write changed point lists back to the targets

Gap fill is fuzzified only in the all-surfaces mode, matching the role-specific
policy in the implementation. Unsupported modes, zero dimensions, short paths,
and blocked painted areas remain unchanged.
*/

namespace slic3r_api { namespace Perimeter { namespace FuzzySkinPlugin {

namespace {

// --------------------------------------------------------------------------
// Constants and small data carriers
// --------------------------------------------------------------------------

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

constexpr uint16_t k_perimeter_flag_hole = uint16_t(C_EXTRUSION_PERIMETER_FLAG_HOLE);

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

    bool operator==(const FuzzyParameters &other) const
    {
        return mode == other.mode &&
               thickness == other.thickness &&
               point_distance == other.point_distance;
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

struct FuzzyPartition
{
    std::vector<StoredExPolygonCollection> areas;
    std::vector<FuzzyParameters> params;
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

// --------------------------------------------------------------------------
// Settings interpretation
// --------------------------------------------------------------------------

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
    const EPropertyPerimeter *perimeter = entity.get(EPropertyPerimeter::key);
    if (perimeter != nullptr) {
        state.has_perimeter = true;
        state.perimeter = *perimeter;
    }

    return state;
}

bool should_fuzzify_perimeter(const FuzzyParameters &params, const InheritedExtrusionState &state)
{
    if (!params.can_fuzz_perimeters())
        return false;
    if (params.mode == k_fuzzy_all)
        return true;
    if (!state.has_perimeter || state.perimeter.shell_count() != 0)
        return false;
    if (params.mode == k_fuzzy_shell)
        return true;
    if (params.mode == k_fuzzy_external)
        return (state.perimeter.perimeter_flags() & k_perimeter_flag_hole) == 0;
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

// --------------------------------------------------------------------------
// Region / painting selection and extrusion-tree splitting
// --------------------------------------------------------------------------

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

FuzzyParameters parameters_for_painting_enforcer(FuzzyParameters params)
{
    // A painted enforcer is an explicit request for fuzzy skin in the painted
    // area. When the region setting is "none", use the least invasive fuzzy
    // mode and keep thickness / point distance from that same region.
    if (params.mode == k_fuzzy_none)
        params.mode = k_fuzzy_external;
    return params;
}

void append_grouped_fuzzy_clip(std::vector<FuzzyClip> &clips,
                               StoredExPolygonCollection &&area,
                               const FuzzyParameters &params)
{
    if (area.empty())
        return;
    area.ensure_valid();
    if (area.empty())
        return;

    // Settings tuples are an implementation detail. Grouping by the final
    // fuzzy parameters minimizes the number of spatial areas and therefore
    // the number of fragments produced for one extrusion leaf.
    for (FuzzyClip &clip : clips) {
        if (!(clip.params == params))
            continue;
        clip.area.append_copy_from(area.readonly());
        return;
    }

    clips.emplace_back(std::move(area), params);
}

FuzzyPartition build_fuzzy_partition(storage_handle *storage,
                                     const ExPolygon &island_slice,
                                     const RegionSettings::AreaMap &areas,
                                     const FuzzyPaintingClip &painting,
                                     double nozzle_diameter,
                                     raw_extrusion_role role,
                                     const InheritedExtrusionState &state)
{
    std::vector<FuzzyClip> fuzzy_clips;

    // RegionSettings partitions the island by configuration. Painting then
    // restricts or enables fuzzy skin inside each region while preserving the
    // region's own thickness and point-distance values.
    for (const auto &[setting_value, setting_clip] : areas) {
        StoredExPolygonCollection region_area = setting_clip.intersections(island_slice);
        if (region_area.empty())
            continue;

        const FuzzyParameters params = fuzzy_parameters_from_value(setting_value, nozzle_diameter);
        if (should_fuzzify_for_role(role, params, state)) {
            if (!painting.has_blockers()) {
                append_grouped_fuzzy_clip(fuzzy_clips,
                                          region_area.readonly().clone(storage), params);
            } else {
                ClipperContext clipper(storage);
                // Expanding blockers slightly gives a painted boundary clear
                // ownership instead of leaving a coincident fuzzy sliver.
                ClipperOperand blockers = clipper_offset(clipper(painting.blockers.readonly()),
                                                         1000. * double(SCALED_EPSILON));
                append_grouped_fuzzy_clip(
                    fuzzy_clips,
                    clipper_diff(clipper(region_area.readonly()), blockers).to_expolygon_collection(),
                    params);
            }
            continue;
        }

        const FuzzyParameters painted_params = parameters_for_painting_enforcer(params);
        if (should_fuzzify_for_role(role, painted_params, state) && painting.has_enforcers()) {
            ClipperContext clipper(storage);
            ClipperOperand enforced = clipper_intersection(clipper(region_area.readonly()),
                                                           clipper(painting.enforcers.readonly()));
            if (painting.has_blockers()) {
                ClipperOperand blockers = clipper_offset(clipper(painting.blockers.readonly()),
                                                         1000. * double(SCALED_EPSILON));
                enforced = clipper_diff(enforced, blockers);
            }
            append_grouped_fuzzy_clip(fuzzy_clips,
                                      enforced.to_expolygon_collection(), painted_params);
        }
    }

    FuzzyPartition partition;
    if (fuzzy_clips.empty())
        return partition;

    StoredExPolygonCollection accepted_area(storage);
    for (FuzzyClip &clip : fuzzy_clips) {
        // Union only areas with identical final behavior. Areas with different
        // parameters remain separate members of the partition.
        ClipperContext clipper(storage);
        clip.area = clipper_union(clipper(clip.area.readonly())).to_expolygon_collection();
        clip.area.ensure_valid();
        accepted_area.append_copy_from(clip.area.readonly());
        partition.params.push_back(clip.params);
        partition.areas.push_back(std::move(clip.area));
    }

    ClipperContext clipper(storage);
    accepted_area = clipper_union(clipper(accepted_area.readonly())).to_expolygon_collection();
    accepted_area.ensure_valid();

    // The generic splitter requires a complete partition. Add the smooth
    // complement explicitly so every point of a perimeter or gap-fill path is
    // owned by exactly one area, including blockers and non-fuzzy settings.
    StoredExPolygonCollection island_area(storage, island_slice);
    StoredExPolygonCollection disabled_area =
        clipper_diff(clipper(island_area.readonly()), clipper(accepted_area.readonly())).to_expolygon_collection();
    disabled_area.ensure_valid();
    partition.params.push_back(FuzzyParameters{});
    partition.areas.push_back(std::move(disabled_area));
    return partition;
}

void split_leaf_and_collect_targets(storage_handle *storage,
                                    MutableExtrusionEntity entity,
                                    const ExPolygon &island_slice,
                                    const RegionSettings::AreaMap &areas,
                                    const FuzzyPaintingClip &painting,
                                    double nozzle_diameter,
                                    raw_extrusion_role role,
                                    const InheritedExtrusionState &state,
                                    std::vector<FuzzyTarget> &targets)
{
    FuzzyPartition partition = build_fuzzy_partition(storage, island_slice, areas, painting,
                                                     nozzle_diameter, role, state);
    if (partition.areas.empty())
        return;

    // Materialize borrowed area views only after the owning vector is complete,
    // then split once. The returned handles already follow source path order.
    std::vector<ExPolygonCollection> area_views;
    area_views.reserve(partition.areas.size());
    for (const StoredExPolygonCollection &area : partition.areas)
        area_views.push_back(area.readonly());

    const std::vector<ExtrusionAreaFragment> fragments = entity.split_leaf_by_areas(area_views);
    for (const ExtrusionAreaFragment &fragment : fragments) {
        assert(fragment.area_index < partition.params.size());
        if (fragment.area_index < partition.params.size() &&
            should_fuzzify_for_role(role, partition.params[fragment.area_index], state))
            targets.push_back({ fragment.entity, partition.params[fragment.area_index] });
    }
}

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
    // Walk the extrusion tree directly instead of flattening it. The generic
    // splitter mutates only the leaf it receives, so parent collections keep
    // their identity, order and loop semantics.
    const InheritedExtrusionState state = state_with_entity_properties(parent_state, entity.readonly());

    if (entity.child_count() > 0) {
        const uint32_t child_count = entity.child_count();
        for (uint32_t child_idx = 0; child_idx < child_count; ++child_idx)
            process_entity(storage, entity.child_mutable(child_idx), island_slice, areas, painting,
                           solo_params, nozzle_diameter, role, state, targets);
        return;
    }

    if (!entity.has_polyline() || entity.point_count() < 2)
        return;

    if (areas == nullptr) {
        if (should_fuzzify_for_role(role, solo_params, state))
            targets.push_back({ entity, solo_params });
        return;
    }

    split_leaf_and_collect_targets(storage, entity, island_slice, *areas, painting,
                                   nozzle_diameter, role, state, targets);
}

// --------------------------------------------------------------------------
// Fuzzy algorithm core
// --------------------------------------------------------------------------

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

std::vector<c_point> remove_fuzzy_self_crossings(std::vector<c_point> points, bool closed)
{
    // Fuzzy offsets are random enough that a local back-and-forth can sometimes
    // fold over itself. The repair keeps the first and last point of the folded
    // span, replaces the interior by its average point, then checks again. This
    // preserves the visible detour while removing invalid centerline crossings.
    const auto append_repair_point = [](std::vector<c_point> &out, c_point point) {
        if (out.empty() || !points_equal(out.back(), point))
            out.push_back(point);
    };
    const auto orientation_value = [](c_point a, c_point b, c_point c) {
        const double ab_x = double(b.x) - double(a.x);
        const double ab_y = double(b.y) - double(a.y);
        const double ac_x = double(c.x) - double(a.x);
        const double ac_y = double(c.y) - double(a.y);
        return ab_x * ac_y - ab_y * ac_x;
    };
    const auto opposite_strict_signs = [](double lhs, double rhs) {
        return (lhs < 0. && rhs > 0.) || (lhs > 0. && rhs < 0.);
    };
    const auto proper_segment_crossing = [&](c_point lhs_a, c_point lhs_b, c_point rhs_a, c_point rhs_b) {
        // Only interior/interior crossings are repaired. Shared endpoints are
        // valid polyline joints, especially after clipping by several regions.
        return opposite_strict_signs(orientation_value(lhs_a, lhs_b, rhs_a),
                                     orientation_value(lhs_a, lhs_b, rhs_b)) &&
               opposite_strict_signs(orientation_value(rhs_a, rhs_b, lhs_a),
                                     orientation_value(rhs_a, rhs_b, lhs_b));
    };
    const auto adjacent_segments = [](size_t lhs, size_t rhs, bool closed_path, size_t segment_count) {
        if (lhs + 1 == rhs || rhs + 1 == lhs)
            return true;
        return closed_path && segment_count > 1 &&
               ((lhs == 0 && rhs + 1 == segment_count) ||
                (rhs == 0 && lhs + 1 == segment_count));
    };
    const auto average_internal_points = [](const std::vector<c_point> &source, size_t first, size_t last) {
        // Averaging only internal points keeps the preserved endpoints stable;
        // those endpoints define where the repaired fuzzy detour starts/ends.
        assert(first + 1 < last);
        double sum_x = 0.;
        double sum_y = 0.;
        size_t count = 0;
        for (size_t idx = first + 1; idx < last; ++idx) {
            sum_x += double(source[idx].x);
            sum_y += double(source[idx].y);
            ++count;
        }
        assert(count > 0);
        return c_point{ coord_t(std::llround(sum_x / double(count))),
                        coord_t(std::llround(sum_y / double(count))) };
    };
    const auto collapse_first_crossing = [&](std::vector<c_point> &path) {
        if (path.size() < (closed ? 5u : 4u))
            return false;

        const size_t segment_count = path.size() - 1;
        for (size_t first_segment = 0; first_segment < segment_count; ++first_segment)
            for (size_t second_segment = first_segment + 1; second_segment < segment_count; ++second_segment) {
                if (adjacent_segments(first_segment, second_segment, closed, segment_count))
                    continue;

                if (!proper_segment_crossing(path[first_segment], path[first_segment + 1],
                                             path[second_segment], path[second_segment + 1]))
                    continue;

                const size_t first = first_segment;
                const size_t last = second_segment + 1;
                const c_point start = path[first];
                const c_point middle = average_internal_points(path, first, last);
                const c_point end = path[last];

                std::vector<c_point> repaired;
                repaired.reserve(path.size() - (last - first) + 2);
                for (size_t idx = 0; idx < first; ++idx)
                    append_repair_point(repaired, path[idx]);
                append_repair_point(repaired, start);
                append_repair_point(repaired, middle);
                append_repair_point(repaired, end);
                for (size_t idx = last + 1; idx < path.size(); ++idx)
                    append_repair_point(repaired, path[idx]);

                if (closed && !repaired.empty() && !points_equal(repaired.back(), repaired.front()))
                    append_repair_point(repaired, repaired.front());

                path = std::move(repaired);
                return true;
            }

        return false;
    };

    // Collapse one crossing at a time. Each repair removes at least one point,
    // so the bounded loop prevents an accidental infinite repair cycle if a
    // future generator creates a degenerate path.
    const size_t max_repairs = points.size();
    size_t repair_count = 0;
    while (repair_count < max_repairs && collapse_first_crossing(points))
        ++repair_count;
    return points;
}

// Keep the public entry point of the local algorithm first; these two helpers
// are declared here and implemented immediately below it.
void fuzzy_polygon(std::vector<c_point> &points, const FuzzyParameters &params);
void fuzzy_extrusion_line(std::vector<c_point> &points, const FuzzyParameters &params);

// Thanks Cura developers for this function.
void fuzzy_paths(MutableExtrusionEntity entity, const FuzzyParameters &params)
{
    // This is the compact entry point for the Cura-derived fuzzy algorithm:
    // read the current centerline, perturb it as either a closed polygon or an
    // open extrusion line, then write back only if the path really changed.
    std::vector<c_point> points = entity.points();
    if (points.size() < 2)
        return;

    const std::vector<c_point> original = points;
    // not always a loop, with arachne
    if (entity.local_is_closed())
        fuzzy_polygon(points, params);
    else
        fuzzy_extrusion_line(points, params);

    bool changed = points.size() != original.size();
    for (size_t idx = 0; !changed && idx < points.size(); ++idx)
        changed = !points_equal(points[idx], original[idx]);
    if (points.size() >= 2 && changed)
        entity.set_points(points);
}

// Thanks Cura developers for this function.
void fuzzy_polygon(std::vector<c_point> &points, const FuzzyParameters &params)
{
    // Closed paths are processed as a ring, then explicitly closed again. This
    // keeps loop semantics intact even when the first randomized point moves.
    if (points.size() < 4)
        return;

    std::vector<c_point> ring(points.begin(), points.end() - 1);
    const coordf_t min_dist_between_points = params.point_distance * 3. / 4.; // hardcoded: the point distance may vary between 3/4 and 5/4 the supplied value
    const coordf_t range_random_point_dist = params.point_distance / 2.;
    double source_length = 0.;
    for (size_t idx = 1; idx < points.size(); ++idx)
        source_length += norm(points[idx] - points[idx - 1]);
    if (min_dist_between_points <= SCALED_EPSILON || source_length < min_dist_between_points * 3.)
        return;

    std::minstd_rand rng(seed_from_points(points));
    const auto random_between_0_and_1 = [&rng]() { return double(rng()) / double(rng.max()); };
    const auto append_if_different = [](std::vector<c_point> &out, c_point point) {
        if (out.empty() || !points_equal(out.back(), point))
            out.push_back(point);
    };
    const auto fuzzy_point_between = [](c_point p0, c_point p1, double distance, double offset) {
        const double dx = double(p1.x) - double(p0.x);
        const double dy = double(p1.y) - double(p0.y);
        const double length = std::sqrt(dx * dx + dy * dy);
        if (length <= 0.)
            return p0;

        const double ratio = distance / length;
        const double x = double(p0.x) + dx * ratio - dy * offset / length;
        const double y = double(p0.y) + dy * ratio + dx * offset / length;
        return c_point{ coord_t(x), coord_t(y) };
    };
    double dist_left_over = random_between_0_and_1() * (min_dist_between_points / 2.); // the distance to be traversed on the line before making the first new point
    int offset_side = random_between_0_and_1() < 0.5 ? -1 : 1;
    std::vector<c_point> out;
    out.reserve(points.size());
    append_if_different(out, ring.back());

    c_point previous = ring.back();
    for (const c_point point : ring) {
        // 'a' is the (next) new point between previous and point.
        const double segment_length = norm(point - previous);
        if (segment_length > 0.) {
            // so that segment_length - last_inserted_distance evaulates to
            // dist_left_over - segment_length
            double last_inserted_distance = dist_left_over + segment_length * 2.;
            for (double distance = dist_left_over; distance < segment_length;
                 distance += min_dist_between_points + random_between_0_and_1() * range_random_point_dist) {
                // Alternate sides so each local fuzzy stroke crosses the original
                // centerline, which keeps the final closed loop printable.
                const double offset = random_between_0_and_1() * params.thickness * double(offset_side);
                append_if_different(out, fuzzy_point_between(previous, point, distance, offset));
                offset_side = -offset_side;
                last_inserted_distance = distance;
            }
            dist_left_over = segment_length - last_inserted_distance;
        }
        previous = point;
    }

    if (out.size() < 3)
        return;
    // loop -> last point is the same as the first
    append_if_different(out, out.front());
    points = remove_fuzzy_self_crossings(std::move(out), true);
}

// Thanks Cura developers for this function.
// supermerill: the historical Arachne::ExtrusionLine version was not used.
// This plugin variant keeps the same point-placement idea for an open polyline.
void fuzzy_extrusion_line(std::vector<c_point> &points, const FuzzyParameters &params)
{
    // Open paths keep their original endpoints. Only intermediate randomized
    // points are inserted, otherwise travel planning may see a changed start or
    // end position for the extrusion.
    if (points.size() < 2)
        return;

    const coordf_t min_dist_between_points = params.point_distance * 3. / 4.; // hardcoded: the point distance may vary between 3/4 and 5/4 the supplied value
    const coordf_t range_random_point_dist = params.point_distance / 2.;

    // check if the path length is enough for at least 3 points, or return.
    double source_length = 0.;
    for (size_t idx = 1; idx < points.size(); ++idx)
        source_length += norm(points[idx] - points[idx - 1]);
    if (min_dist_between_points <= SCALED_EPSILON || source_length < min_dist_between_points * 3.)
        return;

    std::minstd_rand rng(seed_from_points(points));
    const auto random_between_0_and_1 = [&rng]() { return double(rng()) / double(rng.max()); };
    const auto append_if_different = [](std::vector<c_point> &out, c_point point) {
        if (out.empty() || !points_equal(out.back(), point))
            out.push_back(point);
    };
    const auto fuzzy_point_between = [](c_point p0, c_point p1, double distance, double offset) {
        const double dx = double(p1.x) - double(p0.x);
        const double dy = double(p1.y) - double(p0.y);
        const double length = std::sqrt(dx * dx + dy * dy);
        if (length <= 0.)
            return p0;

        const double ratio = distance / length;
        const double x = double(p0.x) + dx * ratio - dy * offset / length;
        const double y = double(p0.y) + dy * ratio + dx * offset / length;
        return c_point{ coord_t(x), coord_t(y) };
    };
    double dist_left_over = random_between_0_and_1() * (min_dist_between_points / 2.); // the distance to be traversed on the line before making the first new point
    int offset_side = random_between_0_and_1() < 0.5 ? -1 : 1;
    std::vector<c_point> out;
    out.reserve(points.size());
    append_if_different(out, points.front());

    for (size_t idx = 1; idx < points.size(); ++idx) {
        const c_point previous = points[idx - 1];
        const c_point point = points[idx];
        // 'a' is the (next) new point between previous and point.
        const double segment_length = norm(point - previous);
        if (segment_length <= 0.)
            continue;

        // skip points too close to each other.
        // so that segment_length - last_inserted_distance evaulates to
        // dist_left_over - segment_length
        double last_inserted_distance = dist_left_over + segment_length * 2.;
        for (double distance = dist_left_over; distance < segment_length;
             distance += min_dist_between_points + random_between_0_and_1() * range_random_point_dist) {
            // Alternate sides so the fuzzy strokes cross the original line
            // instead of drifting along the same side of the perimeter.
            const double offset = random_between_0_and_1() * params.thickness * double(offset_side);
            append_if_different(out, fuzzy_point_between(previous, point, distance, offset));
            offset_side = -offset_side;
            last_inserted_distance = distance;
        }
        dist_left_over = segment_length - last_inserted_distance;
    }

    // line -> ensure you end with the same last point
    append_if_different(out, points.back());
    if (out.size() >= 2)
        points = remove_fuzzy_self_crossings(std::move(out), false);
}

// --------------------------------------------------------------------------
// Plugin run orchestration
// --------------------------------------------------------------------------

bool object_has_fuzzy_skin_painting(const Object &object)
{
    for (uint32_t volume_idx = 0; volume_idx < object.volume_count(); ++volume_idx)
        if (object.volume(volume_idx).has_painting(k_fuzzy_skin_painting_key))
            return true;
    return false;
}

StoredExPolygonCollection painting_polygons_for_island(storage_handle *storage,
                                                       const LayerIsland &island,
                                                       const std::vector<StoredPolygonCollection> *by_layer,
                                                       uint32_t layer_idx,
                                                       coordf_t painting_margin)
{
    StoredExPolygonCollection out(storage);
    if (by_layer == nullptr || layer_idx >= by_layer->size() || (*by_layer)[layer_idx].empty())
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
        fuzzy_paths(target.entity, target.params);
}

} // namespace

// --------------------------------------------------------------------------
// Plugin lifecycle
// --------------------------------------------------------------------------

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
