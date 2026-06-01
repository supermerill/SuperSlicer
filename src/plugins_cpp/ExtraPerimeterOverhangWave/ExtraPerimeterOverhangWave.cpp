///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "ExtraPerimeterOverhangWave.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "libslic3r/Api/plugin/c/steps/slic3r_step_post_perimeter.h"
#include "libslic3r/Api/plugin/c/slic3r_extrusions.h"
#include "libslic3r/Api/plugin/c/slic3r_utils.h"
#include "libslic3r/Api/plugin/cpp/DataTreeViews.hpp"
#include "libslic3r/Api/plugin/cpp/ExtrusionViews.hpp"
#include "libslic3r/Api/plugin/cpp/LineDistancer.hpp"
#include "libslic3r/Api/plugin/cpp/ParallelFor.hpp"
#include "libslic3r/Api/plugin/cpp/RegionSettingsViews.hpp"

namespace slic3r_api { namespace Perimeter { namespace ExtraPerimeterOverhangWavePlugin {

namespace {

const char *k_extra_overhang_perimeters_wave_id = "perimeter.post_process.extra_perimeter_overhang_wave";
const char *k_extra_overhang_perimeters_group = "perimeter.post_process.extra_perimeters_on_overhangs";
const char *k_no_dependencies[] = { nullptr };
const raw_used_config_key k_used_config_keys[] = {
    { "extra_perimeters_on_overhangs", RAW_CO_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "infill_overlap", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "overhangs_extrusion_spacing", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "perimeters", RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "raft_layers", RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE }
};
const char *k_extra_perimeters_on_overhangs_key = "extra_perimeters_on_overhangs";
const char *k_infill_overlap_key = "infill_overlap";
const char *k_overhangs_extrusion_spacing_key = "overhangs_extrusion_spacing";
const char *k_perimeters_key = "perimeters";
const char *k_raft_layers_key = "raft_layers";

// Pipeline overview
// -----------------
// This post-perimeter plugin is intentionally organized around the same data
// ownership as the step:
//  1. process_layer()/process_island() read one LayerIsland and locate the
//     mutable perimeter root that will receive the new paths.
//  2. enabled_infill_area() clips the island infill area to regions where the
//     option is active. Disabled regions are returned to normal infill.
//  3. generate_extra_perimeters_over_overhangs_wave() creates ordered,
//     non-reversible internal-perimeter wave groups from the lower-layer
//     outline toward open air.
//  4. update_fill_areas() removes the consumed 2D footprint from the island so
//     later infill and gap-fill stages do not print the same volume twice.
//
// Geometry helpers below are kept at the level of those domain operations.
// Small one-line forwarding helpers are intentionally avoided: direct Clipper
// calls make ownership and temporary storage easier to follow in this plugin.
struct WaveFlow
{
    EPropertyAttributes attributes;
    coord_t width = 0;
    coord_t spacing = 0;
    coord_t height = 0;
    coord_t nozzle_diameter = 0;
    double mm3_per_mm = 0.;
};

struct OverhangGenerationInput
{
    explicit OverhangGenerationInput(storage_handle *storage) : lower_slices(storage) {}

    coord_t perimeter_depth = 0;
    coord_t overhang_spacing = 0;
    WaveFlow wave_flow;
    StoredExPolygonCollection lower_slices;
};

struct OverhangGenerationOutput
{
    explicit OverhangGenerationOutput(storage_handle *storage) : filled_area(storage), unfilled_area(storage) {}

    std::vector<StoredExtrusionEntity> extra_perimeters;
    StoredExPolygonCollection filled_area;
    StoredExPolygonCollection unfilled_area;
};

int32_t perimeter_count_from_config(const Config &config)
{
    return config.has(k_perimeters_key) ? std::max(0, config.get(k_perimeters_key).get_int()) : 0;
}

coord_t overhang_spacing_from_config(const Config &config, const c_flow &perimeter_flow)
{
    if (!config.has(k_overhangs_extrusion_spacing_key))
        return perimeter_flow.spacing;

    const coord_t configured =
        scale_i(config.get(k_overhangs_extrusion_spacing_key)
                    .get_effective_value(unscaled(perimeter_flow.nozzle_diameter)));
    return configured > 0 ? configured : perimeter_flow.spacing;
}

coord_t minimum_printable_split_length(const coord_t spacing)
{
    return std::max<coord_t>(SCALED_EPSILON, spacing / 10);
}

WaveFlow wave_flow_from_perimeter_flow(const c_flow &perimeter_flow)
{
    // The wave paths are regular internal perimeters. This plugin only creates
    // extra geometry and updates the fill domains; DetectOverhang later
    // decides which spans are unsupported and need overhang behavior.
    WaveFlow out = {};
    out.width = perimeter_flow.width;
    out.spacing = perimeter_flow.spacing;
    out.height = perimeter_flow.height;
    out.nozzle_diameter = perimeter_flow.nozzle_diameter;
    out.mm3_per_mm = perimeter_flow.mm3_per_mm;
    out.attributes.extrusion_role(RAW_EXTRUSION_ROLE_INTERNAL_PERIMETER)
                         .mm3_per_mm(perimeter_flow.mm3_per_mm)
                         .width(float(unscaled(perimeter_flow.width)))
                         .height(float(unscaled(perimeter_flow.height)));
    return out;
}

EPropertyAttributes gap_fill_attributes_from_wave_flow(const WaveFlow &wave_flow)
{
    // The residual gap-fill is still an overhang-side repair, but its section
    // must stay inside the same volumetric budget as the wave perimeter flow.
    // A square section with side sqrt(mm3/mm) reaches that budget while keeping
    // height == width; fallback guards degenerate configs.
    double side_mm = wave_flow.mm3_per_mm > 0. ? std::sqrt(wave_flow.mm3_per_mm) : 0.;
    if (side_mm <= 0.)
        side_mm = std::min(unscaled(wave_flow.width), unscaled(wave_flow.height));
    if (side_mm <= 0.)
        side_mm = unscaled(wave_flow.nozzle_diameter);

    const double max_mm3_per_mm = std::max(0., wave_flow.mm3_per_mm);
    const double square_mm3_per_mm = side_mm * side_mm;
    if (max_mm3_per_mm > 0. && square_mm3_per_mm > max_mm3_per_mm)
        side_mm = std::sqrt(max_mm3_per_mm);

    return EPropertyAttributes()
        .extrusion_role(RAW_EXTRUSION_ROLE_GAP_FILL)
        .mm3_per_mm(side_mm * side_mm)
        .width(float(side_mm))
        .height(float(side_mm));
}

void extend_bbox(c_bounding_box &box, c_point point, bool &initialized)
{
    if (!initialized) {
        box.min = point;
        box.max = point;
        initialized = true;
        return;
    }
    box.min.x = std::min(box.min.x, point.x);
    box.min.y = std::min(box.min.y, point.y);
    box.max.x = std::max(box.max.x, point.x);
    box.max.y = std::max(box.max.y, point.y);
}

void extend_bbox(c_bounding_box &box, const Polygon &polygon, bool &initialized)
{
    for (c_point point : polygon.points())
        extend_bbox(box, point, initialized);
}

c_bounding_box bounding_box(const ExPolygonCollection &areas)
{
    c_bounding_box box = {};
    bool initialized = false;
    for (const ExPolygon area : areas) {
        extend_bbox(box, area.contour(), initialized);
        for (const Polygon hole : area.holes())
            extend_bbox(box, hole, initialized);
    }
    return box;
}

c_bounding_box inflated(c_bounding_box box, coord_t delta)
{
    box.min.x -= delta;
    box.min.y -= delta;
    box.max.x += delta;
    box.max.y += delta;
    return box;
}

StoredExPolygonCollection collection_from_expolygon(storage_handle *storage, const ExPolygon &area)
{
    // Several C ABI calls operate on collections only. Keep this adapter local
    // so call sites make the "single ExPolygon as collection" conversion clear.
    StoredExPolygonCollection out(storage);
    out.push_back(area);
    return out;
}

StoredPolyline make_polyline(storage_handle *storage, const std::vector<c_point> &points)
{
    StoredPolyline out(storage);
    for (c_point point : points)
        out.push_back(point);
    return out;
}

StoredPolylineCollection make_polylines(storage_handle *storage,
                                        const std::vector<std::vector<c_point>> &lines,
                                        coord_t resolution = 0)
{
    StoredPolylineCollection out(storage);
    for (const std::vector<c_point> &points : lines) {
        if (points.empty())
            continue;
        StoredPolyline line = make_polyline(storage, points);
        if (resolution > 0)
            line.ensure_valid(resolution);
        if (!line.empty())
            out.push_back(line);
    }
    return out;
}

double squared_distance(c_point lhs, c_point rhs)
{
    const double dx = double(lhs.x) - double(rhs.x);
    const double dy = double(lhs.y) - double(rhs.y);
    return dx * dx + dy * dy;
}

StoredPolylineCollection reconnect_polylines(storage_handle *storage,
                                             const PolylineCollection &polylines,
                                             double limit_distance,
                                             coord_t resolution)
{
    if (polylines.empty())
        return StoredPolylineCollection(storage);

    std::vector<std::vector<c_point>> connected;
    connected.reserve(polylines.size());
    for (const Polyline polyline : polylines)
        if (!polyline.empty())
            connected.push_back(polyline.points());

    std::vector<bool> alive(connected.size(), true);
    const double limit_squared = limit_distance * limit_distance;

    // Boolean clipping often splits one conceptual wave into small adjacent
    // pieces. Reconnect only near endpoints here; longer gaps are preserved so
    // they become real travels between separate extrusion paths.
    for (size_t first_idx = 0; first_idx < connected.size(); ++first_idx) {
        if (!alive[first_idx])
            continue;
        for (size_t second_idx = first_idx + 1; second_idx < connected.size(); ++second_idx) {
            if (!alive[second_idx] || connected[first_idx].empty() || connected[second_idx].empty())
                continue;

            std::vector<c_point> &base = connected[first_idx];
            const std::vector<c_point> &next = connected[second_idx];
            if (squared_distance(base.back(), next.front()) < limit_squared) {
                base.insert(base.end(), next.begin(), next.end());
                alive[second_idx] = false;
            } else if (squared_distance(base.back(), next.back()) < limit_squared) {
                const std::vector<c_point> reversed(next.rbegin(), next.rend());
                base.insert(base.end(), reversed.begin(), reversed.end());
                alive[second_idx] = false;
            } else if (squared_distance(base.front(), next.back()) < limit_squared) {
                std::vector<c_point> merged = next;
                merged.insert(merged.end(), base.begin(), base.end());
                base = std::move(merged);
                alive[second_idx] = false;
            } else if (squared_distance(base.front(), next.front()) < limit_squared) {
                std::vector<c_point> merged(next.rbegin(), next.rend());
                merged.insert(merged.end(), base.begin(), base.end());
                base = std::move(merged);
                alive[second_idx] = false;
            }
        }
    }

    std::vector<std::vector<c_point>> result;
    result.reserve(connected.size());
    for (size_t idx = 0; idx < connected.size(); ++idx)
        if (alive[idx])
            result.push_back(std::move(connected[idx]));

    return make_polylines(storage, result, resolution);
}

StoredPolylineCollection order_wave_polylines_from_current_point(storage_handle *storage,
                                                                 const PolylineCollection &polylines,
                                                                 const c_point *current_point)
{
    if (current_point == nullptr || polylines.size() < 2)
        return polylines.clone(storage);

    std::vector<std::vector<c_point>> remaining;
    remaining.reserve(polylines.size());
    for (const Polyline polyline : polylines)
        if (!polyline.empty())
            remaining.push_back(polyline.points());

    std::vector<std::vector<c_point>> ordered;
    ordered.reserve(remaining.size());
    c_point cursor = *current_point;

    // Clipper does not return pieces in travel-friendly order. Pick the next
    // piece by closest endpoint so any connector remains as short as possible.
    while (!remaining.empty()) {
        size_t best_idx = 0;
        double best_distance = std::numeric_limits<double>::max();
        for (size_t idx = 0; idx < remaining.size(); ++idx) {
            const std::vector<c_point> &candidate = remaining[idx];
            if (candidate.empty())
                continue;
            const double candidate_distance =
                std::min(squared_distance(cursor, candidate.front()),
                         squared_distance(cursor, candidate.back()));
            if (candidate_distance < best_distance) {
                best_distance = candidate_distance;
                best_idx = idx;
            }
        }

        std::vector<c_point> selected = std::move(remaining[best_idx]);
        if (squared_distance(cursor, selected.back()) < squared_distance(cursor, selected.front()))
            std::reverse(selected.begin(), selected.end());

        cursor = selected.back();
        ordered.push_back(std::move(selected));
        remaining.erase(remaining.begin() + best_idx);
    }

    return make_polylines(storage, ordered);
}

StoredPolylineCollection intersection_polylines_expolygons(storage_handle *storage,
                                                           const PolylineCollection &polylines,
                                                           const ExPolygonCollection &clip_area)
{
    StoredPolylineCollection out(storage);
    if (polylines.empty() || clip_area.empty())
        return out;

    for (const Polyline polyline : polylines) {
        StoredPolylineCollection clipped = clipper_intersection_polyline_expolygons(storage, polyline, clip_area);
        out.append_move_from(clipped);
    }
    return out;
}

StoredExPolygonCollection lower_slices_for_island(storage_handle *storage, const LayerIsland &island)
{
    StoredExPolygonCollection lower_expolygons(storage);
    const std::vector<LayerIsland> lower_islands = island.lower_islands();
    for (const LayerIsland &lower_island : lower_islands)
        lower_expolygons.push_back(lower_island.slice());

    if (lower_islands.size() > 1) {
        ClipperContext clipper(storage);
        lower_expolygons = clipper_union(clipper(lower_expolygons)).to_expolygon_collection();
    }
    return lower_expolygons;
}

StoredExPolygonCollection enabled_infill_area(storage_handle *storage,
                                              const ExPolygonCollection &candidate,
                                              const RegionSettings &settings)
{
    // RegionSettings clips the island by the regions whose setting value is
    // active. The wave generation only receives the part of the infill domain
    // where the feature is enabled.
    if (!settings.has_many_config(k_extra_perimeters_on_overhangs_key)) {
        if (!settings.get_solo_config(k_extra_perimeters_on_overhangs_key)
                 .get_bool(k_extra_perimeters_on_overhangs_key))
            return StoredExPolygonCollection(storage);
        return candidate.clone(storage);
    }

    const RegionSettings::AreaMap &areas = settings.get_areas(k_extra_perimeters_on_overhangs_key);
    for (const std::pair<const RegionSettingsValue, RegionSettingsClip> &entry : areas) {
        if (entry.first.get_bool())
            return entry.second.intersections(candidate);
    }

    return StoredExPolygonCollection(storage);
}

void orient_extra_perimeter_from_support(MutableExtrusionEntity path, const LineDistancer &lower_layer_distancer)
{
    if (path.empty() || path.point_count() < 2 || lower_layer_distancer.empty())
        return;

    // Every generated stroke should start from the side closest to existing
    // material. Open strokes choose their direction; closed strokes rotate
    // their first point to the closest sampled vertex.
    std::vector<c_point> points = path.points();
    if (points.front().x == points.back().x && points.front().y == points.back().y) {
        size_t closest_idx = 0;
        double closest_distance = std::numeric_limits<double>::max();
        points.pop_back();
        for (size_t idx = 0; idx < points.size(); ++idx) {
            const double distance = lower_layer_distancer.distance_from_lines(points[idx], true);
            if (distance < closest_distance) {
                closest_distance = distance;
                closest_idx = idx;
            }
        }
        std::rotate(points.begin(), points.begin() + closest_idx, points.end());
        points.push_back(points.front());
        path.set_points(points);
        return;
    }

    const double first_distance = lower_layer_distancer.distance_from_lines(path.front(), true);
    const double last_distance = lower_layer_distancer.distance_from_lines(path.back(), true);
    if (last_distance < first_distance)
        path.reverse();
}

struct EndpointLinkChoice
{
    bool reverse_previous = false;
    bool reverse_next = false;
    double distance_squared = std::numeric_limits<double>::max();
};

EndpointLinkChoice closest_endpoint_link(const ExtrusionEntity &previous,
                                         const ExtrusionEntity &next)
{
    // Consecutive waves are allowed to reverse locally before linking. This
    // keeps the physical zone continuous without making the whole zone
    // reversible for the later G-code sorter.
    EndpointLinkChoice best;
    const c_point previous_first = previous.front();
    const c_point previous_last = previous.back();
    const c_point next_first = next.front();
    const c_point next_last = next.back();

    const double distance_keep_keep = squared_distance(previous_last, next_first);
    if (distance_keep_keep < best.distance_squared) {
        best.distance_squared = distance_keep_keep;
        best.reverse_previous = false;
        best.reverse_next = false;
    }

    const double distance_keep_reverse = squared_distance(previous_last, next_last);
    if (distance_keep_reverse < best.distance_squared) {
        best.distance_squared = distance_keep_reverse;
        best.reverse_previous = false;
        best.reverse_next = true;
    }

    const double distance_reverse_keep = squared_distance(previous_first, next_first);
    if (distance_reverse_keep < best.distance_squared) {
        best.distance_squared = distance_reverse_keep;
        best.reverse_previous = true;
        best.reverse_next = false;
    }

    const double distance_reverse_reverse = squared_distance(previous_first, next_last);
    if (distance_reverse_reverse < best.distance_squared) {
        best.distance_squared = distance_reverse_reverse;
        best.reverse_previous = true;
        best.reverse_next = true;
    }

    return best;
}

bool append_wave_polyline(storage_handle *storage,
                          StoredExtrusionEntity &zone_paths,
                          const Polyline &polyline,
                          const OverhangGenerationInput &input,
                          const LineDistancer &lower_layer_distancer)
{
    const coord_t min_split_length = minimum_printable_split_length(input.overhang_spacing);
    if (!polyline.is_valid() ||
        polyline.length() < std::max<double>(double(input.wave_flow.width), double(min_split_length)))
        return false;

    StoredExtrusionEntity path(storage, polyline);
    path.get_or_add_property<EPropertyAttributes>() = input.wave_flow.attributes;
    path.disable_reverse();
    if (zone_paths.child_count() > 0) {
        MutableExtrusionEntity previous = zone_paths.child_mutable(zone_paths.child_count() - 1);
        const EndpointLinkChoice link_choice = closest_endpoint_link(previous.readonly(), path.readonly());
        const double link_distance = double(input.wave_flow.width) * 2.0;
        if (link_choice.reverse_previous)
            previous.reverse();
        if (link_choice.reverse_next)
            path.reverse();

        // Keep one physical zone in wave order. Nearby consecutive waves are
        // joined into one extrusion; distant waves become separate leaves, so
        // the printer travels without breaking the zone order.
        if (link_choice.distance_squared < link_distance * link_distance) {
            std::vector<c_point> previous_points = previous.points();
            std::vector<c_point> path_points = path.points();
            if (link_choice.distance_squared < double(min_split_length) * double(min_split_length) &&
                !path_points.empty()) {
                // If the split created a sub-spacing connector, snap the next
                // wave to the previous endpoint instead of emitting a tiny
                // segment that cannot be printed reliably.
                path_points.front() = previous.back();
            }
            previous_points.insert(previous_points.end(), path_points.begin(), path_points.end());
            previous.set_points(previous_points);
            return true;
        }

        // Distant waves stay separate leaves inside the same locked zone. The
        // wrapper preserves the support-to-air order while allowing a travel
        // move between leaves that are too far apart to merge cleanly.
        const uint32_t idx = zone_paths.append_child_move(path.mutable_view());
        assert(!is_invalid_index(idx));
        (void)idx;
        return true;
    }

    orient_extra_perimeter_from_support(path.mutable_view(), lower_layer_distancer);
    const uint32_t idx = zone_paths.append_child_move(path.mutable_view());
    assert(!is_invalid_index(idx));
    (void)idx;
    return true;
}

StoredExPolygonCollection coverage_for_extra_perimeters(storage_handle *storage,
                                                        const std::vector<StoredExtrusionEntity> &extra_perimeters,
                                                        const ExPolygonCollection &clip_area,
                                                        const OverhangGenerationInput &input)
{
    StoredExPolygonCollection coverage(storage);
    ClipperContext clipper(storage);
    for (const StoredExtrusionEntity &zone : extra_perimeters) {
        for (const ExtrusionEntity path : zone.children()) {
            if (path.empty() || path.point_count() < 2)
                continue;

            StoredPolyline centerline = make_polyline(storage, path.points());
            const clipper_end_type_t end_type =
                centerline.front().x == centerline.back().x && centerline.front().y == centerline.back().y ?
                CLIPPER_END_CLOSED_LINE :
                CLIPPER_END_OPEN_SQUARE;
            // Free-fill bookkeeping uses spacing, not the physical overhang
            // width. The path may extrude a fatter bridge bead that sags below
            // the layer, but the next infill step only needs to reserve the
            // nominal 2D slot occupied by the generated centerline.
            StoredExPolygonCollection path_coverage =
                clipper_offset(clipper(centerline),
                               0.5 * double(input.overhang_spacing),
                               CLIPPER_JOIN_SQUARE,
                               0.,
                               end_type).to_expolygon_collection();
            coverage.append_move_from(path_coverage);
        }
    }

    if (coverage.empty())
        return StoredExPolygonCollection(storage);

    StoredExPolygonCollection coverage_union =
        clipper_union(clipper(coverage)).to_expolygon_collection();
    return clipper_intersection(clipper(coverage_union), clipper(clip_area)).to_expolygon_collection();
}

bool residual_area_is_too_small_for_gap_fill(storage_handle *storage,
                                             const ExPolygon &area,
                                             const OverhangGenerationInput &input)
{
    if (input.wave_flow.nozzle_diameter <= 0)
        return false;

    // Two very small leftovers are intentionally swallowed by the feature:
    //  - if the configured spacing is below nozzle/10, a distinct path is below
    //    useful extrusion resolution;
    //  - if no point in the leftover has a nozzle radius of clearance, the
    //    whole area is thinner than roughly two nozzle diameters.
    if (input.overhang_spacing < input.wave_flow.nozzle_diameter / 10)
        return true;

    ClipperContext clipper(storage);
    return clipper_offset(clipper(area), -double(input.wave_flow.nozzle_diameter)).empty();
}

c_flow medial_axis_flow_from_wave_flow(const WaveFlow &flow)
{
    c_flow out = {};
    out.width = flow.width;
    out.spacing = flow.spacing;
    out.height = flow.height;
    out.nozzle_diameter = flow.nozzle_diameter;
    out.is_bridge = 0;
    out.spacing_ratio = 1.f;
    out.mm3_per_mm = flow.mm3_per_mm;
    return out;
}

void append_residual_gap_fill_paths(storage_handle *storage,
                                    std::vector<StoredExtrusionEntity> &extra_perimeters,
                                    const ExPolygonCollection &residual_overhangs,
                                    const OverhangGenerationInput &input,
                                    const LineDistancer &lower_layer_distancer)
{
    if (residual_overhangs.empty())
        return;

    const EPropertyAttributes gap_fill_attributes = gap_fill_attributes_from_wave_flow(input.wave_flow);

    for (const ExPolygon residual : residual_overhangs) {
        if (residual_area_is_too_small_for_gap_fill(storage, residual, input))
            continue;

        std::optional<StoredExtrusionEntity> fills =
            medial_axis_gap_fill(medial_axis_flow_from_wave_flow(input.wave_flow))
                .medial_widths(coord_t(0.75 * input.wave_flow.width), coord_t(3.0 * input.overhang_spacing))
                .constant_width()
                .min_extrusion_length(input.wave_flow.nozzle_diameter / 10)
                .try_build(storage, residual);
        if (!fills.has_value())
            continue;

        StoredExtrusionEntity gap_fill_zone(storage);
        for (uint32_t idx = 0; idx < fills->child_count(); ++idx) {
            StoredExtrusionEntity path(storage, fills->child(idx));
            path.get_or_add_property<EPropertyAttributes>() = gap_fill_attributes;
            path.disable_reverse();
            orient_extra_perimeter_from_support(path.mutable_view(), lower_layer_distancer);
            const uint32_t child_idx = gap_fill_zone.append_child_move(path.mutable_view());
            assert(!is_invalid_index(child_idx));
            (void)child_idx;
        }

        if (!gap_fill_zone.empty())
            extra_perimeters.push_back(std::move(gap_fill_zone));
    }
}

OverhangGenerationOutput generate_extra_perimeters_over_overhangs_wave(storage_handle *storage,
                                                                       const ExPolygonCollection &infill_area,
                                                                       const OverhangGenerationInput &input)
{
    OverhangGenerationOutput out(storage);
    if (infill_area.empty() || input.lower_slices.empty() ||
        input.perimeter_depth <= 0 || input.overhang_spacing <= 0)
        return out;

    const c_bounding_box infill_area_bb =
        inflated(bounding_box(infill_area), SCALED_EPSILON + input.perimeter_depth);
    StoredExPolygonCollection optimized_lower_slices =
        clipper_clip_expolygons_with_subject_bbox(storage, input.lower_slices, infill_area_bb);
    if (optimized_lower_slices.empty())
        return out;

    ClipperContext clipper(storage);

    // The printable domain is the enabled infill area that is not already
    // covered by lower-layer material. Each later wave is clipped to this area.
    StoredExPolygonCollection overhangs =
        clipper_diff(clipper(infill_area), clipper(optimized_lower_slices)).to_expolygon_collection();
    if (overhangs.empty())
        return out;

    LineDistancer lower_layer_distancer(optimized_lower_slices);
    std::vector<StoredExtrusionEntity> extra_perimeters;

    // A zone is printed from the support boundary outward. Multiple zones are
    // independent and may later be sorted as whole units by the wrapper entity.
    StoredExPolygonCollection zones = clipper_union(clipper(overhangs)).to_expolygon_collection();
    for (const ExPolygon zone : zones) {
        StoredExtrusionEntity zone_paths(storage);
        bool has_unsupported_wave = false;
        StoredExPolygonCollection zone_clip = collection_from_expolygon(storage, zone);
        const c_bounding_box zone_bbox = bounding_box(zone_clip);
        const coord_t max_wave_distance =
            std::max(zone_bbox.max.x - zone_bbox.min.x,
                     zone_bbox.max.y - zone_bbox.min.y) +
            input.overhang_spacing;

        // Add one supported line before crossing into the unsupported area.
        // This line consumes only a narrow supported band and returns the rest
        // of the supported fill domain to the regular infill generator.
        StoredExPolygonCollection supported_lower =
            clipper_intersection(clipper(infill_area), clipper(optimized_lower_slices)).to_expolygon_collection();
        StoredExPolygonCollection zone_margin =
            clipper_offset(clipper(zone_clip), double(input.overhang_spacing)).to_expolygon_collection();
        StoredExPolygonCollection supported_band =
            clipper_intersection(clipper(supported_lower), clipper(zone_margin)).to_expolygon_collection();
        if (!supported_band.empty()) {
            StoredExPolygonCollection support_wave_area =
                clipper_offset(clipper(optimized_lower_slices), -0.5 * double(input.overhang_spacing)).to_expolygon_collection();
            StoredPolylineCollection support_area_lines =
                expolygons_to_polylines(storage, support_wave_area);
            StoredPolylineCollection support_lines =
                intersection_polylines_expolygons(storage, support_area_lines, supported_band);
            support_lines =
                reconnect_polylines(storage, support_lines, SCALED_EPSILON * 2, coord_t(SCALED_EPSILON));
            support_lines = order_wave_polylines_from_current_point(storage, support_lines, nullptr);
            for (const Polyline line : support_lines)
                (void)append_wave_polyline(storage, zone_paths, line, input, lower_layer_distancer);
        }

        for (coord_t distance_from_support = std::max<coord_t>(SCALED_EPSILON, input.overhang_spacing / 2);
             distance_from_support <= max_wave_distance + SCALED_EPSILON;
             distance_from_support += input.overhang_spacing) {
            StoredExPolygonCollection wave_area =
                clipper_offset(clipper(optimized_lower_slices), double(distance_from_support)).to_expolygon_collection();
            if (wave_area.empty())
                continue;

            StoredPolylineCollection wave_area_lines = expolygons_to_polylines(storage, wave_area);
            StoredPolylineCollection wave_lines =
                intersection_polylines_expolygons(storage, wave_area_lines, zone_clip);
            wave_lines = reconnect_polylines(storage, wave_lines, SCALED_EPSILON * 2, coord_t(SCALED_EPSILON));
            if (wave_lines.empty())
                continue;

            c_point current_point = {};
            const c_point *current_point_ptr = nullptr;
            if (zone_paths.child_count() > 0) {
                current_point = zone_paths.child(zone_paths.child_count() - 1).back();
                current_point_ptr = &current_point;
            }
            wave_lines = order_wave_polylines_from_current_point(storage, wave_lines, current_point_ptr);
            for (const Polyline line : wave_lines)
                has_unsupported_wave |=
                    append_wave_polyline(storage, zone_paths, line, input, lower_layer_distancer);
        }

        // A supported pre-line is only useful as the lead-in for real
        // unsupported-side waves. If clipping removed every unsupported wave
        // from a tiny zone, do not consume supported fill just to print that
        // lead-in alone.
        if (!zone_paths.empty() && has_unsupported_wave) {
            zone_paths.disable_sort();
            zone_paths.disable_reverse();
            extra_perimeters.push_back(std::move(zone_paths));
        }
    }

    out.extra_perimeters = std::move(extra_perimeters);
    StoredExPolygonCollection wave_filled_area =
        coverage_for_extra_perimeters(storage, out.extra_perimeters, infill_area, input);
    wave_filled_area.ensure_valid();
    StoredExPolygonCollection residual_overhangs =
        clipper_diff(clipper(overhangs), clipper(wave_filled_area)).to_expolygon_collection();
    residual_overhangs.ensure_valid();

    // Any leftover inside the overhang domain is owned by this post-process:
    // tiny leftovers disappear, printable leftovers become square-flow gap
    // fill. In both cases the area is removed from later fill/gap-fill stages
    // so it cannot be printed twice.
    append_residual_gap_fill_paths(storage, out.extra_perimeters, residual_overhangs, input, lower_layer_distancer);
    out.filled_area =
        clipper_union2(clipper(wave_filled_area), clipper(residual_overhangs)).to_expolygon_collection();
    out.filled_area.ensure_valid();
    out.unfilled_area = clipper_diff(clipper(infill_area), clipper(out.filled_area)).to_expolygon_collection();
    out.unfilled_area.ensure_valid();
    return out;
}

OverhangGenerationInput generation_input_for_island(storage_handle *storage,
                                                    const Object &object,
                                                    const LayerIsland &island,
                                                    uint32_t layer_idx,
                                                    const c_flow &perimeter_flow,
                                                    const c_flow &external_flow)
{
    const Config region_config = island.region(0).print_region().config();
    const Config object_config = object.config();
    const int32_t perimeter_count = perimeter_count_from_config(region_config);

    OverhangGenerationInput input(storage);
    input.wave_flow = wave_flow_from_perimeter_flow(perimeter_flow);
    input.overhang_spacing = overhang_spacing_from_config(region_config, perimeter_flow);
    input.perimeter_depth = perimeter_count <= 0 ? 0 :
        external_flow.width + perimeter_flow.spacing * (perimeter_count - 1);
    input.lower_slices = lower_slices_for_island(storage, island);

    // Object-local first layers do not receive extra overhang anchors. There
    // is no lower object material to grow from even if raft/support exists.
    if (layer_idx == 0 || (object_config.has(k_raft_layers_key) &&
                           layer_idx <= uint32_t(std::max(0, object_config.get(k_raft_layers_key).get_int()))))
        input.perimeter_depth = 0;

    return input;
}

coord_t infill_overlap_for_island(const LayerIsland &island,
                                  const c_flow &perimeter_flow,
                                  const c_flow &external_flow)
{
    if (island.region_count() == 0)
        return 0;

    const Config config = island.region(0).print_region().config();
    const int32_t perimeter_count = perimeter_count_from_config(config);
    if (perimeter_count <= 0 || !config.has(k_infill_overlap_key))
        return 0;

    const double ratio = unscaled(perimeter_count == 1 ? external_flow.spacing : perimeter_flow.spacing);
    return scale_i(config.get(k_infill_overlap_key).get_effective_value(ratio));
}

void prepend_extra_perimeter_zone_groups_to_root(storage_handle *storage,
                                                 MutableExtrusionEntity root,
                                                 std::vector<StoredExtrusionEntity> &extra_perimeters)
{
    // The root prints the extra-perimeter wrapper first, then the original
    // perimeter tree. The wrapper itself is sortable between zones, while
    // every zone is locked so its waves stay ordered from support toward open
    // air.
    StoredExtrusionEntity original(storage, root.readonly());
    StoredExtrusionEntity extra_zones(storage);

    for (StoredExtrusionEntity &zone_paths : extra_perimeters) {
        if (zone_paths.empty())
            continue;

        const uint32_t idx = extra_zones.append_child_move(zone_paths.mutable_view());
        assert(!is_invalid_index(idx));
        (void)idx;
    }

    extra_zones.set_flags(extra_zones.flags() | RAW_EXTRUSION_FLAG_SORTABLE);
    extra_zones.disable_reverse();

    root.clear_content();
    if (!extra_zones.empty()) {
        const uint32_t idx = root.append_child_move(extra_zones.mutable_view());
        assert(!is_invalid_index(idx));
        (void)idx;
    }

    if (original.child_count() > 0) {
        while (original.child_count() > 0)
            root.move_child_from(root.child_count(), original.mutable_view(), 0);
    } else if (!original.empty()) {
        root.append_child_move(original.mutable_view());
    }

    root.disable_sort();
    root.disable_reverse();
}

void update_fill_areas(storage_handle *storage,
                       const run_ctx_post_perimeter_generation &ctx,
                       const LayerIsland &island,
                       const OverhangGenerationOutput &generated,
                       coord_t infill_overlap)
{
    // The generated paths now occupy part of the infill area. Keep the strict
    // free area conservative, and rebuild the wider fill area with the same
    // overlap rule used by the post-perimeter pipeline.
    const ExPolygonCollection fill_areas = island.infill_areas();
    const ExPolygonCollection free_source = island.infill_no_overlap_areas().empty() ?
        island.infill_areas() :
        island.infill_no_overlap_areas();

    ClipperContext clipper(storage);
    StoredExPolygonCollection next_free_areas =
        clipper_diff(clipper(free_source), clipper(generated.filled_area)).to_expolygon_collection();
    StoredExPolygonCollection next_fill_areas(storage);
    if (infill_overlap != 0) {
        StoredExPolygonCollection expanded_unfilled =
            clipper_offset(clipper(generated.unfilled_area), infill_overlap).to_expolygon_collection();
        next_fill_areas =
            clipper_intersection(clipper(fill_areas), clipper(expanded_unfilled)).to_expolygon_collection();
    } else {
        next_fill_areas =
            clipper_diff(clipper(fill_areas), clipper(generated.filled_area)).to_expolygon_collection();
    }

    const int32_t fill_ok = ctx.set_island_fill_areas(island.handle(), next_fill_areas.handle());
    const int32_t free_ok = ctx.set_island_fill_free_areas(island.handle(), next_free_areas.handle());
    assert(fill_ok != 0);
    assert(free_ok != 0);
    (void)fill_ok;
    (void)free_ok;
}

MutableExtrusionEntity first_mutable_perimeter_root(const run_ctx_post_perimeter_generation &ctx,
                                                    const LayerIsland &island)
{
    for (uint32_t idx = 0; idx < island.region_island_count(); ++idx) {
        const LayerRegionIsland region_island = island.region_island(idx);
        extrusion_entity_handle *root =
            ctx.get_region_island_mutable_extrusion(region_island.handle(), RAW_EXTRUSION_ROLE_PERIMETER);
        if (root != nullptr)
            return MutableExtrusionEntity(root);
    }
    return MutableExtrusionEntity();
}

void process_island(const run_ctx_post_perimeter_generation &ctx,
                    storage_handle *storage,
                    const Object &object,
                    const LayerIsland &island,
                    uint32_t layer_idx)
{
    if (island.region_count() == 0 || island.region_island_count() == 0 ||
        island.infill_areas().empty() || island.lower_island_count() == 0)
        return;

    MutableExtrusionEntity root = first_mutable_perimeter_root(ctx, island);
    if (!root)
        return;

    const c_flow perimeter_flow = island.region(0).flow(RAW_EXTRUSION_ROLE_INTERNAL_PERIMETER);
    const c_flow external_flow = island.region(0).flow(RAW_EXTRUSION_ROLE_EXTERNAL_PERIMETER);
    OverhangGenerationInput input =
        generation_input_for_island(storage, object, island, layer_idx, perimeter_flow, external_flow);
    if (input.perimeter_depth <= 0 || input.overhang_spacing <= 0 || input.lower_slices.empty())
        return;

    RegionSettings settings(storage, island, {{k_extra_perimeters_on_overhangs_key}});
    settings.segregate(island.slice());

    const ExPolygonCollection infill_candidate =
        island.infill_no_overlap_areas().empty() ?
        island.infill_areas() :
        island.infill_no_overlap_areas();
    StoredExPolygonCollection enabled_area = enabled_infill_area(storage, infill_candidate, settings);
    if (enabled_area.empty())
        return;

    OverhangGenerationOutput generated =
        generate_extra_perimeters_over_overhangs_wave(storage, enabled_area, input);
    const bool has_extra_perimeter =
        std::any_of(generated.extra_perimeters.begin(),
                    generated.extra_perimeters.end(),
                    [](const StoredExtrusionEntity &paths) { return !paths.empty(); });
    if (!has_extra_perimeter)
        return;

    if (settings.has_many_config(k_extra_perimeters_on_overhangs_key)) {
        ClipperContext clipper(storage);
        StoredExPolygonCollection disabled_area =
            clipper_diff(clipper(infill_candidate), clipper(enabled_area)).to_expolygon_collection();
        generated.unfilled_area =
            clipper_union2(clipper(generated.unfilled_area), clipper(disabled_area)).to_expolygon_collection();
    }

    prepend_extra_perimeter_zone_groups_to_root(storage, root, generated.extra_perimeters);
    update_fill_areas(storage, ctx, island, generated, infill_overlap_for_island(island, perimeter_flow, external_flow));
}

void process_layer(uint32_t layer_idx,
                   storage_handle *scratch_storage,
                   const run_ctx_post_perimeter_generation *ctx,
                   const Object *object)
{
    assert(scratch_storage != nullptr);
    assert(ctx != nullptr);
    assert(object != nullptr);

    // Each worker gets its own scratch storage from the helper. Temporary
    // RegionSettings, Clipper operands and generated path containers stay local
    // to one layer, while the step callbacks move the final data into the host
    // layer island before the scratch storage is cleared.
    const Layer layer = object->layer(layer_idx);
    for (uint32_t island_idx = 0; island_idx < layer.island_count(); ++island_idx) {
        process_island(*ctx, scratch_storage, *object, layer.island(island_idx), layer_idx);
    }
}

} // namespace

ExtraPerimeterOverhangWave &
ExtraPerimeterOverhangWave::instance(orchestrator_handle *orch)
{
    static ExtraPerimeterOverhangWave s_instance(orch);
    return s_instance;
}

const char *ExtraPerimeterOverhangWave::id_impl() const noexcept
{
    return k_extra_overhang_perimeters_wave_id;
}

const char *ExtraPerimeterOverhangWave::name_impl() const noexcept
{
    return "Extra perimeter overhang wave";
}

const char *ExtraPerimeterOverhangWave::description_impl() const noexcept
{
    return "Adds overhang perimeter anchors by growing printable waves from lower-layer support.";
}

const char *ExtraPerimeterOverhangWave::exclusive_group_impl() const noexcept
{
    return k_extra_overhang_perimeters_group;
}

const char *ExtraPerimeterOverhangWave::exclusive_group_label_impl() const noexcept
{
    return "Extra overhang perimeter strategy";
}

const char *ExtraPerimeterOverhangWave::exclusive_group_tooltip_impl() const noexcept
{
    return "Choose the algorithm used to add extra perimeter anchors under overhangs.";
}

slicing_step_t ExtraPerimeterOverhangWave::step_impl() const noexcept
{
    return STEP_POST_PERIMETER;
}

const char *const *ExtraPerimeterOverhangWave::dependencies_impl() const noexcept
{
    return k_no_dependencies;
}

int32_t ExtraPerimeterOverhangWave::priority_impl() const noexcept
{
    return -19;
}

int32_t ExtraPerimeterOverhangWave::used_config_keys(raw_used_config_key *keys) const noexcept
{
    if (keys != nullptr)
        for (uint32_t idx = 0; idx < sizeof(k_used_config_keys) / sizeof(k_used_config_keys[0]); ++idx)
            keys[idx] = k_used_config_keys[idx];
    return int32_t(sizeof(k_used_config_keys) / sizeof(k_used_config_keys[0]));
}

const char *ExtraPerimeterOverhangWave::progress_message_format_impl() const noexcept
{
    return "Extra overhang wave perimeters: %u / %u layers";
}

const char *ExtraPerimeterOverhangWave::exclusive_group_ui_fragment() noexcept
{
    // The selector key is generated by the host from STEP_POST_PERIMETER and
    // the shared exclusive group id. Keeping the selector in the same line as
    // the activation boolean makes the user choose the strategy exactly where
    // the feature is enabled.
    return "page:Perimeters & Shell\n"
           "group:Quality\n"
           "line:Extra perimeters\n"
           "setting:insert$aftersetting$extra_perimeters_on_overhangs:"
           "exclusive_group_700_perimeter_post_process_extra_perimeters_on_overhangs_plugin\n"
           "end_line\n";
}

void ExtraPerimeterOverhangWave::inilialize_impl(storage_handle *) const
{
    orchestrator_add_ui_fragment(m_orchestrator,
                                 "print.ui",
                                 k_extra_overhang_perimeters_group,
                                 ExtraPerimeterOverhangWave::exclusive_group_ui_fragment(),
                                 1);
}

void ExtraPerimeterOverhangWave::setup_run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_post_perimeter_generation *ctx = plugin_ctx_as_post_perimeter_generation(run_ctx);
    if (ctx != nullptr && ctx->object != nullptr)
        progress().add_max(Object(ctx->object).layer_count());
}

void ExtraPerimeterOverhangWave::run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_post_perimeter_generation *ctx = plugin_ctx_as_post_perimeter_generation(run_ctx);
    if (ctx == nullptr || run_ctx == nullptr || run_ctx->plugin_storage == nullptr ||
        ctx->object == nullptr ||
        ctx->get_region_island_mutable_extrusion == nullptr ||
        ctx->set_island_fill_areas == nullptr ||
        ctx->set_island_fill_free_areas == nullptr)
        return;

    const Object object(ctx->object);
    parallel_for_storage_with_progress(
        0,
        object.layer_count(),
        run_ctx,
        &progress(),
        process_layer,
        ctx,
        &object);
}

void register_extra_perimeter_overhang_wave_plugin(orchestrator_handle *orch)
{
    orchestrator_register_plugin(orch, ExtraPerimeterOverhangWave::instance(orch).c_instance());
}

}}} // namespace slic3r_api::Perimeter::ExtraPerimeterOverhangWavePlugin

#ifdef EXTRA_PERIMETER_OVERHANG_WAVE_PLUGIN_DLL
SLIC3R_PLUGIN_DECLARE_ABI_VERSION()

extern "C" SLIC3R_PLUGIN_API void register_plugin(orchestrator_handle *orch)
{
    slic3r_api::Perimeter::ExtraPerimeterOverhangWavePlugin::register_extra_perimeter_overhang_wave_plugin(orch);
}
#endif // EXTRA_PERIMETER_OVERHANG_WAVE_PLUGIN_DLL
