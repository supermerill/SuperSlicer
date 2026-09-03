///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_geometry_h_
#define slic3r_geometry_h_

#define SLIC3R_PLUGIN_API_GEOMETRY_MAJOR 1u
#define SLIC3R_PLUGIN_API_GEOMETRY_MINOR 0u

#include <stddef.h>
#include <stdint.h>

#include "slic3r_def.h"
#include "slic3r_utils.h"

#ifdef __cplusplus
extern "C" {
#endif

/* scalar coordinates for precise computation */
typedef int64_t coord_t;
/* fallback to double for big distance that may go over maximum value of coord_t */
typedef double distf_t;

/* ---- Point ---- */
typedef struct c_point
{
    coord_t x;
    coord_t y;
} c_point;

typedef struct c_vec3f {
    float x;
    float y;
    float z;
} c_vec3f;

typedef struct c_vec2d {
    double x;
    double y;
} c_vec2d;

typedef struct c_vec3d {
    double x;
    double y;
    double z;
} c_vec3d;

/*
4x4 transform matrix in row-major order:
    value[row * 4 + column]

It mirrors Eigen::Transform3d::matrix(), but keeps the ABI strictly C.
The matrix values are unscaled floating-point 3D coordinates. Do not use
coord_t here: coord_t is for scaled 2D/height coordinates in the slicer space.
*/
typedef struct c_matrix4d {
    double value[16];
} c_matrix4d;

SLIC3R_HOST_API distf_t c_point_distance_to(c_point lhs, c_point rhs);
SLIC3R_HOST_API distf_t c_point_distance_to_square(c_point lhs, c_point rhs);

/* Return an identity 4x4 matrix. */
SLIC3R_HOST_API c_matrix4d matrix4d_identity(void);

/* Return a 4x4 translation matrix in unscaled 3D coordinates. */
SLIC3R_HOST_API c_matrix4d matrix4d_translation(double x, double y, double z);

/* Compose two row-major ABI matrices. The returned matrix is lhs * rhs. */
SLIC3R_HOST_API c_matrix4d matrix4d_mul(c_matrix4d lhs, c_matrix4d rhs);

/* ---- BoundingBox ---- */
typedef struct c_bounding_box
{
    c_point min;
    c_point max;
} c_bounding_box;

typedef struct c_bounding_box3f
{
    c_vec3f min;
    c_vec3f max;
} c_bounding_box3f;

/* ---- MultiPoint & Polyline & Polygon ---- */
/*
Array of points. The size is fixed for this view.
The struct is only a view over the array and does not own it.
If the owner changes the size of the underlying array, this view becomes invalid.
*/
typedef struct multipoint_view
{
    /*
    This array is not owned by the struct, it is only a view over the array.
    The values may be modified if the view is mutable, but the array cannot be
    resized or deleted through this view.
    */
    c_point *array;
    uint32_t   size;
    uint32_t flags;
} multipoint_view;

/*
Array of points. The size is fixed for this view.
The struct is only a view over the array and does not own it.
If the owner changes the size of the underlying array, this view becomes invalid.
*/
typedef struct multipoint_const_view
{
    /*
    This array is not owned by the struct, it is only a view over the array.
    The values cannot be modified through this view, and the array cannot be
    resized or deleted through this view.
    */
    const c_point *array;
    uint32_t         size;
    uint32_t       flags;
} multipoint_const_view;

/* multipoint_view flags */
/* If set, this is a polygon. Otherwise it is a polyline. */
/* If set, consider the first point repeated at the end. */
#define MULTIPOINT_CLOSED 1u
/* For polygons, one of these orientation flags may be enforced. */
/* This influences the result of points_is_valid(). */
#define MULTIPOINT_CW  2u
#define MULTIPOINT_CCW 4u

/* Returns non-zero if the content is valid. */
SLIC3R_HOST_API int32_t points_is_valid(const multipoint_const_view *me);
SLIC3R_HOST_API int32_t points_intersection(const multipoint_const_view *me, c_point line_a, c_point line_b, c_point *out_intersection);
SLIC3R_HOST_API int32_t points_first_intersection(const multipoint_const_view *me, c_point line_a, c_point line_b, c_point *out_intersection);
/*
Returns the number of intersection points.
Stores them into out_intersections until it is full or all intersections are stored.
*/
SLIC3R_HOST_API uint32_t points_intersections(const multipoint_const_view *me, c_point line_a, c_point line_b, multipoint_view *out_intersections);
SLIC3R_HOST_API c_bounding_box points_bounding_box(const multipoint_const_view *me);
/* Returns the index of the closest point within max_distance, or -1 if none exists. */
SLIC3R_HOST_API int32_t points_find_point_index(const multipoint_const_view *me, c_point point_search, coord_t max_distance);
SLIC3R_HOST_API int32_t points_closest_point_index(const multipoint_const_view *me, c_point point_search);

/* Create a const view from a mutable view. */
static inline multipoint_const_view multipoint_view_as_const(multipoint_view me)
{
    multipoint_const_view out;
    out.array = me.array;
    out.size  = me.size;
    out.flags = me.flags;
    return out;
}

/* ---- MultiPoint ---- */
typedef struct multipoint_handle multipoint_handle;
typedef struct polygon_handle polygon_handle;
typedef struct polyline_handle polyline_handle;
typedef struct polygon_collection_handle polygon_collection_handle;

/* Explicit downcast to the shared MultiPoint ABI when a point-level function is needed. */
SLIC3R_HOST_API multipoint_handle *polygon_as_multipoint(polygon_handle *me);
SLIC3R_HOST_API const multipoint_handle *polygon_as_multipoint_const(const polygon_handle *me);
SLIC3R_HOST_API multipoint_handle *polyline_as_multipoint(polyline_handle *me);
SLIC3R_HOST_API const multipoint_handle *polyline_as_multipoint_const(const polyline_handle *me);

/* Getters */
SLIC3R_HOST_API uint32_t  multipoint_size(const multipoint_handle *me);
SLIC3R_HOST_API distf_t multipoint_length(const multipoint_handle *me);
SLIC3R_HOST_API int32_t multipoint_valid(const multipoint_handle *me);
/* Prefer multipoint_view() if accessing multiple points. */
SLIC3R_HOST_API c_point multipoint_get(const multipoint_handle *me, uint32_t idx);
/*
Fails if idx is not valid, then returns 0.
Prefer using the view if modifying many points without resizing.
*/
SLIC3R_HOST_API int32_t multipoint_set(multipoint_handle *me, uint32_t idx, c_point new_value);
SLIC3R_HOST_API int32_t multipoint_equals(const multipoint_handle *me, const multipoint_handle *other);

/*
Returns a non-owning view over the points contained in the MultiPoint.

The returned view is only valid as long as the underlying MultiPoint is not modified.
Any call to a function that takes a mutable multipoint_handle* may invalidate this view.
After such a call, the view must be considered invalid and must not be used.
*/
SLIC3R_HOST_API multipoint_view multipoint_view_mut(multipoint_handle *me);
SLIC3R_HOST_API multipoint_const_view multipoint_view_const(const multipoint_handle *me);
// set the MULTIPOINT_CW / MULTIPOINT_CCW flag according to the orientation of the points. It's expected that it exists a line between the last and first point. return true if counter-clockwise
SLIC3R_HOST_API int32_t multipoint_is_cw(multipoint_view *me);

/* Setters */
SLIC3R_HOST_API void multipoint_push_back(multipoint_handle *me, c_point point);
SLIC3R_HOST_API void multipoint_insert(multipoint_handle *me, uint32_t idx, c_point point);
/* Insert array_size elements from array at position idx. */
SLIC3R_HOST_API void multipoint_insert_array(multipoint_handle *me, uint32_t idx, const c_point *array, uint32_t array_size);
SLIC3R_HOST_API void multipoint_pop_back(multipoint_handle *me);
SLIC3R_HOST_API void multipoint_clear(multipoint_handle *me);
/* Remove elements in [begin_idx, begin_idx + erase_size). */
SLIC3R_HOST_API void multipoint_erase(multipoint_handle *me, uint32_t begin_idx, uint32_t erase_size);
/* Clear dst, then copy from src into dst. */
SLIC3R_HOST_API void multipoint_copy(multipoint_handle *dst, const multipoint_handle *src);

/* Modifiers */
SLIC3R_HOST_API void multipoint_scale(multipoint_handle *me, double factor);
SLIC3R_HOST_API void multipoint_scale_xy(multipoint_handle *me, double factor_x, double factor_y);
SLIC3R_HOST_API void multipoint_translate(multipoint_handle *me, double x, double y);
SLIC3R_HOST_API void multipoint_rotate_origin(multipoint_handle *me, double angle);
SLIC3R_HOST_API void multipoint_rotate_around(multipoint_handle *me, double angle, c_point center);
SLIC3R_HOST_API void multipoint_reverse(multipoint_handle *me);
SLIC3R_HOST_API void multipoint_densify(multipoint_handle *me, coord_t min_length);
/* deviation: distance from point to its projection on the segment formed by previous and next points. */
SLIC3R_HOST_API void multipoint_simplify(multipoint_handle *me, coord_t min_length, coord_t min_deviation);
/* Simplify the point sequence enough to remove duplicate / too-close points. */
SLIC3R_HOST_API void multipoint_ensure_valid(multipoint_handle *me, coord_t resolution);

/* ---- Polygon ---- */
// test if a multipoint is a polygon object (return false if not valid or null)
SLIC3R_HOST_API int32_t multipoint_is_polygon(const multipoint_handle *me);

/* create a new polygon in the storage, set its pointer into out_new_object and return its index. The new polygon is empty. */
SLIC3R_HOST_API polygon_handle *storage_new_polygon(storage_handle *me);

/* These methods require is_polygon() == true to work correctly or (multipoint_view_const().flags & MULTIPOINT_CLOSED) != 0 to work correctly. */

SLIC3R_HOST_API int32_t polygon_valid(const polygon_handle *me);

SLIC3R_HOST_API double polygon_area(const polygon_handle *me);
SLIC3R_HOST_API int32_t polygon_is_counter_clockwise(const polygon_handle *me);
SLIC3R_HOST_API int32_t polygon_is_clockwise(const polygon_handle *me);
SLIC3R_HOST_API int32_t polygon_make_counter_clockwise(polygon_handle *me);
SLIC3R_HOST_API int32_t polygon_make_clockwise(polygon_handle *me);
SLIC3R_HOST_API int32_t polygon_is_valid(const polygon_handle *me);

/* Does an unoriented polygon contain a point? */
SLIC3R_HOST_API int32_t polygon_contains(const polygon_handle *me, c_point point);
/* Approximate boundary test. */
SLIC3R_HOST_API int32_t polygon_on_boundary(const polygon_handle *me, c_point point, coord_t max_dist);

SLIC3R_HOST_API c_point polygon_centroid(const polygon_handle *me);

/*
Considering CCW orientation of this polygon
(it means that a ccw contour is mostly convex, while a cw hole is mostly concave),
find all convex or concave points with the angle at the vertex between two thresholds.
*/
SLIC3R_HOST_API uint32_t polygon_concave_points_idx(
    const polygon_handle *me, double min_angle, double max_angle, uint32_t *out, uint32_t max_size);
SLIC3R_HOST_API uint32_t polygon_convex_points_idx(
    const polygon_handle *me, double min_angle, double max_angle, uint32_t *out, uint32_t max_size);
/* Projection of a point onto the polygon using the shortest distance. */
SLIC3R_HOST_API c_point polygon_point_projection(const polygon_handle *me, c_point point, uint32_t *out_idx);
/*
Build a convex hull from the points of one polygon or a polygon collection.

The result is a new storage-owned polygon. The input orientation and holes are
ignored: these helpers look only at the point cloud, which is what skirt and
first-layer-envelope algorithms need.
*/
SLIC3R_HOST_API polygon_handle *polygon_convex_hull(storage_handle *storage, const polygon_handle *me);
SLIC3R_HOST_API polygon_handle *polygons_convex_hull(storage_handle *storage, const polygon_collection_handle *me);
/*
Move polygon contents from src into dst, then leave src empty but still valid.
Both handles must be mutable. This moves the geometry, not ownership of either
handle. If dst and src are the same handle, the function does nothing.
*/
SLIC3R_HOST_API void polygon_move(polygon_handle *dst, polygon_handle *src);

/* ---- Polyline ---- */
// test if a multipoint is a polyline object (return false if not valid or null)
SLIC3R_HOST_API int32_t multipoint_is_polyline(const multipoint_handle *me);

/* create a new polyline in the storage, set its pointer into out_new_object and return its index. The new polyline is empty. */
SLIC3R_HOST_API polyline_handle *storage_new_polyline(storage_handle *me);

/* These methods require is_polyline() == true  or (multipoint_view_const().flags & MULTIPOINT_CLOSED) == 0 to work correctly. */
SLIC3R_HOST_API int32_t polyline_valid(const polyline_handle *me);

SLIC3R_HOST_API void polyline_clip_end(polyline_handle *me, distf_t distance);
SLIC3R_HOST_API void polyline_clip_start(polyline_handle *me, distf_t distance);
SLIC3R_HOST_API void polyline_extend_end(polyline_handle *me, distf_t distance);
SLIC3R_HOST_API void polyline_extend_start(polyline_handle *me, distf_t distance);
/*
Move polyline contents from src into dst, then leave src empty but still valid.
Both handles must be mutable. Element handles previously borrowed
from either collection may be invalidated because the destination is replaced
and the source is cleared. If dst and src are the same handle, nothing changes.
*/
SLIC3R_HOST_API void polyline_move(polyline_handle *dst, polyline_handle *src);

/* --- Polygon / Polyline / MultiPoint conversions --- */

/* Split a closed polygon into an open polyline, with the split point duplicated at both ends. */
SLIC3R_HOST_API int32_t polygon_split(const polygon_handle *polygon_me, uint32_t index, polyline_handle *polyline_out);

/*
If front == back, create a polygon with current lines.
Otherwise add a new line from back to front to create the polygon.
*/
SLIC3R_HOST_API int32_t polyline_close(const polyline_handle *me, polygon_handle *out);

/* ---- Polygon Collection ---- */
/* Create a new polygon collection in the storage. */
SLIC3R_HOST_API polygon_collection_handle *storage_new_polygons(storage_handle *me);

SLIC3R_HOST_API int32_t polygons_valid(const polygon_collection_handle *me);

/* Getters */
SLIC3R_HOST_API uint32_t polygons_size(const polygon_collection_handle *me);
/*
The returned polygon_handle is invalidated by any call that may modify the
polygon_collection_handle.
*/
SLIC3R_HOST_API polygon_handle *polygons_at(polygon_collection_handle *me, uint32_t idx);
SLIC3R_HOST_API const polygon_handle *polygons_at_const(const polygon_collection_handle *me, uint32_t idx);
SLIC3R_HOST_API int32_t polygons_equals(const polygon_collection_handle *me, const polygon_collection_handle *other);

/* Setters */
/* Resize: when shrinking, keep first polygons; when growing, add empty polygons to the back. */
SLIC3R_HOST_API void polygons_resize(polygon_collection_handle *me, uint32_t new_size);
/* Same as polygons_resize(me, polygons_size(me) + 1). */
SLIC3R_HOST_API void polygons_emplace_back(polygon_collection_handle *me);
/* Same as polygons_resize(me, polygons_size(me) - 1). */
SLIC3R_HOST_API void polygons_pop_back(polygon_collection_handle *me);
SLIC3R_HOST_API void polygons_clear(polygon_collection_handle *me);
SLIC3R_HOST_API void polygons_erase(polygon_collection_handle *me, uint32_t begin_idx, uint32_t erase_size);
/*
Insert a copy of src before idx. idx is clamped to the end of dst.
Any polygon_handle previously borrowed from dst may be invalidated because the
underlying storage may reallocate or move its elements.
*/
SLIC3R_HOST_API void polygons_insert_copy(polygon_collection_handle *dst, uint32_t idx, const polygon_handle *src);
/*
Append copies of all polygons from src to dst. src is not modified.
Any polygon_handle previously borrowed from dst may be invalidated.
*/
SLIC3R_HOST_API void polygons_append_copy(polygon_collection_handle *dst, const polygon_collection_handle *src);
/*
Insert src before idx by moving its content, then leave src empty but still
valid. idx is clamped to the end of dst. Any polygon_handle previously borrowed
from dst may be invalidated.
*/
SLIC3R_HOST_API void polygons_insert_move(polygon_collection_handle *dst, uint32_t idx, polygon_handle *src);
/*
Append all polygons from src to dst by moving their contents, then leave src
empty but still valid. Any polygon_handle previously borrowed from either
collection may be invalidated. If dst and src are the same handle, nothing
changes.
*/
SLIC3R_HOST_API void polygons_append_move(polygon_collection_handle *dst, polygon_collection_handle *src);
/* Clear dst, then copy polygons from src into dst. */
SLIC3R_HOST_API void polygons_copy(polygon_collection_handle *dst, const polygon_collection_handle *src);
/*
Move all polygons from src into dst, then leave src empty but still valid.
Both collection handles must be mutable. Element handles previously borrowed
from either collection may be invalidated because the destination is replaced
and the source is cleared. If dst and src are the same handle, nothing changes.
*/
SLIC3R_HOST_API void polygons_move(polygon_collection_handle *dst, polygon_collection_handle *src);

/* Utility operations */
SLIC3R_HOST_API int32_t polygons_on_boundary(const polygon_collection_handle *me, c_point point, coord_t max_distance);
SLIC3R_HOST_API c_point polygons_point_projection(const polygon_collection_handle *me, c_point point);

/* ---- Polyline Collection ---- */
typedef struct polyline_collection_handle polyline_collection_handle;

/* Create a new polyline collection in the storage. */
SLIC3R_HOST_API polyline_collection_handle *storage_new_polylines(storage_handle *me);

SLIC3R_HOST_API int32_t polylines_valid(const polyline_collection_handle *me);

/* Getters */
SLIC3R_HOST_API uint32_t polylines_size(const polyline_collection_handle *me);
/*
The returned polyline_handle is invalidated by any call that may modify the
polyline_collection_handle.
*/
SLIC3R_HOST_API polyline_handle *polylines_at(polyline_collection_handle *me, uint32_t idx);
SLIC3R_HOST_API const polyline_handle *polylines_at_const(const polyline_collection_handle *me, uint32_t idx);
SLIC3R_HOST_API int32_t polylines_equals(const polyline_collection_handle *me, const polyline_collection_handle *other);

/* Setters */
/* Resize: when shrinking, keep first polylines; when growing, add empty polylines to the back. */
SLIC3R_HOST_API void polylines_resize(polyline_collection_handle *me, uint32_t new_size);
/* Same as polylines_resize(me, polylines_size(me) + 1). */
SLIC3R_HOST_API void polylines_emplace_back(polyline_collection_handle *me);
/* Same as polylines_resize(me, polylines_size(me) - 1). */
SLIC3R_HOST_API void polylines_pop_back(polyline_collection_handle *me);
SLIC3R_HOST_API void polylines_clear(polyline_collection_handle *me);
SLIC3R_HOST_API void polylines_erase(polyline_collection_handle *me, uint32_t begin_idx, uint32_t erase_size);
/*
Insert a copy of src before idx. idx is clamped to the end of dst.
Any polyline_handle previously borrowed from dst may be invalidated because the
underlying storage may reallocate or move its elements.
*/
SLIC3R_HOST_API void polylines_insert_copy(polyline_collection_handle *dst, uint32_t idx, const polyline_handle *src);
/*
Append copies of all polylines from src to dst. src is not modified.
Any polyline_handle previously borrowed from dst may be invalidated.
*/
SLIC3R_HOST_API void polylines_append_copy(polyline_collection_handle *dst, const polyline_collection_handle *src);
/*
Insert src before idx by moving its content, then leave src empty but still
valid. idx is clamped to the end of dst. Any polyline_handle previously borrowed
from dst may be invalidated.
*/
SLIC3R_HOST_API void polylines_insert_move(polyline_collection_handle *dst, uint32_t idx, polyline_handle *src);
/*
Append all polylines from src to dst by moving their contents, then leave src
empty but still valid. Any polyline_handle previously borrowed from either
collection may be invalidated. If dst and src are the same handle, nothing
changes.
*/
SLIC3R_HOST_API void polylines_append_move(polyline_collection_handle *dst, polyline_collection_handle *src);
/* Clear dst, then copy polylines from src into dst. */
SLIC3R_HOST_API void polylines_copy(polyline_collection_handle *dst, const polyline_collection_handle *src);
/*
Move all polylines from src into dst, then leave src empty but still valid.
Both collection handles must be mutable. Element handles previously borrowed
from either collection may be invalidated because the destination is replaced
and the source is cleared. If dst and src are the same handle, nothing changes.
*/
SLIC3R_HOST_API void polylines_move(polyline_collection_handle *dst, polyline_collection_handle *src);

/* ---- ExPolygon ---- */
/* ExPolygon has one contour polygon and zero or more hole polygons. */
/* The contour must be CCW and the holes must be CW. */
typedef struct expolygon_handle expolygon_handle;

typedef int32_t expolygon_status;
#define EXPOLYGON_STATUS_OK                  0
#define EXPOLYGON_STATUS_EMPTY               1
#define EXPOLYGON_STATUS_INVALID_ORIENTATION 2
#define EXPOLYGON_STATUS_NOT_CLOSED          3
#define EXPOLYGON_STATUS_INVALID_GEOMETRY    4

SLIC3R_HOST_API expolygon_status expolygon_valid(const expolygon_handle *me);
SLIC3R_HOST_API void expolygon_ensure_valid(expolygon_handle *me, coord_t resolution);

/* create a new expolygon in the storage, set its pointer into out_new_object and return its index. The new expolygon is empty. */
SLIC3R_HOST_API expolygon_handle *storage_new_expolygon(storage_handle *me);

/* Getters */
SLIC3R_HOST_API polygon_handle *expolygon_contour(expolygon_handle *me);
SLIC3R_HOST_API const polygon_handle *expolygon_contour_const(const expolygon_handle *me);
SLIC3R_HOST_API uint32_t expolygon_hole_size(const expolygon_handle *me);
SLIC3R_HOST_API polygon_handle *expolygon_hole_at(expolygon_handle *me, uint32_t hole_idx);
SLIC3R_HOST_API const polygon_handle *expolygon_hole_at_const(const expolygon_handle *me, uint32_t hole_idx);
SLIC3R_HOST_API int32_t expolygon_equals(const expolygon_handle *me, const expolygon_handle *other);

/* Setters */
/* Resize: when shrinking, keep first holes; when growing, add empty holes to the back. */
SLIC3R_HOST_API void expolygon_holes_resize(expolygon_handle *me, uint32_t new_size);
/* Same as expolygon_holes_resize(me, expolygon_hole_size(me) + 1). */
SLIC3R_HOST_API void expolygon_holes_emplace_back(expolygon_handle *me);
/* Same as expolygon_holes_resize(me, expolygon_hole_size(me) - 1). */
SLIC3R_HOST_API void expolygon_holes_pop_back(expolygon_handle *me);
SLIC3R_HOST_API void expolygon_holes_clear(expolygon_handle *me);
SLIC3R_HOST_API void expolygon_holes_erase(expolygon_handle *me, uint32_t begin_idx, uint32_t erase_size);
/* Clear dst, then copy from src into dst. */
SLIC3R_HOST_API void expolygon_copy(expolygon_handle *dst, const expolygon_handle *src);
/*
Move ExPolygon contents from src into dst, then leave src empty but still valid.
Both handles must be mutable. Element handles previously borrowed
from either collection may be invalidated because the destination is replaced
and the source is cleared. If dst and src are the same handle, nothing changes.
*/
SLIC3R_HOST_API void expolygon_move(expolygon_handle *dst, expolygon_handle *src);

SLIC3R_HOST_API double expolygon_area(const expolygon_handle *me);
/* Return non-zero when point is inside the ExPolygon or on its boundary. */
SLIC3R_HOST_API int32_t expolygon_contains(const expolygon_handle *me, c_point point);
/*
Returns non-zero if this expolygon overlaps another expolygon.

Either the expolygons intersect, or one is fully inside the other and not inside
a hole of the other expolygon.

The test may not be commutative if the two expolygons touch only at a boundary.
*/
SLIC3R_HOST_API int32_t expolygon_overlaps(const expolygon_handle *me, const expolygon_handle *other);

/* Conversion: clear dst, then copy the contour and holes from src into dst. */
SLIC3R_HOST_API void expolygon_to_polygons(const expolygon_handle *src, polygon_collection_handle *dst);
/*
Clear dst, then copy the first Polygon into the contour of dst, and the others
into holes. Stops on failure and returns the failure status.
*/
SLIC3R_HOST_API expolygon_status polygons_to_expolygon(const polygon_collection_handle *src, expolygon_handle *dst);

/* ---- ExPolygon Collection ---- */
typedef struct expolygon_collection_handle expolygon_collection_handle;

/* create a new expolygon collection in the storage. */
SLIC3R_HOST_API expolygon_collection_handle *storage_new_expolygons(storage_handle *me);

SLIC3R_HOST_API expolygon_status expolygons_valid(const expolygon_collection_handle *me);
SLIC3R_HOST_API void expolygons_ensure_valid(expolygon_collection_handle *me, coord_t resolution);

SLIC3R_HOST_API uint32_t expolygons_size(const expolygon_collection_handle *me);
SLIC3R_HOST_API expolygon_handle *expolygons_at(expolygon_collection_handle *me, uint32_t i);
SLIC3R_HOST_API const expolygon_handle *expolygons_at_const(const expolygon_collection_handle *me, uint32_t i);
SLIC3R_HOST_API int32_t expolygons_equals(const expolygon_collection_handle *me, const expolygon_collection_handle *other);

/* Setters */
/* Resize: when shrinking, keep first expolygons; when growing, add empty expolygons to the back. */
SLIC3R_HOST_API void expolygons_resize(expolygon_collection_handle *me, uint32_t new_size);
/* Same as expolygons_resize(me, expolygons_size(me) + 1). */
SLIC3R_HOST_API void expolygons_emplace_back(expolygon_collection_handle *me);
/* Same as expolygons_resize(me, expolygons_size(me) - 1). */
SLIC3R_HOST_API void expolygons_pop_back(expolygon_collection_handle *me);
SLIC3R_HOST_API void expolygons_clear(expolygon_collection_handle *me);
SLIC3R_HOST_API void expolygons_erase(expolygon_collection_handle *me, uint32_t begin_idx, uint32_t erase_size);
/*
Insert a copy of src before idx. idx is clamped to the end of dst.
Any expolygon_handle previously borrowed from dst may be invalidated because the
underlying storage may reallocate or move its elements.
*/
SLIC3R_HOST_API void expolygons_insert_copy(expolygon_collection_handle *dst, uint32_t idx, const expolygon_handle *src);
/*
Append copies of all ExPolygons from src to dst. src is not modified.
Any expolygon_handle previously borrowed from dst may be invalidated.
*/
SLIC3R_HOST_API void expolygons_append_copy(expolygon_collection_handle *dst, const expolygon_collection_handle *src);
/*
Insert src before idx by moving its content, then leave src empty but still
valid. idx is clamped to the end of dst. Any expolygon_handle previously
borrowed from dst may be invalidated.
*/
SLIC3R_HOST_API void expolygons_insert_move(expolygon_collection_handle *dst, uint32_t idx, expolygon_handle *src);
/*
Append all ExPolygons from src to dst by moving their contents, then leave src
empty but still valid. Any expolygon_handle previously borrowed from either
collection may be invalidated. If dst and src are the same handle, nothing
changes.
*/
SLIC3R_HOST_API void expolygons_append_move(expolygon_collection_handle *dst, expolygon_collection_handle *src);
/* Clear dst, then copy from src into dst. */
SLIC3R_HOST_API void expolygons_copy(expolygon_collection_handle *dst, const expolygon_collection_handle *src);
/*
Move all ExPolygons from src into dst, then leave src empty but still valid.
Both collection handles must be mutable. Element handles previously borrowed
from either collection may be invalidated because the destination is replaced
and the source is cleared. If dst and src are the same handle, nothing changes.
*/
SLIC3R_HOST_API void expolygons_move(expolygon_collection_handle *dst, expolygon_collection_handle *src);

/* Conversion: clear dst, then copy each expolygon from src into dst. */
SLIC3R_HOST_API void expolygons_to_polygons(const expolygon_collection_handle *src, polygon_collection_handle *dst);
/*
Convert every contour and hole boundary from src into polylines owned by
storage. The returned collection must be released with storage_free().
*/
SLIC3R_HOST_API polyline_collection_handle *expolygons_to_polylines(storage_handle *storage, const expolygon_collection_handle *src);
/*
Clear dst, then convert Polygons from src into expolygons.
A CCW polygon starts a new expolygon, and following polygons become its holes
until the next CCW polygon.
Returns a status code on failure.
*/
SLIC3R_HOST_API expolygon_status polygons_to_expolygons(const polygon_collection_handle *src, expolygon_collection_handle *dst);

#ifdef __cplusplus
}
#endif

#endif /* slic3r_geometry_h_ */
