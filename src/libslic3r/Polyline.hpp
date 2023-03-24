#ifndef slic3r_Polyline_hpp_
#define slic3r_Polyline_hpp_

#include "libslic3r.h"
#include "Geometry/ArcFitter.hpp"
#include "Line.hpp"
#include "MultiPoint.hpp"
#include <string>
#include <vector>

namespace Slic3r {

class Polyline;
class ThickPolyline;
class PolylineOrArc;
typedef std::vector<Polyline> Polylines;
typedef std::vector<ThickPolyline> ThickPolylines;
typedef std::vector<PolylineOrArc> PolylinesOrArcs;

class Polyline : public MultiPoint {
public:
    Polyline() {};
    Polyline(const Polyline& other) : MultiPoint(other.points) {}
    Polyline(Polyline&& other) : MultiPoint(std::move(other.points)) {}
    Polyline(std::initializer_list<Point> list) : MultiPoint(list) {}
    explicit Polyline(const Point& p1, const Point& p2) { points.reserve(2); points.emplace_back(p1); points.emplace_back(p2); }
    explicit Polyline(const Points& points) : MultiPoint(points) {}
    explicit Polyline(Points&& points) : MultiPoint(std::move(points)) {}
    Polyline& operator=(const Polyline& other) { points = other.points; return *this; }
    Polyline& operator=(Polyline&& other) { points = std::move(other.points); return *this; }
    bool operator==(const Polyline& other) const { return points == other.points; }
    bool operator!=(const Polyline& other) const { return points != other.points; }
    static Polyline new_scale(const std::vector<Vec2d> &points) {
        Polyline pl;
        pl.points.reserve(points.size());
        for (const Vec2d &pt : points)
            pl.points.emplace_back(Point::new_scale(pt(0), pt(1)));
        return pl;
    }

    void append(const Point& point) { this->points.push_back(point); }
    void append(const Points& src) { this->append(src.begin(), src.end()); }
    void append(const Points::const_iterator& begin, const Points::const_iterator& end) { this->points.insert(this->points.end(), begin, end); }
    void append(Points &&src)
    {
        if (this->points.empty()) {
            this->points = std::move(src);
        } else {
            this->points.insert(this->points.end(), src.begin(), src.end());
            src.clear();
        }
    }
    void append(const Polyline& src)
    {
        points.insert(points.end(), src.points.begin(), src.points.end());
    }

    void append(Polyline&& src)
    {
        if (this->points.empty()) {
            this->points = std::move(src.points);
        } else {
            this->points.insert(this->points.end(), src.points.begin(), src.points.end());
            src.points.clear();
        }
    }
  
    const Point& last_point() const override { return this->points.back(); }
    const Point& leftmost_point() const;
    Lines lines() const override;

    virtual void clip_end(coordf_t distance);
    void clip_start(coordf_t distance);
    void extend_end(coordf_t distance);
    void extend_start(coordf_t distance);
    Points equally_spaced_points(coordf_t distance) const;
    void simplify(coordf_t tolerance);
//    template <class T> void simplify_by_visibility(const T &area);
    void split_at(const Point &point, Polyline* p1, Polyline* p2) const;
    bool is_straight() const;
    bool is_closed() const { return this->points.front() == this->points.back(); }
};

inline bool operator==(const Polyline &lhs, const Polyline &rhs) { return lhs.points == rhs.points; }
inline bool operator!=(const Polyline &lhs, const Polyline &rhs) { return lhs.points != rhs.points; }

// Don't use this class in production code, it is used exclusively by the Perl binding for unit tests!
#ifdef PERL_UCHAR_MIN
class PolylineCollection
{
public:
    Polylines polylines;
};
#endif /* PERL_UCHAR_MIN */

extern BoundingBox get_extents(const Polyline& polyline);
extern BoundingBox get_extents(const Polylines& polylines);

inline double total_length(const Polylines& polylines) {
    double total = 0;
    for (const Polyline& pl : polylines)
        total += pl.length();
    return total;
}

inline Lines to_lines(const Polyline& poly)
{
    Lines lines;
    if (poly.points.size() >= 2) {
        lines.reserve(poly.points.size() - 1);
        for (Points::const_iterator it = poly.points.begin(); it != poly.points.end() - 1; ++it)
            lines.push_back(Line(*it, *(it + 1)));
    }
    return lines;
}

inline Lines to_lines(const Polylines& polys)
{
    size_t n_lines = 0;
    for (size_t i = 0; i < polys.size(); ++i)
        if (polys[i].points.size() > 1)
            n_lines += polys[i].points.size() - 1;
    Lines lines;
    lines.reserve(n_lines);
    for (size_t i = 0; i < polys.size(); ++i) {
        const Polyline& poly = polys[i];
        for (Points::const_iterator it = poly.points.begin(); it != poly.points.end() - 1; ++it)
            lines.push_back(Line(*it, *(it + 1)));
    }
    return lines;
}

inline Polylines to_polylines(const std::vector<Points>& paths)
{
    Polylines out;
    out.reserve(paths.size());
    for (const Points& path : paths)
        out.emplace_back(path);
    return out;
}

inline Polylines to_polylines(std::vector<Points>&& paths)
{
    Polylines out;
    out.reserve(paths.size());
    for (const Points& path : paths)
        out.emplace_back(std::move(path));
    return out;
}

inline void polylines_append(Polylines& dst, const Polylines& src)
{
    dst.insert(dst.end(), src.begin(), src.end());
}

inline void polylines_append(Polylines& dst, Polylines&& src)
{
    if (dst.empty()) {
        dst = std::move(src);
    } else {
        std::move(std::begin(src), std::end(src), std::back_inserter(dst));
        src.clear();
    }
}

const Point& leftmost_point(const Polylines& polylines);

bool remove_degenerate(Polylines& polylines);

// Returns index of a segment of a polyline and foot point of pt on polyline.
std::pair<int, Point> foot_pt(const Points& polyline, const Point& pt);

/// ThickPolyline : a polyline with a width for each point
/// This class has a vector of coordf_t, it must be the same size as points.
/// it's used to store the size of the line at this point.
/// Also, the endpoint let us know if the front() and back() of the polyline 
/// join something or is a dead-end.
class ThickPolyline : public Polyline {
public:
    enum StartPos : int8_t { tpspBegin = -1, tpspBoth = 0, tpspEnd = 1 };
    /// width size must be == point size
    std::vector<coord_t> points_width;
    /// if true => it's an endpoint, if false it join an other ThickPolyline. first is at front(), second is at back()
    std::pair<bool, bool> endpoints;
    //if it's important to begin at a specific bit.
    StartPos start_at = tpspBoth;

    ThickPolyline() : endpoints(std::make_pair(false, false)) {}
    ThickLines thicklines() const;
    void reverse() {
        Polyline::reverse();
        std::reverse(this->points_width.begin(), this->points_width.end());
        std::swap(this->endpoints.first, this->endpoints.second);
        start_at = StartPos(-start_at);
    }

    void clip_end(coordf_t distance) override;
};

inline ThickPolylines to_thick_polylines(Polylines&& polylines, const coordf_t width)
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

/// concatenate poylines if possible and refresh the endpoints
void concatThickPolylines(ThickPolylines& polylines);

class Polyline3 : public MultiPoint3
{
public:
    virtual Lines3 lines() const;
};

typedef std::vector<Polyline3> Polylines3;


//private inheritance to avoid letting 'points' visible, as it has to be sync with arc fitting.
class PolylineOrArc : /*public*/ Polyline {
public:
    PolylineOrArc() {};
    PolylineOrArc(const PolylineOrArc& other) : Polyline(other.points), m_fitting_result(other.m_fitting_result) {
        assert(this->m_fitting_result.empty() || this->m_fitting_result.back().end_point_index < this->points.size());
    }
    PolylineOrArc(PolylineOrArc&& other) : Polyline(std::move(other.points)), m_fitting_result(std::move(other.m_fitting_result)) {
        assert(this->m_fitting_result.empty() || this->m_fitting_result.back().end_point_index < this->points.size());
    }
    PolylineOrArc(const Polyline& other) : Polyline(other), m_fitting_result() { }
    PolylineOrArc(Polyline&& other) : Polyline(std::move(other)), m_fitting_result() { }
    PolylineOrArc(std::initializer_list<Point> list) : Polyline(list) {
        m_fitting_result.clear();
    }
    explicit PolylineOrArc(const Point& p1, const Point& p2) : Polyline(p1, p2) { m_fitting_result.clear(); }
    explicit PolylineOrArc(const Points& points) : Polyline(points) { m_fitting_result.clear(); }
    explicit PolylineOrArc(Points&& points) : Polyline(std::move(points)) { m_fitting_result.clear(); }
    PolylineOrArc& operator=(const PolylineOrArc& other) {
        points = other.points; m_fitting_result = other.m_fitting_result;
        assert(this->m_fitting_result.empty() || this->m_fitting_result.back().end_point_index < this->points.size()); return *this;
    }
    PolylineOrArc& operator=(PolylineOrArc&& other) {
        points = std::move(other.points); m_fitting_result = std::move(other.m_fitting_result);
        assert(this->m_fitting_result.empty() || this->m_fitting_result.back().end_point_index < this->points.size()); return *this;
    }
    PolylineOrArc& operator=(const Polyline& other) { points = other.points; this->m_fitting_result.clear(); return *this; }
    PolylineOrArc& operator=(Polyline&& other) { points = std::move(other.points); this->m_fitting_result.clear(); return *this; }
    PolylineOrArc& operator=(const Points& other) { points = other; this->m_fitting_result.clear(); return *this; }
    PolylineOrArc& operator=(Points&& other) { points = std::move(other); this->m_fitting_result.clear(); return *this; }
    Polyline& as_polyline() { return static_cast<Polyline&>(*this); }
    const Polyline& as_polyline() const { return static_cast<const Polyline&>(*this); }
    bool operator==(const PolylineOrArc& other) const { return points == other.points; }
    bool operator!=(const PolylineOrArc& other) const { return points != other.points; }
    //static PolylineOrArc new_scale(const std::vector<Vec2d>& points) {
    //    Polyline pl;
    //    pl.points.reserve(points.size());
    //    for (const Vec2d& pt : points)
    //        pl.points.emplace_back(Point::new_scale(pt(0), pt(1)));
    //    //BBS: new_scale doesn't support arc, so clean
    //    pl.fitting_result.clear();
    //    return pl;
    //}

    void append(const Point& point) {
        //BBS: don't need to append same point
        if (!this->empty() && this->back() == point) {
            assert(false);
            return;
        }
        this->points.push_back(point);
        append_fitting_result_after_append_points();
    }
    void append_before(const Point& point) {
        //BBS: don't need to append same point
        if (!this->empty() && this->front() == point) {
            assert(false);
            return;
        }
        if (this->size() == 1) {
            this->m_fitting_result.clear();
            MultiPoint::append(point);
            MultiPoint::reverse();
        } else {
            this->reverse();
            this->append(point);
            this->reverse();
        }
        assert(this->m_fitting_result.empty() || this->m_fitting_result.back().end_point_index < this->points.size());
    }
    void append(const Points& src) {
        //BBS: don't need to append same point
        if (!this->empty() && !src.empty() && this->back() == src[0]) {
            assert(false);
            this->append(src.begin() + 1, src.end());
        } else {
            this->append(src.begin(), src.end());
        }
        append_fitting_result_after_append_points();
    }
    void append(const Points::const_iterator& begin, const Points::const_iterator& end) {
        //BBS: don't need to append same point
        if (!this->empty() && begin != end && this->back() == *begin) {
            assert(false);
            MultiPoint::append(begin + 1, end);
        } else {
            this->points.insert(this->points.end(), begin, end);
        }
        append_fitting_result_after_append_points();
    }
    void append(Points&& src)
    {
        if (this->points.empty()) {
            this->points = std::move(src);
        } else {
            this->points.insert(this->points.end(), src.begin(), src.end());
            src.clear();
        }
        append_fitting_result_after_append_points();
    }
    void append(const PolylineOrArc& src);
    void append(PolylineOrArc&& src);
    void clear() { MultiPoint::clear(); this->m_fitting_result.clear(); }
    void swap(PolylineOrArc& other) {
        std::swap(this->points, other.points);
        std::swap(this->m_fitting_result, other.m_fitting_result);
    }

    //multipoint methods
    const Point& front() const { return Polyline::front(); }
    const Point& back() const { return Polyline::back(); }
    Lines lines() const { return Polyline::lines(); }
    size_t size() const { return Polyline::size(); }
    bool   empty() const { return Polyline::empty(); }
    double length() const { return Polyline::length(); }
    bool   is_valid() const { return Polyline::is_valid(); }
    int  find_point(const Point& point) const { return Polyline::find_point(point); }
    int  find_point(const Point& point, const double scaled_epsilon) const { return Polyline::find_point(point, scaled_epsilon); }
    int  closest_point_index(const Point& point) const { return Polyline::closest_point_index(point); }
    Point point_projection(const Point& point) const { return Polyline::point_projection(point); }

    virtual void reverse() override;

    bool has_arc() const { return !m_fitting_result.empty(); }
    const std::vector<Slic3r::Geometry::PathFittingData>& get_arc() const { return m_fitting_result; }
    void reset_arc() { m_fitting_result.clear(); }


    const Points& get_points() const { return points; }
    Points& set_points() {
        assert(m_fitting_result.empty());
        return points;
    }

    Points equally_spaced_points(coordf_t distance) const;
    void simplify(coordf_t tolerance, bool with_fitting_arc, double fit_tolerance);

    //split& & clip
    //    template <class T> void simplify_by_visibility(const T &area);
    void split_at(Point& point, PolylineOrArc* p1, PolylineOrArc* p2) const;
    bool split_at_index(const size_t index, PolylineOrArc* p1, PolylineOrArc* p2) const;
    void clip_end(coordf_t distance);
    void clip_start(coordf_t distance);
    void clip_first_point(); // pop_front();
    void clip_last_point(); // pop_back();

    //bool is_straight() const;
    bool is_closed() const { return this->points.front() == this->points.back(); }

    //BBS: 
    PolylineOrArc equally_spaced_lines(double distance) const;

    void ensure_fitting_result_valid() const {
        assert(m_fitting_result.empty() || m_fitting_result.back().end_point_index < size());
    }

protected:
    //BBS: store arc fitting result
    std::vector<Slic3r::Geometry::PathFittingData> m_fitting_result;

    void append_fitting_result_after_append_points();
    void append_fitting_result_after_append_polyline(const PolylineOrArc& src);
    void reset_to_linear_move();
    bool split_fitting_result_before_index(const size_t index, Point& new_endpoint, std::vector<Slic3r::Geometry::PathFittingData>& data) const;
    bool split_fitting_result_after_index(const size_t index, Point& new_startpoint, std::vector<Slic3r::Geometry::PathFittingData>& data) const;
};

Polylines to_polylines(const PolylinesOrArcs& polys_or_arcs);

} //namespace slic3r

#endif
