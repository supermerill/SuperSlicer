#ifndef slic3r_Geometry_Circle_hpp_
#define slic3r_Geometry_Circle_hpp_

#include "../Point.hpp"

#include <Eigen/Geometry>

namespace Slic3r { namespace Geometry {

// https://en.wikipedia.org/wiki/Circumscribed_circle
// Circumcenter coordinates, Cartesian coordinates
template<typename Vector>
Vector circle_center(const Vector &a, const Vector &bsrc, const Vector &csrc, typename Vector::Scalar epsilon)
{
    using Scalar = typename Vector::Scalar;
    Vector b  = bsrc - a;
    Vector c  = csrc - a;
	Scalar lb = b.squaredNorm();
	Scalar lc = c.squaredNorm();
    if (Scalar d = b.x() * c.y() - b.y() * c.x(); std::abs(d) < epsilon) {
    	// The three points are collinear. Take the center of the two points
    	// furthest away from each other.
    	Scalar lbc = (csrc - bsrc).squaredNorm();
		return Scalar(0.5) * (
			lb > lc && lb > lbc ? a + bsrc :
			lc > lb && lc > lbc ? a + csrc : bsrc + csrc);
    } else {
        Vector v = lc * b - lb * c;
        return a + Vector(- v.y(), v.x()) / (2 * d);
    }
}

// 2D circle defined by its center and squared radius
template<typename Vector>
struct CircleSq {
    using Scalar = typename Vector::Scalar;

    Vector center;
    Scalar radius2;

    CircleSq() {}
    CircleSq(const Vector &center, const Scalar radius2) : center(center), radius2(radius2) {}
    CircleSq(const Vector &a, const Vector &b) : center(Scalar(0.5) * (a + b)) { radius2 = (a - center).squaredNorm(); }
    CircleSq(const Vector &a, const Vector &b, const Vector &c, Scalar epsilon) {
        this->center = circle_center(a, b, c, epsilon);
		this->radius2 = (a - this->center).squaredNorm();
    }

    bool invalid() const { return this->radius2 < 0; }
    bool valid() const { return ! this->invalid(); }
    bool contains(const Vector &p) const { return (p - this->center).squaredNorm() < this->radius2; }
    bool contains(const Vector &p, const Scalar epsilon2) const { return (p - this->center).squaredNorm() < this->radius2 + epsilon2; }

    CircleSq inflated(Scalar epsilon) const 
    	{ assert(this->radius2 >= 0); Scalar r = sqrt(this->radius2) + epsilon; return { this->center, r * r }; }

    static CircleSq make_invalid() { return CircleSq { { 0, 0 }, -1 }; }
};

// 2D circle defined by its center and radius
template<typename Vector>
struct Circle {
    using Scalar = typename Vector::Scalar;

    Vector center;
    Scalar radius;

    Circle() {}
    Circle(const Vector &center, const Scalar radius) : center(center), radius(radius) {}
    Circle(const Vector &a, const Vector &b) : center(Scalar(0.5) * (a + b)) { radius = (a - center).norm(); }
    Circle(const Vector &a, const Vector &b, const Vector &c, const Scalar epsilon) { *this = CircleSq(a, b, c, epsilon); }

    // Conversion from CircleSq
    template<typename Vector2>
    explicit Circle(const CircleSq<Vector2> &c) : center(c.center), radius(c.radius2 <= 0 ? c.radius2 : sqrt(c.radius2)) {}
    template<typename Vector2>
    Circle operator=(const CircleSq<Vector2>& c) { this->center = c.center; this->radius = c.radius2 <= 0 ? c.radius2 : sqrt(c.radius2); }

    bool invalid() const { return this->radius < 0; }
    bool valid() const { return ! this->invalid(); }
    bool contains(const Vector &p) const { return (p - this->center).squaredNorm() <= this->radius * this->radius; }
    bool contains(const Vector &p, const Scalar epsilon) const
    	{ Scalar re = this->radius + epsilon; return (p - this->center).squaredNorm() < re * re; }

    Circle inflated(Scalar epsilon) const { assert(this->radius >= 0); return { this->center, this->radius + epsilon }; }

    static Circle make_invalid() { return Circle { { 0, 0 }, -1 }; }
};

using Circlef = Circle<Vec2f>;
using Circled = Circle<Vec2d>;
using CircleSqf = CircleSq<Vec2f>;
using CircleSqd = CircleSq<Vec2d>;

/// Find the center of the circle corresponding to the vector of Points as an arc.
Point circle_center_taubin_newton(const Points::const_iterator& input_start, const Points::const_iterator& input_end, size_t cycles = 20);
inline Point circle_center_taubin_newton(const Points& input, size_t cycles = 20) { return circle_center_taubin_newton(input.cbegin(), input.cend(), cycles); }

/// Find the center of the circle corresponding to the vector of Pointfs as an arc.
Vec2d circle_center_taubin_newton(const Vec2ds::const_iterator& input_start, const Vec2ds::const_iterator& input_end, size_t cycles = 20);
inline Vec2d circle_center_taubin_newton(const Vec2ds& input, size_t cycles = 20) { return circle_center_taubin_newton(input.cbegin(), input.cend(), cycles); }
Circled circle_taubin_newton(const Vec2ds& input, size_t cycles = 20);

// Find circle using RANSAC randomized algorithm.
Circled circle_ransac(const Vec2ds& input, size_t iterations = 20);

// Randomized algorithm by Emo Welzl, working with squared radii for efficiency. The returned circle radius is inflated by epsilon.
template<typename Vector, typename Points>
CircleSq<Vector> smallest_enclosing_circle2_welzl(const Points &points, const typename Vector::Scalar epsilon)
{
    using Scalar = typename Vector::Scalar;
    CircleSq<Vector> circle;

    if (! points.empty()) {
	    const auto &p0 = points[0].template cast<Scalar>();
	    if (points.size() == 1) {
	    	circle.center = p0;
	    	circle.radius2 = epsilon * epsilon;
	    } else {
		    circle = CircleSq<Vector>(p0, points[1].template cast<Scalar>()).inflated(epsilon);
		    for (size_t i = 2; i < points.size(); ++ i)
		        if (const Vector &p = points[i].template cast<Scalar>(); ! circle.contains(p)) {
		            // p is the first point on the smallest circle enclosing points[0..i]
		            circle = CircleSq<Vector>(p0, p).inflated(epsilon);
		            for (size_t j = 1; j < i; ++ j)
		                if (const Vector &q = points[j].template cast<Scalar>(); ! circle.contains(q)) {
		                    // q is the second point on the smallest circle enclosing points[0..i]
		                    circle = CircleSq<Vector>(p, q).inflated(epsilon);
		                    for (size_t k = 0; k < j; ++ k)
		                        if (const Vector &r = points[k].template cast<Scalar>(); ! circle.contains(r))
                                    circle = CircleSq<Vector>(p, q, r, epsilon).inflated(epsilon);
		                }
		        }
		}
	}

    return circle;
}

// Randomized algorithm by Emo Welzl. The returned circle radius is inflated by epsilon.
template<typename Vector, typename Points>
Circle<Vector> smallest_enclosing_circle_welzl(const Points &points, const typename Vector::Scalar epsilon)
{
    return Circle<Vector>(smallest_enclosing_circle2_welzl<Vector, Points>(points, epsilon));
}

// Randomized algorithm by Emo Welzl. The returned circle radius is inflated by SCALED_EPSILON.
inline Circled smallest_enclosing_circle_welzl(const Points &points)
{
    return smallest_enclosing_circle_welzl<Vec2d, Points>(points, SCALED_EPSILON);
}

// Ugly named variant, that accepts the squared line 
// Don't call me with a nearly zero length vector!
// sympy: 
// factor(solve([a * x + b * y + c, x**2 + y**2 - r**2], [x, y])[0])
// factor(solve([a * x + b * y + c, x**2 + y**2 - r**2], [x, y])[1])
template<typename T>
int ray_circle_intersections_r2_lv2_c(T r2, T a, T b, T lv2, T c, std::pair<Eigen::Matrix<T, 2, 1, Eigen::DontAlign>, Eigen::Matrix<T, 2, 1, Eigen::DontAlign>> &out)
{
    T x0 = - a * c;
    T y0 = - b * c;
    T d2 = r2 * lv2 - c * c;
    if (d2 < T(0))
        return 0;
    T d = sqrt(d2);
    out.first.x() = (x0 + b * d) / lv2;
    out.first.y() = (y0 - a * d) / lv2;
    out.second.x() = (x0 - b * d) / lv2;
    out.second.y() = (y0 + a * d) / lv2;
    return d == T(0) ? 1 : 2;
}
template<typename T>
int ray_circle_intersections(T r, T a, T b, T c, std::pair<Eigen::Matrix<T, 2, 1, Eigen::DontAlign>, Eigen::Matrix<T, 2, 1, Eigen::DontAlign>> &out)
{
    T lv2 = a * a + b * b;
    if (lv2 < T(SCALED_EPSILON * SCALED_EPSILON)) {
        //FIXME what is the correct epsilon?
        // What if the line touches the circle?
        return false;
    }
    return ray_circle_intersections_r2_lv2_c2(r * r, a, b, a * a + b * b, c, out);
}


enum class ArcDirection : unsigned char {
    Arc_Dir_unknow,
    Arc_Dir_CCW,
    Arc_Dir_CW,
    Count
};

//// arc fitting ////

//constexpr double ZERO_TOLERANCE = EPSILON / 20;//0.000005;
#define ZERO_TOLERANCE EPSILON/20

class ArcCircle {
public:
    ArcCircle() {
        center = Point(0, 0);
        radius = 0;
    }
    ArcCircle(Point& p, double r) {
        center = p;
        radius = r;
    }
    Point center;
    double radius;

    Point get_closest_point(const Point& input) {
        Vec2d v = (input - center).cast<double>().normalized();
        return (center + (v * radius).cast<coord_t>());
    }

    static bool try_create_circle(const Point& p1, const Point& p2, const Point& p3, const double max_radius, ArcCircle& new_circle);
    static bool try_create_circle(const Points& points, const double max_radius, const double tolerance, ArcCircle& new_circle);
    double get_polar_radians(const Point& p1) const;
    bool is_over_deviation(const Points& points, const double tolerance);
    bool get_deviation_sum_squared(const Points& points, const double tolerance, double& sum_deviation);

    //BBS: only support calculate on X-Y plane, Z is useless
    static Vec3f calc_tangential_vector(const Vec3f& pos, const Vec3f& center_pos, const bool is_ccw);
    static bool get_closest_perpendicular_point(const Point& p1, const Point& p2, const Point& c, Point& out);
    static bool is_equal(double x, double y, double tolerance = ZERO_TOLERANCE) {
        double abs_difference = std::fabs(x - y);
        return abs_difference < tolerance;
    };
    static bool greater_than(double x, double y, double tolerance = ZERO_TOLERANCE) {
        return x > y && !ArcCircle::is_equal(x, y, tolerance);
    };
    static bool greater_than_or_equal(double x, double y, double tolerance = ZERO_TOLERANCE) {
        return x > y || ArcCircle::is_equal(x, y, tolerance);
    };
    static bool less_than(double x, double y, double tolerance = ZERO_TOLERANCE) {
        return x < y && !ArcCircle::is_equal(x, y, tolerance);
    };
    static bool less_than_or_equal(double x, double y, double tolerance = ZERO_TOLERANCE) {
        return x < y || ArcCircle::is_equal(x, y, tolerance);
    };

};

#define DEFAULT_SCALED_MAX_RADIUS scale_(2000)        // 2000mm
#define DEFAULT_SCALED_RESOLUTION scale_(0.05)        // 0.05mm
#define DEFAULT_ARC_LENGTH_PERCENT_TOLERANCE  0.05    // 5 percent

class ArcSegment : public ArcCircle {
public:
    ArcSegment() : ArcCircle() {}
    ArcSegment(Point center, double radius, Point start, Point end, ArcDirection dir) :
        ArcCircle(center, radius),
        start_point(start),
        end_point(end),
        direction(dir) {
        if (radius == 0.0 ||
            start_point == center ||
            end_point == center ||
            start_point == end_point) {
            is_arc = false;
            return;
        }
        update_angle_and_length();
        is_arc = true;
    }

    bool is_arc = false;
    double length = 0;
    double angle_radians = 0;
    double polar_start_theta = 0;
    double polar_end_theta = 0;
    Point start_point{ Point(0,0) };
    Point end_point{ Point(0,0) };
    ArcDirection direction = ArcDirection::Arc_Dir_unknow;

    bool is_valid() const { return is_arc; }
    bool clip_start(const Point& point);
    bool clip_end(const Point& point);
    bool reverse();
    bool split_at(const Point& point, ArcSegment& p1, ArcSegment& p2);
    bool is_point_inside(const Point& point) const;

private:
    void update_angle_and_length();

public:
    static bool try_create_arc(
        const Points& points,
        ArcSegment& target_arc,
        double approximate_length,
        double max_radius = DEFAULT_SCALED_MAX_RADIUS,
        double tolerance = DEFAULT_SCALED_RESOLUTION,
        double path_tolerance_percent = DEFAULT_ARC_LENGTH_PERCENT_TOLERANCE);

    static bool are_points_within_slice(const ArcSegment& test_arc, const Points& points);
    // BBS: this function is used to detect whether a ray cross the segment
    static bool ray_intersects_segment(const Point& rayOrigin, const Vec2d& rayDirection, const Line& segment);
    // BBS: these three functions are used to calculate related arguments of arc in unscale_field.
    static float calc_arc_radian(Vec3f start_pos, Vec3f end_pos, Vec3f center_pos, bool is_ccw);
    static float calc_arc_radius(Vec3f start_pos, Vec3f center_pos);
    static float calc_arc_length(Vec3f start_pos, Vec3f end_pos, Vec3f center_pos, bool is_ccw);
private:
    static bool try_create_arc(
        const ArcCircle& c,
        const Point& start_point,
        const Point& mid_point,
        const Point& end_point,
        ArcSegment& target_arc,
        double approximate_length,
        double path_tolerance_percent = DEFAULT_ARC_LENGTH_PERCENT_TOLERANCE);
};

} } // namespace Slic3r::Geometry

#endif // slic3r_Geometry_Circle_hpp_
