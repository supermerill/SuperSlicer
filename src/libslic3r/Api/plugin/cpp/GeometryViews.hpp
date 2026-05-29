///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_GeometryViews_hpp_
#define slic3r_Api_plugin_cpp_GeometryViews_hpp_

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <utility>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_geometry.h"
#ifndef PI
#define PI 3.141592653589793238
#endif

namespace slic3r_api {

class MultiPoint;
class Polygon;
class Polyline;
class PolygonCollection;
class PolylineCollection;
class ExPolygon;
class ExPolygonCollection;

class StoredPolygon;
class StoredPolyline;
class StoredPolygonCollection;
class StoredPolylineCollection;
class StoredExPolygon;
class StoredExPolygonCollection;

/*
Geometry view quick reference
=============================

This file provides small C++ wrappers over the strict C ABI declared in
slic3r_geometry.h. The wrappers do not own native Slic3r objects unless their
name starts with Stored.

Lifetime / mutability model
---------------------------
- MultiPoint, Polygon, Polyline, PolygonCollection, PolylineCollection,
  ExPolygon and ExPolygonCollection are read-only views. They only store a
  handle borrowed from another object, from the data tree, or from a Stored*
  object.
- StoredPolygon, StoredPolyline, StoredPolygonCollection,
  StoredPolylineCollection, StoredExPolygon and StoredExPolygonCollection own a
  mutable handle allocated in a storage_handle. They are move-only and call
  storage_free() in their destructor.
- Stored* objects convert implicitly to their matching read-only view only when
  they are lvalues. This avoids accidentally returning a dangling read-only view
  from a temporary Stored* object.

Classes most plugin code will meet
----------------------------------

class MultiPoint
  Read-only view over a point sequence.
  Available methods:
    bool valid() const;
    explicit operator bool() const;
    const multipoint_handle *handle() const;
    bool same_handle(const MultiPoint &other) const;
    uint32_t size() const;
    bool empty() const;
    c_point at(uint32_t idx) const;
    c_point operator[](uint32_t idx) const;
    c_point front() const;
    c_point back() const;
    distf_t length() const;
    multipoint_const_view view() const;
    c_bounding_box bounding_box() const;
    int32_t find_point_index(c_point point_search, coord_t max_distance) const;
    int32_t closest_point_index(c_point point_search) const;
    std::vector<c_point> points() const;

class Polygon : public MultiPoint read API
  Read-only view over a polygon. It has every MultiPoint read method, plus:
    const polygon_handle *handle() const;
    const multipoint_handle *multipoint_handle() const;
    bool valid_polygon() const;
    double area() const;
    bool is_counter_clockwise() const;
    bool is_clockwise() const;
    bool contains(c_point point) const;
    bool on_boundary(c_point point, coord_t max_dist) const;
    c_point centroid() const;
    std::vector<uint32_t> convex_points_idx(double min_angle, double max_angle) const;
    std::vector<uint32_t> concave_points_idx(double min_angle, double max_angle) const;

class StoredPolygon : public Polygon read API + MultiPoint mutable API
  Owned mutable polygon. It has every Polygon and MultiPoint read method, plus:
    explicit StoredPolygon(storage_handle *storage);
    static StoredPolygon adopt(storage_handle *storage, polygon_handle *handle);
    polygon_handle *mutable_handle() const;
    storage_handle *storage() const;
    bool free_from_storage();
    bool set(uint32_t idx, c_point point);
    bool is_cw();
    void push_back(c_point point);
    void insert(uint32_t idx, c_point point);
    void insert_array(uint32_t idx, const c_point *array, uint32_t array_size);
    void pop_back();
    void clear();
    void erase(uint32_t idx);
    void erase(uint32_t begin_idx, uint32_t erase_size);
    template<class Other> void copy_from(const Other &other);
    void scale(double factor);
    void scale_xy(double factor_x, double factor_y);
    void translate(double x, double y);
    void rotate_origin(double angle);
    void rotate_around(double angle, c_point center);
    void reverse();
    void densify(coord_t min_length);
    void simplify(coord_t min_length, coord_t min_deviation);
    bool make_counter_clockwise();
    bool make_clockwise();

class Polyline : public MultiPoint read API
  Same read methods as MultiPoint. Extra methods compared to MultiPoint:
    const polyline_handle *handle() const;
    const multipoint_handle *multipoint_handle() const;
    bool valid_polyline() const;

class StoredPolyline : public Polyline read API + Stored MultiPoint mutable API
  Same owned/mutable methods as StoredPolygon for point editing. Extra methods
  compared to Polyline and to the Stored MultiPoint mutable API:
    explicit StoredPolyline(storage_handle *storage);
    static StoredPolyline adopt(storage_handle *storage, polyline_handle *handle);
    void clip_end(distf_t distance);
    void clip_start(distf_t distance);
    void extend_end(distf_t distance);
    void extend_start(distf_t distance);

class PolygonCollection
  Read-only collection of Polygon views.
    uint32_t size() const;
    bool empty() const;
    bool valid_collection() const;
    Polygon view_at(uint32_t idx) const;
    Polygon at(uint32_t idx) const;
    Polygon operator[](uint32_t idx) const;
    Polygon front() const;
    Polygon back() const;
    StoredPolygonCollection clone(storage_handle *storage) const;
    iterator begin() const;
    iterator end() const;

class StoredPolygonCollection
  Owned mutable collection of polygons. The collection owns the container, not
  individually owned StoredPolygon elements.
    explicit StoredPolygonCollection(storage_handle *storage);
    static StoredPolygonCollection adopt(storage_handle *storage, polygon_collection_handle *handle);
    void clear();
    void copy_from(const PolygonCollection &other);
    void move_from(StoredPolygonCollection &other);
    void move_from(StoredPolygonCollection &&other);
    void append_copy_from(const PolygonCollection &other);
    void append_move_from(StoredPolygonCollection &other);
    void append_move_from(StoredPolygonCollection &&other);
    void push_back(const Polygon &value);
    void insert(const Polygon &value, uint32_t idx);
    void erase(uint32_t idx);
    StoredPolygon clone(uint32_t idx) const;
    StoredPolygon extract(uint32_t idx);

class PolylineCollection
  Read-only collection of Polyline views.
    uint32_t size() const;
    bool empty() const;
    bool valid_collection() const;
    Polyline view_at(uint32_t idx) const;
    Polyline at(uint32_t idx) const;
    Polyline operator[](uint32_t idx) const;
    Polyline front() const;
    Polyline back() const;
    StoredPolylineCollection clone(storage_handle *storage) const;
    iterator begin() const;
    iterator end() const;

class StoredPolylineCollection
  Owned mutable collection of polylines. The collection owns the container, not
  individually owned StoredPolyline elements.
    explicit StoredPolylineCollection(storage_handle *storage);
    static StoredPolylineCollection adopt(storage_handle *storage, polyline_collection_handle *handle);
    void clear();
    void copy_from(const PolylineCollection &other);
    void move_from(StoredPolylineCollection &other);
    void move_from(StoredPolylineCollection &&other);
    void append_copy_from(const PolylineCollection &other);
    void append_move_from(StoredPolylineCollection &other);
    void append_move_from(StoredPolylineCollection &&other);
    void push_back(const Polyline &value);
    void insert(const Polyline &value, uint32_t idx);
    void erase(uint32_t idx);
    StoredPolyline clone(uint32_t idx) const;
    StoredPolyline extract(uint32_t idx);

class ExPolygon
  Read-only view over one contour polygon and zero or more hole polygons.
    Polygon contour() const;
    uint32_t hole_size() const;
    Polygon hole(uint32_t idx) const;
    holes_range holes() const;
    double area() const;
    bool overlaps(const ExPolygon &other) const;

class StoredExPolygon
  Owned mutable ExPolygon. Contour and holes are exposed as read-only Polygon
  views; hole container size can be changed through the ExPolygon ABI.
    explicit StoredExPolygon(storage_handle *storage);
    static StoredExPolygon adopt(storage_handle *storage, expolygon_handle *handle);
    void holes_resize(uint32_t new_size);
    void holes_emplace_back();
    void holes_pop_back();
    void holes_clear();
    void holes_erase(uint32_t begin_idx, uint32_t erase_size);
    void copy_from(const ExPolygon &other);

class ExPolygonCollection
  Read-only collection of ExPolygon views.
    uint32_t size() const;
    bool empty() const;
    bool valid_collection() const;
    ExPolygon view_at(uint32_t idx) const;
    ExPolygon at(uint32_t idx) const;
    ExPolygon operator[](uint32_t idx) const;
    ExPolygon front() const;
    ExPolygon back() const;
    StoredExPolygonCollection clone(storage_handle *storage) const;
    iterator begin() const;
    iterator end() const;

class StoredExPolygonCollection
  Owned mutable collection of ExPolygons.
    explicit StoredExPolygonCollection(storage_handle *storage);
    static StoredExPolygonCollection adopt(storage_handle *storage, expolygon_collection_handle *handle);
    void clear();
    void copy_from(const ExPolygonCollection &other);
    void move_from(StoredExPolygonCollection &other);
    void move_from(StoredExPolygonCollection &&other);
    void append_copy_from(const ExPolygonCollection &other);
    void append_move_from(StoredExPolygonCollection &other);
    void append_move_from(StoredExPolygonCollection &&other);
    void push_back(const ExPolygon &value);
    void push_back_move(StoredExPolygon &&value);
    void insert(const ExPolygon &value, uint32_t idx);
    void insert_move(StoredExPolygon &&value, uint32_t idx);
    void erase(uint32_t idx);
    StoredExPolygon clone(uint32_t idx) const;
    StoredExPolygon extract(uint32_t idx);
*/

/* ========================= c_point helpers ========================= */
/*
Small C++ convenience operators/functions for c_point used by the ABI views.
They do not extend the C ABI itself; they only make view-side geometry code
read naturally.
*/

inline c_point make_point(coord_t x, coord_t y) { return c_point{x, y}; }

inline c_point operator+(c_point lhs, c_point rhs) { return c_point{lhs.x + rhs.x, lhs.y + rhs.y}; }

inline c_point operator-(c_point lhs, c_point rhs) { return c_point{lhs.x - rhs.x, lhs.y - rhs.y}; }

inline c_point operator*(c_point point, coord_t factor) { return c_point{point.x * factor, point.y * factor}; }

inline c_point operator/(c_point point, coord_t divisor) { return c_point{point.x / divisor, point.y / divisor}; }

inline c_point midpoint(c_point p1, c_point p2) { return c_point{(p1.x + p2.x) / 2, (p1.y + p2.y) / 2}; }

inline distsqrf_t norm_square(c_point point) {
    return distf_t(point.x) * distf_t(point.x) + distf_t(point.y) * distf_t(point.y);
}

inline distf_t norm(c_point point) { return std::sqrt(norm_square(point)); }

inline c_point point_at(c_point p1, c_point p2, distf_t distance) {
    const c_point direction = p2 - p1;
    const distf_t length = norm(direction);
    c_point result = p1;
    if (length == 0)
        return result;
    if (p1.x != p2.x)
        result.x = coord_t(double(p1.x) + double(direction.x) * double(distance) / length);
    if (p1.y != p2.y)
        result.y = coord_t(double(p1.y) + double(direction.y) * double(distance) / length);
    return result;
}
inline c_point extend_start(c_point p1, c_point p2, distf_t distance) { return point_at(p1, p2, -distance); }
inline c_point extend_end(c_point p1, c_point p2, distf_t distance) { return point_at(p2, p1, -distance); }

inline double abs_angle(double rad) { return rad <= 0 ? rad + 2.0 * PI : rad; }

// Angle from v1 to v2, normalized to <-PI, PI>, matching Slic3r::angle_ccw().
inline double angle_ccw(c_point v1, c_point v2) {
    const double det = double(v1.x) * double(v2.y) - double(v1.y) * double(v2.x);
    const double dot = double(v1.x) * double(v2.x) + double(v1.y) * double(v2.y);
    return std::atan2(det, dot);
}

inline bool equals(multipoint_const_view lhs, multipoint_const_view rhs) {
    if (lhs.size != rhs.size)
        return false;
    for (uint32_t idx = 0; idx < lhs.size; ++idx) {
        if (lhs.array[idx].x != rhs.array[idx].x || lhs.array[idx].y != rhs.array[idx].y)
            return false;
    }
    return true;
}

inline bool equals(multipoint_view lhs, multipoint_view rhs) {
    return equals(multipoint_view_as_const(lhs), multipoint_view_as_const(rhs));
}

inline bool equals(multipoint_view lhs, multipoint_const_view rhs) {
    return equals(multipoint_view_as_const(lhs), rhs);
}

inline bool equals(multipoint_const_view lhs, multipoint_view rhs) {
    return equals(lhs, multipoint_view_as_const(rhs));
}

/*
ConstGeometryHandleView is intentionally tiny: it is only a borrowed pointer wrapper.
It never frees anything and never exposes mutable access. Data tree views return
these read-only geometry wrappers because the tree owns the underlying objects.
*/
template<class Handle> class ConstGeometryHandleView
{
public:
    ConstGeometryHandleView() = default;
    explicit ConstGeometryHandleView(const Handle *handle) : m_handle(handle) { assert(handle != nullptr); }

    bool valid() const { return m_handle != nullptr; }
    explicit operator bool() const { return valid(); }

    const Handle *handle() const {
        assert(m_handle != nullptr);
        return m_handle;
    }

    bool same_handle(const ConstGeometryHandleView &other) const { return m_handle == other.m_handle; }

protected:
    const Handle *m_handle = nullptr;
};

/*
StoredGeometryHandleView owns a handle that was allocated in a storage_handle.

Important details:
- move-only: there is exactly one C++ owner of the storage allocation;
- destructor calls storage_free();
- lvalue conversion to the read-only View is allowed for ergonomic calls;
- rvalue conversion is deleted to avoid a dangling view after a temporary dies.
*/
template<class Derived, class View, class Handle> class StoredGeometryHandleView
{
public:
    StoredGeometryHandleView() = delete;
    StoredGeometryHandleView(const StoredGeometryHandleView &) = delete;
    StoredGeometryHandleView &operator=(const StoredGeometryHandleView &) = delete;

    StoredGeometryHandleView(StoredGeometryHandleView &&other) noexcept :
        m_storage(other.m_storage), m_handle(other.m_handle) {
        assert(other.m_storage != nullptr);
        assert(other.m_handle != nullptr);
        other.m_storage = nullptr;
        other.m_handle = nullptr;
    }

    StoredGeometryHandleView &operator=(StoredGeometryHandleView &&other) noexcept {
        if (this == &other)
            return *this;
        assert(other.m_storage != nullptr);
        assert(other.m_handle != nullptr);
        reset();
        m_storage = other.m_storage;
        m_handle = other.m_handle;
        other.m_storage = nullptr;
        other.m_handle = nullptr;
        return *this;
    }

    ~StoredGeometryHandleView() { reset(); }

    bool valid() const { return m_handle != nullptr; }
    explicit operator bool() const { return valid(); }

    const Handle *handle() const {
        assert(m_handle != nullptr);
        return m_handle;
    }
    Handle *mutable_handle() const {
        assert(m_handle != nullptr);
        return m_handle;
    }
    Handle *mutable_handler() const { return mutable_handle(); }
    storage_handle *storage() const {
        assert(m_storage != nullptr);
        return m_storage;
    }

    operator View() const & { return View(m_handle); }
    operator View() && = delete;

    View readonly() const & { return View(m_handle); }
    View readonly() && = delete;

    bool same_handle(const View &other) const { return other.handle() == m_handle; }

    bool free_from_storage() { return reset(); }
    bool free_from_storage(storage_handle *storage) {
        assert(storage == m_storage);
        return storage == m_storage ? reset() : false;
    }

protected:
    StoredGeometryHandleView(storage_handle *storage, Handle *handle) :
        m_storage(storage), m_handle(handle) {
        assert(storage != nullptr);
        assert(handle != nullptr);
    }

private:
    bool reset() {
        if (m_storage == nullptr || m_handle == nullptr)
            return false;
        const bool freed = storage_free(m_storage, m_handle) != 0;
        assert(freed);
        m_storage = nullptr;
        m_handle = nullptr;
        return freed;
    }

protected:
    storage_handle *m_storage = nullptr;
    Handle *m_handle = nullptr;
};

/* ========================= multipoint read / mutable APIs ========================= */

/*
CRTP read API shared by MultiPoint, Polygon, Polyline and their Stored*
counterparts. Polygon and Polyline are native Slic3r types, but both are also
point sequences, so they expose multipoint_handle() explicitly for point-level
ABI calls.
*/
template<class Derived> class MultiPointReadApi
{
public:
    uint32_t size() const { return multipoint_size(self().multipoint_handle()); }
    bool empty() const { return size() == 0; }
    bool is_valid() const { return multipoint_valid(self().multipoint_handle()) != 0; }
    c_point at(uint32_t idx) const { assert(idx < size()); return multipoint_get(self().multipoint_handle(), idx); }
    c_point operator[](uint32_t idx) const { return at(idx); }
    c_point front() const {
        assert(!empty());
        return multipoint_get(self().multipoint_handle(), 0);
    }
    c_point back() const {
        assert(!empty());
        return multipoint_get(self().multipoint_handle(), size() - 1);
    }
    distf_t length() const { return multipoint_length(self().multipoint_handle()); }
    multipoint_const_view view() const { return multipoint_view_const(self().multipoint_handle()); }

    c_bounding_box bounding_box() const {
        multipoint_const_view mp_view = view();
        return points_bounding_box(&mp_view);
    }

    int32_t find_point_index(c_point point_search, coord_t max_distance) const {
        multipoint_const_view mp_view = view();
        return points_find_point_index(&mp_view, point_search, max_distance);
    }

    int32_t closest_point_index(c_point point_search) const {
        multipoint_const_view mp_view = view();
        return points_closest_point_index(&mp_view, point_search);
    }

    std::vector<c_point> points() const {
        std::vector<c_point> out;
        out.reserve(size());
        for (uint32_t idx = 0; idx < size(); ++idx)
            out.push_back(at(idx));
        return out;
    }

protected:
    const Derived &self() const { return static_cast<const Derived &>(*this); }
};

/*
Mutable point-sequence operations. Only StoredPolygon and StoredPolyline inherit
this today, because a plain MultiPoint view is read-only and collections do not
own individual editable MultiPoint elements.
*/
template<class Derived> class MultiPointMutableApi
{
public:
    bool set(uint32_t idx, c_point point) {
        return multipoint_set(self().mutable_multipoint_handle(), idx, point) != 0;
    }

    bool is_cw() {
        multipoint_view mp_view = multipoint_view_mut(self().mutable_multipoint_handle());
        return multipoint_is_cw(&mp_view) != 0;
    }

    void push_back(c_point point) { multipoint_push_back(self().mutable_multipoint_handle(), point); }
    void insert(uint32_t idx, c_point point) { multipoint_insert(self().mutable_multipoint_handle(), idx, point); }
    void insert_array(uint32_t idx, const c_point *array, uint32_t array_size) {
        multipoint_insert_array(self().mutable_multipoint_handle(), idx, array, array_size);
    }
    void pop_back() { multipoint_pop_back(self().mutable_multipoint_handle()); }
    void clear() { multipoint_clear(self().mutable_multipoint_handle()); }
    void erase(uint32_t idx) { multipoint_erase(self().mutable_multipoint_handle(), idx, 1); }
    void erase(uint32_t begin_idx, uint32_t erase_size) {
        multipoint_erase(self().mutable_multipoint_handle(), begin_idx, erase_size);
    }
    template<class Other> void copy_from(const Other &other) { multipoint_copy(self().mutable_multipoint_handle(), other.multipoint_handle()); }
    void scale(double factor) { multipoint_scale(self().mutable_multipoint_handle(), factor); }
    void scale_xy(double factor_x, double factor_y) {
        multipoint_scale_xy(self().mutable_multipoint_handle(), factor_x, factor_y);
    }
    void translate(double x, double y) { multipoint_translate(self().mutable_multipoint_handle(), x, y); }
    void rotate_origin(double angle) { multipoint_rotate_origin(self().mutable_multipoint_handle(), angle); }
    void rotate_around(double angle, c_point center) {
        multipoint_rotate_around(self().mutable_multipoint_handle(), angle, center);
    }
    void reverse() { multipoint_reverse(self().mutable_multipoint_handle()); }
    void densify(coord_t min_length) { multipoint_densify(self().mutable_multipoint_handle(), min_length); }
    void simplify(coord_t min_length, coord_t min_deviation) {
        multipoint_simplify(self().mutable_multipoint_handle(), min_length, min_deviation);
    }
    void ensure_valid(coord_t resolution = SCALED_EPSILON) {
        multipoint_ensure_valid(self().mutable_multipoint_handle(), resolution);
    }

protected:
    Derived &self() { return static_cast<Derived &>(*this); }
};

class MultiPoint : public ConstGeometryHandleView<multipoint_handle>, public MultiPointReadApi<MultiPoint>
{
public:
    MultiPoint() = delete;
    using ConstGeometryHandleView<multipoint_handle>::ConstGeometryHandleView;

    const multipoint_handle *multipoint_handle() const { return handle(); }
};

/* ========================= polygon / polyline views ========================= */

/* Polygon-specific read helpers layered on top of the shared point-sequence API. */
template<class Derived> class PolygonReadApi
{
public:
    bool valid_polygon() const { return polygon_valid(self().handle()) != 0; }
    double area() const { return polygon_area(self().handle()); }
    bool is_counter_clockwise() const { return polygon_is_counter_clockwise(self().handle()) != 0; }
    bool is_clockwise() const { return polygon_is_clockwise(self().handle()) != 0; }
    bool contains(c_point point) const { return polygon_contains(self().handle(), point) != 0; }
    bool on_boundary(c_point point, coord_t max_dist) const {
        return polygon_on_boundary(self().handle(), point, max_dist) != 0;
    }
    c_point centroid() const { return polygon_centroid(self().handle()); }

    std::vector<uint32_t> convex_points_idx(double min_angle, double max_angle) const {
        std::vector<uint32_t> out(self().size());
        const uint32_t count = polygon_convex_points_idx(self().handle(), min_angle, max_angle, out.data(),
                                                         static_cast<uint32_t>(out.size()));
        out.resize(count);
        return out;
    }

    std::vector<uint32_t> concave_points_idx(double min_angle, double max_angle) const {
        std::vector<uint32_t> out(self().size());
        const uint32_t count = polygon_concave_points_idx(self().handle(), min_angle, max_angle, out.data(),
                                                          static_cast<uint32_t>(out.size()));
        out.resize(count);
        return out;
    }

protected:
    const Derived &self() const { return static_cast<const Derived &>(*this); }
};

/* Polygon orientation mutators are only available on StoredPolygon. */
template<class Derived> class PolygonMutableApi
{
public:
    bool make_counter_clockwise() {
        return polygon_make_counter_clockwise(self().mutable_handle()) != 0;
    }

    bool make_clockwise() {
        return polygon_make_clockwise(self().mutable_handle()) != 0;
    }

protected:
    Derived &self() { return static_cast<Derived &>(*this); }
};

class Polygon : public ConstGeometryHandleView<polygon_handle>,
                public MultiPointReadApi<Polygon>,
                public PolygonReadApi<Polygon>
{
public:
    Polygon() = delete;
    using ConstGeometryHandleView<polygon_handle>::ConstGeometryHandleView;

    // Polygon has its own ABI type, but point-sequence helpers still operate on MultiPoint.
    const multipoint_handle *multipoint_handle() const { return polygon_as_multipoint_const(handle()); }
};

class StoredPolygon : public StoredGeometryHandleView<StoredPolygon, Polygon, polygon_handle>,
                      public MultiPointReadApi<StoredPolygon>,
                      public MultiPointMutableApi<StoredPolygon>,
                      public PolygonReadApi<StoredPolygon>,
                      public PolygonMutableApi<StoredPolygon>
{
public:
    StoredPolygon(StoredPolygon &&) noexcept = default;
    StoredPolygon &operator=(StoredPolygon &&) noexcept = default;
    explicit StoredPolygon(storage_handle *storage) :
        StoredPolygon(storage, storage_new_polygon(storage)) {}

    static StoredPolygon adopt(storage_handle *storage, polygon_handle *handle) {
        return StoredPolygon(storage, handle);
    }

    // The explicit cast keeps the public ABI typed while reusing point mutators.
    const multipoint_handle *multipoint_handle() const { return polygon_as_multipoint_const(handle()); }
    ::multipoint_handle *mutable_multipoint_handle() const { return polygon_as_multipoint(mutable_handle()); }
    void move_from(StoredPolygon &other) { polygon_move(mutable_handle(), other.mutable_handle()); }

private:
    friend class StoredGeometryHandleView<StoredPolygon, Polygon, polygon_handle>;
    StoredPolygon(storage_handle *storage, polygon_handle *handle) :
        StoredGeometryHandleView<StoredPolygon, Polygon, polygon_handle>(storage, handle) {}
};

/* Polyline-specific read helpers layered on top of the shared point-sequence API. */
template<class Derived> class PolylineReadApi
{
public:
    bool valid_polyline() const { return polyline_valid(self().handle()) != 0; }

protected:
    const Derived &self() const { return static_cast<const Derived &>(*this); }
};

/* Polyline end trimming/extension mutators are only available on StoredPolyline. */
template<class Derived> class PolylineMutableApi
{
public:
    void clip_end(distf_t distance) { polyline_clip_end(self().mutable_handle(), distance); }
    void clip_start(distf_t distance) { polyline_clip_start(self().mutable_handle(), distance); }
    void extend_end(distf_t distance) { polyline_extend_end(self().mutable_handle(), distance); }
    void extend_start(distf_t distance) { polyline_extend_start(self().mutable_handle(), distance); }

protected:
    Derived &self() { return static_cast<Derived &>(*this); }
};

class Polyline : public ConstGeometryHandleView<polyline_handle>,
                 public MultiPointReadApi<Polyline>,
                 public PolylineReadApi<Polyline>
{
public:
    Polyline() = delete;
    using ConstGeometryHandleView<polyline_handle>::ConstGeometryHandleView;

    // Polyline has its own ABI type, but point-sequence helpers still operate on MultiPoint.
    const multipoint_handle *multipoint_handle() const { return polyline_as_multipoint_const(handle()); }
};

class StoredPolyline : public StoredGeometryHandleView<StoredPolyline, Polyline, polyline_handle>,
                       public MultiPointReadApi<StoredPolyline>,
                       public MultiPointMutableApi<StoredPolyline>,
                       public PolylineReadApi<StoredPolyline>,
                       public PolylineMutableApi<StoredPolyline>
{
public:
    StoredPolyline(StoredPolyline &&) noexcept = default;
    StoredPolyline &operator=(StoredPolyline &&) noexcept = default;
    explicit StoredPolyline(storage_handle *storage) :
        StoredPolyline(storage, storage_new_polyline(storage)) {}

    static StoredPolyline adopt(storage_handle *storage, polyline_handle *handle) {
        return StoredPolyline(storage, handle);
    }

    // The explicit cast keeps the public ABI typed while reusing point mutators.
    const multipoint_handle *multipoint_handle() const { return polyline_as_multipoint_const(handle()); }
    ::multipoint_handle *mutable_multipoint_handle() const { return polyline_as_multipoint(mutable_handle()); }
    void move_from(StoredPolyline &other) { polyline_move(mutable_handle(), other.mutable_handle()); }

private:
    friend class StoredGeometryHandleView<StoredPolyline, Polyline, polyline_handle>;
    StoredPolyline(storage_handle *storage, polyline_handle *handle) :
        StoredGeometryHandleView<StoredPolyline, Polyline, polyline_handle>(storage, handle) {}
};

/* ========================= geometry collections ========================= */

/*
Collection views all expose the same C++ shape:
- the const view borrows a collection handle and returns read-only element views;
- the stored view owns the collection handle, can replace its contents, and can
  copy or extract individual elements into independent Stored* objects.

The element views returned by view_at(), at(), operator[] and iterators are
borrowed from the collection. They become invalid if the collection is resized,
cleared, moved from, extracted from, or destroyed.
*/
template<class Derived, class Traits> class GeometryCollectionReadApi
{
public:
    using value_type = typename Traits::View;

    class iterator
    {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = typename Traits::View;
        using difference_type = std::ptrdiff_t;

        iterator() = default;
        iterator(const Derived *owner, uint32_t index) : m_owner(owner), m_index(index) {}

        value_type operator*() const { return m_owner->at(m_index); }

        iterator &operator++() {
            ++m_index;
            return *this;
        }

        iterator operator++(int) {
            iterator copy = *this;
            ++(*this);
            return copy;
        }

        bool operator==(const iterator &other) const {
            return m_owner == other.m_owner && m_index == other.m_index;
        }

        bool operator!=(const iterator &other) const { return !(*this == other); }

    private:
        const Derived *m_owner = nullptr;
        uint32_t m_index = 0;
    };

    uint32_t size() const { return Traits::size(self().handle()); }
    bool empty() const { return size() == 0; }
    bool valid_collection() const { return Traits::valid(self().handle()); }
    value_type view_at(uint32_t idx) const { assert(idx < size()); return Traits::view_at(self().handle(), idx); }
    value_type at(uint32_t idx) const { return view_at(idx); }
    value_type operator[](uint32_t idx) const { return view_at(idx); }
    value_type front() const { assert(!empty()); return view_at(0); }
    value_type back() const { assert(!empty()); return view_at(size() - 1); }
    iterator begin() const { return iterator(&self(), 0); }
    iterator end() const { return iterator(&self(), size()); }

    typename Traits::StoredCollection clone(storage_handle *storage) const {
        typename Traits::StoredCollection out(storage);
        out.copy_from(self());
        return out;
    }

protected:
    const Derived &self() const { return static_cast<const Derived &>(*this); }
};

template<class Derived, class ConstView, class Traits> class StoredGeometryCollectionApi
{
public:
    using stored_value_type = typename Traits::StoredView;

    void clear() { Traits::clear(self().mutable_handle()); }
    void copy_from(const ConstView &other) { Traits::copy(self().mutable_handle(), other.handle()); }
    void move_from(Derived &other) { Traits::move(self().mutable_handle(), other.mutable_handle()); }
    void move_from(Derived &&other) { Traits::move(self().mutable_handle(), other.mutable_handle()); }
    void append_copy_from(const ConstView &other) { Traits::append_copy(self().mutable_handle(), other.handle()); }
    void append_move_from(Derived &other) { Traits::append_move(self().mutable_handle(), other.mutable_handle()); }
    void append_move_from(Derived &&other) { Traits::append_move(self().mutable_handle(), other.mutable_handle()); }

    void push_back(const typename Traits::View &value) { insert(value, self().size()); }
    void push_back_move(stored_value_type &&value) { insert_move(std::move(value), self().size()); }

    void insert(const typename Traits::View &value, uint32_t idx) {
        Traits::insert_copy(self().mutable_handle(), idx, value.handle());
    }

    // Move the element payload into the collection while keeping both storage
    // handles owned by their C++ wrappers. The moved-from Stored* remains valid
    // but no longer contains the original geometry payload.
    void insert_move(stored_value_type &&value, uint32_t idx) {
        assert(idx <= self().size());
        Traits::insert_move(self().mutable_handle(), idx, value.mutable_handle());
    }

    void erase(uint32_t idx) {
        assert(idx < self().size());
        Traits::erase(self().mutable_handle(), idx, 1);
    }

    stored_value_type clone(uint32_t idx) const {
        stored_value_type out(self().storage());
        out.copy_from(self().view_at(idx));
        return out;
    }

    stored_value_type extract(uint32_t idx) {
        assert(idx < self().size());
        stored_value_type out(self().storage());
        Traits::move_element(out.mutable_handle(), Traits::mutable_at(self().mutable_handle(), idx));
        Traits::erase(self().mutable_handle(), idx, 1);
        return out;
    }

protected:
    Derived &self() { return static_cast<Derived &>(*this); }
    const Derived &self() const { return static_cast<const Derived &>(*this); }
};

struct PolygonCollectionTraits
{
    using Handle = polygon_collection_handle;
    using ElementHandle = polygon_handle;
    using View = Polygon;
    using StoredView = StoredPolygon;
    using StoredCollection = StoredPolygonCollection;

    static Handle *create(storage_handle *storage) { return storage_new_polygons(storage); }
    static uint32_t size(const Handle *handle) { return polygons_size(handle); }
    static bool valid(const Handle *handle) { return polygons_valid(handle) != 0; }
    static View view_at(const Handle *handle, uint32_t idx) { return View(polygons_at_const(handle, idx)); }
    static ElementHandle *mutable_at(Handle *handle, uint32_t idx) { return polygons_at(handle, idx); }
    static void clear(Handle *handle) { polygons_clear(handle); }
    static void resize(Handle *handle, uint32_t new_size) { polygons_resize(handle, new_size); }
    static void copy(Handle *dst, const Handle *src) { polygons_copy(dst, src); }
    static void move(Handle *dst, Handle *src) { polygons_move(dst, src); }
    static void insert_copy(Handle *dst, uint32_t idx, const ElementHandle *src) {
        polygons_insert_copy(dst, idx, src);
    }
    static void insert_move(Handle *dst, uint32_t idx, ElementHandle *src) { polygons_insert_move(dst, idx, src); }
    static void append_copy(Handle *dst, const Handle *src) { polygons_append_copy(dst, src); }
    static void append_move(Handle *dst, Handle *src) { polygons_append_move(dst, src); }
    static void erase(Handle *handle, uint32_t begin_idx, uint32_t erase_size) {
        polygons_erase(handle, begin_idx, erase_size);
    }
    static void move_element(ElementHandle *dst, ElementHandle *src) { polygon_move(dst, src); }
};

struct PolylineCollectionTraits
{
    using Handle = polyline_collection_handle;
    using ElementHandle = polyline_handle;
    using View = Polyline;
    using StoredView = StoredPolyline;
    using StoredCollection = StoredPolylineCollection;

    static Handle *create(storage_handle *storage) { return storage_new_polylines(storage); }
    static uint32_t size(const Handle *handle) { return polylines_size(handle); }
    static bool valid(const Handle *handle) { return polylines_valid(handle) != 0; }
    static View view_at(const Handle *handle, uint32_t idx) { return View(polylines_at_const(handle, idx)); }
    static ElementHandle *mutable_at(Handle *handle, uint32_t idx) { return polylines_at(handle, idx); }
    static void clear(Handle *handle) { polylines_clear(handle); }
    static void resize(Handle *handle, uint32_t new_size) { polylines_resize(handle, new_size); }
    static void copy(Handle *dst, const Handle *src) { polylines_copy(dst, src); }
    static void move(Handle *dst, Handle *src) { polylines_move(dst, src); }
    static void insert_copy(Handle *dst, uint32_t idx, const ElementHandle *src) {
        polylines_insert_copy(dst, idx, src);
    }
    static void insert_move(Handle *dst, uint32_t idx, ElementHandle *src) { polylines_insert_move(dst, idx, src); }
    static void append_copy(Handle *dst, const Handle *src) { polylines_append_copy(dst, src); }
    static void append_move(Handle *dst, Handle *src) { polylines_append_move(dst, src); }
    static void erase(Handle *handle, uint32_t begin_idx, uint32_t erase_size) {
        polylines_erase(handle, begin_idx, erase_size);
    }
    static void move_element(ElementHandle *dst, ElementHandle *src) { polyline_move(dst, src); }
};

class PolygonCollection : public ConstGeometryHandleView<polygon_collection_handle>,
                          public GeometryCollectionReadApi<PolygonCollection, PolygonCollectionTraits>
{
public:
    PolygonCollection() = delete;
    using ConstGeometryHandleView<polygon_collection_handle>::ConstGeometryHandleView;
};

class StoredPolygonCollection :
    public StoredGeometryHandleView<StoredPolygonCollection, PolygonCollection, polygon_collection_handle>,
    public GeometryCollectionReadApi<StoredPolygonCollection, PolygonCollectionTraits>,
    public StoredGeometryCollectionApi<StoredPolygonCollection, PolygonCollection, PolygonCollectionTraits>
{
public:
    StoredPolygonCollection(StoredPolygonCollection &&) noexcept = default;
    StoredPolygonCollection &operator=(StoredPolygonCollection &&) noexcept = default;
    explicit StoredPolygonCollection(storage_handle *storage) :
        StoredPolygonCollection(storage, PolygonCollectionTraits::create(storage)) {}

    static StoredPolygonCollection adopt(storage_handle *storage, polygon_collection_handle *handle) {
        return StoredPolygonCollection(storage, handle);
    }

private:
    friend class StoredGeometryHandleView<StoredPolygonCollection, PolygonCollection, polygon_collection_handle>;
    StoredPolygonCollection(storage_handle *storage, polygon_collection_handle *handle) :
        StoredGeometryHandleView<StoredPolygonCollection, PolygonCollection, polygon_collection_handle>(storage, handle) {}
};

class PolylineCollection : public ConstGeometryHandleView<polyline_collection_handle>,
                           public GeometryCollectionReadApi<PolylineCollection, PolylineCollectionTraits>
{
public:
    PolylineCollection() = delete;
    using ConstGeometryHandleView<polyline_collection_handle>::ConstGeometryHandleView;
};

class StoredPolylineCollection :
    public StoredGeometryHandleView<StoredPolylineCollection, PolylineCollection, polyline_collection_handle>,
    public GeometryCollectionReadApi<StoredPolylineCollection, PolylineCollectionTraits>,
    public StoredGeometryCollectionApi<StoredPolylineCollection, PolylineCollection, PolylineCollectionTraits>
{
public:
    StoredPolylineCollection(StoredPolylineCollection &&) noexcept = default;
    StoredPolylineCollection &operator=(StoredPolylineCollection &&) noexcept = default;
    explicit StoredPolylineCollection(storage_handle *storage) :
        StoredPolylineCollection(storage, PolylineCollectionTraits::create(storage)) {}

    static StoredPolylineCollection adopt(storage_handle *storage, polyline_collection_handle *handle) {
        return StoredPolylineCollection(storage, handle);
    }

private:
    friend class StoredGeometryHandleView<StoredPolylineCollection, PolylineCollection, polyline_collection_handle>;
    StoredPolylineCollection(storage_handle *storage, polyline_collection_handle *handle) :
        StoredGeometryHandleView<StoredPolylineCollection, PolylineCollection, polyline_collection_handle>(storage, handle) {}
};

/* ========================= expolygon views ========================= */

/*
ExPolygonReadApi exposes contour() and holes() as Polygon views. Even for a
StoredExPolygon these Polygon views are read-only: changing the number of holes
is supported through ExPolygonMutableApi, but editing contour/hole points needs
an explicit mutable ABI path so callers do not accidentally mutate borrowed
geometry.
*/
template<class Derived> class ExPolygonReadApi
{
public:
    class holes_iterator
    {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = Polygon;
        using difference_type = std::ptrdiff_t;

        holes_iterator() = default;
        holes_iterator(const Derived *owner, uint32_t index) : m_owner(owner), m_index(index) {}

        value_type operator*() const { return m_owner->hole(m_index); }

        holes_iterator &operator++() {
            ++m_index;
            return *this;
        }

        holes_iterator operator++(int) {
            holes_iterator copy = *this;
            ++(*this);
            return copy;
        }

        bool operator==(const holes_iterator &other) const {
            return m_owner == other.m_owner && m_index == other.m_index;
        }

        bool operator!=(const holes_iterator &other) const { return !(*this == other); }

    private:
        const Derived *m_owner = nullptr;
        uint32_t m_index = 0;
    };

    class holes_range
    {
    public:
        explicit holes_range(const Derived *owner) : m_owner(owner) {}

        holes_iterator begin() const { return holes_iterator(m_owner, 0); }
        holes_iterator end() const {
            return holes_iterator(m_owner, m_owner == nullptr ? 0 : m_owner->hole_size());
        }

    private:
        const Derived *m_owner = nullptr;
    };

    Polygon contour() const { return Polygon(expolygon_contour_const(self().handle())); }
    uint32_t hole_size() const { return expolygon_hole_size(self().handle()); }
    Polygon hole(uint32_t idx) const { return Polygon(expolygon_hole_at_const(self().handle(), idx)); }
    holes_range holes() const { return holes_range(&self()); }

    double area() const { return expolygon_area(self().handle()); }
    bool contains(c_point point) const { return expolygon_contains(self().handle(), point) != 0; }
    bool is_valid() const { return expolygon_valid(self().handle()) == EXPOLYGON_STATUS_OK; }
    template<class Other> bool overlaps(const Other &other) const {
        return expolygon_overlaps(self().handle(), other.handle()) != 0;
    }

protected:
    const Derived &self() const { return static_cast<const Derived &>(*this); }
};

template<class Derived, class ConstDerived> class ExPolygonMutableApi
{
public:
    // These operations modify the holes container, which may invalidate Polygon views previously returned by hole().
    void holes_resize(uint32_t new_size) { expolygon_holes_resize(self().mutable_handle(), new_size); }
    void holes_emplace_back() { expolygon_holes_emplace_back(self().mutable_handle()); }
    void holes_pop_back() { expolygon_holes_pop_back(self().mutable_handle()); }
    void holes_clear() { expolygon_holes_clear(self().mutable_handle()); }
    void holes_erase(uint32_t begin_idx, uint32_t erase_size) {
        expolygon_holes_erase(self().mutable_handle(), begin_idx, erase_size);
    }
    void copy_from(const ConstDerived &other) { expolygon_copy(self().mutable_handle(), other.handle()); }
    void move_from(Derived &other) { expolygon_move(self().mutable_handle(), other.mutable_handle()); }
    void ensure_valid(coord_t resolution = SCALED_EPSILON) {
        expolygon_ensure_valid(self().mutable_handle(), resolution);
    }

protected:
    Derived &self() { return static_cast<Derived &>(*this); }
};

class ExPolygon : public ConstGeometryHandleView<expolygon_handle>,
                  public ExPolygonReadApi<ExPolygon>
{
public:
    ExPolygon() = delete;
    using ConstGeometryHandleView<expolygon_handle>::ConstGeometryHandleView;

};

class StoredExPolygon : public StoredGeometryHandleView<StoredExPolygon, ExPolygon, expolygon_handle>,
                        public ExPolygonReadApi<StoredExPolygon>,
                        public ExPolygonMutableApi<StoredExPolygon, ExPolygon>
{
public:
    StoredExPolygon(StoredExPolygon &&) noexcept = default;
    StoredExPolygon &operator=(StoredExPolygon &&) noexcept = default;
    explicit StoredExPolygon(storage_handle *storage) :
        StoredExPolygon(storage, storage_new_expolygon(storage)) {}

    static StoredExPolygon adopt(storage_handle *storage, expolygon_handle *handle) {
        return StoredExPolygon(storage, handle);
    }

private:
    friend class StoredGeometryHandleView<StoredExPolygon, ExPolygon, expolygon_handle>;
    StoredExPolygon(storage_handle *storage, expolygon_handle *handle) :
        StoredGeometryHandleView<StoredExPolygon, ExPolygon, expolygon_handle>(storage, handle) {}
};

// Strict point-order equality helper. Changing hole order or point order makes this return false.
inline bool equals(const ExPolygon &lhs, const ExPolygon &rhs) {
    if (!equals(lhs.contour().view(), rhs.contour().view()))
        return false;
    if (lhs.hole_size() != rhs.hole_size())
        return false;
    for (uint32_t idx = 0; idx < lhs.hole_size(); ++idx) {
        if (!equals(lhs.hole(idx).view(), rhs.hole(idx).view()))
            return false;
    }
    return true;
}

/* ========================= expolygon collections ========================= */

struct ExPolygonCollectionTraits
{
    using Handle = expolygon_collection_handle;
    using ElementHandle = expolygon_handle;
    using View = ExPolygon;
    using StoredView = StoredExPolygon;
    using StoredCollection = StoredExPolygonCollection;

    static Handle *create(storage_handle *storage) { return storage_new_expolygons(storage); }
    static uint32_t size(const Handle *handle) { return expolygons_size(handle); }
    static bool valid(const Handle *handle) { return expolygons_valid(handle) == EXPOLYGON_STATUS_OK; }
    static View view_at(const Handle *handle, uint32_t idx) { return View(expolygons_at_const(handle, idx)); }
    static ElementHandle *mutable_at(Handle *handle, uint32_t idx) { return expolygons_at(handle, idx); }
    static void clear(Handle *handle) { expolygons_clear(handle); }
    static void resize(Handle *handle, uint32_t new_size) { expolygons_resize(handle, new_size); }
    static void copy(Handle *dst, const Handle *src) { expolygons_copy(dst, src); }
    static void move(Handle *dst, Handle *src) { expolygons_move(dst, src); }
    static void insert_copy(Handle *dst, uint32_t idx, const ElementHandle *src) {
        expolygons_insert_copy(dst, idx, src);
    }
    static void insert_move(Handle *dst, uint32_t idx, ElementHandle *src) {
        expolygons_insert_move(dst, idx, src);
    }
    static void append_copy(Handle *dst, const Handle *src) { expolygons_append_copy(dst, src); }
    static void append_move(Handle *dst, Handle *src) { expolygons_append_move(dst, src); }
    static void erase(Handle *handle, uint32_t begin_idx, uint32_t erase_size) {
        expolygons_erase(handle, begin_idx, erase_size);
    }
    static void move_element(ElementHandle *dst, ElementHandle *src) { expolygon_move(dst, src); }
};

class ExPolygonCollection : public ConstGeometryHandleView<expolygon_collection_handle>,
                            public GeometryCollectionReadApi<ExPolygonCollection, ExPolygonCollectionTraits>
{
public:
    ExPolygonCollection() = delete;
    using ConstGeometryHandleView<expolygon_collection_handle>::ConstGeometryHandleView;
};

class StoredExPolygonCollection :
    public StoredGeometryHandleView<StoredExPolygonCollection, ExPolygonCollection, expolygon_collection_handle>,
    public GeometryCollectionReadApi<StoredExPolygonCollection, ExPolygonCollectionTraits>,
    public StoredGeometryCollectionApi<StoredExPolygonCollection, ExPolygonCollection, ExPolygonCollectionTraits>
{
public:
    StoredExPolygonCollection(StoredExPolygonCollection &&) noexcept = default;
    StoredExPolygonCollection &operator=(StoredExPolygonCollection &&) noexcept = default;
    explicit StoredExPolygonCollection(storage_handle *storage) :
        StoredExPolygonCollection(storage, ExPolygonCollectionTraits::create(storage)) {}

    static StoredExPolygonCollection adopt(storage_handle *storage, expolygon_collection_handle *handle) {
        return StoredExPolygonCollection(storage, handle);
    }

    void ensure_valid(coord_t resolution = SCALED_EPSILON) {
        expolygons_ensure_valid(mutable_handle(), resolution);
    }

private:
    friend class StoredGeometryHandleView<StoredExPolygonCollection, ExPolygonCollection, expolygon_collection_handle>;
    StoredExPolygonCollection(storage_handle *storage, expolygon_collection_handle *handle) :
        StoredGeometryHandleView<StoredExPolygonCollection, ExPolygonCollection, expolygon_collection_handle>(storage, handle) {}
};

inline StoredPolylineCollection expolygons_to_polylines(storage_handle *storage,
                                                        const ExPolygonCollection &src)
{
    return StoredPolylineCollection::adopt(storage, ::expolygons_to_polylines(storage, src.handle()));
}

inline StoredPolylineCollection expolygon_medial_axis(storage_handle *storage,
                                                      const ExPolygon &src,
                                                      double min_width,
                                                      double max_width)
{
    return StoredPolylineCollection::adopt(storage, ::expolygon_medial_axis(storage, src.handle(), min_width, max_width));
}

} // namespace slic3r_api


#endif // slic3r_Api_plugin_cpp_GeometryViews_hpp_
