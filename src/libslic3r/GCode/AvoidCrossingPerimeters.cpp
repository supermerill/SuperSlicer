///|/ Copyright (c) SuperSlicer 2026 Durand R?mi @supermerill
///|/ Copyright (c) Prusa Research 2020 - 2022 Vojtěch Bubník @bubnikv, Lukáš Hejl @hejllukas
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "AvoidCrossingPerimeters.hpp"

#include <numeric>
#include <unordered_set>

#include <boost/log/trivial.hpp>
#include <boost/range/adaptor/reversed.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/EdgeGrid.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintObject.hpp"
#include "libslic3r/PrintRegion.hpp"
#include "libslic3r/SVG.hpp"

//#define AVOID_CROSSING_PERIMETERS_DEBUG_OUTPUT

namespace Slic3r {

struct TravelPoint
{
    Point point;
    // Index of the polygon containing this point. A negative value indicates that the point is not on any border.
    int   border_idx;
    // simplify_travel() doesn't remove this point.
    bool  do_not_remove = false;
};

struct Intersection
{
    // Index of the polygon containing this point of intersection.
    size_t border_idx;
    // Index of the line on the polygon containing this point of intersection.
    size_t line_idx;
    // Point of intersection.
    Point  point;
    // Distance from the first point in the corresponding boundary
    float  distance;
    // simplify_travel() doesn't remove this point.
    bool   do_not_remove = false;
};

struct ClosestLine
{
    // Index of the polygon containing this line.
    size_t border_idx;
    // Index of this line on the polygon containing it.
    size_t line_idx;
    // Closest point on the line.
    Point  point;
};

// Finding all intersections of a set of contours with a line segment.
struct AllIntersectionsVisitor
{
    AllIntersectionsVisitor(const EdgeGrid::Grid &grid, std::vector<Intersection> &intersections) : grid(grid), intersections(intersections)
    {
        intersection_set.reserve(intersections.capacity());
    }

    AllIntersectionsVisitor(const EdgeGrid::Grid &grid, std::vector<Intersection> &intersections, const Line &travel_line)
        : grid(grid), intersections(intersections), travel_line(travel_line)
    {
        intersection_set.reserve(intersections.capacity());
    }

    void reset() {
        intersection_set.clear();
    }

    bool operator()(int64_t iy, int64_t ix)
    {
        // Called with a row and column of the grid cell, which is intersected by a line.
        auto cell_data_range = grid.cell_data_range(iy, ix);
        for (auto it_contour_and_segment = cell_data_range.first; it_contour_and_segment != cell_data_range.second; ++it_contour_and_segment) {
            Point intersection_point;
            if (travel_line.intersection(grid.line(*it_contour_and_segment), &intersection_point) &&
                intersection_set.find(*it_contour_and_segment) == intersection_set.end()) {
                intersections.push_back({ it_contour_and_segment->first, it_contour_and_segment->second, intersection_point });
                intersection_set.insert(*it_contour_and_segment);
            }
        }
        // Continue traversing the grid along the edge.
        return true;
    }

    const EdgeGrid::Grid                                                                 &grid;
    std::vector<Intersection>                                                            &intersections;
    Line                                                                                  travel_line;
    std::unordered_set<std::pair<size_t, size_t>, boost::hash<std::pair<size_t, size_t>>> intersection_set;
};

// Visitor to check for any collision of a line segment with any contour stored inside the edge_grid.
struct FirstIntersectionVisitor
{
    explicit FirstIntersectionVisitor(const EdgeGrid::Grid &grid) : grid(grid) {}

    bool operator()(coord_t iy, coord_t ix)
    {
        assert(pt_current != nullptr);
        assert(pt_next != nullptr);
        // Called with a row and column of the grid cell, which is intersected by a line.
        auto cell_data_range = grid.cell_data_range(iy, ix);
        this->intersect      = false;
        for (auto it_contour_and_segment = cell_data_range.first; it_contour_and_segment != cell_data_range.second; ++it_contour_and_segment) {
            // End points of the line segment and their vector.
            auto segment = grid.segment(*it_contour_and_segment);
            if (segment.first == *pt_current | segment.first == *pt_next | Geometry::segments_intersect(segment.first, segment.second, *pt_current, *pt_next)) {
                this->intersect = true;
                intersection_contour_idx = it_contour_and_segment->first;
                intersection_line_idx = it_contour_and_segment->second;
#ifdef AVOID_CROSSING_PERIMETERS_DEBUG_OUTPUT
                Line(*pt_current, *pt_next).intersection(grid.line(*it_contour_and_segment), &intersection_pt);
#endif
                return false;
            }
        }
        // Continue traversing the grid along the edge.
        return true;
    }

    const EdgeGrid::Grid &grid;
    const Slic3r::Point  *pt_current = nullptr;
    const Slic3r::Point  *pt_next    = nullptr;
    bool                  intersect  = false;
#ifdef AVOID_CROSSING_PERIMETERS_DEBUG_OUTPUT
    Point intersection_pt;
#endif
    size_t intersection_contour_idx = size_t(-1);
    size_t intersection_line_idx = size_t(-1);
};

/* Fast optional grid visitor used by the perimeter-crossing hot path. */
struct GridIntersectTest
{
    explicit GridIntersectTest(const EdgeGrid::Grid &grid, const Line &&line) : grid(grid), test_line(line) {}

    bool operator()(coord_t iy, coord_t ix)
    {
        // Stop as soon as one contour segment intersects the tested travel.
        const auto cell_data_range = grid.cell_data_range(iy, ix);
        this->intersect = false;
        for (auto it = cell_data_range.first; it != cell_data_range.second; ++it) {
            const auto segment = grid.segment(*it);
            if (Geometry::segments_intersect(segment.first, segment.second, test_line.a, test_line.b)) {
                this->intersect = true;
                return false;
            }
        }
        return true;
    }

    const EdgeGrid::Grid &grid;
    Line test_line;
    bool intersect = false;
};

struct FirstEpsilonIntersectionVisitor
{
    const EdgeGrid::Grid &grid;
    Slic3r::Point  &pt_current;
    Slic3r::Point  &pt_next;
    bool                  intersect  = false;
    size_t intersection_contour_idx = size_t(-1);
    size_t intersection_line_idx = size_t(-1);

    explicit FirstEpsilonIntersectionVisitor(const EdgeGrid::Grid &grid, Slic3r::Point &pt_a, Slic3r::Point &pt_b) : grid(grid), pt_current(pt_a), pt_next(pt_b) {}

    bool operator()(coord_t iy, coord_t ix)
    {
        static int aiuhaiod = 0;
        // Called with a row and column of the grid cell, which is intersected by a line.
        auto cell_data_range = grid.cell_data_range(iy, ix);
        this->intersect      = false;
        Lines lines;
        Point intersection_pt;
        for (auto it_contour_and_segment = cell_data_range.first; it_contour_and_segment != cell_data_range.second; ++it_contour_and_segment) {
            // End points of the line segment and their vector.
            std::pair<const Slic3r::Point&, const Slic3r::Point&> segment = grid.segment(*it_contour_and_segment);
            if (Line::distance_to_squared_abp(segment.first, segment.second, pt_current, &intersection_pt) <
                SCALED_EPSILON * SCALED_EPSILON) {
                this->intersect = true;
                intersection_contour_idx = it_contour_and_segment->first;
                intersection_line_idx = it_contour_and_segment->second;
                Line l(intersection_pt, pt_next);
                for (int i = 2;
                     i < SCALED_EPSILON && !Geometry::segments_intersect(segment.first, segment.second, l.a, pt_next);
                     i += 2) {
                    l.a = intersection_pt;
                    l.extend(i);
                }
                assert(pt_current != l.a);
                pt_current = l.a;
                assert(Geometry::segments_intersect(segment.first, segment.second, pt_current, pt_next));
                return false;
            }
        }
        // Continue traversing the grid along the edge.
        return true;
    }
};

// Visitor to create a list of closet lines to a defined point.
struct MinDistanceVisitor
{
    explicit MinDistanceVisitor(const EdgeGrid::Grid &grid, const Point &center, coordf_t max_distance_squared)
        : grid(grid), center(center), max_distance_squared(max_distance_squared)
    {}

    void init()
    {
        this->closest_lines.clear();
        this->closest_lines_set.clear();
    }

    bool operator()(coord_t iy, coord_t ix)
    {
        // Called with a row and column of the grid cell, which is inside a bounding box.
        auto cell_data_range = grid.cell_data_range(iy, ix);
        for (auto it_contour_and_segment = cell_data_range.first; it_contour_and_segment != cell_data_range.second; ++it_contour_and_segment) {
            // End points of the line segment and their vector.
            auto  segment = grid.segment(*it_contour_and_segment);
            Point closest_point;
            if (closest_lines_set.find(*it_contour_and_segment) == closest_lines_set.end() &&
                line_alg::distance_to_squared(Line(segment.first, segment.second), center, &closest_point) <= this->max_distance_squared) {
                closest_lines.push_back({it_contour_and_segment->first, it_contour_and_segment->second, closest_point});
                closest_lines_set.insert(*it_contour_and_segment);
            }
        }
        // Continue traversing the grid along the edge.
        return true;
    }

    const EdgeGrid::Grid &                                                                grid;
    const Slic3r::Point                                                                   center;
    std::vector<ClosestLine>                                                              closest_lines;
    std::unordered_set<std::pair<size_t, size_t>, boost::hash<std::pair<size_t, size_t>>> closest_lines_set;
    coordf_t                                                                              max_distance_squared = std::numeric_limits<double>::max();
};

// Finding if any intersection with Polyline
struct AnyIntersectionsVisitor
{

    AnyIntersectionsVisitor(const EdgeGrid::Grid &grid, const Polyline &travel_polyline)
        : grid(grid), travel_polyline(travel_polyline), has_intersection(false)
    { }

    void reset() {
        has_intersection = false;
    }

    bool operator()(coord_t iy, coord_t ix)
    {
        if (has_intersection)
            return false;
        // Called with a row and column of the grid cell, which is intersected by a line.
        auto cell_data_range = grid.cell_data_range(iy, ix);
        for (auto it_contour_and_segment = cell_data_range.first; it_contour_and_segment != cell_data_range.second; ++it_contour_and_segment) {
            Point intersection_point;
            for(const Line &travel_line : travel_polyline.lines())
                if (travel_line.intersection(grid.line(*it_contour_and_segment), &intersection_point)) {
                    has_intersection = true;
                    return false;
                }
        }
        // Continue traversing the grid along the edge.
        return true;
    }

    const EdgeGrid::Grid &grid;
    bool                  has_intersection;
    Polyline              travel_polyline;
};

// Returns sorted list of closest lines to a passed point within a passed radius
static std::vector<ClosestLine> get_closest_lines_in_radius(const EdgeGrid::Grid &grid, const Point &center, coord_t search_radius)
{
    Point              radius_vector(search_radius, search_radius);
    MinDistanceVisitor visitor(grid, center, coord_sqr(search_radius));
    grid.visit_cells_intersecting_box(BoundingBox(center - radius_vector, center + radius_vector), visitor);
    std::sort(visitor.closest_lines.begin(), visitor.closest_lines.end(), [&center](const auto &l, const auto &r) {
        return (center - l.point).template cast<double>().squaredNorm() < (center - r.point).template cast<double>().squaredNorm();
    });

    return visitor.closest_lines;
}

// When the offset is too big, then original travel doesn't have to cross created boundaries.
// For these cases, this function adds another intersection with lines around the start and the end point of the original travel.
static std::vector<Intersection> extend_for_closest_lines(const std::vector<Intersection>         &intersections,
                                                          const AvoidCrossingPerimeters::Boundary &boundary,
                                                          const Point                             &start,
                                                          const Point                             &end,
                                                          const coord_t                            search_radius)
{
    const std::vector<ClosestLine> start_lines = get_closest_lines_in_radius(boundary.grid, start, search_radius);
    const std::vector<ClosestLine> end_lines   = get_closest_lines_in_radius(boundary.grid, end, search_radius);

    // Compute distance to the closest point in the ClosestLine from begin of contour.
    auto compute_distance = [&boundary](const ClosestLine &closest_line) -> float {
        float dist_from_line_begin = (closest_line.point - boundary.boundaries[closest_line.border_idx][closest_line.line_idx]).cast<float>().norm();
        return boundary.boundaries_params[closest_line.border_idx][closest_line.line_idx] + dist_from_line_begin;
    };

    // It tries to find closest lines for both start point and end point of the travel which has the same border_idx
    auto endpoints_close_to_same_boundary = [&start_lines, &end_lines]() -> std::pair<size_t, size_t> {
        std::unordered_set<size_t> boundaries_from_start;
        for (const ClosestLine &cl_start : start_lines)
            boundaries_from_start.insert(cl_start.border_idx);
        for (const ClosestLine &cl_end : end_lines)
            if (boundaries_from_start.find(cl_end.border_idx) != boundaries_from_start.end())
                for (const ClosestLine &cl_start : start_lines)
                    if (cl_start.border_idx == cl_end.border_idx) {
                        size_t cl_start_idx = &cl_start - &start_lines.front();
                        size_t cl_end_idx   = &cl_end - &end_lines.front();
                        return std::make_pair(cl_start_idx, cl_end_idx);
                    }
        return std::make_pair(std::numeric_limits<size_t>::max(), std::numeric_limits<size_t>::max());
    };

    // If the existing two lines within the search radius start and end point belong to the same boundary,
    // discard all intersection points because the whole detour could be on one boundary.
    if (!start_lines.empty() && !end_lines.empty()) {
        std::pair<size_t, size_t> cl_indices = endpoints_close_to_same_boundary();
        if (cl_indices.first != std::numeric_limits<size_t>::max()) {
            assert(cl_indices.second != std::numeric_limits<size_t>::max());
            const ClosestLine &cl_start = start_lines[cl_indices.first];
            const ClosestLine &cl_end   = end_lines[cl_indices.second];
            std::vector<Intersection> new_intersections;
            new_intersections.push_back({cl_start.border_idx, cl_start.line_idx, cl_start.point, compute_distance(cl_start), true});
            new_intersections.push_back({cl_end.border_idx, cl_end.line_idx, cl_end.point, compute_distance(cl_end), true});
            return new_intersections;
        }
    }

    // Returns ClosestLine which is closer to the point "close_to" then point inside passed Intersection.
    auto get_closer = [&search_radius](const std::vector<ClosestLine> &closest_lines, const Intersection &intersection,
                                       const Point &close_to) -> size_t {
        for (const ClosestLine &cl : closest_lines) {
            double old_dist = (close_to - intersection.point).cast<float>().squaredNorm();
            if (cl.border_idx == intersection.border_idx && old_dist <= coord_sqr(search_radius) &&
                (close_to - cl.point).cast<float>().squaredNorm() < old_dist)
                return &cl - &closest_lines.front();
        }
        return std::numeric_limits<size_t>::max();
    };

    // Try to find ClosestLine with same boundary_idx as any existing Intersection
    auto find_closest_line_with_same_boundary_idx = [](const std::vector<ClosestLine> & closest_lines,
                                                       const std::vector<Intersection> &intersections, const bool reverse) -> size_t {
        std::unordered_set<size_t> boundaries_indices;
        for (const ClosestLine &closest_line : closest_lines)
            boundaries_indices.insert(closest_line.border_idx);

        // This function must be called only in the case that exists closest_line with boundary_idx equals to intersection.border_idx
        auto find_closest_line_index = [&closest_lines](const Intersection &intersection) -> size_t {
            for (const ClosestLine &closest_line : closest_lines)
                if (closest_line.border_idx == intersection.border_idx) return &closest_line - &closest_lines.front();
            // This is an invalid state.
            assert(false);
            return std::numeric_limits<size_t>::max();
        };

        if (reverse) {
            for (const Intersection &intersection : boost::adaptors::reverse(intersections))
                if (boundaries_indices.find(intersection.border_idx) != boundaries_indices.end())
                    return find_closest_line_index(intersection);
        } else {
            for (const Intersection &intersection : intersections)
                if (boundaries_indices.find(intersection.border_idx) != boundaries_indices.end())
                    return find_closest_line_index(intersection);
        }
        return std::numeric_limits<size_t>::max();
    };

    std::vector<Intersection> new_intersections = intersections;
    if (!new_intersections.empty() && !start_lines.empty()) {
        size_t cl_start_idx = get_closer(start_lines, new_intersections.front(), start);
        if (cl_start_idx != std::numeric_limits<size_t>::max()) {
            // If there is any ClosestLine around the start point closer to the Intersection, then replace this Intersection with ClosestLine.
            const ClosestLine &cl_start = start_lines[cl_start_idx];
            new_intersections.front()   = {cl_start.border_idx, cl_start.line_idx, cl_start.point, compute_distance(cl_start), true};
        } else {
            // Check if there is any ClosestLine with the same boundary_idx as any Intersection. If this ClosestLine exists, then add it to the
            // vector of intersections. This allows in some cases when it is more than one around ClosestLine start point chose that one which
            // minimizes the number of contours (also length of the detour) in result detour. If there doesn't exist any ClosestLine like this, then
            // use the first one, which is the closest one to the start point.
            size_t             start_closest_lines_idx = find_closest_line_with_same_boundary_idx(start_lines, new_intersections, true);
            const ClosestLine &cl_start                = (start_closest_lines_idx != std::numeric_limits<size_t>::max()) ? start_lines[start_closest_lines_idx] : start_lines.front();
            new_intersections.insert(new_intersections.begin(),{cl_start.border_idx, cl_start.line_idx, cl_start.point, compute_distance(cl_start), true});
        }
    }

    if (!new_intersections.empty() && !end_lines.empty()) {
        size_t cl_end_idx = get_closer(end_lines, new_intersections.back(), end);
        if (cl_end_idx != std::numeric_limits<size_t>::max()) {
            // If there is any ClosestLine around the end point closer to the Intersection, then replace this Intersection with ClosestLine.
            const ClosestLine &cl_end = end_lines[cl_end_idx];
            new_intersections.back()  = {cl_end.border_idx, cl_end.line_idx, cl_end.point, compute_distance(cl_end), true};
        } else {
            // Check if there is any ClosestLine with the same boundary_idx as any Intersection. If this ClosestLine exists, then add it to the
            // vector of intersections. This allows in some cases when it is more than one around ClosestLine end point chose that one which
            // minimizes the number of contours (also length of the detour) in result detour. If there doesn't exist any ClosestLine like this, then
            // use the first one, which is the closest one to the end point.
            size_t             end_closest_lines_idx = find_closest_line_with_same_boundary_idx(end_lines, new_intersections, false);
            const ClosestLine &cl_end                = (end_closest_lines_idx != std::numeric_limits<size_t>::max()) ? end_lines[end_closest_lines_idx] : end_lines.front();
            new_intersections.push_back({cl_end.border_idx, cl_end.line_idx, cl_end.point, compute_distance(cl_end), true});
        }
    }
    return new_intersections;
}

// point_idx is the index from which is different vertex is searched.
template<bool forward>
static Point find_first_different_vertex(const Polygon &polygon, const size_t point_idx, const Point &point)
{
    assert(point_idx < polygon.size());
    // Solve case when vertex on passed index point_idx is different that pass point. This helps the following code keep simple.
    if (point != polygon.points[point_idx])
        return polygon.points[point_idx];

    auto line_idx = (int(point_idx) + 1) % int(polygon.points.size());
    assert(line_idx != int(point_idx));
    if constexpr (forward)
        for (; point == polygon.points[line_idx] && line_idx != int(point_idx); line_idx = line_idx + 1 < int(polygon.points.size()) ? line_idx + 1 : 0);
    else
        for (; point == polygon.points[line_idx] && line_idx != int(point_idx); line_idx = line_idx - 1 >= 0 ? line_idx - 1 : int(polygon.points.size()) - 1);
    assert(point != polygon.points[line_idx]);
    return polygon.points[line_idx];
}

static Vec2d three_points_inward_normal(const Point &left, const Point &middle, const Point &right)
{
    assert(left != middle);
    assert(middle != right);
    return (perp(Point(middle - left)).cast<double>().normalized() + perp(Point(right - middle)).cast<double>().normalized()).normalized();
}

// Compute normal of the polygon's vertex in an inward direction
static Vec2d get_polygon_vertex_inward_normal(const Polygon &polygon, const size_t point_idx)
{
    const size_t left_idx  = prev_idx_modulo(point_idx, polygon.points);
    const size_t right_idx = next_idx_modulo(point_idx, polygon.points);
    const Point &middle    = polygon.points[point_idx];
    const Point &left      = find_first_different_vertex<false>(polygon, left_idx, middle);
    const Point &right     = find_first_different_vertex<true>(polygon, right_idx, middle);
    return three_points_inward_normal(left, middle, right);
}

// Compute offset of point_idx of the polygon in a direction of inward normal
static Point get_polygon_vertex_offset(const Polygon &polygon, const size_t point_idx, const int offset)
{
    return polygon.points[point_idx] + (get_polygon_vertex_inward_normal(polygon, point_idx) * double(offset)).cast<coord_t>();
}

// Compute offset (in the direction of inward normal) of the point(passed on "middle") based on the nearest points laying on the polygon (left_idx and right_idx).
static Point get_middle_point_offset(const Polygon &polygon, const size_t left_idx, const size_t right_idx, const Point &middle, const coord_t offset)
{
    const Point &left  = find_first_different_vertex<false>(polygon, left_idx, middle);
    const Point &right = find_first_different_vertex<true>(polygon, right_idx, middle);
    return middle + (three_points_inward_normal(left, middle, right) * double(offset)).cast<coord_t>();
}

static Polyline to_polyline(const std::vector<TravelPoint> &travel)
{
    Polyline result;
    result.points.reserve(travel.size());
    for (const TravelPoint &t_point : travel)
        result.append(t_point.point);
    return result;
}

#ifdef AVOID_CROSSING_PERIMETERS_DEBUG_OUTPUT
static void export_travel_to_svg(const Polygons                  &boundary,
                                 const Line                      &original_travel,
                                 const Polyline                  &result_travel,
                                 const std::vector<Intersection> &intersections,
                                 const std::string               &path)
{
    coordf_t      stroke_width = scale_(0.05);
    BoundingBox   bbox         = get_extents(boundary);
    ::Slic3r::SVG svg(path, bbox);
    svg.draw(to_polylines(boundary), "green", stroke_width);
    svg.draw(original_travel, "blue", stroke_width);
    svg.draw(result_travel, "red", stroke_width);
    svg.draw(original_travel.a, "black", stroke_width);
    svg.draw(original_travel.b, "grey", stroke_width);

    for (const Intersection &intersection : intersections)
        svg.draw(intersection.point, "lightseagreen", stroke_width);
}

static void export_travel_to_svg(const Polygons                  &boundary,
                                 const Line                      &original_travel,
                                 const std::vector<TravelPoint>  &result_travel,
                                 const std::vector<Intersection> &intersections,
                                 const std::string               &path)
{
    export_travel_to_svg(boundary, original_travel, to_polyline(result_travel), intersections, path);
}
#endif /* AVOID_CROSSING_PERIMETERS_DEBUG_OUTPUT */

// Returns a direction of the shortest path along the polygon boundary
enum class Direction { Forward, Backward };
// Returns a direction of the shortest path along the polygon boundary
static Direction get_shortest_direction(const AvoidCrossingPerimeters::Boundary &boundary,
                                        const Intersection                      &intersection_first,
                                        const Intersection                      &intersection_second,
                                        float                                    contour_length)
{
    assert(intersection_first.border_idx == intersection_second.border_idx);
    const Polygon &poly        = boundary.boundaries[intersection_first.border_idx];
    float          dist_first  = intersection_first.distance;
    float          dist_second = intersection_second.distance;

    assert(dist_first  >= 0.f && dist_first  <= contour_length);
    assert(dist_second >= 0.f && dist_second <= contour_length);

    bool reversed = false;
    if (dist_first > dist_second) {
        std::swap(dist_first, dist_second);
        reversed = true;
    }
    float total_length_forward  = dist_second - dist_first;
    float total_length_backward = dist_first + contour_length - dist_second;
    if (reversed) std::swap(total_length_forward, total_length_backward);

    total_length_forward  -= (intersection_first.point - poly[intersection_first.line_idx]).cast<float>().norm();
    total_length_backward -= (poly[(intersection_first.line_idx + 1) % poly.size()] - intersection_first.point).cast<float>().norm();

    total_length_forward  -= (poly[(intersection_second.line_idx + 1) % poly.size()] - intersection_second.point).cast<float>().norm();
    total_length_backward -= (intersection_second.point - poly[intersection_second.line_idx]).cast<float>().norm();

    if (total_length_forward < total_length_backward) return Direction::Forward;
    return Direction::Backward;
}

// Straighten the travel path as long as it does not collide with the contours stored in edge_grid.
static std::vector<TravelPoint> simplify_travel(const AvoidCrossingPerimeters::Boundary &boundary, const std::vector<TravelPoint> &travel)
{
    if(travel.size() < 3)
        return travel;

    FirstIntersectionVisitor visitor(boundary.grid);
    std::vector<TravelPoint> simplified_path;
    simplified_path.reserve(travel.size());
    simplified_path.emplace_back(travel.front());

    // Try to skip some points in the path.
    //FIXME maybe use a binary search to trim the line?
    //FIXME how about searching tangent point at long segments? 
    Point current_point = travel.front().point;
    for (size_t point_idx = 1; point_idx < travel.size(); ++point_idx) {
        TravelPoint  next          = travel[point_idx];
        assert(current_point == simplified_path.back().point);

        // check for first point dist, if same point as next, then fuse them.
        if (point_idx == 1 && current_point.coincides_with_epsilon(next.point)) {
            simplified_path.front().do_not_remove = next.do_not_remove;
            simplified_path.front().border_idx = next.border_idx;
            continue;
        }

        if (current_point.coincides_with_epsilon(next.point) && point_idx + 1 < travel.size()) {
            if (next.do_not_remove && !simplified_path.back().do_not_remove) {
                simplified_path.back() = next;
                current_point = next.point;
            }
            continue;
        }

        visitor.pt_current = &current_point;
        if (!next.do_not_remove) {
            // if same point (but not the last) -> skip it
            if (current_point.coincides_with_epsilon(next.point) && point_idx + 1 < travel.size()) {
                continue;
            }
            // search for useless points after this one to skip them.
            for (size_t point_idx_2 = point_idx + 1; point_idx_2 < travel.size(); ++point_idx_2) {
                if (travel[point_idx_2].point.coincides_with_epsilon(current_point)) {
                    next = travel[point_idx_2];
                    point_idx = point_idx_2;
                    continue;
                }
#ifdef AVOID_CROSSING_PERIMETERS_DEBUG_OUTPUT
                visitor.intersection_pt = Point();
#endif
                visitor.pt_next = &travel[point_idx_2].point;
                boundary.grid.visit_cells_intersecting_line(*visitor.pt_current, *visitor.pt_next, visitor);
                // Check if deleting point causes crossing a boundary
                if (!visitor.intersect) {
                    next = travel[point_idx_2];
                    point_idx = point_idx_2;
                }

                //if the current dest point (point_idx_2) is do_not_remove, don't continue forward.
                // Workaround for some issue in MSVC 19.29.30037 32-bit compiler.
#if defined(_WIN32) && !defined(_WIN64)
                if (bool volatile do_not_remove = travel[point_idx_2].do_not_remove; do_not_remove)
                    break;
#else
                if (travel[point_idx_2].do_not_remove)
                    break;
#endif
            }
        }

        simplified_path.emplace_back(next);
        current_point = next.point;
    }

    // check for last point dist
    if (simplified_path.size() > 2 &&
        simplified_path.back().point.coincides_with_epsilon(simplified_path[simplified_path.size() - 2].point)) {
        auto & previous = simplified_path[simplified_path.size() - 2];
        previous.point = simplified_path.back().point;
        simplified_path.pop_back();
    }

    if (simplified_path.size() > 2) {
        for (size_t idx = 1; idx < simplified_path.size(); ++idx) {
            assert(!simplified_path[idx - 1].point.coincides_with_epsilon(simplified_path[idx].point));
        }
    }
    assert(simplified_path.front().point == travel.front().point);
    assert(simplified_path.back().point == travel.back().point);
    return simplified_path;
}

// called by get_perimeter_spacing() / get_perimeter_spacing_external()
static inline coord_t get_default_perimeter_spacing(const PrintObject &print_object)
{
    std::set<uint16_t> printing_extruders = print_object.object_extruders();
    assert(!printing_extruders.empty());
    coord_t avg_extruder = 0;
    for(uint16_t extruder_id : printing_extruders)
        avg_extruder += scale_i(print_object.print()->config().nozzle_diameter.get_at(extruder_id));
    avg_extruder /= printing_extruders.size();
    return avg_extruder;
}

// called by get_boundary() / avoid_perimeters_inner()
static coord_t get_perimeter_spacing(const Layer &layer)
{
    size_t regions_count     = 0;
    coord_t  perimeter_spacing = 0;
    for (const LayerRegion &layer_region : layer.regions())
        if (! layer_region.slices().empty()) {
            perimeter_spacing += layer_region.flow(frPerimeter).scaled_spacing();
            ++regions_count;
        }

    assert(perimeter_spacing >= 0.f);
    if (regions_count != 0)
        perimeter_spacing /= regions_count;
    else
        perimeter_spacing = get_default_perimeter_spacing(*layer.object());
    return perimeter_spacing;
}

// called by get_boundary_external()
static coord_t get_perimeter_spacing_external(const Layer &layer)
{
    size_t  regions_count     = 0;
    coord_t perimeter_spacing = 0.f;
    for (const PrintObject &object : layer.object()->print()->objects()) {
        assert(object.get_layer_at_printz(layer.unscaled_print_z(), EPSILON * 4) ==
               object.get_layer_at_printz(layer.scaled_print_z()));
        if (const Layer *l = object.get_layer_at_printz(layer.scaled_print_z()); l) {
            for (const LayerRegion &layer_region : l->regions()) {
                if (!layer_region.slices().empty()) {
                    perimeter_spacing += layer_region.flow(frPerimeter).scaled_spacing();
                    ++regions_count;
                }
            }
        }
    }

    assert(perimeter_spacing >= 0.f);
    if (regions_count != 0) {
        perimeter_spacing /= regions_count;
    } else {
        perimeter_spacing = get_default_perimeter_spacing(*layer.object());
    }
    return perimeter_spacing;
}

bool is_in_internal_boundary(const AvoidCrossingPerimeters::Boundary& boundary, const Point& pt) {
    for (const std::pair<ExPolygon, ExPolygon>& bound : boundary.boundary_growth)
        if (bound.second.contains(pt))
            return true;
    return false;
}

bool find_point_on_boundary(Point& pt_to_move, const AvoidCrossingPerimeters::Boundary& boundary, coord_t max_dist) {

    if (!is_in_internal_boundary(boundary, pt_to_move)) {
        //get nearest point
        EdgeGrid::Grid::ClosestPointResult pt_closest = boundary.grid.closest_point_signed_distance(pt_to_move, max_dist);
        Point contour_pt;
        if (pt_closest.contour_idx == size_t(-1)) {
            // manual search on edges :
            bool found = false;
            for (const std::pair<ExPolygon, ExPolygon> &bound : boundary.boundary_growth) {
                if (bound.first.contains(pt_to_move)) {
                    coordf_t dist2 = std::numeric_limits<coordf_t>::max();
                    for (const Polygon &poly : to_polygons(bound.second)) {
                        Point test_point = *poly.closest_point(pt_to_move);
                        coordf_t dist2_test = test_point.distance_to_square(pt_to_move);
                        if (dist2_test < dist2) {
                            dist2 = dist2_test;
                            contour_pt = test_point;
                            found = true;
                        }
                    }
                    if (std::sqrt(dist2) > max_dist)
                        for (const Polygon &poly : to_polygons(bound.second)) {
                            Point test_point = poly.point_projection(pt_to_move).first;
                            coordf_t dist2_test = test_point.distance_to_square(pt_to_move);
                            if (dist2_test < dist2) {
                                dist2 = dist2_test;
                                contour_pt = test_point;
                                found = true;
                            }
                        }
                    break;
                }
            }
            if (found) {
                assert(boundary.grid.bbox().contains(contour_pt));
                if (boundary.grid.bbox().contains(contour_pt)) {
                    //push it a bit to be sure it's inside
                    Line l{ pt_to_move, contour_pt };
                    l.extend_end(SCALED_EPSILON);
                    pt_to_move = l.b;
                    assert(boundary.grid.bbox().contains(pt_to_move));
                    return true;
                }
            }
            return false;
        } else {
            const EdgeGrid::Contour& pts = boundary.grid.contours()[pt_closest.contour_idx];
            contour_pt = pts.segment_start(pt_closest.start_point_idx).interpolate(pt_closest.t, pts.segment_end(pt_closest.start_point_idx));
        }
        //push it a bit to be sure it's inside
        Line l{ pt_to_move, contour_pt };
        l.extend_end(SCALED_EPSILON);
        pt_to_move = l.b;
        assert(boundary.grid.bbox().contains(pt_to_move));
    }
    return true;
}

// Returns average perimeter width calculated from all LayerRegion within the layer.
static float get_external_perimeter_width(const Layer &layer)
{
    size_t regions_count     = 0;
    float  perimeter_width   = 0.f;
    for (const LayerRegion &layer_region : layer.regions())
        if (! layer_region.slices().empty()) {
            perimeter_width += float(layer_region.flow(frExternalPerimeter).scaled_width());
            ++regions_count;
        }

    assert(perimeter_width >= 0.f);
    if (regions_count != 0)
        perimeter_width /= float(regions_count);
    else
        perimeter_width = get_default_perimeter_spacing(*layer.object());
    return perimeter_width;
}

static float get_external_perimeter_overlap(const Layer &layer)
{
    size_t regions_count     = 0;
    float  ext_perimeter_overlap   = 0.f;
    for (const LayerRegion &layer_region : layer.regions())
        if (! layer_region.slices().empty()) {
            ext_perimeter_overlap += layer_region.region().config().external_perimeter_overlap.value;
            ++regions_count;
        }

    assert(ext_perimeter_overlap >= 0.f);
    if (regions_count != 0) {
        ext_perimeter_overlap /= float(regions_count);
    } else {
        ext_perimeter_overlap = layer.object()->print()->default_region_config().external_perimeter_overlap.value;
    }
    return ext_perimeter_overlap;
}

std::vector<std::pair<int,int>> datapoints;
void print_debug_cross( int id,
                        const AvoidCrossingPerimeters::Boundary &boundary,
                       const Intersection &best_intersection_start,
                       const Intersection &best_intersection_end,
                       const Point &start,
                       const Point &end,
                       const std::string &suffix)
{
#ifdef AVOID_CROSSING_PERIMETERS_DEBUG_OUTPUT
    coordf_t      stroke_width = scale_d(0.05);
    BoundingBox   bbox         = get_extents(boundary.boundaries);
    static int iRun = 0;
    ::Slic3r::SVG svg(debug_out_path("AvoidCrossingPerimetersInner-simplify_%d_%d_dist_%s.svg", id, iRun++, suffix.c_str()), bbox);
    svg.draw(to_polylines(boundary.boundaries), "blue", stroke_width + scale_d(0.02));
    svg.draw(Polyline({start, end}), "cyan", stroke_width + scale_d(0.02));
    {
        const Points &points = boundary.boundaries[best_intersection_start.border_idx].points;
        svg.draw(Polyline({points[best_intersection_start.line_idx],
                           points[best_intersection_start.line_idx + 1 < points.size() ?
                                      best_intersection_start.line_idx + 1 :
                                      0]}),
                    "green", stroke_width );
    }
    {
        const Points &points = boundary.boundaries[best_intersection_end.border_idx].points;
        svg.draw(Polyline(
                     {points[best_intersection_end.line_idx],
                      points[best_intersection_end.line_idx + 1 < points.size() ? best_intersection_end.line_idx + 1 :
                                                                                  0]}),
                    "green", stroke_width);
    }
    svg.draw(Polyline({start, best_intersection_start.point}), "orange", stroke_width - scale_d(0.01));
    svg.draw(Polyline({best_intersection_end.point, end}), "orange", stroke_width - scale_d(0.01));
    svg.draw(Polyline({best_intersection_start.point, best_intersection_end.point}), "red", stroke_width - scale_d(0.02));
    svg.Close();
#endif
}

char brute_force_jump(Intersection &intersection_start,
                      Intersection &intersection_end,
                      Line start_end_line,
                      const Polygon &contour_start,
                      const Polygon &contour_end,
                      const double weight_island_travel = 0.4) {
    // brute-force version,
    distf_t shortest_dist = start_end_line.length() * (1 + weight_island_travel);
    distsqrf_t shortest_dist_sqr = sqr(shortest_dist);
    char type_dbg = 'N';

    // line-line
    Point res_end_saved;
    Point start = start_end_line.a;
    Point end = start_end_line.b;
    {
        for (size_t i = 0; i < contour_start.points.size(); ++i) {
            Line l(contour_start.points[i], contour_start.points[(i + 1) % contour_start.points.size()]);
            if (l.intersection(start_end_line, &intersection_start.point)) {
                if (intersection_start.point == l.b) {
                    intersection_start.line_idx = (i + 1) % contour_start.points.size();
                    intersection_start.distance = 0;
                } else {
                    intersection_start.line_idx = i;
                    intersection_start.distance = l.a.distance_to(intersection_start.point);
                }
                break;
            }
        }
        // intersection fail, mayeb the point is just out of it. search for nearest instead.
        if (intersection_start.line_idx == size_t(-1)) {
            distsqrf_t dist_best = shortest_dist_sqr;
            Point test_pt;
            for (size_t i = 0; i < contour_start.points.size(); ++i) {
                Line l(contour_start.points[i], contour_start.points[(i + 1) % contour_start.points.size()]);
                distsqrf_t dist_test = l.distance_to_squared(start, &test_pt);
                if (dist_test < dist_best) {
                    dist_best = dist_test;
                    intersection_start.point = test_pt;
                    if (intersection_start.point == l.b) {
                        intersection_start.line_idx = (i + 1) % contour_start.points.size();
                        intersection_start.distance = 0;
                    } else {
                        intersection_start.line_idx = i;
                        intersection_start.distance = l.a.distance_to(intersection_start.point);
                    }
                }
            }
        }
    }
    {
        for (size_t i = 0; i < contour_end.points.size(); ++i) {
            Line l(contour_end.points[i], contour_end.points[(i + 1) % contour_end.points.size()]);
            if (l.intersection(start_end_line, &intersection_end.point)) {
                if (intersection_end.point == l.b) {
                    intersection_end.line_idx = (i + 1) % contour_end.points.size();
                    intersection_end.distance = 0;
                } else {
                    intersection_end.line_idx = i;
                    intersection_end.distance = l.a.distance_to(intersection_end.point);
                }
                break;
            }
        }
        // intersection fail, mayeb the point is just out of it. search for nearest instead.
        if (intersection_end.line_idx == size_t(-1)) {
            distsqrf_t dist_best = shortest_dist_sqr;
            Point test_pt;
            for (size_t i = 0; i < contour_end.points.size(); ++i) {
                Line l(contour_end.points[i], contour_end.points[(i + 1) % contour_end.points.size()]);
                distsqrf_t dist_test = l.distance_to_squared(start, &test_pt);
                if (dist_test < dist_best) {
                    dist_best = dist_test;
                    intersection_end.point = test_pt;
                    if (intersection_end.point == l.b) {
                        intersection_end.line_idx = (i + 1) % contour_end.points.size();
                        intersection_end.distance = 0;
                    } else {
                        intersection_end.line_idx = i;
                        intersection_end.distance = l.a.distance_to(intersection_end.point);
                    }
                }
            }
        }
    }
    assert(intersection_start.line_idx != size_t(-1));
    assert(intersection_end.line_idx != size_t(-1));
    Intersection best_intersection_end_saved = intersection_end;
    // for (size_t i_end = 0; i_end < contour_end.size(); i_end++)
    {
        // point-line
        Point res;
        distsqrf_t new_dist_sqr =
            Line(contour_end.points[intersection_end.line_idx],
                    contour_end.points[(intersection_end.line_idx + 1) % contour_end.points.size()])
                .distance_to_squared(intersection_start.point, &res);
        if (new_dist_sqr < shortest_dist_sqr) {
            distf_t new_dist = std::sqrt(new_dist_sqr);
            // TODO: here it's fly-distance, it's better to store & use the contour distance from
            // start/end to the contour point.
            new_dist += start.distance_to(intersection_start.point) * weight_island_travel +
                res.distance_to(end) * weight_island_travel;
            if (new_dist < shortest_dist) {
                type_dbg = 'A';
                assert(new_dist > 0);
                shortest_dist = new_dist;
                shortest_dist_sqr = sqr(shortest_dist);
                if (res == contour_end.points[intersection_end.line_idx]) {
                    type_dbg = 'a';
                    intersection_end.point = contour_end.points[intersection_end.line_idx];
                    intersection_end.distance = 0;
                } else if (res ==
                            contour_end
                                .points[(intersection_end.line_idx + 1) % contour_end.points.size()]) {
                    type_dbg = 'o';
                    intersection_end.line_idx = (intersection_end.line_idx + 1) %
                        contour_end.points.size();
                    intersection_end.point = contour_end.points[intersection_end.line_idx];
                    intersection_end.distance = 0;
                } else {
                    type_dbg = 'i';
                    intersection_end.point = res;
                    intersection_end.distance = contour_end.points[intersection_end.line_idx]
                                                                .distance_to(res);
                }
                //print_debug_cross(i_id_run, boundary, intersection_start, intersection_end,
                //                    start, end, std::string("dbg_startpt_") + std::to_string(new_dist));
            }
        }
    }
    // std::cout<<" -"<<type_dbg<<" : "<<shortest_dist<<"\n";
    // for (size_t i_start = 0; i_start < contour_start.size(); i_start++)
    {
        // line-point
        Point res;
        distsqrf_t new_dist_sqr =
            Line(contour_start.points[intersection_start.line_idx],
                    contour_start.points[(intersection_start.line_idx + 1) % contour_start.points.size()])
                .distance_to_squared(best_intersection_end_saved.point, &res);
        res_end_saved = res;
        if (new_dist_sqr < shortest_dist_sqr) {
            distf_t new_dist = std::sqrt(new_dist_sqr);
            new_dist += start.distance_to(res) * weight_island_travel +
                best_intersection_end_saved.point.distance_to(end) * weight_island_travel;
            if (new_dist < shortest_dist) {
                intersection_end = best_intersection_end_saved;
                type_dbg = 'B';
                assert(new_dist > 0);
                shortest_dist = new_dist;
                shortest_dist_sqr = sqr(shortest_dist);
                if (res == contour_start.points[intersection_start.line_idx]) {
                    type_dbg = 'b';
                    intersection_start.point = contour_start.points[intersection_start.line_idx];
                    intersection_start.distance = 0;
                } else if (res ==
                            contour_start
                                .points[(intersection_start.line_idx + 1) % contour_start.points.size()]) {
                    type_dbg = 'c';
                    intersection_start.line_idx = (intersection_start.line_idx + 1) %
                        contour_start.points.size();
                    intersection_start.point = contour_start.points[intersection_start.line_idx];
                    intersection_start.distance = 0;
                } else {
                    type_dbg = 'g';
                    intersection_start.point = res;
                    intersection_start.distance =
                        contour_start.points[intersection_start.line_idx].distance_to(res);
                }
                //print_debug_cross(i_id_run, boundary, intersection_start, intersection_end,
                //                    start, end, std::string("dbg_endpt_") + std::to_string(new_dist));
            }
        }
    }
    
    for (size_t i_start = 0; i_start < contour_start.size(); i_start++) {
        // TODO: count boundary dist, not fly dist
        for (size_t i_end = 0; i_end < contour_end.size(); i_end++) {
            // point-line
            Point res;
            distf_t new_dist = Line(contour_end.points[i_end],
                                    contour_end.points[(i_end + 1) % contour_end.points.size()])
                                   .distance_to_squared(contour_start.points[i_start], &res);
            if (new_dist < shortest_dist_sqr) {
                new_dist = std::sqrt(new_dist);
                // TODO: here it's fly-distance, it's better to store & use the contour distance from
                // start/end to the contour point.
                new_dist += start.distance_to(contour_start.points[i_start]) * weight_island_travel +
                    res.distance_to(end) * weight_island_travel;
                if (new_dist < shortest_dist) {
                    type_dbg = 'P';
                    assert(new_dist > 0);
                    shortest_dist = new_dist;
                    shortest_dist_sqr = sqr(shortest_dist);
                    intersection_start.line_idx = i_start;
                    intersection_start.point = contour_start.points[i_start];
                    intersection_start.distance = 0;
                    if (res == contour_end.points[i_end]) {
                        type_dbg = 'Q';
                        intersection_end.line_idx = i_end;
                        intersection_end.point = contour_end.points[i_end];
                        intersection_end.distance = 0;
                    } else if (res == contour_end.points[(i_end + 1) % contour_end.points.size()]) {
                        type_dbg = 'R';
                        intersection_end.line_idx = (i_end + 1) % contour_end.points.size();
                        intersection_end.point = contour_end.points[(i_end + 1) % contour_end.points.size()];
                        intersection_end.distance = 0;
                    } else {
                        intersection_end.line_idx = i_end;
                        intersection_end.point = res;
                        intersection_end.distance = contour_end.points[i_end].distance_to(res);
                    }
                    //print_debug_cross(i_id_run, boundary, intersection_start, intersection_end,
                    //                  start, end, std::string("dbg_startpt_") + std::to_string(new_dist));
                }
            }
            // line-point
            new_dist = Line(contour_start.points[i_start],
                            contour_start.points[(i_start + 1) % contour_start.points.size()])
                           .distance_to_squared(contour_end.points[i_end], &res);
            if (new_dist < shortest_dist_sqr) {
                new_dist = std::sqrt(new_dist);
                new_dist += start.distance_to(res) * weight_island_travel +
                    contour_end.points[i_end].distance_to(end) * weight_island_travel;
                if (new_dist < shortest_dist) {
                    type_dbg = 'L';
                    assert(new_dist > 0);
                    shortest_dist = new_dist;
                    shortest_dist_sqr = sqr(shortest_dist);
                    if (res == contour_start.points[i_start]) {
                        intersection_start.line_idx = i_start;
                        intersection_start.point = contour_start.points[i_start];
                        intersection_start.distance = 0;
                    } else if (res == contour_start.points[(i_start + 1) % contour_start.points.size()]) {
                        intersection_start.line_idx = (i_start + 1) % contour_start.points.size();
                        intersection_start.point = contour_start
                                                                .points[(i_start + 1) % contour_start.points.size()];
                        intersection_start.distance = 0;
                    } else {
                        intersection_start.line_idx = i_start;
                        intersection_start.point = res;
                        intersection_start.distance = contour_start.points[i_start].distance_to(res);
                    }
                    intersection_end.line_idx = i_end;
                    intersection_end.point = contour_end.points[i_end];
                    intersection_end.distance = 0;
                    //print_debug_cross(i_id_run, boundary, intersection_start, intersection_end,
                    //                  start, end, std::string("dbg_endpt_") + std::to_string(new_dist));
                }
            }
        }
    }

    return type_dbg;
}

// TODO: avoid other islands in-between
static void jump_between_island(AvoidCrossingPerimeters::Boundary &boundary, // not const because of possible late init of island_to_grid
                                      Point start,
                                      Point end,
                                      std::vector<Intersection> &intersections,
                                const coord_t search_radius,
                                const double weight_island_travel = 0.4) {
    constexpr int GRID_RESOLUTION_MULT = 4; // TODO: find a way to have a good value, or create treegrid
    static int i_id_run = 0;
    i_id_run++;
    assert(intersections.size() > 1);
    // simplify the intersections:
    // if start & end are not on same border_id => not withtin same island
    if (intersections.size() > 1 &&
        boundary.islands[intersections.front().border_idx] != boundary.islands[intersections.back().border_idx]) {

        // recreate a new intersection chain
        // get intersections point that are on island_start or island_end, to uses them to check for inter-island
        // travel. also try to find nearest bridges between the islands.
        const distf_t strait_dist = start.distance_to(end);
        // is one island inside a hole or not?
        size_t start_island = boundary.islands[intersections.front().border_idx];
        size_t end_island = boundary.islands[intersections.back().border_idx];
        if (boundary.bboxes[start_island].contains(boundary.bboxes[end_island])) {
            // find hole
            size_t hole_contour = size_t(-1);
            for (size_t hole_idx = start_island + 1;
                 hole_idx < boundary.islands.size() && boundary.islands[hole_idx] == start_island; ++hole_idx) {
                if (boundary.bboxes[hole_idx].contains(boundary.bboxes[end_island])) {
                    hole_contour = hole_idx;
                    break;
                }
            }
            // if in a hole (and not next to the contour in the bbox)
            // bboxes overlap, but maybe the polygon not
            if (hole_contour != size_t(-1)) {
                start_island = hole_contour;
            }
        }
        if (boundary.bboxes[end_island].contains(boundary.bboxes[start_island])) {
            // find hole
            size_t hole_contour = size_t(-1);
            for (size_t hole_idx = end_island + 1;
                 hole_idx < boundary.islands.size() && boundary.islands[hole_idx] == end_island; ++hole_idx) {
                if (boundary.bboxes[hole_idx].contains(boundary.bboxes[start_island])) {
                    hole_contour = hole_idx;
                    break;
                }
            }
            // if in a hole (and not next to the contour in the bbox)
            // bboxes overlap, but maybe the polygon not
            if (hole_contour != size_t(-1)) {
                end_island = hole_contour;
            }
        }
        const Polygon &contour_start = boundary.boundaries[start_island];
        const Polygon &contour_end = boundary.boundaries[end_island];

        auto time_start = std::chrono::high_resolution_clock::now();

        Intersection best_intersection_start;
        best_intersection_start.border_idx = start_island;
        best_intersection_start.line_idx = -1;
        best_intersection_start.point = contour_start.points.front();
        best_intersection_start.distance = 0;
        best_intersection_start.do_not_remove = true;
        Intersection best_intersection_end;
        best_intersection_end.border_idx = end_island;
        best_intersection_end.line_idx = -1;
        best_intersection_end.point = contour_end.points.front();
        best_intersection_end.distance = 0;
        best_intersection_end.do_not_remove = true;
        distf_t shortest_dist = strait_dist * (1 + weight_island_travel);
        distsqrf_t shortest_dist_sqr = sqr(shortest_dist);

        // create edgegrid if not present already
        // note: create & use the grid allow to increase the speed if the number of point is high enough
        // if notenough point, a brute-force algo is more time efficient.
        if (boundary.island_to_grid.find(int(start_island)) == boundary.island_to_grid.end()) {
            EdgeGrid::Grid &grid = boundary.island_to_grid[int(start_island)];
            grid.set_bbox(boundary.grid.bbox());
            // reduce the resolution, to have less cells. there is a need to have a cell tree, with a higher level of cell to evict quicker.
            grid.create(boundary.boundaries[start_island], boundary.grid.resolution() * GRID_RESOLUTION_MULT);
            // calculate sdf to use signed_distance_bilinear to evict candidate quicker.
            //grid.calculate_sdf();
        }
        EdgeGrid::Grid &start_grid = boundary.island_to_grid[int(start_island)];

        if (boundary.island_to_grid.find(int(end_island)) == boundary.island_to_grid.end()) {
            EdgeGrid::Grid &grid = boundary.island_to_grid[int(end_island)];
            grid.set_bbox(boundary.grid.bbox());
            grid.create(boundary.boundaries[end_island], boundary.grid.resolution() * GRID_RESOLUTION_MULT);
            // calculate sdf to use signed_distance_bilinear to evict candidate quicker.
            //grid.calculate_sdf();
        }
        EdgeGrid::Grid &end_grid = boundary.island_to_grid[int(end_island)];
        size_t shortest_cell_dist = 1 + size_t(shortest_dist / start_grid.resolution());

        // compute the distance from start for each contour point
        // far too pessimistic without any simplification.
        //std::vector<distf_t> dist_from_start(contour_start.size());
        //{
        //    static int iRun = 0;
        //    // find the nearest point
        //    EdgeGrid::Grid::ClosestPointResult pt_closest = start_grid.closest_point_signed_distance(start, strait_dist);
        //    assert(pt_closest.contour_idx == 0);
        //    assert(contour_start.points[pt_closest.start_point_idx] == start_grid.contours().front().segment_start(pt_closest.start_point_idx));
        //    Point contour_pt = contour_start.points[pt_closest.start_point_idx].interpolate(pt_closest.t, contour_start.points[(pt_closest.start_point_idx + 1) % contour_start.size()]);
        //    // from the nearest point, dijkstra in both direction to the last point.
        //    size_t idx_decr = pt_closest.start_point_idx;
        //    size_t idx_incr = (pt_closest.start_point_idx + 1) % contour_start.size();
        //    distf_t distance_decr = std::abs(pt_closest.distance) + contour_pt.distance_to(contour_start.points[idx_decr]);
        //    assert(distance_decr > start.distance_to(contour_start.points[idx_decr]));
        //    distf_t distance_incr = std::abs(pt_closest.distance) + contour_pt.distance_to(contour_start.points[idx_incr]);
        //    assert(distance_incr > start.distance_to(contour_start.points[idx_incr]));
        //    dist_from_start[idx_decr] = distance_decr;
        //    dist_from_start[idx_incr] = distance_incr;
        //    // already two points filled, so start at 2 instead of 0
        //    for (size_t need_to_fill = 2; need_to_fill < dist_from_start.size(); ++need_to_fill) {
        //        if (distance_incr <= distance_decr) {
        //            size_t next_idx_incr = (idx_incr + 1) % contour_start.size();
        //            distance_incr += contour_start.points[idx_incr].distance_to(contour_start.points[next_idx_incr]);
        //            idx_incr = next_idx_incr;
        //            assert(dist_from_start[idx_incr] == 0);
        //            dist_from_start[idx_incr] = distance_incr;
        //            assert(dist_from_start[idx_incr] > start.distance_to(contour_start.points[idx_incr]));
        //        } else {
        //            size_t prev_idx_decr = (idx_decr == 0) ? (contour_start.size() - 1) : (idx_decr - 1);
        //            distance_decr += contour_start.points[idx_decr].distance_to(contour_start.points[prev_idx_decr]);
        //            idx_decr = prev_idx_decr;
        //            assert(dist_from_start[idx_decr] == 0);
        //            dist_from_start[idx_decr] = distance_decr;
        //            assert(dist_from_start[idx_decr] > start.distance_to(contour_start.points[idx_decr]));
        //        }
        //    }
        //    for (distf_t d: dist_from_start) assert(d > 0);
        //}
        
#ifdef _DEBUG
        auto time_cell = std::chrono::high_resolution_clock::now();
#endif
        Line start_end_line(start, end);
        // initialize best_intersection_start with intersection against contour_start
        {
            FirstIntersectionVisitor visitor(start_grid);
            visitor.pt_current = &start;
            visitor.pt_next = &end;
            start_grid.visit_cells_intersecting_line(start, end, visitor);
            if (!visitor.intersect) {
                // probably a epsilon-difference for the intersection to happen.
                // try with a visitor that look at epsilon distance, and modify the start point to the intersection
                FirstEpsilonIntersectionVisitor visitor_epsilon(start_grid, start_end_line.a, start_end_line.b);
                start_grid.visit_cells_intersecting_line(start, end, visitor_epsilon);
                assert(visitor_epsilon.intersect);
                visitor.intersect = visitor_epsilon.intersect;
                visitor.intersection_contour_idx = visitor_epsilon.intersection_contour_idx;
                visitor.intersection_line_idx = visitor_epsilon.intersection_line_idx;
                assert(start != start_end_line.a);
                start = start_end_line.a;
            }
            if (visitor.intersect) {
                assert(visitor.intersection_contour_idx == 0);
                assert(visitor.intersection_line_idx >= 0);
                best_intersection_start.line_idx = visitor.intersection_line_idx;
                assert(best_intersection_start.line_idx < contour_start.points.size());
                bool found = start_end_line.intersection(
                    Line(contour_start.points[best_intersection_start.line_idx],
                            contour_start.points[(best_intersection_start.line_idx + 1) % contour_start.points.size()]),
                    &best_intersection_start.point);
                if (found) {
                    best_intersection_start.distance = contour_start.points[best_intersection_start.line_idx]
                                                        .distance_to(best_intersection_start.point);
                }
            }
        }
        // initialize best_intersection_end with intersection against contour_end
        {
            FirstIntersectionVisitor visitor(end_grid);
            visitor.pt_current = &start;
            visitor.pt_next = &end;
            end_grid.visit_cells_intersecting_line(start, end, visitor);
            if (!visitor.intersect) {
                // probably a epsilon-difference for the intersection to happen.
                // try with a visitor that look at epsilon distance, and modify the start point to the intersection
                FirstEpsilonIntersectionVisitor visitor_epsilon(end_grid, start_end_line.a, start_end_line.b);
                assert(visitor_epsilon.intersect);
                start_grid.visit_cells_intersecting_line(start, end, visitor_epsilon);
                visitor.intersect = visitor_epsilon.intersect;
                visitor.intersection_contour_idx = visitor_epsilon.intersection_contour_idx;
                visitor.intersection_line_idx = visitor_epsilon.intersection_line_idx;
                assert(start != start_end_line.a);
                start = start_end_line.a;
            }
            if (visitor.intersect) {
                assert(visitor.intersection_contour_idx == 0);
                assert(visitor.intersection_line_idx >= 0);
                best_intersection_end.line_idx = visitor.intersection_line_idx;
                assert(best_intersection_end.line_idx < contour_end.points.size());
                bool found = start_end_line.intersection(
                    Line(contour_end.points[best_intersection_end.line_idx],
                            contour_end.points[(best_intersection_end.line_idx + 1) % contour_end.points.size()]),
                    &best_intersection_end.point);
                if (found) {
                    best_intersection_end.distance = contour_end.points[best_intersection_end.line_idx]
                                                        .distance_to(best_intersection_end.point);
                }
            }
        }
        if (best_intersection_start.line_idx >= contour_start.size() ||
            best_intersection_end.line_idx >= contour_end.size()) {
            // a grid search has failed.
            // brute-force
            brute_force_jump(best_intersection_start, best_intersection_end, Line(start,end), contour_start, contour_end, weight_island_travel);
        } else {
            // continue with grids

            assert(best_intersection_start.line_idx < contour_start.size());
            assert(best_intersection_end.line_idx < contour_end.size());
            // line-line
            distf_t s1, s2, s3;
            distf_t e1, e2, e3;
            Point res_end;
            {
                Point res_start;
                distf_t new_dist_from_start =
                    Line(contour_end.points[best_intersection_end.line_idx],
                         contour_end.points[(best_intersection_end.line_idx + 1) % contour_end.points.size()])
                        .distance_to_squared(best_intersection_start.point, &res_start);
                s1 = start.distance_to(best_intersection_start.point) * weight_island_travel;
                s2 = std::sqrt(new_dist_from_start);
                new_dist_from_start = start.distance_to(best_intersection_start.point) * weight_island_travel +
                    std::sqrt(new_dist_from_start) + res_start.distance_to(end) * weight_island_travel;
                s3 = res_start.distance_to(end) * weight_island_travel;
                assert(is_approx(s2, best_intersection_start.point.distance_to(res_start), 2.));
                assert(is_approx(s1 + s2 + s3, new_dist_from_start, 2.));
                // Point res_end;
                distf_t new_dist_from_end =
                    Line(contour_start.points[best_intersection_start.line_idx],
                         contour_start.points[(best_intersection_start.line_idx + 1) % contour_start.points.size()])
                        .distance_to_squared(best_intersection_end.point, &res_end);
                e1 = start.distance_to(res_end) * weight_island_travel;
                e2 = std::sqrt(new_dist_from_end);
            new_dist_from_end = start.distance_to(res_end) * weight_island_travel + std::sqrt(new_dist_from_end) +
                best_intersection_end.point.distance_to(end) * weight_island_travel;
            e3 = best_intersection_end.point.distance_to(end) * weight_island_travel;
            if (new_dist_from_start <= new_dist_from_end) {
                assert(new_dist_from_start > 0);
                shortest_dist = new_dist_from_start;
                shortest_dist_sqr = sqr(shortest_dist);
                best_intersection_end = best_intersection_end;
                if (res_start == contour_end.points[best_intersection_end.line_idx]) {
                    best_intersection_end.point = contour_end.points[best_intersection_end.line_idx];
                    best_intersection_end.distance = 0;
                } else if (res_start ==
                           contour_end.points[(best_intersection_end.line_idx + 1) % contour_end.points.size()]) {
                    best_intersection_end.line_idx = (best_intersection_end.line_idx + 1) %
                        contour_end.points.size();
                    best_intersection_end.point = contour_end.points[best_intersection_end.line_idx];
                    best_intersection_end.distance = 0;
                } else {
                    best_intersection_end.point = res_start;
                    best_intersection_end.distance = contour_end.points[best_intersection_end.line_idx]
                                                             .distance_to(res_start);
                }
                print_debug_cross(i_id_run, boundary, best_intersection_start, best_intersection_end, start, end,
                                  std::string("dbg_startpt_") + std::to_string(new_dist_from_start));
            } else {
                assert(new_dist_from_end > 0);
                shortest_dist = new_dist_from_end;
                shortest_dist_sqr = sqr(shortest_dist);
                if (res_end == contour_start.points[best_intersection_start.line_idx]) {
                    best_intersection_start.point = contour_start.points[best_intersection_start.line_idx];
                        best_intersection_start.distance = 0;
                    } else if (res_end == contour_start.points[
                            (best_intersection_start.line_idx + 1) % contour_start.points.size()]) {
                        best_intersection_start.line_idx = (best_intersection_start.line_idx + 1) %
                            contour_start.points.size();
                        best_intersection_start.point = contour_start.points[best_intersection_start.line_idx];
                        best_intersection_start.distance = 0;
                    } else {
                        best_intersection_start.point = res_end;
                        best_intersection_start.distance = contour_start.points[best_intersection_start.line_idx]
                                                               .distance_to(res_end);
                    }
                    print_debug_cross(i_id_run, boundary, best_intersection_start, best_intersection_end, start, end,
                                      std::string("dbg_endpt_") + std::to_string(new_dist_from_end));
                }
            }

            // for each grid cell, if they are near enough
            // BoundingBox start_cells = start_grid.get_cells_intersecting_box(bb_start);
            // BoundingBox end_cells = end_grid.get_cells_intersecting_box(bb_end);
            for (size_t start_iy = 0; start_iy < start_grid.rows(); ++start_iy) {
                const size_t min_end_y = start_iy > shortest_cell_dist ? start_iy - shortest_cell_dist : size_t(0);
                const size_t max_end_y = std::min(1 + start_iy + shortest_cell_dist, end_grid.rows());
                for (size_t start_ix = 0; start_ix < start_grid.cols(); ++start_ix) {
                    const size_t min_end_x = start_ix > shortest_cell_dist ? start_ix - shortest_cell_dist :
                                                                             size_t(0);
                    const size_t max_end_x = std::min(1 + start_ix + shortest_cell_dist, end_grid.cols());
                    // check if the cell has any data
                    if (!start_grid.cell_has_data(start_iy, start_ix)) {
                        continue;
                    }
                    // TODO: use signed_distance_bilinear with the center of this cell to check if there is something near.
                    auto start_cell_data_range = start_grid.cell_data_range(start_iy, start_ix);
                    for (size_t end_iy = min_end_y; end_iy < max_end_y; ++end_iy) {
                        assert(end_iy >= 0 && end_iy < end_grid.rows());
                        for (size_t end_ix = min_end_x; end_ix < max_end_x; ++end_ix) {
                            assert(end_ix >= 0 && end_ix < end_grid.cols());
                            // check if the cell has any data
                            if (!end_grid.cell_has_data(end_iy, end_ix)) {
                                continue;
                            }
                            auto end_cell_data_range = end_grid.cell_data_range(end_iy, end_ix);
                            // both cell are near enough and have data inside, compare them.
                            for (auto it_start_contour_and_segment = start_cell_data_range.first;
                                 it_start_contour_and_segment != start_cell_data_range.second;
                                 ++it_start_contour_and_segment) {
                                Line start_line = start_grid.line(*it_start_contour_and_segment);
                                assert(it_start_contour_and_segment->first == 0); // only one contour
                                for (auto it_end_contour_and_segment = end_cell_data_range.first;
                                     it_end_contour_and_segment != end_cell_data_range.second;
                                     ++it_end_contour_and_segment) {
                                    Line end_line = end_grid.line(*it_end_contour_and_segment);
                                    assert(it_end_contour_and_segment->first == 0); // only one contour
                                    // point-line
                                    Point res;
                                    distf_t new_dist = end_line.distance_to_squared(start_line.a, &res);
                                    if (new_dist < shortest_dist_sqr) {
                                        new_dist = std::sqrt(new_dist);
                                        new_dist += start.distance_to(start_line.a) * weight_island_travel +
                                            res.distance_to(end) * weight_island_travel;
                                        // this dist can be far too pessimistic, doing the whole loop instead of just
                                        // travelling in strait line.
                                        // assert(dist_from_start[it_start_contour_and_segment->second] >
                                        // start.distance_to(start_line.a)); new_dist +=
                                        // dist_from_start[it_start_contour_and_segment->second] * weight_island_travel +
                                        //    res.distance_to(end) * weight_island_travel;
                                        if (new_dist < shortest_dist) {
                                            assert(new_dist > 0);
                                            shortest_dist = new_dist;
                                            shortest_dist_sqr = sqr(shortest_dist);
                                            shortest_cell_dist = 1 + shortest_dist / start_grid.resolution();
                                            best_intersection_start.line_idx = it_start_contour_and_segment->second;
                                            best_intersection_start.point = start_line.a;
                                            best_intersection_start.distance = 0;
                                            if (res == end_line.a) {
                                                best_intersection_end.line_idx = it_end_contour_and_segment->second;
                                                best_intersection_end.point = end_line.a;
                                                best_intersection_end.distance = 0;
                                            } else if (res == end_line.b) {
                                                best_intersection_end.line_idx =
                                                    (it_end_contour_and_segment->second + 1) % contour_end.size();
                                                best_intersection_end.point = end_line.b;
                                                best_intersection_end.distance = 0;
                                            } else {
                                                best_intersection_end.line_idx = it_end_contour_and_segment->second;
                                                best_intersection_end.point = res;
                                                best_intersection_end.distance = end_line.a.distance_to(res);
                                            }
                                            assert(best_intersection_start.line_idx < contour_start.points.size() &&
                                                   contour_start.points[best_intersection_start.line_idx] ==
                                                       best_intersection_start.point);
                                            print_debug_cross(i_id_run, boundary, best_intersection_start,
                                                              best_intersection_end, start, end,
                                                              std::string("startpt_") + std::to_string(new_dist));
                                        }
                                    }
                                    // line-point
                                    new_dist = start_line.distance_to_squared(end_line.a, &res);
                                    if (new_dist < shortest_dist_sqr) {
                                        new_dist = std::sqrt(new_dist);
                                        assert(start_line.a ==
                                               contour_start.points[it_start_contour_and_segment->second]);
                                        new_dist += start.distance_to(res) * weight_island_travel +
                                            end_line.a.distance_to(end) * weight_island_travel;
                                        // this dist can be far too pessimistic, doing the whole loop instead of just
                                        // travelling in strait line.
                                        // new_dist += (std::min(dist_from_start[it_start_contour_and_segment->second] +
                                        //                          start_line.a.distance_to(res),
                                        //                      dist_from_start[(it_start_contour_and_segment->second
                                        //                      + 1) % dist_from_start.size()] +
                                        //                      start_line.b.distance_to(res))) *
                                        //        weight_island_travel + end_line.a.distance_to(end) * weight_island_travel;
                                        if (new_dist < shortest_dist) {
                                            assert(new_dist > 0);
                                            shortest_dist = new_dist;
                                            shortest_dist_sqr = sqr(shortest_dist);
                                            shortest_cell_dist = 1 + shortest_dist / start_grid.resolution();
                                            if (res == start_line.a) {
                                                best_intersection_start.line_idx = it_start_contour_and_segment->second;
                                                best_intersection_start.point = start_line.a;
                                                best_intersection_start.distance = 0;
                                            } else if (res == start_line.b) {
                                                best_intersection_start.line_idx =
                                                    (it_start_contour_and_segment->second + 1) % contour_start.size();
                                                best_intersection_start.point = start_line.b;
                                                best_intersection_start.distance = 0;
                                            } else {
                                                best_intersection_start.line_idx = it_start_contour_and_segment->second;
                                                best_intersection_start.point = res;
                                                best_intersection_start.distance = start_line.a.distance_to(res);
                                            }
                                            best_intersection_end.line_idx = it_end_contour_and_segment->second;
                                            best_intersection_end.point = end_line.a;
                                            best_intersection_end.distance = 0;
                                            assert(best_intersection_end.line_idx < contour_end.points.size() &&
                                                   contour_end.points[best_intersection_end.line_idx] ==
                                                       best_intersection_end.point);
                                            print_debug_cross(i_id_run, boundary, best_intersection_start,
                                                              best_intersection_end, start, end,
                                                              std::string("endpt_") + std::to_string(new_dist));
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
//#ifdef _DEBUG
#ifdef false
        std::chrono::time_point<std::chrono::high_resolution_clock> time_mid = std::chrono::high_resolution_clock::now();

        std::chrono::time_point<std::chrono::high_resolution_clock> time_end;
        {
            //brute-force version, 
            Intersection best_intersection_start_dbg;
            best_intersection_start_dbg.border_idx = start_island;
            best_intersection_start_dbg.line_idx = -1;
            best_intersection_start_dbg.point = contour_start.points.front();
            best_intersection_start_dbg.distance = 0;
            best_intersection_start_dbg.do_not_remove = true;
            Intersection best_intersection_end_dbg;
            best_intersection_end_dbg.border_idx = end_island;
            best_intersection_end_dbg.line_idx = -1;
            best_intersection_end_dbg.point = contour_end.points.front();
            best_intersection_end_dbg.distance = 0;
            best_intersection_end_dbg.do_not_remove = true;
            char type_dbg = brute_force_jump(best_intersection_start_dbg, best_intersection_end_dbg, Line(start,end), contour_start, contour_end, weight_island_travel);


            time_end = std::chrono::high_resolution_clock::now();
            assert(best_intersection_start.border_idx == best_intersection_start_dbg.border_idx);
            assert(best_intersection_start.line_idx == best_intersection_start_dbg.line_idx);
            assert(best_intersection_start.point.distance_to(best_intersection_start_dbg.point) < 10);
            assert(is_approx(best_intersection_start.distance, best_intersection_start_dbg.distance, 10.f));

            assert(best_intersection_end.border_idx == best_intersection_end_dbg.border_idx);
            assert(best_intersection_end.line_idx == best_intersection_end_dbg.line_idx);
            assert(best_intersection_end.point.distance_to(best_intersection_end_dbg.point) < 10);
            assert(is_approx(best_intersection_end.distance, best_intersection_end_dbg.distance, 10.f));
            long long step1 = std::chrono::duration_cast<std::chrono::microseconds>(time_mid - time_start).count();
            long long stepcreate = std::chrono::duration_cast<std::chrono::microseconds>(time_cell - time_start).count();
            long long step2 = std::chrono::duration_cast<std::chrono::microseconds>(time_end - time_mid).count();
            datapoints.emplace_back(int(step1), int(step2));
            double speedup = 0;
            size_t nb = 0;
            for (auto [st1, st2] : datapoints) {
                speedup += double(st2) / st1;
                nb++;
            }
            if (best_intersection_start.line_idx == size_t(-1)) {
                // can't find any, it's not possible as the strait path exists.
                assert(false);
                return;
            }
        }
#endif
        assert(best_intersection_start.line_idx != size_t(-1) && best_intersection_end.line_idx != size_t(-1));

        // create intersections between start & best_intersection_start
        std::vector<Intersection> intersections_start;
        {
            intersections_start.reserve(boundary.boundaries.size());
            AllIntersectionsVisitor visitor(boundary.grid, intersections_start, Line(start, best_intersection_start.point));
            boundary.grid.visit_cells_intersecting_line(start, best_intersection_start.point, visitor);
            Vec2d dir = (best_intersection_start.point - start).cast<double>();
            for (Intersection &intersection : intersections_start) {
                float dist_from_line_begin = (intersection.point -
                                              boundary.boundaries[intersection.border_idx][intersection.line_idx])
                                                 .cast<float>()
                                                 .norm();
                intersection.distance = boundary.boundaries_params[intersection.border_idx][intersection.line_idx] +
                    dist_from_line_begin;
            }
            std::sort(intersections_start.begin(), intersections_start.end(), [dir](const auto &l, const auto &r) {
                return (r.point - l.point).template cast<double>().dot(dir) > 0.;
            });

            // When the offset is too big, then original travel doesn't have to cross created boundaries.
            // These cases are fixed by calling extend_for_closest_lines.
            intersections_start = extend_for_closest_lines(intersections_start, boundary, start, best_intersection_start.point, search_radius);
#ifdef AVOID_CROSSING_PERIMETERS_DEBUG_OUTPUT
    {
        static int iRun = 0;
        export_travel_to_svg(boundary.boundaries, Line(start, best_intersection_start.point), std::vector<TravelPoint>{}, intersections_start,
                             debug_out_path("AvoidCrossingPerimetersInner-initial-start-%d-%d.svg", 0/*layer.id()*/,
                                            iRun++));
    }
#endif /* AVOID_CROSSING_PERIMETERS_DEBUG_OUTPUT */
        }
        // create intersections between  best_intersection_end & end
        std::vector<Intersection> intersections_end;
        {
            intersections_end.reserve(boundary.boundaries.size());
            AllIntersectionsVisitor visitor(boundary.grid, intersections_end, Line(best_intersection_end.point, end));
            boundary.grid.visit_cells_intersecting_line(best_intersection_end.point, end, visitor);
            Vec2d dir = (end - best_intersection_end.point).cast<double>();
            for (Intersection &intersection : intersections_end) {
                float dist_from_line_begin = (intersection.point -
                                              boundary.boundaries[intersection.border_idx][intersection.line_idx])
                                                 .cast<float>()
                                                 .norm();
                intersection.distance = boundary.boundaries_params[intersection.border_idx][intersection.line_idx] +
                    dist_from_line_begin;
            }
            std::sort(intersections_end.begin(), intersections_end.end(), [dir](const auto &l, const auto &r) {
                return (r.point - l.point).template cast<double>().dot(dir) > 0.;
            });

            // When the offset is too big, then original travel doesn't have to cross created boundaries.
            // These cases are fixed by calling extend_for_closest_lines.
            intersections_end = extend_for_closest_lines(intersections_end, boundary, best_intersection_end.point, end, search_radius);
#ifdef AVOID_CROSSING_PERIMETERS_DEBUG_OUTPUT
    {
        static int iRun = 0;
        export_travel_to_svg(boundary.boundaries, Line(best_intersection_end.point, end), std::vector<TravelPoint>{}, intersections_end,
                             debug_out_path("AvoidCrossingPerimetersInner-initial-end-%d-%d.svg", 0/*layer.id()*/, iRun++));
    }
#endif /* AVOID_CROSSING_PERIMETERS_DEBUG_OUTPUT */
        }
        // continue as normal
        intersections = intersections_start;
        append(intersections, intersections_end);

    }
}

// Called by avoid_perimeters() and by simplify_travel_heuristics().
static size_t avoid_perimeters_inner(      AvoidCrossingPerimeters::Boundary &boundary, // not const because of possible late init of island_to_grid
                                     const Point                             &real_start,
                                     const Point                             &real_end,
                                           coord_t                            extrusion_spacing,
                                     const Layer                             &layer,
                                     std::vector<TravelPoint>                &result_out)
{
    const Polygons           &boundaries = boundary.boundaries;
    const EdgeGrid::Grid     &edge_grid  = boundary.grid;

    Point start = real_start;
    Point end = real_end;

    //ensure that start & end are inside
    if (extrusion_spacing > 0) {
        if (!edge_grid.bbox().contains(start) || !edge_grid.bbox().contains(end) 
            || !find_point_on_boundary(start, boundary, (extrusion_spacing * 3) / 2) 
            || !find_point_on_boundary(end, boundary, (extrusion_spacing * 3) / 2)) {
            //can't find one, but maybe we can find something better than nothing.
            coordf_t dist = start.distance_to(end);
            EdgeGrid::Grid::ClosestPointResult pt_closest_start = boundary.grid.closest_point_signed_distance(start, dist/2);
            EdgeGrid::Grid::ClosestPointResult pt_closest_end = boundary.grid.closest_point_signed_distance(end, dist/2);
            // Is it useful enough?
            bool find_better = false;
            if (pt_closest_start.distance + pt_closest_end.distance < dist) {
                const EdgeGrid::Contour& pts_start = boundary.grid.contours()[pt_closest_start.contour_idx];
                Point new_start = pts_start.segment_start(pt_closest_start.start_point_idx).interpolate(pt_closest_start.t, pts_start.segment_end(pt_closest_start.start_point_idx));
                const EdgeGrid::Contour& pts_end = boundary.grid.contours()[pt_closest_end.contour_idx];
                Point new_end = pts_end.segment_start(pt_closest_end.start_point_idx).interpolate(pt_closest_end.t, pts_end.segment_end(pt_closest_end.start_point_idx));
                // check if travel top
                AnyIntersectionsVisitor visitor(boundary.to_avoid_grid, Polyline{start, new_start, new_end, end});
                if(boundary.to_avoid_grid.bbox().contains(start) && boundary.to_avoid_grid.bbox().contains(new_start))
                    boundary.to_avoid_grid.visit_cells_intersecting_line(start, new_start, visitor); //TODO is this the right way to use it?
                if(boundary.to_avoid_grid.bbox().contains(new_start) && boundary.to_avoid_grid.bbox().contains(new_end))
                    boundary.to_avoid_grid.visit_cells_intersecting_line(new_start, new_end, visitor);
                if(boundary.to_avoid_grid.bbox().contains(new_end) && boundary.to_avoid_grid.bbox().contains(end))
                    boundary.to_avoid_grid.visit_cells_intersecting_line(new_end, end, visitor);
                if (visitor.has_intersection) {
                    // go over avoid area, check if it's more or less than the dumb travel
                    Polylines lines = diff_pl({Polyline{start, end}}, boundary.to_avoid);
                    coordf_t old_dist_over_top = 0;
                    for(const Polyline &pl : lines)
                        old_dist_over_top += pl.length();
                    //check if it's not obviously worse
                    if (old_dist_over_top > 0) {
                        lines = diff_pl({Polyline{start, new_start, new_end, end}}, boundary.to_avoid);
                        coordf_t new_dist_over_top = 0;
                        for (const Polyline &pl : lines) new_dist_over_top += pl.length();
                        // check if it's really better
                        if (new_dist_over_top < old_dist_over_top / 2) {
                            // really better!
                            find_better = true;
                            start = new_start;
                            end = new_end;
                        }
                    }
                } else {
                    find_better = true;
                    start = new_start;
                    end = new_end;
                }
            }

            // travel strait to the nearest point to the same boudary as the other one, if it reduce the strait travel
            auto saerch_best_pt = [&](const EdgeGrid::Grid::ClosestPointResult &good_one,
                                      Point &bad_one)->bool {
                coordf_t min_dist_sqr = dist*dist;
                Point best_pt = bad_one;
                const EdgeGrid::Contour& good_boudary = boundary.grid.contours()[good_one.contour_idx];
                Point last_pt = good_boudary.back();
                Point nearest;
                Point current;
                for (auto it = good_boudary.begin(); it != good_boudary.end(); ++it) {
                    current = *it;
                    coordf_t dist_sqr = Line(last_pt, current).distance_to_squared(bad_one, &nearest);
                    if (dist_sqr < min_dist_sqr) {
                        min_dist_sqr = dist_sqr;
                        best_pt = nearest;
                    }
                }
                bad_one = best_pt;
                return  min_dist_sqr < dist*dist;
            };
            if (!find_better && pt_closest_start.contour_idx == size_t(-1) && pt_closest_end.contour_idx != size_t(-1)) {
                find_better = saerch_best_pt(pt_closest_end, start);
            }
            if (!find_better && pt_closest_end.contour_idx == size_t(-1) && pt_closest_start.contour_idx != size_t(-1)) {
                find_better = saerch_best_pt(pt_closest_start, end);
            }
            if (!find_better) {
                BOOST_LOG_TRIVIAL(debug) << "Fail to find a point in the contour for avoid_perimeter.";
                result_out = {{start, -1}, {end, -1}};
                return 0;
            }
        }
    }
    // Find all intersections between boundaries and the line segment, sort them along the line segment.
    std::vector<Intersection> intersections;
    {
        intersections.reserve(boundaries.size());
        AllIntersectionsVisitor visitor(edge_grid, intersections, Line(start, end));
        edge_grid.visit_cells_intersecting_line(start, end, visitor);
        Vec2d dir = (end - start).cast<double>();
        for (Intersection &intersection : intersections) {
            float dist_from_line_begin = (intersection.point - boundary.boundaries[intersection.border_idx][intersection.line_idx]).cast<float>().norm();
            intersection.distance = boundary.boundaries_params[intersection.border_idx][intersection.line_idx] + dist_from_line_begin;
        }
        std::sort(intersections.begin(), intersections.end(), [dir](const auto &l, const auto &r) { return (r.point - l.point).template cast<double>().dot(dir) > 0.; });

        // Search radius should always be at least equals to the value of offset used for computing boundaries.
        const coord_t search_radius = 2 * get_perimeter_spacing(layer);
        // When the offset is too big, then original travel doesn't have to cross created boundaries.
        // These cases are fixed by calling extend_for_closest_lines.
        intersections             = extend_for_closest_lines(intersections, boundary, start, end, search_radius);
    }

    std::vector<TravelPoint> result;
    result.push_back({start, -1});

#if 0
    auto crossing_boundary_from_inside = [&boundary](const Point &start, const Intersection &intersection) {
        const Polygon &poly             = boundary.boundaries[intersection.border_idx];
        Vec2d          poly_line        = Line(poly[intersection.line_idx], poly[(intersection.line_idx + 1) % poly.size()]).normal().cast<double>();
        Vec2d          intersection_vec = (intersection.point - start).cast<double>();
        return poly_line.normalized().dot(intersection_vec.normalized()) >= 0;
    };
#endif

    // modify intersections if jumping between island to choose the best points to jump
    // note: boundary.islands.empty() => use_external_mp
    if (intersections.size() > 1 && !boundary.islands.empty()) {
        bool has_avoid_travel_island = false;
        double avoid_travel_island_weight = 0;
        // avoid_travel_island only works for full layers, so find at least one.
        for (const LayerRegion &layer_region : layer.regions()) {
            if (layer_region.region().config().avoid_travel_island.value) {
                has_avoid_travel_island = true;
                avoid_travel_island_weight = layer_region.region().config().avoid_travel_island_weight.value;
                break;
            }
        }
        if (has_avoid_travel_island) {
            jump_between_island(boundary, start, end, intersections, 2 * get_perimeter_spacing(layer),
                                avoid_travel_island_weight);
        }
    }

    size_t iprocess=0;
    for (auto it_first = intersections.begin(); it_first != intersections.end(); ++it_first) {
        // The entry point to the boundary polygon
        const Intersection &intersection_first = *it_first;
//        if(!crossing_boundary_from_inside(start, intersection_first))
//            continue;
        // Skip the it_first from the search for the farthest exit point from the boundary polygon
        auto it_last_item = std::make_reverse_iterator(it_first) - 1;
        // Search for the farthest intersection different from it_first but with the same border_idx
        auto it_second_r  = std::find_if(intersections.rbegin(), it_last_item, [&intersection_first](const Intersection &intersection) {
            return intersection_first.border_idx == intersection.border_idx;
        });

        // Append the first intersection into the path
        size_t left_idx  = intersection_first.line_idx;
        size_t right_idx = intersection_first.line_idx + 1 == boundaries[intersection_first.border_idx].points.size() ? 0 : intersection_first.line_idx + 1;
        // Offset of the polygon's point using get_middle_point_offset is used to simplify the calculation of intersection between the
        // boundary and the travel. The appended point is translated in the direction of inward normal. This translation ensures that the
        // appended point will be inside the polygon and not on the polygon border.
        result.push_back({get_middle_point_offset(boundaries[intersection_first.border_idx], left_idx, right_idx, intersection_first.point, coord_t(SCALED_EPSILON)), int(intersection_first.border_idx), intersection_first.do_not_remove});

        // Check if intersection line also exit the boundary polygon
        if (it_second_r != it_last_item) {
            // Transform reverse iterator to forward
            auto it_second = it_second_r.base() - 1;
            // The exit point from the boundary polygon
            const Intersection &intersection_second = *it_second;
            Direction           shortest_direction  = get_shortest_direction(boundary, intersection_first, intersection_second,
                                                                             boundary.boundaries_params[intersection_first.border_idx].back());
            // Append the path around the border into the path
            if (shortest_direction == Direction::Forward)
                for (int line_idx = int(intersection_first.line_idx); line_idx != int(intersection_second.line_idx);
                    line_idx      = line_idx + 1 < int(boundaries[intersection_first.border_idx].size()) ? line_idx + 1 : 0)
                    result.push_back({get_polygon_vertex_offset(boundaries[intersection_first.border_idx],
                                                                (line_idx + 1 == int(boundaries[intersection_first.border_idx].points.size())) ? 0 : (line_idx + 1), coord_t(SCALED_EPSILON)), int(intersection_first.border_idx)});
            else
                for (int line_idx = int(intersection_first.line_idx); line_idx != int(intersection_second.line_idx);
                    line_idx      = line_idx - 1 >= 0 ? line_idx - 1 : int(boundaries[intersection_first.border_idx].size()) - 1)
                    result.push_back({get_polygon_vertex_offset(boundaries[intersection_second.border_idx], line_idx + 0, coord_t(SCALED_EPSILON)), int(intersection_first.border_idx)});

            // Append the farthest intersection into the path
            left_idx  = intersection_second.line_idx;
            right_idx = (intersection_second.line_idx >= (boundaries[intersection_second.border_idx].points.size() - 1)) ? 0 : (intersection_second.line_idx + 1);
            result.push_back({get_middle_point_offset(boundaries[intersection_second.border_idx], left_idx, right_idx, intersection_second.point, coord_t(SCALED_EPSILON)), int(intersection_second.border_idx), intersection_second.do_not_remove});
            // Skip intersections in between
            it_first = it_second;
        }
    }

    result.push_back({end, -1});

#ifdef AVOID_CROSSING_PERIMETERS_DEBUG_OUTPUT
    {
        static int iRun = 0;
        export_travel_to_svg(boundaries, Line(start, end), result, intersections, debug_out_path("AvoidCrossingPerimetersInner-initial-%d-%d.svg", layer.id(), iRun++));
    }
#endif /* AVOID_CROSSING_PERIMETERS_DEBUG_OUTPUT */
    
    if (! intersections.empty())
        result = simplify_travel(boundary, result);

#ifdef AVOID_CROSSING_PERIMETERS_DEBUG_OUTPUT
    {
        static int iRun = 0;
        export_travel_to_svg(boundaries, Line(start, end), result, intersections,
                             debug_out_path("AvoidCrossingPerimetersInner-final-%d-%d.svg", layer.id(), iRun++));
    }
#endif /* AVOID_CROSSING_PERIMETERS_DEBUG_OUTPUT */

    //if travel distance lower than epsilon, make it one centre point between .
    if (start != real_start && end != real_end && result.size() == 2 &&
        result.front().point.coincides_with_epsilon(result.back().point)) {
        result.front().point = Line(result.front().point, result.back().point).midpoint();
        result.pop_back();
    }

    if(start != real_start)
        result_out.push_back({ real_start, -1 });
    append(result_out, std::move(result));
    while (result_out.size() > 1 && result_out.front().point.coincides_with_epsilon(result_out[1].point)) {
        //ensure real_start is more than epsilon than start
        result_out.erase(result_out.begin() + 1);
    }
    if (end != real_end)
        result_out.push_back({ real_end, -1 });
    while (result_out.size() > 1 &&
           result_out.back().point.coincides_with_epsilon(result_out[result_out.size() - 2].point)) {
        //ensure real_end is more than epsilon than end
        result_out.erase(result_out.begin() + result_out.size() - 2);
    }

    assert(result_out.size() > 1);
    for (size_t i = 1; i < result_out.size(); i++)
        assert(!result_out[i - 1].point.coincides_with_epsilon(result_out[i].point));

    return intersections.size();
}

// Called by AvoidCrossingPerimeters::travel_to()
static size_t avoid_perimeters(      AvoidCrossingPerimeters::Boundary &boundary,  // not const because of possible late init of island_to_grid
                               const Point                             &start,
                               const Point                             &end,
                                   coord_t                              spacing,
                               const Layer                             &layer,
                               Polyline                                &result_out)
{
    // Travel line is completely or partially inside the bounding box.
    std::vector<TravelPoint> path;
    size_t num_intersections = avoid_perimeters_inner(boundary, start, end, spacing, layer, path);
    result_out = to_polyline(path);

#ifdef AVOID_CROSSING_PERIMETERS_DEBUG_OUTPUT
    {
        static int iRun = 0;
        export_travel_to_svg(boundary.boundaries, Line(start, end), path, {}, debug_out_path("AvoidCrossingPerimeters-final-%d-%d.svg", layer.id(), iRun ++));
    }
#endif /* AVOID_CROSSING_PERIMETERS_DEBUG_OUTPUT */

    return num_intersections;
}

// Adds points around all vertices so that the offset affects only small sections around these vertices.
static void resample_polygon(Polygon &polygon, double dist_from_vertex, double max_allowed_distance)
{
    Points resampled_poly;
    resampled_poly.reserve(3 * polygon.size());
    for (size_t pt_idx = 0; pt_idx < polygon.size(); ++pt_idx) {
        resampled_poly.emplace_back(polygon[pt_idx]);

        const Point &p1          = polygon[pt_idx];
        const Point &p2          = polygon[next_idx_modulo(pt_idx, polygon.size())];
        const Vec2d  line_vec    = (p2 - p1).cast<double>();
        double       line_length = line_vec.norm();
        const Vector vertex_offset_vec = (line_vec.normalized() * dist_from_vertex).cast<coord_t>();
        if (line_length > 2 * dist_from_vertex && vertex_offset_vec != Vector(0, 0)) {
            resampled_poly.emplace_back(p1 + vertex_offset_vec);

            const Vec2d  new_vertex_vec        = (p2 - p1 - 2 * vertex_offset_vec).cast<double>();
            const double new_vertex_vec_length = new_vertex_vec.norm();
            if (new_vertex_vec_length > max_allowed_distance) {
                const Vec2d &prev_point  = resampled_poly.back().cast<double>();
                const size_t parts_count = size_t(ceil(new_vertex_vec_length / max_allowed_distance));
                for (size_t part_idx = 1; part_idx < parts_count; ++part_idx) {
                    const double part_param = double(part_idx) / double(parts_count);
                    const Vec2d  new_point  = prev_point + new_vertex_vec * part_param;
                    resampled_poly.emplace_back(new_point.cast<coord_t>());
                }
            }

            resampled_poly.emplace_back(p2 - vertex_offset_vec);
        }
    }
    polygon.points = std::move(resampled_poly);
}

static void resample_expolygon(ExPolygon &ex_polygon, double dist_from_vertex, double max_allowed_distance)
{
    resample_polygon(ex_polygon.contour, dist_from_vertex, max_allowed_distance);
    for (Polygon &polygon : ex_polygon.holes)
        resample_polygon(polygon, dist_from_vertex, max_allowed_distance);
}

static void resample_expolygons(ExPolygons &ex_polygons, double dist_from_vertex, double max_allowed_distance)
{
    for (ExPolygon &ex_poly : ex_polygons)
        resample_expolygon(ex_poly, dist_from_vertex, max_allowed_distance);
}

static void precompute_polygon_distances(const Polygon &polygon, std::vector<float> &polygon_distances_out)
{
    polygon_distances_out.assign(polygon.size() + 1, 0.f);
    for (size_t point_idx = 1; point_idx < polygon.size(); ++point_idx)
        polygon_distances_out[point_idx] = polygon_distances_out[point_idx - 1] + float((polygon[point_idx] - polygon[point_idx - 1]).cast<double>().norm());
    polygon_distances_out.back() = polygon_distances_out[polygon.size() - 1] + float((polygon.points.back() - polygon.points.front()).cast<double>().norm());
}

static void precompute_expolygon_distances(const ExPolygon &ex_polygon, std::vector<std::vector<float>> &expolygon_distances_out)
{
    expolygon_distances_out.assign(ex_polygon.holes.size() + 1, std::vector<float>());
    precompute_polygon_distances(ex_polygon.contour, expolygon_distances_out.front());
    for (size_t hole_idx = 0; hole_idx < ex_polygon.holes.size(); ++hole_idx)
        precompute_polygon_distances(ex_polygon.holes[hole_idx], expolygon_distances_out[hole_idx + 1]);
}

// It is highly based on the function contour_distance2 from the ElephantFootCompensation.cpp
static std::vector<float> contour_distance(const EdgeGrid::Grid     &grid,
                                    const std::vector<float> &poly_distances,
                                    const size_t              contour_idx,
                                    const Polygon            &polygon,
                                    double                    compensation,
                                    double                    search_radius)
{
    assert(! polygon.empty());
    assert(polygon.size() >= 2);

    std::vector<float> out;

    if (polygon.size() > 2)
    {
        struct Visitor {
            Visitor(const EdgeGrid::Grid &grid, const size_t contour_idx, const std::vector<float> &polygon_distances, double dist_same_contour_accept, double dist_same_contour_reject) :
                grid(grid), idx_contour(contour_idx), contour(grid.contours()[contour_idx]), boundary_parameters(polygon_distances), dist_same_contour_accept(dist_same_contour_accept), dist_same_contour_reject(dist_same_contour_reject) {}

            void init(const Points &contour, const Point &apoint)
            {
                this->idx_point  = &apoint - contour.data();
                this->point      = apoint;
                this->found      = false;
                this->dir_inside = this->dir_inside_at_point(contour, this->idx_point);
                this->distance   = std::numeric_limits<double>::max();
            }

            bool operator()(coord_t iy, coord_t ix)
            {
                // Called with a row and colum of the grid cell, which is intersected by a line.
                auto cell_data_range = this->grid.cell_data_range(iy, ix);
                for (auto it_contour_and_segment = cell_data_range.first; it_contour_and_segment != cell_data_range.second;
                     ++it_contour_and_segment) {
                    // End points of the line segment and their vector.
                    std::pair<const Point &, const Point &> segment = this->grid.segment(*it_contour_and_segment);
                    const Vec2d  v        = (segment.second - segment.first).cast<double>();
                    const Vec2d  va       = (this->point - segment.first).cast<double>();
                    const double l2       = v.squaredNorm(); // avoid a sqrt
                    const double t        = (l2 == 0.0) ? 0. : std::clamp(va.dot(v) / l2, 0., 1.);
                    // Closest point from this->point to the segment.
                    const Vec2d  foot     = segment.first.cast<double>() + t * v;
                    const Vec2d  bisector = foot - this->point.cast<double>();
                    const double dist     = bisector.norm();

                    if ((!this->found || dist < this->distance) && this->dir_inside.dot(bisector) > 0) {
                        bool accept = true;
                        if (it_contour_and_segment->first == idx_contour) {
                            // Complex case: The closest segment originates from the same contour as the starting point.
                            // Reject the closest point if its distance along the contour is reasonable compared to the current contour bisector
                            // (this->pt, foot).
                            const EdgeGrid::Contour &contour   = grid.contours()[it_contour_and_segment->first];
                            double                   param_lo  = boundary_parameters[this->idx_point];
                            double                   param_hi  = t * sqrt(l2);
                            double                   param_end = boundary_parameters.back();
                            const size_t ipt = it_contour_and_segment->second;
                            if (contour.begin() + ipt + 1 < contour.end())
                                param_hi += boundary_parameters[ipt];
                            if (param_lo > param_hi)
                                std::swap(param_lo, param_hi);
                            assert(param_lo > -SCALED_EPSILON && param_lo <= param_end + SCALED_EPSILON);
                            assert(param_hi > -SCALED_EPSILON && param_hi <= param_end + SCALED_EPSILON);
                            double dist_along_contour = std::min(param_hi - param_lo, param_lo + param_end - param_hi);
                            if (dist_along_contour < dist_same_contour_accept)
                                accept = false;
                            else if (dist < dist_same_contour_reject + SCALED_EPSILON) {
                                // this->point is close to foot. This point will only be accepted if the path along the contour is significantly
                                // longer than the bisector. That is, the path shall not bulge away from the bisector too much.
                                // Bulge is estimated by 0.6 of the circle circumference drawn around the bisector.
                                // Test whether the contour is convex or concave.
                                bool inside = (t == 0.) ? this->inside_corner(contour, ipt, this->point) :
                                              (t == 1.) ? this->inside_corner(contour, contour.segment_idx_next(ipt), this->point) :
                                                          this->left_of_segment(contour, ipt, this->point);
                                accept      = inside && dist_along_contour > 0.6 * M_PI * dist;
                            }
                        }
                        if (accept && (!this->found || dist < this->distance)) {
                            // Simple case: Just measure the shortest distance.
                            this->distance = dist;
                            this->found    = true;
                        }
                    }
                }
                // Continue traversing the grid.
                return true;
            }

            const EdgeGrid::Grid 			   &grid;
            const size_t 		  				idx_contour;
            const EdgeGrid::Contour            &contour;

            const std::vector<float>           &boundary_parameters;
            const double                        dist_same_contour_accept;
            const double 						dist_same_contour_reject;

            size_t 								idx_point;
            Point			      				point;
            // Direction inside the contour from idx_point, not normalized.
            Vec2d								dir_inside;
            bool 								found;
            double 								distance;

        private:
            static Vec2d dir_inside_at_point(const Points &contour, size_t i)
            {
                size_t iprev = prev_idx_modulo(i, contour);
                size_t inext = next_idx_modulo(i, contour);
                Vec2d  v1    = (contour[i] - contour[iprev]).cast<double>();
                Vec2d  v2    = (contour[inext] - contour[i]).cast<double>();
                return Vec2d(-v1.y() - v2.y(), v1.x() + v2.x());
            }

            static bool inside_corner(const EdgeGrid::Contour &contour, size_t i, const Point &pt_oposite)
            {
                const Vec2d pt         = pt_oposite.cast<double>();
                const Point &pt_prev   = contour.segment_prev(i);
                const Point &pt_this   = contour.segment_start(i);
                const Point &pt_next   = contour.segment_end(i);
                Vec2d       v1         = (pt_this - pt_prev).cast<double>();
                Vec2d       v2         = (pt_next - pt_this).cast<double>();
                bool        left_of_v1 = cross2(v1, pt - pt_prev.cast<double>()) > 0.;
                bool        left_of_v2 = cross2(v2, pt - pt_this.cast<double>()) > 0.;
                return cross2(v1, v2) > 0 ? left_of_v1 && left_of_v2 : // convex corner
                                            left_of_v1 || left_of_v2;                   // concave corner
            }

            static bool left_of_segment(const EdgeGrid::Contour &contour, size_t i, const Point &pt_oposite)
            {
                const Vec2d  pt      = pt_oposite.cast<double>();
                const Point &pt_this = contour.segment_start(i);
                const Point &pt_next = contour.segment_end(i);
                Vec2d        v       = (pt_next - pt_this).cast<double>();
                return cross2(v, pt - pt_this.cast<double>()) > 0.;
            }
        } visitor(grid, contour_idx, poly_distances, 0.5 * compensation * M_PI, search_radius);

        out.reserve(polygon.size());
        Point radius_vector(search_radius, search_radius);
        for (const Point &pt : polygon.points) {
            visitor.init(polygon.points, pt);
            grid.visit_cells_intersecting_box(BoundingBox(pt - radius_vector, pt + radius_vector), visitor);
            out.emplace_back(float(visitor.found ? std::min(visitor.distance, search_radius) : search_radius));
        }
    }

    return out;
}

// Polygon offset which ensures that if a polygon breaks up into several separate parts, the original polygon will be used in these places.
// ExPolygons are handled one by one so returned ExPolygons could intersect.
static std::vector<std::pair<ExPolygon, ExPolygon>> inner_offset(const ExPolygons &ex_polygons, double offset)
{
    const std::vector<double> min_contour_width_values = {offset / 2., offset, 2. * offset + SCALED_EPSILON};
    std::vector<std::pair<ExPolygon, ExPolygon>> ex_poly_result;
    for (const ExPolygon &expo : ex_polygons) {
        ex_poly_result.emplace_back(expo, expo);
        resample_expolygon(ex_poly_result.back().second, offset / 2, scale_d(0.5));
        ExPolygons ensure_resample_dont_grow = diff_ex(ex_poly_result.back().second, ex_poly_result.back().first);
        if (ensure_resample_dont_grow.size() != 1) {
            // fail, use the unsimplified one.
            ex_poly_result.back().second = ex_poly_result.back().first;
        } else {
            ex_poly_result.back().second = ensure_resample_dont_grow.front();
        }
    }

    for (std::pair<ExPolygon,ExPolygon> &old_2_new : ex_poly_result) {
        ExPolygon &ex_poly = old_2_new.second;
        BoundingBox bbox(get_extents(ex_poly));
        bbox.offset(SCALED_EPSILON);

        // Filter out expolygons smaller than 0.1mm^2
        if (Vec2d bbox_size = bbox.size().cast<double>(); bbox_size.x() * bbox_size.y() < Slic3r::sqr(scale_d(0.1f)))
            continue;

        for (const double &min_contour_width : min_contour_width_values) {
            const size_t min_contour_width_idx = &min_contour_width - &min_contour_width_values.front();
            const double search_radius         = 2. * (offset + min_contour_width);

            EdgeGrid::Grid grid;
            grid.set_bbox(bbox);
            grid.create(ex_poly, coord_t(0.7 * search_radius));

            std::vector<std::vector<float>> ex_poly_distances;
            precompute_expolygon_distances(ex_poly, ex_poly_distances);

            std::vector<std::vector<float>> offsets;
            offsets.reserve(ex_poly.holes.size() + 1);
            for (size_t idx_contour = 0; idx_contour <= ex_poly.holes.size(); ++idx_contour) {
                const Polygon &poly = (idx_contour == 0) ? ex_poly.contour : ex_poly.holes[idx_contour - 1];
                assert(poly.is_counter_clockwise() == (idx_contour == 0));
                std::vector<float> distances = contour_distance(grid, ex_poly_distances[idx_contour], idx_contour, poly, offset, search_radius);
                for (float &distance : distances) {
                    if (distance < min_contour_width)
                        distance = 0.f;
                    else if (distance > min_contour_width + 2. * offset)
                        distance = -float(offset);
                    else
                        distance = -(distance - float(min_contour_width)) / 2.f;
                }
                offsets.emplace_back(distances);
            }

            ExPolygons offset_ex_poly = variable_offset_inner_ex(ex_poly, offsets);
            // If variable_offset_inner_ex produces empty result, then original ex_polygon is used
            if (offset_ex_poly.size() == 1 && offset_ex_poly.front().holes.size() == ex_poly.holes.size()) {
                ex_poly = std::move(offset_ex_poly.front());
                break;
            } else if ((min_contour_width_idx + 1) < min_contour_width_values.size()) {
                continue; // Try the next round with bigger min_contour_width.
            } else if (offset_ex_poly.size() == 1) {
                ex_poly = std::move(offset_ex_poly.front());
                break;
            } else if (offset_ex_poly.size() > 1) {
                // fix_after_inner_offset called inside variable_offset_inner_ex sometimes produces
                // tiny artefacts polygons, so these artefacts are removed.
                double max_area     = offset_ex_poly.front().area();
                size_t max_area_idx = 0;
                for (size_t poly_idx = 1; poly_idx < offset_ex_poly.size(); ++poly_idx) {
                    double area = offset_ex_poly[poly_idx].area();
                    if (max_area < area) {
                        max_area     = area;
                        max_area_idx = poly_idx;
                    }
                }
                ex_poly = std::move(offset_ex_poly[max_area_idx]);
                break;
            }
        }
    }
    return ex_poly_result;
}

//#define INCLUDE_SUPPORTS_IN_BOUNDARY

// called by AvoidCrossingPerimeters::travel_to()
static ExPolygons get_boundary(const Layer &layer, uint16_t extruder_id, std::vector<std::pair<ExPolygon, ExPolygon>> &slice_2_boundary, ExPolygons &to_avoid)
{
    const coord_t perimeter_spacing = get_perimeter_spacing(layer);
    const bool support_layer = layer.get_property<LayerSupportProperty>() != nullptr;
    ExPolygons  perimeter_boundary;
    ExPolygons  boundary;

    //get perimeter_boundary from islands
    for (const LayerSliceIsland &layer_island : layer.islands()) {
        append(perimeter_boundary, layer_island.get_perimeter_slices());
    }
    perimeter_boundary = union_ex(perimeter_boundary);

    auto old_2_new_expolygons = inner_offset(perimeter_boundary, coordf_t(1.5 * perimeter_spacing));
    for (const std::pair<ExPolygon, ExPolygon> &old_2_new_expoly : old_2_new_expolygons) {
        // note: sometimes, old_2_new_expoly.first.contains(old_2_new_expoly.second.contour.split_at_index(0)) can fail when a new point is created on a circle, but diff_ex doesn't.
        assert(old_2_new_expoly.first.contains(old_2_new_expoly.second.contour.split_at_index(0)) ||
               old_2_new_expoly.first == old_2_new_expoly.second ||
               diff_ex(old_2_new_expoly.second, old_2_new_expoly.first).empty());
        boundary.push_back(old_2_new_expoly.second);
        slice_2_boundary.push_back(std::move(old_2_new_expoly));
    }
    boundary = union_ex(boundary);
    if(support_layer) {
#ifdef INCLUDE_SUPPORTS_IN_BOUNDARY
        append(boundary, inner_offset(support_layer->support_islands.expolygons, 1.5 * perimeter_spacing));
#endif
       assert(layer.object()->get_first_layer_below_printz(layer.unscaled_print_z(), EPSILON*4) == layer.object()->get_first_layer_below_printz(layer.scaled_print_z() + SCALED_EPSILON));
        auto *layer_below = layer.object()->get_first_layer_below_printz(layer.scaled_print_z() + SCALED_EPSILON);
        if (layer_below) { // why?
            perimeter_boundary.clear();
            //get perimeter_boundary from islands
            for (const LayerSliceIsland &layer_island : layer_below->islands()) {
                append(perimeter_boundary, layer_island.get_perimeter_slices());
            }
            perimeter_boundary = union_ex(perimeter_boundary);
            auto old_2_new_expolygons_supp = inner_offset(perimeter_boundary, coordf_t(1.5 * perimeter_spacing));
            for (std::pair<ExPolygon, ExPolygon> &old_2_new_supp : old_2_new_expolygons_supp) {
                boundary.push_back(old_2_new_supp.second);
                slice_2_boundary.push_back(std::move(old_2_new_supp));
            }
        }
        // After calling inner_offset it is necessary to call union_ex because of the possibility of intersection ExPolygons
        boundary = union_ex(boundary);
    }
    // Collect all top layers that will not be crossed.
    size_t      polygons_count    = 0;
    for (const LayerRegion &layer_region : layer.regions())
        if(layer_region.region().config().avoid_crossing_top)
            for (const Surface &surface : layer_region.fill_surfaces())
                if (surface.has_pos_top()) ++polygons_count;

    if (polygons_count > 0) {
        ExPolygons top_layer_polygons;
        top_layer_polygons.reserve(polygons_count);
        for (const LayerRegion &layer_region : layer.regions()) {
            if (layer_region.region().config().avoid_crossing_top) {
                for (const Surface &surface : layer_region.fill_surfaces()) {
                    if (surface.has_pos_top()) {
                        ExPolygons offseted_top = offset_ex(surface.expolygon, coordf_t(-perimeter_spacing / 2));
                        // still get a perimeter width to travel (unless no perimeters or post-process ironing)
                        if (layer_region.region().config().perimeters > 0 && !layer_region.region().config().ironing) {
                            append(boundary,
                                   diff_ex(offset_ex(surface.expolygon, coordf_t(perimeter_spacing / 4)), offseted_top));
                        }
                        append(top_layer_polygons, offseted_top);
                    }
                }
            }
        }
        
        top_layer_polygons = union_ex(top_layer_polygons);
        boundary = union_ex(boundary);
        append(to_avoid, top_layer_polygons);
        boundary = diff_ex(boundary, top_layer_polygons);
    }

    // multiple extruders?
    // clip by region
    LayerRegionCRefs all_regions = layer.regions();
    bool multiple_extruders = false;
    for (const LayerRegion &lregion : all_regions) {
            multiple_extruders = multiple_extruders ||
                lregion.region().config().perimeter_extruder.value !=
                    extruder_id + 1;
            multiple_extruders = multiple_extruders ||
                lregion.region().config().infill_extruder.value !=
                    extruder_id + 1;
            multiple_extruders = multiple_extruders ||
                lregion.region().config().solid_infill_extruder.value !=
                    extruder_id + 1;
            if (multiple_extruders) {
                break;
            }
    }
    if (multiple_extruders) {
        ExPolygons clip;
        for (const LayerRegion &lregion : all_regions) {
            bool same_extruders = lregion.region().config().perimeter_extruder.value ==
                    lregion.region().config().infill_extruder.value &&
                lregion.region().config().infill_extruder.value ==
                    lregion.region().config().solid_infill_extruder.value;

            if (same_extruders) {
                if (lregion.region().config().perimeter_extruder.value ==
                    extruder_id + 1) {
                    clip = union_ex(clip, lregion.get_raw_slices());
                }
            } else {
                if (lregion.region().config().infill_extruder.value !=
                    lregion.region().config().solid_infill_extruder.value) {
                    BOOST_LOG_TRIVIAL(warning) << "";
                }
                if (lregion.region().config().perimeter_extruder.value ==
                    extruder_id + 1) {
                    assert(lregion.region().config().infill_extruder.value !=
                            extruder_id + 1);
                    clip = union_ex(clip, diff_ex(lregion.get_raw_slices(), lregion.fill_expolygons()));
                } else {
                    assert(lregion.region().config().infill_extruder.value ==
                            extruder_id + 1);
                    clip = union_ex(clip, lregion.fill_expolygons());
                }
            }
        }
        boundary = intersection_ex(boundary, offset_ex(clip, SCALED_EPSILON * 10 /*safety offset*/));
    }

    return boundary;
}

// called by AvoidCrossingPerimeters::travel_to()
static Polygons get_boundary_external(const Layer &layer)
{
    const coord_t perimeter_spacing = get_perimeter_spacing_external(layer);
    const coord_t perimeter_offset  = perimeter_spacing / 2;
    const bool support_layer = layer.get_property<LayerSupportProperty>() != nullptr;
    Polygons    boundary;
#ifdef INCLUDE_SUPPORTS_IN_BOUNDARY
    ExPolygons  supports_boundary;
#endif
    // Collect all holes for all printed objects and their instances, which will be printed at the same time as passed "layer".
    for (const PrintObject &object : layer.object()->print()->objects()) {
        Polygons   holes_per_obj;
#ifdef INCLUDE_SUPPORTS_IN_BOUNDARY
        ExPolygons supports_per_obj;
#endif
        assert(object.get_layer_at_printz(layer.unscaled_print_z(), EPSILON * 4) == object.get_layer_at_printz(layer.scaled_print_z()));
        if (const Layer *l = object.get_layer_at_printz(layer.scaled_print_z()); l)
            for (const ExPolygon &island : l->lslices())
                append(holes_per_obj, island.holes);
        if (support_layer) {
            assert(layer.object()->get_first_layer_below_printz(layer.unscaled_print_z(), EPSILON*4) == layer.object()->get_first_layer_below_printz(layer.scaled_print_z() + SCALED_EPSILON));
            auto *layer_below = object.get_first_layer_below_printz(layer.scaled_print_z() + SCALED_EPSILON);
            if (layer_below)
                for (const ExPolygon &island : layer_below->lslices())
                    append(holes_per_obj, island.holes);
#ifdef INCLUDE_SUPPORTS_IN_BOUNDARY
            append(supports_per_obj, support_layer->support_islands.expolygons);
#endif
        }

        // After 7ff76d07684858fd937ef2f5d863f105a10f798e, when expand is called on CW polygons (holes), they are shrunk
        // instead of expanded because union that makes CCW from CW isn't called anymore. So let's make it CCW.
        polygons_reverse(holes_per_obj);

        for (const PrintInstance &instance : object.instances()) {
            size_t boundary_idx = boundary.size();
            append(boundary, holes_per_obj);
            for (; boundary_idx < boundary.size(); ++boundary_idx)
                boundary[boundary_idx].translate(instance.shift);
#ifdef INCLUDE_SUPPORTS_IN_BOUNDARY
            size_t support_idx = supports_boundary.size();
            append(supports_boundary, supports_per_obj);
            for (; support_idx < supports_boundary.size(); ++support_idx)
                supports_boundary[support_idx].translate(instance.shift);
#endif
        }
    }

    // Used offset_ex for cases when another object will be in the hole of another polygon
    boundary = expand(boundary, coordf_t(perimeter_offset));
    // Reverse all polygons for making normals point from the polygon out.
    for (Polygon &poly : boundary)
        poly.reverse();
#ifdef INCLUDE_SUPPORTS_IN_BOUNDARY
    append(boundary, to_polygons(inner_offset(supports_boundary, coordf_t(perimeter_offset))));
#endif
    return boundary;
}

static void init_boundary_distances(AvoidCrossingPerimeters::Boundary *boundary)
{
    boundary->boundaries_params.assign(boundary->boundaries.size(), std::vector<float>());
    for (size_t poly_idx = 0; poly_idx < boundary->boundaries.size(); ++poly_idx)
        precompute_polygon_distances(boundary->boundaries[poly_idx], boundary->boundaries_params[poly_idx]);
}

static void init_boundary(AvoidCrossingPerimeters::Boundary *boundary, Polygons &&boundary_polygons, coord_t extra_available_space)
{
    boundary->clear();
    boundary->boundaries = std::move(boundary_polygons);
    BoundingBox bbox(get_extents(boundary->boundaries));
    bbox.offset(SCALED_EPSILON + extra_available_space);
    boundary->bbox = BoundingBoxf(bbox.min.cast<double>(), bbox.max.cast<double>());
    boundary->grid.set_bbox(bbox);
    // FIXME 1mm grid? -> use nozzle size or extrusion width
    boundary->grid.create(boundary->boundaries, (scale_i(1.)));
    init_boundary_distances(boundary);
    
    boundary->to_avoid = union_ex(boundary->to_avoid);
    bbox = BoundingBox(get_extents(boundary->to_avoid));
    bbox.offset(SCALED_EPSILON + extra_available_space);
    boundary->to_avoid_grid.set_bbox(bbox);
    // FIXME 1mm grid? -> use nozzle size or extrusion width
    boundary->to_avoid_grid.create(to_polygons(boundary->to_avoid), (scale_i(1.)));
}

static void init_boundary(AvoidCrossingPerimeters::Boundary *boundary, ExPolygons &&boundary_islands, coord_t extra_available_space)
{
    boundary->clear();
    // fill boundaries & boundary_to_island
    size_t island_id = 0;
    for (ExPolygon &island : boundary_islands) {
        island_id = boundary->boundaries.size();
        boundary->bboxes.emplace_back(island.contour.points);
        boundary->boundaries.push_back(std::move(island.contour));
        boundary->islands.push_back(island_id);
        for (Polygon &hole : island.holes) {
            boundary->bboxes.emplace_back(hole.points);
            boundary->boundaries.push_back(std::move(hole));
            boundary->islands.push_back(island_id);
        }
    }
    BoundingBox bbox(get_extents(boundary->boundaries));
    bbox.offset(SCALED_EPSILON + extra_available_space);
    boundary->bbox = BoundingBoxf(bbox.min.cast<double>(), bbox.max.cast<double>());
    boundary->grid.set_bbox(bbox);
    // FIXME 1mm grid? -> use nozzle size or extrusion width
    boundary->grid.create(boundary->boundaries, scale_i(1.));
    init_boundary_distances(boundary);
    
    boundary->to_avoid = union_ex(boundary->to_avoid);
    bbox = BoundingBox(get_extents(boundary->to_avoid));
    bbox.offset(SCALED_EPSILON + extra_available_space);
    boundary->to_avoid_grid.set_bbox(bbox);
    // FIXME 1mm grid? -> use nozzle size or extrusion width
    boundary->to_avoid_grid.create(to_polygons(boundary->to_avoid), scale_i(1.));

    assert(boundary->islands.size() == boundary->boundaries.size());
}

void AvoidCrossingPerimeters::SliceIsland::create_hole_bounding_boxes()
{
    if (hole_boundingboxes.size() == expolygon.holes.size())
        return;
    hole_boundingboxes.clear();
    for (const Polygon &hole : expolygon.holes)
        hole_boundingboxes.emplace_back(hole.points);
}

bool AvoidCrossingPerimeters::prepare_crossing_test(const PerimeterCrossingContext &context)
{
    const PrintObject *object = context.layer.object();
    const bool object_changed = m_crossing_cache.object == nullptr || m_crossing_cache.object != object;
    const bool instance_changed = m_crossing_cache.instance_id != context.instance_id;
    const bool cache_changed = m_crossing_cache.layer != &context.layer || object_changed || instance_changed ||
        m_crossing_cache.extruder_id != context.extruder_id ||
        m_crossing_cache.nozzle_radius != context.nozzle_radius;

    m_crossing_throw_if_canceled = context.throw_if_canceled ? context.throw_if_canceled : []() {};
    if (!cache_changed)
        return object_changed || instance_changed;

    m_crossing_cache.layer = &context.layer;
    m_crossing_cache.object = object;
    m_crossing_cache.instance_id = context.instance_id;
    m_crossing_cache.extruder_id = context.extruder_id;
    m_crossing_cache.nozzle_radius = context.nozzle_radius;

    const bool is_support_layer = context.layer.get_property<LayerSupportProperty>() != nullptr;
    ExPolygons slices;
    ExPolygons offset_slices;

    // Support travels may encounter any object layer already printed at this
    // height. Object travels only need the slices of their own current layer.
    if (is_support_layer) {
        for (const Layer *layer : context.already_printed_object_layers) {
            append(slices, layer->lslices());
            append(offset_slices, offset_ex(layer->lslices(), context.nozzle_radius * 1.5f));
        }
        slices = union_ex(slices);
        offset_slices = union_ex(offset_slices);
    } else {
        append(slices, context.layer.lslices());
        slices = union_ex(slices);

        // On multi-extruder layers, retain only regions printed by the active
        // tool so another material is not treated as safe interior.
        const LayerRegionCRefs all_regions = context.layer.regions();
        bool multiple_extruders = false;
        for (const LayerRegion &region : all_regions) {
            multiple_extruders = multiple_extruders ||
                region.region().config().perimeter_extruder.value != context.extruder_id + 1 ||
                region.region().config().infill_extruder.value != context.extruder_id + 1 ||
                region.region().config().solid_infill_extruder.value != context.extruder_id + 1;
            if (multiple_extruders)
                break;
        }

        if (multiple_extruders) {
            ExPolygons clip;
            for (const LayerRegion &region : all_regions) {
                const bool same_extruders =
                    region.region().config().perimeter_extruder.value ==
                        region.region().config().infill_extruder.value &&
                    region.region().config().infill_extruder.value ==
                        region.region().config().solid_infill_extruder.value;
                if (same_extruders) {
                    if (region.region().config().perimeter_extruder.value == context.extruder_id + 1)
                        clip = union_ex(clip, region.get_raw_slices());
                } else {
                    if (region.region().config().infill_extruder.value !=
                        region.region().config().solid_infill_extruder.value)
                        BOOST_LOG_TRIVIAL(warning) << "";
                    if (region.region().config().perimeter_extruder.value == context.extruder_id + 1) {
                        assert(region.region().config().infill_extruder.value != context.extruder_id + 1);
                        clip = union_ex(clip, diff_ex(region.get_raw_slices(), region.fill_expolygons()));
                    } else {
                        assert(region.region().config().infill_extruder.value == context.extruder_id + 1);
                        clip = union_ex(clip, region.fill_expolygons());
                    }
                }
            }
            slices = intersection_ex(slices, offset_ex(clip, SCALED_EPSILON * 10));
        }
        append(offset_slices, offset_ex(slices, -context.nozzle_radius * 1.5f));
    }

    // Top surfaces are not safe interior because crossing them may leave
    // visible marks even without crossing an outer contour.
    if (!is_support_layer) {
        for (const LayerRegion &region : context.layer.regions()) {
            m_crossing_throw_if_canceled();
            const ExPolygons top_surfaces = to_expolygons(
                region.fill_surfaces().filter_by_type_flag(SurfaceType::stPosTop));
            offset_slices = diff_ex(offset_slices, top_surfaces);
            slices = diff_ex(slices, top_surfaces);
        }
    }

    // Simplification and bounding boxes make repeated crossing tests cheap.
    m_crossing_cache.slices.clear();
    for (ExPolygon &expolygon : slices) {
        BoundingBox bounding_box{expolygon.contour.points};
        for (ExPolygon &simplified : expolygon.simplify(context.nozzle_radius)) {
#ifdef CAN_CROSS_PERIMETER_USE_GRID
            const int point_count = int(simplified.contour.size());
            if (point_count > 100) {
                EdgeGrid::Grid grid;
                grid.set_bbox(bounding_box);
                const coordf_t max_distance = std::max(
                    bounding_box.max.x() - bounding_box.min.x(),
                    bounding_box.max.y() - bounding_box.min.x());
                grid.create(simplified, max_distance / 100);
                m_crossing_cache.slices.emplace_back(
                    std::move(simplified), std::move(bounding_box), std::move(grid));
            } else
#endif
            {
                m_crossing_cache.slices.emplace_back(std::move(simplified), std::move(bounding_box));
            }
        }
    }

    m_crossing_cache.offset_slices.clear();
    for (ExPolygon &expolygon : offset_slices) {
        BoundingBox bounding_box{expolygon.contour.points};
        for (ExPolygon &simplified : expolygon.simplify(context.nozzle_radius)) {
#ifdef CAN_CROSS_PERIMETER_USE_GRID
            const int point_count = int(simplified.contour.size());
            if (point_count > 100) {
                EdgeGrid::Grid grid;
                grid.set_bbox(bounding_box);
                const coordf_t max_distance = std::max(
                    bounding_box.max.x() - bounding_box.min.x(),
                    bounding_box.max.y() - bounding_box.min.x());
                grid.create(simplified, max_distance / 100);
                m_crossing_cache.offset_slices.emplace_back(
                    std::move(simplified), std::move(bounding_box), std::move(grid));
            } else
#endif
            {
                m_crossing_cache.offset_slices.emplace_back(
                    std::move(simplified), std::move(bounding_box));
            }
        }
    }

    return object_changed || instance_changed;
}

bool AvoidCrossingPerimeters::can_cross_perimeter(const Polyline &travel, const bool offset)
{
    assert(m_crossing_cache.layer != nullptr);
    std::vector<SliceIsland> &islands =
        offset ? m_crossing_cache.offset_slices : m_crossing_cache.slices;

    // Bounding boxes reject most islands before the contour hot path runs.
    for (SliceIsland &island : islands) {
        if (travel.size() <= 1 ||
            !(island.boundingbox.contains(travel.front()) ||
              island.boundingbox.contains(travel.back()) ||
              island.boundingbox.contains(travel.points[travel.size() / 2]) ||
              island.boundingbox.cross(travel)))
            continue;

        const bool has_front = contains(island.expolygon.contour, travel.front(), true);
        const bool has_back = contains(island.expolygon.contour, travel.back(), true);
        if (has_front != has_back)
            return true;

        assert(travel.size() >= 2);
#ifdef CAN_CROSS_PERIMETER_USE_GRID
        if (travel.size() == 2 && island.grid) {
            GridIntersectTest tester(*island.grid, Line(travel.front(), travel.back()));
            island.grid->visit_cells_intersecting_line(tester.test_line.a, tester.test_line.b, tester);
            if (tester.intersect)
                return true;
            if (!has_front && !has_back)
                continue;
        } else
#endif
        {
            Line travel_line;
            Point intersection;
            for (size_t travel_idx = travel.size() - 1; travel_idx > 0; --travel_idx) {
                travel_line.a = travel.points[travel_idx];
                travel_line.b = travel.points[travel_idx - 1];
                if (island.expolygon.contour.first_intersection(travel_line, &intersection) ||
                    Line(island.expolygon.contour.first_point(),
                         island.expolygon.contour.last_point()).intersection(travel_line, &intersection))
                    return true;
            }
            if (!has_front)
                continue;
        }

        // A path inside the outer contour may still cross one of its holes.
        if (has_front && has_back) {
            Line travel_line;
            Point intersection;
            island.create_hole_bounding_boxes();
            for (size_t hole_idx = 0; hole_idx < island.expolygon.holes.size(); ++hole_idx) {
                const Polygon &hole = island.expolygon.holes[hole_idx];
                const BoundingBox &hole_box = island.hole_boundingboxes[hole_idx];
                m_crossing_throw_if_canceled();
                for (size_t travel_idx = travel.size() - 1; travel_idx > 0; --travel_idx) {
                    travel_line.a = travel.points[travel_idx];
                    travel_line.b = travel.points[travel_idx - 1];
                    if (hole.size() > 10 && !hole_box.cross(travel_line) &&
                        !hole_box.contains(travel_line.a))
                        continue;
                    if (hole.first_intersection(travel_line, &intersection) ||
                        Line(hole.first_point(), hole.last_point()).intersection(travel_line, &intersection))
                        return true;
                }
            }
        }
    }
    return false;
}

// Plan travel, which avoids perimeter crossings by following the boundaries of the layer.
Polyline AvoidCrossingPerimeters::travel_to(const TravelContext &context,
                                            const Point &point,
                                            bool *could_be_wipe_disabled)
{
    // External routing uses print coordinates, while internal routing keeps the
    // active object's local coordinates. The supplied origin converts between
    // these spaces without consulting the caller that owns the machine state.
    // Otherwise perform the path planning in the coordinate system of the active object.
    bool        use_external  = m_use_external_mp || m_use_external_mp_once;
    Point       scaled_origin = use_external ? context.origin : Point(0, 0);
    const Point start         = context.start + scaled_origin;
    const Point end           = point + scaled_origin;
    const Line  travel(start, end);

    Polyline result_pl;
    Vec2d startf = start.cast<double>();
    Vec2d endf   = end  .cast<double>();

    //const ExPolygons &lslices           = context.layer.lslices();
    const coord_t     perimeter_spacing = get_perimeter_spacing(context.layer);
    bool              is_support_layer  = context.layer.get_property<LayerSupportProperty>() != nullptr;

    if (!use_external && (is_support_layer || !m_lslices_offset.empty())) {
        // Initialize m_internal only when it is necessary.
        if (m_internal.boundaries.empty()) {
            std::vector<std::pair<ExPolygon, ExPolygon>> boundary_growth;
            init_boundary(&m_internal, get_boundary(context.layer, context.extruder_id, boundary_growth, m_internal.to_avoid), perimeter_spacing * 2);
            m_internal.boundary_growth = std::move(boundary_growth);
        }

        // Don't
        // Trim the travel line by the bounding box.
        if (!m_internal.boundaries.empty() && Geometry::liang_barsky_line_clipping(startf, endf, m_internal.bbox)) {
            Point nearest_start = start;
            Point nearest_end = end;
            // get nearest point
            if (!m_internal.bbox.contains(nearest_start.cast<double>())) {
                BoundingBox bb_coord_t(m_internal.bbox.min.cast<coord_t>(), m_internal.bbox.max.cast<coord_t>());
                nearest_start = bb_coord_t.nearest_point(nearest_start);
            }
            if (!m_internal.bbox.contains(nearest_end.cast<double>())) {
                BoundingBox bb_coord_t(m_internal.bbox.min.cast<coord_t>(), m_internal.bbox.max.cast<coord_t>());
                nearest_end = bb_coord_t.nearest_point(nearest_end);
            }
            avoid_perimeters(m_internal, nearest_start/*startf.cast<coord_t>()*/, nearest_end/*endf.cast<coord_t>()*/, perimeter_spacing, context.layer, result_pl);
            assert(result_pl.size() > 1);
            for (size_t i = 1; i < result_pl.size(); i++)
                assert(!result_pl.points[i - 1].coincides_with_epsilon(result_pl.points[i]));
            result_pl.points.front()  = start;
            result_pl.points.back()   = end;
        }
    } else if(use_external) {
        // Initialize m_external only when exist any external travel for the current layer.
        if (m_external.boundaries.empty())
            init_boundary(&m_external, get_boundary_external(context.layer), perimeter_spacing * 2);

        // Trim the travel line by the bounding box.
        if (!m_external.boundaries.empty() && Geometry::liang_barsky_line_clipping(startf, endf, m_external.bbox)) {
            Point nearest_start = start;
            Point nearest_end = end;
            // get nearest point
            if (!m_external.bbox.contains(nearest_start.cast<double>())) {
                BoundingBox bb_coord_t(m_external.bbox.min.cast<coord_t>(), m_external.bbox.max.cast<coord_t>());
                nearest_start = bb_coord_t.nearest_point(nearest_start);
            }
            if (!m_external.bbox.contains(nearest_end.cast<double>())) {
                BoundingBox bb_coord_t(m_external.bbox.min.cast<coord_t>(), m_external.bbox.max.cast<coord_t>());
                nearest_end = bb_coord_t.nearest_point(nearest_end);
            }
            avoid_perimeters(m_external, nearest_start/*startf.cast<coord_t>()*/, nearest_end/*endf.cast<coord_t>()*/, 0, context.layer, result_pl);
            assert(result_pl.size() > 1);
            for (size_t i = 1; i < result_pl.size(); i++)
                assert(!result_pl.points[i - 1].coincides_with_epsilon(result_pl.points[i]));
            result_pl.points.front()  = start;
            result_pl.points.back()   = end;
        }
    }
    assert(result_pl.size() > 1);
    for (size_t i = 1; i < result_pl.size(); i++)
        assert(!result_pl.points[i - 1].coincides_with_epsilon(result_pl.points[i]));

    if(result_pl.empty()) {
        // Travel line is completely outside the bounding box.
        result_pl = {start, end};
    }

    bool                              max_detour_length_exceeded = false;
    if (context.max_detour > 0) {
        distf_t direct_length     = travel.length();
        distf_t detour            = result_pl.length() - direct_length;
        distf_t max_detour_length = context.max_detour_is_percent ?
            direct_length * 0.01 * context.max_detour :
            scale_d(context.max_detour);
        if (detour > max_detour_length) {
            result_pl = {start, end};
            max_detour_length_exceeded = true;
        }
    }

    if (use_external) {
        result_pl.translate(-scaled_origin);
        *could_be_wipe_disabled = false;
    } else if (max_detour_length_exceeded) {
        *could_be_wipe_disabled = false;
    }
    assert(result_pl.size() > 1);
    for (size_t i = 1; i < result_pl.size(); i++)
        assert(!result_pl.points[i - 1].coincides_with_epsilon(result_pl.points[i]));

    return result_pl;
}

// ************************************* AvoidCrossingPerimeters::init_layer() *****************************************

void AvoidCrossingPerimeters::init_layer(const Layer &layer)
{
    m_internal.clear();
    m_external.clear();
    m_lslices_offset.clear();
    m_init_to = &layer;

    float ext_perimeter_width = get_external_perimeter_width(layer);
    //get perimeter_boundary from island instead of layer.lslices()
    for (const LayerSliceIsland &layer_island : layer.islands()) {
        append(m_lslices_offset, layer_island.get_perimeter_slices());
    }
    m_lslices_offset = offset_ex(m_lslices_offset, -ext_perimeter_width / 2);
    m_init = true;
}


} // namespace Slic3r
