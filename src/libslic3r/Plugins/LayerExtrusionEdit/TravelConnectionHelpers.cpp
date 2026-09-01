///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

/*
Ordered travel connection implementation
========================================

This file centralizes the structural edits shared by travel providers. It
turns a gap into an ordered Travel sibling without cloning the old content and
keeps the resulting path continuous in XYZ, including closed seams and
per-point Z offsets.
*/

#include "TravelConnectionHelpers.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

#include "libslic3r/Api/plugin/cpp/ExtrusionViews.hpp"
#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"
#include "libslic3r/Api/plugin/cpp/properties/ExtrusionProperties.hpp"

namespace slic3r_api { namespace LayerExtrusionEdit { namespace TravelConnection {
namespace {

/* Add two scaled coordinates without allowing signed overflow. */
coord_t checked_add(coord_t lhs, coord_t rhs);

/* Subtract two scaled coordinates without allowing signed overflow. */
coord_t checked_subtract(coord_t lhs, coord_t rhs);

/* Resolve one point into absolute PrintingPlan XYZ coordinates. */
PlannedPosition absolute_position(c_point point,
                                  coord_t print_z,
                                  coord_t entity_z_offset,
                                  coord_t point_z_offset);

/* Return the scaled three-dimensional distance without overflowing coord_t. */
long double scaled_distance(const PlannedPosition &lhs, const PlannedPosition &rhs);

/* Detect a genuinely closed 3D leaf before a possible seam snap. */
bool leaf_is_closed(const MutableExtrusionEntity &entity,
                    const PlannedPosition &first,
                    const PlannedPosition &last);

/* Build one endpoint and a boundary-safe probe inside its source leaf. */
TravelEndpoint endpoint_from_leaf(const MutableExtrusionEntity &entity,
                                  const PlannedPosition &position,
                                  const LayerRegionIsland &region_island,
                                  uint16_t object_instance_idx,
                                  bool from_begin);

/* Move a sub-epsilon start onto the preceding endpoint. */
void snap_leaf_start(MutableExtrusionEntity entity,
                     const PlannedPosition &position,
                     coord_t print_z,
                     coord_t entity_z_offset,
                     bool closed);

/* Interpolate absolute Z along a provider-selected planar path. */
std::vector<coord_t> interpolated_z_offsets(const std::vector<c_point> &path,
                                            const PlannedPosition &from,
                                            const PlannedPosition &to,
                                            coord_t base_z);

/* Create one explicit ordered Travel leaf before existing content. */
void prepend_travel(MutableExtrusionEntity entity,
                    const std::vector<c_point> &path,
                    const PlannedPosition &from,
                    const PlannedPosition &to,
                    coord_t print_z,
                    coord_t parent_z_offset);

/* Traverse one tree and connect every geometric leaf in execution order. */
void connect_entity(MutableExtrusionEntity entity,
                    coord_t print_z,
                    coord_t inherited_z_offset,
                    const LayerRegionIsland &region_island,
                    uint16_t object_instance_idx,
                    uint16_t extruder_id,
                    PlannedPosition &position,
                    TravelEndpoint &previous_endpoint,
                    const TravelPathPlanner &planner);

coord_t checked_add(const coord_t lhs, const coord_t rhs)
{
    if ((rhs > 0 && lhs > (std::numeric_limits<coord_t>::max)() - rhs) ||
        (rhs < 0 && lhs < (std::numeric_limits<coord_t>::min)() - rhs))
        throw std::overflow_error("A travel endpoint exceeds the coord_t range.");
    return lhs + rhs;
}

coord_t checked_subtract(const coord_t lhs, const coord_t rhs)
{
    if ((rhs > 0 && lhs < (std::numeric_limits<coord_t>::min)() + rhs) ||
        (rhs < 0 && lhs > (std::numeric_limits<coord_t>::max)() + rhs))
        throw std::overflow_error("A travel Z offset exceeds the coord_t range.");
    return lhs - rhs;
}

PlannedPosition absolute_position(const c_point point,
                                  const coord_t print_z,
                                  const coord_t entity_z_offset,
                                  const coord_t point_z_offset)
{
    PlannedPosition position = {};
    position.x = point.x;
    position.y = point.y;
    position.z = checked_add(checked_add(print_z, entity_z_offset), point_z_offset);
    position.known = true;
    return position;
}

long double scaled_distance(const PlannedPosition &lhs, const PlannedPosition &rhs)
{
    const long double dx = static_cast<long double>(rhs.x) - static_cast<long double>(lhs.x);
    const long double dy = static_cast<long double>(rhs.y) - static_cast<long double>(lhs.y);
    const long double dz = static_cast<long double>(rhs.z) - static_cast<long double>(lhs.z);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool leaf_is_closed(const MutableExtrusionEntity &entity,
                    const PlannedPosition &first,
                    const PlannedPosition &last)
{
    return entity.point_count() > 1 && first.x == last.x && first.y == last.y && first.z == last.z;
}

TravelEndpoint endpoint_from_leaf(const MutableExtrusionEntity &entity,
                                  const PlannedPosition &position,
                                  const LayerRegionIsland &region_island,
                                  const uint16_t object_instance_idx,
                                  const bool from_begin)
{
    TravelEndpoint endpoint;
    endpoint.position = position;
    endpoint.region_island = region_island;
    endpoint.object_instance_idx = object_instance_idx;

    // The endpoint itself may lie on two touching regional slices. A short
    // point sampled inside the leaf gives providers an unambiguous fallback.
    const distf_t length = entity.length();
    const distf_t probe_distance = std::min(distf_t(SCALED_EPSILON), length / 2.0);
    if (probe_distance > 0.0) {
        endpoint.interior_point = from_begin ?
            entity.point_from_begin(probe_distance) : entity.point_from_end(probe_distance);
        endpoint.has_interior_point = true;
    }
    return endpoint;
}

void snap_leaf_start(MutableExtrusionEntity entity,
                     const PlannedPosition &position,
                     const coord_t print_z,
                     const coord_t entity_z_offset,
                     const bool closed)
{
    const coord_t base_z = checked_add(print_z, entity_z_offset);
    const coord_t snapped_z_offset = checked_subtract(position.z, base_z);
    const c_point snapped_point{position.x, position.y};

    // Rebuild from segments because set_point() intentionally linearizes arcs
    // adjacent to the edited point. Only seam coordinates change here.
    std::vector<c_extrusion_segment> segments = entity.segments();
    if (segments.empty())
        throw std::runtime_error("Unable to snap an extrusion leaf without segments.");
    segments.front().point_a = snapped_point;
    segments.front().z_offset_a = snapped_z_offset;
    if (closed) {
        segments.back().point_b = snapped_point;
        segments.back().z_offset_b = snapped_z_offset;
    }
    if (!entity.set_segments(segments))
        throw std::runtime_error("Unable to snap an extrusion start while preserving its segments.");
}

std::vector<coord_t> interpolated_z_offsets(const std::vector<c_point> &path,
                                            const PlannedPosition &from,
                                            const PlannedPosition &to,
                                            const coord_t base_z)
{
    std::vector<long double> cumulative(path.size(), 0.0L);
    for (size_t point_idx = 1; point_idx < path.size(); ++point_idx) {
        const long double dx = static_cast<long double>(path[point_idx].x) - path[point_idx - 1].x;
        const long double dy = static_cast<long double>(path[point_idx].y) - path[point_idx - 1].y;
        cumulative[point_idx] = cumulative[point_idx - 1] + std::sqrt(dx * dx + dy * dy);
    }

    std::vector<coord_t> offsets(path.size(), checked_subtract(from.z, base_z));
    const long double total = cumulative.back();
    for (size_t point_idx = 1; point_idx + 1 < path.size(); ++point_idx) {
        const long double ratio = total > 0.0L ? cumulative[point_idx] / total : 0.0L;
        const long double absolute_z = static_cast<long double>(from.z) +
            (static_cast<long double>(to.z) - from.z) * ratio;
        if (absolute_z < static_cast<long double>((std::numeric_limits<coord_t>::min)()) ||
            absolute_z > static_cast<long double>((std::numeric_limits<coord_t>::max)()))
            throw std::overflow_error("An interpolated travel Z exceeds the coord_t range.");
        offsets[point_idx] = checked_subtract(coord_t(std::llround(absolute_z)), base_z);
    }
    offsets.back() = checked_subtract(to.z, base_z);
    return offsets;
}

void prepend_travel(MutableExtrusionEntity entity,
                    const std::vector<c_point> &path,
                    const PlannedPosition &from,
                    const PlannedPosition &to,
                    const coord_t print_z,
                    const coord_t parent_z_offset)
{
    if (path.size() < 2 || path.front().x != from.x || path.front().y != from.y ||
        path.back().x != to.x || path.back().y != to.y)
        throw std::runtime_error("A travel path must preserve both requested endpoints.");

    MutableExtrusionEntity travel = entity.emplace_ordered_leaf(
        OrderedLeafPosition::Before,
        ExistingPropertyPlacement::MoveWithExistingContent);
    if (!travel.valid())
        throw std::runtime_error("Unable to insert an ordered travel before an extrusion leaf.");

    const coord_t base_z = checked_add(print_z, parent_z_offset);
    const std::vector<coord_t> z_offsets = interpolated_z_offsets(path, from, to, base_z);
    if (!travel.set_points(path))
        throw std::runtime_error("Unable to define the travel geometry.");
    for (uint32_t point_idx = 0; point_idx < uint32_t(path.size()); ++point_idx) {
        if (!travel.set_z_offset(point_idx, z_offsets[point_idx]))
            throw std::runtime_error("Unable to define a travel Z offset.");
    }

    EPropertyAttributes &attributes = travel.get_or_add(EPropertyAttributes::key);
    attributes.extrusion_role(RAW_EXTRUSION_ROLE_TRAVEL)
              .mm3_per_mm(0.0)
              .width(0.f)
              .height(0.f)
              .no_seam_enabled(false);
}

void connect_entity(MutableExtrusionEntity entity,
                    const coord_t print_z,
                    const coord_t inherited_z_offset,
                    const LayerRegionIsland &region_island,
                    const uint16_t object_instance_idx,
                    const uint16_t extruder_id,
                    PlannedPosition &position,
                    TravelEndpoint &previous_endpoint,
                    const TravelPathPlanner &planner)
{
    coord_t entity_z_offset = inherited_z_offset;
    if (const EPropertyZOffset *direct_z = entity.get(EPropertyZOffset::key))
        entity_z_offset = direct_z->get();

    if (entity.child_count() > 0) {
        const uint32_t child_count = entity.child_count();
        for (uint32_t child_idx = 0; child_idx < child_count; ++child_idx) {
            connect_entity(entity.child_mutable(child_idx), print_z, entity_z_offset,
                           region_island, object_instance_idx, extruder_id,
                           position, previous_endpoint, planner);
        }
        return;
    }
    if (entity.segment_count() == 0)
        return;

    const c_extrusion_segment first_segment = entity.segment(0);
    const c_extrusion_segment last_segment = entity.segment(entity.segment_count() - 1);
    PlannedPosition first = absolute_position(
        first_segment.point_a, print_z, entity_z_offset, first_segment.z_offset_a);
    PlannedPosition last = absolute_position(
        last_segment.point_b, print_z, entity_z_offset, last_segment.z_offset_b);
    const bool closed = leaf_is_closed(entity, first, last);

    if (position.known) {
        const long double gap = scaled_distance(position, first);
        if (gap < static_cast<long double>(SCALED_EPSILON)) {
            snap_leaf_start(entity, position, print_z, entity_z_offset, closed);
            first = position;
            if (closed)
                last = position;
        } else {
            const TravelEndpoint target = endpoint_from_leaf(
                entity, first, region_island, object_instance_idx, true);
            const std::vector<c_point> path = planner(previous_endpoint, target, extruder_id);
            prepend_travel(entity, path, position, first, print_z, inherited_z_offset);

            // Structural insertion turns entity into a wrapper. Child 1 is
            // the original leaf because the travel was inserted before it.
            previous_endpoint = endpoint_from_leaf(
                entity.child_mutable(1), last, region_island, object_instance_idx, false);
            position = last;
            return;
        }
    }

    previous_endpoint = endpoint_from_leaf(
        entity, last, region_island, object_instance_idx, false);
    position = last;
}

} // namespace

void add_layer_progress(const PrintingLayerGroup &layer, PluginProgress &progress)
{
    uint32_t extrusion_count = 0;
    for (uint32_t tool_idx = 0; tool_idx < layer.tool_group_count(); ++tool_idx)
        extrusion_count += layer.tool_group(tool_idx).extrusion_count();
    progress.add_max(extrusion_count);
}

void connect_layer(const PrintingLayerGroup &layer,
                   PlannedPosition &position,
                   const TravelPathPlanner &planner,
                   PluginProgress &progress)
{
    TravelEndpoint previous_endpoint;
    previous_endpoint.position = position;
    for (uint32_t tool_idx = 0; tool_idx < layer.tool_group_count(); ++tool_idx) {
        const PrintingToolGroup tool = layer.tool_group(tool_idx);
        for (uint32_t extrusion_idx = 0; extrusion_idx < tool.extrusion_count(); ++extrusion_idx) {
            const PrintingExtrusion extrusion = tool.extrusion(extrusion_idx);
            connect_entity(extrusion.mutable_root(), layer.print_z(), 0,
                           extrusion.region_island(), extrusion.object_instance_idx(),
                           tool.extruder_id(), position, previous_endpoint, planner);
            progress.increment();
        }
    }
}

std::vector<c_point> straight_path(const TravelEndpoint &source,
                                   const TravelEndpoint &target,
                                   uint16_t)
{
    return {
        c_point{source.position.x, source.position.y},
        c_point{target.position.x, target.position.y}
    };
}

}}} // namespace slic3r_api::LayerExtrusionEdit::TravelConnection
