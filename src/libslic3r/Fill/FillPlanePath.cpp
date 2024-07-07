#include "../ClipperUtils.hpp"
#include "../ShortestPath.hpp"
#include "../Surface.hpp"

#include "FillPlanePath.hpp"

namespace Slic3r {

void FillPlanePath::_fill_surface_single(
    const FillParams                &params,
    unsigned int                     thickness_layers,
    const std::pair<float, Point>   &direction,
    ExPolygon                        expolygon,
    Polylines                       &polylines_out) const
{
    expolygon.rotate(- direction.first);

    coord_t distance_between_lines = scale_t(this->get_spacing() / params.density);

    // align infill across layers using the object's bounding box
    // Rotated bounding box of the whole object.
    BoundingBox bounding_box = this->bounding_box.rotated(- direction.first);

    Point shift = this->_centered() ?
        bounding_box.center() :
        bounding_box.min;
    expolygon.translate(-double(shift.x()), -double(shift.y()));
    bounding_box.translate(-double(shift.x()), -double(shift.y()));

    Pointfs pts = _generate(
        coord_t(ceil(coordf_t(bounding_box.min.x()) / distance_between_lines)),
        coord_t(ceil(coordf_t(bounding_box.min.y()) / distance_between_lines)),
        coord_t(ceil(coordf_t(bounding_box.max.x()) / distance_between_lines)),
        coord_t(ceil(coordf_t(bounding_box.max.y()) / distance_between_lines)),
        params.resolution);

    // doing intersection_pl(polylines, expolygon); on >200k points is too inneficient.
    // new version: with a "is_inside" for each point, in parallel
    //note: seems like there is no need to parallelize now, too quick.
    Polylines all_poly;
    for (size_t istart = 0; istart < pts.size(); istart += 1000) {
        const size_t iend = istart + 1000 < pts.size() ? istart + 1001 : pts.size();
        // Convert points to a polyline, upscale.
        Polylines polylines(1, Polyline());
        Polyline &polyline = polylines.front();
        polyline.points.reserve(iend - istart);
        //for (const Vec2d &pt : pts)
        for(size_t ip = istart ; ip < iend; ++ip)
            polyline.points.emplace_back(coord_t(floor(pts[ip].x() * distance_between_lines + 0.5)),
                                         coord_t(floor(pts[ip].y() * distance_between_lines + 0.5)));
        polylines = intersection_pl(polylines, expolygon);
        if (!polylines.empty()) {
            assert(!polylines.front().empty());
            if (!all_poly.empty() && polylines.front().front().coincides_with_epsilon(all_poly.back().back())) {
                // it continue the last polyline, so just append to it
                append(all_poly.back().points, std::move(polylines.front().points));
                //append other polylines
                if (polylines.size() > 1) {
                    all_poly.insert(all_poly.end(), polylines.begin() + 1, polylines.end());
                }
            } else {
                append(all_poly, std::move(polylines));
            }
        }
    }
    Polylines chained;
    if (params.dont_connect() || params.density > 0.5 || all_poly.size() <= 1)
        chained = chain_polylines(std::move(all_poly));
    else
        connect_infill(std::move(all_poly), expolygon, chained, scale_t(this->get_spacing()), params);
    // paths must be repositioned and rotated back
    for (Polyline &pl : chained) {
        pl.translate(double(shift.x()), double(shift.y()));
        pl.rotate(direction.first);
    }
    append(polylines_out, std::move(chained));
}

// Follow an Archimedean spiral, in polar coordinates: r=a+b\theta
Pointfs FillArchimedeanChords::_generate(coord_t min_x, coord_t min_y, coord_t max_x, coord_t max_y, const double resolution) const
{
    // Radius to achieve.
    coordf_t rmax = std::sqrt(coordf_t(max_x)*coordf_t(max_x)+coordf_t(max_y)*coordf_t(max_y)) * std::sqrt(2.) + 1.5;
    // Now unwind the spiral.
    coordf_t a = 1.;
    coordf_t b = 1./(2.*M_PI);
    coordf_t theta = 0.;
    coordf_t r = 1;
    Pointfs out;
    //FIXME Vojtech: If used as a solid infill, there is a gap left at the center.
    out.emplace_back(0, 0);
    out.emplace_back(1, 0);
    while (r < rmax) {
        // Discretization angle to achieve a discretization error lower than resolution.
        theta += 2. * acos(1. - resolution / r);
        r = a + b * theta;
        out.emplace_back(r * cos(theta), r * sin(theta));
    }
    return out;
}

// Adapted from
// http://cpansearch.perl.org/src/KRYDE/Math-PlanePath-122/lib/Math/PlanePath/HilbertCurve.pm
//
// state=0    3--2   plain
//               |
//            0--1
//
// state=4    1--2  transpose
//            |  |
//            0  3
//
// state=8
//
// state=12   3  0  rot180 + transpose
//            |  |
//            2--1
//
static inline Point hilbert_n_to_xy(const size_t n)
{
    static constexpr const int next_state[16] { 4,0,0,12, 0,4,4,8, 12,8,8,4, 8,12,12,0 };
    static constexpr const int digit_to_x[16] { 0,1,1,0, 0,0,1,1, 1,0,0,1, 1,1,0,0 };
    static constexpr const int digit_to_y[16] { 0,0,1,1, 0,1,1,0, 1,1,0,0, 1,0,0,1 };

    // Number of 2 bit digits.
    size_t ndigits = 0;
    {
        size_t nc = n;
        while(nc > 0) {
            nc >>= 2;
            ++ ndigits;
        }
    }
    int state    = (ndigits & 1) ? 4 : 0;
    coord_t x = 0;
    coord_t y = 0;
    for (int i = (int)ndigits - 1; i >= 0; -- i) {
        int digit = (n >> (i * 2)) & 3;
        state += digit;
        x |= digit_to_x[state] << i;
        y |= digit_to_y[state] << i;
        state = next_state[state];
    }
    return Point(x, y);
}

Pointfs FillHilbertCurve::_generate(coord_t min_x, coord_t min_y, coord_t max_x, coord_t max_y, const double /* resolution */) const
{
    // Minimum power of two square to fit the domain.
    size_t sz = 2;
    size_t pw = 1;
    {
        size_t sz0 = std::max(max_x + 1 - min_x, max_y + 1 - min_y);
        while (sz < sz0) {
            sz = sz << 1;
            ++ pw;
        }
    }

    size_t sz2 = sz * sz;
    Pointfs line;
    line.reserve(sz2);
    for (size_t i = 0; i < sz2; ++ i) {
        Point p = hilbert_n_to_xy(i);
        line.emplace_back(p.x() + min_x, p.y() + min_y);
    }
    return line;
}

Pointfs FillOctagramSpiral::_generate(coord_t min_x, coord_t min_y, coord_t max_x, coord_t max_y, const double /* resolution */) const
{
    // Radius to achieve.
    coordf_t rmax = std::sqrt(coordf_t(max_x)*coordf_t(max_x)+coordf_t(max_y)*coordf_t(max_y)) * std::sqrt(2.) + 1.5;
    // Now unwind the spiral.
    coordf_t r = 0;
    coordf_t r_inc = sqrt(2.);
    Pointfs out;
    out.emplace_back(0., 0.);
    while (r < rmax) {
        r += r_inc;
        coordf_t rx = r / sqrt(2.);
        coordf_t r2 = r + rx;
        out.emplace_back( r,  0.);
        out.emplace_back( r2, rx);
        out.emplace_back( rx, rx);
        out.emplace_back( rx, r2);
        out.emplace_back( 0.,  r);
        out.emplace_back(-rx, r2);
        out.emplace_back(-rx, rx);
        out.emplace_back(-r2, rx);
        out.emplace_back(- r, 0.);
        out.emplace_back(-r2, -rx);
        out.emplace_back(-rx, -rx);
        out.emplace_back(-rx, -r2);
        out.emplace_back( 0., -r);
        out.emplace_back( rx, -r2);
        out.emplace_back( rx, -rx);
        out.emplace_back( r2+r_inc, -rx);
    }
    return out;
}

} // namespace Slic3r
