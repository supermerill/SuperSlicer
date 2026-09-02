///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include <cstdint>
#include <utility>
#include <vector>

#include "libslic3r/Api/internal/ArcPolylineAccess.hpp"
#include "libslic3r/Api/plugin/c/slic3r_extrusion_polyline.h"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polyline.hpp"

namespace Slic3r {

static ExtrusionEntity *to_extrusion(extrusion_entity_handle *me)
{
    return reinterpret_cast<ExtrusionEntity *>(me);
}

static const ExtrusionEntity *to_extrusion(const extrusion_entity_handle *me)
{
    return reinterpret_cast<const ExtrusionEntity *>(me);
}

static Point to_point(c_point point)
{
    return Point(point.x, point.y);
}

static c_point to_c_point(const Point &point)
{
    c_point out = {};
    out.x = point.x();
    out.y = point.y();
    return out;
}

static bool same_point(c_point lhs, c_point rhs)
{
    return lhs.x == rhs.x && lhs.y == rhs.y;
}

static Geometry::ArcWelder::Orientation to_orientation(raw_extrusion_arc_orientation orientation)
{
    switch (orientation) {
    case RAW_EXTRUSION_ARC_ORIENTATION_CCW: return Geometry::ArcWelder::Orientation::CCW;
    case RAW_EXTRUSION_ARC_ORIENTATION_CW:  return Geometry::ArcWelder::Orientation::CW;
    case RAW_EXTRUSION_ARC_ORIENTATION_UNKNOWN:
    default:                                return Geometry::ArcWelder::Orientation::Unknown;
    }
}

static raw_extrusion_arc_orientation to_c_orientation(Geometry::ArcWelder::Orientation orientation)
{
    switch (orientation) {
    case Geometry::ArcWelder::Orientation::CCW: return RAW_EXTRUSION_ARC_ORIENTATION_CCW;
    case Geometry::ArcWelder::Orientation::CW:  return RAW_EXTRUSION_ARC_ORIENTATION_CW;
    case Geometry::ArcWelder::Orientation::Unknown:
    default:                                   return RAW_EXTRUSION_ARC_ORIENTATION_UNKNOWN;
    }
}

static const ArcPolyline *polyline_or_null(const extrusion_entity_handle *entity)
{
    return entity == nullptr ? nullptr : to_extrusion(entity)->polyline_or_null();
}

static ArcPolyline *polyline_or_null(extrusion_entity_handle *entity)
{
    return entity == nullptr ? nullptr : to_extrusion(entity)->polyline_or_null();
}

static ArcPolyline *polyline_for_replace(extrusion_entity_handle *entity)
{
    if (entity == nullptr)
        return nullptr;

    ExtrusionEntity *extrusion = to_extrusion(entity);
    if (!extrusion->is_leaf())
        return nullptr;
    if (!extrusion->has_polyline())
        extrusion->set_polyline(ArcPolyline());
    return extrusion->polyline_or_null();
}

static coord_t z_offset_or_zero(const ArcPolyline &polyline, size_t idx)
{
    const coord_t z_offset = polyline.z_offset(idx);
    return z_offset == ArcPolyline::INVALID_COORD ? coord_t(0) : z_offset;
}

static bool segment_is_valid(const c_extrusion_segment &segment)
{
    if (segment.radius == 0.f)
        return segment.orientation == RAW_EXTRUSION_ARC_ORIENTATION_UNKNOWN;
    return segment.orientation == RAW_EXTRUSION_ARC_ORIENTATION_CCW ||
           segment.orientation == RAW_EXTRUSION_ARC_ORIENTATION_CW;
}

static bool set_polyline_from_segments(ExtrusionEntity &entity,
                                       const c_extrusion_segment *segments,
                                       uint32_t count)
{
    if (!entity.is_leaf())
        return false;
    if (count == 0) {
        entity.clear_content();
        return true;
    }
    if (segments == nullptr)
        return false;

    bool has_z_offsets = false;
    for (uint32_t idx = 0; idx < count; ++idx) {
        if (!segment_is_valid(segments[idx]))
            return false;
        if (idx + 1 < count) {
            if (!same_point(segments[idx].point_b, segments[idx + 1].point_a))
                return false;
            if (segments[idx].z_offset_b != segments[idx + 1].z_offset_a)
                return false;
        }
        has_z_offsets = has_z_offsets ||
            segments[idx].z_offset_a != 0 ||
            segments[idx].z_offset_b != 0;
    }

    Geometry::ArcWelder::Path path;
    path.reserve(size_t(count) + 1);
    path.emplace_back(to_point(segments[0].point_a), 0.f, Geometry::ArcWelder::Orientation::Unknown);
    for (uint32_t idx = 0; idx < count; ++idx) {
        path.emplace_back(to_point(segments[idx].point_b),
                          segments[idx].radius,
                          to_orientation(segments[idx].orientation));
    }

    ArcPolyline polyline(path);
    // Publish Z before validating the complete path. Consecutive points may
    // legitimately share XY when their offsets describe a vertical move;
    // set_z_offset() marks the ArcPolyline as 3D for that validation.
    if (has_z_offsets) {
        polyline.set_z_offset(0, segments[0].z_offset_a);
        for (uint32_t idx = 0; idx < count; ++idx)
            polyline.set_z_offset(size_t(idx) + 1, segments[idx].z_offset_b);
    }
    ApiInternal::ArcPolylineAccess::refresh_after_bulk_replace(polyline);
    entity.set_polyline(std::move(polyline));
    return true;
}

static bool replace_output_polyline(extrusion_entity_handle *out, ArcPolyline &&polyline)
{
    if (out == nullptr)
        return false;

    ExtrusionEntity *entity = to_extrusion(out);
    if (!entity->is_leaf())
        return false;
    entity->set_polyline(std::move(polyline));
    return true;
}

} // namespace Slic3r

extern "C" {

uint32_t extrusion_polyline_point_count(const extrusion_entity_handle *entity)
{
    const Slic3r::ArcPolyline *polyline = Slic3r::polyline_or_null(entity);
    return polyline == nullptr ? 0 : static_cast<uint32_t>(polyline->size());
}

uint32_t extrusion_polyline_segment_count(const extrusion_entity_handle *entity)
{
    const uint32_t point_count = extrusion_polyline_point_count(entity);
    return point_count > 1 ? point_count - 1 : 0;
}

int32_t extrusion_polyline_clear(extrusion_entity_handle *entity)
{
    if (entity == nullptr)
        return 0;
    if (Slic3r::to_extrusion(entity)->has_polyline())
        Slic3r::to_extrusion(entity)->clear_content();
    return 1;
}

c_point extrusion_polyline_point(const extrusion_entity_handle *entity, uint32_t point_idx)
{
    const Slic3r::ArcPolyline *polyline = Slic3r::polyline_or_null(entity);
    if (polyline == nullptr || point_idx >= polyline->size())
        return {};
    return Slic3r::to_c_point(polyline->get_point(point_idx));
}

int32_t extrusion_polyline_set_point(extrusion_entity_handle *entity, uint32_t point_idx, c_point point)
{
    Slic3r::ArcPolyline *polyline = Slic3r::polyline_or_null(entity);
    return polyline != nullptr &&
        Slic3r::ApiInternal::ArcPolylineAccess::set_point(*polyline, point_idx, Slic3r::to_point(point));
}

uint32_t extrusion_polyline_insert_point(extrusion_entity_handle *entity, uint32_t point_idx, c_point point)
{
    Slic3r::ArcPolyline *polyline = Slic3r::polyline_for_replace(entity);
    if (polyline == nullptr || point_idx > polyline->size())
        return EXTRUSION_INDEX_INVALID;
    return Slic3r::ApiInternal::ArcPolylineAccess::insert_point(*polyline, point_idx, Slic3r::to_point(point)) ?
        point_idx :
        EXTRUSION_INDEX_INVALID;
}

int32_t extrusion_polyline_remove_point(extrusion_entity_handle *entity, uint32_t point_idx)
{
    Slic3r::ArcPolyline *polyline = Slic3r::polyline_or_null(entity);
    return polyline != nullptr &&
        Slic3r::ApiInternal::ArcPolylineAccess::remove_point(*polyline, point_idx);
}

uint32_t extrusion_polyline_find_point(const extrusion_entity_handle *entity, c_point point, coord_t max_distance)
{
    const Slic3r::ArcPolyline *polyline = Slic3r::polyline_or_null(entity);
    if (polyline == nullptr)
        return EXTRUSION_INDEX_INVALID;

    const int idx = polyline->find_point(Slic3r::to_point(point), coordf_t(max_distance));
    return idx < 0 ? EXTRUSION_INDEX_INVALID : static_cast<uint32_t>(idx);
}

uint32_t extrusion_polyline_copy_points(const extrusion_entity_handle *entity, c_point *dst, uint32_t dst_capacity)
{
    const Slic3r::ArcPolyline *polyline = Slic3r::polyline_or_null(entity);
    if (polyline == nullptr)
        return 0;

    const uint32_t count = static_cast<uint32_t>(polyline->size());
    if (dst != nullptr && dst_capacity >= count) {
        for (uint32_t idx = 0; idx < count; ++idx)
            dst[idx] = Slic3r::to_c_point(polyline->get_point(idx));
    }
    return count;
}

int32_t extrusion_polyline_set_points(extrusion_entity_handle *entity, const c_point *points, uint32_t count)
{
    if (entity == nullptr)
        return 0;

    Slic3r::ExtrusionEntity *extrusion = Slic3r::to_extrusion(entity);
    if (!extrusion->is_leaf())
        return 0;
    if (count == 0) {
        extrusion->clear_content();
        return 1;
    }
    if (points == nullptr)
        return 0;

    Slic3r::Points new_points;
    new_points.reserve(count);
    for (uint32_t idx = 0; idx < count; ++idx)
        new_points.emplace_back(points[idx].x, points[idx].y);
    extrusion->set_polyline(Slic3r::ArcPolyline(std::move(new_points)));
    return 1;
}

int32_t extrusion_polyline_segment(const extrusion_entity_handle *entity,
                                   uint32_t segment_idx,
                                   c_extrusion_segment *out_segment)
{
    const Slic3r::ArcPolyline *polyline = Slic3r::polyline_or_null(entity);
    if (polyline == nullptr || out_segment == nullptr || segment_idx + 1 >= polyline->size())
        return 0;

    const Slic3r::Geometry::ArcWelder::Segment &segment = polyline->get_arc(size_t(segment_idx) + 1);
    out_segment->point_a = Slic3r::to_c_point(polyline->get_point(segment_idx));
    out_segment->point_b = Slic3r::to_c_point(segment.point);
    out_segment->radius = segment.radius;
    out_segment->orientation = Slic3r::to_c_orientation(segment.orientation);
    out_segment->z_offset_a = Slic3r::z_offset_or_zero(*polyline, segment_idx);
    out_segment->z_offset_b = Slic3r::z_offset_or_zero(*polyline, size_t(segment_idx) + 1);
    return 1;
}

int32_t extrusion_polyline_set_segment(extrusion_entity_handle *entity,
                                       uint32_t segment_idx,
                                       const c_extrusion_segment *segment)
{
    if (segment == nullptr || !Slic3r::segment_is_valid(*segment))
        return 0;

    Slic3r::ArcPolyline *polyline = Slic3r::polyline_or_null(entity);
    if (polyline == nullptr)
        return 0;

    if (!Slic3r::ApiInternal::ArcPolylineAccess::set_segment(*polyline,
                                                             segment_idx,
                                                             Slic3r::to_point(segment->point_a),
                                                             Slic3r::to_point(segment->point_b),
                                                             segment->radius,
                                                             Slic3r::to_orientation(segment->orientation)))
        return 0;

    if (polyline->has_z_offset() || segment->z_offset_a != 0 || segment->z_offset_b != 0) {
        polyline->set_z_offset(segment_idx, segment->z_offset_a);
        polyline->set_z_offset(size_t(segment_idx) + 1, segment->z_offset_b);
    }
    return 1;
}

uint32_t extrusion_polyline_copy_segments(const extrusion_entity_handle *entity,
                                          c_extrusion_segment *dst,
                                          uint32_t dst_capacity)
{
    const uint32_t count = extrusion_polyline_segment_count(entity);
    if (dst != nullptr && dst_capacity >= count) {
        for (uint32_t idx = 0; idx < count; ++idx)
            extrusion_polyline_segment(entity, idx, &dst[idx]);
    }
    return count;
}

int32_t extrusion_polyline_set_segments(extrusion_entity_handle *entity,
                                        const c_extrusion_segment *segments,
                                        uint32_t count)
{
    if (entity == nullptr)
        return 0;
    return Slic3r::set_polyline_from_segments(*Slic3r::to_extrusion(entity), segments, count);
}

int32_t extrusion_polyline_split_at_point(const extrusion_entity_handle *entity,
                                          c_point point,
                                          extrusion_entity_handle *out_first,
                                          extrusion_entity_handle *out_second)
{
    const Slic3r::ArcPolyline *polyline = Slic3r::polyline_or_null(entity);
    if (polyline == nullptr)
        return 0;

    Slic3r::ArcPolyline first;
    Slic3r::ArcPolyline second;
    Slic3r::Point split_point = Slic3r::to_point(point);
    polyline->split_at(split_point, first, second);
    return Slic3r::replace_output_polyline(out_first, std::move(first)) &&
        Slic3r::replace_output_polyline(out_second, std::move(second));
}

int32_t extrusion_polyline_split_at_distance(const extrusion_entity_handle *entity,
                                             distf_t distance,
                                             extrusion_entity_handle *out_first,
                                             extrusion_entity_handle *out_second)
{
    const Slic3r::ArcPolyline *polyline = Slic3r::polyline_or_null(entity);
    if (polyline == nullptr)
        return 0;

    Slic3r::ArcPolyline first;
    Slic3r::ArcPolyline second;
    polyline->split_at(distance, first, second);
    return Slic3r::replace_output_polyline(out_first, std::move(first)) &&
        Slic3r::replace_output_polyline(out_second, std::move(second));
}

int32_t extrusion_polyline_split_at_index(const extrusion_entity_handle *entity,
                                          uint32_t point_idx,
                                          extrusion_entity_handle *out_first,
                                          extrusion_entity_handle *out_second)
{
    const Slic3r::ArcPolyline *polyline = Slic3r::polyline_or_null(entity);
    if (polyline == nullptr)
        return 0;

    Slic3r::ArcPolyline first;
    Slic3r::ArcPolyline second;
    if (!polyline->split_at_index(point_idx, first, second))
        return 0;
    return Slic3r::replace_output_polyline(out_first, std::move(first)) &&
        Slic3r::replace_output_polyline(out_second, std::move(second));
}

int32_t extrusion_polyline_clip_end(extrusion_entity_handle *entity, distf_t distance)
{
    Slic3r::ArcPolyline *polyline = Slic3r::polyline_or_null(entity);
    if (polyline == nullptr)
        return 0;
    polyline->clip_end(distance);
    return 1;
}

distf_t extrusion_polyline_length(const extrusion_entity_handle *entity)
{
    const Slic3r::ArcPolyline *polyline = Slic3r::polyline_or_null(entity);
    return polyline == nullptr ? 0. : polyline->length();
}

int32_t extrusion_polyline_translate(extrusion_entity_handle *entity, c_point offset)
{
    Slic3r::ArcPolyline *polyline = Slic3r::polyline_or_null(entity);
    if (polyline == nullptr)
        return 0;
    polyline->translate(Slic3r::Vector(offset.x, offset.y));
    return 1;
}

int32_t extrusion_polyline_rotate(extrusion_entity_handle *entity, double angle)
{
    Slic3r::ArcPolyline *polyline = Slic3r::polyline_or_null(entity);
    if (polyline == nullptr)
        return 0;
    polyline->rotate(angle);
    return 1;
}

c_point extrusion_polyline_point_from_end(const extrusion_entity_handle *entity, distf_t distance)
{
    const Slic3r::ArcPolyline *polyline = Slic3r::polyline_or_null(entity);
    if (polyline == nullptr || polyline->empty())
        return {};
    return Slic3r::to_c_point(polyline->get_point_from_end(distance));
}

int32_t extrusion_polyline_reverse(extrusion_entity_handle *entity)
{
    Slic3r::ArcPolyline *polyline = Slic3r::polyline_or_null(entity);
    if (polyline == nullptr)
        return 0;
    polyline->reverse();
    return 1;
}

int32_t extrusion_polyline_has_z_offsets(const extrusion_entity_handle *entity)
{
    const Slic3r::ArcPolyline *polyline = Slic3r::polyline_or_null(entity);
    return polyline != nullptr && polyline->has_z_offset();
}

int32_t extrusion_polyline_z_offset(const extrusion_entity_handle *entity, uint32_t point_idx, coord_t *out_z_offset)
{
    const Slic3r::ArcPolyline *polyline = Slic3r::polyline_or_null(entity);
    if (polyline == nullptr || out_z_offset == nullptr || point_idx >= polyline->size())
        return 0;
    *out_z_offset = Slic3r::z_offset_or_zero(*polyline, point_idx);
    return 1;
}

int32_t extrusion_polyline_set_z_offset(extrusion_entity_handle *entity, uint32_t point_idx, coord_t z_offset)
{
    Slic3r::ArcPolyline *polyline = Slic3r::polyline_or_null(entity);
    if (polyline == nullptr || point_idx >= polyline->size())
        return 0;
    polyline->set_z_offset(point_idx, z_offset);
    return 1;
}

int32_t extrusion_polyline_clear_z_offsets(extrusion_entity_handle *entity)
{
    Slic3r::ArcPolyline *polyline = Slic3r::polyline_or_null(entity);
    if (polyline == nullptr)
        return 0;
    Slic3r::ApiInternal::ArcPolylineAccess::clear_z_offsets(*polyline);
    return 1;
}

} // extern "C"
