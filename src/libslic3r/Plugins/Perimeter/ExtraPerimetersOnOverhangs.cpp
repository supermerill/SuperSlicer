///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "ExtraPerimetersOnOverhangs.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <tuple>
#include <unordered_set>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_post_perimeter.h"
#include "libslic3r/Api/plugin/cpp/BridgeDetectorViews.hpp"
#include "libslic3r/Api/plugin/cpp/ClipperViews.hpp"
#include "libslic3r/Api/plugin/cpp/DataTreeViews.hpp"
#include "libslic3r/Api/plugin/cpp/ExtrusionViews.hpp"
#include "libslic3r/Api/plugin/cpp/LineDistancer.hpp"
#include "libslic3r/Api/plugin/cpp/RegionSettingsViews.hpp"

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

constexpr double k_pi = 3.1415926535897932384626433832795;

double deg_to_rad(double degrees)
{
    return degrees * k_pi / 180.;
}

struct OverhangFlow
{
    EPropertyAttributes attributes = {};
    coord_t width = 0;
    coord_t spacing = 0;
    float width_mm = 0.f;
    float spacing_mm = 0.f;
};

struct OverhangGenerationInput
{
    explicit OverhangGenerationInput(storage_handle *storage) : lower_slices(storage) {}

    coord_t perimeter_depth = 0;
    coord_t anchors_size = 0;
    coord_t overhang_spacing = 0;
    coord_t bridge_precision = 0;
    int layer_id = 0;
    bool bridge_angle_enabled = false;
    double bridge_angle_rad = 0.;
    OverhangFlow overhang_flow;
    StoredExPolygonCollection lower_slices;
};

struct GeneratedPath
{
    explicit GeneratedPath(storage_handle *storage) : entity(storage) {}
    GeneratedPath(storage_handle *storage, const Polyline &polyline, const EPropertyAttributes &attributes)
        : entity(storage)
    {
        entity.set(polyline);
        entity.get_or_add_property<EPropertyAttributes>() = attributes;
        entity.disable_reverse();
    }

    GeneratedPath(GeneratedPath &&) noexcept = default;
    GeneratedPath &operator=(GeneratedPath &&) noexcept = default;
    GeneratedPath(const GeneratedPath &) = delete;
    GeneratedPath &operator=(const GeneratedPath &) = delete;

    StoredExtrusionEntity entity;
};

struct OverhangGenerationOutput
{
    explicit OverhangGenerationOutput(storage_handle *storage) : filled_area(storage), unfilled_area(storage) {}

    std::vector<std::vector<GeneratedPath>> extra_perimeters;
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

double config_float_or(const Config &config, const char *key, double fallback)
{
    return config.has(key) ? config.get(key).get_float() : fallback;
}

coord_t overhang_spacing_from_config(const Config &config, const c_flow &perimeter_flow)
{
    const coord_t configured = scaled_float_or_percent_value(
        config, k_overhangs_extrusion_spacing_key, unscaled(perimeter_flow.nozzle_diameter), 0);
    return configured > 0 ? configured : perimeter_flow.spacing;
}

coord_t minimum_printable_split_length(coordf_t spacing)
{
    return std::max<coord_t>(coord_t(SCALED_EPSILON), coord_t(spacing / 10.0));
}

double squared_distance(c_point lhs, c_point rhs)
{
    const double dx = double(lhs.x) - double(rhs.x);
    const double dy = double(lhs.y) - double(rhs.y);
    return dx * dx + dy * dy;
}

bool same_point(c_point lhs, c_point rhs)
{
    return lhs.x == rhs.x && lhs.y == rhs.y;
}

OverhangFlow overhang_flow_from_perimeter_flow(const c_flow &perimeter_flow)
{
    // These generated paths are geometric anchors under overhang areas. They
    // deliberately stay normal internal perimeters: DetectOverhang later
    // decides which spans are really unsupported and applies overhang speed or
    // flow metadata.
    OverhangFlow out = {};
    out.width = perimeter_flow.width;
    out.spacing = perimeter_flow.spacing;
    out.width_mm = float(unscaled(perimeter_flow.width));
    out.spacing_mm = float(unscaled(perimeter_flow.spacing));
    out.attributes.extrusion_role(RAW_EXTRUSION_ROLE_PERIMETER)
        .mm3_per_mm(perimeter_flow.mm3_per_mm)
        .width(float(unscaled(perimeter_flow.width)))
        .height(float(unscaled(perimeter_flow.height)));
    return out;
}

StoredPolyline polyline_from_points(storage_handle *storage, const std::vector<c_point> &points)
{
    StoredPolyline out(storage);
    if (!points.empty())
        out.insert_array(0, points.data(), static_cast<uint32_t>(points.size()));
    return out;
}

StoredPolyline polyline_from_path(storage_handle *storage, const GeneratedPath &path)
{
    return polyline_from_points(storage, path.entity.points());
}

void append_points_to_polyline(StoredPolyline &dst, const std::vector<c_point> &src)
{
    if (src.empty())
        return;
    const uint32_t insert_idx = dst.size();
    const uint32_t skip = insert_idx > 0 && same_point(dst.back(), src.front()) ? 1 : 0;
    if (skip < src.size())
        dst.insert_array(insert_idx, src.data() + skip, static_cast<uint32_t>(src.size() - skip));
}

void append_points_to_path(GeneratedPath &dst, const GeneratedPath &src)
{
    std::vector<c_point> points = dst.entity.points();
    const std::vector<c_point> src_points = src.entity.points();
    const size_t skip = !points.empty() && !src_points.empty() && same_point(points.back(), src_points.front()) ? 1 : 0;
    points.insert(points.end(), src_points.begin() + skip, src_points.end());
    dst.entity.set_points(points);
}

void set_path_front(GeneratedPath &path, c_point point)
{
    if (path.entity.point_count() == 0)
        return;
    path.entity.set_point(0, point);
}

void drop_second_point_until_printable(GeneratedPath &path, double min_length_squared)
{
    while (path.entity.point_count() > 1 &&
           squared_distance(path.entity.local_front(), path.entity.point(1)) < min_length_squared) {
        path.entity.remove_point(1);
        set_path_front(path, path.entity.local_front());
    }
}

void collapse_tiny_segments(StoredPolyline &polyline, coord_t min_length)
{
    if (polyline.size() < 2)
        return;

    const double min_length_squared = double(min_length) * double(min_length);
    const bool closed = same_point(polyline.front(), polyline.back());
    std::vector<c_point> source = polyline.points();
    if (closed && source.size() > 1)
        source.pop_back();
    if (source.size() < 2) {
        polyline.clear();
        return;
    }

    // Clipper can create very short pieces when a generated line barely
    // touches the clipping boundary. They are below the physical spacing
    // budget and later become almost zero-length G-code moves, so merge them
    // into their neighbor before publishing the extrusion path.
    std::vector<c_point> cleaned;
    cleaned.reserve(source.size() + (closed ? 1 : 0));
    cleaned.push_back(source.front());
    for (size_t point_idx = 1; point_idx < source.size(); ++point_idx) {
        const bool last_point = point_idx + 1 == source.size();
        if (squared_distance(cleaned.back(), source[point_idx]) >= min_length_squared) {
            cleaned.push_back(source[point_idx]);
        } else if (last_point && cleaned.size() > 1) {
            cleaned.back() = source[point_idx];
        }
    }

    if (closed) {
        while (cleaned.size() > 2 &&
               squared_distance(cleaned.back(), cleaned.front()) < min_length_squared)
            cleaned.pop_back();
        if (cleaned.size() > 2)
            cleaned.push_back(cleaned.front());
    }

    polyline.clear();
    if (cleaned.size() < 2 || (closed && cleaned.size() < 4))
        return;
    polyline.insert_array(0, cleaned.data(), static_cast<uint32_t>(cleaned.size()));
}

StoredPolylineCollection collapse_tiny_segments(storage_handle *storage,
                                                const PolylineCollection &polylines,
                                                coord_t min_length)
{
    StoredPolylineCollection out(storage);
    for (Polyline polyline : polylines) {
        StoredPolyline cleaned(storage);
        cleaned.copy_from(polyline);
        collapse_tiny_segments(cleaned, min_length);
        if (cleaned.size() >= 2 && cleaned.length() >= min_length)
            out.push_back_move(std::move(cleaned));
    }
    return out;
}

StoredExPolygonCollection lower_slices_for_island(storage_handle *storage, const LayerIsland &island)
{
    StoredExPolygonCollection lower_slices(storage);
    const std::vector<LayerIsland> lower_islands = island.lower_islands();
    for (const LayerIsland &lower_island : lower_islands)
        lower_slices.push_back(lower_island.slice());

    if (lower_slices.empty())
        return lower_slices;

    ClipperContext clipper(storage);
    return clipper_union(clipper(lower_slices)).to_expolygon_collection();
}

StoredExPolygonCollection enabled_infill_area(storage_handle *storage,
                                             const ExPolygonCollection &candidate,
                                             const RegionSettings &settings)
{
    // RegionSettings may describe one value for the whole island or several
    // non-overlapping clips. Convert that setting state into the part of the
    // infill domain where this module is allowed to add overhang anchors.
    if (!settings.has_many_config(k_extra_perimeters_on_overhangs_key)) {
        if (!settings.get_solo_config(k_extra_perimeters_on_overhangs_key)
                 .get_bool(k_extra_perimeters_on_overhangs_key))
            return StoredExPolygonCollection(storage);
        return candidate.clone(storage);
    }

    StoredExPolygonCollection enabled(storage);
    const RegionSettings::AreaMap &areas = settings.get_areas(k_extra_perimeters_on_overhangs_key);
    for (const auto &[setting_value, setting_clip] : areas) {
        if (!setting_value.get_bool(k_extra_perimeters_on_overhangs_key))
            continue;
        setting_clip.append_intersections_to(enabled, candidate);
        if (setting_clip.is_accept_all())
            break;
    }

    ClipperContext clipper(storage);
    return clipper_union(clipper(enabled)).to_expolygon_collection();
}

StoredExPolygonCollection disabled_infill_area(storage_handle *storage,
                                              const ExPolygonCollection &candidate,
                                              const ExPolygonCollection &enabled)
{
    // Disabled clips are not processed, but they still belong to the island's
    // infill domain. They are unioned back into the unfilled area so later
    // infill steps do not lose material in regions where the module is off.
    if (candidate.empty() || enabled.empty())
        return candidate.clone(storage);
    ClipperContext clipper(storage);
    return clipper_diff(clipper(candidate), clipper(enabled)).to_expolygon_collection();
}

double collection_area(const ExPolygonCollection &collection)
{
    double area = 0.;
    for (ExPolygon expolygon : collection)
        area += expolygon.area();
    return area;
}

double collection_boundary_length(const ExPolygonCollection &collection)
{
    double length = 0.;
    for (ExPolygon expolygon : collection) {
        length += expolygon.contour().length();
        for (Polygon hole : expolygon.holes())
            length += hole.length();
    }
    return length;
}

bool collection_bounding_box(const ExPolygonCollection &collection, c_bounding_box &out)
{
    if (collection.empty())
        return false;

    bool initialized = false;
    for (ExPolygon expolygon : collection) {
        const c_bounding_box bbox = expolygon.contour().bounding_box();
        if (!initialized) {
            out = bbox;
            initialized = true;
        } else {
            out.min.x = std::min(out.min.x, bbox.min.x);
            out.min.y = std::min(out.min.y, bbox.min.y);
            out.max.x = std::max(out.max.x, bbox.max.x);
            out.max.y = std::max(out.max.y, bbox.max.y);
        }
    }

    return initialized;
}

c_bounding_box inflated_bounding_box(c_bounding_box bbox, coord_t delta)
{
    bbox.min.x -= delta;
    bbox.min.y -= delta;
    bbox.max.x += delta;
    bbox.max.y += delta;
    return bbox;
}

bool collection_equals(const ExPolygonCollection &lhs, const ExPolygonCollection &rhs)
{
    return expolygons_equals(lhs.handle(), rhs.handle()) != 0;
}

LineDistancer line_distancer_for_path(const GeneratedPath &path)
{
    LineDistancer distancer;
    const std::vector<c_point> points = path.entity.points();
    for (size_t idx = 1; idx < points.size(); ++idx)
        distancer.append_segment(points[idx - 1], points[idx]);
    return distancer;
}

bool paths_touch(const GeneratedPath &path_one,
                 const GeneratedPath &path_two,
                 coordf_t limit_distance)
{
    // The sort step builds a dependency graph between overhang strokes. Two
    // paths that touch are treated as printable in sequence, because the first
    // path can anchor the second path into already supported material.
    const LineDistancer lines_two = line_distancer_for_path(path_two);
    for (c_point point : path_one.entity.points())
        if (lines_two.distance_from_lines(point) < limit_distance)
            return true;

    const LineDistancer lines_one = line_distancer_for_path(path_one);
    for (c_point point : path_two.entity.points())
        if (lines_one.distance_from_lines(point) < limit_distance)
            return true;

    return false;
}

StoredPolylineCollection reconnect_polylines(storage_handle *storage,
                                             const PolylineCollection &polylines,
                                             coordf_t limit_distance)
{
    if (polylines.empty())
        return StoredPolylineCollection(storage);

    std::vector<std::unique_ptr<StoredPolyline>> connected;
    connected.reserve(polylines.size());
    for (Polyline polyline : polylines) {
        if (polyline.empty())
            continue;
        std::unique_ptr<StoredPolyline> copy(new StoredPolyline(storage));
        copy->copy_from(polyline);
        connected.push_back(std::move(copy));
    }

    // Generated loops often arrive as small pieces from boolean clipping.
    // Reconnect touching endpoints before creating extrusion entities so the
    // G-code planner receives long printable strokes instead of many crumbs.
    const double limit_squared = limit_distance * limit_distance;
    for (size_t first_idx = 0; first_idx < connected.size(); ++first_idx) {
        if (!connected[first_idx])
            continue;
        StoredPolyline &base = *connected[first_idx];
        for (size_t second_idx = first_idx + 1; second_idx < connected.size(); ++second_idx) {
            if (!connected[second_idx])
                continue;
            StoredPolyline &next = *connected[second_idx];
            if (squared_distance(base.back(), next.front()) < limit_squared) {
                append_points_to_polyline(base, next.points());
                connected[second_idx].reset();
            } else if (squared_distance(base.back(), next.back()) < limit_squared) {
                std::vector<c_point> points = next.points();
                std::reverse(points.begin(), points.end());
                append_points_to_polyline(base, points);
                connected[second_idx].reset();
            } else if (squared_distance(base.front(), next.back()) < limit_squared) {
                std::vector<c_point> points = base.points();
                StoredPolyline rebuilt(storage);
                rebuilt.copy_from(next);
                append_points_to_polyline(rebuilt, points);
                base.move_from(rebuilt);
                connected[second_idx].reset();
            } else if (squared_distance(base.front(), next.front()) < limit_squared) {
                base.reverse();
                append_points_to_polyline(base, next.points());
                connected[second_idx].reset();
            }
        }
    }

    StoredPolylineCollection result(storage);
    for (std::unique_ptr<StoredPolyline> &polyline : connected) {
        if (!polyline || polyline->size() < 2)
            continue;
        polyline->ensure_valid(coord_t(SCALED_EPSILON));
        if (polyline->size() >= 2)
            result.push_back_move(std::move(*polyline));
    }
    return result;
}

void orient_extra_perimeter_from_support(GeneratedPath &path,
                                         const LineDistancer &lower_layer_distancer)
{
    if (path.entity.point_count() < 2)
        return;

    // The extra path ordering has a physical meaning: the first printed point
    // should be the part that is closest to already supported material. Closed
    // paths need a rotation, while open paths only need their direction chosen.
    std::vector<c_point> points = path.entity.points();
    if (same_point(points.front(), points.back())) {
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
        path.entity.set_points(points);
        return;
    }

    const double first_distance = lower_layer_distancer.distance_from_lines(path.entity.local_front(), true);
    const double last_distance = lower_layer_distancer.distance_from_lines(path.entity.local_back(), true);
    if (last_distance < first_distance)
        path.entity.reverse();
}

std::vector<GeneratedPath> sort_extra_perimeters(std::vector<GeneratedPath> extra_perims,
                                                 int index_of_first_unanchored,
                                                 coordf_t extrusion_spacing)
{
    if (extra_perims.empty())
        return {};

    const coord_t min_split_length = minimum_printable_split_length(extrusion_spacing);
    const double min_split_length_squared = double(min_split_length) * double(min_split_length);

    // Every path before index_of_first_unanchored is already touching lower
    // material. Later paths should be printed only once they are connected to a
    // processed path, otherwise unsupported strokes may be emitted in mid-air.
    std::vector<std::unordered_set<size_t>> dependencies(extra_perims.size());
    for (size_t path_idx = 0; path_idx < extra_perims.size(); ++path_idx)
        for (size_t prev_path_idx = 0; prev_path_idx < path_idx; ++prev_path_idx)
            if (paths_touch(extra_perims[path_idx], extra_perims[prev_path_idx], extrusion_spacing * 1.5f))
                dependencies[path_idx].insert(prev_path_idx);

    std::vector<bool> processed(extra_perims.size(), false);
    for (int path_idx = 0; path_idx < index_of_first_unanchored; ++path_idx)
        processed[path_idx] = true;

    // Dependencies mean "print this after a touching already-supported path".
    // Flip unresolved dependencies when needed so unsupported strokes still get
    // printed in an order that tends to grow out of anchored material.
    for (size_t iteration = size_t(index_of_first_unanchored); iteration < extra_perims.size(); ++iteration) {
        bool changed = false;
        for (size_t path_idx = size_t(index_of_first_unanchored); path_idx < extra_perims.size(); ++path_idx) {
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

    c_point current_point = extra_perims.begin()->entity.local_front();
    std::vector<GeneratedPath> sorted_paths;
    const size_t invalid_idx = size_t(-1);
    size_t next_idx = invalid_idx;
    bool reverse = false;
    while (true) {
        if (next_idx == invalid_idx) {
            double dist = std::numeric_limits<double>::max();
            for (size_t path_idx = 0; path_idx < extra_perims.size(); ++path_idx) {
                if (!dependencies[path_idx].empty())
                    continue;
                const GeneratedPath &path = extra_perims[path_idx];
                const double dist_a = squared_distance(path.entity.local_front(), current_point);
                if (dist_a < dist) {
                    dist = dist_a;
                    next_idx = path_idx;
                    reverse = false;
                }
                const double dist_b = squared_distance(path.entity.local_back(), current_point);
                if (dist_b < dist) {
                    dist = dist_b;
                    next_idx = path_idx;
                    reverse = true;
                }
            }
            if (next_idx == invalid_idx)
                break;
        } else {
            if (reverse)
                extra_perims[next_idx].entity.reverse();
            sorted_paths.push_back(std::move(extra_perims[next_idx]));
            assert(dependencies[next_idx].empty());
            dependencies[next_idx].insert(invalid_idx);
            current_point = sorted_paths.back().entity.local_back();
            for (size_t path_idx = 0; path_idx < extra_perims.size(); ++path_idx)
                dependencies[path_idx].erase(next_idx);

            double dist = std::numeric_limits<double>::max();
            next_idx = invalid_idx;
            for (size_t path_idx = 0; path_idx < extra_perims.size(); ++path_idx) {
                if (!dependencies[path_idx].empty())
                    continue;
                const GeneratedPath &next_path = extra_perims[path_idx];
                const double dist_a = squared_distance(next_path.entity.local_front(), current_point);
                if (dist_a < dist) {
                    dist = dist_a;
                    next_idx = path_idx;
                    reverse = false;
                }
                const double dist_b = squared_distance(next_path.entity.local_back(), current_point);
                if (dist_b < dist) {
                    dist = dist_b;
                    next_idx = path_idx;
                    reverse = true;
                }
            }
            if (dist > scale_d(5.0))
                next_idx = invalid_idx;
        }
    }

    std::vector<GeneratedPath> reconnected;
    reconnected.reserve(sorted_paths.size());
    for (GeneratedPath &path : sorted_paths) {
        if (path.entity.local_length() < min_split_length)
            continue;

        if (!reconnected.empty() &&
            squared_distance(reconnected.back().entity.local_back(), path.entity.local_front()) <
                extrusion_spacing * extrusion_spacing * 4.0) {
            const double connector_length_squared =
                squared_distance(reconnected.back().entity.local_back(), path.entity.local_front());
            if (connector_length_squared < min_split_length_squared) {
                // A connector shorter than spacing/10 is a clipping crumb, not
                // a printable split. Snap the next path onto the previous one
                // so the G-code writer does not receive a 100 nm segment.
                set_path_front(path, reconnected.back().entity.local_back());
                drop_second_point_until_printable(path, min_split_length_squared);
            } else if (!same_point(reconnected.back().entity.local_back(), path.entity.local_front())) {
                std::vector<c_point> points = reconnected.back().entity.points();
                points.push_back(path.entity.local_front());
                reconnected.back().entity.set_points(points);
            }
            if (path.entity.local_length() > min_split_length)
                append_points_to_path(reconnected.back(), path);
        } else {
            reconnected.push_back(std::move(path));
        }
    }

    std::vector<GeneratedPath> filtered;
    filtered.reserve(reconnected.size());
    for (GeneratedPath &path : reconnected)
        if (path.entity.local_length() > 3 * extrusion_spacing)
            filtered.push_back(std::move(path));

    return filtered;
}

void append_overhang_paths(storage_handle *storage,
                           std::vector<GeneratedPath> &dst,
                           const PolylineCollection &polylines,
                           const OverhangGenerationInput &input)
{
    const coord_t min_split_length = minimum_printable_split_length(input.overhang_spacing);
    StoredPolylineCollection reconnected = reconnect_polylines(storage, polylines, input.overhang_spacing);
    StoredPolylineCollection cleaned = collapse_tiny_segments(storage, reconnected.readonly(), min_split_length);
    for (Polyline polyline : cleaned)
        dst.emplace_back(storage, polyline, input.overhang_flow.attributes);
}

StoredPolylineCollection clipped_polylines(storage_handle *storage,
                                           const PolylineCollection &polylines,
                                           const ExPolygonCollection &clip_area)
{
    StoredPolylineCollection out(storage);
    for (Polyline polyline : polylines) {
        StoredPolylineCollection clipped = clipper_intersection_polyline_expolygons(storage, polyline, clip_area);
        out.append_move_from(std::move(clipped));
    }
    return out;
}

c_flow medial_axis_flow_from_overhang_flow(const OverhangFlow &flow, coord_t spacing)
{
    const c_extrusion_property_attributes &attributes = flow.attributes;
    c_flow out = {};
    out.width = flow.width;
    out.spacing = spacing;
    out.height = scale_i(attributes.height);
    out.nozzle_diameter = flow.width;
    out.is_bridge = 0;
    out.spacing_ratio = 1.f;
    out.mm3_per_mm = attributes.mm3_per_mm;
    return out;
}

void append_medial_axis_paths(storage_handle *storage,
                              std::vector<GeneratedPath> &dst,
                              const ExPolygonCollection &areas,
                              const OverhangGenerationInput &input)
{
    /*
    Gap fill is optional for each area: tiny or degenerate shapes may produce no
    centerline. try_build() makes that case explicit while still returning a
    storage-owned extrusion tree when the medial axis finds printable paths.
    */
    const c_flow flow = medial_axis_flow_from_overhang_flow(input.overhang_flow, input.overhang_spacing);
    for (ExPolygon area : areas) {
        std::optional<StoredExtrusionEntity> tree =
            medial_axis_extrusion(RAW_EXTRUSION_ROLE_INTERNAL_PERIMETER, flow)
                .medial_widths(coord_t(0.75 * input.overhang_flow.width), coord_t(3.0 * input.overhang_spacing))
                .constant_width()
                .min_extrusion_length(input.overhang_spacing / 10)
                .try_build(storage, area);
        if (!tree.has_value())
            continue;

        for (uint32_t idx = 0; idx < tree->child_count(); ++idx) {
            GeneratedPath path(storage);
            path.entity.copy_from(tree->child(idx));
            path.entity.get_or_add_property<EPropertyAttributes>() = input.overhang_flow.attributes;
            path.entity.disable_reverse();
            dst.push_back(std::move(path));
        }
    }
}

bool bridgeable_overhang_area(storage_handle *storage,
                              const ExPolygonCollection &real_overhang,
                              const ExPolygonCollection &anchors,
                              const OverhangGenerationInput &input)
{
    if (real_overhang.empty() || anchors.empty())
        return false;

    // BridgeDetector is exposed as a service through the plugin ABI. Using it
    // here keeps this perimeter post-process independent from host-only
    // BridgeDetector classes while preserving the same bridgeability question:
    // "can this unsupported region be spanned by material attached to anchors?"
    bridge_detector_create_input detector_input = {};
    detector_input.expolygons = real_overhang.handle();
    detector_input.lower_slices = anchors.handle();
    detector_input.spacing = input.overhang_flow.spacing;
    detector_input.precision = input.bridge_precision;
    detector_input.layer_id = input.layer_id;

    BridgeDetector detector(orchestrator_create_bridge_detector(nullptr, &detector_input));
    if (!detector)
        return false;

    double bridge_angle = input.bridge_angle_enabled ? input.bridge_angle_rad : 0.;
    if (!input.bridge_angle_enabled) {
        if (!detector.detect_angle())
            return false;
        bridge_angle = detector.angle();
    }

    StoredPolylineCollection unsupported_lines(storage);
    const uint32_t unsupported_count = detector.unsupported_edges(unsupported_lines, bridge_angle);
    double unsupported_length = 0.;
    if (unsupported_count > 0) {
        for (Polyline line : unsupported_lines)
            unsupported_length += line.length();
    }

    StoredPolygonCollection coverage_polygons(storage);
    detector.coverage(coverage_polygons, bridge_angle);

    ClipperContext clipper(storage);
    ClipperOperand coverage = clipper_union(clipper(coverage_polygons));
    StoredExPolygonCollection unbridgeable =
        clipper_diff(clipper(real_overhang), coverage).to_expolygon_collection();

    const double real_area = collection_area(real_overhang);
    if (real_area <= 0.)
        return true;

    return collection_area(unbridgeable) < 0.2 * real_area &&
           unsupported_length < collection_boundary_length(real_overhang) * 0.2;
}

OverhangGenerationOutput generate_extra_perimeters_over_overhangs(storage_handle *storage,
                                                                  const ExPolygon &island,
                                                                  const ExPolygonCollection &infill_area,
                                                                  const OverhangGenerationInput &input)
{
    OverhangGenerationOutput out(storage);
    if (infill_area.empty() || input.lower_slices.empty() || input.perimeter_depth <= 0)
        return out;

    ClipperContext clipper(storage);
    c_bounding_box infill_area_bb = {};
    if (!collection_bounding_box(infill_area, infill_area_bb))
        return out;
    const coord_t bbox_margin = coord_t(SCALED_EPSILON) + input.anchors_size;
    StoredExPolygonCollection optimized_lower_slices =
        clipper_clip_expolygons_with_subject_bbox(
            storage, input.lower_slices.readonly(), inflated_bounding_box(infill_area_bb, bbox_margin));

    StoredExPolygonCollection overhangs =
        clipper_diff(clipper(infill_area), clipper(optimized_lower_slices)).to_expolygon_collection();
    if (overhangs.empty())
        return out;

    LineDistancer lower_layer_distancer(optimized_lower_slices);
    StoredExPolygonCollection island_collection(storage, island);
    StoredExPolygonCollection anchors =
        clipper_intersection(clipper(island_collection), clipper(optimized_lower_slices)).to_expolygon_collection();
    StoredExPolygonCollection anchors_no_overhangs =
        clipper_diff(clipper(anchors), clipper(overhangs)).to_expolygon_collection();
    ClipperOperand expanded_overhangs_for_anchors =
        clipper_offset(clipper(overhangs), input.anchors_size, CLIPPER_JOIN_SQUARE, 0.);
    StoredExPolygonCollection inset_anchors =
        clipper_diff(clipper(anchors), expanded_overhangs_for_anchors).to_expolygon_collection();
    StoredExPolygonCollection inset_overhang_area =
        clipper_diff(clipper(infill_area), clipper(inset_anchors)).to_expolygon_collection();

    StoredExPolygonCollection inset_overhang_area_left_unfilled(storage);
    std::vector<std::vector<GeneratedPath>> extra_perimeters;

    // Each disconnected overhang area is processed independently. The loop
    // builds clipped overhang perimeters until the remaining area is either
    // bridgeable or too small, then returns both the generated paths and the
    // area consumed by them.
    StoredExPolygonCollection overhang_regions =
        clipper_union(clipper(inset_overhang_area)).to_expolygon_collection();
    for (ExPolygon overhang : overhang_regions) {
        StoredExPolygonCollection overhang_to_cover(storage, overhang);
        // These expanded/shrunken masks feed several boolean operations before
        // any code needs to iterate their ExPolygons. Keeping them as Clipper
        // operands avoids creating temporary collections after each offset.
        ClipperOperand expanded_overhang_to_cover =
            clipper_offset(clipper(overhang_to_cover), 1.1 * input.overhang_spacing);
        ClipperOperand shrinked_overhang_shape =
            clipper_offset(clipper(overhang_to_cover), -0.1 * input.overhang_spacing);

        StoredExPolygonCollection real_overhang =
            clipper_intersection(clipper(overhang_to_cover), clipper(overhangs)).to_expolygon_collection();
        if (real_overhang.empty()) {
            inset_overhang_area_left_unfilled.append_copy_from(overhang_to_cover.readonly());
            continue;
        }

        std::vector<GeneratedPath> &overhang_region = extra_perimeters.emplace_back();
        StoredExPolygonCollection anchoring =
            clipper_intersection(expanded_overhang_to_cover, clipper(inset_anchors)).to_expolygon_collection();
        ClipperOperand grown_overhang_to_cover =
            clipper_offset(clipper(overhang_to_cover), 0.1 * input.overhang_spacing);
        ClipperOperand perimeter_source =
            clipper_union2(grown_overhang_to_cover, clipper(anchoring));
        StoredExPolygonCollection perimeter_polygon =
            clipper_offset2(
                perimeter_source,
                -input.overhang_spacing * (0.1 + 0.5 + 0.1),
                input.overhang_spacing * 0.1)
                .to_expolygon_collection();

        if (bridgeable_overhang_area(storage, real_overhang, anchors, input)) {
            inset_overhang_area_left_unfilled.append_copy_from(overhang_to_cover.readonly());
            perimeter_polygon.clear();
        } else {
            ClipperOperand inset_anchor_exclusion =
                clipper_offset(clipper(inset_anchors), input.overhang_spacing * 0.5);
            StoredExPolygonCollection shrinked_overhang_to_cover =
                clipper_diff(shrinked_overhang_shape, inset_anchor_exclusion).to_expolygon_collection();

            int continuation_loops = 2;
            while (continuation_loops >= 0) {
                StoredExPolygonCollection previous = perimeter_polygon.readonly().clone(storage);
                StoredPolylineCollection perimeter_polylines =
                    expolygons_to_polylines(storage, perimeter_polygon);
                StoredPolylineCollection perimeter =
                    clipped_polylines(storage, perimeter_polylines, shrinked_overhang_to_cover);

                // The next perimeter is a chain of union -> offset ->
                // intersection. Only the final intersection is materialized
                // because the rest is immediately consumed by Clipper again.
                ClipperOperand perimeter_with_anchoring =
                    clipper_union2(clipper(perimeter_polygon), clipper(anchoring));
                ClipperOperand next_perimeter =
                    clipper_offset(perimeter_with_anchoring, -input.overhang_spacing);
                perimeter_polygon =
                    clipper_intersection(next_perimeter, expanded_overhang_to_cover).to_expolygon_collection();

                if (perimeter_polygon.empty()) {
                    ClipperOperand previous_shrunken =
                        clipper_offset(clipper(previous), -0.3 * input.overhang_spacing);
                    StoredExPolygonCollection shrinked =
                        clipper_intersection(previous_shrunken, expanded_overhang_to_cover)
                            .to_expolygon_collection();
                    if (!shrinked.empty())
                        append_overhang_paths(storage, overhang_region, perimeter, input);

                    StoredExPolygonCollection gap =
                        shrinked.empty() ?
                        clipper_offset(clipper(previous), input.overhang_spacing * 0.5).to_expolygon_collection() :
                        std::move(shrinked);
                    StoredExPolygonCollection clipped_gap =
                        clipper_intersection(clipper(gap), clipper(shrinked_overhang_to_cover)).to_expolygon_collection();
                    append_medial_axis_paths(storage, overhang_region, clipped_gap, input);
                    break;
                }

                append_overhang_paths(storage, overhang_region, perimeter, input);
                ClipperOperand remaining_real_overhang =
                    clipper_intersection(clipper(perimeter_polygon), clipper(real_overhang));
                if (remaining_real_overhang.empty())
                    --continuation_loops;
                if (collection_equals(previous, perimeter_polygon))
                    break;
            }

            ClipperOperand grown_perimeter_polygon =
                clipper_offset(clipper(perimeter_polygon), 0.5 * input.overhang_spacing);
            perimeter_polygon =
                clipper_union2(grown_perimeter_polygon, clipper(anchoring)).to_expolygon_collection();
            inset_overhang_area_left_unfilled.append_copy_from(perimeter_polygon.readonly());

            overhang_region.erase(
                std::remove_if(overhang_region.begin(), overhang_region.end(),
                               [](const GeneratedPath &path) {
                                   return path.entity.point_count() < 2 || path.entity.local_length() <= 0.;
                               }),
                overhang_region.end());

            if (!overhang_region.empty()) {
                StoredPolyline first_polyline = polyline_from_path(storage, overhang_region.front());
                StoredPolylineCollection anchored_part =
                    clipper_intersection_polyline_expolygons(storage, first_polyline, optimized_lower_slices);
                const bool first_overhang_is_closed_and_anchored =
                    overhang_region.front().entity.local_is_closed() && !anchored_part.empty();

                const auto is_anchored =
                    [&lower_layer_distancer](const GeneratedPath &path) {
                        return lower_layer_distancer.distance_from_lines(path.entity.local_front(), true) <= 0 ||
                               lower_layer_distancer.distance_from_lines(path.entity.local_back(), true) <= 0;
                    };

                if (!first_overhang_is_closed_and_anchored)
                    std::reverse(overhang_region.begin(), overhang_region.end());

                for (GeneratedPath &path : overhang_region)
                    orient_extra_perimeter_from_support(path, lower_layer_distancer);

                std::vector<GeneratedPath>::iterator first_unanchored =
                    std::stable_partition(overhang_region.begin(), overhang_region.end(), is_anchored);
                const int index_of_first_unanchored = int(first_unanchored - overhang_region.begin());
                overhang_region =
                    sort_extra_perimeters(std::move(overhang_region), index_of_first_unanchored, input.overhang_spacing);
            }
        }
    }

    inset_overhang_area_left_unfilled =
        clipper_union(clipper(inset_overhang_area_left_unfilled)).to_expolygon_collection();

    out.extra_perimeters = std::move(extra_perimeters);
    out.filled_area =
        clipper_diff(clipper(inset_overhang_area), clipper(inset_overhang_area_left_unfilled)).to_expolygon_collection();
    out.filled_area.ensure_valid();
    out.unfilled_area =
        clipper_union2(clipper(inset_anchors), clipper(inset_overhang_area_left_unfilled)).to_expolygon_collection();
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
    // Gather the small immutable bundle needed by the overhang-anchor
    // algorithm. Geometry still uses the configured overhang spacing so the
    // anchors are distributed as before, but the generated paths are plain
    // internal perimeters. DetectOverhang later decides which spans are really
    // unsupported.
    const Config region_config = island.region(0).print_region().config();
    const Config print_config = print.config();
    const Config object_config = object.config();
    const int32_t perimeter_count = std::max(0, config_int_or(region_config, k_perimeters_key, 0));

    OverhangGenerationInput input(storage);
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
        input.bridge_precision = std::max<coord_t>(coord_t(SCALED_EPSILON), input.overhang_flow.spacing / 4);
    input.bridge_angle_enabled = region_config.has(k_bridge_angle_key) &&
        region_config.get(k_bridge_angle_key).is_enabled();
    input.bridge_angle_rad = deg_to_rad(config_float_or(region_config, k_bridge_angle_key, 0.));
    input.lower_slices = lower_slices_for_island(storage, island);

    // Keep the old "do not add overhang extras on the first printable object
    // layer" guard. The C layer handle currently exposes traversal index rather
    // than native Layer::id(), so use the object-local layer index.
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
    const int32_t perimeter_count = std::max(0, config_int_or(config, k_perimeters_key, 0));
    if (perimeter_count <= 0 || !config.has(k_infill_overlap_key))
        return 0;

    const double ratio = unscaled(perimeter_count == 1 ? external_flow.spacing : perimeter_flow.spacing);
    return scale_i(config.get(k_infill_overlap_key).get_effective_value(ratio));
}

void prepend_extra_perimeters_to_root(storage_handle *storage,
                                      MutableExtrusionEntity root,
                                      std::vector<std::vector<GeneratedPath>> &extra_perimeters)
{
    // Preserve the current perimeter bucket before clearing it. The rebuilt
    // root is an unsortable collection: generated extra anchors first,
    // followed by the original normal perimeter children in their old order.
    StoredExtrusionEntity original(storage, root.readonly());
    root.clear_content();

    for (std::vector<GeneratedPath> &paths : extra_perimeters)
        for (GeneratedPath &path : paths)
            root.append_child_move(path.entity.mutable_view());

    if (original.child_count() > 0) {
        while (original.child_count() > 0)
            root.move_child_from(root.child_count(), original.mutable_view(), 0);
    } else if (!original.empty()) {
        root.append_child_move(original.mutable_view());
    }

    // Adding the first child turns an empty entity into a regular collection,
    // and the host helper defaults such collections to sortable. Force the
    // final ordering contract after all children are in place: generated
    // extra anchors first, then the normal perimeter tree.
    root.disable_sort();
    root.disable_reverse();
}

bool extra_perimeters_empty(const std::vector<std::vector<GeneratedPath>> &extra_perimeters)
{
    for (const std::vector<GeneratedPath> &paths : extra_perimeters)
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
    // Extra perimeter paths consume part of the old infill job. The strict
    // free area loses the generated extrusion coverage; the wider fill/anchor
    // area either follows the legacy overlap expansion or subtracts the same
    // coverage when no infill/perimeter overlap is requested.
    ClipperContext clipper(storage);
    const ExPolygonCollection fill_areas = island.infill_areas();
    const ExPolygonCollection free_areas = island.infill_no_overlap_areas();
    ClipperOperand free_source =
        free_areas.empty() ? clipper(fill_areas) : clipper(free_areas);

    StoredExPolygonCollection next_free_areas =
        clipper_diff(free_source, clipper(generated.filled_area)).to_expolygon_collection();
    StoredExPolygonCollection next_fill_areas =
        infill_overlap != 0 ?
        clipper_intersection(clipper(fill_areas),
                             clipper_offset(clipper(generated.unfilled_area), infill_overlap)).to_expolygon_collection() :
        clipper_diff(clipper(fill_areas), clipper(generated.filled_area)).to_expolygon_collection();

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
        generate_extra_perimeters_over_overhangs(storage, island.slice(), enabled_area, input);
    if (extra_perimeters_empty(generated.extra_perimeters))
        return;

    if (settings.has_many_config(k_extra_perimeters_on_overhangs_key)) {
        StoredExPolygonCollection disabled_area = disabled_infill_area(storage, infill_candidate, enabled_area);
        ClipperContext clipper(storage);
        generated.unfilled_area =
            clipper_union2(clipper(generated.unfilled_area), clipper(disabled_area)).to_expolygon_collection();
    }

    prepend_extra_perimeters_to_root(storage, root, generated.extra_perimeters);
    update_fill_areas(storage, ctx, island, generated, infill_overlap_for_island(island, perimeter_flow, external_flow));
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
