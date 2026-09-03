///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "ExtrusionTreeOrderingGeometry.hpp"

#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

#include "libslic3r/ExtrusionEntity.hpp"

namespace slic3r_api { namespace Ordering { namespace TreeOrderingGeometry {
namespace {

/*
Extrusion-tree ordering geometry
================================

Ordering providers decide where a path should begin and whether it should run
forward or backward. This module performs those decisions without adding a
second policy layer. Tree reversal preserves every existing handle. Loop
rotation projects an already validated seam onto lines or arcs, splits only the
containing path when necessary, and keeps segment metadata aligned.
*/

/* Find the nearest point on any local line or arc in one entity subtree. */
bool projected_point_on_entity(const ExtrusionEntity &entity,
                               c_point requested,
                               c_point &projected,
                               double &distance_squared);

/* Rotate a loop stored as one local polyline by splitting at its seam. */
void rotate_local_loop_to_seam(storage_handle *scratch_storage,
                               MutableExtrusionEntity loop,
                               c_point seam);

/* Compare two points without converting their scaled coordinates. */
double squared_distance(c_point lhs, c_point rhs);

bool projected_point_on_entity(const ExtrusionEntity &entity,
                               const c_point requested,
                               c_point &projected,
                               double &distance_squared_out)
{
    bool found_projection = false;
    double best_distance = (std::numeric_limits<double>::max)();
    c_point best_point = {};

    if (entity.point_count() > 0) {
        /* ArcWelder performs the same projection for straight and curved segments. */
        const Slic3r::ExtrusionEntity *core_entity =
            reinterpret_cast<const Slic3r::ExtrusionEntity *>(entity.handle());
        const Slic3r::ArcPolyline path = core_entity->as_polyline();
        const Slic3r::Geometry::ArcWelder::PathSegmentProjection projection =
            Slic3r::Geometry::ArcWelder::point_to_path_projection(
                path.get_arc(), Slic3r::Point(requested.x, requested.y));
        if (projection.valid()) {
            best_point = c_point{projection.point.x(), projection.point.y()};
            best_distance = projection.distance2;
            found_projection = true;
        }
    }

    for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx) {
        c_point child_point = {};
        double child_distance = (std::numeric_limits<double>::max)();
        if (projected_point_on_entity(
                entity.child(child_idx), requested, child_point, child_distance) &&
            (!found_projection || child_distance < best_distance)) {
            best_point = child_point;
            best_distance = child_distance;
            found_projection = true;
        }
    }

    if (found_projection) {
        projected = best_point;
        distance_squared_out = best_distance;
    }
    return found_projection;
}

void rotate_local_loop_to_seam(storage_handle *scratch_storage,
                               MutableExtrusionEntity loop,
                               const c_point seam)
{
    StoredExtrusionEntity before(scratch_storage, loop.readonly());
    StoredExtrusionEntity after(scratch_storage, loop.readonly());
    if (extrusion_polyline_split_at_point(
            loop.handle(), seam, before.mutable_handle(), after.mutable_handle()) == 0)
        throw std::runtime_error(
            "The local extrusion loop could not be split at its seam.");

    c_point projected = {};
    if (before.point_count() > 0)
        projected = before.local_back();
    else if (after.point_count() > 0)
        projected = after.local_front();
    else
        throw std::runtime_error(
            "The local extrusion loop produced no seam projection.");

    const double tolerance_squared =
        double(SCALED_EPSILON) * double(SCALED_EPSILON);
    if (squared_distance(seam, projected) > tolerance_squared)
        throw std::runtime_error(
            "The seam placer returned a point outside the local loop.");
    if (points_equal(projected, loop.local_front()) ||
        points_equal(projected, loop.local_back()))
        return;

    /* Recompose suffix + prefix while retaining arcs and endpoint Z offsets. */
    std::vector<c_extrusion_segment> rotated_segments = after.segments();
    const std::vector<c_extrusion_segment> prefix_segments = before.segments();
    rotated_segments.insert(
        rotated_segments.end(), prefix_segments.begin(), prefix_segments.end());
    if (!loop.set_segments(rotated_segments))
        throw std::runtime_error(
            "The local extrusion loop rejected its rotated segments.");
}

double squared_distance(const c_point lhs, const c_point rhs)
{
    const double dx = double(lhs.x) - double(rhs.x);
    const double dy = double(lhs.y) - double(rhs.y);
    return dx * dx + dy * dy;
}

} // namespace

void reverse_tree(MutableExtrusionEntity entity)
{
    if (entity.point_count() > 0) {
        if (!entity.reverse())
            throw std::runtime_error("The extrusion polyline could not be reversed.");
        return;
    }

    /* Reverse every path first, then reverse child order as one unit. */
    for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        reverse_tree(entity.child_mutable(child_idx));

    std::vector<const extrusion_entity_handle *> desired;
    std::vector<const extrusion_entity_handle *> current;
    desired.reserve(entity.child_count());
    current.reserve(entity.child_count());
    for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        current.push_back(entity.child(child_idx).handle());
    for (uint32_t child_idx = entity.child_count(); child_idx > 0; --child_idx)
        desired.push_back(entity.child(child_idx - 1).handle());

    for (uint32_t target_idx = 0; target_idx < desired.size(); ++target_idx) {
        if (current[target_idx] == desired[target_idx])
            continue;
        uint32_t source_idx = target_idx + 1;
        while (source_idx < current.size() && current[source_idx] != desired[target_idx])
            ++source_idx;
        if (source_idx == current.size() ||
            entity.move_child_from(target_idx, entity, source_idx) != target_idx)
            throw std::runtime_error(
                "The extrusion collection could not be reversed.");
        const extrusion_entity_handle *moved = current[source_idx];
        current.erase(current.begin() + source_idx);
        current.insert(current.begin() + target_idx, moved);
    }
}

void rotate_loop_to_seam(storage_handle *scratch_storage,
                         MutableExtrusionEntity loop,
                         const c_point seam)
{
    if (loop.point_count() > 0) {
        rotate_local_loop_to_seam(scratch_storage, loop, seam);
        return;
    }
    if (loop.child_count() == 0)
        throw std::runtime_error("A seam cannot be materialized on an empty loop.");

    /* Select the direct child nearest to the seam; child order breaks ties. */
    uint32_t containing_idx = EXTRUSION_INDEX_INVALID;
    c_point projected = {};
    double best_distance = (std::numeric_limits<double>::max)();
    for (uint32_t child_idx = 0; child_idx < loop.child_count(); ++child_idx) {
        c_point child_projection = {};
        double child_distance = (std::numeric_limits<double>::max)();
        if (projected_point_on_entity(
                loop.child(child_idx), seam, child_projection, child_distance) &&
            child_distance < best_distance) {
            containing_idx = child_idx;
            projected = child_projection;
            best_distance = child_distance;
        }
    }
    const double tolerance_squared =
        double(SCALED_EPSILON) * double(SCALED_EPSILON);
    if (containing_idx == EXTRUSION_INDEX_INVALID ||
        best_distance > tolerance_squared)
        throw std::runtime_error(
            "The seam placer returned a point outside the extrusion loop.");

    uint32_t start_idx = containing_idx;
    std::unique_ptr<StoredExtrusionEntity> closing_piece;
    MutableExtrusionEntity child = loop.child_mutable(containing_idx);
    if (child.point_count() > 0) {
        if (points_equal(projected, child.local_front())) {
            start_idx = containing_idx;
        } else if (points_equal(projected, child.local_back())) {
            start_idx = (containing_idx + 1) % loop.child_count();
        } else {
            /* Keep the source handle for the suffix and append a cloned prefix. */
            StoredExtrusionEntity before(scratch_storage, child.readonly());
            StoredExtrusionEntity after(scratch_storage, child.readonly());
            if (extrusion_polyline_split_at_point(
                    child.handle(), projected,
                    before.mutable_handle(), after.mutable_handle()) == 0 ||
                extrusion_move_from(child.mutable_handle(), after.mutable_handle()) == 0)
                throw std::runtime_error(
                    "The extrusion loop could not be split at its seam.");
            closing_piece.reset(new StoredExtrusionEntity(std::move(before)));
            start_idx = containing_idx;
        }
    } else {
        rotate_loop_to_seam(scratch_storage, child, projected);
    }

    /* Move the original prefix behind the selected child without cloning it. */
    for (uint32_t moved_count = 0; moved_count < start_idx; ++moved_count) {
        const uint32_t moved_idx = loop.move_child_from(loop.child_count(), loop, 0);
        if (moved_idx == EXTRUSION_INDEX_INVALID)
            throw std::runtime_error(
                "The extrusion loop rejected its seam rotation.");
    }
    if (closing_piece != nullptr &&
        loop.append_child_move(closing_piece->mutable_view()) == EXTRUSION_INDEX_INVALID)
        throw std::runtime_error(
            "The extrusion loop rejected its closing seam fragment.");
}

void disable_ordering_flags_recursively(MutableExtrusionEntity entity)
{
    for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        disable_ordering_flags_recursively(entity.child_mutable(child_idx));
    entity.disable_sort();
    entity.disable_reverse();
}

}}} // namespace slic3r_api::Ordering::TreeOrderingGeometry
