///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "DetectOverhang.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "libslic3r/Api/plugin/c/steps/slic3r_step_post_perimeter.h"
#include "libslic3r/Api/plugin/c/slic3r_extrusions.h"
#include "libslic3r/Api/plugin/c/slic3r_utils.h"
#include "libslic3r/Api/plugin/cpp/ClipperViews.hpp"
#include "libslic3r/Api/plugin/cpp/DataTreeViews.hpp"
#include "libslic3r/Api/plugin/cpp/ExtrusionTreeVisitors.hpp"
#include "libslic3r/Api/plugin/cpp/ExtrusionViews.hpp"
#include "libslic3r/Api/plugin/cpp/LineDistancer.hpp"
#include "libslic3r/Api/plugin/cpp/RegionSettingsViews.hpp"

/*
Overhang detection and perimeter annotation
============================================

This plugin provides the STEP_POST_PERIMETER pass that identifies unsupported
parts of perimeter paths and annotates them with the configured overhang flow
and speed. It processes object layers after the first one; the first printable
layer has no lower layer against which support can be measured.

For each LayerIsland, process_island() segregates the overhang settings by
region, builds the supported centerline area from lower islands, and prepares a
LineDistancer plus curled-line proximity data. Each region-island perimeter
root is then visited by DetectOverhangVisitor. The visitor keeps the inherited
extrusion attributes, analyzes every leaf with split_leaf(), and replaces a
leaf only when its fragments differ from the original path.

split_leaf() classifies each path against support distance and curled geometry.
It creates ordered fragments, applies dynamic or fixed flow and speed
properties where required, merges tiny neighboring fragments, and preserves
unchanged portions as clones. A replacement leaf becomes a collection of the
ordered fragments; the original perimeter tree and its non-leaf structure are
otherwise preserved.

The normal call flow is:

    DetectOverhang::setup_run_impl()
    `-- reserve one progress unit per object layer

    DetectOverhang::run_impl()
    `-- walk object layers (skip layer 0)
        `-- process_island()
            |-- segregate overhang settings by region
            |-- supported_centerline_area()
            |-- create LineDistancer and CurledLineProximity
            `-- process_region_island() for each region island
                |-- obtain the mutable perimeter root
                |-- DetectOverhangVisitor::traverse(root)
                `-- visit_leaf() for every printable leaf
                    |-- split_leaf()
                    |   |-- measure support and curled proximity
                    |   |-- split and annotate affected fragments
                    |   `-- merge tiny fragments
                    `-- replace_root_leaf_with_fragments() when needed

The visitor changes path fragments and their attributes, not the layer
topology or the source region settings. Unsupported sections that do not meet
the configured thresholds remain unchanged, and an arc-aware representation
must be preserved by the fragment operations rather than flattened into a
plain polyline.
*/

namespace slic3r_api { namespace Perimeter { namespace DetectOverhangPlugin {

namespace {

const char *k_detect_overhang_id = "perimeter.post_process.detect_overhang";
const char *k_no_dependencies[] = { nullptr };

const raw_used_config_key k_used_config_keys[] = {
    { "overhangs", RAW_CO_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "overhangs_flow_ratio", RAW_CO_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "overhangs_width", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "overhangs_dynamic_flow", RAW_CO_GRAPH, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "overhangs_type", RAW_CO_ENUM, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "overhangs_width_speed", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "overhangs_dynamic_speed", RAW_CO_GRAPH, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE }
};

const char *k_overhangs_key = "overhangs";
const char *k_overhangs_flow_ratio_key = "overhangs_flow_ratio";
const char *k_overhangs_width_key = "overhangs_width";
const char *k_overhangs_dynamic_flow_key = "overhangs_dynamic_flow";
const char *k_overhangs_type_key = "overhangs_type";
const char *k_overhangs_width_speed_key = "overhangs_width_speed";
const char *k_overhangs_dynamic_speed_key = "overhangs_dynamic_speed";

constexpr double k_distance_split_ratio = 0.01;
constexpr double k_curled_split_epsilon = 0.001;

struct OverhangConfig
{
    bool enabled = false;
    bool flow_enabled = false;
    bool dynamic_flow_enabled = false;
    bool speed_enabled = false;
    bool dynamic_speed_enabled = false;

    double flow_threshold_mm = 0.;
    double speed_threshold_mm = 0.;
    double nozzle_diameter_mm = 0.;

    const graph_data_handle *dynamic_flow_graph = nullptr;
    const graph_data_handle *dynamic_speed_graph = nullptr;

    c_flow overhang_flow = {};

    double max_threshold_mm() const
    {
        return std::max(flow_enabled ? flow_threshold_mm : 0.,
                        speed_enabled ? speed_threshold_mm : 0.);
    }
};

struct PointSupportMetrics
{
    double distance_mm = 0.;
    double curled_proximity = 0.;
};

struct Fragment
{
    Fragment(storage_handle *storage,
             const ExtrusionEntity &source,
             const std::vector<c_point> &points,
             const EPropertyAttributes &attributes,
             double order_in,
             bool changed_in,
             double min_length_in = 0.) :
        entity(storage, source),
        order(order_in),
        changed(changed_in),
        min_length(min_length_in)
    {
        entity.set_points(points);
        entity.get_or_add(EPropertyAttributes::key) = attributes;
    }

    StoredExtrusionEntity entity;
    double order = 0.;
    bool changed = false;
    double min_length = 0.;
};

StoredPolyline polyline_from_points(storage_handle *storage, const std::vector<c_point> &points)
{
    StoredPolyline polyline(storage);
    if (!points.empty())
        polyline.insert_array(0, points.data(), static_cast<uint32_t>(points.size()));
    return polyline;
}

double squared_distance_to_segment(c_point point,
                                   c_point segment_start,
                                   c_point segment_end,
                                   double &projection_ratio)
{
    const double vx = double(segment_end.x - segment_start.x);
    const double vy = double(segment_end.y - segment_start.y);
    const double wx = double(point.x - segment_start.x);
    const double wy = double(point.y - segment_start.y);
    const double segment_length_sq = vx * vx + vy * vy;

    if (segment_length_sq <= 0.) {
        projection_ratio = 0.;
        const double dx = double(point.x - segment_start.x);
        const double dy = double(point.y - segment_start.y);
        return dx * dx + dy * dy;
    }

    projection_ratio = (wx * vx + wy * vy) / segment_length_sq;
    projection_ratio = std::max(0., std::min(1., projection_ratio));

    const double px = double(segment_start.x) + vx * projection_ratio;
    const double py = double(segment_start.y) + vy * projection_ratio;
    const double dx = double(point.x) - px;
    const double dy = double(point.y) - py;
    return dx * dx + dy * dy;
}

double distance_along_points(const std::vector<c_point> &source, c_point point)
{
    // Clipper may return polyline fragments in an order optimized for geometry
    // processing rather than extrusion. Project every fragment start back onto
    // the original path so the replacement tree keeps travel order stable.
    double best_distance_sq = std::numeric_limits<double>::max();
    double best_distance = 0.;
    double accumulated = 0.;

    for (size_t idx = 1; idx < source.size(); ++idx) {
        double projection_ratio = 0.;
        const double distance_sq = squared_distance_to_segment(point, source[idx - 1], source[idx], projection_ratio);
        const double segment_length = norm(source[idx] - source[idx - 1]);
        if (distance_sq < best_distance_sq) {
            best_distance_sq = distance_sq;
            best_distance = accumulated + projection_ratio * segment_length;
        }
        accumulated += segment_length;
    }

    return best_distance;
}

bool same_points(const std::vector<c_point> &lhs, const std::vector<c_point> &rhs)
{
    if (lhs.size() != rhs.size())
        return false;
    for (size_t idx = 0; idx < lhs.size(); ++idx)
        if (lhs[idx].x != rhs[idx].x || lhs[idx].y != rhs[idx].y)
            return false;
    return true;
}

double point_path_length(const std::vector<c_point> &points)
{
    double length = 0.;
    for (size_t idx = 1; idx < points.size(); ++idx)
        length += norm(points[idx] - points[idx - 1]);
    return length;
}

double minimum_split_length(const OverhangConfig &config, const EPropertyAttributes &attributes)
{
    // Overhang detection is a classifier, not a path generator. Very short
    // classifier fragments should be absorbed by a neighbor because they do
    // not carry enough printable length to justify a separate G-code move.
    const raw_extrusion_role role = raw_extrusion_role(attributes.extrusion_role());
    const bool external_perimeter = RAW_EXTRUSION_ROLE_IS_EXTERNAL(role);
    const double min_mm = external_perimeter ?
        config.nozzle_diameter_mm / 4. :
        std::max(config.nozzle_diameter_mm, config.max_threshold_mm()) / 10.;
    return scale_d(std::max(0.001, min_mm));
}

void append_connected_points(std::vector<c_point> &dst, const std::vector<c_point> &src)
{
    if (src.empty())
        return;
    if (dst.empty()) {
        dst = src;
        return;
    }

    const size_t first = points_equal(dst.back(), src.front()) ? 1u : 0u;
    dst.insert(dst.end(), src.begin() + first, src.end());
}

void replace_fragment_points(Fragment &fragment, std::vector<c_point> &&points)
{
    fragment.entity.set_points(points);
    fragment.changed = true;
}

void append_fragment_points(Fragment &dst, const Fragment &src)
{
    std::vector<c_point> points = dst.entity.points();
    append_connected_points(points, src.entity.points());
    replace_fragment_points(dst, std::move(points));
}

void prepend_fragment_points(Fragment &dst, const Fragment &src)
{
    std::vector<c_point> points = src.entity.points();
    append_connected_points(points, dst.entity.points());
    replace_fragment_points(dst, std::move(points));
}

bool is_tiny_fragment(const Fragment &fragment)
{
    return fragment.min_length > 0. &&
           point_path_length(fragment.entity.points()) < fragment.min_length;
}

bool collapse_tiny_segments(Fragment &fragment)
{
    if (fragment.min_length <= 0.)
        return true;

    const std::vector<c_point> source = fragment.entity.points();
    if (source.size() < 2)
        return false;

    const double min_length_sq = fragment.min_length * fragment.min_length;
    std::vector<c_point> cleaned;
    cleaned.reserve(source.size());
    cleaned.push_back(source.front());
    for (size_t point_idx = 1; point_idx < source.size(); ++point_idx) {
        const bool last_point = point_idx + 1 == source.size();
        const double dx = double(cleaned.back().x - source[point_idx].x);
        const double dy = double(cleaned.back().y - source[point_idx].y);
        const double distance_sq = dx * dx + dy * dy;
        if (distance_sq >= min_length_sq) {
            cleaned.push_back(source[point_idx]);
        } else if (last_point && cleaned.size() > 1) {
            cleaned.back() = source[point_idx];
        }
    }

    if (cleaned.size() < 2)
        return false;

    replace_fragment_points(fragment, std::move(cleaned));
    return true;
}

void merge_tiny_fragments(std::vector<Fragment> &fragments)
{
    // The support/overhang boundary can cut an extrusion at a very shallow
    // angle and leave a classifier-only sliver. Keeping that sliver as its own
    // path creates almost-zero extrusion moves. Merging it into a neighbor
    // preserves the centerline order and merely lets the neighbor's overhang
    // classification cover an unprintably small distance.
    fragments.erase(
        std::remove_if(fragments.begin(), fragments.end(),
                       [](Fragment &fragment) {
                           return !collapse_tiny_segments(fragment) || fragment.entity.point_count() < 2;
                       }),
        fragments.end());

    for (size_t idx = 0; idx < fragments.size();) {
        if (!is_tiny_fragment(fragments[idx])) {
            ++idx;
            continue;
        }

        if (idx > 0) {
            append_fragment_points(fragments[idx - 1], fragments[idx]);
            collapse_tiny_segments(fragments[idx - 1]);
            fragments.erase(fragments.begin() + idx);
            continue;
        }

        if (fragments.size() > 1) {
            prepend_fragment_points(fragments[1], fragments[0]);
            collapse_tiny_segments(fragments[1]);
            fragments.erase(fragments.begin());
            continue;
        }

        ++idx;
    }
}

std::vector<c_point> densify_points(const std::vector<c_point> &points, coord_t max_segment_length)
{
    // Dynamic overhang flow/speed depends on the distance to support along the
    // path. Long straight segments would hide that variation if only endpoints
    // were sampled, so split them into short measuring spans before assigning
    // dynamic attributes.
    if (points.size() < 2 || max_segment_length <= 0)
        return points;

    std::vector<c_point> out;
    out.reserve(points.size());
    out.push_back(points.front());
    for (size_t idx = 1; idx < points.size(); ++idx) {
        const c_point previous = points[idx - 1];
        const c_point current = points[idx];
        const double length = norm(current - previous);
        const uint32_t parts = std::max<uint32_t>(1u, uint32_t(std::ceil(length / double(max_segment_length))));
        for (uint32_t part = 1; part <= parts; ++part)
            out.push_back(point_at(previous, current, length * double(part) / double(parts)));
    }
    return out;
}

StoredExPolygonCollection supported_centerline_area(storage_handle *storage,
                                                    const LayerIsland &island,
                                                    coord_t external_perimeter_width)
{
    StoredExPolygonCollection lower_slices(storage);
    for (uint32_t idx = 0; idx < island.lower_island_count(); ++idx)
        lower_slices.push_back(island.lower_island(idx).slice());

    if (lower_slices.empty())
        return lower_slices;

    ClipperContext clipper(storage);
    ClipperOperand merged_lower = clipper_union(clipper(lower_slices.readonly()));
    return clipper_offset(merged_lower, SCALED_EPSILON -0.5 * double(external_perimeter_width)).to_expolygon_collection();
}

OverhangConfig overhang_config_from_value(const RegionSettingsValue &value, const LayerRegion &region, bool external_role)
{
    OverhangConfig out;
    out.enabled = value.get_bool(k_overhangs_key);
    if (!out.enabled)
        return out;

    const ConfigOption flow_ratio = value.option(k_overhangs_flow_ratio_key);
    const ConfigOption speed_width = value.option(k_overhangs_width_speed_key);
    out.flow_enabled = flow_ratio.is_enabled();
    out.speed_enabled = speed_width.is_enabled();

    // Detecting overhangs only makes sense when some later rule will consume
    // the result. If both optional overhang flow and overhang speed are disabled,
    // keep the path in the same fast no-op branch as the global overhang switch.
    if (!out.flow_enabled && !out.speed_enabled) {
        out.enabled = false;
        return out;
    }

    const raw_extrusion_role base_role =
        external_role ? RAW_EXTRUSION_ROLE_EXTERNAL_PERIMETER : RAW_EXTRUSION_ROLE_INTERNAL_PERIMETER;
    const c_flow normal_flow = region.flow(base_role);
    out.overhang_flow = region.bridging_flow(base_role);
    out.nozzle_diameter_mm = unscaled(normal_flow.nozzle_diameter);
    if (out.nozzle_diameter_mm <= 0.)
        out.nozzle_diameter_mm = unscaled(out.overhang_flow.nozzle_diameter);

    if (out.flow_enabled) {
        out.flow_threshold_mm = value.get_effective_value(out.nozzle_diameter_mm, k_overhangs_width_key);
        const ConfigOption dynamic_flow = value.option(k_overhangs_dynamic_flow_key);
        out.dynamic_flow_enabled = dynamic_flow.is_enabled();
        out.dynamic_flow_graph = dynamic_flow.graph();
    }

    if (out.speed_enabled) {
        out.speed_threshold_mm = speed_width.get_effective_value(out.nozzle_diameter_mm);
        const ConfigOption dynamic_speed = value.option(k_overhangs_dynamic_speed_key);
        out.dynamic_speed_enabled = dynamic_speed.is_enabled();
        out.dynamic_speed_graph = dynamic_speed.graph();
    }

    return out;
}

double graph_interpolate_or_default(const graph_data_handle *graph, double x, double fallback)
{
    return graph == nullptr ? fallback : graph_interpolate(graph, x);
}

void apply_overhang_flow(EPropertyAttributes &attributes,
                         const OverhangConfig &config,
                         const EPropertyOverhang &overhang,
                         double ratio)
{
    if (ratio <= 0.)
        return;

    ratio = std::max(0., std::min(1., ratio));
    const double max_overhang_mm =
        std::max(double(overhang.start_distance_from_prev_layer), double(overhang.end_distance_from_prev_layer));
    const double blended_mm3 =
        attributes.c_extrusion_property_attributes::mm3_per_mm * (1. - ratio) +
        config.overhang_flow.mm3_per_mm * ratio;

    if (max_overhang_mm > attributes.c_extrusion_property_attributes::width) {
        // A fully suspended extrusion is modeled as a round strand. The cross
        // section area is mm3/mm, so diameter is derived from area = pi*d^2/4.
        const double round_factor = 0.25 * PI;
        const double diameter = std::sqrt(std::max(0., blended_mm3 / round_factor));
        attributes.mm3_per_mm(blended_mm3)
                  .width(float(diameter))
                  .height(float(diameter));
        return;
    }

    // For a partially supported overhang, keep the visual width and solve the
    // rounded-rectangle area equation for height. Numerical guards avoid a NaN
    // if a custom flow creates a physically impossible cross-section.
    const double width = std::max(0.000001, double(attributes.c_extrusion_property_attributes::width));
    const double rounded_factor = 1. - 0.25 * PI;
    const double discriminant =
        width * width / (4. * rounded_factor * rounded_factor) - blended_mm3 / rounded_factor;
    const double height = width / (2. * rounded_factor) - std::sqrt(std::max(0., discriminant));
    attributes.mm3_per_mm(blended_mm3)
              .height(float(height));
}

void apply_overhang_properties(EPropertyAttributes &attributes,
                               StoredExtrusionEntity &entity,
                               const OverhangConfig &config,
                               double min_distance_mm,
                               double max_distance_mm,
                               double curled_proximity)
{
    const bool full_flow = config.flow_enabled && max_distance_mm > config.flow_threshold_mm;
    const bool dynamic_flow =
        config.flow_enabled && !full_flow && config.dynamic_flow_enabled && max_distance_mm > 0.;
    const bool full_speed = config.speed_enabled && max_distance_mm > config.speed_threshold_mm;
    const bool dynamic_speed =
        config.speed_enabled && !full_speed && config.dynamic_speed_enabled && max_distance_mm > 0.;

    if (!full_flow && !dynamic_flow && !full_speed && !dynamic_speed) {
        attributes.extrusion_role(attributes.extrusion_role() & ~RAW_EXTRUSION_ROLE_BRIDGE);
        entity.remove(EPropertyOverhang::key);
        entity.get_or_add(EPropertyAttributes::key) = attributes;
        return;
    }

    EPropertyOverhang &overhang = entity.get_or_add(EPropertyOverhang::key);
    overhang = EPropertyOverhang{};
    overhang.distance(float(min_distance_mm), float(max_distance_mm))
            .curled_proximity(float(curled_proximity))
            .full_overhangs_flow(full_flow)
            .full_overhangs_speed(full_speed)
            .dynamic_overhangs_flow(dynamic_flow)
            .dynamic_overhangs_speed(dynamic_speed);

    if (full_flow || full_speed)
        attributes.extrusion_role(attributes.extrusion_role() | RAW_EXTRUSION_ROLE_BRIDGE);
    else
        attributes.extrusion_role(attributes.extrusion_role() & ~RAW_EXTRUSION_ROLE_BRIDGE);

    if (full_flow) {
        apply_overhang_flow(attributes, config, overhang, 1.);
    } else if (dynamic_flow && config.flow_threshold_mm > 0.) {
        // Keep the exact legacy convention from apply_overhang_flow(): the
        // dynamic flow graph is sampled with distance/threshold, not with the
        // inverted "overlap percent" used by speed.
        const double start_ratio = graph_interpolate_or_default(
            config.dynamic_flow_graph, std::min(1., min_distance_mm / config.flow_threshold_mm), 0.);
        const double end_ratio = graph_interpolate_or_default(
            config.dynamic_flow_graph, std::min(1., max_distance_mm / config.flow_threshold_mm), 0.);
        apply_overhang_flow(attributes, config, overhang, 0.5 * (start_ratio + end_ratio));
    }

    entity.get_or_add(EPropertyAttributes::key) = attributes;
}

void append_unchanged_fragment(storage_handle *storage,
                               const ExtrusionEntity &source,
                               const Polyline &polyline,
                               const std::vector<c_point> &source_points,
                               const EPropertyAttributes &effective_attributes,
                               std::vector<Fragment> &fragments)
{
    if (polyline.size() < 2)
        return;

    // A disabled RegionSettings clip still owns a piece of the source path.
    // Clone that clipped piece exactly as it came in so region-specific
    // overhang settings never delete geometry outside their active area.
    fragments.emplace_back(storage, source, polyline.points(), effective_attributes,
                           distance_along_points(source_points, polyline.front()),
                           !same_points(polyline.points(), source_points));
    if (const EPropertyOverhang *overhang = source.get(EPropertyOverhang::key))
        fragments.back().entity.get_or_add(EPropertyOverhang::key) = *overhang;
}

class CurledLineProximity
{
public:
    explicit CurledLineProximity(const Layer &layer) : m_lines(layer.curled_lines())
    {
        for (const c_curled_line &line : m_lines)
            m_distancer.append_segment(line.a, line.b);
    }

    double proximity(c_point a, c_point b, double path_width_mm, double path_height_mm) const
    {
        if (m_lines.empty() || path_width_mm <= 0. || path_height_mm <= 0.)
            return 0.;

        const c_point middle = midpoint(a, b);
        const double dist_limit = scale_d(10. * path_width_mm);
        const std::vector<size_t> candidates = m_distancer.all_lines_in_radius(middle, dist_limit);
        double best = 0.;
        for (size_t idx : candidates) {
            if (idx >= m_lines.size())
                continue;
            double unused_projection = 0.;
            const double distance_mm =
                unscaled(std::sqrt(squared_distance_to_segment(middle, m_lines[idx].a, m_lines[idx].b, unused_projection)));
            const double normalized = std::max(0., 1. - distance_mm / (10. * path_width_mm));
            const double proximity =
                normalized * normalized * (double(m_lines[idx].curled_height) / (path_height_mm * 10.));
            best = std::max(best, proximity);
        }
        return best;
    }

private:
    std::vector<c_curled_line> m_lines;
    LineDistancer m_distancer;
};

PointSupportMetrics point_metrics(c_point point,
                                  const LineDistancer &support_distancer,
                                  double forced_distance_mm)
{
    PointSupportMetrics out = {};
    if (support_distancer.empty()) {
        out.distance_mm = forced_distance_mm;
        return out;
    }

    const double signed_distance = support_distancer.distance_from_lines(point, true);
    out.distance_mm = std::max(0., unscaled(signed_distance));
    return out;
}

void append_supported_fragment(storage_handle *storage,
                               const ExtrusionEntity &source,
                               const Polyline &polyline,
                               const std::vector<c_point> &source_points,
                               const EPropertyAttributes &effective_attributes,
                               const OverhangConfig &config,
                               std::vector<Fragment> &fragments)
{
    if (polyline.size() < 2)
        return;

    EPropertyAttributes attributes = effective_attributes;
    attributes.extrusion_role(attributes.extrusion_role() & ~RAW_EXTRUSION_ROLE_BRIDGE);
    fragments.emplace_back(storage, source, polyline.points(), attributes,
                           distance_along_points(source_points, polyline.front()), true,
                           minimum_split_length(config, effective_attributes));
    fragments.back().entity.remove(EPropertyOverhang::key);
}

void append_overhang_fragments(storage_handle *storage,
                               const ExtrusionEntity &source,
                               const Polyline &polyline,
                               const std::vector<c_point> &source_points,
                               const EPropertyAttributes &effective_attributes,
                               const OverhangConfig &config,
                               const LineDistancer &support_distancer,
                               const CurledLineProximity &curled_lines,
                               std::vector<Fragment> &fragments)
{
    if (polyline.size() < 2)
        return;

    const double forced_distance = std::max(config.max_threshold_mm(), config.nozzle_diameter_mm) + config.nozzle_diameter_mm;
    const coord_t sample_step = std::max<coord_t>(1, scale_i(std::max(0.05, config.nozzle_diameter_mm * 0.5)));
    const std::vector<c_point> points = densify_points(polyline.points(), sample_step);
    if (points.size() < 2)
        return;

    std::vector<PointSupportMetrics> metrics(points.size());
    for (size_t idx = 0; idx < points.size(); ++idx)
        metrics[idx] = point_metrics(points[idx], support_distancer, forced_distance);

    size_t begin = 0;
    while (begin + 1 < points.size()) {
        double min_distance = std::min(metrics[begin].distance_mm, metrics[begin + 1].distance_mm);
        double max_distance = std::max(metrics[begin].distance_mm, metrics[begin + 1].distance_mm);
        double curled = curled_lines.proximity(points[begin], points[begin + 1],
                                               effective_attributes.c_extrusion_property_attributes::width,
                                               effective_attributes.c_extrusion_property_attributes::height);
        size_t end = begin + 1;

        while (end + 1 < points.size()) {
            const double next_min = std::min(metrics[end].distance_mm, metrics[end + 1].distance_mm);
            const double next_max = std::max(metrics[end].distance_mm, metrics[end + 1].distance_mm);
            const double next_curled = curled_lines.proximity(points[end], points[end + 1],
                                                              effective_attributes.c_extrusion_property_attributes::width,
                                                              effective_attributes.c_extrusion_property_attributes::height);
            const bool similar_distance =
                std::abs(max_distance - next_max) <= k_distance_split_ratio * std::max(0.001, config.nozzle_diameter_mm);
            const bool similar_curled = std::abs(curled - next_curled) <= k_curled_split_epsilon;
            if (!similar_distance || !similar_curled)
                break;

            min_distance = std::min(min_distance, next_min);
            max_distance = std::max(max_distance, next_max);
            curled = std::max(curled, next_curled);
            ++end;
        }

        std::vector<c_point> fragment_points(points.begin() + begin, points.begin() + end + 1);
        EPropertyAttributes attributes = effective_attributes;
        fragments.emplace_back(storage, source, fragment_points, attributes,
                               distance_along_points(source_points, fragment_points.front()), true,
                               minimum_split_length(config, effective_attributes));
        apply_overhang_properties(attributes, fragments.back().entity, config, min_distance, max_distance, curled);

        begin = end;
    }
}

void append_region_fragments(storage_handle *storage,
                             const ExtrusionEntity &source,
                             const StoredPolyline &source_polyline,
                             const std::vector<c_point> &source_points,
                             const RegionSettingsClip &setting_clip,
                             const EPropertyAttributes &effective_attributes,
                             const OverhangConfig &config,
                             const ExPolygonCollection &supported_area,
                             const LineDistancer &support_distancer,
                             const CurledLineProximity &curled_lines,
                             std::vector<Fragment> &fragments)
{
    StoredPolylineCollection region_polylines = setting_clip.intersections(source_polyline);
    for (const Polyline region_polyline : region_polylines) {
        if (region_polyline.size() < 2)
            continue;

        if (!config.enabled) {
            append_unchanged_fragment(storage, source, region_polyline, source_points, effective_attributes, fragments);
            continue;
        }

        StoredPolylineCollection supported_polylines =
            clipper_intersection_polyline_expolygons(storage, region_polyline, supported_area);
        for (const Polyline supported : supported_polylines)
            append_supported_fragment(storage, source, supported, source_points, effective_attributes, config, fragments);

        StoredPolylineCollection overhang_polylines =
            clipper_diff_polyline_expolygons(storage, region_polyline, supported_area);
        for (const Polyline overhang : overhang_polylines)
            append_overhang_fragments(storage, source, overhang, source_points, effective_attributes, config,
                                      support_distancer, curled_lines, fragments);
    }
}

std::vector<Fragment> split_leaf(storage_handle *storage,
                                 const RegionSettings &settings,
                                 const LayerRegionIsland &region_island,
                                 const LineDistancer &support_distancer,
                                 const ExPolygonCollection &supported_area,
                                 const CurledLineProximity &curled_lines,
                                 MutableExtrusionEntity entity,
                                 const EPropertyAttributes &effective_attributes)
{
    std::vector<Fragment> fragments;
    const std::vector<c_point> source_points = entity.points();
    if (source_points.size() < 2 || entity.has_z_offsets())
        return fragments;

    const raw_extrusion_role role = raw_extrusion_role(effective_attributes.extrusion_role());
    if (!RAW_EXTRUSION_ROLE_IS_PERIMETER(role))
        return fragments;

    // All overhang options are consumed together when one extrusion fragment is
    // classified. Keep them in one RegionSettings group so each clipped area
    // carries a complete and internally consistent configuration tuple.
    StoredPolyline source_polyline = polyline_from_points(storage, source_points);
    const RegionSettings::AreaMap &areas = settings.get_areas(k_overhangs_key);
    const bool external_role = RAW_EXTRUSION_ROLE_IS_EXTERNAL(role);
    for (const auto &[setting_value, setting_clip] : areas) {
        const std::vector<LayerRegion> &regions = settings.get_regions(k_overhangs_key, setting_value);
        const LayerRegion region = !regions.empty() ? regions.front() : region_island.region(0);
        const OverhangConfig config = overhang_config_from_value(setting_value, region, external_role);

        append_region_fragments(storage, entity.readonly(), source_polyline, source_points,
                                setting_clip, effective_attributes, config,
                                supported_area, support_distancer, curled_lines, fragments);
    }

    std::sort(fragments.begin(), fragments.end(),
              [](const Fragment &lhs, const Fragment &rhs) { return lhs.order < rhs.order; });
    merge_tiny_fragments(fragments);
    return fragments;
}

bool fragments_need_replacement(const std::vector<Fragment> &fragments, const MutableExtrusionEntity &entity)
{
    if (fragments.empty())
        return false;
    if (fragments.size() != 1)
        return true;
    return fragments.front().changed || !same_points(fragments.front().entity.points(), entity.points());
}

void replace_root_leaf_with_fragments(MutableExtrusionEntity root, std::vector<Fragment> &fragments)
{
    if (fragments.empty())
        return;

    if (fragments.size() == 1) {
        const bool moved = extrusion_move_from(root.mutable_handle(), fragments.front().entity.mutable_handle()) != 0;
        assert(moved);
        (void)moved;
        return;
    }

    // In the new extrusion tree a leaf is a plain ExtrusionEntity, so replacing
    // its local polyline by ordered children is safe. The visitor intentionally
    // does not revisit these freshly inserted children during the same pass.
    const bool was_reversible = (root.flags() & RAW_EXTRUSION_FLAG_REVERSIBLE) != 0;
    root.clear_content();
    root.set_flags(was_reversible ? RAW_EXTRUSION_FLAG_REVERSIBLE : 0);
    for (Fragment &fragment : fragments)
        root.append_child_move(fragment.entity.mutable_view());
}

class DetectOverhangVisitor : public ExtrusionTreeVisitor<>
{
public:
    DetectOverhangVisitor(storage_handle *storage,
                          const RegionSettings &settings,
                          const LayerRegionIsland &region_island,
                          const LineDistancer &support_distancer,
                          const ExPolygonCollection &supported_area,
                          const CurledLineProximity &curled_lines) :
        m_storage(storage),
        m_settings(settings),
        m_region_island(region_island),
        m_support_distancer(support_distancer),
        m_supported_area(supported_area),
        m_curled_lines(curled_lines)
    {
        assert(m_storage != nullptr);
    }

protected:
    void visit_leaf(MutableExtrusionEntity entity) override
    {
        const EPropertyAttributes *attributes = current_property(EPropertyAttributes::key);
        if (attributes == nullptr)
            return;

        std::vector<Fragment> fragments =
            split_leaf(m_storage, m_settings, m_region_island, m_support_distancer,
                       m_supported_area, m_curled_lines, entity, *attributes);
        if (fragments_need_replacement(fragments, entity))
            replace_root_leaf_with_fragments(entity, fragments);
    }

private:
    storage_handle *m_storage = nullptr;
    const RegionSettings &m_settings;
    const LayerRegionIsland &m_region_island;
    const LineDistancer &m_support_distancer;
    const ExPolygonCollection &m_supported_area;
    const CurledLineProximity &m_curled_lines;
};

void process_region_island(const run_ctx_post_perimeter_generation &ctx,
                           storage_handle *storage,
                           const LayerRegionIsland &region_island,
                           const RegionSettings &settings,
                           const ExPolygonCollection &supported_area,
                           const LineDistancer &support_distancer,
                           const CurledLineProximity &curled_lines)
{
    if (ctx.get_region_island_mutable_extrusion == nullptr ||
        !region_island.has_extrusion(RAW_EXTRUSION_ROLE_PERIMETER))
        return;

    extrusion_entity_handle *root_handle =
        ctx.get_region_island_mutable_extrusion(region_island.handle(), RAW_EXTRUSION_ROLE_PERIMETER);
    if (root_handle == nullptr)
        return;

    MutableExtrusionEntity root(root_handle);
    DetectOverhangVisitor visitor(storage, settings, region_island,
                                  support_distancer, supported_area, curled_lines);
    visitor.traverse(root);
}

coord_t external_perimeter_width(const LayerIsland &island)
{
    if (island.region_count() == 0)
        return 0;
    return island.region(0).flow(RAW_EXTRUSION_ROLE_EXTERNAL_PERIMETER).width;
}

void process_island(const run_ctx_post_perimeter_generation &ctx,
                    storage_handle *storage,
                    const LayerIsland &island)
{
    if (island.region_count() == 0 || island.region_island_count() == 0)
        return;

    RegionSettings settings(storage, island,
                            {{k_overhangs_key,
                              k_overhangs_flow_ratio_key,
                              k_overhangs_width_key,
                              k_overhangs_dynamic_flow_key,
                              k_overhangs_type_key,
                              k_overhangs_width_speed_key,
                              k_overhangs_dynamic_speed_key}});
    settings.segregate(island.slice());

    const coord_t support_offset_width = external_perimeter_width(island);
    StoredExPolygonCollection supported_area = supported_centerline_area(storage, island, support_offset_width);
    LineDistancer support_distancer(supported_area.readonly());
    CurledLineProximity curled_lines(island.layer());

    for (uint32_t idx = 0; idx < island.region_island_count(); ++idx)
        process_region_island(ctx, storage, island.region_island(idx), settings,
                              supported_area.readonly(), support_distancer, curled_lines);
}

} // namespace

DetectOverhang &DetectOverhang::instance(orchestrator_handle *orch)
{
    static DetectOverhang s_instance(orch);
    return s_instance;
}

const char *DetectOverhang::id_impl() const noexcept
{
    return k_detect_overhang_id;
}

const char *DetectOverhang::name_impl() const noexcept
{
    return "Detect overhangs";
}

const char *DetectOverhang::description_impl() const noexcept
{
    return "Splits perimeter paths where they lose support and annotates those fragments for overhang flow and speed.";
}

slicing_step_t DetectOverhang::step_impl() const noexcept
{
    return STEP_POST_PERIMETER;
}

const char *const *DetectOverhang::dependencies_impl() const noexcept
{
    return k_no_dependencies;
}

int32_t DetectOverhang::priority_impl() const noexcept
{
    return 50;
}

int32_t DetectOverhang::used_config_keys(raw_used_config_key *keys) const noexcept
{
    if (keys != nullptr)
        for (uint32_t idx = 0; idx < sizeof(k_used_config_keys) / sizeof(k_used_config_keys[0]); ++idx)
            keys[idx] = k_used_config_keys[idx];
    return int32_t(sizeof(k_used_config_keys) / sizeof(k_used_config_keys[0]));
}

const char *DetectOverhang::progress_message_format_impl() const noexcept
{
    return "Detecting overhangs: %u / %u layers";
}

void DetectOverhang::setup_run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_post_perimeter_generation *ctx = plugin_ctx_as_post_perimeter_generation(run_ctx);
    if (ctx != nullptr && ctx->object != nullptr)
        progress().add_max(Object(ctx->object).layer_count());
}

void DetectOverhang::run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_post_perimeter_generation *ctx = plugin_ctx_as_post_perimeter_generation(run_ctx);
    if (ctx == nullptr || ctx->object == nullptr || run_ctx == nullptr || run_ctx->plugin_storage == nullptr)
        return;

    const Object object(ctx->object);
    for (uint32_t layer_idx = 0; layer_idx < object.layer_count(); ++layer_idx) {
        throw_if_cancelled(run_ctx);
        const Layer layer = object.layer(layer_idx);
        if (layer_idx > 0) {
            for (uint32_t island_idx = 0; island_idx < layer.island_count(); ++island_idx)
                process_island(*ctx, run_ctx->plugin_storage, layer.island(island_idx));
        }
        progress().increment();
    }
}

void register_detect_overhang_plugin(orchestrator_handle *orch)
{
    orchestrator_register_plugin(orch, DetectOverhang::instance(orch).c_instance());
}

}}} // namespace slic3r_api::Perimeter::DetectOverhangPlugin

#ifdef DETECT_OVERHANG_PLUGIN_DLL
extern "C" void register_plugin(orchestrator_handle *orch)
{
    slic3r_api::Perimeter::DetectOverhangPlugin::register_detect_overhang_plugin(orch);
}
#endif // DETECT_OVERHANG_PLUGIN_DLL
