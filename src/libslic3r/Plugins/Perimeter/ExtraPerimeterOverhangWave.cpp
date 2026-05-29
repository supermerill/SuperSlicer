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
#include <unordered_map>
#include <vector>

#include "libslic3r/AABBTreeLines.hpp"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_post_perimeter.h"
#include "libslic3r/Api/plugin/c/slic3r_utils.h"
#include "libslic3r/Api/plugin/cpp/DataTreeViews.hpp"
#include "libslic3r/Api/plugin/cpp/ExtrusionViews.hpp"
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
    Slic3r::coord_t width = 0;
    Slic3r::coord_t spacing = 0;
    Slic3r::coord_t height = 0;
    Slic3r::coord_t nozzle_diameter = 0;
    double mm3_per_mm = 0.;
};

struct OverhangGenerationInput
{
    Slic3r::coord_t perimeter_depth = 0;
    Slic3r::coord_t overhang_spacing = 0;
    WaveFlow wave_flow;
    EPropertyAttributes supported_anchor_attributes;
    Slic3r::Polygons lower_slices;
};

struct OverhangGenerationOutput
{
    std::vector<ExtrusionEntity> extra_perimeters;
    StoredExPolygonCollection filled_area;
    StoredExPolygonCollection unfilled_area;
};

Slic3r::coord_t scaled_float_or_percent_value(const Config &config,
                                              const char *key,
                                              double ratio,
                                              Slic3r::coord_t fallback = 0)
{
    if (!config.has(key))
        return fallback;
    return Slic3r::scale_i(config.get(key).get_effective_value(ratio));
}

int32_t config_int_or(const Config &config, const char *key, int32_t fallback)
{
    return config.has(key) ? config.get(key).get_int() : fallback;
}

Slic3r::coord_t overhang_spacing_from_config(const Config &config, const c_flow &perimeter_flow)
{
    const Slic3r::coord_t configured = scaled_float_or_percent_value(
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
    if (max_mm3_per_mm > 0. && square_mm3_per_mm > max_mm3_per_mm) {
        side_mm = std::sqrt(max_mm3_per_mm);
    }

    return EPropertyAttributes()
        .extrusion_role(RAW_EXTRUSION_ROLE_GAP_FILL)
        .mm3_per_mm(side_mm * side_mm)
        .width(float(side_mm))
        .height(float(side_mm));
}

StoredExPolygonCollection lower_slices_for_island(storage_handle *storage, const LayerIsland &island) {
    StoredExPolygonCollection lower_expolygons(storage);
    const std::vector<LayerIsland> lower_islands = island.lower_islands();
    for (const LayerIsland &lower_island : lower_islands)
        lower_expolygons.push_back(lower_island.slice());

    if (lower_islands.size() > 1){
        ClipperContext clip(storage);
        lower_expolygons = clipper_union(clip(lower_expolygons)).to_expolygon_collection();
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

    // case solo config: only one setting value for the whole RegionSettings
    if (!settings.has_many_config(k_extra_perimeters_on_overhangs_key)) {
        if (!settings.get_solo_config(k_extra_perimeters_on_overhangs_key)
                 .get_bool(k_extra_perimeters_on_overhangs_key))
            return StoredExPolygonCollection(storage);
        return  candidate.clone(storage);
    }
    // case many_config : need to clip the area.

    Slic3r::ExPolygons enabled;
    // the RegionSettings is initialised with only a bool setting, so only two area possible: with and without
    const RegionSettings::AreaMap &areas = settings.get_areas(k_extra_perimeters_on_overhangs_key);
    for (auto &[is_overhang, clip] : areas) {
        if (is_overhang.get_bool()) {
            //found the good area, return it.
            return clip.intersections(candidate);
        }
    }

    return StoredExPolygonCollection(storage);
}

Slic3r::ExPolygons disabled_infill_area(const Slic3r::ExPolygons &candidate,
                                        const Slic3r::ExPolygons &enabled)
{
    if (candidate.empty() || enabled.empty())
        return candidate;
    return Slic3r::diff_ex(candidate, enabled);
}

Slic3r::Polylines reconnect_polylines(const Slic3r::Polylines &polylines,
                                      Slic3r::coordf_t limit_distance,
                                      Slic3r::coord_t resolution)
{
    if (polylines.empty())
        return polylines;

    std::unordered_map<size_t, Slic3r::Polyline> connected;
    connected.reserve(polylines.size());
    for (size_t idx = 0; idx < polylines.size(); idx++)
        if (!polylines[idx].empty())
            connected.emplace(idx, polylines[idx]);

    // Boolean clipping often splits one conceptual wave into small adjacent
    // pieces. Reconnect only near endpoints here; longer gaps are preserved so
    // they become real travels between separate extrusion paths.
    for (size_t first_idx = 0; first_idx < polylines.size(); first_idx++) {
        if (connected.find(first_idx) == connected.end())
            continue;
        Slic3r::Polyline &base = connected.at(first_idx);
        for (size_t second_idx = first_idx + 1; second_idx < polylines.size(); second_idx++) {
            if (connected.find(second_idx) == connected.end())
                continue;
            Slic3r::Polyline &next = connected.at(second_idx);
            if ((base.last_point() - next.first_point()).cast<Slic3r::coordf_t>().squaredNorm() <
                limit_distance * limit_distance) {
                base.append(std::move(next.points));
                connected.erase(second_idx);
            } else if ((base.last_point() - next.last_point()).cast<Slic3r::coordf_t>().squaredNorm() <
                       limit_distance * limit_distance) {
                base.points.insert(base.points.end(), next.points.rbegin(), next.points.rend());
                connected.erase(second_idx);
            } else if ((base.first_point() - next.last_point()).cast<Slic3r::coordf_t>().squaredNorm() <
                       limit_distance * limit_distance) {
                next.append(std::move(base.points));
                base = std::move(next);
                base.reverse();
                connected.erase(second_idx);
            } else if ((base.first_point() - next.first_point()).cast<Slic3r::coordf_t>().squaredNorm() <
                       limit_distance * limit_distance) {
                base.reverse();
                base.append(std::move(next.points));
                base.reverse();
                connected.erase(second_idx);
            }
        }
    }

    Slic3r::Polylines result;
    result.reserve(connected.size());
    for (std::pair<const size_t, Slic3r::Polyline> &entry : connected)
        result.push_back(std::move(entry.second));

    Slic3r::ensure_valid(result, resolution);
    return result;
}

void orient_extra_perimeter_from_support(
    ExtrusionEntity &path, const Slic3r::AABBTreeLines::LinesDistancer<Slic3r::Line> &lower_layer_aabb_tree) {
    if (path.empty())
        return;

    // Every generated stroke should start from the side closest to existing
    // material. Open strokes choose their direction; closed strokes rotate
    // their first point to the closest sampled vertex.
    Slic3r::Polyline discrete_polyline = path.polyline().to_polyline();
    if (discrete_polyline.size() < 2)
        return;

    if (discrete_polyline.front() == discrete_polyline.back()) {
        size_t closest_idx = 0;
        double closest_distance = std::numeric_limits<double>::max();
        discrete_polyline.points.pop_back();
        for (size_t idx = 0; idx < discrete_polyline.size(); idx++) {
            const double distance = lower_layer_aabb_tree.distance_from_lines<true>(discrete_polyline.points[idx]);
            if (distance < closest_distance) {
                closest_distance = distance;
                closest_idx = idx;
            }
        }
        std::rotate(discrete_polyline.begin(), discrete_polyline.begin() + closest_idx, discrete_polyline.end());
        discrete_polyline.points.push_back(discrete_polyline.points.front());
        path.polyline() = Slic3r::ArcPolyline(discrete_polyline);
        return;
    }

    const double first_distance = lower_layer_aabb_tree.distance_from_lines<true>(path.first_point());
    const double last_distance = lower_layer_aabb_tree.distance_from_lines<true>(path.last_point());
    if (last_distance < first_distance)
        path.reverse();
}

double squared_distance(const Slic3r::Point &lhs, const Slic3r::Point &rhs)
{
    return (lhs - rhs).cast<double>().squaredNorm();
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
    // merged into the same ExtrusionPath. They can still be oriented as a
    // pair so the previous leaf ends near the next leaf's start.
    EndpointLinkChoice best;
    const Slic3r::Point previous_first = previous.first_point();
    const Slic3r::Point previous_last = previous.last_point();
    const Slic3r::Point next_first = next.first_point();
    const Slic3r::Point next_last = next.last_point();

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
                                         const OverhangGenerationInput &input) {
    StoredExtrusionEntity path(storage, polyline);
    path.get_or_add_property<EPropertyAttributes>() = input.wave_flow.attributes;
    path.disable_reverse();
    return path;
}

StoredExtrusionEntity make_supported_anchor_path(storage_handle *storage,
                                                 const Polyline &polyline,
                                                 const OverhangGenerationInput &input) {
    StoredExtrusionEntity path(storage, polyline);
    path.get_or_add_property<EPropertyAttributes>() = input.supported_anchor_attributes;
    path.disable_reverse();
    return path;
}

void append_polyline_to_wave_path(ExtrusionEntity &dst, const Polyline &src)
{
    if (src.empty())
        return;

    // This append intentionally preserves a short connector as an extrusion
    // segment. The caller has already checked that the connector is short
    // enough to extrude instead of forcing a travel.
    dst.polyline().append(src.points);
}

void append_supported_anchor_polyline(
    ExtrusionEntity &zone_paths,
    Polyline polyline,
    const OverhangGenerationInput &input,
    const Slic3r::AABBTreeLines::LinesDistancer<Slic3r::Line> &lower_layer_aabb_tree)
{
    if (!polyline.is_valid() || polyline.length() < input.wave_flow.width)
        return;

    Slic3r::ExtrusionPath path = make_supported_anchor_path(polyline, input);
    orient_extra_perimeter_from_support(path, lower_layer_aabb_tree);
    zone_paths.push_back(std::move(path));
}

void append_wave_polyline(Slic3r::ExtrusionPaths &zone_paths,
                          Slic3r::Polyline polyline,
                          const OverhangGenerationInput &input,
                          const Slic3r::AABBTreeLines::LinesDistancer<Slic3r::Line> &lower_layer_aabb_tree)
{
    if (!polyline.is_valid() || polyline.length() < input.wave_flow.width)
        return;

    Slic3r::ExtrusionPath path = make_overhang_path(polyline, input);
    if (!zone_paths.empty()) {
        Slic3r::ExtrusionPath &previous = zone_paths.back();
        const EndpointLinkChoice link_choice = closest_endpoint_link(previous, path);
        const double link_distance = double(input.wave_flow.width) * 2.0;
        if (link_choice.reverse_previous)
            previous.reverse();
        if (link_choice.reverse_next)
            path.reverse();

        // Keep one physical zone in wave order. Nearby consecutive waves are
        // joined into one extrusion; distant waves become separate leaves, so
        // the printer travels without breaking the zone order.
        if (previous.role().has(Slic3r::ExtrusionRole::OverhangPerimeter) &&
            link_choice.distance_squared < link_distance * link_distance) {
            append_polyline_to_wave_path(previous, path.polyline().to_polyline());
            return;
        }

        // A supported pre-line has a different role from the first overhang
        // line, so it stays a separate leaf. It still participates in endpoint
        // orientation above, which avoids an immediate travel back to the
        // opposite end of the same local wave group.
        zone_paths.push_back(std::move(path));
        return;
    }

    orient_extra_perimeter_from_support(path, lower_layer_aabb_tree);
    zone_paths.push_back(std::move(path));
}

Slic3r::Polylines order_wave_polylines_from_current_point(Slic3r::Polylines polylines,
                                                          const Slic3r::Point *current_point)
{
    if (current_point == nullptr || polylines.size() < 2)
        return polylines;

    Slic3r::Polylines ordered;
    ordered.reserve(polylines.size());
    Slic3r::Point cursor = *current_point;

    // Clipper does not return pieces in travel-friendly order. Pick the next
    // piece by closest endpoint so any connector remains as short as possible.
    while (!polylines.empty()) {
        size_t best_idx = 0;
        double best_distance = std::numeric_limits<double>::max();
        for (size_t idx = 0; idx < polylines.size(); ++idx) {
            const Slic3r::Polyline &candidate = polylines[idx];
            if (candidate.empty())
                continue;
            const double candidate_distance =
                std::min(squared_distance(cursor, candidate.first_point()),
                         squared_distance(cursor, candidate.last_point()));
            if (candidate_distance < best_distance) {
                best_distance = candidate_distance;
                best_idx = idx;
            }
        }

        Slic3r::Polyline selected = std::move(polylines[best_idx]);
        if (squared_distance(cursor, selected.last_point()) <
            squared_distance(cursor, selected.first_point()))
            selected.reverse();

        ordered.push_back(std::move(selected));
        cursor = ordered.back().last_point();
        polylines.erase(polylines.begin() + best_idx);
    }

    return ordered;
}

Slic3r::ExPolygons coverage_for_extra_perimeters(const std::vector<StoredExtrusionEntityCollection> &extra_perimeters,
                                                 const Slic3r::ExPolygons &clip_area,
                                                 const OverhangGenerationInput &input)
{
    Slic3r::ExPolygons coverage;
    for (const StoredExtrusionEntityCollection &paths : extra_perimeters)
        for (const ExtrusionEntity &path : paths) {
            if (path.empty())
                continue;

            const Polyline centerline = path.polyline();
            if (centerline.size() < 2)
                continue;

            const Slic3r::ClipperLib::EndType end_type =
                centerline.front() == centerline.back() ?
                Slic3r::ClipperLib::etClosedLine :
                Slic3r::ClipperLib::etOpenSquare;
            // Free-fill bookkeeping uses spacing, not the physical overhang
            // width. The path may extrude a fatter bridge bead that sags below
            // the layer, but the next infill step only needs to reserve the
            // nominal 2D slot occupied by the generated centerline.
            Slic3r::ExPolygons path_coverage =
                Slic3r::offset_ex(centerline,
                                  0.5 * double(input.overhang_spacing),
                                  Slic3r::ClipperLib::jtSquare,
                                  0.,
                                  end_type);
            coverage.insert(coverage.end(), path_coverage.begin(), path_coverage.end());
        }

    if (coverage.empty())
        return {};
    return Slic3r::intersection_ex(Slic3r::union_ex(coverage), clip_area);
}

bool residual_area_is_too_small_for_gap_fill(const Slic3r::ExPolygon &area,
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

    return Slic3r::offset_ex(Slic3r::ExPolygons{area}, -double(input.wave_flow.nozzle_diameter)).empty();
}

void append_residual_gap_fill_paths(std::vector<Slic3r::ExtrusionPaths> &extra_perimeters,
                                    const Slic3r::ExPolygons &residual_overhangs,
                                    const OverhangGenerationInput &input,
                                    const Slic3r::AABBTreeLines::LinesDistancer<Slic3r::Line> &lower_layer_aabb_tree)
{
    if (residual_overhangs.empty())
        return;

    const Slic3r::ExtrusionAttributes gap_fill_attributes =
        gap_fill_attributes_from_wave_flow(input.wave_flow);

    for (const Slic3r::ExPolygon &residual : residual_overhangs) {
        if (residual_area_is_too_small_for_gap_fill(residual, input))
            continue;

        Slic3r::Polylines fills;
        residual.medial_axis(0.75 * input.wave_flow.width,
                             3.0 * input.overhang_spacing,
                             fills);
        if (fills.empty())
            continue;

        // Medial-axis output may slightly overshoot the clipped leftover.
        // Intersect it back with the residual polygon before turning the lines
        // into extrusion paths.
        fills = Slic3r::intersection_pl(fills, Slic3r::ExPolygons{residual});
        fills = reconnect_polylines(fills, SCALED_EPSILON * 2, Slic3r::coord_t(SCALED_EPSILON));
        if (fills.empty())
            continue;

        Slic3r::ExtrusionPaths gap_fill_zone;
        gap_fill_zone.reserve(fills.size());
        for (Slic3r::Polyline &fill : fills) {
            if (!fill.is_valid() || fill.length() < input.wave_flow.nozzle_diameter / 10)
                continue;

            Slic3r::ExtrusionPath path(
                Slic3r::ArcPolyline(fill),
                gap_fill_attributes,
                nullptr,
                false);
            path.set_can_reverse(false);
            orient_extra_perimeter_from_support(path, lower_layer_aabb_tree);
            gap_fill_zone.push_back(std::move(path));
        }

        if (!gap_fill_zone.empty())
            extra_perimeters.push_back(std::move(gap_fill_zone));
    }
}

bool contains_overhang_path(const Slic3r::ExtrusionPaths &paths)
{
    for (const Slic3r::ExtrusionPath &path : paths)
        if (path.role().has(Slic3r::ExtrusionRole::OverhangPerimeter))
            return true;
    return false;
}

OverhangGenerationOutput generate_extra_perimeters_over_overhangs_wave(
    const ExPolygonCollection &infill_area,
    const OverhangGenerationInput &input)
{
    if (infill_area.empty() || input.lower_slices.empty() ||
        input.perimeter_depth <= 0 || input.overhang_spacing <= 0)
        return {};

    const Slic3r::BoundingBox infill_area_bb =
        Slic3r::get_extents(infill_area).inflated(SCALED_EPSILON + input.perimeter_depth);
    const Slic3r::Polygons optimized_lower_slices =
        Slic3r::ClipperUtils::clip_clipper_polygons_with_subject_bbox(input.lower_slices, infill_area_bb);
    if (optimized_lower_slices.empty())
        return {};

    // The printable domain is the enabled infill area that is not already
    // covered by lower-layer material. Each later wave is clipped to this area.
    const Slic3r::ExPolygons overhangs = Slic3r::diff_ex(infill_area, optimized_lower_slices);
    if (overhangs.empty())
        return {};

    Slic3r::AABBTreeLines::LinesDistancer<Slic3r::Line> lower_layer_aabb_tree{
        Slic3r::to_lines(optimized_lower_slices)
    };
    std::vector<Slic3r::ExtrusionPaths> extra_perimeters;

    // A zone is printed from the support boundary outward. Multiple zones are
    // independent and may later be sorted as whole units by the wrapper entity.
    const Slic3r::ExPolygons zones = Slic3r::union_ex(overhangs);
    for (const Slic3r::ExPolygon &zone : zones) {
        Slic3r::ExtrusionPaths zone_paths;
        const Slic3r::ExPolygons zone_clip = { zone };
        const Slic3r::BoundingBox zone_bbox = Slic3r::get_extents(zone_clip);
        const Slic3r::coord_t max_wave_distance =
            std::max(zone_bbox.max.x() - zone_bbox.min.x(),
                     zone_bbox.max.y() - zone_bbox.min.y()) +
            input.overhang_spacing;

        // Add one supported line before crossing into the unsupported area.
        // This line consumes only a narrow supported band and returns the rest
        // of the supported fill domain to the regular infill generator.
        const Slic3r::ExPolygons supported_band = Slic3r::intersection_ex(
            Slic3r::intersection_ex(infill_area, optimized_lower_slices),
            Slic3r::offset_ex(zone_clip, double(input.overhang_spacing)));
        if (!supported_band.empty()) {
            const Slic3r::ExPolygons support_wave_area =
                Slic3r::offset_ex(optimized_lower_slices, -0.5 * double(input.overhang_spacing));
            Slic3r::Polylines support_lines =
                Slic3r::intersection_pl(Slic3r::to_polylines(support_wave_area), supported_band);
            support_lines =
                reconnect_polylines(support_lines, SCALED_EPSILON * 2, Slic3r::coord_t(SCALED_EPSILON));
            support_lines = order_wave_polylines_from_current_point(std::move(support_lines), nullptr);
            for (Slic3r::Polyline &line : support_lines)
                append_supported_anchor_polyline(zone_paths, std::move(line), input, lower_layer_aabb_tree);
        }

        for (Slic3r::coord_t distance_from_support = std::max<Slic3r::coord_t>(SCALED_EPSILON, input.overhang_spacing / 2);
             distance_from_support <= max_wave_distance + SCALED_EPSILON;
             distance_from_support += input.overhang_spacing) {
            const Slic3r::ExPolygons wave_area =
                Slic3r::offset_ex(optimized_lower_slices, double(distance_from_support));
            if (wave_area.empty())
                continue;

            Slic3r::Polylines wave_lines =
                Slic3r::intersection_pl(Slic3r::to_polylines(wave_area), zone_clip);
            wave_lines = reconnect_polylines(wave_lines, SCALED_EPSILON * 2, Slic3r::coord_t(SCALED_EPSILON));
            if (wave_lines.empty())
                continue;

            const Slic3r::Point *current_point =
                zone_paths.empty() ? nullptr : &zone_paths.back().last_point();
            wave_lines = order_wave_polylines_from_current_point(std::move(wave_lines), current_point);
            for (Slic3r::Polyline &line : wave_lines)
                append_wave_polyline(zone_paths, std::move(line), input, lower_layer_aabb_tree);
        }

        zone_paths.erase(
            std::remove_if(zone_paths.begin(), zone_paths.end(),
                           [](const Slic3r::ExtrusionPath &path) { return path.empty(); }),
            zone_paths.end());
        // A supported pre-line is only useful as the lead-in for real overhang
        // strokes. If clipping removed all overhang strokes from a tiny zone,
        // do not consume supported fill just to print that lead-in alone.
        if (!zone_paths.empty() && contains_overhang_path(zone_paths))
            extra_perimeters.push_back(std::move(zone_paths));
    }

    OverhangGenerationOutput out;
    out.extra_perimeters = std::move(extra_perimeters);
    const Slic3r::ExPolygons wave_filled_area =
        Slic3r::ensure_valid(coverage_for_extra_perimeters(out.extra_perimeters, infill_area, input));
    const Slic3r::ExPolygons residual_overhangs =
        Slic3r::ensure_valid(Slic3r::diff_ex(overhangs, wave_filled_area));

    // Any leftover inside the overhang domain is owned by this post-process:
    // tiny leftovers disappear, printable leftovers become square-flow gap
    // fill. In both cases the area is removed from later fill/gap-fill stages
    // so it cannot be printed twice.
    append_residual_gap_fill_paths(out.extra_perimeters, residual_overhangs, input, lower_layer_aabb_tree);
    out.filled_area = Slic3r::ensure_valid(Slic3r::union_ex(wave_filled_area, residual_overhangs));
    out.unfilled_area = Slic3r::ensure_valid(Slic3r::diff_ex(infill_area, out.filled_area));
    return out;
}

OverhangGenerationInput generation_input_for_island(const Print &print,
                                                    const Object &object,
                                                    const LayerIsland &island,
                                                    uint32_t layer_idx,
                                                    const c_flow &perimeter_flow,
                                                    const c_flow &external_flow)
{
    const Config region_config = island.region(0).print_region().config();
    const Config object_config = object.config();
    const int32_t perimeter_count = std::max(0, config_int_or(region_config, k_perimeters_key, 0));

    OverhangGenerationInput input;
    input.wave_flow = wave_flow_from_perimeter_flow(perimeter_flow);
    input.supported_anchor_attributes = supported_anchor_attributes_from_perimeter_flow(perimeter_flow);
    input.overhang_spacing = overhang_spacing_from_config(region_config, perimeter_flow);
    input.perimeter_depth = perimeter_count <= 0 ? 0 :
        external_flow.width + perimeter_flow.spacing * (perimeter_count - 1);
    input.lower_slices = lower_slices_for_island(island);

    // Object-local first layers do not receive extra overhang anchors. There
    // is no lower object material to grow from even if raft/support exists.
    if (layer_idx == 0 || (object_config.has(k_raft_layers_key) &&
                           layer_idx <= uint32_t(std::max(0, object_config.get(k_raft_layers_key).get_int()))))
        input.perimeter_depth = 0;

    (void)print;
    return input;
}

Slic3r::coord_t infill_overlap_for_island(const LayerIsland &island,
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
    return Slic3r::scale_i(config.get(k_infill_overlap_key).get_effective_value(ratio));
}

void append_native_copy(MutableExtrusionEntity &dst, const Slic3r::ExtrusionEntity &src)
{
    const uint32_t index =
        extrusion_insert_child_copy(dst.mutable_handle(), dst.child_count(), native_entity_handle(src));
    assert(!is_invalid_index(index));
    (void)index;
}

void append_extra_path(MutableExtrusionEntity dst, const Slic3r::ExtrusionPath &path)
{
    if (path.empty())
        return;
    append_native_copy(dst, path);
}

void prepend_extra_perimeter_zone_groups_to_root(storage_handle *storage,
                                                 MutableExtrusionEntity root,
                                                 const std::vector<Slic3r::ExtrusionPaths> &extra_perimeters)
{
    // The root prints the overhang wrapper first, then the original perimeter
    // tree. The wrapper itself is sortable between zones, while every zone is
    // locked so its waves stay ordered from support toward open air.
    StoredExtrusionEntity original(storage, root.readonly());
    StoredExtrusionEntity extra_zones(storage);

    for (const Slic3r::ExtrusionPaths &zone_paths : extra_perimeters) {
        if (zone_paths.empty())
            continue;

        StoredExtrusionEntity zone(storage);
        for (const Slic3r::ExtrusionPath &path : zone_paths)
            append_extra_path(zone.mutable_view(), path);
        if (!zone.empty()) {
            zone.disable_sort();
            zone.disable_reverse();
            const uint32_t idx = extra_zones.add_child(zone.mutable_view());
            assert(!is_invalid_index(idx));
            (void)idx;
        }
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

bool extra_perimeters_empty(const std::vector<Slic3r::ExtrusionPaths> &extra_perimeters)
{
    for (const Slic3r::ExtrusionPaths &paths : extra_perimeters)
        if (!paths.empty())
            return false;
    return true;
}

void publish_fill_areas(const run_ctx_post_perimeter_generation &ctx,
                        const LayerIsland &island,
                        const Slic3r::ExPolygons &fill_areas,
                        const Slic3r::ExPolygons &free_areas)
{
    const expolygon_collection_handle *fill_handle =
        reinterpret_cast<const expolygon_collection_handle *>(&fill_areas);
    const expolygon_collection_handle *free_handle =
        reinterpret_cast<const expolygon_collection_handle *>(&free_areas);
    const int32_t fill_ok = ctx.set_island_fill_areas(island.handle(), fill_handle);
    const int32_t free_ok = ctx.set_island_fill_free_areas(island.handle(), free_handle);
    assert(fill_ok != 0);
    assert(free_ok != 0);
    (void)fill_ok;
    (void)free_ok;
}

void update_fill_areas(const run_ctx_post_perimeter_generation &ctx,
                       const LayerIsland &island,
                       const OverhangGenerationOutput &generated,
                       Slic3r::coord_t infill_overlap)
{
    // The generated paths now occupy part of the infill area. Keep the strict
    // free area conservative, and rebuild the wider fill area with the same
    // overlap rule used by the post-perimeter pipeline.
    const Slic3r::ExPolygons fill_areas = native_collection(island.infill_areas());
    const Slic3r::ExPolygons free_areas = native_collection(island.infill_no_overlap_areas());
    const Slic3r::ExPolygons free_source = free_areas.empty() ? fill_areas : free_areas;

    Slic3r::ExPolygons next_fill_areas;
    Slic3r::ExPolygons next_free_areas = Slic3r::diff_ex(free_source, generated.filled_area);
    if (infill_overlap != 0) {
        next_fill_areas =
            Slic3r::intersection_ex(fill_areas, Slic3r::offset_ex(generated.unfilled_area, infill_overlap));
    } else {
        next_fill_areas = Slic3r::diff_ex(fill_areas, generated.filled_area);
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
        generation_input_for_island(print, object, island, layer_idx, perimeter_flow, external_flow);
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
        generate_extra_perimeters_over_overhangs_wave(enabled_area, input);
    if (extra_perimeters_empty(generated.extra_perimeters))
        return;

    if (settings.has_many_config(k_extra_perimeters_on_overhangs_key)) {
        const Slic3r::ExPolygons disabled_area = disabled_infill_area(infill_candidate, enabled_area);
        generated.unfilled_area = Slic3r::union_ex(generated.unfilled_area, disabled_area);
    }

    prepend_extra_perimeter_zone_groups_to_root(storage, root, generated.extra_perimeters);
    update_fill_areas(ctx, island, generated, infill_overlap_for_island(island, perimeter_flow, external_flow));
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
