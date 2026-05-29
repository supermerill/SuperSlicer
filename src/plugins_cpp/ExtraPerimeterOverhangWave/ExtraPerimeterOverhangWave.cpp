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
#include <mutex>
#include <utility>
#include <vector>

#include "libslic3r/Api/plugin/c/steps/slic3r_step_post_perimeter.h"
#include "libslic3r/Api/plugin/c/slic3r_extrusions.h"
#include "libslic3r/Api/plugin/c/slic3r_utils.h"
#include "libslic3r/Api/plugin/cpp/DataTreeViews.hpp"
#include "libslic3r/Api/plugin/cpp/ExtrusionViews.hpp"
#include "libslic3r/Api/plugin/cpp/LineDistancer.hpp"
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
    EPropertyAttributes supported_anchor_attributes;
    StoredExPolygonCollection lower_slices;
};

struct OverhangGenerationOutput
{
    explicit OverhangGenerationOutput(storage_handle *storage) : filled_area(storage), unfilled_area(storage) {}

    std::vector<StoredExtrusionEntity> extra_perimeters;
    StoredExPolygonCollection filled_area;
    StoredExPolygonCollection unfilled_area;
};

coord_t scaled_float_or_percent_value(const Config &config,
                                      const char *key,
                                      double ratio,
                                      coord_t fallback = 0)
{
    if (!config.has(key))
        return fallback;
    return scale_i(config.get(key).get_effective_value(ratio));
}

int32_t config_int_or(const Config &config, const char *key, int32_t fallback)
{
    return config.has(key) ? config.get(key).get_int() : fallback;
}

coord_t overhang_spacing_from_config(const Config &config, const c_flow &perimeter_flow)
{
    const coord_t configured = scaled_float_or_percent_value(
        config, k_overhangs_extrusion_spacing_key, unscaled(perimeter_flow.nozzle_diameter), 0);
    return configured > 0 ? configured : perimeter_flow.spacing;
}

WaveFlow wave_flow_from_perimeter_flow(const c_flow &perimeter_flow)
{
    // The wave paths are tagged as overhangs for downstream classification,
    // but their physical section stays the normal perimeter flow. This keeps
    // the feature focused on ordering/anchoring instead of changing flow.
    WaveFlow out;
    out.width = perimeter_flow.width;
    out.spacing = perimeter_flow.spacing;
    out.height = perimeter_flow.height;
    out.nozzle_diameter = perimeter_flow.nozzle_diameter;
    out.mm3_per_mm = perimeter_flow.mm3_per_mm;
    out.attributes.extrusion_role(RAW_EXTRUSION_ROLE_OVERHANG_PERIMETER)
                         .no_seam_enabled(true)
                         .mm3_per_mm(perimeter_flow.mm3_per_mm)
                         .width(float(unscaled(perimeter_flow.width)))
                         .height(float(unscaled(perimeter_flow.height)));
    return out;
}

EPropertyAttributes supported_anchor_attributes_from_perimeter_flow(const c_flow &perimeter_flow)
{
    // The first wave is placed slightly on the supported side of the boundary.
    // It is a real printable anchor, but it must not be tagged as overhang
    // material because it is not suspended in air.
    EPropertyAttributes out;
    out.extrusion_role(RAW_EXTRUSION_ROLE_INTERNAL_PERIMETER)
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
    StoredExPolygonCollection out(storage);
    out.push_back(area);
    return out;
}

StoredExPolygonCollection union_collection(storage_handle *storage, const ExPolygonCollection &areas)
{
    if (areas.empty())
        return StoredExPolygonCollection(storage);
    ClipperContext clip(storage);
    return clipper_union(clip(areas)).to_expolygon_collection();
}

StoredExPolygonCollection union2_collection(storage_handle *storage,
                                            const ExPolygonCollection &first,
                                            const ExPolygonCollection &second)
{
    if (first.empty())
        return second.clone(storage);
    if (second.empty())
        return first.clone(storage);
    ClipperContext clip(storage);
    return clipper_union2(clip(first), clip(second)).to_expolygon_collection();
}

StoredExPolygonCollection diff_collection(storage_handle *storage,
                                          const ExPolygonCollection &subject,
                                          const ExPolygonCollection &clip_area)
{
    if (subject.empty())
        return StoredExPolygonCollection(storage);
    if (clip_area.empty())
        return subject.clone(storage);
    ClipperContext clip(storage);
    return clipper_diff(clip(subject), clip(clip_area)).to_expolygon_collection();
}

StoredExPolygonCollection intersection_collection(storage_handle *storage,
                                                  const ExPolygonCollection &subject,
                                                  const ExPolygonCollection &clip_area)
{
    if (subject.empty() || clip_area.empty())
        return StoredExPolygonCollection(storage);
    ClipperContext clip(storage);
    return clipper_intersection(clip(subject), clip(clip_area)).to_expolygon_collection();
}

StoredExPolygonCollection offset_collection(storage_handle *storage,
                                            const ExPolygonCollection &subject,
                                            double delta,
                                            clipper_join_type_t join_type = CLIPPER_JOIN_MITER,
                                            double miter_limit = 3.0,
                                            clipper_end_type_t end_type = CLIPPER_END_CLOSED_POLYGON)
{
    if (subject.empty())
        return StoredExPolygonCollection(storage);
    ClipperContext clip(storage);
    return clipper_offset(clip(subject), delta, join_type, miter_limit, end_type).to_expolygon_collection();
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

void append_points(std::vector<c_point> &dst, const std::vector<c_point> &src)
{
    dst.insert(dst.end(), src.begin(), src.end());
}

std::vector<c_point> reversed_points(const std::vector<c_point> &points)
{
    return std::vector<c_point>(points.rbegin(), points.rend());
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
                append_points(base, next);
                alive[second_idx] = false;
            } else if (squared_distance(base.back(), next.back()) < limit_squared) {
                const std::vector<c_point> reversed = reversed_points(next);
                append_points(base, reversed);
                alive[second_idx] = false;
            } else if (squared_distance(base.front(), next.back()) < limit_squared) {
                std::vector<c_point> merged = next;
                append_points(merged, base);
                base = std::move(merged);
                alive[second_idx] = false;
            } else if (squared_distance(base.front(), next.front()) < limit_squared) {
                std::vector<c_point> merged = reversed_points(next);
                append_points(merged, base);
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

    if (lower_islands.size() > 1)
        lower_expolygons = union_collection(storage, lower_expolygons);
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
    // Two consecutive leaves may have different roles and therefore cannot be
    // merged into the same path. They can still be oriented as a pair so the
    // previous leaf ends near the next leaf's start.
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

StoredExtrusionEntity make_overhang_path(storage_handle *storage,
                                         const Polyline &polyline,
                                         const OverhangGenerationInput &input)
{
    StoredExtrusionEntity path(storage, polyline);
    path.get_or_add_property<EPropertyAttributes>() = input.wave_flow.attributes;
    path.disable_reverse();
    return path;
}

StoredExtrusionEntity make_supported_anchor_path(storage_handle *storage,
                                                 const Polyline &polyline,
                                                 const OverhangGenerationInput &input)
{
    StoredExtrusionEntity path(storage, polyline);
    path.get_or_add_property<EPropertyAttributes>() = input.supported_anchor_attributes;
    path.disable_reverse();
    return path;
}

bool entity_is_overhang_perimeter(const ExtrusionEntity &entity)
{
    const EPropertyAttributes *attributes = entity.property<EPropertyAttributes>();
    return attributes != nullptr &&
           RAW_EXTRUSION_ROLE_IS_PERIMETER(attributes->role) &&
           RAW_EXTRUSION_ROLE_IS_BRIDGE(attributes->role);
}

void append_polyline_to_wave_path(MutableExtrusionEntity dst, const Polyline &src)
{
    if (src.empty())
        return;

    // This append intentionally preserves a short connector as an extrusion
    // segment. The caller has already checked that the connector is short
    // enough to extrude instead of forcing a travel.
    std::vector<c_point> points = dst.points();
    const std::vector<c_point> src_points = src.points();
    points.insert(points.end(), src_points.begin(), src_points.end());
    dst.set_points(points);
}

void append_supported_anchor_polyline(storage_handle *storage,
                                      StoredExtrusionEntity &zone_paths,
                                      const Polyline &polyline,
                                      const OverhangGenerationInput &input,
                                      const LineDistancer &lower_layer_distancer)
{
    if (!polyline.is_valid() || polyline.length() < input.wave_flow.width)
        return;

    StoredExtrusionEntity path = make_supported_anchor_path(storage, polyline, input);
    orient_extra_perimeter_from_support(path.mutable_view(), lower_layer_distancer);
    const uint32_t idx = zone_paths.add_child(path.mutable_view());
    assert(!is_invalid_index(idx));
    (void)idx;
}

void append_wave_polyline(storage_handle *storage,
                          StoredExtrusionEntity &zone_paths,
                          const Polyline &polyline,
                          const OverhangGenerationInput &input,
                          const LineDistancer &lower_layer_distancer)
{
    if (!polyline.is_valid() || polyline.length() < input.wave_flow.width)
        return;

    StoredExtrusionEntity path = make_overhang_path(storage, polyline, input);
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
        if (entity_is_overhang_perimeter(previous.readonly()) &&
            link_choice.distance_squared < link_distance * link_distance) {
            StoredPolyline path_polyline = make_polyline(storage, path.points());
            append_polyline_to_wave_path(previous, path_polyline);
            return;
        }

        // A supported pre-line has a different role from the first overhang
        // line, so it stays a separate leaf. It still participates in endpoint
        // orientation above, which avoids an immediate travel back to the
        // opposite end of the same local wave group.
        const uint32_t idx = zone_paths.add_child(path.mutable_view());
        assert(!is_invalid_index(idx));
        (void)idx;
        return;
    }

    orient_extra_perimeter_from_support(path.mutable_view(), lower_layer_distancer);
    const uint32_t idx = zone_paths.add_child(path.mutable_view());
    assert(!is_invalid_index(idx));
    (void)idx;
}

StoredExPolygonCollection coverage_for_extra_perimeters(storage_handle *storage,
                                                        const std::vector<StoredExtrusionEntity> &extra_perimeters,
                                                        const ExPolygonCollection &clip_area,
                                                        const OverhangGenerationInput &input)
{
    StoredExPolygonCollection coverage(storage);
    ClipperContext clip(storage);
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
                clipper_offset(clip(centerline),
                               0.5 * double(input.overhang_spacing),
                               CLIPPER_JOIN_SQUARE,
                               0.,
                               end_type).to_expolygon_collection();
            coverage.append_move_from(path_coverage);
        }
    }

    if (coverage.empty())
        return StoredExPolygonCollection(storage);

    StoredExPolygonCollection coverage_union = union_collection(storage, coverage);
    return intersection_collection(storage, coverage_union, clip_area);
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

    StoredExPolygonCollection area_collection = collection_from_expolygon(storage, area);
    return offset_collection(storage, area_collection, -double(input.wave_flow.nozzle_diameter)).empty();
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

        StoredPolylineCollection fills =
            expolygon_medial_axis(storage, residual, 0.75 * input.wave_flow.width, 3.0 * input.overhang_spacing);
        if (fills.empty())
            continue;

        // Medial-axis output may slightly overshoot the clipped leftover.
        // Intersect it back with the residual polygon before turning the lines
        // into extrusion paths.
        StoredExPolygonCollection residual_area = collection_from_expolygon(storage, residual);
        fills = intersection_polylines_expolygons(storage, fills, residual_area);
        fills = reconnect_polylines(storage, fills, SCALED_EPSILON * 2, coord_t(SCALED_EPSILON));
        if (fills.empty())
            continue;

        StoredExtrusionEntity gap_fill_zone(storage);
        for (const Polyline fill : fills) {
            if (!fill.is_valid() || fill.length() < input.wave_flow.nozzle_diameter / 10)
                continue;

            StoredExtrusionEntity path(storage, fill);
            path.get_or_add_property<EPropertyAttributes>() = gap_fill_attributes;
            path.disable_reverse();
            orient_extra_perimeter_from_support(path.mutable_view(), lower_layer_distancer);
            const uint32_t idx = gap_fill_zone.add_child(path.mutable_view());
            assert(!is_invalid_index(idx));
            (void)idx;
        }

        if (!gap_fill_zone.empty())
            extra_perimeters.push_back(std::move(gap_fill_zone));
    }
}

bool contains_overhang_path(const ExtrusionEntity &zone)
{
    for (const ExtrusionEntity path : zone.children())
        if (entity_is_overhang_perimeter(path))
            return true;
    return false;
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

    // The printable domain is the enabled infill area that is not already
    // covered by lower-layer material. Each later wave is clipped to this area.
    StoredExPolygonCollection overhangs = diff_collection(storage, infill_area, optimized_lower_slices);
    if (overhangs.empty())
        return out;

    LineDistancer lower_layer_distancer(optimized_lower_slices);
    std::vector<StoredExtrusionEntity> extra_perimeters;

    // A zone is printed from the support boundary outward. Multiple zones are
    // independent and may later be sorted as whole units by the wrapper entity.
    StoredExPolygonCollection zones = union_collection(storage, overhangs);
    for (const ExPolygon zone : zones) {
        StoredExtrusionEntity zone_paths(storage);
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
            intersection_collection(storage, infill_area, optimized_lower_slices);
        StoredExPolygonCollection zone_margin =
            offset_collection(storage, zone_clip, double(input.overhang_spacing));
        StoredExPolygonCollection supported_band =
            intersection_collection(storage, supported_lower, zone_margin);
        if (!supported_band.empty()) {
            StoredExPolygonCollection support_wave_area =
                offset_collection(storage, optimized_lower_slices, -0.5 * double(input.overhang_spacing));
            StoredPolylineCollection support_area_lines =
                expolygons_to_polylines(storage, support_wave_area);
            StoredPolylineCollection support_lines =
                intersection_polylines_expolygons(storage, support_area_lines, supported_band);
            support_lines =
                reconnect_polylines(storage, support_lines, SCALED_EPSILON * 2, coord_t(SCALED_EPSILON));
            support_lines = order_wave_polylines_from_current_point(storage, support_lines, nullptr);
            for (const Polyline line : support_lines)
                append_supported_anchor_polyline(storage, zone_paths, line, input, lower_layer_distancer);
        }

        for (coord_t distance_from_support = std::max<coord_t>(SCALED_EPSILON, input.overhang_spacing / 2);
             distance_from_support <= max_wave_distance + SCALED_EPSILON;
             distance_from_support += input.overhang_spacing) {
            StoredExPolygonCollection wave_area =
                offset_collection(storage, optimized_lower_slices, double(distance_from_support));
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
                append_wave_polyline(storage, zone_paths, line, input, lower_layer_distancer);
        }

        // A supported pre-line is only useful as the lead-in for real overhang
        // strokes. If clipping removed all overhang strokes from a tiny zone,
        // do not consume supported fill just to print that lead-in alone.
        if (!zone_paths.empty() && contains_overhang_path(zone_paths.readonly())) {
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
        diff_collection(storage, overhangs, wave_filled_area);
    residual_overhangs.ensure_valid();

    // Any leftover inside the overhang domain is owned by this post-process:
    // tiny leftovers disappear, printable leftovers become square-flow gap
    // fill. In both cases the area is removed from later fill/gap-fill stages
    // so it cannot be printed twice.
    append_residual_gap_fill_paths(storage, out.extra_perimeters, residual_overhangs, input, lower_layer_distancer);
    out.filled_area = union2_collection(storage, wave_filled_area, residual_overhangs);
    out.filled_area.ensure_valid();
    out.unfilled_area = diff_collection(storage, infill_area, out.filled_area);
    out.unfilled_area.ensure_valid();
    return out;
}

OverhangGenerationInput generation_input_for_island(storage_handle *storage,
                                                    const Print &print,
                                                    const Object &object,
                                                    const LayerIsland &island,
                                                    uint32_t layer_idx,
                                                    const c_flow &perimeter_flow,
                                                    const c_flow &external_flow)
{
    const Config region_config = island.region(0).print_region().config();
    const Config object_config = object.config();
    const int32_t perimeter_count = std::max(0, config_int_or(region_config, k_perimeters_key, 0));

    OverhangGenerationInput input(storage);
    input.wave_flow = wave_flow_from_perimeter_flow(perimeter_flow);
    input.supported_anchor_attributes = supported_anchor_attributes_from_perimeter_flow(perimeter_flow);
    input.overhang_spacing = overhang_spacing_from_config(region_config, perimeter_flow);
    input.perimeter_depth = perimeter_count <= 0 ? 0 :
        external_flow.width + perimeter_flow.spacing * (perimeter_count - 1);
    input.lower_slices = lower_slices_for_island(storage, island);

    // Object-local first layers do not receive extra overhang anchors. There
    // is no lower object material to grow from even if raft/support exists.
    if (layer_idx == 0 || (object_config.has(k_raft_layers_key) &&
                           layer_idx <= uint32_t(std::max(0, object_config.get(k_raft_layers_key).get_int()))))
        input.perimeter_depth = 0;

    (void)print;
    return input;
}

coord_t infill_overlap_for_island(const LayerIsland &island,
                                  const c_flow &perimeter_flow,
                                  const c_flow &external_flow)
{
    if (island.region_count() == 0)
        return 0;

    const Config config = island.region(0).print_region().config();
    const int32_t perimeter_count = std::max(0, config_int_or(config, k_perimeters_key, 0));
    if (perimeter_count <= 0 || !config.has(k_infill_overlap_key))
        return 0;

    const double ratio = unscaled(perimeter_count == 1 ? external_flow.spacing : perimeter_flow.spacing);
    return scale_i(config.get(k_infill_overlap_key).get_effective_value(ratio));
}

void prepend_extra_perimeter_zone_groups_to_root(storage_handle *storage,
                                                 MutableExtrusionEntity root,
                                                 std::vector<StoredExtrusionEntity> &extra_perimeters)
{
    // The root prints the overhang wrapper first, then the original perimeter
    // tree. The wrapper itself is sortable between zones, while every zone is
    // locked so its waves stay ordered from support toward open air.
    StoredExtrusionEntity original(storage, root.readonly());
    StoredExtrusionEntity extra_zones(storage);

    for (StoredExtrusionEntity &zone_paths : extra_perimeters) {
        if (zone_paths.empty())
            continue;

        const uint32_t idx = extra_zones.add_child(zone_paths.mutable_view());
        assert(!is_invalid_index(idx));
        (void)idx;
    }

    extra_zones.set_flags(extra_zones.flags() | RAW_EXTRUSION_FLAG_SORTABLE);
    extra_zones.disable_reverse();

    root.clear_content();
    if (!extra_zones.empty()) {
        const uint32_t idx = root.add_child(extra_zones.mutable_view());
        assert(!is_invalid_index(idx));
        (void)idx;
    }

    if (original.child_count() > 0) {
        while (original.child_count() > 0)
            root.move_child_from(root.child_count(), original.mutable_view(), 0);
    } else if (!original.empty()) {
        root.add_child(original.mutable_view());
    }

    root.disable_sort();
    root.disable_reverse();
}

bool extra_perimeters_empty(const std::vector<StoredExtrusionEntity> &extra_perimeters)
{
    for (const StoredExtrusionEntity &paths : extra_perimeters)
        if (!paths.empty())
            return false;
    return true;
}

void publish_fill_areas(const run_ctx_post_perimeter_generation &ctx,
                        const LayerIsland &island,
                        const ExPolygonCollection &fill_areas,
                        const ExPolygonCollection &free_areas)
{
    const int32_t fill_ok = ctx.set_island_fill_areas(island.handle(), fill_areas.handle());
    const int32_t free_ok = ctx.set_island_fill_free_areas(island.handle(), free_areas.handle());
    assert(fill_ok != 0);
    assert(free_ok != 0);
    (void)fill_ok;
    (void)free_ok;
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

    StoredExPolygonCollection next_free_areas = diff_collection(storage, free_source, generated.filled_area);
    StoredExPolygonCollection next_fill_areas(storage);
    if (infill_overlap != 0) {
        StoredExPolygonCollection expanded_unfilled =
            offset_collection(storage, generated.unfilled_area, infill_overlap);
        next_fill_areas = intersection_collection(storage, fill_areas, expanded_unfilled);
    } else {
        next_fill_areas = diff_collection(storage, fill_areas, generated.filled_area);
    }

    publish_fill_areas(ctx, island, next_fill_areas, next_free_areas);
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
                    const Print &print,
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
        generation_input_for_island(storage, print, object, island, layer_idx, perimeter_flow, external_flow);
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
    if (extra_perimeters_empty(generated.extra_perimeters))
        return;

    if (settings.has_many_config(k_extra_perimeters_on_overhangs_key)) {
        StoredExPolygonCollection disabled_area = diff_collection(storage, infill_candidate, enabled_area);
        generated.unfilled_area = union2_collection(storage, generated.unfilled_area, disabled_area);
    }

    prepend_extra_perimeter_zone_groups_to_root(storage, root, generated.extra_perimeters);
    update_fill_areas(storage, ctx, island, generated, infill_overlap_for_island(island, perimeter_flow, external_flow));
}

struct ParallelLayerRunData
{
    const run_ctx_post_perimeter_generation *ctx = nullptr;
    const plugin_run_context *run_ctx = nullptr;
    storage_handle *storage = nullptr;
    const Print *print = nullptr;
    const Object *object = nullptr;
    PluginProgress *progress = nullptr;

    // Plugin storage owns temporary handles created by RegionSettings and the
    // extrusion helpers. It is shared for the whole plugin run, so each island
    // mutation keeps storage access serialized until the ABI grows per-worker
    // temporary storage.
    std::mutex storage_mutex;
};

void process_layer_parallel(uint32_t layer_idx, void *user_data)
{
    ParallelLayerRunData *data = static_cast<ParallelLayerRunData *>(user_data);
    assert(data != nullptr);
    assert(data->ctx != nullptr);
    assert(data->run_ctx != nullptr);
    assert(data->storage != nullptr);
    assert(data->print != nullptr);
    assert(data->object != nullptr);
    assert(data->progress != nullptr);

    throw_if_cancelled(data->run_ctx);
    const Layer layer = data->object->layer(layer_idx);
    for (uint32_t island_idx = 0; island_idx < layer.island_count(); ++island_idx) {
        std::lock_guard<std::mutex> lock(data->storage_mutex);
        process_island(*data->ctx, data->storage, *data->print, *data->object, layer.island(island_idx), layer_idx);
    }
    data->progress->increment();
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
        ctx->print == nullptr || ctx->object == nullptr ||
        ctx->get_region_island_mutable_extrusion == nullptr ||
        ctx->set_island_fill_areas == nullptr ||
        ctx->set_island_fill_free_areas == nullptr)
        return;

    const Print print(ctx->print);
    const Object object(ctx->object);
    ParallelLayerRunData parallel_data;
    parallel_data.ctx = ctx;
    parallel_data.run_ctx = run_ctx;
    parallel_data.storage = run_ctx->plugin_storage;
    parallel_data.print = &print;
    parallel_data.object = &object;
    parallel_data.progress = &progress();

    slic3r_parallel_for(0, object.layer_count(), &parallel_data, process_layer_parallel);
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
