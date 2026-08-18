///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

/*
Extrusion leaf splitting
========================

This translation unit implements the geometry-heavy part of
extrusion_split_leaf_by_areas(). It linearizes the source only for polygon
clipping, records every clipped interval in source traversal order, then cuts
the original ArcPolyline by distance. The final children therefore retain arc
segments and interpolated Z offsets instead of inheriting the temporary
straight approximation.

The supplied areas are a caller-owned partition. This code validates each
individual geometry, but intentionally does not test whether the collections
overlap or whether their union covers the path. Keeping that responsibility at
the producer avoids hiding configuration-partition errors in this low-level
mutation primitive.
*/

#include "libslic3r/Api/plugin/c/slic3r_extrusion_entity.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Geometry/ArcWelder.hpp"
#include "libslic3r/Polyline.hpp"

namespace Slic3r {

struct LinearizedExtrusionSegment
{
    Point chord_start;
    Point chord_end;
    Point source_segment_start;
    Geometry::ArcWelder::Segment source_segment_end;
    distf_t source_segment_offset = 0.;
    distf_t source_segment_length = 0.;
};

struct ExtrusionAreaSpan
{
    distf_t start = 0.;
    distf_t end = 0.;
    uint32_t area_index = 0;
};

static ExtrusionEntity *to_extrusion(extrusion_entity_handle *handle);
static extrusion_entity_handle *to_handle(ExtrusionEntity *entity);
static const ExPolygons *to_expolygons(const expolygon_collection_handle *handle);
static bool valid_area_collection(const ExPolygons &areas);
static std::vector<LinearizedExtrusionSegment> linearize_source(const ArcPolyline &source,
                                                               coord_t max_deviation);
static double projection_ratio(const Point &point, const Point &start, const Point &end);
static distf_t source_distance_for_point(const Point &point,
                                         const LinearizedExtrusionSegment &segment);
static void append_area_span(std::vector<ExtrusionAreaSpan> &spans,
                             ExtrusionAreaSpan span,
                             distf_t merge_tolerance);
static raw_extrusion_split_status clip_source_by_areas(
    const std::vector<LinearizedExtrusionSegment> &segments,
    const expolygon_collection_handle *const *areas,
    uint32_t area_count,
    coord_t max_deviation,
    std::vector<ExtrusionAreaSpan> &out_spans);
static ArcPolyline extract_source_span(const ArcPolyline &source,
                                       distf_t source_length,
                                       const ExtrusionAreaSpan &span);
static bool span_covers_source(const ExtrusionAreaSpan &span, distf_t source_length);
static void publish_split(ExtrusionEntity &leaf,
                          const ArcPolyline &source,
                          const std::vector<ExtrusionAreaSpan> &spans);

static ExtrusionEntity *to_extrusion(extrusion_entity_handle *handle)
{
    return reinterpret_cast<ExtrusionEntity *>(handle);
}

static extrusion_entity_handle *to_handle(ExtrusionEntity *entity)
{
    return reinterpret_cast<extrusion_entity_handle *>(entity);
}

static const ExPolygons *to_expolygons(const expolygon_collection_handle *handle)
{
    return reinterpret_cast<const ExPolygons *>(handle);
}

static bool valid_area_collection(const ExPolygons &areas)
{
    // An empty collection is a useful placeholder in a parallel area array.
    // Non-empty entries still have to satisfy the normal ExPolygon orientation
    // and contour validity rules before they are passed to Clipper.
    for (const ExPolygon &area : areas)
        if (area.empty() || !area.is_valid())
            return false;
    return true;
}

static std::vector<LinearizedExtrusionSegment> linearize_source(const ArcPolyline &source,
                                                               coord_t max_deviation)
{
    std::vector<LinearizedExtrusionSegment> out;
    const Geometry::ArcWelder::Path &path = source.get_arc();
    distf_t source_offset = 0.;

    // Every temporary chord remembers the original line or arc that it
    // approximates. Clipped chord endpoints can therefore be projected back
    // onto that native segment and converted to exact path distances later.
    for (size_t path_idx = 1; path_idx < path.size(); ++path_idx) {
        const Geometry::ArcWelder::Segment &source_start = path[path_idx - 1];
        const Geometry::ArcWelder::Segment &source_end = path[path_idx];
        const distf_t source_segment_length =
            Geometry::ArcWelder::segment_length<distf_t>(source_start, source_end);

        Points points;
        if (source_end.linear()) {
            points.push_back(source_start.point);
            points.push_back(source_end.point);
        } else {
            points = Geometry::ArcWelder::arc_discretize(source_start.point,
                                                         source_end.point,
                                                         source_end.radius,
                                                         source_end.ccw(),
                                                         double(max_deviation));
        }

        const size_t chord_count = points.size() > 1 ? points.size() - 1 : 0;
        for (size_t chord_idx = 0; chord_idx < chord_count; ++chord_idx) {
            out.push_back({ points[chord_idx],
                            points[chord_idx + 1],
                            source_start.point,
                            source_end,
                            source_offset,
                            source_segment_length });
        }

        source_offset += source_segment_length;
    }

    return out;
}

static double projection_ratio(const Point &point, const Point &start, const Point &end)
{
    const Vec2d direction = (end - start).cast<double>();
    const double length_squared = direction.squaredNorm();
    if (length_squared <= 0.)
        return 0.;

    const Vec2d from_start = (point - start).cast<double>();
    return std::clamp(from_start.dot(direction) / length_squared, 0., 1.);
}

static distf_t source_distance_for_point(const Point &point,
                                         const LinearizedExtrusionSegment &segment)
{
    if (segment.source_segment_end.linear()) {
        const double ratio = projection_ratio(point,
                                                segment.source_segment_start,
                                                segment.source_segment_end.point);
        return segment.source_segment_offset + segment.source_segment_length * ratio;
    }

    // The clipped point lies on a temporary chord, not necessarily on the
    // circle. Project it onto the original arc segment before converting it to
    // a native path distance, so the later split does not inherit chord error.
    Geometry::ArcWelder::Path source_segment = {
        Geometry::ArcWelder::Segment(segment.source_segment_start),
        segment.source_segment_end
    };
    const Geometry::ArcWelder::PathSegmentProjection projection =
        Geometry::ArcWelder::point_to_path_projection(source_segment, point);
    if (!projection.valid())
        throw std::runtime_error("Failed to project a clipped endpoint onto its source segment.");

    distf_t partial_length = 0.;
    if (projection.point == segment.source_segment_end.point) {
        partial_length = segment.source_segment_length;
    } else if (projection.point != segment.source_segment_start) {
        const Vec2d source_start = segment.source_segment_start.cast<double>();
        const Vec2d projected_point = projection.point.cast<double>();
        const Vec2d arc_center = projection.center.cast<double>();
        partial_length = Geometry::ArcWelder::arc_length<Vec2d, Vec2d, Vec2d, distf_t>(
            source_start,
            projected_point,
            arc_center,
            segment.source_segment_end.ccw());
    }

    return segment.source_segment_offset +
           std::clamp(partial_length, distf_t(0.), segment.source_segment_length);
}

static void append_area_span(std::vector<ExtrusionAreaSpan> &spans,
                             ExtrusionAreaSpan span,
                             distf_t merge_tolerance)
{
    if (span.end < span.start)
        std::swap(span.start, span.end);
    if (span.end - span.start <= distf_t(SCALED_EPSILON))
        return;

    // Consecutive chords of the same area are one logical fragment. Only
    // adjacent entries are merged, so the first and last spans of a closed
    // loop remain separated by the source seam.
    if (!spans.empty() && spans.back().area_index == span.area_index &&
        std::abs(span.start - spans.back().end) <= merge_tolerance) {
        spans.back().end = std::max(spans.back().end, span.end);
        return;
    }

    spans.push_back(span);
}

static raw_extrusion_split_status clip_source_by_areas(
    const std::vector<LinearizedExtrusionSegment> &segments,
    const expolygon_collection_handle *const *areas,
    uint32_t area_count,
    coord_t max_deviation,
    std::vector<ExtrusionAreaSpan> &out_spans)
{
    const distf_t merge_tolerance = distf_t(std::max(max_deviation, SCALED_EPSILON));

    try {
        // Clip each temporary chord independently. Iterating the chords in
        // source order makes the result deterministic without asking Clipper
        // to preserve or reconstruct traversal order for a complete polyline.
        for (const LinearizedExtrusionSegment &segment : segments) {
            std::vector<ExtrusionAreaSpan> local_spans;
            const Polyline chord(segment.chord_start, segment.chord_end);

            for (uint32_t area_idx = 0; area_idx < area_count; ++area_idx) {
                const ExPolygons &area = *to_expolygons(areas[area_idx]);
                if (area.empty())
                    continue;

                const Polylines clipped = intersection_pl(chord, area);
                for (const Polyline &fragment : clipped) {
                    if (fragment.size() < 2)
                        continue;

                    distf_t start_distance = source_distance_for_point(fragment.front(), segment);
                    distf_t end_distance = source_distance_for_point(fragment.back(), segment);
                    if (end_distance < start_distance)
                        std::swap(start_distance, end_distance);

                    local_spans.push_back({ start_distance, end_distance, area_idx });
                }
            }

            std::sort(local_spans.begin(), local_spans.end(),
                      [](const ExtrusionAreaSpan &lhs, const ExtrusionAreaSpan &rhs) {
                          if (lhs.start != rhs.start)
                              return lhs.start < rhs.start;
                          if (lhs.end != rhs.end)
                              return lhs.end < rhs.end;
                          return lhs.area_index < rhs.area_index;
                      });
            for (const ExtrusionAreaSpan &span : local_spans)
                append_area_span(out_spans, span, merge_tolerance);
        }
    } catch (...) {
        return RAW_EXTRUSION_SPLIT_STATUS_CLIPPING_FAILED;
    }

    return out_spans.empty() ? RAW_EXTRUSION_SPLIT_STATUS_CLIPPING_FAILED :
                               RAW_EXTRUSION_SPLIT_STATUS_SUCCESS;
}

static ArcPolyline extract_source_span(const ArcPolyline &source,
                                       distf_t source_length,
                                       const ExtrusionAreaSpan &span)
{
    const distf_t start = span.start <= distf_t(SCALED_EPSILON) ? 0. : span.start;
    const distf_t end = source_length - span.end <= distf_t(SCALED_EPSILON) ? source_length : span.end;

    ArcPolyline tail;
    if (start == 0.) {
        tail = source;
    } else {
        ArcPolyline prefix;
        source.split_at(start, prefix, tail);
    }

    // The remaining suffix is discarded after the second native split. Both
    // native cuts retain the exact source arcs and interpolate Z at their new
    // boundary points.
    if (end == source_length)
        return tail;

    ArcPolyline fragment;
    ArcPolyline suffix;
    tail.split_at(end - start, fragment, suffix);
    return fragment;
}

static bool span_covers_source(const ExtrusionAreaSpan &span, distf_t source_length)
{
    return span.start <= distf_t(SCALED_EPSILON) &&
           source_length - span.end <= distf_t(SCALED_EPSILON);
}

static void publish_split(ExtrusionEntity &leaf,
                          const ArcPolyline &source,
                          const std::vector<ExtrusionAreaSpan> &spans)
{
    const distf_t source_length = source.length();
    ExtrusionEntity replacement(leaf);
    replacement.clear_content();
    const bool can_reverse = leaf.can_reverse();

    // Clone the original leaf before changing the live tree. Each clone keeps
    // the dynamic extrusion type, direct properties and reversible flag; only
    // its local ArcPolyline is replaced by the exact source interval.
    for (const ExtrusionAreaSpan &span : spans) {
        std::unique_ptr<ExtrusionEntity> fragment(leaf.clone());
        fragment->set_polyline(extract_source_span(source, source_length, span));
        fragment->set_can_sort_reverse(false, can_reverse);
        replacement.append_child(std::move(fragment));
    }

    // Creating the first child initializes a generic collection as sortable.
    // Override that default only after all children exist so their source order
    // remains a mandatory continuous sequence.
    replacement.set_can_sort_reverse(false, can_reverse);

    // One move assignment is the publication point. Before this line every
    // failure leaves the caller's leaf byte-for-byte structurally unchanged.
    leaf = std::move(replacement);
}

} // namespace Slic3r

extern "C" raw_extrusion_split_status extrusion_split_leaf_by_areas(
    extrusion_entity_handle *leaf_handle,
    const expolygon_collection_handle *const *areas,
    uint32_t area_count,
    coord_t max_deviation,
    extrusion_split_fragment_fn on_fragment,
    void *user_data)
{
    if (leaf_handle == nullptr || areas == nullptr || area_count == 0)
        return RAW_EXTRUSION_SPLIT_STATUS_INVALID_ARGUMENT;

    Slic3r::ExtrusionEntity &leaf = *Slic3r::to_extrusion(leaf_handle);
    if (!leaf.is_leaf())
        return RAW_EXTRUSION_SPLIT_STATUS_NOT_A_LEAF;
    if (!leaf.has_polyline())
        return RAW_EXTRUSION_SPLIT_STATUS_INVALID_GEOMETRY;

    const Slic3r::ArcPolyline source = leaf.polyline_ref();
    // Reject empty and degenerate paths before invoking the stricter native
    // validity checker, which assumes that at least one path segment exists.
    if (source.size() < 2 || source.length() <= distf_t(SCALED_EPSILON) || !source.is_valid())
        return RAW_EXTRUSION_SPLIT_STATUS_INVALID_GEOMETRY;

    // Validate handles and individual geometries only. Whether these areas are
    // disjoint and cover the path is deliberately the caller's precondition.
    for (uint32_t area_idx = 0; area_idx < area_count; ++area_idx) {
        if (areas[area_idx] == nullptr)
            return RAW_EXTRUSION_SPLIT_STATUS_INVALID_ARGUMENT;
        if (!Slic3r::valid_area_collection(*Slic3r::to_expolygons(areas[area_idx])))
            return RAW_EXTRUSION_SPLIT_STATUS_INVALID_GEOMETRY;
    }

    try {
        max_deviation = std::max(max_deviation, coord_t(SCALED_EPSILON));
        const std::vector<Slic3r::LinearizedExtrusionSegment> segments =
            Slic3r::linearize_source(source, max_deviation);
        if (segments.empty())
            return RAW_EXTRUSION_SPLIT_STATUS_INVALID_GEOMETRY;

        std::vector<Slic3r::ExtrusionAreaSpan> spans;
        const raw_extrusion_split_status clipping_status =
            Slic3r::clip_source_by_areas(segments, areas, area_count, max_deviation, spans);
        if (clipping_status != RAW_EXTRUSION_SPLIT_STATUS_SUCCESS)
            return clipping_status;

        // Avoid a structural mutation for the overwhelmingly common case in
        // which one final settings area owns the complete extrusion leaf.
        if (spans.size() == 1 && Slic3r::span_covers_source(spans.front(), source.length())) {
            if (on_fragment != nullptr)
                on_fragment(leaf_handle, spans.front().area_index, user_data);
            return RAW_EXTRUSION_SPLIT_STATUS_SUCCESS;
        }

        Slic3r::publish_split(leaf, source, spans);
        if (on_fragment != nullptr)
            for (size_t fragment_idx = 0; fragment_idx < spans.size(); ++fragment_idx)
                on_fragment(Slic3r::to_handle(&leaf.child(fragment_idx)),
                            spans[fragment_idx].area_index,
                            user_data);
        return RAW_EXTRUSION_SPLIT_STATUS_SUCCESS;
    } catch (...) {
        return RAW_EXTRUSION_SPLIT_STATUS_INTERNAL_ERROR;
    }
}
