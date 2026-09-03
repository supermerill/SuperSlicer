///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_extrusion_polyline_h_
#define slic3r_extrusion_polyline_h_

#include <stdint.h>

#include "slic3r_def.h"
#include "slic3r_geometry.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
Polyline access for extrusion entities.

An extrusion entity may either contain a local polyline or contain children.
Those two forms are exclusive: functions that create or replace the local
polyline fail if the entity already has children. Clear the entity content first
if switching from children to a local polyline is intentional.

The polyline is stored as points, optional arc data between consecutive points,
and optional Z offsets per point. A segment in this API is a true segment from
point_a to point_b, so a polyline with N points has N - 1 segments.

Developer guide:
[Using Unified Extrusion Entities](/doc/plugins/extrusions.md)
*/

typedef struct extrusion_entity_handle extrusion_entity_handle;

#ifndef EXTRUSION_INDEX_INVALID
#define EXTRUSION_INDEX_INVALID ((uint32_t)UINT32_MAX)
#endif

typedef enum raw_extrusion_arc_orientation {
    RAW_EXTRUSION_ARC_ORIENTATION_UNKNOWN = 0,
    RAW_EXTRUSION_ARC_ORIENTATION_CCW = 1,
    RAW_EXTRUSION_ARC_ORIENTATION_CW = 2
} raw_extrusion_arc_orientation;

typedef struct c_extrusion_segment {
    c_point point_a;
    c_point point_b;

    /*
    Radius of the arc from point_a to point_b.
    radius == 0 means a straight segment and orientation must be UNKNOWN.arcpolyline
    A non-zero radius means an arc and orientation must be CCW or CW.
    Positive radius selects the shorter arc, negative radius selects the longer arc.
    */
    float radius;
    raw_extrusion_arc_orientation orientation;

    /*
    Z offsets are attached to points, not to segments.
    If the host does not store explicit Z offsets, the exposed value is 0.
    When setting an array of segments, adjacent segments that share a point must
    also agree on that shared point's Z offset.
    */
    coord_t z_offset_a;
    coord_t z_offset_b;
} c_extrusion_segment;

/* Return the number of points in the local polyline. Returns 0 if absent. */
SLIC3R_HOST_API uint32_t extrusion_polyline_point_count(const extrusion_entity_handle *entity);

/* Return point_count - 1 when a polyline has at least two points, otherwise 0. */
SLIC3R_HOST_API uint32_t extrusion_polyline_segment_count(const extrusion_entity_handle *entity);

/* Remove the local polyline from the entity. Children and properties are unchanged. */
SLIC3R_HOST_API int32_t extrusion_polyline_clear(extrusion_entity_handle *entity);

/*
Read or write one point.

Writing a point keeps arc and Z arrays aligned with the point index. It does not
change the number of points.

extrusion_polyline_point() returns {0, 0} if entity is NULL, if the entity has no
local polyline, or if point_idx is invalid.
*/
SLIC3R_HOST_API c_point extrusion_polyline_point(const extrusion_entity_handle *entity, uint32_t point_idx);
SLIC3R_HOST_API int32_t extrusion_polyline_set_point(extrusion_entity_handle *entity, uint32_t point_idx, c_point point);

/*
Insert or remove one point.

Insertion at point_idx == point_count appends to the end. point_idx > point_count
is invalid and returns UINT32_MAX. When a point is inserted, its Z offset is
created as 0 if the polyline already has Z offsets.
*/
SLIC3R_HOST_API uint32_t extrusion_polyline_insert_point(extrusion_entity_handle *entity, uint32_t point_idx, c_point point);
SLIC3R_HOST_API int32_t extrusion_polyline_remove_point(extrusion_entity_handle *entity, uint32_t point_idx);

/*
Return the index of a point close enough to point.

max_distance is expressed in scaled coordinates. Returns EXTRUSION_INDEX_INVALID
if entity has no local polyline or if no point is close enough.
*/
SLIC3R_HOST_API uint32_t extrusion_polyline_find_point(const extrusion_entity_handle *entity, c_point point, coord_t max_distance);

/*
Copy points into dst and return the number of points required.

If dst is NULL or dst_capacity is too small, no partial write is required by the
host. Call once with dst == NULL to query the size, then again with a large
enough buffer.
*/
SLIC3R_HOST_API uint32_t extrusion_polyline_copy_points(const extrusion_entity_handle *entity, c_point *dst, uint32_t dst_capacity);

/*
Replace the local polyline with count straight points.

Fails if entity has children. Passing count == 0 removes the local polyline.
Any previous arc data and Z offsets are cleared.
*/
SLIC3R_HOST_API int32_t extrusion_polyline_set_points(extrusion_entity_handle *entity, const c_point *points, uint32_t count);

/*
Read or write one true segment.

Segment index i represents the path from point i to point i + 1.
Writing a segment may update both endpoint points, arc data, and Z offset data.

The segment is returned through out_segment instead of by value so callers may
reuse the same storage while scanning a path, and so invalid indices can be
reported without a sentinel segment value.
*/
SLIC3R_HOST_API int32_t extrusion_polyline_segment(const extrusion_entity_handle *entity,
                                                   uint32_t segment_idx,
                                                   c_extrusion_segment *out_segment);
SLIC3R_HOST_API int32_t extrusion_polyline_set_segment(extrusion_entity_handle *entity,
                                                       uint32_t segment_idx,
                                                       const c_extrusion_segment *segment);

/*
Copy true segments into dst and return the number of segments required.

The same two-call pattern as extrusion_polyline_copy_points() applies.
*/
SLIC3R_HOST_API uint32_t extrusion_polyline_copy_segments(const extrusion_entity_handle *entity,
                                                          c_extrusion_segment *dst,
                                                          uint32_t dst_capacity);

/*
Replace the local polyline from an array of true segments.

Fails if entity has children. count == 0 removes the local polyline.
For count > 0, segment[i].point_b must equal segment[i + 1].point_a for every
adjacent pair. Shared Z offsets must also match.

If all supplied Z offsets are 0, the host may keep the polyline without explicit
Z-offset storage.
*/
SLIC3R_HOST_API int32_t extrusion_polyline_set_segments(extrusion_entity_handle *entity,
                                                        const c_extrusion_segment *segments,
                                                        uint32_t count);

/*
Split the local polyline into two output entities.

The output entities receive local polylines. Their properties and flags are not
modified. The operation fails if an output entity has children, because replacing
an entity's local polyline while it has children is forbidden.
*/
SLIC3R_HOST_API int32_t extrusion_polyline_split_at_point(const extrusion_entity_handle *entity,
                                                          c_point point,
                                                          extrusion_entity_handle *out_first,
                                                          extrusion_entity_handle *out_second);
SLIC3R_HOST_API int32_t extrusion_polyline_split_at_distance(const extrusion_entity_handle *entity,
                                                             distf_t distance,
                                                             extrusion_entity_handle *out_first,
                                                             extrusion_entity_handle *out_second);
SLIC3R_HOST_API int32_t extrusion_polyline_split_at_index(const extrusion_entity_handle *entity,
                                                          uint32_t point_idx,
                                                          extrusion_entity_handle *out_first,
                                                          extrusion_entity_handle *out_second);

/*
Remove distance from the end of the local polyline.

distance is expressed in scaled coordinates. Arc and Z-offset data are kept in
sync by the host. Returns non-zero on success.
*/
SLIC3R_HOST_API int32_t extrusion_polyline_clip_end(extrusion_entity_handle *entity, distf_t distance);

/* Return the local polyline length in scaled coordinates, or 0 if absent. */
SLIC3R_HOST_API distf_t extrusion_polyline_length(const extrusion_entity_handle *entity);

/* Translate all points by offset. Returns non-zero on success. */
SLIC3R_HOST_API int32_t extrusion_polyline_translate(extrusion_entity_handle *entity, c_point offset);

/* Rotate all points around the origin. angle is expressed in radians. */
SLIC3R_HOST_API int32_t extrusion_polyline_rotate(extrusion_entity_handle *entity, double angle);

/*
Return a point located distance from the end of the local polyline.

distance is expressed in scaled coordinates. The returned point is a copy; the
polyline is not modified. Returns {0, 0} if entity is NULL or if the local
polyline is absent or empty.
*/
SLIC3R_HOST_API c_point extrusion_polyline_point_from_end(const extrusion_entity_handle *entity, distf_t distance);

/* Reverse point order, arc direction, and per-point Z offsets. */
SLIC3R_HOST_API int32_t extrusion_polyline_reverse(extrusion_entity_handle *entity);

/* Return non-zero if at least one point of the local polyline has a Z offset array. */
SLIC3R_HOST_API int32_t extrusion_polyline_has_z_offsets(const extrusion_entity_handle *entity);

/*
Read or write one point Z offset.

Setting a Z offset creates the point Z-offset array if it does not exist yet and
initializes other points to 0.
*/
SLIC3R_HOST_API int32_t extrusion_polyline_z_offset(const extrusion_entity_handle *entity, uint32_t point_idx, coord_t *out_z_offset);
SLIC3R_HOST_API int32_t extrusion_polyline_set_z_offset(extrusion_entity_handle *entity, uint32_t point_idx, coord_t z_offset);

/* Remove all per-point Z offsets from the local polyline. */
SLIC3R_HOST_API int32_t extrusion_polyline_clear_z_offsets(extrusion_entity_handle *entity);

#ifdef __cplusplus
}
#endif

#endif /* slic3r_extrusion_polyline_h_ */
