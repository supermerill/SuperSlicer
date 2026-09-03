///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_LineDistancer_hpp_
#define slic3r_Api_plugin_cpp_LineDistancer_hpp_

/*
LineDistancer
=============

LineDistancer copies the boundary segments of an `ExPolygonCollection` and
answers nearest-distance queries against those segments. Both outer contours
and holes are included. A query is measured against the finite segments, not
against their infinite supporting lines, so the closest point may be a segment
endpoint.

The stored coordinates use the same scaled units as `c_point`. The returned
distance and the radius argument use those units as well; convert to or from
millimeters at the API boundary when necessary.

    LineDistancer boundaries(areas);
    double distance = boundaries.distance_from_lines(point);
    std::vector<size_t> nearby = boundaries.all_lines_in_radius(point, radius);

`distance_from_lines()` returns positive distance outside the stored areas. If
`signed_distance` is true, it returns a negative distance for a point inside an
outer contour and outside its holes. An empty distancer returns positive
infinity. `all_lines_in_radius()` returns the indices of matching stored
segments in insertion order and returns no indices for a negative radius.

The implementation deliberately uses a simple linear scan. It is appropriate
for the relatively small boundary sets used by plugin algorithms, but it has
O(number of stored segments) query cost and does not provide a spatial index.
The source polygons are copied during construction, so later changes to the
input views do not change an existing LineDistancer.
*/

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

#include "libslic3r/Api/plugin/cpp/GeometryViews.hpp"

namespace slic3r_api {

class LineDistancer
{
public:
    LineDistancer() = default;

    explicit LineDistancer(const ExPolygonCollection &areas) {
        for (const ExPolygon area : areas)
            append(area);
    }

    bool empty() const { return m_segments.empty(); }

    void append_segment(c_point a, c_point b) {
        m_segments.push_back({a, b});
    }

    double distance_from_lines(c_point point, bool signed_distance = false) const {
        if (m_segments.empty())
            return std::numeric_limits<double>::infinity();

        double best_distance_squared = std::numeric_limits<double>::max();
        for (const Segment &segment : m_segments) {
            const double distance_squared = segment_distance_squared(point, segment);
            if (distance_squared < best_distance_squared)
                best_distance_squared = distance_squared;
        }

        double distance = std::sqrt(best_distance_squared);
        if (signed_distance && contains(point))
            distance = -distance;
        return distance;
    }

    std::vector<size_t> all_lines_in_radius(c_point point, double radius) const {
        std::vector<size_t> out;
        if (radius < 0.)
            return out;

        const double radius_squared = radius * radius;
        for (size_t idx = 0; idx < m_segments.size(); ++idx) {
            if (segment_distance_squared(point, m_segments[idx]) <= radius_squared)
                out.push_back(idx);
        }
        return out;
    }

private:
    struct Segment
    {
        c_point a;
        c_point b;
    };

    struct Area
    {
        std::vector<c_point> contour;
        std::vector<std::vector<c_point>> holes;
    };

    void append(const ExPolygon &area) {
        Area stored_area;
        stored_area.contour = area.contour().points();
        append_polygon_segments(stored_area.contour);
        stored_area.holes.reserve(area.hole_size());
        for (const Polygon hole : area.holes()) {
            stored_area.holes.push_back(hole.points());
            append_polygon_segments(stored_area.holes.back());
        }
        m_areas.push_back(std::move(stored_area));
    }

    void append_polygon_segments(const std::vector<c_point> &points) {
        if (points.size() < 2)
            return;
        for (size_t idx = 1; idx < points.size(); ++idx)
            m_segments.push_back({points[idx - 1], points[idx]});
        if (points.front().x != points.back().x || points.front().y != points.back().y)
            m_segments.push_back({points.back(), points.front()});
    }

    static double segment_distance_squared(c_point point, const Segment &segment) {
        const double ax = double(segment.a.x);
        const double ay = double(segment.a.y);
        const double bx = double(segment.b.x);
        const double by = double(segment.b.y);
        const double px = double(point.x);
        const double py = double(point.y);
        const double dx = bx - ax;
        const double dy = by - ay;
        const double length_squared = dx * dx + dy * dy;

        double t = 0.;
        if (length_squared > 0.)
            t = ((px - ax) * dx + (py - ay) * dy) / length_squared;
        t = std::max(0., std::min(1., t));

        const double nearest_x = ax + t * dx;
        const double nearest_y = ay + t * dy;
        const double diff_x = px - nearest_x;
        const double diff_y = py - nearest_y;
        return diff_x * diff_x + diff_y * diff_y;
    }

    static bool polygon_contains(const std::vector<c_point> &polygon, c_point point) {
        if (polygon.size() < 3)
            return false;

        bool inside = false;
        size_t previous_idx = polygon.size() - 1;
        for (size_t idx = 0; idx < polygon.size(); ++idx) {
            const c_point current = polygon[idx];
            const c_point previous = polygon[previous_idx];
            const bool crosses = (current.y > point.y) != (previous.y > point.y);
            if (crosses) {
                const double x_at_y =
                    double(previous.x - current.x) * double(point.y - current.y) /
                    double(previous.y - current.y) + double(current.x);
                if (double(point.x) < x_at_y)
                    inside = !inside;
            }
            previous_idx = idx;
        }
        return inside;
    }

    bool contains(c_point point) const {
        for (const Area &area : m_areas) {
            if (!polygon_contains(area.contour, point))
                continue;
            bool inside_hole = false;
            for (const std::vector<c_point> &hole : area.holes) {
                if (polygon_contains(hole, point)) {
                    inside_hole = true;
                    break;
                }
            }
            if (!inside_hole)
                return true;
        }
        return false;
    }

    std::vector<Segment> m_segments;
    std::vector<Area> m_areas;
};

} // namespace slic3r_api

#endif // slic3r_Api_plugin_cpp_LineDistancer_hpp_
