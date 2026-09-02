///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/ Copyright (c) Prusa Research 2016 - 2023 Vojtěch Bubník @bubnikv, Lukáš Hejl @hejllukas, Enrico Turri @enricoturri1966
///|/ Copyright (c) Slic3r 2013 - 2016 Alessandro Ranellucci @alranel
///|/ Copyright (c) 2015 Maksim Derbasov @ntfshard
///|/ Copyright (c) 2014 Petr Ledvina @ledvinap
///|/
///|/ ported from lib/Slic3r/Polyline.pm:
///|/ Copyright (c) Prusa Research 2018 Vojtěch Bubník @bubnikv
///|/ Copyright (c) Slic3r 2011 - 2014 Alessandro Ranellucci @alranel
///|/ Copyright (c) 2012 Mark Hindess
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "Polyline.hpp"

#include <algorithm>
#include <iostream>
#include <utility>

#include "Api/internal/ArcPolylineAccess.hpp"
#include "BoundingBox.hpp"
#include "Exception.hpp"
#include "ExPolygon.hpp"
#include "Line.hpp"
#include "Polygon.hpp"

namespace Slic3r {

double total_length(const Polylines &polylines)
{
    double total = 0;
    for (const Polyline &pl : polylines)
        total += pl.length();
    return total;
}

Lines to_lines(const Polyline &poly) 
{
    Lines lines;
    if (poly.points.size() >= 2) {
        lines.reserve(poly.points.size() - 1);
        for (Points::const_iterator it = poly.points.begin(); it != poly.points.end()-1; ++it)
            lines.push_back(Line(*it, *(it + 1)));
    }
    return lines;
}

Lines to_lines(const Polylines &polys) 
{
    size_t n_lines = 0;
    for (size_t i = 0; i < polys.size(); ++ i)
        if (polys[i].points.size() > 1)
            n_lines += polys[i].points.size() - 1;
    Lines lines;
    lines.reserve(n_lines);
    for (size_t i = 0; i < polys.size(); ++ i) {
        const Polyline &poly = polys[i];
        for (Points::const_iterator it = poly.points.begin(); it != poly.points.end()-1; ++it)
            lines.push_back(Line(*it, *(it + 1)));
    }
    return lines;
}

Polylines to_polylines(const std::vector<Points> &paths)
{
    Polylines out;
    out.reserve(paths.size());
    for (const Points &path : paths)
        out.emplace_back(path);
    return out;
}

Polylines to_polylines(std::vector<Points> &&paths)
{
    Polylines out;
    out.reserve(paths.size());
    for (Points &path : paths)
        out.emplace_back(std::move(path));
    return out;
}

void polylines_append(Polylines &dst, const Polylines &src) 
{ 
    dst.insert(dst.end(), src.begin(), src.end());
}

void polylines_append(Polylines &dst, Polylines &&src) 
{
    if (dst.empty()) {
        dst = std::move(src);
    } else {
        std::move(std::begin(src), std::end(src), std::back_inserter(dst));
        src.clear();
    }
}

ThickPolylines to_thick_polylines(Polylines &&polylines, const coordf_t width)
{
    ThickPolylines out;
    out.reserve(polylines.size());
    for (Polyline& polyline : polylines) {
        out.emplace_back();
        out.back().points_width.assign(polyline.points.size(), width);
        out.back().points = std::move(polyline.points);
    }
    return out;
}

bool ThickPolyline::is_valid() const
{
    assert(points.size() == points_width.size());
    return this->points.size() >= 2;
}

distf_t ThickPolyline::length() const
{
    assert(points.size() == points_width.size());
    return Slic3r::length(this->points);
}

void ThickPolyline::reverse()
{
    assert(points.size() == points_width.size());
    std::reverse(this->points.begin(), this->points.end());
    std::reverse(this->points_width.begin(), this->points_width.end());
    std::swap(this->endpoints.first, this->endpoints.second);
    start_at = StartPos(-start_at);
}

distf_t ArcPolyline::length() const
{
    return Geometry::ArcWelder::path_length<distf_t>(m_path);
}

const Point& Polyline::leftmost_point() const
{
    const Point *p = &this->points.front();
    for (Points::const_iterator it = this->points.begin() + 1; it != this->points.end(); ++ it) {
        if (it->x() < p->x()) 
        	p = &(*it);
    }
    return *p;
}

double Polyline::length() const
{
    double l = 0;
    for (size_t i = 1; i < this->points.size(); ++ i)
        l += this->points[i].distance_to(this->points[i - 1]);
    return l;
}

Lines Polyline::lines() const
{
    Lines lines;
    if (this->points.size() >= 2) {
        lines.reserve(this->points.size() - 1);
        for (Points::const_iterator it = this->points.begin(); it != this->points.end()-1; ++it) {
            lines.push_back(Line(*it, *(it + 1)));
        }
    }
    return lines;
}

// removes the given distance from the end of the polyline
void Polyline::clip_end(distf_t distance)
{
    while (distance > 0) {
        Vec2d  last_point = this->last_point().cast<coordf_t>();
        this->points.pop_back();
        if (this->points.empty())
            break;
        Vec2d  v    = this->last_point().cast<coordf_t>() - last_point;
        distsqrf_t lsqr = v.squaredNorm();
        if (lsqr > distance * distance) {
            this->points.push_back(Point::round(last_point + v * (distance / sqrt(lsqr))));
            return;
        }
        distance -= sqrt(lsqr);
    }
}

// removes the given distance from the start of the polyline
void Polyline::clip_start(distf_t distance)
{
    this->reverse();
    this->clip_end(distance);
    if (this->points.size() >= 2)
        this->reverse();
}

void Polyline::extend_end(distf_t distance)
{
    // relocate last point by extending the last segment by the specified length
    Vec2d v = (this->points.back() - *(this->points.end() - 2)).cast<coordf_t>().normalized();
    this->points.back() += Point::round(v * distance);
}

void Polyline::extend_start(distf_t distance)
{
    // relocate first point by extending the first segment by the specified length
    Vec2d v = (this->points.front() - this->points[1]).cast<coordf_t>().normalized();
    this->points.front() += Point::round(v * distance);
}

/* this method returns a collection of points picked on the polygon contour
   so that they are evenly spaced according to the input distance */
Points Polyline::equally_spaced_points(distf_t distance) const
{
    Points points;
    points.emplace_back(this->first_point());
    double len = 0;
    
    for (Points::const_iterator it = this->points.begin() + 1; it != this->points.end(); ++it) {
        const Vec2d  p1 = (it-1)->cast<double>();
        const Vec2d  v  = it->cast<double>() - p1;
        const coordf_t segment_length = v.norm();
        len += segment_length;
        if (len < distance)
            continue;
        if (len == distance) {
            points.emplace_back(*it);
            len = 0;
            continue;
        }
        coordf_t take = segment_length - (len - distance);  // how much we take of this segment
        points.push_back(Point::round(p1 + v * (take / segment_length)));
        -- it;
        len = - take;
    }
    return points;
}

void Polyline::simplify(coordf_t tolerance)
{
    this->points = MultiPoint::douglas_peucker(this->points, tolerance);
}

#if 0
// This method simplifies all *lines* contained in the supplied area
template <class T>
void Polyline::simplify_by_visibility(const T &area)
{
    Points &pp = this->points;
    
    size_t s = 0;
    bool did_erase = false;
    for (size_t i = s+2; i < pp.size(); i = s + 2) {
        if (area.contains(Line(pp[s], pp[i]))) {
            pp.erase(pp.begin() + s + 1, pp.begin() + i);
            did_erase = true;
        } else {
            ++s;
        }
    }
    if (did_erase)
        this->simplify_by_visibility(area);
}
template void Polyline::simplify_by_visibility<ExPolygon>(const ExPolygon &area);
template void Polyline::simplify_by_visibility<ExPolygonCollection>(const ExPolygonCollection &area);
#endif

void Polyline::split_at(const Point &point, Polyline* p1, Polyline* p2) const
{
    if (this->points.empty()) return;

    if (this->size() < 2) {
        *p1 = *this;
        p2->clear();
        return;
    }

    if (this->points.front() == point) {
        //FIXME why is p1 NOT empty as in the case above?
        *p1 = { point };
        *p2 = *this;
        return;
    }

    double min_dist2 = std::numeric_limits<double>::max();
    auto   min_point_it = this->points.cbegin();
    Point  prev = this->points.front();
    for (auto it = this->points.cbegin() + 1; it != this->points.cend(); ++it) {
        Point proj;
        if (double d2 = Line::distance_to_squared_abp(prev, *it, point, &proj); d2 < min_dist2) {
            min_dist2 = d2;
            min_point_it = it;
        }
        prev = *it;
    }

    p1->points.assign(this->points.cbegin(), min_point_it);
    if (p1->points.back() != point)
        p1->points.emplace_back(point);
    
    p2->points = { point };
    if (*min_point_it == point)
        ++ min_point_it;
    p2->points.insert(p2->points.end(), min_point_it, this->points.cend());
}

Polyline Polyline::split_at(distf_t dist) {
    if (dist <= SCALED_EPSILON) {
        Polyline me = *this;
        clear();
        return me;
    }

    Point pt_split = points.front();
    size_t next_point_idx = 1;
    while (dist > SCALED_EPSILON && next_point_idx < size()) {
        distsqrf_t dist_sqr = points[next_point_idx - 1].distance_to_square(points[next_point_idx]);
        if (dist_sqr < Slic3r::sqr(SCALED_EPSILON + dist)) {
            dist -= std::sqrt(dist_sqr);
            ++next_point_idx;
        } else {
            pt_split = Line(points[next_point_idx - 1], points[next_point_idx]).point_at(dist);;
            dist = 0;
            break;
        }
    }

    if (pt_split == points.front()) {
        if (next_point_idx < size()) {
            // just over an existing point
            Polyline second_part;
            second_part.points.insert(second_part.points.end(), points.begin() + next_point_idx, points.end());
            points.resize(next_point_idx + 1);
            return second_part;
        } else {
            // too long
            return Polyline();
        }
    } else {
        // mid point
        Polyline second_part;
        second_part.points.push_back(pt_split);
        second_part.points.insert(second_part.points.end(), points.begin() + next_point_idx, points.end());
        points.resize(next_point_idx + 1);
        points.back() = pt_split;
        return second_part;
    }
}

bool Polyline::is_straight() const
{
    // Check that each segment's direction is equal to the line connecting
    // first point and last point. (Checking each line against the previous
    // one would cause the error to accumulate.)
    double dir = Line(this->first_point(), this->last_point()).direction();
    for (const auto &line: this->lines())
        if (! line.parallel_to(dir))
            return false;
    return true;
}

BoundingBox get_extents(const Polyline &polyline)
{
    return polyline.bounding_box();
}

BoundingBox get_extents(const Polylines &polylines)
{
    BoundingBox bb;
    if (! polylines.empty()) {
        bb = polylines.front().bounding_box();
        for (size_t i = 1; i < polylines.size(); ++ i)
            bb.merge(polylines[i].points);
    }
    return bb;
}

Polyline reverse_polyline(const Polyline &polyline) {
    Polyline p2 = polyline;
    p2.reverse();
    return p2;
};

// Return True when erase some otherwise False.
bool remove_same_neighbor(Polyline &polyline) {
    Points &points = polyline.points;
    if (points.empty())
        return false;
    auto last = std::unique(points.begin(), points.end());

    // no duplicits
    if (last == points.end())
        return false;

    points.erase(last, points.end());
    return true;
}

bool remove_same_neighbor(Polylines &polylines){
    if (polylines.empty())
        return false;
    bool exist = false;
    for (Polyline &polyline : polylines)
        exist |= remove_same_neighbor(polyline);
    // remove empty polylines
    polylines.erase(std::remove_if(polylines.begin(), polylines.end(), [](const Polyline &p) { return p.points.size() <= 1; }), polylines.end());
    return exist;
}

void ensure_valid(Polylines &polylines, coord_t resolution) {
    for (size_t i = 0; i < polylines.size(); ++i) {
        assert(polylines[i].size() > 1);
        polylines[i].douglas_peucker(resolution);
        assert(polylines[i].size() > 1);
        if (polylines[i].size() == 2 && polylines[i].front().coincides_with_epsilon(polylines[i].back())) {
            polylines.erase(polylines.begin() + i);
            --i;
        }
    }
}

Polylines ensure_valid(Polylines &&polylines, coord_t resolution) {
    for (size_t i = 0; i < polylines.size(); ++i) {
        assert(polylines[i].size() > 1);
        polylines[i].douglas_peucker(resolution);
        assert(polylines[i].size() > 1);
        if (polylines[i].size() == 2 && polylines[i].front().coincides_with_epsilon(polylines[i].back())) {
            polylines.erase(polylines.begin() + i);
            --i;
        }
    }
    return std::move(polylines);
}

void ensure_valid(Polyline &polyline, coord_t resolution) {
    assert(polyline.size() > 1);
    polyline.douglas_peucker(resolution);
    assert(polyline.size() > 1);
    if (polyline.size() == 2 && polyline.front().coincides_with_epsilon(polyline.back())) {
        polyline.clear();
    }
}

const Point& leftmost_point(const Polylines &polylines)
{
    if (polylines.empty())
        throw Slic3r::InvalidArgument("leftmost_point() called on empty Polylines");
    Polylines::const_iterator it = polylines.begin();
    const Point *p = &it->leftmost_point();
    for (++ it; it != polylines.end(); ++it) {
        const Point *p2 = &it->leftmost_point();
        if (p2->x() < p->x())
            p = p2;
    }
    return *p;
}

bool remove_degenerate(Polylines &polylines)
{
    bool modified = false;
    size_t j = 0;
    for (size_t i = 0; i < polylines.size(); ++ i) {
        if (polylines[i].points.size() >= 2) {
            if (j < i) 
                std::swap(polylines[i].points, polylines[j].points);
            ++ j;
        } else
            modified = true;
    }
    if (j < polylines.size())
        polylines.erase(polylines.begin() + j, polylines.end());
    return modified;
}

#ifdef _DEBUGINFO
void assert_valid(const Polylines &polylines) {
    for (const Polyline &polyline : polylines) {
        polyline.assert_valid();
    }
}
#endif

std::pair<int, Point> foot_pt(const Points &polyline, const Point &pt)
{
    if (polyline.size() < 2)
        return std::make_pair(-1, Point(0, 0));

    auto  d2_min  = std::numeric_limits<double>::max();
    Point foot_pt_min;
    Point prev = polyline.front();
    auto  it = polyline.begin();
    auto  it_proj = polyline.begin();
    for (++ it; it != polyline.end(); ++ it) {
        Point foot_pt;
        if (double d2 = Line::distance_to_squared_abp(prev, *it, pt, &foot_pt); d2 < d2_min) {
            d2_min      = d2;
            foot_pt_min = foot_pt;
            it_proj     = it;
        }
        prev = *it;
    }
    return std::make_pair(int(it_proj - polyline.begin()) - 1, foot_pt_min);
}

ThickLines ThickPolyline::thicklines() const
{
    ThickLines lines;
    if (this->points.size() >= 2) {
        lines.reserve(this->points.size() - 1);
        for (size_t i = 0; i + 1 < this->points.size(); ++ i)
            lines.emplace_back(this->points[i], this->points[i + 1], this->points_width[i], this->points_width[i + 1]);
    }
    return lines;
}

// Removes the given distance from the end of the ThickPolyline
void ThickPolyline::clip_end(distf_t distance)
{
    assert(this->points_width.size() == this->points.size());
    if (! this->empty()) {
        while (distance > 0) {
            Vec2d    last_point = this->back().cast<double>();
            coord_t last_width = this->points_width.back();
            this->points.pop_back();
            this->points_width.pop_back();
            assert(this->points_width.size() == this->points.size());
            if (this->points.empty())
                break;

            assert(this->points_width.size() == this->points.size());
            Vec2d    vec            = this->back().cast<double>() - last_point;
            coordf_t width_diff     = this->points_width.back() - last_width;
            coordf_t   vec_length_sqr = vec.squaredNorm();
            if (vec_length_sqr > distance * distance) {
                coordf_t t = (distance / std::sqrt(vec_length_sqr));
                this->points.push_back(Point::round(last_point + vec * t));
                this->points_width.emplace_back(last_width + width_diff * t);
                assert(this->points_width.size() == this->points.size());
                return;
            }

        distance -= std::sqrt(vec_length_sqr);
        }
    assert(this->points_width.size() == this->points.size());
    }
}
void ThickPolyline::extend_end(distf_t distance)
{
    // relocate last point by extending the last segment by the specified length
    Vec2d v = (this->points.back() - *(this->points.end() - 2)).cast<coordf_t>().normalized();
    this->points.back() += Point::round(v * distance);
}

void ThickPolyline::extend_start(distf_t distance)
{
    // relocate first point by extending the first segment by the specified length
    Vec2d v = (this->points.front() - this->points[1]).cast<coordf_t>().normalized();
    this->points.front() += Point::round(v * distance);
}

void ThickPolyline::start_at_index(int index)
{
    assert(index >= 0 && index < this->points.size());
    assert(this->points.front() == this->points.back() && this->points_width.front() == this->points_width.back());
    if (index != 0 && index + 1 != int(this->points.size()) && this->points.front() == this->points.back() && this->points_width.front() == this->points_width.back()) {
        this->points.pop_back();
        this->points_width.pop_back();
        assert(this->points.size() == this->points_width.size());
        std::rotate(this->points.begin(), this->points.begin() + index, this->points.end());
        std::rotate(this->points_width.begin(), this->points_width.begin() + index, this->points_width.end());
        this->points.emplace_back(this->points.front());
        this->points_width.emplace_back(this->points_width.front());
    }
}

double Polyline3::length() const
{
    double l = 0;
    for (size_t i = 1; i < this->points.size(); ++ i)
        l += (this->points[i] - this->points[i - 1]).cast<double>().norm();
    return l;
}

Lines3 Polyline3::lines() const
{
    Lines3 lines;
    if (points.size() >= 2)
    {
        lines.reserve(points.size() - 1);
        for (Points3::const_iterator it = points.begin(); it != points.end() - 1; ++it)
        {
            lines.emplace_back(*it, *(it + 1));
        }
    }
    return lines;
}

void concatThickPolylines(ThickPolylines& pp) {
    bool changes = true;
    while (changes){
        changes = false;
        //concat polyline if only 2 polyline at a point
        for (size_t i = 0; i < pp.size(); ++i) {
            ThickPolyline *polyline = &pp[i];
            if (polyline->front().coincides_with_epsilon(polyline->back())) {
                polyline->endpoints.first = false;
                polyline->endpoints.second = false;
                continue;
            }

            size_t id_candidate_first_point = -1;
            size_t id_candidate_last_point = -1;
            size_t nbCandidate_first_point = 0;
            size_t nbCandidate_last_point = 0;
            // find another polyline starting here
            for (size_t j = 0; j < pp.size(); ++j) {
                if (j == i) continue;
                ThickPolyline *other = &pp[j];
                if (polyline->back().coincides_with_epsilon(other->back())) {
                    id_candidate_last_point = j;
                    nbCandidate_last_point++;
                }
                if (polyline->back().coincides_with_epsilon(other->front())) {
                    id_candidate_last_point = j;
                    nbCandidate_last_point++;
                }
                if (polyline->front().coincides_with_epsilon(other->back())) {
                    id_candidate_first_point = j;
                    nbCandidate_first_point++;
                }
                if (polyline->front().coincides_with_epsilon(other->front())) {
                    id_candidate_first_point = j;
                    nbCandidate_first_point++;
                }
            }
            if (id_candidate_last_point == id_candidate_first_point && nbCandidate_first_point == 1 && nbCandidate_last_point == 1) {
                if (polyline->front().coincides_with_epsilon(pp[id_candidate_first_point].front())) pp[id_candidate_first_point].reverse();
                // it's a trap! it's a  loop!
                polyline->points.insert(polyline->points.end(), pp[id_candidate_first_point].points.begin() + 1, pp[id_candidate_first_point].points.end());
                polyline->points_width.insert(polyline->points_width.end(), pp[id_candidate_first_point].points_width.begin() + 1, pp[id_candidate_first_point].points_width.end());
                pp.erase(pp.begin() + id_candidate_first_point);
                changes = true;
                polyline->endpoints.first = false;
                polyline->endpoints.second = false;
            } else {

                if (nbCandidate_first_point == 1) {
                    if (polyline->front().coincides_with_epsilon(pp[id_candidate_first_point].front())) pp[id_candidate_first_point].reverse();
                    //concat at front
                    polyline->points_width[0] = std::max(polyline->points_width.front(), pp[id_candidate_first_point].points_width.back());
                    polyline->points.insert(polyline->points.begin(), pp[id_candidate_first_point].points.begin(), pp[id_candidate_first_point].points.end() - 1);
                    polyline->points_width.insert(polyline->points_width.begin(), pp[id_candidate_first_point].points_width.begin(), pp[id_candidate_first_point].points_width.end() - 1);
                    polyline->endpoints.first = pp[id_candidate_first_point].endpoints.first;
                    pp.erase(pp.begin() + id_candidate_first_point);
                    changes = true;
                    if (id_candidate_first_point < i) {
                        i--;
                        polyline = &pp[i];
                    }
                    if (id_candidate_last_point > id_candidate_first_point) {
                        id_candidate_last_point--;
                    }
                } else if (nbCandidate_first_point == 0) {
                    //update endpoint
                    polyline->endpoints.first = true;
                }
                if (nbCandidate_last_point == 1) {
                    if (polyline->back().coincides_with_epsilon(pp[id_candidate_last_point].back())) pp[id_candidate_last_point].reverse();
                    //concat at back
                    polyline->points_width[polyline->points_width.size() - 1] = std::max(polyline->points_width.back(), pp[id_candidate_last_point].points_width.front());
                    polyline->points.insert(polyline->points.end(), pp[id_candidate_last_point].points.begin() + 1, pp[id_candidate_last_point].points.end());
                    polyline->points_width.insert(polyline->points_width.end(), pp[id_candidate_last_point].points_width.begin() + 1, pp[id_candidate_last_point].points_width.end());
                    polyline->endpoints.second = pp[id_candidate_last_point].endpoints.second;
                    pp.erase(pp.begin() + id_candidate_last_point);
                    changes = true;
                    if (id_candidate_last_point < i) {
                        i--;
                        polyline = &pp[i];
                    }
                } else if (nbCandidate_last_point == 0) {
                    //update endpoint
                    polyline->endpoints.second = true;
                }

                if (polyline->back().coincides_with_epsilon(polyline->front())) {
                    //the concat has created a loop : update endpoints
                    polyline->endpoints.first = false;
                    polyline->endpoints.second = false;
                }
            }
        }
    }
}

//////////////// ArcPolyline ////////////////////////
// 
// Length of a smooth path.
//
//std::optional<Point> sample_path_point_at_distance_from_start(Geometry::ArcWelder::Path &path, double distance)
//{
//    if (distance >= 0) {
//        auto  it         = path.begin();
//        auto  end        = path.end();
//        Point prev_point = it->point;
//        for (++it; it != end; ++it) {
//            Point point = it->point;
//            if (it->linear()) {
//                // Linear segment
//                Vec2d  v    = (point - prev_point).cast<double>();
//                double lsqr = v.squaredNorm();
//                if (lsqr > sqr(distance))
//                    return std::make_optional<Point>(prev_point + Point::round(v * (distance / sqrt(lsqr))));
//                distance -= sqrt(lsqr);
//            } else {
//                // Circular segment
//                float  angle = Geometry::ArcWelder::arc_angle(prev_point.cast<float>(), point.cast<float>(), it->radius);
//                double len   = std::abs(it->radius) * angle;
//                if (len > distance) {
//                    // Rotate the segment end point in reverse towards the start point.
//                    return std::make_optional<Point>(
//                        prev_point.rotated(-angle * (distance / len),
//                                            Point::round(Geometry::ArcWelder::arc_center(prev_point.cast<float>(), point.cast<float>(), it->radius,
//                                                                            it->ccw())
//                                                )));
//                }
//                distance -= len;
//            }
//            if (distance < 0)
//                return std::make_optional<Point>(point);
//            prev_point = point;
//        }
//    }
//    // Failed.
//    return {};
//}
//
//
//std::optional<Point> sample_path_point_at_distance_from_end(Geometry::ArcWelder::Path &path, double distance)
//{
//    if (distance >= 0) {
//        auto  it         = path.begin();
//        auto  end        = path.end();
//        Point prev_point = it->point;
//        for (++it; it != end; ++it) {
//            Point point = it->point;
//            if (it->linear()) {
//                // Linear segment
//                Vec2d  v    = (point - prev_point).cast<double>();
//                double lsqr = v.squaredNorm();
//                if (lsqr > sqr(distance))
//                    return std::make_optional<Point>(prev_point + Point::round(v * (distance / sqrt(lsqr))));
//                distance -= sqrt(lsqr);
//            } else {
//                // Circular segment
//                float  angle = Geometry::ArcWelder::arc_angle(prev_point.cast<float>(), point.cast<float>(), it->radius);
//                double len   = std::abs(it->radius) * angle;
//                if (len > distance) {
//                    // Rotate the segment end point in reverse towards the start point.
//                    return std::make_optional<Point>(
//                        prev_point.rotated(-angle * (distance / len),
//                                            Point::round(Geometry::ArcWelder::arc_center(prev_point.cast<float>(), point.cast<float>(), it->radius,
//                                                                            it->ccw())
//                                                )));
//                }
//                distance -= len;
//            }
//            if (distance < 0)
//                return std::make_optional<Point>(point);
//            prev_point = point;
//        }
//    }
//    // Failed.
//    return {};
//}

bool not_arc(const ArcPolyline& arcs)
{
    for (const Geometry::ArcWelder::Segment &s : arcs.get_arc())
        if (s.radius != 0)
            return false;
    return true;
}

ArcPolyline::ArcPolyline(const Geometry::ArcWelder::Path &other) : m_path(other) {
    m_only_strait = not_arc(*this);
}

ArcPolyline::ArcPolyline(const ArcPolyline &other)
    : m_path(other.m_path)
    , m_z_offset(other.m_z_offset ? std::make_unique<std::vector<coord_t>>(*other.m_z_offset) : nullptr)
    , m_only_strait(other.m_only_strait)
{
#ifdef _DEBUG
    is_3D = other.is_3D;
#endif
}

ArcPolyline &ArcPolyline::operator=(const ArcPolyline &other)
{
    if (this != &other) {
        m_path = other.m_path;
        m_z_offset = other.m_z_offset ? std::make_unique<std::vector<coord_t>>(*other.m_z_offset) : nullptr;
        m_only_strait = other.m_only_strait;
#ifdef _DEBUG
        is_3D = other.is_3D;
#endif
    }
    return *this;
}

std::vector<coord_t>& ArcPolyline::z_offsets_raw_mutable()
{
    if (!m_z_offset)
        m_z_offset = std::make_unique<std::vector<coord_t>>();
    return *m_z_offset;
}

std::vector<coord_t>& ArcPolyline::z_offsets_mutable()
{
    std::vector<coord_t> &z_offsets = this->z_offsets_raw_mutable();
    if (z_offsets.empty())
        z_offsets.resize(m_path.size(), coord_t(0));
    return z_offsets;
}

bool ArcPolyline::has_arc() const {
    assert(not_arc(*this) == m_only_strait);
    return !m_only_strait;
}

void ArcPolyline::clear() {
    m_path.clear();
    m_z_offset.reset();
    m_only_strait = true;
}

void ArcPolyline::swap(ArcPolyline &other) {
    m_path.swap(other.m_path);
    m_z_offset.swap(other.m_z_offset);
    std::swap(this->m_only_strait, other.m_only_strait);
    assert(is_valid());
}

void ArcPolyline::reverse() {
    Geometry::ArcWelder::reverse(m_path);
    if (m_z_offset)
        std::reverse(m_z_offset->begin(), m_z_offset->end());
}

void ArcPolyline::append(const Point &point) {
    m_path.emplace_back(/*Geometry::ArcWelder::Segment{*/ point, 0.f, Geometry::ArcWelder::Orientation::Unknown /*}*/);
    if (m_z_offset)
        m_z_offset->push_back(0);
}

void ArcPolyline::append_before(const Point &point) {
    m_path.insert(m_path.begin(), Geometry::ArcWelder::Segment{point, 0.f, Geometry::ArcWelder::Orientation::Unknown});
    if (m_z_offset)
        m_z_offset->insert(m_z_offset->begin(), 0);
}

void ArcPolyline::append(const Points &src)
{
    size_t i = 0;
    if (!m_path.empty() && !src.empty() && m_path.back().point.coincides_with_epsilon(src.front())) {
        i++;
    }
    if (m_z_offset)
        m_z_offset->insert(m_z_offset->end(), src.size() - i, 0);
    for (; i < src.size(); ++i) {
        const Point &point = src[i];
        m_path.emplace_back(point, 0.f, Geometry::ArcWelder::Orientation::Unknown);
    }
    assert(is_valid());
}

void ArcPolyline::append(Points &&src)
{
    size_t i = 0;
    if (!m_path.empty() && !src.empty() && m_path.back().point.coincides_with_epsilon(src.front())) {
        i++;
    }
    if (m_z_offset)
        m_z_offset->insert(m_z_offset->end(), src.size() - i, 0);
    for (; i < src.size(); ++i) {
        const Point &point = src[i];
        m_path.emplace_back(std::move(point), 0, Geometry::ArcWelder::Orientation::Unknown);
    }
    assert(is_valid());
}

void ArcPolyline::append(const Points::const_iterator &begin, const Points::const_iterator &end)
{
    Points::const_iterator it = begin;
    if (!m_path.empty() && begin != end && m_path.back().point.coincides_with_epsilon(*begin)) {
        ++it;
    }
    if (m_z_offset)
        m_z_offset->insert(m_z_offset->end(), end - it, 0);
    while (it != end) {
        m_path.emplace_back(*it, 0, Geometry::ArcWelder::Orientation::Unknown);
        ++it;
    }
    assert(is_valid());
}

void ArcPolyline::append(const ArcPolyline &src)
{
    assert(this->empty() || this->is_valid());
    this->m_only_strait &= src.m_only_strait;
    if (m_path.empty()) {
        *this = src;
    } else if (src.m_path.front().point == this->m_path.back().point) {
        if (src.size() > 1) {
            bool epsilon_merge = false;
            if (!this->empty() && this->m_only_strait && src.m_only_strait) {
                epsilon_merge = this->size() == 2 && this->front().coincides_with_epsilon(this->back());
                if (!epsilon_merge) {
                    epsilon_merge = (this->back().coincides_with_epsilon(src.get_point(1)));
                    assert(!epsilon_merge || src.size() == 2);
                }
            }
            if (epsilon_merge) {
                m_path.back().point = src.get_point(1);
                if (src.size() > 2) {
                    const size_t next_size = this->size() + src.size() - 2;
                    if (this->m_z_offset || src.m_z_offset) {
                        coord_t joint_z = this->m_z_offset ? this->m_z_offset->back() : src.m_z_offset->front();
                        assert(!this->m_z_offset || !src.m_z_offset || this->m_z_offset->back() == src.m_z_offset->front());
                        std::vector<coord_t> &z_offsets = this->z_offsets_mutable();
                        z_offsets.resize(next_size, coord_t(0));
                        if (src.m_z_offset && src.m_z_offset->size() > 2) {
                            std::copy(src.m_z_offset->begin() + 2, src.m_z_offset->end(),
                                      z_offsets.begin() + m_path.size());
                        }
                        z_offsets[m_path.size() - 1] = joint_z;
                    }
                    m_path.reserve(next_size);
                    this->m_path.insert(this->m_path.end(), src.m_path.begin() + 2, src.m_path.end());
                    assert(next_size == m_path.size());
                }
            } else {
                assert(src.is_valid());
                const size_t next_size = m_path.size() + src.m_path.size() - 1;
                if (this->m_z_offset || src.m_z_offset) {
                    assert(!this->m_z_offset || !src.m_z_offset || this->m_z_offset->back() == src.m_z_offset->front());
                    std::vector<coord_t> &z_offsets = this->z_offsets_mutable();
                    z_offsets.resize(next_size, coord_t(0));
                    if (src.m_z_offset && src.m_z_offset->size() > 1) {
                        std::copy(src.m_z_offset->begin(), src.m_z_offset->end(),
                                  z_offsets.begin() + m_path.size() - 1);
                    }
                }
                m_path.reserve(next_size);
                // std::move(src.m_path.begin() + 1, src.m_path.end(), std::back_inserter(m_path));
                this->m_path.insert(this->m_path.end(), src.m_path.begin() + 1, src.m_path.end());
                assert(next_size == m_path.size());
            }
        }
    } else {
        // weird, are you sure you want to append it?
        assert(false);
        const size_t next_size = m_path.size() + src.m_path.size();
        if (this->m_z_offset || src.m_z_offset) {
            std::vector<coord_t> &z_offsets = this->z_offsets_mutable();
            z_offsets.resize(next_size, coord_t(0));
            if (src.m_z_offset) {
                std::copy(src.m_z_offset->begin(), src.m_z_offset->end(),
                          z_offsets.begin() + m_path.size());
            }
        }
        m_path.reserve(next_size);
        //std::move(src.m_path.begin(), src.m_path.end(), std::back_inserter(m_path));
        this->m_path.insert(this->m_path.end(), src.m_path.begin(), src.m_path.end());
        assert(next_size == m_path.size());
    }
    assert(is_valid());
}

void ArcPolyline::append(ArcPolyline &&src) {
    if (src.empty()) {
        return;
    }
    Point pt_back = src.back();
    assert(src.is_valid());
    assert(empty() || is_valid());
    this->m_only_strait &= src.m_only_strait;
    if (m_path.empty()) {
        *this = std::move(src);
    } else if (src.m_path.front().point == this->m_path.back().point) {
        if (src.size() > 1) {
            const size_t next_size = m_path.size() + src.m_path.size() - 1;
            if (this->m_z_offset || src.m_z_offset) {
                assert(!this->m_z_offset || !src.m_z_offset || this->m_z_offset->back() == src.m_z_offset->front());
                std::vector<coord_t> &z_offsets = this->z_offsets_mutable();
                z_offsets.resize(next_size, coord_t(0));
                if (src.m_z_offset && src.m_z_offset->size() > 1) {
                    std::copy(src.m_z_offset->begin(), src.m_z_offset->end(),
                              z_offsets.begin() + m_path.size() - 1);
                }
            }
            m_path.reserve(next_size);
            m_path.insert(m_path.end(), std::make_move_iterator(src.m_path.begin() + 1), std::make_move_iterator(src.m_path.end()));
            assert(is_valid());
            assert(next_size == m_path.size());
        }
    } else {
        // weird, are you sure you want to append it?
        assert(false);
        const size_t next_size = m_path.size() + src.m_path.size();
        if (this->m_z_offset || src.m_z_offset) {
            std::vector<coord_t> &z_offsets = this->z_offsets_mutable();
            z_offsets.resize(next_size, coord_t(0));
            if (src.m_z_offset) {
                std::copy(src.m_z_offset->begin(), src.m_z_offset->end(),
                          z_offsets.begin() + m_path.size());
            }
        }
        m_path.reserve(next_size);
        m_path.insert(m_path.end(), std::make_move_iterator(src.m_path.begin()), std::make_move_iterator(src.m_path.end()));
        assert(next_size == m_path.size());
    }
    assert(is_valid());
    assert(m_path.back().point == pt_back);
}

void ArcPolyline::append(const Geometry::ArcWelder::Segment &arc)
{
    assert(arc.linear() || !this->empty());
    assert(!arc.linear() || arc.orientation == Geometry::ArcWelder::Orientation::Unknown);
    this->m_path.push_back(arc);
    if (!arc.linear()) {
        this->m_only_strait = false;
    }
    if (m_z_offset)
        m_z_offset->push_back(0);
#ifdef _DEBUG
    if (this->m_path.size() > 1) {
        this->m_path.back().length = Geometry::ArcWelder::segment_length<coordf_t>(this->m_path[this->m_path.size() - 2], this->m_path.back());
        if (!arc.linear() && arc.center == Point()) {
            this->m_path.back().center =
                Geometry::ArcWelder::arc_center_scalar(this->m_path[this->m_path.size() - 2].point,
                                                       this->m_path.back().point, this->m_path.back().radius,
                                                       this->m_path.back().ccw());
        }
    }
#endif
    assert(is_valid());
}

void ArcPolyline::translate(const Vector &vector)
{
    for (auto &seg : m_path)
        seg.point += vector;
    assert(is_valid());
}
void ArcPolyline::rotate(double angle)
{
    double cos_angle = cos(angle), sin_angle = sin(angle);
    for (auto &seg : this->m_path) {
        double cur_x  = double(seg.point.x());
        double cur_y  = double(seg.point.y());
        seg.point.x() = coord_t(round(cos_angle * cur_x - sin_angle * cur_y));
        seg.point.y() = coord_t(round(cos_angle * cur_y + sin_angle * cur_x));
    }
    assert(is_valid());
}

int ArcPolyline::find_point(const Point &point) const
{
    for (size_t idx = 0; idx < this->m_path.size(); ++idx)
        if (m_path[idx].point == point)
            return int(idx);
    return -1; // not found
}

int ArcPolyline::find_point(const Point &point, coordf_t epsilon) const
{
    //fast track for perfect match
    const int fast_result = this->find_point(point);
    if (epsilon == 0 || fast_result >= 0)
        return fast_result;
    if (this->m_only_strait) {
        auto dist2_min = std::numeric_limits<double>::max();
        auto eps2      = epsilon * epsilon;
        int  idx_min   = -1;
        for (size_t idx = 0; idx < this->m_path.size(); ++idx) {
            double d2 = (this->m_path[idx].point - point).cast<double>().squaredNorm();
            if (d2 < dist2_min) {
                idx_min   = int(idx);
                dist2_min = d2;
            }
        }
        return dist2_min < eps2 ? idx_min : -1;
    } else {
        Geometry::ArcWelder::PathSegmentProjection result = Geometry::ArcWelder::point_to_path_projection(m_path, point);
        if (result.distance2 > epsilon * epsilon)
            return -1;
        if (result.segment_id + 1 == m_path.size()) {
            // i guess it's not possible?
            assert(false);
            return (this->m_path[result.segment_id].point - point).cast<double>().squaredNorm() < epsilon * epsilon ? result.segment_id : -1;
        } else {
            coordf_t first_dist = (this->m_path[result.segment_id].point - point).cast<double>().squaredNorm();
            coordf_t second_dist = (this->m_path[result.segment_id + 1].point - point).cast<double>().squaredNorm();
            int idx_min = first_dist < second_dist ? result.segment_id : (result.segment_id + 1);
            return std::min(first_dist, second_dist) <= epsilon * epsilon ? idx_min : -1;
        }
    }
}

coord_t ArcPolyline::z_offset(size_t idx) const {
    if (m_z_offset && idx < m_z_offset->size())
        return (*m_z_offset)[idx];
    return INVALID_COORD;
}


void ArcPolyline::set_z_offset(size_t idx, coord_t z_offset) {
    std::vector<coord_t> &z_offsets = this->z_offsets_mutable();
    assert(idx < z_offsets.size());
    z_offsets[idx] = z_offset;
#ifdef _DEBUG
    is_3D = true;
#endif
}

namespace ApiInternal {

namespace {

void make_linear(Geometry::ArcWelder::Segment &segment)
{
    segment.radius = 0.f;
    segment.orientation = Geometry::ArcWelder::Orientation::Unknown;
#ifdef _DEBUG
    segment.length = 0;
    segment.center = Point(0, 0);
#endif
}

void refresh_segment_debug(Geometry::ArcWelder::Path &path, size_t idx)
{
#ifdef _DEBUG
    if (idx > 0 && idx < path.size()) {
        path[idx].length = Geometry::ArcWelder::segment_length<coordf_t>(path[idx - 1], path[idx]);
        path[idx].center = path[idx].linear() ?
            Point(0, 0) :
            Geometry::ArcWelder::arc_center_scalar(path[idx - 1].point, path[idx].point,
                                                   path[idx].radius, path[idx].ccw());
    }
#else
    (void) path;
    (void) idx;
#endif
}

} // namespace

void ArcPolylineAccess::refresh_after_edit(ArcPolyline &polyline)
{
    polyline.m_only_strait = not_arc(polyline);
#ifdef _DEBUG
    for (size_t idx = 1; idx < polyline.m_path.size(); ++idx)
        refresh_segment_debug(polyline.m_path, idx);
#endif
    assert(polyline.m_path.size() < 2 || polyline.is_valid());
}

/* Publish valid derived arc metadata after one host-side path replacement. */
void ArcPolylineAccess::refresh_after_bulk_replace(ArcPolyline &polyline)
{
    refresh_after_edit(polyline);
}

bool ArcPolylineAccess::set_point(ArcPolyline &polyline, size_t idx, const Point &point)
{
    if (idx >= polyline.m_path.size())
        return false;

    polyline.m_path[idx].point = point;
    if (idx > 0)
        make_linear(polyline.m_path[idx]);
    if (idx + 1 < polyline.m_path.size())
        make_linear(polyline.m_path[idx + 1]);
    ArcPolylineAccess::refresh_after_edit(polyline);
    return true;
}

bool ArcPolylineAccess::insert_point(ArcPolyline &polyline, size_t idx, const Point &point)
{
    if (idx > polyline.m_path.size())
        return false;

    polyline.m_path.insert(polyline.m_path.begin() + idx,
                           Geometry::ArcWelder::Segment{point, 0.f, Geometry::ArcWelder::Orientation::Unknown});
    if (idx + 1 < polyline.m_path.size())
        make_linear(polyline.m_path[idx + 1]);
    if (polyline.m_z_offset)
        polyline.m_z_offset->insert(polyline.m_z_offset->begin() + idx, coord_t(0));
    ArcPolylineAccess::refresh_after_edit(polyline);
    return true;
}

bool ArcPolylineAccess::remove_point(ArcPolyline &polyline, size_t idx)
{
    if (idx >= polyline.m_path.size())
        return false;

    polyline.m_path.erase(polyline.m_path.begin() + idx);
    if (idx < polyline.m_path.size())
        make_linear(polyline.m_path[idx]);
    if (polyline.m_z_offset)
        polyline.m_z_offset->erase(polyline.m_z_offset->begin() + idx);
    ArcPolylineAccess::refresh_after_edit(polyline);
    return true;
}

bool ArcPolylineAccess::set_segment(ArcPolyline &polyline,
                                    size_t segment_idx,
                                    const Point &point_a,
                                    const Point &point_b,
                                    float radius,
                                    Geometry::ArcWelder::Orientation orientation)
{
    if (segment_idx + 1 >= polyline.m_path.size())
        return false;
    if ((radius == 0.f && orientation != Geometry::ArcWelder::Orientation::Unknown) ||
        (radius != 0.f && orientation == Geometry::ArcWelder::Orientation::Unknown))
        return false;

    polyline.m_path[segment_idx].point = point_a;
    polyline.m_path[segment_idx + 1].point = point_b;
    if (segment_idx > 0)
        make_linear(polyline.m_path[segment_idx]);
    polyline.m_path[segment_idx + 1].radius = radius;
    polyline.m_path[segment_idx + 1].orientation = orientation;
    if (segment_idx + 2 < polyline.m_path.size())
        make_linear(polyline.m_path[segment_idx + 2]);

    ArcPolylineAccess::refresh_after_edit(polyline);
    return true;
}

void ArcPolylineAccess::clear_z_offsets(ArcPolyline &polyline)
{
    polyline.m_z_offset.reset();
}

} // namespace ApiInternal

bool ArcPolyline::at_least_length(distf_t length) const
{
    for (size_t i = 1; length > 0 && i < m_path.size(); ++ i)
        length -= Geometry::ArcWelder::segment_length<double>(m_path[i - 1], m_path[i]);
    return length <= 0;
}

//for seams
std::pair<int, Point> ArcPolyline::foot_pt(const Point &pt) const
{
    if (m_path.size() < 2)
        return std::make_pair(-1, Point(0, 0));

    if (this->m_only_strait) {
        auto  d2_min = std::numeric_limits<double>::max();
        Point foot_pt_min;
        size_t foot_idx_min = 0;
        Point prev    = m_path.front().point;
        for (size_t idx = 1; idx < m_path.size(); ++idx) {
            Point foot_pt;
            if (double d2 = Line::distance_to_squared_abp(prev, m_path[idx].point, pt, &foot_pt); d2 < d2_min) {
                d2_min      = d2;
                foot_pt_min = foot_pt;
                foot_idx_min = idx;
            }
            prev = m_path[idx].point;
        }
        return std::make_pair(int(foot_idx_min) - 1, foot_pt_min);
    } else {
        Geometry::ArcWelder::PathSegmentProjection result = Geometry::ArcWelder::point_to_path_projection(m_path, pt);
        // check that if last point, then it's really the last point
        //assert(result.segment_id + 1 < m_path.size() ||
        //       (m_path.back().point.distance_to(pt) - std::sqrt(result.distance2)) < SCALED_EPSILON);
        // else check that if on strait segment, it's proj into it.
        assert(result.segment_id + 1 == m_path.size() || !m_path[result.segment_id + 1].linear() ||
               result.point.distance_to(
                   result.point.projection_onto(m_path[result.segment_id].point, m_path[result.segment_id + 1].point)) < SCALED_EPSILON);
        // else check that it's on the arc
        assert(result.segment_id + 1 == m_path.size() || m_path[result.segment_id + 1].linear()
            || m_path[result.segment_id + 1].point == result.point || m_path[result.segment_id].point == result.point
            || Geometry::ArcWelder::point_to_path_projection({m_path[result.segment_id], m_path[result.segment_id + 1]},
                                                                      result.point,
                                                                      (result.distance2) + SCALED_EPSILON * 2).point.distance_to(result.point) < SCALED_EPSILON);
        // if on strait segment, then no center, if on arc, then there is a center. (unless it's the first or last point)
        if (result.segment_id > 0 && result.segment_id + 1 < m_path.size()) {
            bool has_center = result.center != Point(0, 0);
            bool has_radius = m_path[result.segment_id + 1].radius != 0;
            assert((has_center && has_radius) || (!has_center && !has_radius)
                || m_path[result.segment_id].point == result.point
                || (m_path[result.segment_id+1].point == result.point && result.segment_id+2 == m_path.size()));
        }
        // arc: projection
        return std::make_pair(int(result.segment_id), result.point);
    }
}

void ArcPolyline::pop_front()
{
    assert(m_only_strait);
    assert(m_path.size() > 2);
    m_path.erase(m_path.begin());
    if (!m_path.empty()) {
        m_path.front().radius = 0.f;
        m_path.front().orientation = Geometry::ArcWelder::Orientation::Unknown;
    }
    if (!m_only_strait)
        m_only_strait = not_arc(*this);
    if (m_z_offset)
        m_z_offset->erase(m_z_offset->begin());
    assert(is_valid());
}

void ArcPolyline::pop_back()
{
    assert(m_only_strait);
    assert(m_path.size() > 2);
    m_path.pop_back();
    if (!m_only_strait)
        m_only_strait = not_arc(*this);
    if (m_z_offset)
        m_z_offset->pop_back();
    assert(is_valid());
}

void ArcPolyline::clip_start(distf_t dist)
{
    // Geometry::ArcWelder::clip_start is doing reverse +clip_end anyway.
    reverse();
    clip_end(dist);
    reverse();
}

coord_t interpolate(coord_t z1, coord_t z2, double ratio) {
    return coord_t(std::llround(double(z1) + double(z2 - z1) * ratio));
}

void ArcPolyline::clip_end(distf_t dist)
{
    distf_t old_last_dist = Geometry::ArcWelder::clip_end_old_size(m_path, dist);
    if (!m_only_strait)
        m_only_strait = not_arc(*this);
    if (m_z_offset) {
        m_z_offset->resize(m_path.size());
        if (m_z_offset->size() > 1 && old_last_dist > EPSILON) {
            distf_t new_dist = Geometry::ArcWelder::segment_length<distf_t>(m_path[m_path.size() - 2], m_path.back());
            m_z_offset->back() = interpolate((*m_z_offset)[m_z_offset->size() - 2], m_z_offset->back(),
                                             new_dist / old_last_dist);
        }
    }
    assert(is_valid());
}

void ArcPolyline::split_at(distf_t distance, ArcPolyline &p1, ArcPolyline &p2) const
{
    assert(p1.empty());
    assert(p2.empty());
    if (m_path.empty()) return;
    assert(distance > SCALED_EPSILON);
    if (distance < SCALED_EPSILON) return;
    assert(this->is_valid());
    p1.m_path.push_back(m_path.front());
    if (m_z_offset)
        p1.z_offsets_raw_mutable().push_back(m_z_offset->front());
#ifdef _DEBUG
    coordf_t length_tot_split = 0;
#endif
    size_t idx = 1;
    while(distance > 0 && idx < m_path.size()) {
        const Geometry::ArcWelder::Segment current = m_path[idx];
        if (current.linear()) {
            // Linear segment
            Vec2d  v    = (current.point - p1.back()).cast<double>();
            double lsqr = v.squaredNorm();
            if (lsqr > sqr(distance)) {
#ifdef _DEBUG
                length_tot_split = current.length;
#endif
                Point split_point = p1.back() + Point::round(v * (distance / sqrt(lsqr)));
                p1.m_path.push_back({split_point, 0, Geometry::ArcWelder::Orientation::Unknown});
                p2.m_path.push_back({split_point, 0, Geometry::ArcWelder::Orientation::Unknown});
                p2.m_path.push_back(current);
                if (m_z_offset) {
                    std::vector<coord_t> &p1_z_offsets = p1.z_offsets_raw_mutable();
                    std::vector<coord_t> &p2_z_offsets = p2.z_offsets_raw_mutable();
                    p1_z_offsets.push_back(interpolate(p1_z_offsets.back(), (*m_z_offset)[idx], distance / sqrt(lsqr)));
                    p2_z_offsets.push_back(p1_z_offsets.back());
                    p2_z_offsets.push_back((*m_z_offset)[idx]);
                }
                // Length to go is zero.
                distance = 0;
            } else {
                p1.m_path.push_back(current);
                if (m_z_offset)
                    p1.z_offsets_raw_mutable().push_back((*m_z_offset)[idx]);
                distance -= sqrt(lsqr);
                if (distance < SCALED_EPSILON) {
                    p2.m_path.push_back(current);
                    if (m_z_offset)
                        p2.z_offsets_raw_mutable().push_back((*m_z_offset)[idx]);
                    distance = 0;
                }
            }
        } else {
#ifdef _DEBUG
                length_tot_split = current.length;
                assert(length_tot_split > 0);
                Point startp = p1.back();
#endif
            // Circular segment
            //double angle = Geometry::ArcWelder::arc_angle(path.back().point.cast<double>(), last.point.cast<double>(), last.radius);
            double angle = Geometry::ArcWelder::arc_angle(p1.back().cast<double>(), current.point.cast<double>(), current.radius);
            assert(angle > 0);
            //double len   = std::abs(last.radius) * angle;
            double len   = std::abs(current.radius) * angle;
            if (len > distance) {
                assert(distance > 0);
                // Rotate the segment end point in reverse towards the start point.
                if (!current.ccw())
                    angle *= -1.;
                const Vec2d center = Geometry::ArcWelder::arc_center(p1.back().cast<double>(), current.point.cast<double>(), double(current.radius), current.ccw());
                const Point split_point = p1.back().rotated(angle * (distance / len), Point::round(center));
                p1.m_path.push_back({split_point, current.radius, current.orientation });
                p2.m_path.push_back({split_point, 0, Geometry::ArcWelder::Orientation::Unknown});
                p2.m_path.push_back(current);
                // the arc is now shorter, if radius negative then is was using the big one. In this case, check if the segment isn't now the shorter one.
                if (current.radius < 0) {
                    assert(std::abs(angle) > PI);
                    int nb_reverse = 0;
                    if (std::abs(angle) * (distance / len) < PI) {
                        p1.m_path.back().radius = (-p1.m_path.back().radius);
                        nb_reverse++;
                        assert(p1.m_path.back().radius > 0);
                    }
                    if (std::abs(angle) * (1- (distance / len)) < PI) {
                        p2.m_path[1].radius = (-p2.m_path[1].radius);
                        nb_reverse++;
                        assert(p2.m_path[1].radius > 0);
                    }
                    assert(nb_reverse > 0);
                }
                if (m_z_offset) {
                    std::vector<coord_t> &p1_z_offsets = p1.z_offsets_raw_mutable();
                    std::vector<coord_t> &p2_z_offsets = p2.z_offsets_raw_mutable();
                    p1_z_offsets.push_back(interpolate(p1_z_offsets.back(), (*m_z_offset)[idx], distance / len));
                    p2_z_offsets.push_back(p1_z_offsets.back());
                    p2_z_offsets.push_back((*m_z_offset)[idx]);
                }
#ifdef _DEBUG
                Point almost_current = startp.rotated(angle, Point::round(center));
                Point almost_current2 = startp.rotated(angle, current.center);
                assert(almost_current.coincides_with_epsilon(current.point));
                assert(p1.back() == split_point);
                double part1 = Geometry::ArcWelder::arc_length(startp, split_point, double(p1.m_path.back().radius));
                double part1b = Geometry::ArcWelder::arc_length(startp, split_point, double(-p1.m_path.back().radius));
                assert(current.point == p2.m_path[1].point);
                double part2 = Geometry::ArcWelder::arc_length(split_point, current.point, double(p2.m_path[1].radius));
                double tot = Geometry::ArcWelder::arc_length(startp, current.point, double(current.radius));
                assert(is_approx(part1 + part2, tot, 1. * SCALED_EPSILON));
#endif
                // Length to go is zero.
                distance = 0;
            } else {
                p1.m_path.push_back(current);
                if (m_z_offset)
                    p1.z_offsets_raw_mutable().push_back((*m_z_offset)[idx]);
                distance -= len;
                if (distance < SCALED_EPSILON) {
                    if (p2.m_path.empty()) {
                        // if first point to add, it's a strating point, not an arc.
                        p2.m_path.push_back(current.point);
                    } else {
                        p2.m_path.push_back(current);
                    }
                    if (m_z_offset)
                        p2.z_offsets_raw_mutable().push_back((*m_z_offset)[idx]);
                    distance = 0;
                }
            }
        }
        //increment
        ++idx;
    }
    assert(!p2.empty());
    assert(p1.m_path.back().point == p2.m_path.front().point);
    //now fill p2
    while (idx < m_path.size()) {
        assert(!p2.m_path.back().point.coincides_with_epsilon(m_path[idx].point));
        p2.m_path.push_back(m_path[idx]);
        if (m_z_offset)
            p2.z_offsets_raw_mutable().push_back((*m_z_offset)[idx]);
        // increment
        ++idx;
    }
    assert(!p2.empty());
    if (p2.back() != back()) {
        if (p2.size() == 1 || !p2.back().coincides_with_epsilon(back())) {
            p2.m_path.push_back(back());
            if (m_z_offset)
                p2.z_offsets_raw_mutable().push_back(m_z_offset->back());
        } else {
            // even with arc, the difference is not measurable (less than epsilon)
            p2.set_back(back());
            if (m_z_offset)
                p2.z_offsets_raw_mutable().back() = m_z_offset->back();
        }
    }
    //check if the last p1 segment is long enough
    if (p1.size() > 3 && p1.back().distance_to_square(p1.get_point(p1.size()-2)) < SCALED_EPSILON * SCALED_EPSILON) {
        //to short of a segment, move the previous point (even if arc, should be short enough of a move)
        p1.m_path[p1.size()-2].point = p1.back();
        p1.m_path.pop_back();
        if (p1.m_z_offset) {
            (*p1.m_z_offset)[p1.size()-2] = p1.m_z_offset->back();
            p1.m_z_offset->pop_back();
        }
    }
    if (has_arc()) {
        p1.m_only_strait = not_arc(p1);
        p2.m_only_strait = not_arc(p2);
    }
#ifdef _DEBUG
    assert(p1.m_path.back().point == p2.m_path[0].point);
    if (p1.size() > 1 && p2.size() > 1 && (!p1.m_path.back().linear() || !p2.m_path[1].linear()) ) {
        assert(std::abs(p1.m_path.back().radius) == std::abs(p2.m_path[1].radius));
        assert(p1.m_path.back().length ==0 && p2.m_path[1].length > 0);
        coordf_t length_old_p1 = p1.m_path.back().length;
        coordf_t length_old_p2 = p2.m_path[1].length;
        p1.m_path.back().length = Geometry::ArcWelder::segment_length<coordf_t>(p1.m_path[p1.m_path.size()-2], p1.m_path.back());
        p1.m_path.back().center = Geometry::ArcWelder::arc_center_scalar(p1.m_path[p1.m_path.size()-2].point, p1.m_path.back().point, p1.m_path.back().radius, p1.m_path.back().ccw());
        p2.m_path[1].length = Geometry::ArcWelder::segment_length<coordf_t>(p2.m_path[0], p2.m_path[1]);
        p2.m_path[1].center = Geometry::ArcWelder::arc_center_scalar(p2.m_path[0].point, p2.m_path[1].point, p2.m_path[1].radius, p2.m_path[1].ccw());
        assert(is_approx(length_tot_split, p1.m_path.back().length + p2.m_path[1].length, 1. * SCALED_EPSILON));
    }
    for (size_t i = 1; i < p1.m_path.size(); i++) {
        if(p1.m_path[i].radius)
            assert(is_approx(Geometry::ArcWelder::segment_length<coordf_t>(p1.m_path[i-1], p1.m_path[i]), p1.m_path[i].length, EPSILON));
    }
    for (size_t i = 1; i < p2.m_path.size(); i++) {
        if(p2.m_path[i].radius)
            assert(is_approx(Geometry::ArcWelder::segment_length<coordf_t>(p2.m_path[i-1], p2.m_path[i]), p2.m_path[i].length, EPSILON));
    }
#endif
    assert(p1.size() == 1 || p1.is_valid());
    assert(p2.size() == 1 || p2.is_valid());
    assert(is_approx(this->length(), p1.length() + p2.length(), coordf_t(SCALED_EPSILON)));
    assert(!m_z_offset || p1.m_z_offset->back() == p2.m_z_offset->front());
}

void ArcPolyline::split_at(Point &point, ArcPolyline &p1, ArcPolyline &p2) const
{
    if (this->m_path.empty())
        return;

    if (this->size() < 2 || this->m_path.back().point.coincides_with_epsilon(point)) {
        p1 = *this;
        p2.clear();
        p2.append(point);
        if (m_z_offset)
            p2.z_offsets_raw_mutable().push_back(m_z_offset->back());
        return;
    }
    assert(this->is_valid());

    if (this->m_path.front().point.coincides_with_epsilon(point)) {
        p1.clear();
        p1.append(point);
        if (m_z_offset)
            p1.z_offsets_raw_mutable().push_back(m_z_offset->front());
        p2 = *this;
        return;
    }

    // 0 judge whether the point is on the polyline
    int index = this->find_point(point);
    if (index != -1) {
        // BBS: the split point is on the polyline, then easy
        split_at_index(index, p1, p2);
        point = p1.is_valid() ? p1.back() : p2.front();
        return;
    }

    //find the line to split at
    Geometry::ArcWelder::PathSegmentProjection result = Geometry::ArcWelder::point_to_path_projection(m_path, point);
    assert(result.segment_id + 1 < this->m_path.size());
    assert(result.center != Point(0, 0) || this->m_path[result.segment_id + 1].linear()); // if no radius, then no center
    assert(result.center == Point(0, 0) || !this->m_path[result.segment_id + 1].linear()); // if center defined, then the radius isn't null
    // the point to add is between m_path[result.segment_id] and m_path[result.segment_id + 1]
    //split and update point
    p1.clear();
    p1.m_path.reserve(result.segment_id + 2);
    p1.m_path.insert(p1.m_path.begin(), this->m_path.begin(), this->m_path.begin() + result.segment_id + 2);
    p1.m_path.back().point = result.point;
    if (p1.m_path.back().radius < 0) {
        //check if the direction isn't reversed because of the smaller angle
        Point previous_center = Geometry::ArcWelder::arc_center_scalar(this->m_path[result.segment_id].point,
                                                               this->m_path[result.segment_id + 1].point,
                                                               this->m_path[result.segment_id + 1].radius,
                                                               this->m_path[result.segment_id + 1].ccw());
        assert(p1.m_path.size() == result.segment_id + 2);
        Point new_center = Geometry::ArcWelder::arc_center_scalar(p1.m_path[result.segment_id].point,
                                                               p1.m_path[result.segment_id + 1].point,
                                                               p1.m_path[result.segment_id + 1].radius,
                                                               p1.m_path[result.segment_id + 1].ccw());
        Point new_center2 = Geometry::ArcWelder::arc_center_scalar(p1.m_path[result.segment_id].point,
                                                               p1.m_path[result.segment_id + 1].point,
                                                               p1.m_path[result.segment_id + 1].radius,
                                                               !p1.m_path[result.segment_id + 1].ccw());
        if (previous_center.distance_to_square(new_center) > previous_center.distance_to_square(new_center2)) {
            p1.m_path.back().radius = (-p1.m_path.back().radius);
        }
    }
#ifdef _DEBUG
    p1.m_path.back().length = Geometry::ArcWelder::segment_length<coordf_t>(p1.m_path[p1.m_path.size()-2], p1.m_path.back());
#endif
    p1.m_only_strait       = not_arc(p1);
    coord_t split_z = INVALID_COORD;
    if (m_z_offset) {
        assert(p1.m_path.size() > 1);
        std::vector<coord_t> &p1_z_offsets = p1.z_offsets_raw_mutable();
        p1_z_offsets.reserve(result.segment_id + 2);
        p1_z_offsets.insert(p1_z_offsets.end(), m_z_offset->begin(), m_z_offset->begin() + result.segment_id + 2);
        distf_t old_dist = Geometry::ArcWelder::segment_length<distf_t>(this->m_path[result.segment_id], this->m_path[result.segment_id + 1]);
        distf_t new_dist = Geometry::ArcWelder::segment_length<distf_t>(this->m_path[result.segment_id], p1.m_path.back());
        split_z = interpolate((*m_z_offset)[result.segment_id], (*m_z_offset)[result.segment_id + 1],
                                new_dist / old_dist);
        p1_z_offsets.back() = split_z;
    }

    p2.clear();
    p2.m_path.reserve(this->size() - result.segment_id);
    p2.m_path.insert(p2.m_path.begin(), this->m_path.begin() + result.segment_id, this->m_path.end());
    p2.m_path.front().point  = result.point;
    p2.m_path.front().radius = 0; // first point can't be an arc
    p2.m_path.front().orientation = Geometry::ArcWelder::Orientation::Unknown;
    if (p2.m_path[1].radius < 0) {
        Point previous_center = Geometry::ArcWelder::arc_center_scalar(this->m_path[result.segment_id].point,
                                                               this->m_path[result.segment_id + 1].point,
                                                               this->m_path[result.segment_id + 1].radius,
                                                               this->m_path[result.segment_id + 1].ccw());
        assert(p1.m_path.size() > 1);
        Point new_center = Geometry::ArcWelder::arc_center_scalar(p2.m_path[0].point,
                                                               p2.m_path[1].point,
                                                               p2.m_path[1].radius,
                                                               p2.m_path[1].ccw());
        Point new_center2 = Geometry::ArcWelder::arc_center_scalar(p2.m_path[0].point,
                                                               p2.m_path[1].point,
                                                               p2.m_path[1].radius,
                                                               !p2.m_path[1].ccw());
        if (previous_center.distance_to_square(new_center) > previous_center.distance_to_square(new_center2)) {
            p2.m_path[1].radius = (-p2.m_path[1].radius);
        }
    }
#ifdef _DEBUG
    p2.m_path[1].length = Geometry::ArcWelder::segment_length<coordf_t>(p2.m_path[0], p2.m_path[1]);
#endif
    p2.m_only_strait         = not_arc(p2);
    if (m_z_offset) {
        assert(p2.m_path.size() > 1);
        std::vector<coord_t> &p2_z_offsets = p2.z_offsets_raw_mutable();
        p2_z_offsets.reserve(this->size() - result.segment_id);
        p2_z_offsets.insert(p2_z_offsets.end(), m_z_offset->begin() + result.segment_id, m_z_offset->end());
        distf_t old_dist = Geometry::ArcWelder::segment_length<distf_t>(m_path[result.segment_id], m_path[result.segment_id + 1]);
        distf_t new_dist = Geometry::ArcWelder::segment_length<distf_t>(p2.m_path.front(), m_path[result.segment_id + 1]);
        assert(split_z != INVALID_COORD);
        p2_z_offsets.front() = split_z;
    }

    point = result.point;

    if (p1.m_path[p1.size() - 2].point.coincides_with_epsilon(p1.m_path.back().point)) {
        if (p1.m_path.back().linear() ||
            Geometry::ArcWelder::arc_length(p1.m_path[p1.size() - 2].point, p1.m_path.back().point,
                                            p1.m_path.back().radius)) {
            // too close to each other
            if (p1.m_path.size() == 2) {
                if (!p2.empty()) {
                    // clear first polyline
                    p2.set_front(p1.front());
                    if (m_z_offset)
                        p2.m_z_offset->front() = p1.m_z_offset->front();
                    p1.clear();
                } else {
                    assert(false);
                }
            } else {
                // remove last segment, keep last point
                p1.m_path[p1.size() - 2].point = p1.m_path.back().point;
                p1.m_path.pop_back();
                if (m_z_offset) {
                    (*p1.m_z_offset)[p1.m_z_offset->size() - 2] = p1.m_z_offset->back();
                    p1.m_z_offset->pop_back();
                }
            }
        }
    }
    if (p2.m_path.front().point.coincides_with_epsilon(p2.m_path[1].point)) {
        if (p2.m_path[1].linear() ||
            Geometry::ArcWelder::arc_length(p2.m_path.front().point, p2.m_path[1].point,
                                            p2.m_path[1].radius)) {
            // too close to each other
            if (p2.m_path.size() == 2) {
                if (!p2.empty()) {
                    // clear first polyline
                    p1.set_back(p2.back());
                    if (m_z_offset)
                        p1.m_z_offset->back() = p2.m_z_offset->back();
                    p2.clear();
                } else {
                    assert(false);
                }
            } else {
                // remove first segment, keep first point
                p2.m_path.erase(p2.m_path.begin() + 1);
                if (m_z_offset)
                    p2.m_z_offset->erase(p2.m_z_offset->begin() + 1);
            }
        }
    }

    assert(p1.size() == 1 || p1.is_valid());
    assert(p2.size() == 1 || p2.is_valid());
    assert(p1.front() == this->front());
    assert(p2.back() == this->back());
    assert(!m_z_offset || p1.m_z_offset->back() == p2.m_z_offset->front());
}

bool ArcPolyline::split_at_index(const size_t index, ArcPolyline &p1, ArcPolyline &p2) const
{
    if (index >= this->size())
        return false;

    if (index == 0) {
        p1.clear();
        p1.append(this->front());
        if (m_z_offset)
            p1.z_offsets_raw_mutable().push_back(m_z_offset->front());
        p2 = *this;
    } else if (index == this->size() - 1) {
        p1 = *this;
        p2.clear();
        p2.m_path.insert(p2.m_path.begin(), Geometry::ArcWelder::Segment{this->back(), 0.f, Geometry::ArcWelder::Orientation::Unknown});
        if (m_z_offset)
            p2.z_offsets_raw_mutable().push_back(m_z_offset->back());
    } else {
        p1.clear();
        p1.m_path.reserve(p1.m_path.size() + index + 1);
        p1.m_path.insert(p1.m_path.end(), this->m_path.begin(), this->m_path.begin() + index + 1);
        p1.m_only_strait = not_arc(p1);

        p2.clear();
        p2.m_path.reserve(p2.m_path.size() + this->size() - index);
        p2.m_path.insert(p2.m_path.begin(), this->m_path.begin() + index, this->m_path.end());
        p2.m_path.front().radius = 0; // first point can't be an arc
        p2.m_path.front().orientation = Geometry::ArcWelder::Orientation::Unknown;
        p2.m_only_strait         = not_arc(p2);

        if (m_z_offset) {
            std::vector<coord_t> &p1_z_offsets = p1.z_offsets_raw_mutable();
            std::vector<coord_t> &p2_z_offsets = p2.z_offsets_raw_mutable();
            p1_z_offsets.reserve(index + 1);
            p1_z_offsets.insert(p1_z_offsets.end(), m_z_offset->begin(), m_z_offset->begin() + index + 1);
            p2_z_offsets.reserve(this->size() - index);
            p2_z_offsets.insert(p2_z_offsets.begin(), m_z_offset->begin() + index, m_z_offset->end());
        }
    }
    assert(p1.size() == 1 || p1.is_valid());
    assert(p2.size() == 1 || p2.is_valid());
    assert(p1.front() == this->front());
    assert(p2.back() == this->back());
    assert(!m_z_offset || p1.m_z_offset->back() == p2.m_z_offset->front());
    return true;
}

//TODO: find a way to avoid duplication of get_point_from_end / get_point_from_begin
Point ArcPolyline::get_point_from_begin(distf_t distance) const {
    size_t idx = 1;
    while (distance > 0 && idx < m_path.size()) {
        const Geometry::ArcWelder::Segment last = m_path[idx - 1];
        const Geometry::ArcWelder::Segment current = m_path[idx];
        if (current.linear()) {
            // Linear segment
            Vec2d  v    = (current.point - last.point).cast<double>();
            double lsqr = v.squaredNorm();
            if (lsqr >= coord_sqr(distance)) {
                // Length to go is zero.
                return last.point + Point::round(v * (distance / sqrt(lsqr)));
            }
            distance -= sqrt(lsqr);
        } else {
            // Circular segment
            double angle = Geometry::ArcWelder::arc_angle(last.point.cast<double>(), current.point.cast<double>(), current.radius);
            double len   = std::abs(current.radius) * angle;
            if (len >= distance) {
                // Rotate the segment end point towards the current point.
                if (current.ccw())
                    angle *= -1.;
                return last.point.rotated( -angle * (distance / len), Point::round(
                        Geometry::ArcWelder::arc_center(last.point.cast<double>(), current.point.cast<double>(), double(current.radius), current.ccw())));
            }
            distance -= len;
        }
        ++idx;
    }

    // Return remaining distance to go.
    assert(distance >= 0);
    return m_path[idx - 1].point;
}

Point ArcPolyline::get_point_from_end(distf_t distance) const {
    size_t idx = m_path.size() - 1;
    while (distance > 0 && idx > 0) {
        const Geometry::ArcWelder::Segment last = m_path[idx];
        const Geometry::ArcWelder::Segment current = m_path[idx - 1];
        if (last.linear()) {
            // Linear segment
            Vec2d  v    = (current.point - last.point).cast<double>();
            double lsqr = v.squaredNorm();
            if (lsqr >= coord_sqr(distance)) {
                // Length to go is zero.
                return last.point + Point::round(v * (distance / sqrt(lsqr)));
            }
            distance -= sqrt(lsqr);
        } else {
            // Circular segment
            double angle = Geometry::ArcWelder::arc_angle(current.point.cast<double>(), last.point.cast<double>(), last.radius);
            double len   = std::abs(last.radius) * angle;
            if (len >= distance) {
                // Rotate the segment end point in reverse towards the start point.
                if (last.ccw())
                    angle *= -1.;
                return last.point.rotated(angle * (distance / len), Point::round(
                        Geometry::ArcWelder::arc_center(current.point.cast<double>(), last.point.cast<double>(), double(last.radius), last.ccw())));
            }
            distance -= len;
        }
        --idx;
    }

    // Return remaining distance to go.
    assert(distance >= 0);
    return m_path[idx].point;
}

void ArcPolyline::set_front(const Point &p) {
    assert(!m_path.empty());
    if (m_path.size() > 1 && m_path.front().point != p &&
        (m_path[1].radius != 0 || m_path[1].orientation != Geometry::ArcWelder::Orientation::Unknown)) {
        // should have been discretized before
        assert(false);
        m_path[1].radius = 0.f;
        m_path[1].orientation = Geometry::ArcWelder::Orientation::Unknown;
    }
    m_path.front().point = p;
    assert(is_valid());
}

void ArcPolyline::set_back(const Point &p) {
    assert(!m_path.empty());
    if (m_path.size() > 1 && m_path.back().point != p &&
        (m_path.back().radius != 0 || m_path.back().orientation != Geometry::ArcWelder::Orientation::Unknown)) {
        // should have been discretized before
        assert(false);
        m_path.back().radius = 0.f;
        m_path.back().orientation = Geometry::ArcWelder::Orientation::Unknown;
    }
    m_path.back().point = p;
    assert(is_valid());
}

Polyline ArcPolyline::to_polyline(coord_t deviation/*=0*/) const {
    Polyline poly_out;
    if (!empty()) {
        assert(m_path.front().linear());
        if (deviation > 0 || m_only_strait || m_path.size() < 2) {
            assert(m_only_strait == not_arc(*this));
            for (const Geometry::ArcWelder::Segment &seg : m_path)
                if (seg.linear() || m_only_strait) {
                    poly_out.append(seg.point);
                } else if(deviation == 0) {
                    assert(false);
                    // add more than one point if angle > 22.5°
                    double angle = Geometry::ArcWelder::arc_angle(poly_out.back().cast<double>(), seg.point.cast<double>(), seg.radius);
                    assert(angle > 0);
                    if (angle > PI / 8) {
                        size_t nb_steps = 1 + angle / (PI / 8);
                        Points pts = Geometry::ArcWelder::arc_discretize(poly_out.back(), seg.point, seg.radius,
                                                                            seg.orientation == Geometry::ArcWelder::Orientation::CCW,
                                                                            nb_steps);
                        if (!pts.empty() && !poly_out.empty() && pts.front().coincides_with_epsilon(poly_out.back())) {
                            poly_out.append(pts.begin() + 1, pts.end());
                        } else {
                            poly_out.append(pts);
                        }
                    } else {
                        poly_out.append(seg.point);
                    }
                } else {
                    Points pts = Geometry::ArcWelder::arc_discretize(poly_out.back(), seg.point, seg.radius,
                                                            seg.orientation == Geometry::ArcWelder::Orientation::CCW,
                                                            (double) deviation);
                    if (!pts.empty() && !poly_out.empty() && pts.front().coincides_with_epsilon(poly_out.back())) {
                        poly_out.append(pts.begin() + 1, pts.end());
                    } else {
                        poly_out.append(pts);
                    }
                }
        } else {

            for (const Geometry::ArcWelder::Segment &seg : m_path) {
                if (seg.linear()) {
                    assert(poly_out.empty() || !poly_out.back().coincides_with_epsilon(seg.point));
                    poly_out.append(seg.point);
                } else {
                    // add more than one point if angle > 22.5°
                    double angle = Geometry::ArcWelder::arc_angle(poly_out.back().cast<double>(),
                                                                  seg.point.cast<double>(), seg.radius);
                    assert(angle > 0);
                    if (angle > PI / 8) {
                        size_t nb_steps = 1 + angle / (PI / 8);
                        Points pts = Geometry::ArcWelder::arc_discretize(poly_out.back(), seg.point, seg.radius,
                                                                            seg.orientation == Geometry::ArcWelder::Orientation::CCW,
                                                                            nb_steps);
                        if (!pts.empty() && !poly_out.empty() && pts.front().coincides_with_epsilon(poly_out.back())) {
                            poly_out.append(pts.begin() + 1, pts.end());
                        } else {
                            poly_out.append(pts);
                        }
                    } else {
                        poly_out.append(seg.point);
                    }
                }
            }
        }
    }
    //for (int i = 1; i < poly_out.points.size(); ++i)
    //    assert(!poly_out.points[i - 1].coincides_with_epsilon(poly_out.points[i]));
    return poly_out;
}

Geometry::ArcWelder::Path ArcPolyline::_from_polyline(const Points &poly)
{
    Geometry::ArcWelder::Path path;
    for (const Point &point : poly)
        path.emplace_back(std::move(point), 0, Geometry::ArcWelder::Orientation::Unknown);
    return path;
}

Geometry::ArcWelder::Path ArcPolyline::_from_polyline(std::initializer_list<Point> poly)
{
    Geometry::ArcWelder::Path path;
    for (const Point &point : poly)
        path.emplace_back(std::move(point), 0, Geometry::ArcWelder::Orientation::Unknown);
    return path;
}

//TODO: unit tests
// it will return the size of the buffer still used. It will try to not use d more than half, unless buffer_init < 0, then it will try to not use any at the end.
//TODO: improvement: instead of watching at three point -> deleting the center (2 instead of 3), look at four -> add center of two center ones -> keep new & start & end (3 instead of 4)
// choose between both based on the result: is the deviation better? is path less stuttery? 
int ArcPolyline::simplify_straits(coordf_t min_tolerance,
                                   coordf_t fl_min_point_distance,
                                   coordf_t mean_dist_per_line,
                                   const int buffer_size,
    const int buffer_init)
{
    assert(is_valid());
    assert(!m_z_offset);
    return 0;
    m_z_offset.reset();
    // incentive to remove odds points
    float squew[] = { 1, 0.94f, 0.98f, 0.96f, 0.99f, 0.93f, 0.97f, 0.95f};

    //use a window of buffer size.
    const coord_t min_point_distance = fl_min_point_distance;
    int current_buffer_size = 0;
    int max_buffer_size = buffer_size - buffer_init;
    const int max_buffer_size_end = buffer_init < 0 ? 0 : buffer_size / 2;
    const int max_buffer_size_start = std::max(1, buffer_init);
    size_t idx_begin = 1;

    coord_t buffer_length = 0;
    coord_t min_buffer_length = 0;
    std::deque<uint8_t> arc; // 0= strait, 1 = arc (don't touch), 2 = point before/after arc (don't touch)
    std::deque<size_t> idxs;
    std::deque<coord_t> line_length;
    std::deque<float> weights;
    //weight of poitns in the current window (idx%size()), to know which one to remove.
    std::vector<size_t> erased;

    idxs.push_back(0);
    for (size_t idx_end = 1; idx_end < this->m_path.size(); ++idx_end) {
        assert(current_buffer_size + 1 == idxs.size());
        assert(current_buffer_size == arc.size());
        assert(current_buffer_size == line_length.size());
        assert(current_buffer_size == weights.size());

        assert(buffer_length <= min_buffer_length || current_buffer_size <= 1);

        // compute max window (smaller at start & end)
        if (idx_end > this->m_path.size() - buffer_size / 2) {
            max_buffer_size = max_buffer_size_end + this->m_path.size() - idx_end;
            assert(max_buffer_size >= max_buffer_size_end);
            if (idx_end < buffer_size) {
                max_buffer_size = std::min(max_buffer_size, std::max(buffer_size - max_buffer_size_start, int(idx_end)));
            }
            min_buffer_length = coord_t(max_buffer_size * mean_dist_per_line);
        } else if (idx_end < buffer_size) {
            max_buffer_size = std::max(buffer_size - max_buffer_size_start, int(idx_end));
            min_buffer_length = coord_t(max_buffer_size * mean_dist_per_line);
        }

        // try add a point in the buffer
        Point new_point = m_path[idx_end].point;
        //TODO better arc (here the length is minimized)
        coord_t new_seg_length = coord_t(m_path[idxs.back()].point.distance_to(new_point));
        assert(new_seg_length > 0);

        // be sure it's not filled
        while (current_buffer_size >= max_buffer_size) {
            // too many points, remove one
//#ifdef _DEBUG
//            std::vector<int> length;
//#endif
            
            size_t worst_idx = 0;
            float worst_weight = 0;
            // compute weight & get worst
            // push next point (to have next ofr tlast point)
            idxs.push_back(idx_end);
            for (size_t i = 0; i < current_buffer_size; ++i) {
//#ifdef _DEBUG
//                    Point previous = m_path[idxs[i]].point;
//                    Point current = m_path[idxs[i+1]].point;
//                    Point next = m_path[idxs[i+2]].point;
//                    length.resize(current_buffer_size);
//                    length[i] = previous.distance_to(current) + current.distance_to(next);
//#endif
                if (weights[i] < 0) {
                    //compute weight : 0 is 'no not remove'. 1 is 'remove this first'
                    assert(idxs.size() > i+2);
                    assert(m_path.size() > idxs[i+2]);
                    //Get previous & next point
                    Point previous = m_path[idxs[i]].point;
                    Point current = m_path[idxs[i+1]].point;
                    Point next = m_path[idxs[i+2]].point;
                    // check deviation
                    coordf_t deviation = std::sqrt(Line::distance_to_squared_abp(previous, next, current));
                    if (deviation > min_tolerance) {
                        weights[i] = 0;
                    } else {
                        coordf_t length = previous.distance_to(current) + current.distance_to(next);
                        if (length < min_point_distance) {
                            weights[i] = 3;
                        } else {
                            //add a squew to incentivise to remove every two points is equally spaced.
                            length *= squew[idxs[i+1]%8];
                            // angle: 0 : U-turn, PI: strait
                            double angle = angle_ccw(previous - current, next - current);
                            double absangle = std::abs(angle);
                            assert(absangle <= PI);
                            weights[i] = (absangle / PI) * (1 - deviation / min_tolerance) * (min_point_distance / length);
                            assert(weights[i] >= 0 && weights[i] <= 1);
                        }
                    }
                    assert(weights[i]==3 || (weights[i] >= 0 && weights[i] <= 1));
                }
                if (weights[i] > worst_weight) {
                    worst_weight = weights[i];
                    worst_idx = i;
                }
            }
            //unpush next point
            idxs.pop_back();

            if (worst_weight == 0) {
                // can't delete anything, we will just force through
                break;
            }
//#ifdef _DEBUG
            //std::cout<<"del idx"<<idxs[worst_idx + 1]<<"\n";
            //for (size_t i = 0; i < current_buffer_size; ++i) {
            //std::cout<<"  ("<<idxs[i + 1]<<") "<<length[i]<<" ("<<squew[idxs[i+1]%8]<<") "<<weights[i] <<"\n";}
//#endif
            // delete
            assert(worst_idx < arc.size());
            assert(arc[worst_idx] == 0);
            erased.push_back(idxs[worst_idx + 1]);
            idxs.erase(idxs.begin() + worst_idx + 1);
            arc.erase(arc.begin() + worst_idx);
            buffer_length -= line_length[worst_idx];
            line_length.erase(line_length.begin() + worst_idx);
            assert(weights[worst_idx] > 0);
            weights.erase(weights.begin() + worst_idx);
            --current_buffer_size;
            // recompute next point things
            if (worst_idx < current_buffer_size) {
                // recompute length from previous point
                Point previous = m_path[worst_idx].point;
                Point next = m_path[worst_idx + 1].point;
                buffer_length -= line_length[worst_idx];
                line_length[worst_idx] = previous.distance_to(next);
                buffer_length += line_length[worst_idx];
                // ask for recompute weight if not arc
                if(arc[worst_idx] == 0)
                    weights[worst_idx] = -1;
            }
            // ask for recompute previous weight if not arc
            if (worst_idx > 0) {
                if (arc[worst_idx - 1] == 0) {
                    weights[worst_idx - 1] = -1;
                }
            }
        }

        //check if the previous point has enough dist at both end
        if (current_buffer_size > 0 && arc.back() == 0 && 
            min_point_distance > line_length.back() && min_point_distance > new_seg_length
            // also make sure it's not an important point for a ponty tip.
            && new_seg_length < m_path[idxs[idxs.size() - 2]].point.distance_to(new_point)
            ) {
            // erase previous point
            erased.push_back(idxs.back());
            idxs.pop_back();
            arc.pop_back();
            buffer_length -= line_length.back();
            line_length.pop_back();
            assert(weights.back() > 0);
            weights.pop_back();
            --current_buffer_size;
            new_seg_length = coord_t(m_path[idxs.back()].point.distance_to(new_point));
            assert(new_seg_length > 0);
        }

        // add new point
        assert(new_seg_length > 0);
        idxs.push_back(idx_end);
        line_length.push_back(new_seg_length);
        buffer_length += new_seg_length;
        bool previous_is_arc = !m_path[idx_end -1].linear();
        if(previous_is_arc)
            assert(arc.empty() || arc.back() == 1);
        if (m_path[idx_end].linear()) {
            arc.push_back(previous_is_arc ? 2 : 0);
        } else {
            if (!previous_is_arc && !arc.empty()) {
                assert(arc.back() == 2 || arc.back() == 0);
                arc.back() = 2;
            }
            arc.push_back(1);
        }
        weights.push_back(arc.back() == 0 ? -1 : 0);
        current_buffer_size++;

        assert(current_buffer_size + 1 == idxs.size());
        assert(current_buffer_size == arc.size());
        assert(current_buffer_size == line_length.size());
        assert(current_buffer_size == weights.size());
        for (size_t i = 1; i < idxs.size(); ++i)
            assert(idxs[i - 1] < idxs[i]);

        // remove first point(s) if enough dist
        //while (buffer_length > min_buffer_length && current_buffer_size > 1) {
        //    idxs.pop_front(); // this erase the idx before the first point. we keep first point idx as a 'previous'
        //    arc.pop_front();
        //    buffer_length -= line_length.front();
        //    line_length.pop_front();
        //    assert(weights.front() > 0);
        //    weights.pop_front();
        //    --current_buffer_size;
        //}

        assert(buffer_length <= min_buffer_length || current_buffer_size <= 1);
    }

    std::sort(erased.begin(), erased.end());

    for (size_t i = 1; i < erased.size(); ++i)
        assert(erased[i - 1] < erased[i]);

    //remove points
    if (erased.size() < 5) {
        for (size_t idx_to_erase = erased.size() - 1; idx_to_erase < erased.size(); --idx_to_erase) {
            assert(erased[idx_to_erase] < this->m_path.size());
            this->m_path.erase(this->m_path.begin() + erased[idx_to_erase]);
        }
    } else {
        assert(!erased.empty());
        // faster? to construct a new one. (TODO: speed tests)
        Geometry::ArcWelder::Path new_path;
        size_t erased_idx = 0;
        size_t next_erased = erased[erased_idx];
        erased.push_back(m_path.size());
        for (size_t i = 0; i < m_path.size(); i++) {
            if (next_erased == i) {
                next_erased = erased[++erased_idx];
            } else {
                new_path.push_back(std::move(m_path[i]));
            }
        }
        m_path = std::move(new_path);
    }
    
    assert(is_valid());
    //at the end, we should have the buffer no more than 1/2 filled.
    return current_buffer_size;
}

void ArcPolyline::simplify_straits(const coordf_t min_tolerance,
                                  const coordf_t min_point_distance)
{
    assert(is_valid());
    assert(!m_z_offset);
    m_z_offset.reset();

    //use a window of buffer size.
    const coord_t min_point_distance_sqr = min_point_distance * min_point_distance;

    for (size_t idx_pt = 1; idx_pt < this->m_path.size() - 1; ++idx_pt) {
        // only erase point between two strait segment
        if (m_path[idx_pt].linear() && m_path[idx_pt + 1].linear()) {
            // Get previous & next point
            Point previous = m_path[idx_pt - 1].point;
            Point current = m_path[idx_pt].point;
            Point next = m_path[idx_pt + 1].point;
            // check deviation
            coordf_t deviation = std::sqrt(Line::distance_to_squared_abp(previous, next, current));
            //if deviation is small enough and the distance is too small
            if (deviation < min_tolerance &&
                (min_point_distance_sqr < previous.distance_to_square(current) ||
                 min_point_distance_sqr < current.distance_to_square(next))) {
                m_path.erase(m_path.begin() + idx_pt);
            }
        }
    }
    assert(is_valid());
    //at the end, we should have the buffer no more than 1/2 filled.
}

// douglas_peuker and create arc if with_fitting_arc
void ArcPolyline::make_arc(ArcFittingType with_fitting_arc, coordf_t tolerance, double fit_percent_tolerance)
{
    assert(!m_z_offset);
    m_z_offset.reset();
    if (with_fitting_arc != ArcFittingType::Disabled && m_path.size() > 2) {
        // BBS: do arc fit first, then use DP simplify to handle the straight part to reduce point.
        Points pts;
        Geometry::ArcWelder::Path path;
        // do only section without arcs
        size_t idx_end_mpath;
        path.push_back(m_path.front());
        pts.push_back(m_path.front().point);
        assert(path.empty() || path.front().linear());
        for (idx_end_mpath = 1; idx_end_mpath < m_path.size(); ++idx_end_mpath) {
            if (m_path[idx_end_mpath].linear()) {
                assert(pts.empty() || !pts.back().coincides_with_epsilon(m_path[idx_end_mpath].point));
                pts.push_back(m_path[idx_end_mpath].point);
            }
            // if current point is arc, make arc on the strait section before it (if enough points)
            // or if it's the last point of the path, do it on the last strait section (if enough points)
            if (!m_path[idx_end_mpath].linear() || idx_end_mpath + 1 >= m_path.size()) {
                for(int ii=1;ii<pts.size();++ii) assert(!pts[ii-1].coincides_with_epsilon(pts[ii]));
                assert(m_path[idx_end_mpath].linear() || !pts.back().coincides_with_epsilon(m_path[idx_end_mpath].point));
                // less than 3 points: don't use
                if (pts.size() > 2) {
                    // remove strait sections
                    assert(path.empty() || path.front().linear());
                    // do arc fitting
                    if (with_fitting_arc == ArcFittingType::Bambu) {
                        // === use BBS method ===
                        std::vector<Slic3r::Geometry::PathFittingData> result;
                        Slic3r::Geometry::ArcFitter::do_arc_fitting_and_simplify(pts, result, tolerance, tolerance, fit_percent_tolerance);
                        // transform PathFittingData into path
                        for (Slic3r::Geometry::PathFittingData data : result) {
                            if (data.path_type == Slic3r::Geometry::EMovePathType::Linear_move) {
                                // add strait section
                                assert(path.empty() || pts[data.start_point_index] == path.back().point);
                                assert(path.empty() || path.back().point.coincides_with_epsilon(pts[data.start_point_index]));
                                for (size_t idx_pts = data.start_point_index + (path.empty() ? 0 : 1); idx_pts < data.end_point_index + 1; ++idx_pts) {
                                    assert(!path.back().point.coincides_with_epsilon(pts[idx_pts]));
                                    path.emplace_back(pts[idx_pts], 0, Geometry::ArcWelder::Orientation::Unknown);
                                }
                            } else if (data.path_type == Slic3r::Geometry::EMovePathType::Arc_move_cw ||
                                       data.path_type == Slic3r::Geometry::EMovePathType::Arc_move_ccw) {
                                if (path.empty()) {
                                    path.emplace_back(data.arc_data.start_point, 0, Geometry::ArcWelder::Orientation::Unknown);
                                }
                                assert(!path.empty() && data.arc_data.start_point.coincides_with_epsilon(path.back().point));
                                // now the arc section
                                // point, radius, orientation
                                assert(data.arc_data.radius > 0);
                                path.emplace_back(data.arc_data.end_point,
                                                  std::abs(data.arc_data.angle_radians) > PI ? -data.arc_data.radius : data.arc_data.radius,
                                                  data.arc_data.direction == Slic3r::Geometry::ArcDirection::Arc_Dir_CCW ?
                                                      Geometry::ArcWelder::Orientation::CCW :
                                                      Geometry::ArcWelder::Orientation::CW);
#ifdef _DEBUG
                                assert(path.size() >= 2);
                                Geometry::ArcWelder::Segment &start = path[path.size() -2];
                                Geometry::ArcWelder::Segment &end = path.back();
                                //set length & center
                                end.center = Slic3r::Geometry::ArcWelder::arc_center_scalar(start.point, end.point, double(end.radius), end.ccw());
                                end.length = Geometry::ArcWelder::segment_length<coordf_t>(start, end);
                                Vec2d center = Slic3r::Geometry::ArcWelder::arc_center(start.point.cast<coordf_t>(), end.point.cast<coordf_t>(), double(end.radius), end.ccw());
                                Vec2d center_bad = Slic3r::Geometry::ArcWelder::arc_center(start.point.cast<coordf_t>(), end.point.cast<coordf_t>(), double(end.radius), !end.ccw());
                                Vec2d center_bad2 = Slic3r::Geometry::ArcWelder::arc_center(start.point.cast<coordf_t>(), end.point.cast<coordf_t>(), double(-end.radius), end.ccw());
                                Vec2d center_bad3 = Slic3r::Geometry::ArcWelder::arc_center(start.point.cast<coordf_t>(), end.point.cast<coordf_t>(), double(-end.radius), !end.ccw());
                                Point centert = Point::round(center);
                                assert(data.arc_data.center.distance_to(centert) < 100*SCALED_EPSILON);
                                double angle = Slic3r::Geometry::ArcWelder::arc_angle(start.point.cast<coordf_t>(), end.point.cast<coordf_t>(), double(end.radius));
                                double ccw_angle = angle_ccw(start.point - centert, end.point - centert);
                                if (!end.ccw())
                                    ccw_angle = (-ccw_angle);
                                if (ccw_angle < 0)
                                    ccw_angle = 2 * PI + ccw_angle;
                                assert(is_approx(ccw_angle, angle, EPSILON));
                                assert(data.arc_data.center.distance_to(end.center) < 100*SCALED_EPSILON);
                                Point center_round = centert;
                                assert(is_approx(1,2,2) && is_approx(2,1,2));
                                assert(is_approx(end.center.x(), centert.x(), coord_t(2)) && is_approx(end.center.y(), centert.y(), coord_t(2)));
                                assert(is_approx(data.arc_data.length, end.length, 2.*SCALED_EPSILON));
                                double length1 = Slic3r::Geometry::ArcWelder::arc_length(start.point, end.point, double(end.radius));
                                double length2 = Slic3r::Geometry::ArcWelder::arc_length<Vec2crd,Vec2crd,Vec2crd,double>(start.point, end.point, end.center, end.ccw());
                                assert(is_approx(length1,length2, 20.*SCALED_EPSILON));
#endif
                            } else {
                                assert(false);
                            }
                        }
                        assert(path.empty() || path.front().linear());
#if _DEBUG
                        for (size_t i = 1; i < m_path.size(); ++i) {
                            Geometry::ArcWelder::Segment &seg = m_path[i];
                            if (seg.radius) {
                                seg.length = Geometry::ArcWelder::segment_length<coordf_t>(m_path[i - 1], seg);
                                seg.center = Geometry::ArcWelder::arc_center_scalar(m_path[i - 1].point, seg.point, seg.radius, seg.ccw());
                            }
                        }
#endif
                    } else /* if (with_fitting_arc == ArcFittingType::ArcWelder)*/ {
                        // === use ArcWelder ===
                        Geometry::ArcWelder::Path result = Geometry::ArcWelder::fit_path(pts, tolerance, fit_percent_tolerance);
                        assert(result.size() > 1);
                        assert(result.front().linear());
                        assert(path.empty() || path.back().point == result.front().point);
                        assert(!path.empty() || result.front().linear());
#ifdef _DEBUG
                        Point prev = path.empty() ? result.front().point : path.back().point;
                        for (auto &seg : result) {
                            if (!seg.linear()) {
                                Vec2d center = Slic3r::Geometry::ArcWelder::arc_center(prev.cast<coordf_t>(), seg.point.cast<coordf_t>(), double(seg.radius), seg.ccw());
                                double angle = Slic3r::Geometry::ArcWelder::arc_angle(prev.cast<coordf_t>(), seg.point.cast<coordf_t>(), double(seg.radius));
                                double ccw_angle = angle_ccw(prev - Point::round(center), seg.point - Point::round(center));
                                double ccw_angle1 = ccw_angle;
                                if (!seg.ccw())
                                    ccw_angle = (-ccw_angle);
                                double ccw_angle2 = ccw_angle;
                                if (ccw_angle < 0)
                                    ccw_angle = 2 * PI + ccw_angle;
                                assert(is_approx(ccw_angle, angle, 0.01));
                                assert(is_approx(ccw_angle, angle, 0.01));
                            }
                            prev = seg.point;
                        }
#endif
                        if (path.empty()) {
                            path.insert(path.end(), result.begin(), result.end());
                        } else if(result.size() > 0) {
                            assert(path.back().point.coincides_with_epsilon(result.front().point));
                            path.insert(path.end(), result.begin() + 1, result.end());
                        }
                        assert(path.empty() || path.front().linear());
                    }
                    //assert(idx_end_mpath > 0 && m_path[idx_end_mpath].point == path.back().point);
                } else {
                    // add strait
                    assert(path.empty() || path.back().point.coincides_with_epsilon(pts.front()));
                    for (size_t idx_pts = path.empty() ? 0 : 1; idx_pts < pts.size(); ++idx_pts) {
                        path.emplace_back(pts[idx_pts], 0, Geometry::ArcWelder::Orientation::Unknown);
                    }
                }
                assert(path.back().point == pts.back());
                if (!m_path[idx_end_mpath].linear()) {
                    // add arc
                    path.push_back(m_path[idx_end_mpath]);
                }
                pts.clear();
                pts.push_back(path.back().point);
            }
        }
        // copy new path (may be the same)
        assert(m_path.front().point == path.front().point && m_path.back().point == path.back().point);
        m_path = path;
        this->m_only_strait = not_arc(*this);
        assert(is_valid());
    } else {
        auto it_end = douglas_peucker_impl(this->m_path.begin(), this->m_path.end(), this->m_path.begin(), tolerance,
                                        [](const Geometry::ArcWelder::Segment &s) { return s.point; });
        if (it_end != this->m_path.end()) {
            size_t new_size = size_t(it_end-this->m_path.begin());
            this->m_path.resize(size_t(it_end - this->m_path.begin()));
        }
        assert(is_valid());
    }
}

bool ArcPolyline::is_valid() const {
    assert(m_path[0].linear() && m_path[0].orientation == Geometry::ArcWelder::Orientation::Unknown);
    for (size_t i = 1; i < m_path.size(); ++i) {
        assert(!m_path[i].linear() || m_path[i].orientation == Geometry::ArcWelder::Orientation::Unknown);
    }
    assert(!m_z_offset || m_z_offset->size() == m_path.size());
#ifdef _DEBUG
    assert(m_path.empty() || m_path.front().linear());
    double min_radius = 0;
    double max_radius = 0;
    Point first_center;
    for (size_t i = 1; i < m_path.size(); ++i) {
        if(!this->is_3D)
            assert(!m_path[i - 1].point.coincides_with_epsilon(m_path[i].point));
        if (!m_path[i].linear()) {
            Vec2d center = Slic3r::Geometry::ArcWelder::arc_center(m_path[i-1].point.cast<coordf_t>(), m_path[i].point.cast<coordf_t>(), coordf_t(m_path[i].radius), m_path[i].ccw());
            double angle = Slic3r::Geometry::ArcWelder::arc_angle(m_path[i-1].point.cast<coordf_t>(), m_path[i].point.cast<coordf_t>(), coordf_t(m_path[i].radius));
            double ccw_angle = angle_ccw(m_path[i-1].point.cast<coordf_t>() - center, m_path[i].point.cast<coordf_t>()   - center);
            if (!m_path[i].ccw())
                ccw_angle = (-ccw_angle);
            if (ccw_angle < 0)
                ccw_angle = 2 * PI + ccw_angle;
            assert(is_approx(ccw_angle, angle, EPSILON));
            coordf_t new_length = Slic3r::Geometry::ArcWelder::arc_length(m_path[i - 1].point, m_path[i].point, coordf_t(m_path[i].radius));
            //coordf_t new_length2 = Slic3r::Geometry::ArcWelder::arc_length<Point,Point,Point,coordf_t>(m_path[i - 1].point, m_path[i].point, Point::round(center), m_path[i].ccw());
            Vec2d startd = m_path[i - 1].point.cast<double>();
            Vec2d endd = m_path[i].point.cast<double>();
            coordf_t new_length2 = Geometry::ArcWelder::arc_length<Vec2d,Vec2d,Vec2d,double>(startd, endd, center, m_path[i].ccw());
            Vec2d centerd = m_path[i].center.cast<double>();
            coordf_t new_length3 = Geometry::ArcWelder::arc_length<Vec2d,Vec2d,Vec2d,double>(startd, endd, centerd, m_path[i].ccw());
            assert(is_approx(new_length, new_length2, SCALED_EPSILON*10.));
            assert(is_approx(new_length2, new_length3, SCALED_EPSILON*10.));
            assert(is_approx(new_length2, m_path[i].length, SCALED_EPSILON*2.));
            Slic3r::Geometry::ArcWelder::Orientation orientation = Slic3r::Geometry::ArcWelder::arc_orientation(m_path[i - 1].point, m_path[i].point, m_path[i].center, m_path[i].radius);
            assert(orientation == m_path[i].orientation);
            Slic3r::Geometry::ArcWelder::Orientation orientation2 = Slic3r::Geometry::ArcWelder::arc_orientation(m_path[i - 1].point, m_path[i].point, Point::round(center), m_path[i].radius);
            assert(is_approx(coord_t(center.x()), m_path[i].center.x(), coord_t(std::abs(m_path[i].radius / 100))));
            assert(is_approx(coord_t(center.y()), m_path[i].center.y(), coord_t(std::abs(m_path[i].radius / 100))));
        }
        //assert(std::abs(max_radius - min_radius) <= std::abs(max_radius) * 0.01);
    }
    assert(not_arc(*this) == m_only_strait);
#endif
    return m_path.size() >= 2;
}

// return false if the length of this path is (now) too short. 
bool ArcPolyline::normalize() {
    assert(!has_arc() ); // TODO: with arc, if needed.
    assert(!m_z_offset);
    m_z_offset.reset();
    // remove points that are too near each other (if possible)
    if (size() > 2) {
        Point prev = get_point(size() - 2);
        Point curr = get_point(size() - 1);
        Point next = get_point(0);
        Point next_next = get_point(1);
        for (size_t i_pt = 0; i_pt < size() - 1; ++i_pt) {
            prev = curr;
            curr = next;
            next = next_next;
            next_next = get_point((i_pt + 2) % size());
            assert(prev == get_point((i_pt - 1 + size()) % size()));
            assert(curr == get_point(i_pt));
            assert(next == get_point(i_pt + 1));
            assert(next_next == get_point((i_pt + 2) % size()));
            if (curr.coincides_with_epsilon(next)) {
                // check longest segment : before or after
                //but don't remove forst or last pt (enless exaclty the same)
                coordf_t dist_before_sqr = i_pt == size() - 1 ? 0 : curr.distance_to_square(prev);
                coordf_t dist_after_sqr =  i_pt == 0 ? 0 : next.distance_to_square(next_next);
                if (dist_before_sqr < dist_after_sqr) {
                    // remove curr
                    assert(i_pt >= 0 && i_pt < size());
                    m_path.erase(m_path.begin() + i_pt);
                    --i_pt;
                    curr = prev;
                } else {
                    // remove next
                    assert(i_pt + 1 >= 0 && i_pt + 1 < size());
                    m_path.erase(m_path.begin() + i_pt + 1);
                    --i_pt;
                    next = curr;
                    curr = prev;
                }
                if (size() < 3) {
                    assert(is_valid());
                    return length() > SCALED_EPSILON;
                }
            }
        }
    } else {
        assert(is_valid());
        return length() > SCALED_EPSILON;
    }
    assert(is_valid());
    return true;
}

Polylines to_polylines(const ArcPolylines &arcpolys, coord_t deviation /*= 0*/)
{
    Polylines polys;
    for (const ArcPolyline &poly : arcpolys) {
        polys.push_back(poly.to_polyline(deviation));
    }
    return polys;
}



}
