///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "ExtraPerimetersOnOverhangs.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "libslic3r/AABBTreeLines.hpp"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_post_perimeter.h"
#include "libslic3r/Api/plugin/cpp/DataTreeViews.hpp"
#include "libslic3r/Api/plugin/cpp/ExtrusionViews.hpp"
#include "libslic3r/Api/plugin/cpp/RegionSettingsViews.hpp"
#include "libslic3r/BridgeDetector.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Geometry/ConvexHull.hpp"
#include "libslic3r/Line.hpp"
#include "libslic3r/Polyline.hpp"

namespace slic3r_api { namespace Perimeter { namespace ExtraPerimetersOnOverhangsPlugin {

namespace {

const char *k_extra_overhang_perimeters_id = "perimeter.post_process.extra_perimeters_on_overhangs";
const char *k_no_dependencies[] = { nullptr };
const raw_used_config_key k_used_config_keys[] = {
    { "extra_perimeters_on_overhangs", RAW_CO_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "bridged_infill_margin", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "bridge_angle", RAW_CO_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "bridge_precision", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "infill_overlap", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "overhangs_extrusion_spacing", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "perimeters", RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "raft_layers", RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE }
};
const char *k_extra_perimeters_on_overhangs_key = "extra_perimeters_on_overhangs";
const char *k_bridged_infill_margin_key = "bridged_infill_margin";
const char *k_bridge_angle_key = "bridge_angle";
const char *k_bridge_precision_key = "bridge_precision";
const char *k_infill_overlap_key = "infill_overlap";
const char *k_overhangs_extrusion_spacing_key = "overhangs_extrusion_spacing";
const char *k_perimeters_key = "perimeters";
const char *k_raft_layers_key = "raft_layers";

struct OverhangFlow
{
    Slic3r::ExtrusionAttributes attributes;
    Slic3r::coord_t width = 0;
    Slic3r::coord_t spacing = 0;
    float width_mm = 0.f;
    float spacing_mm = 0.f;
};

struct OverhangGenerationInput
{
    Slic3r::coord_t perimeter_depth = 0;
    Slic3r::coord_t anchors_size = 0;
    Slic3r::coord_t overhang_spacing = 0;
    Slic3r::coord_t bridge_precision = 0;
    int layer_id = 0;
    bool bridge_angle_enabled = false;
    double bridge_angle_rad = 0.;
    OverhangFlow overhang_flow;
    Slic3r::Polygons lower_slices;
};

struct OverhangGenerationOutput
{
    std::vector<Slic3r::ExtrusionPaths> extra_perimeters;
    Slic3r::ExPolygons filled_area;
    Slic3r::ExPolygons unfilled_area;
};

const Slic3r::ExPolygons &native_expolygons(const expolygon_collection_handle *handle)
{
    assert(handle != nullptr);
    return *reinterpret_cast<const Slic3r::ExPolygons *>(handle);
}

const Slic3r::ExPolygon &native_expolygon(const expolygon_handle *handle)
{
    assert(handle != nullptr);
    return *reinterpret_cast<const Slic3r::ExPolygon *>(handle);
}

const extrusion_entity_handle *native_entity_handle(const Slic3r::ExtrusionEntity &entity)
{
    return reinterpret_cast<const extrusion_entity_handle *>(&entity);
}

Slic3r::ExPolygons native_collection(const ExPolygonCollection &collection)
{
    if (collection.empty())
        return {};
    return native_expolygons(collection.handle());
}

Slic3r::coord_t scaled_effective_value(const Config &config,
                                       const char *key,
                                       double ratio,
                                       Slic3r::coord_t fallback = 0)
{
    if (!config.has(key))
        return fallback;
    ConfigOption option = config.get(key);
    if (!option.is_enabled())
        return fallback;
    return Slic3r::scale_i(option.get_effective_value(ratio));
}

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

double config_float_or(const Config &config, const char *key, double fallback)
{
    return config.has(key) ? config.get(key).get_float() : fallback;
}

Slic3r::coord_t overhang_spacing_from_config(const Config &config, const c_flow &perimeter_flow)
{
    const Slic3r::coord_t configured = scaled_float_or_percent_value(
        config, k_overhangs_extrusion_spacing_key, unscaled(perimeter_flow.nozzle_diameter), 0);
    return configured > 0 ? configured : perimeter_flow.spacing;
}

OverhangFlow overhang_flow_from_perimeter_flow(const c_flow &perimeter_flow)
{
    // The legacy algorithm uses the region overhang/bridge flow. The current C
    // API does not expose that dedicated flow yet, so this post-process keeps
    // the perimeter cross-section and marks the extrusion role/properties as an
    // overhang. G-code flow/speed modifiers can still interpret the overhang
    // property on the generated paths.
    OverhangFlow out;
    out.width = perimeter_flow.width;
    out.spacing = perimeter_flow.spacing;
    out.width_mm = float(unscaled(perimeter_flow.width));
    out.spacing_mm = float(unscaled(perimeter_flow.spacing));
    out.attributes = Slic3r::ExtrusionAttributes(
        Slic3r::ExtrusionRole::OverhangPerimeter,
        Slic3r::ExtrusionFlow(perimeter_flow.mm3_per_mm,
                              float(unscaled(perimeter_flow.width)),
                              float(unscaled(perimeter_flow.height))));
    // These generated anchors already choose their start point from the
    // supported area. Seam placement would be allowed to move the start onto an
    // unsupported span, so the paths opt out of seam candidate extraction.
    out.attributes.no_seam = true;
    return out;
}

Slic3r::Polygons lower_slices_for_island(const LayerIsland &island)
{
    Slic3r::ExPolygons lower_expolygons;
    const std::vector<LayerIsland> lower_islands = island.lower_islands();
    lower_expolygons.reserve(lower_islands.size());
    for (const LayerIsland &lower_island : lower_islands)
        lower_expolygons.push_back(native_expolygon(lower_island.slice().handle()));

    if (lower_expolygons.empty())
        return {};
    return Slic3r::to_polygons(Slic3r::union_ex(lower_expolygons));
}

Slic3r::ExPolygons enabled_infill_area(storage_handle *storage,
                                       const Slic3r::ExPolygons &candidate,
                                       const RegionSettings &settings)
{
    // RegionSettings may describe one value for the whole island or several
    // non-overlapping clips. Convert that setting state into the part of the
    // infill domain where this module is allowed to add overhang anchors.
    if (!settings.has_many_config(k_extra_perimeters_on_overhangs_key)) {
        if (!settings.get_solo_config(k_extra_perimeters_on_overhangs_key)
                 .get_bool(k_extra_perimeters_on_overhangs_key))
            return {};
        return candidate;
    }

    Slic3r::ExPolygons enabled;
    const RegionSettings::AreaMap &areas = settings.get_areas(k_extra_perimeters_on_overhangs_key);
    for (const auto &[setting_value, setting_clip] : areas) {
        if (!setting_value.get_bool(k_extra_perimeters_on_overhangs_key))
            continue;

        if (setting_clip.is_accept_all()) {
            enabled = candidate;
            break;
        }

        const Slic3r::ExPolygons clip = native_collection(setting_clip.expolygons());
        const Slic3r::ExPolygons clipped = Slic3r::intersection_ex(candidate, clip);
        enabled.insert(enabled.end(), clipped.begin(), clipped.end());
    }

    (void)storage;
    return Slic3r::union_ex(enabled);
}

Slic3r::ExPolygons disabled_infill_area(const Slic3r::ExPolygons &candidate,
                                        const Slic3r::ExPolygons &enabled)
{
    // Disabled clips are not processed, but they still belong to the island's
    // infill domain. They are unioned back into the unfilled area so later
    // infill steps do not lose material in regions where the module is off.
    if (candidate.empty() || enabled.empty())
        return candidate;
    return Slic3r::diff_ex(candidate, enabled);
}

bool paths_touch(const Slic3r::ExtrusionPath &path_one,
                 const Slic3r::ExtrusionPath &path_two,
                 Slic3r::coordf_t limit_distance)
{
    // The sort step builds a dependency graph between overhang strokes. Two
    // paths that touch are treated as printable in sequence, because the first
    // path can anchor the second path into already supported material.
    Slic3r::Polyline discrete_polyline_one = path_one.as_polyline().to_polyline();
    Slic3r::Polyline discrete_polyline_two = path_two.as_polyline().to_polyline();
    Slic3r::AABBTreeLines::LinesDistancer<Slic3r::Line> lines_two{ discrete_polyline_two.lines() };
    for (size_t pt_idx = 0; pt_idx < path_one.polyline().size(); pt_idx++) {
        if (lines_two.distance_from_lines<false>(discrete_polyline_one.points[pt_idx]) < limit_distance)
            return true;
    }
    Slic3r::AABBTreeLines::LinesDistancer<Slic3r::Line> lines_one{ discrete_polyline_one.lines() };
    for (size_t pt_idx = 0; pt_idx < path_two.polyline().size(); pt_idx++) {
        if (lines_one.distance_from_lines<false>(discrete_polyline_two.points[pt_idx]) < limit_distance)
            return true;
    }
    return false;
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

    // Generated loops often arrive as small pieces from boolean clipping.
    // Reconnect touching endpoints before creating ExtrusionPath objects so the
    // G-code planner receives long printable strokes instead of many crumbs.
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
                // The clipping output may leave a small gap between two pieces
                // that should be printed as one stroke. Polyline::append(Polyline)
                // is only valid when the endpoints already coincide, so append
                // the raw point list here: it keeps a real connector segment
                // when there is a gap and skips the duplicate point when the
                // pieces already touch.
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
    for (auto &[source_idx, polyline] : connected) {
        (void)source_idx;
        result.push_back(std::move(polyline));
    }

    Slic3r::ensure_valid(result, resolution);
    return result;
}

void orient_extra_perimeter_from_support(
    Slic3r::ExtrusionPath &path,
    const Slic3r::AABBTreeLines::LinesDistancer<Slic3r::Line> &lower_layer_aabb_tree)
{
    if (path.empty())
        return;

    // The extra path ordering has a physical meaning: the first printed point
    // should be the part that is closest to already supported material. Closed
    // paths need a rotation, while open paths only need their direction chosen.
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

Slic3r::ExtrusionPaths sort_extra_perimeters(const Slic3r::ExtrusionPaths &extra_perims,
                                             int index_of_first_unanchored,
                                             Slic3r::coordf_t extrusion_spacing)
{
    if (extra_perims.empty())
        return {};

    // Every path before index_of_first_unanchored is already touching lower
    // material. Later paths should be printed only once they are connected to a
    // processed path, otherwise unsupported strokes may be emitted in mid-air.
    std::vector<std::unordered_set<size_t>> dependencies(extra_perims.size());
    for (size_t path_idx = 0; path_idx < extra_perims.size(); path_idx++)
        for (size_t prev_path_idx = 0; prev_path_idx < path_idx; prev_path_idx++)
            if (paths_touch(extra_perims[path_idx], extra_perims[prev_path_idx], extrusion_spacing * 1.5f))
                dependencies[path_idx].insert(prev_path_idx);

    std::vector<bool> processed(extra_perims.size(), false);
    for (int path_idx = 0; path_idx < index_of_first_unanchored; path_idx++)
        processed[path_idx] = true;

    // Dependencies mean "print this after a touching already-supported path".
    // Flip unresolved dependencies when needed so unsupported strokes still get
    // printed in an order that tends to grow out of anchored material.
    for (size_t iteration = size_t(index_of_first_unanchored); iteration < extra_perims.size(); iteration++) {
        bool changed = false;
        for (size_t path_idx = size_t(index_of_first_unanchored); path_idx < extra_perims.size(); path_idx++) {
            if (processed[path_idx])
                continue;
            std::unordered_set<size_t>::iterator processed_dep = std::find_if(
                dependencies[path_idx].begin(), dependencies[path_idx].end(),
                [&processed](size_t dep) { return processed[dep]; });
            if (processed_dep == dependencies[path_idx].end())
                continue;

            for (std::unordered_set<size_t>::iterator it = dependencies[path_idx].begin();
                 it != dependencies[path_idx].end();) {
                if (!processed[*it]) {
                    dependencies[*it].insert(path_idx);
                    dependencies[path_idx].erase(it++);
                } else {
                    ++it;
                }
            }
            processed[path_idx] = true;
            changed = true;
        }
        if (!changed)
            break;
    }

    Slic3r::Point current_point = extra_perims.begin()->first_point();
    Slic3r::ExtrusionPaths sorted_paths;
    const size_t invalid_idx = size_t(-1);
    size_t next_idx = invalid_idx;
    bool reverse = false;
    while (true) {
        if (next_idx == invalid_idx) {
            double dist = std::numeric_limits<double>::max();
            for (size_t path_idx = 0; path_idx < extra_perims.size(); path_idx++) {
                if (!dependencies[path_idx].empty())
                    continue;
                const Slic3r::ExtrusionPath &path = extra_perims[path_idx];
                const double dist_a = (path.first_point() - current_point).cast<double>().squaredNorm();
                if (dist_a < dist) {
                    dist = dist_a;
                    next_idx = path_idx;
                    reverse = false;
                }
                const double dist_b = (path.last_point() - current_point).cast<double>().squaredNorm();
                if (dist_b < dist) {
                    dist = dist_b;
                    next_idx = path_idx;
                    reverse = true;
                }
            }
            if (next_idx == invalid_idx)
                break;
        } else {
            Slic3r::ExtrusionPath path = extra_perims[next_idx];
            if (reverse)
                path.reverse();
            sorted_paths.push_back(path);
            assert(dependencies[next_idx].empty());
            dependencies[next_idx].insert(invalid_idx);
            current_point = sorted_paths.back().last_point();
            for (size_t path_idx = 0; path_idx < extra_perims.size(); path_idx++)
                dependencies[path_idx].erase(next_idx);

            double dist = std::numeric_limits<double>::max();
            next_idx = invalid_idx;
            for (size_t path_idx = next_idx + 1; path_idx < extra_perims.size(); path_idx++) {
                if (!dependencies[path_idx].empty())
                    continue;
                const Slic3r::ExtrusionPath &next_path = extra_perims[path_idx];
                const double dist_a = (next_path.first_point() - current_point).cast<double>().squaredNorm();
                if (dist_a < dist) {
                    dist = dist_a;
                    next_idx = path_idx;
                    reverse = false;
                }
                const double dist_b = (next_path.last_point() - current_point).cast<double>().squaredNorm();
                if (dist_b < dist) {
                    dist = dist_b;
                    next_idx = path_idx;
                    reverse = true;
                }
            }
            if (dist > Slic3r::scale_d(5.0))
                next_idx = invalid_idx;
        }
    }

    Slic3r::ExtrusionPaths reconnected;
    reconnected.reserve(sorted_paths.size());
    for (Slic3r::ExtrusionPath &path : sorted_paths) {
        if (!reconnected.empty() &&
            (reconnected.back().last_point() - path.first_point()).cast<double>().squaredNorm() <
                extrusion_spacing * extrusion_spacing * 4.0) {
            if (reconnected.back().last_point() != path.first_point() &&
                reconnected.back().last_point().coincides_with_epsilon(path.first_point())) {
                path.polyline().set_front(reconnected.back().last_point());
                if (path.polyline().front().coincides_with_epsilon(path.polyline().get_point(1))) {
                    path.polyline().pop_front();
                    path.polyline().set_front(reconnected.back().last_point());
                }
            } else if (reconnected.back().last_point() != path.first_point()) {
                reconnected.back().polyline().append(path.polyline().front());
            }
            if (path.length() > SCALED_EPSILON)
                reconnected.back().polyline().append(path.polyline());
        } else {
            reconnected.push_back(path);
        }
    }

    Slic3r::ExtrusionPaths filtered;
    filtered.reserve(reconnected.size());
    for (Slic3r::ExtrusionPath &path : reconnected)
        if (path.length() > 3 * extrusion_spacing)
            filtered.push_back(std::move(path));

    return filtered;
}

void append_overhang_paths(Slic3r::ExtrusionPaths &dst,
                           Slic3r::Polylines &&polylines,
                           const OverhangGenerationInput &input)
{
    Slic3r::extrusion_paths_append(
        dst,
        reconnect_polylines(polylines, input.overhang_spacing, Slic3r::coord_t(SCALED_EPSILON)),
        input.overhang_flow.attributes,
        Slic3r::ExtrusionPropertyOverhang(1, 2, 0, true, true, false, false),
        false);
}

OverhangGenerationOutput generate_extra_perimeters_over_overhangs(
    const Slic3r::ExPolygon &island,
    const Slic3r::ExPolygons &infill_area,
    const OverhangGenerationInput &input)
{
    if (infill_area.empty() || input.lower_slices.empty() || input.perimeter_depth <= 0)
        return {};

    const Slic3r::BoundingBox infill_area_bb =
        Slic3r::get_extents(infill_area).inflated(SCALED_EPSILON + input.anchors_size);
    const Slic3r::Polygons optimized_lower_slices =
        Slic3r::ClipperUtils::clip_clipper_polygons_with_subject_bbox(input.lower_slices, infill_area_bb);
    const Slic3r::ExPolygons overhangs = Slic3r::diff_ex(infill_area, optimized_lower_slices);
    if (overhangs.empty())
        return {};

    Slic3r::AABBTreeLines::LinesDistancer<Slic3r::Line> lower_layer_aabb_tree{
        Slic3r::to_lines(optimized_lower_slices)
    };
    const Slic3r::Polygons anchors = Slic3r::intersection(Slic3r::ExPolygons{ island }, optimized_lower_slices);
    const Slic3r::ExPolygons anchors_no_overhangs = Slic3r::diff_ex(anchors, overhangs);
    const Slic3r::ExPolygons inset_anchors = Slic3r::diff_ex(
        anchors,
        Slic3r::offset_ex(overhangs, input.anchors_size, Slic3r::ClipperLib::jtSquare, 0.));
    const Slic3r::ExPolygons inset_overhang_area = Slic3r::diff_ex(infill_area, inset_anchors);

    Slic3r::ExPolygons inset_overhang_area_left_unfilled;
    std::vector<Slic3r::ExtrusionPaths> extra_perimeters;

    // Each disconnected overhang area is processed independently. The loop
    // builds clipped overhang perimeters until the remaining area is either
    // bridgeable or too small, then returns both the generated paths and the
    // area consumed by them.
    const Slic3r::ExPolygons overhang_regions = Slic3r::union_ex(inset_overhang_area);
    for (const Slic3r::ExPolygon &overhang : overhang_regions) {
        const Slic3r::ExPolygons overhang_to_cover = { overhang };
        const Slic3r::ExPolygons expanded_overhang_to_cover =
            Slic3r::offset_ex(overhang_to_cover, 1.1 * input.overhang_spacing);
        Slic3r::ExPolygons shrinked_overhang_to_cover =
            Slic3r::offset_ex(overhang_to_cover, -0.1 * input.overhang_spacing);

        const Slic3r::Polygons real_overhang = Slic3r::intersection(overhang_to_cover, overhangs);
        if (real_overhang.empty()) {
            inset_overhang_area_left_unfilled.insert(
                inset_overhang_area_left_unfilled.end(), overhang_to_cover.begin(), overhang_to_cover.end());
            continue;
        }

        Slic3r::ExtrusionPaths &overhang_region = extra_perimeters.emplace_back();
        const Slic3r::ExPolygons anchoring = Slic3r::intersection_ex(expanded_overhang_to_cover, inset_anchors);
        Slic3r::ExPolygons perimeter_polygon = Slic3r::offset2_ex(
            Slic3r::union_ex(Slic3r::offset_ex(overhang_to_cover, 0.1 * input.overhang_spacing), anchoring),
            -input.overhang_spacing * (0.1 + 0.5 + 0.1),
            input.overhang_spacing * 0.1);

        const Slic3r::Polygon anchoring_convex_hull =
            Slic3r::Geometry::convex_hull(Slic3r::intersection_ex(expanded_overhang_to_cover, anchors_no_overhangs));
        double unbridgeable_area = Slic3r::area(Slic3r::diff(real_overhang, Slic3r::Polygons{ anchoring_convex_hull }));
        std::tuple<Slic3r::Vec2d, double> bridge_direction =
            Slic3r::detect_bridging_direction(real_overhang, anchors);
        double unsupp_dist = std::get<1>(bridge_direction);

#ifdef _DEBUG
        // The cheap detector is enough for most cases. In debug builds, keep
        // the more expensive legacy safety check for suspicious areas so local
        // development catches bridge-classification regressions earlier.
        if (unbridgeable_area > 0.2 * Slic3r::area(real_overhang) ||
            unsupp_dist > Slic3r::total_length(real_overhang) * 0.2) {
            Slic3r::BridgeDetector detector(
                Slic3r::union_ex(real_overhang),
                Slic3r::union_ex(anchors),
                input.overhang_flow.spacing,
                input.bridge_precision,
                input.layer_id);
            double bridge_angle = 0.;
            if (input.bridge_angle_enabled) {
                bridge_angle = input.bridge_angle_rad;
            } else if (detector.detect_angle()) {
                bridge_angle = detector.angle;
            }

            const Slic3r::Polylines unsupported_lines = detector.unsupported_edges(bridge_angle);
            unsupp_dist = 0.;
            for (const Slic3r::Polyline &polyline : unsupported_lines)
                unsupp_dist += polyline.length();
            unbridgeable_area = Slic3r::area(Slic3r::diff(real_overhang, detector.coverage(bridge_angle)));
        }
#endif

        if (unbridgeable_area < 0.2 * Slic3r::area(real_overhang) &&
            unsupp_dist < Slic3r::total_length(real_overhang) * 0.2) {
            inset_overhang_area_left_unfilled.insert(
                inset_overhang_area_left_unfilled.end(), overhang_to_cover.begin(), overhang_to_cover.end());
            perimeter_polygon.clear();
        } else {
            shrinked_overhang_to_cover = Slic3r::diff_ex(
                shrinked_overhang_to_cover,
                Slic3r::offset_ex(inset_anchors, input.overhang_spacing * 0.5));

            int continuation_loops = 2;
            while (continuation_loops >= 0) {
                const Slic3r::ExPolygons previous = perimeter_polygon;
                Slic3r::Polylines perimeter =
                    Slic3r::intersection_pl(Slic3r::to_polylines(perimeter_polygon), shrinked_overhang_to_cover);

                perimeter_polygon = Slic3r::union_ex(perimeter_polygon, anchoring);
                perimeter_polygon = Slic3r::intersection_ex(
                    Slic3r::offset_ex(perimeter_polygon, -input.overhang_spacing),
                    expanded_overhang_to_cover);

                if (perimeter_polygon.empty()) {
                    const Slic3r::ExPolygons shrinked = Slic3r::intersection_ex(
                        Slic3r::offset_ex(previous, -0.3 * input.overhang_spacing),
                        expanded_overhang_to_cover);
                    if (!shrinked.empty())
                        append_overhang_paths(overhang_region, std::move(perimeter), input);

                    Slic3r::Polylines fills;
                    const Slic3r::ExPolygons gap = shrinked.empty() ?
                        Slic3r::offset_ex(previous, input.overhang_spacing * 0.5) :
                        shrinked;
                    for (const Slic3r::ExPolygon &gap_part : gap)
                        gap_part.medial_axis(0.75 * input.overhang_flow.width,
                                             3.0 * input.overhang_spacing,
                                             fills);
                    if (!fills.empty()) {
                        fills = Slic3r::intersection_pl(fills, shrinked_overhang_to_cover);
                        append_overhang_paths(overhang_region, std::move(fills), input);
                    }
                    break;
                }

                append_overhang_paths(overhang_region, std::move(perimeter), input);
                if (Slic3r::intersection(perimeter_polygon, real_overhang).empty())
                    continuation_loops--;
                if (previous == perimeter_polygon)
                    break;
            }

            perimeter_polygon = Slic3r::offset_ex(perimeter_polygon, 0.5 * input.overhang_spacing);
            perimeter_polygon = Slic3r::union_ex(perimeter_polygon, anchoring);
            inset_overhang_area_left_unfilled.insert(
                inset_overhang_area_left_unfilled.end(), perimeter_polygon.begin(), perimeter_polygon.end());

            overhang_region.erase(
                std::remove_if(overhang_region.begin(), overhang_region.end(),
                               [](const Slic3r::ExtrusionPath &path) { return path.empty(); }),
                overhang_region.end());

            if (!overhang_region.empty()) {
                Slic3r::Polyline discrete_polyline = overhang_region.front().polyline().to_polyline();
                const bool first_overhang_is_closed_and_anchored =
                    overhang_region.front().first_point() == overhang_region.front().last_point() &&
                    !Slic3r::intersection_pl(discrete_polyline, optimized_lower_slices).empty();

                const auto is_anchored =
                    [&lower_layer_aabb_tree](const Slic3r::ExtrusionPath &path) {
                        return lower_layer_aabb_tree.distance_from_lines<true>(path.first_point()) <= 0 ||
                               lower_layer_aabb_tree.distance_from_lines<true>(path.last_point()) <= 0;
                    };

                if (!first_overhang_is_closed_and_anchored) {
                    std::reverse(overhang_region.begin(), overhang_region.end());
                }

                for (Slic3r::ExtrusionPath &path : overhang_region)
                    orient_extra_perimeter_from_support(path, lower_layer_aabb_tree);

                const Slic3r::ExtrusionPaths::iterator first_unanchored =
                    std::stable_partition(overhang_region.begin(), overhang_region.end(), is_anchored);
                const int index_of_first_unanchored = int(first_unanchored - overhang_region.begin());
                overhang_region =
                    sort_extra_perimeters(overhang_region, index_of_first_unanchored, input.overhang_spacing);
            }
        }
    }

    inset_overhang_area_left_unfilled = Slic3r::union_ex(inset_overhang_area_left_unfilled);

    OverhangGenerationOutput out;
    out.extra_perimeters = std::move(extra_perimeters);
    out.filled_area = Slic3r::ensure_valid(Slic3r::diff_ex(inset_overhang_area, inset_overhang_area_left_unfilled));
    out.unfilled_area = Slic3r::ensure_valid(Slic3r::union_ex(inset_anchors, inset_overhang_area_left_unfilled));
    return out;
}

OverhangGenerationInput generation_input_for_island(const Print &print,
                                                    const Object &object,
                                                    const LayerIsland &island,
                                                    uint32_t layer_idx,
                                                    const c_flow &perimeter_flow,
                                                    const c_flow &external_flow)
{
    // Gather the small immutable bundle needed by the copied legacy algorithm.
    // The C API currently exposes perimeter flows but not the dedicated legacy
    // overhang flow, so overhang paths reuse the perimeter cross-section and
    // carry an overhang role/property for downstream speed/flow logic.
    const Config region_config = island.region(0).print_region().config();
    const Config print_config = print.config();
    const Config object_config = object.config();
    const int32_t perimeter_count = std::max(0, config_int_or(region_config, k_perimeters_key, 0));

    OverhangGenerationInput input;
    input.layer_id = int(layer_idx);
    input.overhang_flow = overhang_flow_from_perimeter_flow(perimeter_flow);
    input.overhang_spacing = overhang_spacing_from_config(region_config, perimeter_flow);
    input.perimeter_depth = perimeter_count <= 0 ? 0 :
        external_flow.width + perimeter_flow.spacing * (perimeter_count - 1);
    input.anchors_size = std::min(
        scaled_float_or_percent_value(region_config, k_bridged_infill_margin_key, unscaled(external_flow.width), 0),
        input.perimeter_depth);
    input.bridge_precision =
        scaled_float_or_percent_value(print_config, k_bridge_precision_key, unscaled(input.overhang_flow.spacing), 0);
    if (input.bridge_precision <= 0)
        input.bridge_precision = std::max<Slic3r::coord_t>(SCALED_EPSILON, input.overhang_flow.spacing / 4);
    input.bridge_angle_enabled = region_config.has(k_bridge_angle_key) &&
        region_config.get(k_bridge_angle_key).is_enabled();
    input.bridge_angle_rad =
        Slic3r::Geometry::deg2rad(config_float_or(region_config, k_bridge_angle_key, 0.));
    input.lower_slices = lower_slices_for_island(island);

    // Keep the old "do not add overhang extras on the first printable object
    // layer" guard. The C layer handle currently exposes traversal index rather
    // than native Layer::id(), so use the object-local layer index.
    if (layer_idx == 0 || (object_config.has(k_raft_layers_key) &&
                           layer_idx <= uint32_t(std::max(0, object_config.get(k_raft_layers_key).get_int()))))
        input.perimeter_depth = 0;

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

    // A closed overhang anchor is still stored as a path, not as a loop. Loops
    // are split by the seam placer, while this anchor must keep the start/end
    // chosen by the overhang ordering algorithm.
    append_native_copy(dst, path);
}

void prepend_extra_perimeters_to_root(storage_handle *storage,
                                      MutableExtrusionEntity root,
                                      const std::vector<Slic3r::ExtrusionPaths> &extra_perimeters)
{
    // Preserve the current perimeter bucket before clearing it. The rebuilt
    // root is an unsortable collection: generated overhang anchors first,
    // followed by the original normal perimeter children in their old order.
    StoredExtrusionEntity original(storage, root.readonly());
    root.clear_content();

    for (const Slic3r::ExtrusionPaths &paths : extra_perimeters)
        for (const Slic3r::ExtrusionPath &path : paths)
            append_extra_path(root, path);

    if (original.child_count() > 0) {
        while (original.child_count() > 0)
            root.move_child_from(root.child_count(), original.mutable_view(), 0);
    } else if (!original.empty()) {
        root.add_child(original.mutable_view());
    }

    // Adding the first child turns an empty entity into a regular collection,
    // and the host helper defaults such collections to sortable. Force the
    // final ordering contract after all children are in place: generated
    // overhang anchors first, then the normal perimeter tree.
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
    // Extra perimeter paths consume part of the old infill job. The strict
    // free area loses the generated extrusion coverage; the wider fill/anchor
    // area either follows the legacy overlap expansion or subtracts the same
    // coverage when no infill/perimeter overlap is requested.
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
    // The module is an island-level post-process. It needs a generated
    // perimeter bucket, a remaining infill domain, and a lower island graph;
    // otherwise there is either nothing to anchor or no overhang to classify.
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

    const Slic3r::ExPolygons infill_candidate =
        island.infill_no_overlap_areas().empty() ?
        native_collection(island.infill_areas()) :
        native_collection(island.infill_no_overlap_areas());
    Slic3r::ExPolygons enabled_area = enabled_infill_area(storage, infill_candidate, settings);
    if (enabled_area.empty())
        return;

    OverhangGenerationOutput generated =
        generate_extra_perimeters_over_overhangs(native_expolygon(island.slice().handle()), enabled_area, input);
    if (extra_perimeters_empty(generated.extra_perimeters))
        return;

    if (settings.has_many_config(k_extra_perimeters_on_overhangs_key)) {
        const Slic3r::ExPolygons disabled_area = disabled_infill_area(infill_candidate, enabled_area);
        generated.unfilled_area = Slic3r::union_ex(generated.unfilled_area, disabled_area);
    }

    prepend_extra_perimeters_to_root(storage, root, generated.extra_perimeters);
    update_fill_areas(ctx, island, generated, infill_overlap_for_island(island, perimeter_flow, external_flow));
}

} // namespace

ExtraPerimetersOnOverhangs &
ExtraPerimetersOnOverhangs::instance(orchestrator_handle *orch)
{
    static ExtraPerimetersOnOverhangs s_instance(orch);
    return s_instance;
}

const char *ExtraPerimetersOnOverhangs::id_impl() const noexcept
{
    return k_extra_overhang_perimeters_id;
}

const char *ExtraPerimetersOnOverhangs::name_impl() const noexcept
{
    return "Extra perimeters on overhangs";
}

const char *ExtraPerimetersOnOverhangs::description_impl() const noexcept
{
    return "Adds anchored perimeter paths under unsupported overhang areas.";
}

slicing_step_t ExtraPerimetersOnOverhangs::step_impl() const noexcept
{
    return STEP_POST_PERIMETER;
}

const char *const *ExtraPerimetersOnOverhangs::dependencies_impl() const noexcept
{
    return k_no_dependencies;
}

int32_t ExtraPerimetersOnOverhangs::priority_impl() const noexcept
{
    return -20;
}

int32_t ExtraPerimetersOnOverhangs::used_config_keys(raw_used_config_key *keys) const noexcept
{
    if (keys != nullptr)
        for (uint32_t idx = 0; idx < sizeof(k_used_config_keys) / sizeof(k_used_config_keys[0]); ++idx)
            keys[idx] = k_used_config_keys[idx];
    return int32_t(sizeof(k_used_config_keys) / sizeof(k_used_config_keys[0]));
}

const char *ExtraPerimetersOnOverhangs::progress_message_format_impl() const noexcept
{
    return "Extra overhang perimeters: %u / %u layers";
}

void ExtraPerimetersOnOverhangs::setup_run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_post_perimeter_generation *ctx = plugin_ctx_as_post_perimeter_generation(run_ctx);
    if (ctx != nullptr && ctx->object != nullptr)
        progress().add_max(Object(ctx->object).layer_count());
}

void ExtraPerimetersOnOverhangs::run_impl(const plugin_run_context *run_ctx) const
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
    for (uint32_t layer_idx = 0; layer_idx < object.layer_count(); ++layer_idx) {
        throw_if_cancelled(run_ctx);
        const Layer layer = object.layer(layer_idx);
        for (uint32_t island_idx = 0; island_idx < layer.island_count(); ++island_idx)
            process_island(*ctx, run_ctx->plugin_storage, print, object, layer.island(island_idx), layer_idx);
        progress().increment();
    }
}

void register_extra_perimeters_on_overhangs_plugin(orchestrator_handle *orch)
{
    orchestrator_register_plugin(orch, ExtraPerimetersOnOverhangs::instance(orch).c_instance());
}

}}} // namespace slic3r_api::Perimeter::ExtraPerimetersOnOverhangsPlugin

#ifdef EXTRA_PERIMETERS_ON_OVERHANGS_PLUGIN_DLL
extern "C" void register_plugin(orchestrator_handle *orch)
{
    slic3r_api::Perimeter::ExtraPerimetersOnOverhangsPlugin::
        register_extra_perimeters_on_overhangs_plugin(orch);
}
#endif // EXTRA_PERIMETERS_ON_OVERHANGS_PLUGIN_DLL
