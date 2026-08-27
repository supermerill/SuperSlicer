///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/ Copyright (c) superslicer 2019 - 2025 Durand Rémi @supermerill
///|/ Copyright (c) Prusa Research 2016 - 2023 Pavel Mikuš @Godrak, Vojtěch Bubník @bubnikv, Lukáš Hejl @hejllukas, Lukáš Matěna @lukasmatena
///|/ Copyright (c) Slic3r 2014 - 2015 Alessandro Ranellucci @alranel
///|/
///|/ ported from lib/Slic3r/PerimeterGenerator.pm:
///|/ Copyright (c) Prusa Research 2016 - 2022 Vojtěch Bubník @bubnikv
///|/ Copyright (c) Slic3r 2011 - 2016 Alessandro Ranellucci @alranel
///|/
///|/ SuperSlicer, PrusaSlicer, Slic3r are released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "PerimeterGenerator.hpp"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <limits>
#include <list>
#include <ostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>

#include <boost/log/trivial.hpp>

#include <iterator>

#include <stack>

#include "AABBTreeIndirect.hpp"
#include "AABBTreeLines.hpp"
#include <algorithm>
#include "Arachne/utils/ExtrusionJunction.hpp"
#include "Arachne/utils/ExtrusionLine.hpp"
#include "Arachne/WallToolPaths.hpp"
#include "BoundingBox.hpp"
#include "BridgeDetector.hpp"
#include <clipper/clipper_z.hpp>
#include "ExPolygon.hpp"
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "ExtrusionEntityVisitors.hpp"
#include "Geometry.hpp"
#include "Geometry/ConvexHull.hpp"
#include "Geometry/MedialAxis.hpp"
#include "KDTreeIndirect.hpp"
#include "Layer.hpp"
#include "libslic3r.h"
#include "Line.hpp"
#include "Milling/MillingPostProcess.hpp"
#include "MultiPoint.hpp"
#include "Point.hpp"
#include "Polygon.hpp"
#include "Polyline.hpp"
#include "Print.hpp"
#include "Config/PrintConfig.hpp"
#include "ShortestPath.hpp"
#include "Surface.hpp"
#include "SurfaceCollection.hpp"
#include "SVG.hpp"
#include "Thread.hpp"

//#define ARACHNE_DEBUG

#ifdef ARACHNE_DEBUG
#include "Utils.hpp"
#endif


namespace Slic3r::PerimeterGenerator {


void assert_check_polygon(const Polygon &polygon) {
#if _DEBUG
    for (size_t i_pt = 1; i_pt < polygon.size(); ++i_pt)
        assert(!polygon.points[i_pt - 1].coincides_with_epsilon(polygon.points[i_pt]));
    assert(!polygon.points.front().coincides_with_epsilon(polygon.points.back()));
#endif
}

void assert_check_polygons(const Polygons &polygons) {
#if _DEBUG
    for (const Polygon &polygon : polygons)
        assert_check_polygon(polygon);
#endif
}
void assert_check_loops(const std::vector<PerimeterGeneratorLoops> &loops) {
#if _DEBUG
    for (const PerimeterGeneratorLoops &pgls : loops) {
        for (const PerimeterGeneratorLoop &pgl : pgls) {
            assert_check_polygon(pgl.polygon);
        }
    }
#endif
}

PerimeterGeneratorLoops get_all_childs(const PerimeterGeneratorLoop &loop) {
    PerimeterGeneratorLoops ret;
    for (const PerimeterGeneratorLoop &child : loop.children) {
        ret.push_back(child);
        append(ret, get_all_childs(child));
    }
    return ret;
}

PerimeterGeneratorLoops get_all_external_holes(const PerimeterGeneratorLoop &loop) {
    PerimeterGeneratorLoops ret;
    for (size_t idx = 0; idx < loop.children.size(); ++idx) {
        if (!loop.children[idx].is_contour && loop.children[idx].depth == 0) {
            assert(loop.children[idx].children.empty());
            ret.push_back(loop.children[idx]);
        } else {
            append(ret, get_all_external_holes(loop.children[idx]));
        }
    }
    return ret;
}

//return true if normalized, return false if should be deleted.
bool normalize_contour(Polygon &contour) {
    // remove points that are too near each other (if possible)
    if (contour.size() > 3) {
        Point prev = contour.points[contour.size() - 2];
        Point curr = contour.points[contour.size() - 1];
        Point next = contour.points[0];
        Point next_next = contour.points[1];
        //check end -> begin
        while (curr.coincides_with_epsilon(next)) {
            // check longest segment : before or after
            coordf_t dist_before_sqr = curr.distance_to_square(prev);
            coordf_t dist_after_sqr = next.distance_to_square(next_next);
            if (dist_before_sqr < dist_after_sqr) {
                // remove curr
                contour.points.erase(contour.points.end() - 1);
                curr = prev;
                prev = contour.points[contour.size() - 2];
            } else {
                // remove next
                contour.points.erase(contour.points.begin());
                next = next_next;
                next_next = contour.points[1];
            }
            if (contour.size() < 3) {
                assert(false);
                return false;
            }
        }
        //check others
        for (size_t i_pt = 0; i_pt < contour.size() - 1; ++i_pt) {
            prev = curr;
            curr = next;
            next = next_next;
            next_next = contour.points[(i_pt + 2) % contour.size()];
            assert(prev == contour.points[(i_pt - 1 + contour.size()) % contour.size()]);
            assert(curr == contour.points[i_pt]);
            assert(next == contour.points[i_pt + 1]);
            assert(next_next == contour.points[(i_pt + 2) % contour.size()]);
            if (curr.coincides_with_epsilon(next)) {
                // check longest segment : before or after
                coordf_t dist_before_sqr = curr.distance_to_square(prev);
                coordf_t dist_after_sqr = next.distance_to_square(next_next);
                if (dist_before_sqr < dist_after_sqr) {
                    // remove curr
                    assert(i_pt >= 0 && i_pt < contour.size());
                    contour.points.erase(contour.points.begin() + i_pt);
                    --i_pt;
                    curr = prev;
                } else {
                    // remove next
                    assert(i_pt + 1 >= 0 && i_pt + 1 < contour.size());
                    contour.points.erase(contour.points.begin() + i_pt + 1);
                    --i_pt;
                    next = curr;
                    curr = prev;
                }
                if (contour.size() < 3) {
                    assert(false);
                    return false;
                }
            }
        }
    } else {
        contour.douglas_peucker(SCALED_EPSILON * 2);
        if (contour.size() < 3) {
            return false;
        }
    }
    return true;
}

bool PerimeterGeneratorLoop::is_internal_contour() const
{
    // An internal contour is a contour containing no other contours
    if (!this->is_contour)
        return false;
    for (const PerimeterGeneratorLoop &loop : this->children)
        if (loop.is_contour)
            return false;
    return true;
}

// Thanks Cura developers for this function.
static void fuzzy_paths(ExtrusionPaths& paths, coordf_t fuzzy_skin_thickness, coordf_t fuzzy_skin_point_dist)
{
    const coordf_t min_dist_between_points = fuzzy_skin_point_dist * 3. / 4.; // hardcoded: the point distance may vary between 3/4 and 5/4 the supplied value
    const coordf_t range_random_point_dist = fuzzy_skin_point_dist / 2.;
    coordf_t dist_next_point = //min_dist_between_points / 4 + (coordf_t(safe_rand()) * range_random_point_dist / double(RAND_MAX)); // the distance to be traversed on the line before making the first new point
        coordf_t(safe_rand()) * (min_dist_between_points / 2) / double(RAND_MAX); // the distance to be traversed on the line before making the first new point

    // check if the paths length is enough for at least 3 points, or return.
    {
        coordf_t min_dist = min_dist_between_points * 3;
        for (const ExtrusionPath& path : paths) {
            min_dist -= path.length();
            if (min_dist < 0)
                break;
        }
        if (min_dist > 0) {
            // Too small, can't fuzzy.
            return;
        }
    }

    const Point last_point = paths.back().last_point();
    //not always a loop, with arachne
    const bool is_loop = paths.front().first_point() == last_point;
#ifdef _DEBUG
    const Point first_point = paths.front().first_point();
    const bool is_debug_loop = (last_point == last_point);
    ExtrusionPaths saved_paths = paths;
    if (is_loop) assert(paths.back().last_point() == paths.front().first_point());
    for (int i = 1; i < paths.size(); i++) {
        assert(paths[i - 1].last_point() == paths[i].first_point());
    }
#endif
    Point p0 = /*is_loop ? last_point : */paths.front().first_point();
    const Point* previous_point = is_loop ? &last_point : &paths.front().first_point();
    for (size_t idx_path = 0; idx_path < paths.size(); idx_path++) {
        ExtrusionPath& path = paths[idx_path];
        Points out;
        size_t next_idx = 1;
        assert(path.size() > 1);
        // it always follow
        assert(p0 == path.polyline().front());
        out.reserve(path.polyline().size());
        out.push_back(*previous_point);
        for (; next_idx < path.polyline().size(); next_idx++)
        {
            const Point& p1 = path.polyline().get_point(next_idx);
            // 'a' is the (next) new point between p0 and p1
            Vec2d  p0p1 = (p1 - p0).cast<double>();
            coordf_t p0p1_size = p0p1.norm();
            //skip points too close to each other.
            if (dist_next_point < p0p1_size) {
                coordf_t p0pa_dist;
                for (p0pa_dist = dist_next_point; p0pa_dist < p0p1_size;
                    p0pa_dist += min_dist_between_points + coordf_t(safe_rand()) * range_random_point_dist / double(RAND_MAX))
                {
                    coordf_t r = coordf_t(safe_rand()) * (fuzzy_skin_thickness * 2.) / double(RAND_MAX) - fuzzy_skin_thickness;
                    out.emplace_back(p0 + (p0p1 * (p0pa_dist / p0p1_size) + perp(p0p1).cast<double>().normalized() * r).cast<coord_t>());
                    assert(out.size() > 1 && !out.back().coincides_with_epsilon(out[out.size()-2]));
                }
                dist_next_point = p0pa_dist - p0p1_size;
                p0 = p1;
            } else {
                dist_next_point -= p0p1_size;
            }
        }
        if (out.size() <= 1) {
            double mm3_diff_with_next = 0.;
            if (paths.size() - 1 > idx_path) {
                double curr_mm3 = path.mm3_per_mm();
                double next_mm3 = paths[idx_path + 1].mm3_per_mm();
                mm3_diff_with_next = curr_mm3 < next_mm3 ? curr_mm3 / next_mm3 : next_mm3 / curr_mm3;
            }
            if (out.size() == 1 && path.polyline().length() > SCALED_EPSILON && mm3_diff_with_next < 0.9) {
                // if the flow is too different to merge with next path, don't change the path (but the first point)
                assert(path.size() > 1);
                path.polyline().set_front(*previous_point);
                for (size_t i = 1; i < path.polyline().size(); i++)
                    assert(!path.polyline().get_point(i - 1).coincides_with_epsilon(path.polyline().get_point(i)));
            } else if (paths.size() - 1 > idx_path) {
                // too small, merge with next path
                path.polyline().clear();
                paths.erase(paths.begin() + idx_path);
                paths[idx_path].polyline().append_before(p0);
                assert(!paths[idx_path].polyline().get_point(0).coincides_with_epsilon(paths[idx_path].polyline().get_point(1)));
                idx_path--;
            } else {
                // nothing after, just finish at the same point
                assert(path.size() > 1);
                path.polyline().set_front(*previous_point);
                for (size_t i = 1; i < path.polyline().size(); i++)
                    assert(!path.polyline().get_point(i - 1).coincides_with_epsilon(path.polyline().get_point(i)));
            }
        } else {
            p0 = path.polyline().back();
            path.polyline() = ArcPolyline(out);
            previous_point = &path.polyline().back();
        }
    }
    assert(!paths.empty());
    if (is_loop) {
        
        assert(paths.front().polyline().front() != paths.back().polyline().back() ||
            // or the last one is skipped are skipped
               (paths.back().size() == 2 && paths.back().length() < min_dist_between_points * 2));
        //the first point is the old one. remove it and try to make another point if needed.
        if (paths.front().size() > 2 && fuzzy_skin_point_dist * 2 > paths.back().last_point().distance_to(paths.front().polyline().get_point(1))) {
            //distance small enough and enough points to delete the first, just erase
            paths.front().polyline().pop_front();
        }//TODO: else
        //loop -> last point is the same as the first
        paths.back().polyline().append(paths.front().polyline().front());
        assert(paths.front().polyline().front() == paths.back().polyline().back());
    } else {
        //line -> ensure you end with the same last point
        if (!paths.back().polyline().back().coincides_with_epsilon(last_point)) {
            paths.back().polyline().append(last_point);
        } else {
            paths.back().polyline().set_back(last_point);
        }
    }
#ifdef _DEBUG
    if (is_loop) assert(paths.back().last_point() == paths.front().first_point());
    for (int i = 1; i < paths.size(); i++) {
        assert(paths[i - 1].last_point() == paths[i].first_point());
    }
#endif
}

// Thanks Cura developers for this function.
static void fuzzy_polygon(Polygon& poly, coordf_t fuzzy_skin_thickness, coordf_t fuzzy_skin_point_dist)
{
    const double min_dist_between_points = fuzzy_skin_point_dist * 3. / 4.; // hardcoded: the point distance may vary between 3/4 and 5/4 the supplied value
    const double range_random_point_dist = fuzzy_skin_point_dist / 2.;
    double dist_left_over = double(safe_rand()) * (min_dist_between_points / 2) / double(RAND_MAX); // the distance to be traversed on the line before making the first new point
    Point* p0 = &poly.points.back();
    Points out;
    out.reserve(poly.points.size());
    for (Point& p1 : poly.points)
    { // 'a' is the (next) new point between p0 and p1
        Vec2d  p0p1 = (p1 - *p0).cast<double>();
        double p0p1_size = p0p1.norm();
        // so that p0p1_size - dist_last_point evaulates to dist_left_over - p0p1_size
        double dist_last_point = dist_left_over + p0p1_size * 2.;
        for (double p0pa_dist = dist_left_over; p0pa_dist < p0p1_size;
            p0pa_dist += min_dist_between_points + double(safe_rand()) * range_random_point_dist / double(RAND_MAX))
        {
            double r = double(safe_rand()) * (fuzzy_skin_thickness * 2.) / double(RAND_MAX) - fuzzy_skin_thickness;
            out.emplace_back(*p0 + (p0p1 * (p0pa_dist / p0p1_size) + perp(p0p1).cast<double>().normalized() * r).cast<coord_t>());
            dist_last_point = p0pa_dist;
        }
        dist_left_over = p0p1_size - dist_last_point;
        p0 = &p1;
    }
    if (out.size() < 3) {
        size_t point_idx = std::max(size_t(0), poly.size() - 2);
        while (out.size() < 3) {
            out.emplace_back(poly[point_idx]);
            if (point_idx == 0) {
                break;
            }
            --point_idx;
        }
    }
    if (out.size() >= 3) {
        poly.points = std::move(out);
    }
}

// Thanks Cura developers for this function.
//supermerill: doesn't work
static void fuzzy_extrusion_line(Arachne::ExtrusionLine &ext_lines, double fuzzy_skin_thickness, double fuzzy_skin_point_dist)
{
    const double min_dist_between_points = fuzzy_skin_point_dist * 3. / 4.; // hardcoded: the point distance may vary between 3/4 and 5/4 the supplied value
    const double range_random_point_dist = fuzzy_skin_point_dist / 2.;
    double       dist_left_over          = double(safe_rand()) * (min_dist_between_points / 2) / double(RAND_MAX); // the distance to be traversed on the line before making the first new point

    auto                                   *p0 = &ext_lines.front();
    std::vector<Arachne::ExtrusionJunction> out;
    out.reserve(ext_lines.size());
    for (auto &p1 : ext_lines) {
        if (p0->p == p1.p) { // Connect endpoints.
            out.emplace_back(p1.p, p1.w, p1.perimeter_index);
            continue;
        }

        // 'a' is the (next) new point between p0 and p1
        Vec2d  p0p1      = (p1.p - p0->p).cast<double>();
        double p0p1_size = p0p1.norm();
        // so that p0p1_size - dist_last_point evaulates to dist_left_over - p0p1_size
        double dist_last_point = dist_left_over + p0p1_size * 2.;
        for (double p0pa_dist = dist_left_over; p0pa_dist < p0p1_size; p0pa_dist += min_dist_between_points + double(safe_rand()) * range_random_point_dist / double(RAND_MAX)) {
            double r = double(safe_rand()) * (fuzzy_skin_thickness * 2.) / double(RAND_MAX) - fuzzy_skin_thickness;
            out.emplace_back(p0->p + (p0p1 * (p0pa_dist / p0p1_size) + perp(p0p1).cast<double>().normalized() * r).cast<coord_t>(), p1.w, p1.perimeter_index);
            dist_last_point = p0pa_dist;
        }
        dist_left_over = p0p1_size - dist_last_point;
        p0             = &p1;
    }

    while (out.size() < 3) {
        size_t point_idx = ext_lines.size() - 2;
        out.emplace_back(ext_lines[point_idx].p, ext_lines[point_idx].w, ext_lines[point_idx].perimeter_index);
        if (point_idx == 0) {
            break;
        }
        -- point_idx;
    }

    if (ext_lines.back().p == ext_lines.front().p) { // Connect endpoints.
        out.front().p = out.back().p;
    }

    if (out.size() >= 3) {
        ext_lines.junctions = std::move(out);
    }
}

bool PerimeterGenerator::_enforce_speed_overhangs(ExtrusionPaths &paths, int count_since_overhang) const {
    // set to overhang speed if any chunk is overhang
    bool has_overhang = false;
    if (!params.region_setting.has_many_config(&params.config.overhangs_speed_enforce)) {
        int overhangs_speed_enforce = params.region_setting.get_solo_config(&params.config.overhangs_speed_enforce).get_int();
        if (overhangs_speed_enforce > 0)
            for (const ExtrusionPath &path : paths) {
                assert(!path.role().is_overhang() || path.overhang_attributes());
                if (path.overhang_attributes() &&
                    path.overhang_attributes()->start_distance_from_prev_layer >= 1) {
                    has_overhang = true;
                    break;
                }
            }
        if (has_overhang ||
            (count_since_overhang >= 0 && overhangs_speed_enforce > count_since_overhang)) {
            // enforce
            for (ExtrusionPath &path : paths) {
                assert(path.role().is_perimeter());
                if (!path.role().has(ExtrusionRoleModifier::ERM_Bridge)) {
                    path.set_role(path.role() | ExtrusionRoleModifier::ERM_Bridge);
                    path.overhang_attributes_mutable() = ExtrusionPropertyOverhang{1, 2, 0, false, true, false, false};
                } else if (path.overhang_attributes() && path.overhang_attributes()->has_full_overhangs_speed == false) {
                    path.overhang_attributes_mutable().has_full_overhangs_speed = true;
                }
            }
        }
    } else {
        for (auto const &[overhangs_speed_enforce, areas] : params.region_setting.get_areas(&params.config.overhangs_speed_enforce)) {
            if (overhangs_speed_enforce.get_int() > 0) {
                std::vector<bool> has_overhangs;
                for (const ExPolygon &area : areas.expolys) {
                    has_overhangs.push_back(false);
                }
                for (const ExtrusionPath &path : paths) {
                    assert(!path.role().is_overhang() || path.overhang_attributes());
                    if (path.overhang_attributes() &&
                        path.overhang_attributes()->start_distance_from_prev_layer >= 1) {
                        for (size_t i = 0; i < areas.expolys.size(); i++) {
                            if (!has_overhangs[i]) {
                                if (!intersection_pl(path.polyline().to_polyline(), areas.bboxes[i].polygon()).empty() &&
                                    !intersection_pl(path.polyline().to_polyline(), areas.expolys[i]).empty()) {
                                    has_overhangs[i] = true;
                                    break;
                                }
                            }
                        }
                    }
                }
                for (size_t i = 0; i < areas.expolys.size(); i++) {
                    if (has_overhangs[i] ||
                        (count_since_overhang >= 0 && overhangs_speed_enforce.get_int() > count_since_overhang)) {
                        // enforce
                        for (ExtrusionPath &path : paths) {
                            assert(path.role().is_perimeter());
                            if (!path.role().has(ExtrusionRoleModifier::ERM_Bridge)) {
                                if (!intersection_pl(path.polyline().to_polyline(), areas.bboxes[i].polygon()).empty() &&
                                    !intersection_pl(path.polyline().to_polyline(), areas.expolys[i]).empty()) {
                                    assert(!path.overhang_attributes());
                                    path.set_role(path.role() | ExtrusionRoleModifier::ERM_Bridge);
                                    path.overhang_attributes_mutable() = ExtrusionPropertyOverhang{1, 2, 0, false, true, false, false};
                                    has_overhang = true;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return has_overhang;
}

ExtrusionEntityCollection PerimeterGenerator::_traverse_loops_classic(const Parameters &params,
    const PerimeterGeneratorLoops &loops, ThickPolylines &thin_walls, int count_since_overhang /*= -1*/) const
{
    // loops is an arrayref of ::Loop objects
    // turn each one into an ExtrusionLoop object
    ExtrusionEntitiesPtr coll;
    for (const PerimeterGeneratorLoop &loop : loops) {
        bool is_external = loop.is_external();

        ExtrusionRole role = ExtrusionRole::None;
        ExtrusionLoopRole loop_role = ExtrusionLoopRole::elrDefault;
        role = is_external ? ExtrusionRole::ExternalPerimeter : ExtrusionRole::Perimeter;
        if (loop.is_internal_contour()) {
            // Note that we set loop role to ContourInternalPerimeter
            // also when loop is both internal and external (i.e.
            // there's only one contour loop).
            loop_role = (ExtrusionLoopRole)(loop_role | ExtrusionLoopRole::elrInternal);
        }
        if (!loop.is_contour) {
            loop_role = (ExtrusionLoopRole)(loop_role | ExtrusionLoopRole::elrHole);
        }
        if (loop.children.empty()) {
            loop_role = ExtrusionLoopRole(loop_role | ExtrusionLoopRole::elrFirstLoop);
        }
        if (is_external &&
            (params.region_setting.has_many_config(&params.config.seam_slope_type) ||
             params.region_setting.get_solo_config(&params.config.seam_slope_type).get_int() != int32_t(SeamScarfType::None))) {
            this->throw_if_canceled();
            for (auto const &[opt_values, areas] :
                 params.region_setting.get_areas(&params.config.seam_slope_type)) {
                // first test if applicable
                SeamScarfType scarf_type = SeamScarfType(opt_values.get_int(&params.config.seam_slope_type));
                if (scarf_type != SeamScarfType::None &&
                    opt_values.get_bool(&params.config.external_perimeters_first)){
                    // should be okay.
                    // next test
                    if (opt_values.get_bool(&params.config.external_perimeters_first_force) ||
                        (loop.is_contour &&
                         opt_values.get_bool(&params.config.external_perimeters_nothole)) ||
                        (!loop.is_contour && opt_values.get_bool(&params.config.external_perimeters_hole) && scarf_type == SeamScarfType::All)) {
                        // does it intersect?
                        if (areas.is_accept_all() || !intersection(loop.polygon, areas.expolys).empty()) {
                            // then apply the tag
                            loop_role = (ExtrusionLoopRole) (loop_role | ExtrusionLoopRole::elrVase);
                        }
                    }
                }
            }
        }

#if _DEBUG
        for (size_t idx = 1; idx < loop.polygon.size(); ++idx)
            assert(!loop.polygon.points[idx - 1].coincides_with_epsilon(loop.polygon.points[idx]));
#endif
        
        // detect overhanging/bridging perimeters
        ExtrusionPaths paths;

        bool can_overhang = (params.config.overhangs_width_speed.is_enabled() ||
                             params.config.overhangs_width.is_enabled()) &&
            params.layer->id() > 0 && params.layer->id() >= params.object_config.raft_layers;
        if (params.object_config.support_material &&
            params.object_config.support_material_contact_distance_type.value == zdNone) {
            can_overhang = false;
        }
        if (can_overhang) {
            paths = this->create_overhangs_classic(params, loop.polygon.split_at_first_point(), role, is_external);
#if _DEBUG
            for(const ExtrusionPath &path : paths)
                for (size_t idx = 1; idx < path.size(); ++idx)
                    assert(!path.polyline().get_point(idx - 1).coincides_with_epsilon(path.polyline().get_point(idx)));
#endif

        } else {
            assert(!role.is_overhang());
            for (size_t idx = 1; idx < loop.polygon.size(); ++idx)
                assert(!loop.polygon.points[idx-1].coincides_with_epsilon(loop.polygon.points[idx]));
            paths.emplace_back(
                loop.polygon.split_at_first_point(),
                ExtrusionAttributes{
                    role,
                    ExtrusionFlow{
                        is_external ? params.ext_mm3_per_mm() : params.mm3_per_mm(),
                        is_external ? params.ext_perimeter_flow.width() : params.perimeter_flow.width(),
                        float(params.layer->unscaled_height())
                    }
                },
                nullptr, false
            );
            assert(paths.back().mm3_per_mm() == paths.back().mm3_per_mm());
            assert(paths.back().width() == paths.back().width());
            assert(paths.back().height() == paths.back().height());
        }
#if _DEBUG
        for(const ExtrusionPath &path : paths)
            for (size_t idx = 1; idx < path.size(); ++idx)
                assert(!path.polyline().get_point(idx - 1).coincides_with_epsilon(path.polyline().get_point(idx)));
#endif
        if (loop.fuzzify) {
            double nozle_diameter = is_external ? params.ext_perimeter_flow.nozzle_diameter() : params.perimeter_flow.nozzle_diameter();
            double fuzzy_skin_thickness = params.config.fuzzy_skin_thickness.get_effective_value(nozle_diameter);
            double fuzzy_skin_point_dist = params.config.fuzzy_skin_point_dist.get_effective_value(nozle_diameter);
            fuzzy_paths(paths, scale_d(fuzzy_skin_thickness), scale_d(fuzzy_skin_point_dist));
        }
#if _DEBUG
        for(const ExtrusionPath &path : paths)
            for (size_t idx = 1; idx < path.size(); ++idx)
                assert(!path.polyline().get_point(idx - 1).coincides_with_epsilon(path.polyline().get_point(idx)));
#endif

        coll.push_back(new ExtrusionLoop(paths, loop_role));
    
    }
    assert(coll.size() == loops.size());
    // append thin walls to the nearest-neighbor search (only for first iteration)
    if (!thin_walls.empty()) {
        append(coll, Geometry::thin_variable_width(thin_walls, ExtrusionRole::ThinWall, params.ext_perimeter_flow,
                                                   std::max(params.ext_perimeter_flow.scaled_width() / 4,
                                                            scale_i(params.print_config.resolution)),
                                                   false));
        //don't add again
        thin_walls.clear();
    }
    // traverse children and build the final collection
    Point zero_point(0, 0);
    //result is  [idx, needReverse] ?
    std::vector<std::pair<size_t, bool>> chain = chain_extrusion_entities(coll, &zero_point);
    assert(coll.size() == chain.size());
    for(const ExtrusionEntity *loop : coll) assert(loop);
    ExtrusionEntityCollection coll_out;
    if (chain.empty()) {
        return coll_out;
    }

    // little check: if you have external holes with only one extrusion and internal things, please draw the internal first, just in case it can help print the hole better.
    std::vector<std::pair<size_t, bool>> better_chain;
    {
        std::vector<std::pair<size_t, bool>> alone_holes;
        std::vector<std::pair<size_t, bool>> keep_ordering;
        std::vector<std::pair<size_t, bool>> thin_walls;
        for (const std::pair<size_t, bool> &idx : chain) {
            if (idx.first < loops.size()) {
                if (!loops[idx.first].is_external() ||
                    (!loops[idx.first].is_contour && !loops[idx.first].children.empty())) {
                    alone_holes.push_back(idx);
                } else {
                    keep_ordering.push_back(idx);
                }
            } else {
                thin_walls.push_back(idx);
            }
        }
        append(better_chain, std::move(alone_holes));
        append(better_chain, std::move(keep_ordering));
        append(better_chain, std::move(thin_walls));
    }
    assert(better_chain.size() == chain.size());
    
    // if brim will be printed, reverse the order of perimeters so that
    // we continue inwards after having finished the brim
    const bool reverse_contour  = (params.layer->id() == 0 && params.object_config.brim_width.value > 0) ||
                           (params.config.external_perimeters_first.value && (params.config.external_perimeters_nothole.value || params.config.external_perimeters_first_force.value));
    const bool reverse_hole = (params.layer->id() == 0 && params.object_config.brim_width_interior.value > 0) || 
                           (params.config.external_perimeters_first.value && (params.config.external_perimeters_hole.value || params.config.external_perimeters_first_force.value));
    
    const bool CCW_contour = params.config.perimeter_direction.value == PerimeterDirection::pdCCW_CW ||  params.config.perimeter_direction.value == PerimeterDirection::pdCCW_CCW;
    const bool CCW_hole = params.config.perimeter_direction.value == PerimeterDirection::pdCW_CCW ||  params.config.perimeter_direction.value == PerimeterDirection::pdCCW_CCW;

#if _DEBUG
    for(auto ee : coll) DEBUG_TREE_VISIT(*ee, LoopAssertVisitor())
#endif
    //move from coll to coll_out and getting children of each in the same time. (deep first)
    for (const std::pair<size_t, bool> &idx : better_chain) {

        if (idx.first >= loops.size()) {
            // this is a thin wall
            // let's get it from the sorted collection as it might have been reversed
            coll_out.append(ExtrusionEntityUPtr(coll[idx.first]));
            coll[idx.first] = nullptr;
            if (idx.second) {
                coll_out.entities().back()->reverse();
            }
            //if thin extrusion is a loop, make it ccw like a normal contour.
            if (ExtrusionLoop* loop = dynamic_cast<ExtrusionLoop*>(coll_out.entities().back())) {
                if (loop->is_clockwise()) {
                    loop->reverse();
                }
            }
        } else {
            const PerimeterGeneratorLoop &loop = loops[idx.first];

#if _DEBUG
            for(auto ee : coll) if(ee) LoopAssertVisitor().traverse(*ee);
            loop.polygon.assert_valid();
#endif
            ExtrusionLoop *eloop = static_cast<ExtrusionLoop *>(coll[idx.first]);

            //set to overhang speed if any chunk is overhang
            ExtrusionPaths loop_paths = eloop->paths();
            bool has_overhang = this->_enforce_speed_overhangs(loop_paths, count_since_overhang);
            eloop->paths() = std::move(loop_paths);
#if _DEBUG
            for(auto ee : coll) if(ee) LoopAssertVisitor().traverse(*ee);
#endif
            assert(thin_walls.empty());
            // special case: external all first
            ExtrusionEntityCollection children_ext_holes;
            ExtrusionEntityCollection children;
            if (params.config.external_perimeters_first_force.value) {
                if (loop.is_contour && loop.depth == 0) {
                    // here, i may have some external hole as childs
                    PerimeterGeneratorLoops ext_holes = get_all_external_holes(loop);
                    children_ext_holes = this->_traverse_loops_classic(params, {ext_holes}, thin_walls, has_overhang ? 1 : (count_since_overhang < 0 ? -1 : (count_since_overhang+1)));
                }
                PerimeterGeneratorLoops children_no_ext_hole; // TODO fix nlogn copies here
                for (const PerimeterGeneratorLoop &child : loop.children) {
                    if (child.is_contour || child.depth != 0) {
                        children_no_ext_hole.push_back(child);
                    }
                }
                children = this->_traverse_loops_classic(params, children_no_ext_hole, thin_walls, has_overhang ? 1 : (count_since_overhang < 0 ? -1 : (count_since_overhang+1)));
            } else {
                //normal case
                children = this->_traverse_loops_classic(params, loop.children, thin_walls, has_overhang ? 1 : (count_since_overhang < 0 ? -1 : (count_since_overhang+1)));
            }
            coll[idx.first] = nullptr;
            bool has_steep_overhangs_this_loop = false;
            if (loop.is_steep_overhang && params.layer->id() % 2 == 1 && !params.config.perimeter_reverse) {
                has_steep_overhangs_this_loop = HasRoleVisitor::search(*eloop, HasThisRoleVisitor{ExtrusionRole::OverhangPerimeter});
            }
            if ((loop.is_contour && !reverse_contour) || (!loop.is_contour && reverse_hole)) {
                //note: params.layer->id() % 2 == 1 already taken into account in the is_steep_overhang compute (to save time).
                // if CCW: reverse if steep_overhang & odd. if CW: the opposite
                bool clockwise = !(loop.is_contour ? CCW_contour : CCW_hole);
                if ((params.config.perimeter_reverse || has_steep_overhangs_this_loop) && params.layer->id() % 2 == 1) {
                    clockwise = !clockwise;
                }
                // has to reverse the direction if print external first, as the whole thing will be reverse afterwards
                //if (loop.is_contour ? reverse_contour : reverse_hole) {
                //    clockwise = !clockwise;
                //}

                if (clockwise) {
                    if (!eloop->is_clockwise()) {
                        eloop->reverse(); // make_clockwise
                    }
                } else {
                    if (eloop->is_clockwise()) {
                        eloop->reverse(); // make_couter_clockwise
                    }
                }
                //ensure that our children are printed before us
                if (!children.empty() || !children_ext_holes.empty()) {
                    ExtrusionEntityCollection print_child_beforeplz;
                    print_child_beforeplz.set_can_sort_reverse(false, false);
                    if (children.entities().size() > 1 && (children.can_reverse() || children.can_sort())) {
                        print_child_beforeplz.append(children);
                    } else if (!children.entities().empty()) {
                        print_child_beforeplz.append_move_from(children);
                    }
                    if (!children_ext_holes.empty()) {print_child_beforeplz.append(std::move(children_ext_holes));}
                    print_child_beforeplz.append(*eloop);
                    coll_out.append(std::move(print_child_beforeplz));
                } else {
                    coll_out.append(*eloop);
                }
            } else {
                bool counter_clockwise = (loop.is_contour ? CCW_contour : CCW_hole);
                if ((params.config.perimeter_reverse || has_steep_overhangs_this_loop) && params.layer->id() % 2 == 1) {
                    counter_clockwise = !counter_clockwise;
                }
                // has to reverse the direction if print external first, as the whole thing will be reverse afterwards
                //if (loop.is_contour ? reverse_contour : reverse_hole) {
                //    counter_clockwise = !counter_clockwise;
                //}
                // if hole: reverse if steep_overhang & odd. if contour: the opposite
                if (counter_clockwise) {
                    if (eloop->is_clockwise()) {
                        eloop->reverse(); // make_couter_clockwise
                    }
                } else {
                    if (!eloop->is_clockwise()) {
                        eloop->reverse(); // make_clockwise
                    }
                }
                // ensure that our children are printed after us
                if (!children.empty()|| !children_ext_holes.empty()) {
                    ExtrusionEntityCollection print_child_afterplz;
                    print_child_afterplz.set_can_sort_reverse(false, false);
                    print_child_afterplz.append(*eloop);
                    if (!children_ext_holes.empty()) {print_child_afterplz.append(std::move(children_ext_holes));}
                    if (children.entities().size() > 1 && (children.can_reverse() || children.can_sort())) {
                        print_child_afterplz.append(children);
                    } else  if (!children.entities().empty()) {
                        print_child_afterplz.append_move_from(children);
                    }
                    coll_out.append(std::move(print_child_afterplz));
                } else {
                    coll_out.append(*eloop);
                }
            }
        }
    }
#if _DEBUG
    LoopAssertVisitor().traverse(coll_out);
#endif
    return coll_out;
}

ExtrusionPaths PerimeterGenerator::create_overhangs_classic(const Parameters &params,
                                                            const Polyline &loop_polygons,
                                                            const ExtrusionRole role,
                                                            const bool is_external) const {
#ifdef _DEBUG_OVERHANGS
    static int i_inst = 0;
    const int iInst = ++i_inst;
    std::vector<std::string> debug_colors;
#endif
    loop_polygons.assert_valid();

    ExtrusionPaths paths;
    coord_t resolution = std::max(SCALED_EPSILON, this->get_resolution(0, false));

    // if no overhang detection
    if (params.lower_slices_bridge_dynamic.empty()) {
        // error
        paths.emplace_back(loop_polygons,
                           ExtrusionAttributes{role,
                                               ExtrusionFlow{is_external ? params.ext_mm3_per_mm() :
                                                                           params.mm3_per_mm(),
                                                             is_external ? params.ext_perimeter_flow.width() :
                                                                           params.perimeter_flow.width(),
                                                             float(params.layer->unscaled_height())}},
                           nullptr, false);
        assert(paths.back().mm3_per_mm() == paths.back().mm3_per_mm());
        assert(paths.back().height() == paths.back().height());
        assert(paths.size() == 1);
        assert(paths.back().width() == paths.back().width());
#if _DEBUG
        for (size_t idx = 1; idx < paths.front().size(); ++idx)
            assert(!paths.front().polyline().get_point(idx - 1).coincides_with_epsilon(
                paths.front().polyline().get_point(idx)));
#endif
        //assert all overhang path have overhang attributes
        for (const ExtrusionPath &path : paths) {
            assert (!path.role().is_overhang() || path.overhang_attributes());
        }
        return paths;
    }
    // set the fan & speed before the flow
    Polylines ok_polylines = {loop_polygons};

    Polylines dynamic_speed;
    Polylines small_speed;
    Polylines big_speed;
    Polylines small_flow;
    Polylines big_flow;
#ifdef _DEBUG
    for (Polyline &poly : ok_polylines)
        for (int i = 0; i < poly.points.size() - 1; i++)
            assert(!poly.points[i].coincides_with_epsilon(poly.points[i + 1]));
#endif
    // create bouding box of current polyline for clipping to speed up diff_pl & intersection_pl
    BoundingBox bbox(loop_polygons.points);
    bbox.offset(SCALED_EPSILON);
    // detect each overhang area
    Polylines *previous = &ok_polylines;
#ifdef _DEBUG_OVERHANGS
    {
        BoundingBox bbox = get_extents(params.layer->lslices());
        bbox.offset(scale_(1.));
        static int iii=0;
        ::Slic3r::SVG svg(debug_out_path("%d_before_overahngs_%d_%d.svg", params.layer->id(), iInst, ++iii).c_str(), bbox);
        for (const Surface &srf : *this->slices) {
            svg.draw(srf.expolygon, "grey");
        }
        svg.draw(*lower_slices, "black");
        svg.draw(loop_polygons, "purple", scale_i(0.03));
        svg.draw(to_polylines(params.lower_slices_bridge_dynamic), "red", scale_i(0.02));
        svg.draw(to_polylines(params.lower_slices_bridge_speed_small), "orange", scale_i(0.017));
        svg.draw(to_polylines(params.lower_slices_bridge_speed_big), "yellow", scale_i(0.013));
        svg.draw(to_polylines(params.lower_slices_bridge_flow_small), "green", scale_i(0.01));
        svg.draw(to_polylines(params.lower_slices_bridge_flow_big), "cyan", scale_i(0.007));
        svg.Close();
    }
#endif

    Polygons lower_slices_bridge_clipped =
        ClipperUtils::clip_clipper_polygons_with_subject_bbox(params.lower_slices_bridge_dynamic, bbox);
    if (!lower_slices_bridge_clipped.empty()) {
        dynamic_speed = diff_pl(*previous, lower_slices_bridge_clipped);
        if (!dynamic_speed.empty()) {
            *previous = intersection_pl(*previous, lower_slices_bridge_clipped);
            previous = &dynamic_speed;
        }
    } else {
        dynamic_speed = std::move(*previous);
        previous->clear();
        previous = &dynamic_speed;
    }

    Polygons lower_slices_bridge_speed_small_clipped =
        ClipperUtils::clip_clipper_polygons_with_subject_bbox(params.lower_slices_bridge_speed_small, bbox);
    if (!lower_slices_bridge_speed_small_clipped.empty()) {
        small_speed = diff_pl(*previous, lower_slices_bridge_speed_small_clipped);
        for (Polyline &poly : small_speed) {
            poly.douglas_peucker(SCALED_EPSILON);
            assert(poly.size() > 1);
            if (poly.size() > 2)
                poly.assert_valid();
        }
        if (!small_speed.empty()) {
            *previous = intersection_pl(*previous, lower_slices_bridge_speed_small_clipped);
            for (Polyline &poly : *previous) {
                poly.douglas_peucker(SCALED_EPSILON);
                assert(poly.size() > 1);
                if (poly.size() > 2)
                    poly.assert_valid();
            }
            previous = &small_speed;
        }
    } else {
        small_speed = std::move(*previous);
        previous->clear();
        previous = &small_speed;
    }

    Polygons lower_slices_bridge_speed_big_clipped =
        ClipperUtils::clip_clipper_polygons_with_subject_bbox(params.lower_slices_bridge_speed_big, bbox);
    if (!lower_slices_bridge_speed_big_clipped.empty()) {
        big_speed = diff_pl(*previous, lower_slices_bridge_speed_big_clipped);
        for (Polyline &poly : big_speed) {
            poly.douglas_peucker(SCALED_EPSILON);
            assert(poly.size() > 1);
            if (poly.size() > 2)
                poly.assert_valid();
        }
        if (!big_speed.empty()) {
            *previous = intersection_pl(*previous, lower_slices_bridge_speed_big_clipped);
            for (Polyline &poly : *previous) {
                poly.douglas_peucker(SCALED_EPSILON);
                assert(poly.size() > 1);
                if (poly.size() > 2)
                    poly.assert_valid();
            }
            previous = &big_speed;
        }
    } else {
        big_speed = std::move(*previous);
        previous->clear();
        previous = &big_speed;
    }

    Polygons lower_slices_bridge_flow_small_clipped =
        ClipperUtils::clip_clipper_polygons_with_subject_bbox(params.lower_slices_bridge_flow_small, bbox);
    if (!lower_slices_bridge_flow_small_clipped.empty()) {
        small_flow = diff_pl(*previous, lower_slices_bridge_flow_small_clipped);
        for (Polyline &poly : small_flow) {
            poly.douglas_peucker(SCALED_EPSILON);
            assert(poly.size() > 1);
            if (poly.size() > 2)
                poly.assert_valid();
        }
        if (!small_flow.empty()) {
            *previous = intersection_pl(*previous, lower_slices_bridge_flow_small_clipped);
            for (Polyline &poly : *previous) {
                poly.douglas_peucker(SCALED_EPSILON);
                assert(poly.size() > 1);
                if (poly.size() > 2)
                    poly.assert_valid();
            }
            previous = &small_flow;
        }
    } else {
        small_flow = std::move(*previous);
        previous->clear();
        previous = &small_flow;
    }

    Polygons lower_slices_bridge_flow_big_clipped =
        ClipperUtils::clip_clipper_polygons_with_subject_bbox(params.lower_slices_bridge_flow_big, bbox);
    if (!lower_slices_bridge_flow_big_clipped.empty()) {
        big_flow = diff_pl(*previous, lower_slices_bridge_flow_big_clipped);
        for (Polyline &poly : big_flow) {
            poly.douglas_peucker(SCALED_EPSILON);
            assert(poly.size() > 1);
            if (poly.size() > 2)
                poly.assert_valid();
        }
        if (!big_flow.empty()) {
            *previous = intersection_pl(*previous, lower_slices_bridge_flow_big_clipped);
            for (Polyline &poly : *previous) {
                poly.douglas_peucker(SCALED_EPSILON);
                assert(poly.size() > 1);
                if (poly.size() > 2)
                    poly.assert_valid();
            }
            previous = &big_flow;
        }
    } else {
        big_flow = std::move(*previous);
        previous->clear();
        previous = &big_flow;
    }

    // ensure polylines are valid (at least EPSILON between two points), unless the path is itself shorter than
    // epsilon (then it's two points)
    for (Polylines *polylines : {&ok_polylines, &dynamic_speed, &small_speed, &big_speed, &small_flow, &big_flow}) {
        for (Polyline &poly : *polylines) {
            poly.douglas_peucker(SCALED_EPSILON);
        }
    }
    //assert all overhang path have overhang attributes
    for (const ExtrusionPath &path : paths) {
        assert (!path.role().is_overhang() || path.overhang_attributes());
    }
#ifdef _DEBUG_OVERHANGS
    {
        BoundingBox bbox = get_extents(params.layer->lslices());
        bbox.offset(scale_(1.));
        static int iii=0;
        ::Slic3r::SVG svg(debug_out_path("%d_cmiddle_overahngs_%d_%d.svg", params.layer->id(), iInst, ++iii).c_str(), bbox);
        for (const Surface &srf : *this->slices) {
            svg.draw(srf.expolygon, "grey");
        }
        svg.draw(*lower_slices, "black");
        svg.draw(loop_polygons, "white", scale_i(0.065));
        svg.draw(ok_polylines, "lightgrey", scale_i(0.06));
        svg.draw(to_polylines(params.lower_slices_bridge_dynamic), "orange", scale_i(0.055));
        svg.draw((dynamic_speed), "red", scale_i(0.05));
        svg.draw(to_polylines(params.lower_slices_bridge_speed_small), "yellow", scale_i(0.045));
        svg.draw((small_speed), "brown", scale_i(0.04));
        svg.draw(to_polylines(params.lower_slices_bridge_speed_big), "lime", scale_i(0.035));
        svg.draw((big_speed), "green", scale_i(0.03));
        svg.draw(to_polylines(params.lower_slices_bridge_flow_small), "cyan", scale_i(0.025));
        svg.draw((small_flow), "blue", scale_i(0.02));
        svg.draw(to_polylines(params.lower_slices_bridge_flow_big), "pink", scale_i(0.015));
        svg.draw((big_flow), "purple", scale_i(0.01));
        svg.Close();
    }
#endif

    // note: layer height is used to identify the path type
    int idx_lh_size = 0;
    Params_sort_overhangs overhang_params;
    if (!ok_polylines.empty()) {
        // fast track
        if (dynamic_speed.empty() && small_speed.empty() && big_speed.empty() && small_flow.empty() &&
            big_flow.empty()) {
            return {ExtrusionPath{loop_polygons,
                                  ExtrusionAttributes{role,
                                                      ExtrusionFlow{is_external ? params.ext_mm3_per_mm() :
                                                                                  params.mm3_per_mm(),
                                                                    is_external ? params.ext_perimeter_flow.width() :
                                                                                  params.perimeter_flow.width(),
                                                                    float(params.layer->unscaled_height())}},
                                  nullptr, false}};
        }
        extrusion_paths_append(paths, ok_polylines,
                               ExtrusionAttributes{role,
                                                   ExtrusionFlow(is_external ? params.ext_mm3_per_mm() :
                                                                               params.mm3_per_mm(),
                                                                 is_external ? params.ext_perimeter_flow.width() :
                                                                               params.perimeter_flow.width(),
                                                                 idx_lh_size // layer height is used as id, temporarly
                                                                 )});
#ifdef _DEBUG_OVERHANGS
        debug_colors.push_back("darkgrey");
#endif
        overhang_params.overhang_type_2_lh.push_back(idx_lh_size);
        idx_lh_size++;
    } else {
        overhang_params.overhang_type_2_lh.push_back(-1);
    }
    bool has_dynamic = false;
    if (!dynamic_speed.empty()) {
        has_dynamic = true;
        extrusion_paths_append(paths, dynamic_speed,
                               ExtrusionAttributes{role,
                                                   ExtrusionFlow(is_external ? params.ext_mm3_per_mm() :
                                                                               params.mm3_per_mm(),
                                                                 is_external ? params.ext_perimeter_flow.width() :
                                                                               params.perimeter_flow.width(),
                                                                 idx_lh_size // layer height is used as id, temporarly
                                                                 )},
                                                   ExtrusionPropertyOverhang(0, 1, 0, false, false, true, true));
#ifdef _DEBUG_OVERHANGS
        debug_colors.push_back("red");
#endif
        overhang_params.overhang_type_2_lh.push_back(idx_lh_size);
        idx_lh_size++;
    } else {
        overhang_params.overhang_type_2_lh.push_back(-1);
    }
    //assert all overhang path have overhang attributes
    for (const ExtrusionPath &path : paths) {
        assert (!path.role().is_overhang() || path.overhang_attributes());
    }
    if (!small_speed.empty()) {
        extrusion_paths_append(paths, small_speed,
                               ExtrusionAttributes{role | ExtrusionRoleModifier::ERM_Bridge,
                                                   ExtrusionFlow(is_external ? params.ext_mm3_per_mm() :
                                                                               params.mm3_per_mm(),
                                                                 is_external ? params.ext_perimeter_flow.width() :
                                                                               params.perimeter_flow.width(),
                                                                 idx_lh_size // layer height is used as id, temporarly
                                                                 )},
                                                   ExtrusionPropertyOverhang(1, 1, 0, false, true, true, false));
#ifdef _DEBUG_OVERHANGS
        debug_colors.push_back("brown");
#endif
        overhang_params.overhang_type_2_lh.push_back(idx_lh_size);
        idx_lh_size++;
    } else {
        overhang_params.overhang_type_2_lh.push_back(-1);
    }
    if (!big_speed.empty()) {
        extrusion_paths_append(paths, big_speed,
                               ExtrusionAttributes{role | ExtrusionRoleModifier::ERM_Bridge,
                                                   ExtrusionFlow(is_external ? params.ext_mm3_per_mm() :
                                                                               params.mm3_per_mm(),
                                                                 is_external ? params.ext_perimeter_flow.width() :
                                                                               params.perimeter_flow.width(),
                                                                 idx_lh_size // layer height is used as id, temporarly
                                                                 )},
                                                   ExtrusionPropertyOverhang(1, 1, 0, false, true, true, false));
#ifdef _DEBUG_OVERHANGS
        debug_colors.push_back("green");
#endif
        overhang_params.overhang_type_2_lh.push_back(idx_lh_size);
        idx_lh_size++;
    } else {
        overhang_params.overhang_type_2_lh.push_back(-1);
    }
    if (!small_flow.empty()) {
        extrusion_paths_append(paths, small_flow,
                               ExtrusionAttributes{role | ExtrusionRoleModifier::ERM_Bridge,
                                                   ExtrusionFlow(params.m_mm3_per_mm_overhang,
                                                                 params.overhang_flow.width(),
                                                                 idx_lh_size // layer height is used as id, temporarly
                                                                 )},
                                                   ExtrusionPropertyOverhang(1, 2, 0, true, true, false, false));
#ifdef _DEBUG_OVERHANGS
        debug_colors.push_back("blue");
#endif
        overhang_params.overhang_type_2_lh.push_back(idx_lh_size);
        idx_lh_size++;
    } else {
        overhang_params.overhang_type_2_lh.push_back(-1);
    }
    if (!big_flow.empty()) {
        extrusion_paths_append(paths, big_flow,
                               ExtrusionAttributes{role | ExtrusionRoleModifier::ERM_Bridge,
                                                   ExtrusionFlow(params.m_mm3_per_mm_overhang,
                                                                 params.overhang_flow.width(),
                                                                 idx_lh_size // layer height is used as id, temporarly
                                                                 )},
                                                   ExtrusionPropertyOverhang(1, 2, 0, true, true, false, false));
#ifdef _DEBUG_OVERHANGS
        debug_colors.push_back("purple");
#endif
        overhang_params.overhang_type_2_lh.push_back(idx_lh_size);
        idx_lh_size++;
    } else {
        overhang_params.overhang_type_2_lh.push_back(-1);
    }
    assert(overhang_params.overhang_type_2_lh.size() == 6);
    assert(idx_lh_size >= 0 && idx_lh_size < 7);
    overhang_params.is_loop = true;
    overhang_params.is_external = is_external;
    overhang_params.has_dynamic = has_dynamic;
    overhang_params.first_point = loop_polygons.front();
    //not back() at it's the same as the first, and this is for sorting if sort fail.
    overhang_params.last_point = loop_polygons.points[loop_polygons.size() / 2];
#ifdef _DEBUG_OVERHANGS
    overhang_params.debug_i = iInst;
    overhang_params.debug_colors = debug_colors;
#endif

    //common function with arachne to sort & merge extrusions.
    _sort_overhangs(params, paths, role, overhang_params);

#ifdef _DEBUG_OVERHANGS
    {
        BoundingBox bbox = get_extents(params.layer->lslices());
        bbox.offset(scale_(1.));
        static int iii = 0;
        ::Slic3r::SVG svg(debug_out_path("%d_end_overahngs_%d_%d.svg", params.layer->id(), iInst, ++iii).c_str(), bbox);
        for (const Surface &srf : *this->slices) {
            svg.draw(srf.expolygon, "grey");
        }
        svg.draw(*lower_slices, "black");
        svg.draw(loop_polygons, "white", scale_i(0.065));
        svg.draw(ok_polylines, "lightgrey", scale_i(0.06));
        svg.draw(to_polylines(params.lower_slices_bridge_dynamic), "orange", scale_i(0.05));
        svg.draw(to_polylines(params.lower_slices_bridge_speed_small), "yellow", scale_i(0.045));
        svg.draw(to_polylines(params.lower_slices_bridge_speed_big), "lime", scale_i(0.04));
        svg.draw(to_polylines(params.lower_slices_bridge_flow_small), "cyan", scale_i(0.035));
        svg.draw(to_polylines(params.lower_slices_bridge_flow_big), "pink", scale_i(0.03));
        for (const ExtrusionPath &path : paths) {
            if (!path.role().is_overhang()) {
                svg.draw((path.as_polyline().to_polyline()), is_approx(path.mm3_per_mm(), params.m_mm3_per_mm_overhang, 0.001) ? "red" : "brown", scale_i(0.02));
            } else if (path.height() < params.overhang_flow.height()) {
                svg.draw((path.as_polyline().to_polyline()), "green", scale_i(0.02));
            }else{
                svg.draw((path.as_polyline().to_polyline()), "blue", scale_i(0.02));
            }
        }
        svg.Close();
    }
#endif
#ifdef _DEBUG
    {
        Point last_pt = paths.front().last_point();
        for (size_t idx = 1; idx < paths.size(); ++idx) {
            const ExtrusionPath &path = paths[idx];
            assert(path.polyline().size() >= 2);
            assert(path.first_point() == last_pt);
            for (size_t idx = 1; idx < path.size(); ++idx)
                assert(!path.polyline().get_point(idx - 1).coincides_with_epsilon(path.polyline().get_point(idx)));
            last_pt = path.last_point();
        }
    }
#endif

    //assert all overhang path have overhang attributes
    for (const ExtrusionPath &path : paths) {
        assert (!path.role().is_overhang() || path.overhang_attributes());
    }
    assert(paths.size() == 1 || paths.front().first_point() == paths.back().last_point());
    return paths;
}



void PerimeterGenerator::_sort_overhangs(const Parameters &params,
                                         ExtrusionPaths &paths,
                                         const ExtrusionRole role,
                                         const Params_sort_overhangs &overhang_params
) const
{
    const bool dynamic_enabled = params.config.overhangs &&
        (params.config.overhangs_dynamic_speed.is_enabled() ||
         (params.config.overhangs_flow_ratio.is_enabled() && params.config.overhangs_dynamic_flow.is_enabled()));
    // reapply the nearest point search for starting point
    // We allow polyline reversal because Clipper may have randomly reversed polylines during clipping.
    if(!paths.empty())
        chain_and_reorder_extrusion_paths(paths, &overhang_params.first_point);
#ifdef _DEBUG_OVERHANGS
    std::vector<std::string> colors = {"darkgrey", "red", "brown", "green", "blue", "purple"};
    Polylines loop_polygons;
    Polylines ok_polylines;
    auto export_debug_overhangs_svg = [&](const std::string &name){
        BoundingBox bbox = get_extents(params.layer->lslices());
        bbox.offset(scale_(1.));
        static int iii=0;
        ::Slic3r::SVG svg(debug_out_path(name.c_str(), params.layer->id(), overhang_params.debug_i, iii++).c_str(), bbox);
        for (const Surface &srf : *this->slices) {
            svg.draw(srf.expolygon, "grey");
        }
        svg.draw(*lower_slices, "black");
        svg.draw(loop_polygons, "white", scale_i(0.065));
        svg.draw(ok_polylines, "lightgrey", scale_i(0.06));
        svg.draw(to_polylines(params.lower_slices_bridge_dynamic), "orange", scale_i(0.05));
        svg.draw(to_polylines(params.lower_slices_bridge_speed_small), "yellow", scale_i(0.045));
        svg.draw(to_polylines(params.lower_slices_bridge_speed_big), "lime", scale_i(0.04));
        svg.draw(to_polylines(params.lower_slices_bridge_flow_small), "cyan", scale_i(0.035));
        svg.draw(to_polylines(params.lower_slices_bridge_flow_big), "pink", scale_i(0.03));
        for (const ExtrusionPath &path : paths) {
            if (path.attributes().overhang_attributes && path.attributes().overhang_attributes->has_full_overhangs_flow) {
                svg.draw((path.as_polyline().to_polyline()), "purple", scale_i(0.02));
            } else {
                svg.draw((path.as_polyline().to_polyline()), overhang_params.debug_colors[int(path.height())], scale_i(0.02));
            }
        }
        svg.Close();
    };
    export_debug_overhangs_svg("%d_sort1_overhangs_%d_%d.svg");
#endif

    // merge path that are smaller than epsilon
    int nb_erased = 0;
    for (auto &path : paths) assert(path.length() > SCALED_EPSILON || path.size() == 2);
    while (paths.size() > 1 && paths.front().size() == 2 && paths.front().length() < coordf_t(SCALED_EPSILON)) {
        paths[1].polyline().set_front(paths.front().first_point());
        paths.erase(paths.begin());
        nb_erased++;
    }
    for (size_t idx_path = 1; idx_path < paths.size(); ++idx_path) {
        ExtrusionPath &path = paths[idx_path];
        if (path.size() == 2 && path.length() < SCALED_EPSILON) {
            paths[idx_path - 1].polyline().set_back(path.last_point());
            // del
            paths.erase(paths.begin() + idx_path);
            --idx_path;
            nb_erased++;
        } else {
            assert(paths[idx_path-1].last_point().coincides_with_epsilon(paths[idx_path].first_point()));
        }
    }

    // ensure end & start are the same exact point.
    for (int i = 1; i < paths.size(); i++) {
        // diff/inter can generate points with ~3-5 unit of diff.
        if (paths[i - 1].last_point() != paths[i].first_point()) {
            assert(paths[i - 1].last_point().distance_to_square(paths[i].first_point()) < (SCALED_EPSILON * SCALED_EPSILON * 4));
            Point middle = (paths[i - 1].last_point() + paths[i].first_point()) / 2;
            paths[i - 1].polyline().set_back(middle);
            paths[i].polyline().set_front(middle);
        }
    }
    if (overhang_params.is_loop) {
        if (paths.back().last_point() != paths.front().first_point()) {
            assert(paths.back().last_point().distance_to_square(paths.front().first_point()) < (SCALED_EPSILON * SCALED_EPSILON * 4));
            Point middle = (paths.back().last_point() + paths.front().first_point()) / 2;
            paths.back().polyline().set_back(middle);
            paths.front().polyline().set_front(middle);
        }
    }

#ifdef _DEBUGINFO
    for (size_t idx_path = 1; idx_path < paths.size(); ++idx_path) {
        assert(paths[idx_path - 1].last_point() == paths[idx_path].first_point());
    }
    if (overhang_params.is_loop) {
        ExtrusionLoop loop_test;
        loop_test.paths() = paths;
        LoopAssertVisitor(SCALED_EPSILON / 2).traverse(loop_test); // there can't have some very small paths
        assert(!paths.empty());
    }
#endif

    //bool has_normal = !ok_polylines.empty();
    //bool has_speed = !small_speed.empty() || !big_speed.empty();
    //bool has_flow = !small_flow.empty() || !big_flow.empty();

    // now, we are going to remove very small overhangs by merging them into one of their neighbor.
    // dynamic should go into normal perimeter or speed overhang
    // small speed should go into a dynamic perimeter or speed overhang.
    // big speed should go into a speed overhang or flow overhang.
    // small flow should go into a speed overhang or flow overhang.
    // big flow should only go into a flow overhang.
    const bool is_loop = overhang_params.is_loop;
    std::function<void(ExtrusionPaths &, const std::function<bool(ExtrusionPath &, ExtrusionPath &, ExtrusionPath &)> &)> foreach =
        [is_loop](ExtrusionPaths &paths, const std::function<bool(ExtrusionPath &, ExtrusionPath &, ExtrusionPath &)> &doforeach) {
            if (paths.size() > 2) {
                // follow the number from this array to get the next item to check.
                std::vector<uint32_t> sort(paths.size());
                // initialize original index locations
                std::vector<size_t> idxs(paths.size() - (is_loop ? 0 : 2));
                std::iota(idxs.begin(), idxs.end(), is_loop ? 0 : 1);
                // sort indexes based (todo: optimise plz)
                std::stable_sort(idxs.begin(), idxs.end(), [&paths](size_t i1, size_t i2) {
                    return paths[i1].length() < paths[i2].length();
                });
                if (!is_loop) {
                    sort.front() = uint32_t(-1);
                    sort.back() = uint32_t(-1);
                }
                for (uint32_t order = 0; order < uint32_t(idxs.size()); ++order) {
                    sort[idxs[order]] = order;
                }
                // now for each order
                const uint32_t end_order = uint32_t(idxs.size());
                for (uint32_t current_order = 0; current_order < end_order && paths.size() > 2; ++current_order) {
                    bool found = false;
                    assert(paths.size() == sort.size());
                    const size_t max = is_loop ? sort.size() : (sort.size() - 1);
                    for (size_t i_curr = is_loop ? 0 : 1; i_curr < max; i_curr++) {
                        assert(!found);
                        if (sort[i_curr] == current_order) {
                            found = true;
                            // found our next item to check, do the thing.
                            size_t i_prev = i_curr == 0 ? paths.size() - 1 : i_curr - 1;
                            size_t i_next = (i_curr + 1) % paths.size();
                            assert(paths[i_prev].polyline().back() == paths[i_curr].polyline().front());
                            assert(paths[i_curr].polyline().back() == paths[i_next].polyline().front());
                            if (doforeach(paths[i_prev], paths[i_curr], paths[i_next])) {
                                assert(paths[i_prev].polyline().back() == paths[i_next].polyline().front());
                                Point last = paths[i_next].polyline().back();
                                paths.erase(paths.begin() + i_curr);
                                sort.erase(sort.begin() + i_curr);
                                // can't merge same height here, as it will create a hole in sort/order
                            }
                            break;
                        }
                    }
                    assert(found);
                }
                // merge same height
                for (size_t i_curr = 0; i_curr < (is_loop ? paths.size() : (paths.size() - 1)) && paths.size() > 1; i_curr++) {
                    // found our next item to check, do the thing.
                    size_t i_next = (i_curr + 1) % paths.size();
                    assert(paths[i_curr].polyline().back() == paths[i_next].polyline().front());
                    if (paths[i_curr].height() == paths[i_next].height()) {
                        paths[i_curr].polyline().append(paths[i_next].polyline());
                        Point last = paths[i_next].polyline().back();
                        paths.erase(paths.begin() + i_next);
                        sort.erase(sort.begin() + i_next);
                        --i_curr;
                    }
                }
            }
        };

    const double min_length = dynamic_enabled ? params.perimeter_flow.scaled_width() / 2 :
                                          params.perimeter_flow.scaled_width();
    const double ok_length = params.perimeter_flow.scaled_width() * 2;

#ifdef _DEBUG_OVERHANGS
    export_debug_overhangs_svg("%d_sort2_overhangs_%d_%d.svg");
#endif
    coordf_t length_paths = 0;
    for (const ExtrusionPath &path : paths) {
        length_paths += path.length();
    }
    if (length_paths < min_length * 2) {
        // merge to biggest extrusion
        coordf_t length_normal = 0;
        coordf_t length_speed = 0;
        coordf_t length_flow = 0;
        for (const ExtrusionPath &path : paths) {
            if (path.mm3_per_mm() == params.m_mm3_per_mm_overhang) {
                length_flow += path.length();
            } else if (path.role().is_overhang()) {
                length_speed += path.length();
            } else {
                length_normal += path.length();
            }
        }
        while (paths.size() > 1) {
            paths.front().polyline().append(paths[1].polyline());
            paths.erase(paths.begin() + 1);
        }
        if (length_normal > length_speed + length_flow) {
            paths.front().attributes_mutable() = ExtrusionAttributes{
                role,
                ExtrusionFlow(overhang_params.is_external ? params.ext_mm3_per_mm() : params.mm3_per_mm(),
                    overhang_params.is_external ? params.ext_perimeter_flow.width() : params.perimeter_flow.width(),
                    0 // layer height is used as id, temporarly
            )};
        } else if (length_speed > length_flow) {
            paths.front().attributes_mutable() = ExtrusionAttributes{
                role | ExtrusionRoleModifier::ERM_Bridge,
                ExtrusionFlow(overhang_params.is_external ? params.ext_mm3_per_mm() : params.mm3_per_mm(),
                    overhang_params.is_external ? params.ext_perimeter_flow.width() : params.perimeter_flow.width(),
                    2 // layer height is used as id, temporarly
            )};
            paths.front().overhang_attributes_mutable() = ExtrusionPropertyOverhang{1, 1, 0, false, true, true, false};
        } else {
            paths.front().attributes_mutable() = ExtrusionAttributes{
                role | ExtrusionRoleModifier::ERM_Bridge,
                ExtrusionFlow(params.m_mm3_per_mm_overhang,
                    params.overhang_flow.width(),
                    4 // layer height is used as id, temporarly
            )};
            paths.front().overhang_attributes_mutable() = ExtrusionPropertyOverhang{1, 2, 0, true, true, false, false};
        }
    }
    for (int i = 1; i < paths.size(); i++) {
        assert(paths[i - 1].last_point().coincides_with_epsilon(paths[i].first_point()));
    }
    
#ifdef _DEBUG_OVERHANGS
    export_debug_overhangs_svg("%d_sort3_overhangs_%d_%d.svg");
#endif
    if (paths.size() > 2) {
        // curr will be deleted by 'foreach' (our caller, see above) if the return value is true. So its points need
        // to be merged in prev or next.
        assert(!paths.empty());
        // merge too small paths into neighbor if both same "direction"
        foreach (paths, [min_length](ExtrusionPath &prev, ExtrusionPath &curr, ExtrusionPath &next) {
            if (curr.length() < min_length) {
                // if too small between two higher overhangs,-> change to higher overhang
                if (prev.height() >= curr.height() && next.height() >= curr.height()) {
                    if (prev.height() <= next.height()) {
                        // merge to previous
                        assert(prev.last_point() == curr.first_point());
                        assert(curr.polyline().size() > 1);
                        prev.polyline().append(curr.polyline());
                    } else {
                        // merge to next
                        assert(curr.last_point() == next.first_point());
                        assert(curr.polyline().size() > 1);
                        curr.polyline().append(next.polyline());
                        next.polyline().swap(curr.polyline());
                    }
                    return true;
                } else if (prev.height() <= curr.height() && next.height() <= curr.height()) {
                    // opposite: remove too small overhangs
                    if (prev.height() > next.height()) {
                        // merge to previous
                        assert(prev.last_point() == curr.first_point());
                        assert(curr.polyline().size() > 1);
                        prev.polyline().append(curr.polyline());
                    } else {
                        // merge to next
                        assert(curr.last_point() == next.first_point());
                        assert(curr.polyline().size() > 1);
                        curr.polyline().append(next.polyline());
                        next.polyline().swap(curr.polyline());
                    }
                    return true;
                }
            }
            return false;
        });
        assert(!paths.empty());

#ifdef _DEBUG_OVERHANGS
        export_debug_overhangs_svg("%d_sort4_overhangs_%d_%d.svg");
#endif
        for (int i = 1; i < paths.size(); i++) {
            assert(paths[i - 1].last_point().coincides_with_epsilon(paths[i].first_point()));
        }

        // merge too small paths into neighbor
        foreach (paths, [min_length](ExtrusionPath &prev, ExtrusionPath &curr, ExtrusionPath &next) {
            if (curr.length() < min_length) {
                float diff_prev = std::abs(prev.height() - curr.height());
                float diff_next = std::abs(next.height() - curr.height());
                // merge to closest type, or the most overhang if equality
                bool merge_prev = diff_prev != diff_next ? diff_prev < diff_next : prev.height() > next.height();
                if (merge_prev) {
                    // merge to previous
                    assert(prev.last_point() == curr.first_point());
                    assert(curr.polyline().size() > 1);
                    prev.polyline().append(curr.polyline());
                } else {
                    // merge to next
                    assert(curr.last_point() == next.first_point());
                    assert(curr.polyline().size() > 1);
                    curr.polyline().append(next.polyline());
                    next.polyline().swap(curr.polyline());
                }
                return true;
            }
            return false;
        });
        for (int i = 1; i < paths.size(); i++) {
            assert(paths[i - 1].last_point().coincides_with_epsilon(paths[i].first_point()));
        }
#ifdef _DEBUG_OVERHANGS
        export_debug_overhangs_svg("%d_sort5_overhangs_%d_%d.svg");
#endif

        // now, there shouldn't be any paths below min_length.
        // for length
        foreach (paths, [ok_length, &params](ExtrusionPath &prev, ExtrusionPath &curr, ExtrusionPath &next) {
            if (curr.length() < ok_length) {
                if (params.m_mm3_per_mm_overhang == curr.mm3_per_mm()) {
                    // flow
                    // merge to big flow if possible.
                    if (prev.height() >= curr.height() || next.height() >= curr.height()) {
                        bool merge_prev = next.height() < curr.height() || prev.length() < next.length();
                        if (merge_prev) {
                            // merge to previous
                            assert(prev.last_point() == curr.first_point());
                            assert(curr.polyline().size() > 1);
                            prev.polyline().append(curr.polyline());
                        } else {
                            // merge to next
                            assert(curr.last_point() == next.first_point());
                            assert(curr.polyline().size() > 1);
                            curr.polyline().append(next.polyline());
                            next.polyline().swap(curr.polyline());
                        }
                        return true;
                    } else {
                        // merge to lower one if encircled
                        if (prev.height() == curr.height() - 1 && prev.height() == next.height()) {
                            if (prev.length() < next.length()) {
                                // merge to previous
                                assert(prev.last_point() == curr.first_point());
                                assert(curr.polyline().size() > 1);
                                prev.polyline().append(curr.polyline());
                            } else {
                                // merge to next
                                assert(curr.last_point() == next.first_point());
                                assert(curr.polyline().size() > 1);
                                curr.polyline().append(next.polyline());
                                next.polyline().swap(curr.polyline());
                            }
                            return true;
                        }
                    }
                } else if (curr.role().is_overhang()) {
                    // speed
                    // merge to higher one if possible, and too far away.
                    if (prev.height() >= curr.height() || next.height() >= curr.height()) {
                        bool merge_prev = next.height() < curr.height() || prev.length() < next.length();
                        if (merge_prev) {
                            // merge to previous
                            assert(prev.last_point() == curr.first_point());
                            assert(curr.polyline().size() > 1);
                            prev.polyline().append(curr.polyline());
                        } else {
                            // merge to next
                            assert(curr.last_point() == next.first_point());
                            assert(curr.polyline().size() > 1);
                            curr.polyline().append(next.polyline());
                            next.polyline().swap(curr.polyline());
                        }
                        return true;
                    } else {
                        // merge to lower one if encircled
                        if (prev.height() == curr.height() - 1 && prev.height() == next.height()) {
                            if (prev.length() < next.length()) {
                                // merge to previous
                                assert(prev.last_point() == curr.first_point());
                                assert(curr.polyline().size() > 1);
                                prev.polyline().append(curr.polyline());
                            } else {
                                // merge to next
                                assert(curr.last_point() == next.first_point());
                                assert(curr.polyline().size() > 1);
                                curr.polyline().append(next.polyline());
                                next.polyline().swap(curr.polyline());
                            }
                            return true;
                        }
                    }
                } else {
                    // normal / dynamic
                    // don't merge a small normal, it creates confusion.
                }
            }
            return false;
        });
#ifdef _DEBUG_OVERHANGS
        export_debug_overhangs_svg("%d_sort6_overhangs_%d_%d.svg");
#endif
        for (int i = 1; i < paths.size(); i++) {
            assert(paths[i - 1].last_point().coincides_with_epsilon(paths[i].first_point()));
        }

        if (overhang_params.overhang_type_2_lh[OverhangType::SMALL_FLOW_OVERHANG] >= 0) {
            size_t idx_to_merge = overhang_params.overhang_type_2_lh[OverhangType::SMALL_FLOW_OVERHANG];
            // small flow => big flow unless there is none, then merge into big speed
            foreach (paths, [idx_to_merge](ExtrusionPath &prev, ExtrusionPath &curr, ExtrusionPath &next) {
                if (curr.height() == idx_to_merge) {
                    // have to choose the rigth path
                    if (prev.height() == idx_to_merge + 1 ||
                        (prev.height() == idx_to_merge - 1 && next.height() < idx_to_merge - 1)) {
                        // merge to previous
                        assert(prev.last_point() == curr.first_point());
                        assert(curr.polyline().size() > 1);
                        prev.polyline().append(curr.polyline());
                        return true;
                    } else if (next.height() == idx_to_merge + 1 || next.height() == idx_to_merge - 1) {
                        // merge to next
                        assert(curr.last_point() == next.first_point());
                        assert(curr.polyline().size() > 1);
                        curr.polyline().append(next.polyline());
                        next.polyline().swap(curr.polyline());
                        return true;
                    } else {
                        // consider it as big flow
                        // ie do nothing
                    }
                }
                return false;
            });
        }

            // small speed => big speed unless there is none, then merge into normal (or dynamic)
        if (overhang_params.overhang_type_2_lh[OverhangType::SMALL_SPEED_OVERHANG] >= 0) {
            size_t idx_to_merge = overhang_params.overhang_type_2_lh[OverhangType::SMALL_SPEED_OVERHANG];
            foreach (paths, [idx_to_merge](ExtrusionPath &prev, ExtrusionPath &curr, ExtrusionPath &next) {
                if (curr.height() == idx_to_merge) {
                    // have to choose the rigth path
                    if (prev.height() == idx_to_merge + 1 ||
                        (prev.height() == idx_to_merge - 1 && next.height() > idx_to_merge + 1)) {
                        // merge to previous
                        assert(prev.last_point() == curr.first_point());
                        assert(curr.polyline().size() > 1);
                        prev.polyline().append(curr.polyline());
                        return true;
                    } else if (next.height() == idx_to_merge + 1 || (next.height() == idx_to_merge - 1)) {
                        // merge to next
                        assert(curr.last_point() == next.first_point());
                        assert(curr.polyline().size() > 1);
                        curr.polyline().append(next.polyline());
                        next.polyline().swap(curr.polyline());
                        return true;
                    } else {
                        // consider it as big speed
                        // ie do nothing
                    }
                }
                return false;
            });
        }
    }
    for (int i = 1; i < paths.size(); i++) {
        assert(paths[i - 1].last_point().coincides_with_epsilon(paths[i].first_point()));
    }
    if (paths.size() == 2) {
        distf_t min_length = params.perimeter_flow.scaled_width() * 2;
        if (dynamic_enabled) {
            min_length = params.perimeter_flow.scaled_width() / 2;
        }
        if (paths.front().length() < min_length) {
            paths.front().polyline().append(paths.back().polyline());
            paths.back().polyline().swap(paths.front().polyline());
            paths.erase(paths.begin());
        } else if (paths.back().length() < min_length) {
            paths.front().polyline().append(paths.back().polyline());
            paths.erase(paths.begin() + 1);
        }
    }
    for (int i = 1; i < paths.size(); i++) {
        assert(paths[i - 1].last_point().coincides_with_epsilon(paths[i].first_point()));
    }

    //now that very small paths has been merge, remove useless points
    for (ExtrusionPath &path : paths) {
        assert(!path.polyline().has_arc());
        path.polyline().make_arc(ArcFittingType::Disabled, std::max(SCALED_EPSILON * 2, scale_i(params.print_config.resolution)), 0);
        assert(!path.polyline().has_arc());
    }
    for (int i = 1; i < paths.size(); i++) {
        assert(paths[i - 1].last_point().coincides_with_epsilon(paths[i].first_point()));
    }
#ifdef _DEBUG_OVERHANGS
    export_debug_overhangs_svg("%d_sort7_overhangs_%d_%d.svg");
#endif

    //set correct height
#ifdef _DEBUG
    for (ExtrusionPath& path : paths) path.polyline().is_valid();
    assert(!paths.empty());
    // maybe not a loop?
    //Point last_pt = loop_polygons.first_point() == loop_polygons.last_point() ? paths.back().last_point() : paths.front().first_point();
#endif
    int last_type_fh = -1;
    for (size_t idx_path = 0; idx_path < paths.size(); ++idx_path) {
        ExtrusionPath& path = paths[idx_path];
        bool need_erase = !path.polyline().normalize() && paths.size() > 0;
        if (need_erase) {
            if (idx_path + 1 < paths.size()) {
                paths[idx_path + 1].polyline().append_before(path.first_point());
            } else if (idx_path > 0) {
                if (paths[idx_path - 1].last_point().coincides_with_epsilon(path.last_point())) {
                    paths[idx_path - 1].polyline().set_back(path.last_point());
                } else {
                    paths[idx_path - 1].polyline().append(path.last_point());
                }
            }
        }
        if(!need_erase && last_type_fh == int(path.attributes_mutable().height)
            && paths[idx_path-1].width() == path.width()) {
            //merge
            assert(idx_path > 0);
            assert(paths[idx_path-1].width() == path.width());
            assert(paths[idx_path-1].mm3_per_mm() == path.mm3_per_mm());
            assert(paths[idx_path-1].role() == path.role());
            need_erase = true;
            paths[idx_path-1].polyline().append(path.polyline());
#ifdef _DEBUG
            for (size_t idx = 1; idx < paths[idx_path-1].size(); ++idx) {
                assert(!is_approx(paths[idx_path-1].polyline().get_point(idx - 1), paths[idx_path-1].polyline().get_point(idx)));
            }
            //last_pt = paths[idx_path-1].last_point();
#endif
        }
        if (!need_erase) {
            last_type_fh = int(path.attributes().height);
            path.attributes_mutable().height = (path.overhang_attributes() &&
                                                path.overhang_attributes()->has_full_overhangs_flow) ?
                params.overhang_flow.height() :
                (float) params.layer->unscaled_height();
#ifdef _DEBUG
            //assert(last_pt == path.first_point());
            for (size_t idx = 1; idx < path.size(); ++idx) {
                assert(!is_approx(path.polyline().get_point(idx - 1), path.polyline().get_point(idx)));
            }
            //last_pt = path.last_point();
#endif
        } else {
            // remove this path, change the other ones to be in line.
            paths.erase(paths.begin() + idx_path);
            --idx_path;
        }
    }

    for (int i = 1; i < paths.size(); i++) {
        assert(paths[i - 1].last_point().coincides_with(paths[i].first_point()));
    }
}

ExtrusionEntityCollection PerimeterGenerator::_traverse_extrusions(const Parameters &                               params,
                                                                   std::vector<PerimeterGeneratorArachneExtrusion> &pg_extrusions)
{
    const bool CCW_contour = params.config.perimeter_direction.value == PerimeterDirection::pdCCW_CW ||  params.config.perimeter_direction.value == PerimeterDirection::pdCCW_CCW;
    const bool CCW_hole = params.config.perimeter_direction.value == PerimeterDirection::pdCW_CCW ||  params.config.perimeter_direction.value == PerimeterDirection::pdCCW_CCW;

    ExtrusionEntityCollection extrusion_coll;
    size_t biggest_inset_idx = 0;
    for (PerimeterGeneratorArachneExtrusion& pg_extrusion : pg_extrusions) {
        biggest_inset_idx = std::max(biggest_inset_idx, pg_extrusion.extrusion->inset_idx);
    }
    for (PerimeterGeneratorArachneExtrusion& pg_extrusion : pg_extrusions) {
        Arachne::ExtrusionLine* extrusion = pg_extrusion.extrusion;
        if (extrusion->is_zero_length()) {
            continue;
        }
        // possible, i guess a kind of gap fill ?
        if (!extrusion->is_closed && extrusion->junctions.front().p == extrusion->junctions.back().p) {
            //transform to loop
            extrusion->is_closed = true;
        }

        const bool    is_external = extrusion->inset_idx == 0;
        ExtrusionLoopRole loop_role = ExtrusionLoopRole::elrDefault;
        ExtrusionRole role = is_external ? ExtrusionRole::ExternalPerimeter : ExtrusionRole::Perimeter;
        if (biggest_inset_idx == extrusion->inset_idx) {
            // Note that we set loop role to ContourInternalPerimeter
            // also when loop is both internal and external (i.e.
            // there's only one contour loop).
            loop_role = (ExtrusionLoopRole)(loop_role | ExtrusionLoopRole::elrInternal | ExtrusionLoopRole::elrFirstLoop);
        }
        if (!pg_extrusion.is_contour) {
            loop_role = (ExtrusionLoopRole)(loop_role | ExtrusionLoopRole::elrHole);
        }

        // fuzzy_extrusion_line() don't work. I can use fuzzy_paths() anyway, not a big deal.
        //if (pg_extrusion.fuzzify)
        //    fuzzy_extrusion_line(*extrusion, scale_d(params.config.fuzzy_skin_thickness.value), scale_d(params.config.fuzzy_skin_point_dist.value));

        ExtrusionPaths paths;
        // detect overhanging/bridging perimeters
        if ( (params.config.overhangs_width_speed.is_enabled() || params.config.overhangs_width.is_enabled())
            && params.layer->id() > 0 && params.layer->id() >= params.object_config.raft_layers
            && !((params.object_config.support_material || params.object_config.support_material_enforce_layers > 0) &&
                params.object_config.support_material_contact_distance.value == 0)) {

            ClipperLib_Z::Path extrusion_path;
            extrusion_path.reserve(extrusion->size());
            BoundingBox extrusion_path_bbox;
            for (const Arachne::ExtrusionJunction& ej : extrusion->junctions) {
                //remove duplicate points from arachne
                if (extrusion_path.empty() ||
                    (std::abs(ej.p.x() - extrusion_path.back().x()) > SCALED_EPSILON || std::abs(ej.p.y() - extrusion_path.back().y()) > SCALED_EPSILON)) {
                    extrusion_path.emplace_back(ej.p.x(), ej.p.y(), ej.w);
                }
                extrusion_path_bbox.merge(Point{ej.p.x(), ej.p.y()});
            }
            extrusion_path_bbox.offset(SCALED_EPSILON);
            if (extrusion->is_closed) {
                assert((extrusion_path.front() - extrusion_path.back()).norm() <= SCALED_EPSILON);
                assert(Point(extrusion_path.front().x(),extrusion_path.front().y()).coincides_with_epsilon(Point(extrusion_path.back().x(), extrusion_path.back().y())));
            } else if ((extrusion_path.front() - extrusion_path.back()).norm() <= SCALED_EPSILON) {
                extrusion->is_closed = true; // fix error (yes, this happen and sohould be fixed beforehand)
            }
            paths = this->create_overhangs_arachne(params, extrusion_path, extrusion_path_bbox, role, is_external);

            // Reapply the nearest point search for starting point.
            // We allow polyline reversal because Clipper may have randomly reversed polylines during clipping.
            // Arachne sometimes creates extrusion with zero-length (just two same endpoints);
            if (!paths.empty()) {
                Point start_point = paths.front().first_point();
                if (!extrusion->is_closed) {
                    // Especially for open extrusion, we need to select a starting point that is at the start
                    // or the end of the extrusions to make one continuous line. Also, we prefer a non-overhang
                    // starting point.
                    struct PointInfo
                    {
                        size_t occurrence  = 0;
                        bool   is_overhang = false;
                    };
                    ankerl::unordered_dense::map<Point, PointInfo, PointHash> point_occurrence;
                    for (const ExtrusionPath &path : paths) {
                        ++point_occurrence[path.first_point()].occurrence;
                        ++point_occurrence[path.last_point()].occurrence;
                        if (path.role().is_bridge()) {
                            point_occurrence[path.first_point()].is_overhang = true;
                            point_occurrence[path.last_point()].is_overhang  = true;
                        }
                    }

                    // Prefer non-overhang point as a starting point.
                    for (const std::pair<Point, PointInfo> &pt : point_occurrence)
                        if (pt.second.occurrence == 1) {
                            start_point = pt.first;
                            if (!pt.second.is_overhang) {
                                start_point = pt.first;
                                break;
                            }
                        }
                }

                chain_and_reorder_extrusion_paths(paths, &start_point);
                for(size_t i = 1; i< paths.size(); ++i) assert(paths[i-1].last_point().coincides_with_epsilon(paths[i].first_point()));
                if (extrusion->is_closed) {
                    assert(paths.back().last_point().coincides_with_epsilon(paths.front().first_point()));
                } else {
                    assert(!paths.back().last_point().coincides_with_epsilon(paths.front().first_point()));
                }
            }
        } else {
            append(paths, Geometry::unsafe_variable_width(Arachne::to_thick_polyline(*extrusion),
                role,
                is_external ? params.ext_perimeter_flow : params.perimeter_flow,
                std::max(params.ext_perimeter_flow.scaled_width() / 4, scale_i(params.print_config.resolution)),
                (is_external ? params.ext_perimeter_flow : params.perimeter_flow).scaled_width() / 10));
        }

        // test check
        if (!paths.empty()) {
            for (size_t idx_path = 0; idx_path < paths.size(); ++idx_path) {
                if (idx_path > 0) {
                    assert(paths[idx_path - 1].last_point().coincides_with_epsilon(paths[idx_path].first_point()));
                }
                for (size_t idx_pt = 1; idx_pt < paths[idx_path].size(); ++idx_pt) {
                    assert(!paths[idx_path].polyline().get_point(idx_pt - 1).coincides_with_epsilon(paths[idx_path].polyline().get_point(idx_pt)));
                }
            }
        }

        // Apply fuzzify
        if (!paths.empty() && pg_extrusion.fuzzify) {
            double nozle_diameter = is_external ? params.ext_perimeter_flow.nozzle_diameter() : params.perimeter_flow.nozzle_diameter();
            double fuzzy_skin_thickness = params.config.fuzzy_skin_thickness.get_effective_value(nozle_diameter);
            double fuzzy_skin_point_dist = params.config.fuzzy_skin_point_dist.get_effective_value(nozle_diameter);
           fuzzy_paths(paths, scale_d(fuzzy_skin_thickness), scale_d(fuzzy_skin_point_dist));
        }

        //set to overhang speed if any chunk is overhang
        this->_enforce_speed_overhangs(paths, -1);

        // check for seam_slope_type tag need to be applied (need the extrusion)
        if (is_external &&
            (params.region_setting.has_many_config(&params.config.seam_slope_type) ||
             params.region_setting.get_solo_config(&params.config.seam_slope_type).get_int() != int32_t(SeamScarfType::None))) {
            this->throw_if_canceled();
            for (auto const &[opt_values, areas] :
                 params.region_setting.get_areas(&params.config.seam_slope_type)) {
                // first test if applicable
                SeamScarfType scarf_type = SeamScarfType(opt_values.get_int(&params.config.seam_slope_type));
                if (scarf_type != SeamScarfType::None &&
                    opt_values.get_bool(&params.config.external_perimeters_first)){
                    // should be okay.
                    // next test
                    if (opt_values.get_bool(&params.config.external_perimeters_first_force) ||
                        (pg_extrusion.is_contour &&
                         opt_values.get_bool(&params.config.external_perimeters_nothole)) ||
                        (!pg_extrusion.is_contour && opt_values.get_bool(&params.config.external_perimeters_hole) && scarf_type == SeamScarfType::All)) {
                        // does a part is inside?
                        bool inside = areas.is_accept_all();
                        if (!inside) {
                            for (const ExtrusionPath &path : paths) {
                                if (!intersection_pl(path.as_polyline().to_polyline(), areas.expolys).empty()) {
                                    inside = true;
                                    break;
                                }
                            }
                        }
                        if (inside) {
                            // then apply the tag
                            loop_role = (ExtrusionLoopRole) (loop_role | ExtrusionLoopRole::elrVase);
                        }
                    }
                }
            }
        }
        if (params.config.seam_slope_type.value != SeamScarfType::None && params.config.external_perimeters_first.value && is_external) {
            if ((pg_extrusion.is_contour && params.config.external_perimeters_nothole.value) || (!pg_extrusion.is_contour && params.config.external_perimeters_hole.value)) {
                loop_role = (ExtrusionLoopRole)(loop_role | ExtrusionLoopRole::elrVase);
            }
        }

        // Append paths to collection.
        if (!paths.empty()) {
            // test check
            for (size_t idx_path = 0; idx_path < paths.size(); ++idx_path) {
                if (idx_path > 0) {
                    assert(paths[idx_path - 1].last_point().coincides_with_epsilon(paths[idx_path].first_point()));
                }
                for (size_t idx_pt = 1; idx_pt < paths[idx_path].size(); ++idx_pt) {
                    assert(!paths[idx_path].polyline().get_point(idx_pt - 1).coincides_with_epsilon(paths[idx_path].polyline().get_point(idx_pt)));
                }
            }
            if (extrusion->is_closed) {
                assert(paths.back().last_point().coincides_with_epsilon(paths.front().first_point()));
                ExtrusionLoop extrusion_loop(std::move(paths), loop_role);
                // Restore the orientation of the extrusion loop.
                //TODO: use if (loop.is_steep_overhang && params.layer->id() % 2 == 1) to make_clockwise => need to detect is_steep_overhang on the arachne path
                bool need_ccw = ((params.config.perimeter_reverse /*|| pg_extrusion.is_steep_overhang*/ && params.layer->id() % 2 == 1)
                                 == (pg_extrusion.is_contour ? CCW_contour : CCW_hole));
                if (need_ccw != extrusion_loop.is_clockwise()) {
                    extrusion_loop.reverse();
                }
#if _DEBUG
                for (auto it = std::next(extrusion_loop.paths().begin()); it != extrusion_loop.paths().end(); ++it) {
                    assert(it->polyline().size() >= 2);
                    assert(std::prev(it)->last_point() == it->first_point());
                }
                // first & last points can be very near each other but sometimes not exactly.
                assert(extrusion_loop.paths().front().first_point().coincides_with_epsilon(extrusion_loop.paths().back().last_point()));
#endif
                //ensure the start & end points are the same.
                extrusion_loop.paths().front().polyline().set_front(extrusion_loop.paths().back().last_point());
                assert(extrusion_loop.paths().front().first_point() == (extrusion_loop.paths().back().last_point()));

                extrusion_coll.append(std::move(extrusion_loop));
            } else {
                assert(!paths.back().last_point().coincides_with_epsilon(paths.front().first_point()));

                // Because we are processing one ExtrusionLine all ExtrusionPaths should form one connected path.
                // But there is possibility that due to numerical issue there is poss
                assert([&paths = std::as_const(paths)]() -> bool {
                    for (auto it = std::next(paths.begin()); it != paths.end(); ++it)
                        if (std::prev(it)->last_point() != it->first_point())
                            return false;
                    return true;
                }());
                ExtrusionMultiPath multi_path;
                multi_path.paths().push_back(std::move(paths.front()));
                multi_path.set_can_reverse(true);

                for (auto it_path = std::next(paths.begin()); it_path != paths.end(); ++it_path) {
                    if (!multi_path.paths().back().last_point().coincides_with_epsilon(it_path->first_point())) {
                        extrusion_coll.append(std::move(multi_path));
                        multi_path = ExtrusionMultiPath();
                        multi_path.set_can_reverse(true);
                    }
                    it_path->set_can_reverse(false);
                    multi_path.paths().push_back(std::move(*it_path));
                }

                extrusion_coll.append(ExtrusionMultiPath(std::move(multi_path)));
            }
        }
    }

    return extrusion_coll;
}

void convert_to_clipperpath_with_bbox(const Polygons& source, const BoundingBox& extrusion_path_bbox, ClipperLib_Z::Paths& dest) {
    dest.clear();
    dest.reserve(source.size());
    Polygon clipped;
    for (const Polygon& poly : source) {
        clipped.clear();
        ClipperUtils::clip_clipper_polygon_with_subject_bbox(poly, extrusion_path_bbox, clipped);
        if (! clipped.empty()) {
            dest.emplace_back();
            ClipperLib_Z::Path& out = dest.back();
            out.reserve(clipped.points.size());
            for (const Point& pt : clipped.points)
                out.emplace_back(pt.x(), pt.y(), 0);
        }
    }
    //TODO: verify it doesn't need any union_ to fix ccw and cw intersect
}

#ifdef _DEBUG
void test_overhangs(const ClipperLib_Z::Paths& path1, const ClipperLib_Z::Paths& path2, Points &outer_points)
{
    for (const ClipperLib_Z::Path &poly : path1)
        for (int i = 0; i < poly.size() - 1; i++)
            assert(poly[i] != poly[i + 1]);
    for (const ClipperLib_Z::Path &poly : path2)
        for (int i = 0; i < poly.size() - 1; i++)
            assert(poly[i] != poly[i + 1]);
    // check if points are equal
    //Points path2_points;
    //Points path1_points;
    //for (auto &line : path2)
    //    for (auto &pt : line) path2_points.emplace_back(coord_t(pt.x()), coord_t(pt.y()));
    //for (auto &line : path1)
    //    for (auto &pt : line) path1_points.emplace_back(coord_t(pt.x()), coord_t(pt.y()));
    //for (Point &pt : path2_points) {
    //    bool found    = false;
    //    bool in_outer = false;
    //    Point pt2_almost;
    //    for (Point &pt2 : path1_points)
    //        if (pt.coincides_with_epsilon(pt2)) {
    //            found = true;
    //            pt2_almost = pt2;
    //            break;
    //        }
    //    for (Point &pt2 : outer_points)
    //        if (pt.coincides_with_epsilon(pt2)) {
    //            found    = true;
    //            in_outer = true;
    //            pt2_almost = pt2;
    //            break;
    //        }
    //    assert(found);
    //    found    = false;
    //    in_outer = false;
    //    for (Point &pt2 : path1_points)
    //        if (pt.coincides_with(pt2)) {
    //            found = true;
    //            break;
    //        }
    //    for (Point &pt2 : outer_points)
    //        if (pt.coincides_with(pt2)) {
    //            found    = true;
    //            in_outer = true;
    //            break;
    //        }
    //    assert(found);
    //}
    //  => points can be different from diff & intersect
    // TODO: create a new operation that create the diff & intersect at the same time
}
#endif

struct cmpClipperLib_Z {
    bool operator()(const ClipperLib_Z::IntPoint& a, const ClipperLib_Z::IntPoint& b) const {
        return a.x() == b.x() ? a.y() == b.y() ? a.z() < b.z() : a.x() < b.x() : a.x() < b.x();
    }
};

bool is_length_more_than_epsilon(ClipperLib_Z::Path &path) {
    coordf_t length = 0;
    for (size_t i = 1; i < path.size(); i++) {
        length += (path[i - 1] - path[i]).cast<coordf_t>().norm();
        if (length > SCALED_EPSILON) {
            return true;
        }
    }
    return false;
}

bool merge_path(const ClipperLib_Z::Path &tomerge, ClipperLib_Z::Paths &receiver) {
#ifdef _DEBUG
    {
        // check there seems to be a continous path from start to end
        const ClipperLib_Z::Path &path = tomerge;
        bool found_another_path_after = false;
        bool found_another_path_before = false;
        bool found_almost_another_path_after = false;
        bool found_almost_another_path_before = false;
        int other_paths_count = 0;
        for (size_t idx_path2 = 0; idx_path2 < receiver.size(); ++idx_path2) {
            other_paths_count++;
            found_another_path_after = found_another_path_after || (path.back() == receiver[idx_path2].front());
            found_another_path_before = found_another_path_before || (path.front() == receiver[idx_path2].back());
            found_almost_another_path_after = found_almost_another_path_after || (path.back() - receiver[idx_path2].front()).cast<coordf_t>().norm() < SCALED_EPSILON;
            found_almost_another_path_before = found_almost_another_path_before || (path.front() - receiver[idx_path2].back()).cast<coordf_t>().norm() < SCALED_EPSILON;
        }
        bool found_another_path_after_strict = found_another_path_after;
        bool found_another_path_before_strict = found_another_path_before;
        bool found_almost_another_path_after_strict = found_almost_another_path_after;
        bool found_almost_another_path_before_strict = found_almost_another_path_before;
        //assert(other_paths_count == 0 || found_another_path_after || found_another_path_before);
        for (size_t idx_path2 = 0; idx_path2 < receiver.size(); ++idx_path2) {
            found_another_path_after = found_another_path_after || path.back() == receiver[idx_path2].front() || path.back() == receiver[idx_path2].back();
            found_another_path_before = found_another_path_before || path.front() == receiver[idx_path2].back() || path.front() == receiver[idx_path2].front();
            found_almost_another_path_after = found_almost_another_path_after || (path.back() - receiver[idx_path2].back()).cast<coordf_t>().norm() < SCALED_EPSILON; 
            found_almost_another_path_before = found_almost_another_path_before || (path.front() - receiver[idx_path2].front()).cast<coordf_t>().norm() < SCALED_EPSILON;
        }
        assert(other_paths_count == 0 || found_another_path_after_strict || found_another_path_before_strict);
    }
#endif
    size_t idx_first;
    bool found_first = false;
    // search start
    for (idx_first = 0; idx_first < receiver.size(); ++idx_first) {
        if (receiver[idx_first].back() == tomerge.front()) {
            found_first = true;
            receiver[idx_first].insert(receiver[idx_first].end(), tomerge.begin() + 1, tomerge.end());
            break;
        }
    }
    bool found_last = false;
    if (found_first) {
        //find the last, add it and remove it.
        size_t idx_last;
        for (idx_last = 0; idx_last < receiver.size(); ++idx_last) {
            if (idx_last == idx_first) {
                continue;
            }
            if (receiver[idx_last].front() == receiver[idx_first].back()) {
                found_last = true;
                receiver[idx_first].insert(receiver[idx_first].end(), receiver[idx_last].begin() + 1, receiver[idx_last].end());
                receiver.erase(receiver.begin() + idx_last);
                break;
            }
        }
    }
    return found_first && found_last;
}

//TODO: transform to ExtrusionMultiPath instead of ExtrusionPaths
ExtrusionPaths PerimeterGenerator::create_overhangs_arachne(const Parameters &        params,
                                                            const ClipperLib_Z::Path &arachne_path,
                                                            const BoundingBox &       extrusion_path_bbox,
                                                            const ExtrusionRole       role,
                                                            const bool                is_external) const
{
#ifdef _DEBUG
    Point prev = Point{ arachne_path.front().x(), arachne_path.front().y() };
    for (size_t i = 1; i < arachne_path.size(); ++i) {
        Point next = Point{ arachne_path[i].x(), arachne_path[i].y() };
        assert(!prev.coincides_with_epsilon(next));
        prev = next;
    }
#endif
    ExtrusionPaths paths;
    coord_t resolution = std::max(SCALED_EPSILON, this->get_resolution(0,false));
    const bool is_loop = Point{ arachne_path.front().x(), arachne_path.front().y() }.coincides_with_epsilon(Point{ arachne_path.back().x(), arachne_path.back().y() });
    // if no overhang detection
    if (params.lower_slices_bridge_dynamic.empty() ) {
        //error
        append(paths, Geometry::unsafe_variable_width(Arachne::to_thick_polyline(arachne_path),
            role,
            is_external ? params.ext_perimeter_flow : params.perimeter_flow,
            std::max(params.ext_perimeter_flow.scaled_width() / 4, scale_i(params.print_config.resolution)),
            (is_external ? params.ext_perimeter_flow : params.perimeter_flow).scaled_width() / 10));
        //(const ThickPolyline& polyline, const ExtrusionRole role, const Flow& flow, const coord_t resolution_internal, const coord_t tolerance)
        for (ExtrusionPath& path : paths) {
            //these variable_width paths aren't gapfill, they are proper perimeters
            path.set_can_reverse(is_loop);
        }
        return paths;

    }
    //set the fan & speed before the flow
    ClipperLib_Z::Paths ok_polylines = { arachne_path };

    ClipperLib_Z::Paths dynamic_speed;
    ClipperLib_Z::Paths small_speed;
    ClipperLib_Z::Paths big_speed;
    ClipperLib_Z::Paths small_flow;
    ClipperLib_Z::Paths big_flow;
#ifdef _DEBUG
    for (ClipperLib_Z::Path& poly : ok_polylines)
        for (int i = 0; i < poly.size() - 1; i++)
            assert(poly[i] != poly[i + 1]);
#endif
    ClipperLib_Z::Paths clipped_zpaths;

    ClipperLib_Z::Paths* previous = &ok_polylines;

    assert(!params.lower_slices_bridge_dynamic.empty());
    convert_to_clipperpath_with_bbox(params.lower_slices_bridge_dynamic, extrusion_path_bbox, clipped_zpaths);
    if (!clipped_zpaths.empty()) {
#ifdef _DEBUG
        Points outer_points;
        for (auto &line : *previous)
            for (auto &pt : line)
                outer_points.emplace_back(coord_t(pt.x()), coord_t(pt.y()));
#endif
        dynamic_speed = clip_extrusion(*previous, clipped_zpaths, ClipperLib_Z::ctDifference);
#ifdef _DEBUG
        for (ClipperLib_Z::Path &poly : dynamic_speed) // assert dynamic_speed
            for (int i = 0; i < poly.size() - 1; i++)  // assert dynamic_speed
                assert(poly[i] != poly[i + 1]);        // assert dynamic_speed
#endif
        if (!dynamic_speed.empty()) {
            *previous = clip_extrusion(*previous, clipped_zpaths, ClipperLib_Z::ctIntersection);
#ifdef _DEBUG
            test_overhangs(dynamic_speed, *previous, outer_points);
            test_overhangs(*previous, dynamic_speed, outer_points);
            // for (ClipperLib_Z::Path &poly : dynamic_speed) {
            //    assert(poly.size() > 1);
            //    assert(is_length_more_than_epsilon(poly));
            //}
            // for (ClipperLib_Z::Path &poly : *previous) {
            //    assert (poly.size() > 1);
            //    assert(is_length_more_than_epsilon(poly));
            //}
#endif
            // merge epsilon-length from dynamic_speed into previous
            for (size_t path_idx = 0; path_idx < dynamic_speed.size(); ++path_idx) {
                ClipperLib_Z::Path &poly = dynamic_speed[path_idx];
                if (!is_length_more_than_epsilon(poly)) {
                    merge_path(poly, *previous); // TODO
                    dynamic_speed.erase(dynamic_speed.begin() + path_idx);
                    path_idx--;
                }
            }
            for (size_t path_idx = 0; path_idx < previous->size(); ++path_idx) {
                ClipperLib_Z::Path &poly = (*previous)[path_idx];
                if (!is_length_more_than_epsilon(poly)) {
                    merge_path(poly, dynamic_speed); // TODO
                    previous->erase(previous->begin() + path_idx);
                    path_idx--;
                }
            }
#ifdef _DEBUG
            for (ClipperLib_Z::Path &poly : dynamic_speed) {
                assert(poly.size() > 1);
                assert(is_length_more_than_epsilon(poly));
            }
            for (ClipperLib_Z::Path &poly : *previous) {
                assert(poly.size() > 1);
                assert(is_length_more_than_epsilon(poly));
            }
#endif
            previous = &dynamic_speed;
        }
    } else {
        dynamic_speed = std::move(*previous);
        previous->clear();
        previous = &dynamic_speed;
    }

    assert(!params.lower_slices_bridge_speed_small.empty());
    convert_to_clipperpath_with_bbox(params.lower_slices_bridge_speed_small, extrusion_path_bbox, clipped_zpaths);
    if (!clipped_zpaths.empty()) {
        //small_speed = diff_pl(*previous, this->params.lower_slices_bridge_speed_small);
#ifdef _DEBUG
        Points outer_points;
        for(auto & line: *previous) for(auto &pt : line) outer_points.emplace_back(coord_t(pt.x()), coord_t(pt.y()));
#endif
        small_speed = clip_extrusion(*previous, clipped_zpaths, ClipperLib_Z::ctDifference);
#ifdef _DEBUG
        for (ClipperLib_Z::Path& poly : small_speed) //                       assert small_speed
            for (int i = 0; i < poly.size() - 1; i++) //     assert small_speed
                assert(poly[i] != poly[i + 1]); //    assert small_speed
#endif
        if (!small_speed.empty()) {
            *previous = clip_extrusion(*previous, clipped_zpaths, ClipperLib_Z::ctIntersection);
#ifdef _DEBUG
            test_overhangs(small_speed, *previous, outer_points);
            test_overhangs(*previous, small_speed, outer_points);
#endif
            // merge epsilon-length from small_speed into previous
            for (size_t path_idx = 0; path_idx < small_speed.size(); ++path_idx) {
                ClipperLib_Z::Path &poly = small_speed[path_idx];
                if (!is_length_more_than_epsilon(poly)) {
                    merge_path(poly, *previous); //TODO
                    small_speed.erase(small_speed.begin() + path_idx);
                    path_idx--;
                }
            }
            for (size_t path_idx = 0; path_idx < previous->size(); ++path_idx) {
                ClipperLib_Z::Path &poly = (*previous)[path_idx];
                if (!is_length_more_than_epsilon(poly)) {
                    merge_path(poly, small_speed); //TODO
                    previous->erase(previous->begin() + path_idx);
                    path_idx--;
                }
            }
            previous = &small_speed;
        }
    } else {
        small_speed = std::move(*previous);
        previous->clear();
        previous = &small_speed;
    }

    assert(!params.lower_slices_bridge_speed_big.empty());
#ifdef _DEBUG
    Points outer_points;
    for(auto & line: *previous) for(auto &pt : line) outer_points.emplace_back(coord_t(pt.x()), coord_t(pt.y()));
#endif
    convert_to_clipperpath_with_bbox(params.lower_slices_bridge_speed_big, extrusion_path_bbox, clipped_zpaths);
    if (!clipped_zpaths.empty()) {
        big_speed = clip_extrusion(*previous, clipped_zpaths, ClipperLib_Z::ctDifference);
#ifdef _DEBUG
        for (ClipperLib_Z::Path& poly : big_speed) //                         assert big_speed
            for (int i = 0; i < poly.size() - 1; i++) //     assert big_speed
                assert(poly[i] != poly[i + 1]); //    assert big_speed
#endif
        if (!big_speed.empty()) {
            *previous = clip_extrusion(*previous, clipped_zpaths, ClipperLib_Z::ctIntersection);
#ifdef _DEBUG
            test_overhangs(big_speed, *previous, outer_points);
            test_overhangs(*previous, big_speed, outer_points);
#endif
            // merge epsilon-length from big_speed into previous
            for (size_t path_idx = 0; path_idx < big_speed.size(); ++path_idx) {
                ClipperLib_Z::Path &poly = big_speed[path_idx];
                if (!is_length_more_than_epsilon(poly)) {
                    merge_path(poly, *previous); //TODO
                    big_speed.erase(big_speed.begin() + path_idx);
                    path_idx--;
                }
            }
            for (size_t path_idx = 0; path_idx < previous->size(); ++path_idx) {
                ClipperLib_Z::Path &poly = (*previous)[path_idx];
                if (!is_length_more_than_epsilon(poly)) {
                    merge_path(poly, big_speed); //TODO
                    previous->erase(previous->begin() + path_idx);
                    path_idx--;
                }
            }
            previous = &big_speed;
        }
    } else {
        big_speed = std::move(*previous);
        previous->clear();
        previous = &big_speed;
    }

    assert(!params.lower_slices_bridge_flow_small.empty());
#ifdef _DEBUG
    outer_points.clear();
    for(auto & line: *previous) for(auto &pt : line) outer_points.emplace_back(coord_t(pt.x()), coord_t(pt.y()));
#endif
    convert_to_clipperpath_with_bbox(params.lower_slices_bridge_flow_small, extrusion_path_bbox, clipped_zpaths);
    if (!clipped_zpaths.empty()) {
        small_flow = clip_extrusion(*previous, clipped_zpaths, ClipperLib_Z::ctDifference);
#ifdef _DEBUG
        for (ClipperLib_Z::Path& poly : small_flow) //                        assert small_flow
            for (int i = 0; i < poly.size() - 1; i++) //     assert small_flow
                assert(poly[i] != poly[i + 1]); //    assert small_flow
#endif
        if (!small_flow.empty()) {
            *previous = clip_extrusion(*previous, clipped_zpaths, ClipperLib_Z::ctIntersection);
#ifdef _DEBUG
            test_overhangs(small_flow, *previous, outer_points);
            test_overhangs(*previous, small_flow, outer_points);
#endif
            // merge epsilon-length from small_flow into previous
            for (size_t path_idx = 0; path_idx < small_flow.size(); ++path_idx) {
                ClipperLib_Z::Path &poly = small_flow[path_idx];
                if (!is_length_more_than_epsilon(poly)) {
                    merge_path(poly, *previous); //TODO
                    small_flow.erase(small_flow.begin() + path_idx);
                    path_idx--;
                }
            }
            for (size_t path_idx = 0; path_idx < previous->size(); ++path_idx) {
                ClipperLib_Z::Path &poly = (*previous)[path_idx];
                if (!is_length_more_than_epsilon(poly)) {
                    merge_path(poly, small_flow); //TODO
                    previous->erase(previous->begin() + path_idx);
                    path_idx--;
                }
            }
            previous = &small_flow;
        }
    } else {
        small_flow = std::move(*previous);
        previous->clear();
        previous = &small_flow;
    }

    assert(!params.lower_slices_bridge_flow_small.empty());
#ifdef _DEBUG
    outer_points.clear();
    for(auto & line: *previous) for(auto &pt : line) outer_points.emplace_back(coord_t(pt.x()), coord_t(pt.y()));
#endif
    convert_to_clipperpath_with_bbox(params.lower_slices_bridge_flow_big, extrusion_path_bbox, clipped_zpaths);
    if (!clipped_zpaths.empty()) {
        big_flow = clip_extrusion(*previous, clipped_zpaths, ClipperLib_Z::ctDifference);
#ifdef _DEBUG
        for (ClipperLib_Z::Path& poly : big_flow) //                          assert big_flow
            for (int i = 0; i < poly.size() - 1; i++) //     assert big_flow
                assert(poly[i] != poly[i + 1]); //    assert big_flow
#endif
        if (!big_flow.empty()) {
            *previous = clip_extrusion(*previous, clipped_zpaths, ClipperLib_Z::ctIntersection);
#ifdef _DEBUG
            test_overhangs(big_flow, *previous, outer_points);
            test_overhangs(*previous, big_flow, outer_points);
#endif
            // merge epsilon-length from big_flow into previous
            for (size_t path_idx = 0; path_idx < big_flow.size(); ++path_idx) {
                ClipperLib_Z::Path &poly = big_flow[path_idx];
                if (!is_length_more_than_epsilon(poly)) {
                    merge_path(poly, *previous); //TODO
                    big_flow.erase(big_flow.begin() + path_idx);
                    path_idx--;
                }
            }
            for (size_t path_idx = 0; path_idx < previous->size(); ++path_idx) {
                ClipperLib_Z::Path &poly = (*previous)[path_idx];
                if (!is_length_more_than_epsilon(poly)) {
                    merge_path(poly, big_flow); //TODO
                    previous->erase(previous->begin() + path_idx);
                    path_idx--;
                }
            }
            previous = &big_flow;
        }
    } else {
        big_flow = std::move(*previous);
        previous->clear();
        previous = &big_flow;
    }

    // ensure polylines are valid (at least EPSILON between two points), unless the path is itself shorter than epsilon (then it's two points)
    for (ClipperLib_Z::Paths *polylines : {&ok_polylines, &dynamic_speed, &small_speed, &big_speed, &small_flow, &big_flow}) {
        for (ClipperLib_Z::Path &poly : *polylines) {
            if (poly.size() == 1) {
                // this polyline can be removed
                assert(false);
            } else {
                assert(poly.size() >= 2);
                assert(is_length_more_than_epsilon(poly));
                ClipperLib_Z::Path old_poly = poly;
                auto it_end = Slic3r::douglas_peucker_impl(
                    poly.begin(), poly.end(), poly.begin(), double(SCALED_EPSILON),
                    [](const ClipperLib_Z::IntPoint &p) { return Point(p.x(), p.y()); });
                assert(it_end <= poly.end());
                poly.resize(std::distance(poly.begin(), it_end));
                assert(poly.size() >= 2);
            }
        }
    }

    //note: layer height is used to identify the path type
    int idx_lh_size = 0;
    Params_sort_overhangs overhang_params;
    if (!ok_polylines.empty()) {
        //fast track
        if (dynamic_speed.empty() && small_speed.empty() && big_speed.empty() && small_flow.empty() && big_flow.empty()) {
            ExtrusionPaths thickpaths = Geometry::unsafe_variable_width(Arachne::to_thick_polyline(arachne_path),
                    role,
                    is_external ? params.ext_perimeter_flow : params.perimeter_flow,
                    std::max(params.ext_perimeter_flow.scaled_width() / 4, scale_i(params.print_config.resolution)),
                    (is_external ? params.ext_perimeter_flow : params.perimeter_flow).scaled_width() / 10);
#ifdef _DEBUG
            for (int i = 1; i < thickpaths.size(); i++) {
                assert(thickpaths[i - 1].last_point() == thickpaths[i].first_point());
            }
#endif
            // thickpaths can be empty if extrusion_path is too short
            assert(thickpaths.empty() || thickpaths.front().first_point().x() == arachne_path.front().x());
            assert(thickpaths.empty() || thickpaths.front().first_point().y() == arachne_path.front().y());
            assert(thickpaths.empty() || thickpaths.back().last_point().x() == arachne_path.back().x());
            assert(thickpaths.empty() || thickpaths.back().last_point().y() == arachne_path.back().y());
            for (ExtrusionPath& path : thickpaths) {
                path.set_can_reverse(!is_loop);
                paths.push_back(std::move(path));
            }
            return paths;
        }
        for (const ClipperLib_Z::Path& extrusion_path : ok_polylines) {
            auto thick_poly = Arachne::to_thick_polyline(extrusion_path);
            ExtrusionPaths thickpaths = Geometry::unsafe_variable_width(thick_poly,
                    role,
                    is_external ? params.ext_perimeter_flow : params.perimeter_flow,
                    std::max(params.ext_perimeter_flow.scaled_width() / 4, scale_i(params.print_config.resolution)),
                    (is_external ? params.ext_perimeter_flow : params.perimeter_flow).scaled_width() / 10);
#ifdef _DEBUG
            for (int i = 1; i < thickpaths.size(); i++) {
                assert(thickpaths[i - 1].last_point() == thickpaths[i].first_point());
            }
#endif
            // thickpaths can be empty if extrusion_path is too short
            assert(thickpaths.empty() || thickpaths.front().first_point().x() == extrusion_path.front().x());
            assert(thickpaths.empty() || thickpaths.front().first_point().y() == extrusion_path.front().y());
            assert(thickpaths.empty() || thickpaths.back().last_point().x() == extrusion_path.back().x());
            assert(thickpaths.empty() || thickpaths.back().last_point().y() == extrusion_path.back().y());
            for (ExtrusionPath& path : thickpaths) {
                path.set_can_reverse(!is_loop);
                path.attributes_mutable().height = idx_lh_size;
                paths.push_back(std::move(path));
            }
        }
        overhang_params.overhang_type_2_lh.push_back(idx_lh_size);
        idx_lh_size++;
    } else {
        overhang_params.overhang_type_2_lh.push_back(-1);
    }
    bool has_dynamic = false;
    if (!dynamic_speed.empty()) {
        has_dynamic =true;
        for (const ClipperLib_Z::Path& extrusion_path : dynamic_speed) {
            if (extrusion_path.size() <= 1)
                continue;
            ExtrusionPaths thickpaths = Geometry::unsafe_variable_width(Arachne::to_thick_polyline(extrusion_path),
                    role | ExtrusionRoleModifier::ERM_Bridge,
                    is_external ? params.ext_perimeter_flow : params.perimeter_flow,
                    std::max(params.ext_perimeter_flow.scaled_width() / 4, scale_i(params.print_config.resolution)),
                    (is_external ? params.ext_perimeter_flow : params.perimeter_flow).scaled_width() / 10);
#ifdef _DEBUG
            for (int i = 1; i < thickpaths.size(); i++) {
                assert(thickpaths[i - 1].last_point() == thickpaths[i].first_point());
            }
#endif
            // thickpaths can be empty if extrusion_path is too short
            assert(thickpaths.empty() || thickpaths.front().first_point().x() == extrusion_path.front().x());
            assert(thickpaths.empty() || thickpaths.front().first_point().y() == extrusion_path.front().y());
            assert(thickpaths.empty() || thickpaths.back().last_point().x() == extrusion_path.back().x());
            assert(thickpaths.empty() || thickpaths.back().last_point().y() == extrusion_path.back().y());
            for (ExtrusionPath& path : thickpaths) {
                path.set_can_reverse(!is_loop);
                path.attributes_mutable().height = idx_lh_size;
                path.overhang_attributes_mutable() = ExtrusionPropertyOverhang{0, 1, 0, false, false, true, true};
                paths.push_back(std::move(path));
            }
        }
        overhang_params.overhang_type_2_lh.push_back(idx_lh_size);
        idx_lh_size++;
    } else {
        overhang_params.overhang_type_2_lh.push_back(-1);
    }
    if (!small_speed.empty()) {
        for (const ClipperLib_Z::Path& extrusion_path : small_speed) {
            if(extrusion_path.size() <= 1)
                continue;
            ExtrusionPaths thickpaths = Geometry::unsafe_variable_width(Arachne::to_thick_polyline(extrusion_path),
                    role | ExtrusionRoleModifier::ERM_Bridge,
                    is_external ? params.ext_perimeter_flow : params.perimeter_flow,
                    std::max(params.ext_perimeter_flow.scaled_width() / 4, scale_i(params.print_config.resolution)),
                    (is_external ? params.ext_perimeter_flow : params.perimeter_flow).scaled_width() / 10);
#ifdef _DEBUG
            for (int i = 1; i < thickpaths.size(); i++) {
                assert(thickpaths[i - 1].last_point() == thickpaths[i].first_point());
            }
#endif
            // thickpaths can be empty if extrusion_path is too short
            assert(thickpaths.empty() || thickpaths.front().first_point().x() == extrusion_path.front().x());
            assert(thickpaths.empty() || thickpaths.front().first_point().y() == extrusion_path.front().y());
            assert(thickpaths.empty() || thickpaths.back().last_point().x() == extrusion_path.back().x());
            assert(thickpaths.empty() || thickpaths.back().last_point().y() == extrusion_path.back().y());
            for (ExtrusionPath& path : thickpaths) {
                path.set_can_reverse(!is_loop);
                path.attributes_mutable().height = idx_lh_size;
                path.overhang_attributes_mutable() = ExtrusionPropertyOverhang{1, 1, 0, false, true, true, false};
                paths.push_back(std::move(path));
            }
        }
        overhang_params.overhang_type_2_lh.push_back(idx_lh_size);
        idx_lh_size++;
    } else {
        overhang_params.overhang_type_2_lh.push_back(-1);
    }
    if (!big_speed.empty()) {
        for (const ClipperLib_Z::Path& extrusion_path : big_speed) {
            if(extrusion_path.size() <= 1)
                continue;
            ExtrusionPaths thickpaths = Geometry::unsafe_variable_width(Arachne::to_thick_polyline(extrusion_path),
                    role | ExtrusionRoleModifier::ERM_Bridge,
                    is_external ? params.ext_perimeter_flow : params.perimeter_flow,
                    std::max(params.ext_perimeter_flow.scaled_width() / 4, scale_i(params.print_config.resolution)),
                    (is_external ? params.ext_perimeter_flow : params.perimeter_flow).scaled_width() / 10);
#ifdef _DEBUG
            for (int i = 1; i < thickpaths.size(); i++) {
                assert(thickpaths[i - 1].last_point() == thickpaths[i].first_point());
            }
#endif
            // thickpaths can be empty if extrusion_path is too short
            assert(thickpaths.empty() || thickpaths.front().first_point().x() == extrusion_path.front().x());
            assert(thickpaths.empty() || thickpaths.front().first_point().y() == extrusion_path.front().y());
            assert(thickpaths.empty() || thickpaths.back().last_point().x() == extrusion_path.back().x());
            assert(thickpaths.empty() || thickpaths.back().last_point().y() == extrusion_path.back().y());
            for (ExtrusionPath& path : thickpaths) {
                path.set_can_reverse(!is_loop);
                path.attributes_mutable().height = idx_lh_size;
                path.overhang_attributes_mutable() = ExtrusionPropertyOverhang{1, 1, 0, false, true, true, false};
                paths.push_back(std::move(path));
            }
        }
        overhang_params.overhang_type_2_lh.push_back(idx_lh_size);
        idx_lh_size++;
    } else {
        overhang_params.overhang_type_2_lh.push_back(-1);
    }
    if (!small_flow.empty()) {
        for (const ClipperLib_Z::Path& extrusion_path : small_flow) {
            if(extrusion_path.size() <= 1)
                continue;
            ExtrusionPaths thickpaths = Geometry::unsafe_variable_width(Arachne::to_thick_polyline(extrusion_path),
                    role | ExtrusionRoleModifier::ERM_Bridge,
                    //is_external ? params.ext_perimeter_flow : params.perimeter_flow,
                    params.overhang_flow,
                    std::max(params.ext_perimeter_flow.scaled_width() / 4, scale_i(params.print_config.resolution)),
                    (is_external ? params.ext_perimeter_flow : params.perimeter_flow).scaled_width() / 10);
#ifdef _DEBUG
            for (int i = 1; i < thickpaths.size(); i++) {
                assert(thickpaths[i - 1].last_point() == thickpaths[i].first_point());
            }
#endif
            // thickpaths can be empty if extrusion_path is too short
            assert(thickpaths.empty() || thickpaths.front().first_point().x() == extrusion_path.front().x());
            assert(thickpaths.empty() || thickpaths.front().first_point().y() == extrusion_path.front().y());
            assert(thickpaths.empty() || thickpaths.back().last_point().x() == extrusion_path.back().x());
            assert(thickpaths.empty() || thickpaths.back().last_point().y() == extrusion_path.back().y());
            for (ExtrusionPath& path : thickpaths) {
                //// change flow to overhang one if too much.
                //if (path.mm3_per_mm() > params.overhang_flow.mm3_per_mm() ){
                //    path.attributes_mutable().mm3_per_mm = params.overhang_flow.mm3_per_mm();
                //    path.attributes_mutable().height = params.overhang_flow.height();
                //    path.attributes_mutable().width = params.overhang_flow.width();
                //}
                path.set_can_reverse(!is_loop);
                path.attributes_mutable().height = idx_lh_size;
                path.overhang_attributes_mutable() = ExtrusionPropertyOverhang{1, 2, 0, true, true, false, false};
                paths.push_back(std::move(path));
            }
        }
        overhang_params.overhang_type_2_lh.push_back(idx_lh_size);
        idx_lh_size++;
    } else {
        overhang_params.overhang_type_2_lh.push_back(-1);
    }
    if (!big_flow.empty()) {
        for (const ClipperLib_Z::Path& extrusion_path : big_flow) {
            if(extrusion_path.size() <= 1)
                continue;
            ExtrusionPaths thickpaths = Geometry::unsafe_variable_width(Arachne::to_thick_polyline(extrusion_path),
                    is_external ? ExtrusionRole::OverhangExternalPerimeter : ExtrusionRole::OverhangPerimeter,
                    //is_external ? params.ext_perimeter_flow : params.perimeter_flow,
                    params.overhang_flow,
                    std::max(params.ext_perimeter_flow.scaled_width() / 4, scale_i(params.print_config.resolution)),
                    (is_external ? params.ext_perimeter_flow : params.perimeter_flow).scaled_width() / 10);
            if (thickpaths.empty()) {
                // Note: can create problem with chain_and_reorder_extrusion_paths
                assert(extrusion_path.size() < 2 ||
                       Point(extrusion_path.front().x(), extrusion_path.front().y())
                           .coincides_with_epsilon(Point(extrusion_path.back().x(), extrusion_path.back().y())));
                continue;
            }
#ifdef _DEBUG
            for (int i = 1; i < thickpaths.size(); i++) {
                assert(thickpaths[i - 1].last_point() == thickpaths[i].first_point());
            }
#endif
            assert(thickpaths.front().first_point().x() == extrusion_path.front().x());
            assert(thickpaths.front().first_point().y() == extrusion_path.front().y());
            assert(thickpaths.back().last_point().x() == extrusion_path.back().x());
            assert(thickpaths.back().last_point().y() == extrusion_path.back().y());
            for (ExtrusionPath& path : thickpaths) {
                //// change flow to overhang one if too much.
                //if (path.mm3_per_mm() > params.overhang_flow.mm3_per_mm()) {
                //    path.attributes_mutable().mm3_per_mm = params.overhang_flow.mm3_per_mm();
                //    path.attributes_mutable().height = params.overhang_flow.height();
                //    path.attributes_mutable().width = params.overhang_flow.width();
                //}
                path.set_can_reverse(!is_loop);
                path.attributes_mutable().height = idx_lh_size;
                path.overhang_attributes_mutable() = ExtrusionPropertyOverhang{1, 2, 0, true, true, false, false};
                paths.push_back(std::move(path));
            }
        }
        overhang_params.overhang_type_2_lh.push_back(idx_lh_size);
        idx_lh_size++;
    } else {
        overhang_params.overhang_type_2_lh.push_back(-1);
    }
    assert(overhang_params.overhang_type_2_lh.size() == 6);
    assert(idx_lh_size >= 0 && idx_lh_size < 7);
    //FIXME from here, it's ~exactly the same as the other create_overhangs, please merge that into a function.

    overhang_params.is_loop = is_loop;
    overhang_params.is_external = is_external;
    overhang_params.has_dynamic = has_dynamic;
    overhang_params.first_point = Point(arachne_path.front().x(), arachne_path.front().y());
    overhang_params.last_point = Point(arachne_path.back().x(), arachne_path.back().y());

#ifdef _DEBUG
    {
        // check there seems to be a continous path from start to end
        for (size_t idx_path = 0; idx_path < paths.size(); ++idx_path) {
            const ExtrusionPath &path = paths[idx_path];
            bool found_another_path_after = false;
            bool found_another_path_before = false;
            bool found_almost_another_path_after = false;
            bool found_almost_another_path_before = false;
            int other_paths_count = 0;
            for (size_t idx_path2 = 0; idx_path2 < paths.size(); ++idx_path2) {
                if (idx_path == idx_path2)
                    continue;
                other_paths_count++;
                found_another_path_after = found_another_path_after || (path.polyline().back() == paths[idx_path2].polyline().front());
                found_another_path_before = found_another_path_before || (path.polyline().front() == paths[idx_path2].polyline().back());
                found_almost_another_path_after = found_almost_another_path_after || path.polyline().back().coincides_with_epsilon(paths[idx_path2].polyline().front());
                found_almost_another_path_before = found_almost_another_path_before || path.polyline().front().coincides_with_epsilon(paths[idx_path2].polyline().back());
            }
            bool found_another_path_after_strict = found_another_path_after;
            bool found_another_path_before_strict = found_another_path_before;
            bool found_almost_another_path_after_strict = found_almost_another_path_after;
            bool found_almost_another_path_before_strict = found_almost_another_path_before;
            //assert(other_paths_count == 0 || found_another_path_after || found_another_path_before);
            for (size_t idx_path2 = 0; idx_path2 < paths.size(); ++idx_path2) {
                if (idx_path == idx_path2)
                    continue;
                found_another_path_after = found_another_path_after || path.polyline().back() == paths[idx_path2].polyline().front() || path.polyline().back() == paths[idx_path2].polyline().back();
                found_another_path_before = found_another_path_before || path.polyline().front() == paths[idx_path2].polyline().back() || path.polyline().front() == paths[idx_path2].polyline().front();
                found_almost_another_path_after = found_almost_another_path_after || path.polyline().back().coincides_with_epsilon(paths[idx_path2].polyline().back());
                found_almost_another_path_before = found_almost_another_path_before || path.polyline().front().coincides_with_epsilon(paths[idx_path2].polyline().front());
            }
            assert(other_paths_count == 0 || found_another_path_after || found_another_path_before);
        }
    }
#endif

    //common function with arachne to sort & merge extrusions.
    _sort_overhangs(params, paths, role, overhang_params);
    
#ifdef _DEBUG
    {
        Point last_pt = paths.front().last_point();
        for (size_t idx_path = 1; idx_path < paths.size(); ++idx_path) {
            const ExtrusionPath &path = paths[idx_path];
            assert(path.polyline().size() >= 2);
            assert(path.first_point() == last_pt);
            for (size_t idx_pt = 1; idx_pt < path.size(); ++idx_pt)
                assert(!path.polyline().get_point(idx_pt - 1).coincides_with_epsilon(path.polyline().get_point(idx_pt)));
            last_pt = path.last_point();
        }
        if(is_loop)
            assert(paths.front().first_point() == last_pt);
    }
#endif
    if (is_loop && paths.size() > 1) {
         // no epsilon diff, please
        assert(paths.front().first_point().coincides_with_epsilon(paths.back().last_point()));
        Point mean = (paths.front().first_point() + paths.back().last_point()) / 2;
        paths.front().polyline().set_front(mean);
        paths.back().polyline().set_back(mean);
    }
    for (const ExtrusionPath &path : paths) {
        assert (!path.role().is_overhang() || path.overhang_attributes());
    }
    return paths;
}


// find out if paths touch - at least one point of one path is within limit distance of second path
bool paths_touch(const ExtrusionPath &path_one, const ExtrusionPath &path_two, coordf_t limit_distance)
{
    Polyline discrete_polyline_one = path_one.as_polyline().to_polyline();
    Polyline discrete_polyline_two = path_two.as_polyline().to_polyline();
    AABBTreeLines::LinesDistancer<Line> lines_two{discrete_polyline_two.lines()};
    for (size_t pt_idx = 0; pt_idx < path_one.polyline().size(); pt_idx++) {
        if (lines_two.distance_from_lines<false>(discrete_polyline_one.points[pt_idx]) < limit_distance) { return true; }
    }
    AABBTreeLines::LinesDistancer<Line> lines_one{discrete_polyline_one.lines()};
    for (size_t pt_idx = 0; pt_idx < path_two.polyline().size(); pt_idx++) {
        if (lines_one.distance_from_lines<false>(discrete_polyline_two.points[pt_idx]) < limit_distance) { return true; }
    }
    return false;
}

Polylines reconnect_polylines(const Polylines &polylines, coordf_t limit_distance, coord_t resolution)
{
    if (polylines.empty())
        return polylines;

    std::unordered_map<size_t, Polyline> connected;
    connected.reserve(polylines.size());
    for (size_t i = 0; i < polylines.size(); i++) {
        if (!polylines[i].empty()) {
            connected.emplace(i, polylines[i]);
        }
    }

    for (size_t a = 0; a < polylines.size(); a++) {
        if (connected.find(a) == connected.end()) {
            continue;
        }
        Polyline &base = connected.at(a);
        for (size_t b = a + 1; b < polylines.size(); b++) {
            if (connected.find(b) == connected.end()) {
                continue;
            }
            Polyline &next = connected.at(b);
            if ((base.last_point() - next.first_point()).cast<coordf_t>().squaredNorm() < limit_distance * limit_distance) {
                base.append(std::move(next));
                connected.erase(b);
            } else if ((base.last_point() - next.last_point()).cast<coordf_t>().squaredNorm() < limit_distance * limit_distance) {
                base.points.insert(base.points.end(), next.points.rbegin(), next.points.rend());
                connected.erase(b);
            } else if ((base.first_point() - next.last_point()).cast<coordf_t>().squaredNorm() < limit_distance * limit_distance) {
                next.append(std::move(base));
                base = std::move(next);
                base.reverse();
                connected.erase(b);
            } else if ((base.first_point() - next.first_point()).cast<coordf_t>().squaredNorm() < limit_distance * limit_distance) {
                base.reverse();
                base.append(std::move(next));
                base.reverse();
                connected.erase(b);
            }
        }
    }

    Polylines result;
    for (auto &ext : connected) {
        result.push_back(std::move(ext.second));
    }

    ensure_valid(result, resolution);
    return result;
}

ExtrusionPaths sort_extra_perimeters(const ExtrusionPaths& extra_perims, int index_of_first_unanchored, coordf_t extrusion_spacing)
{
    if (extra_perims.empty()) return {};

    std::vector<std::unordered_set<size_t>> dependencies(extra_perims.size());
    for (size_t path_idx = 0; path_idx < extra_perims.size(); path_idx++) {
        for (size_t prev_path_idx = 0; prev_path_idx < path_idx; prev_path_idx++) {
            if (paths_touch(extra_perims[path_idx], extra_perims[prev_path_idx], extrusion_spacing * 1.5f)) {
                       dependencies[path_idx].insert(prev_path_idx);        
            }
        }
    }

    std::vector<bool> processed(extra_perims.size(), false);
    for (int path_idx = 0; path_idx < index_of_first_unanchored; path_idx++) {
        processed[path_idx] = true;
    }

    for (size_t i = index_of_first_unanchored; i < extra_perims.size(); i++) {
        bool change = false;
        for (size_t path_idx = index_of_first_unanchored; path_idx < extra_perims.size(); path_idx++) {
            if (processed[path_idx])
                       continue;
            auto processed_dep = std::find_if(dependencies[path_idx].begin(), dependencies[path_idx].end(),
                                              [&](size_t dep) { return processed[dep]; });
            if (processed_dep != dependencies[path_idx].end()) {
                for (auto it = dependencies[path_idx].begin(); it != dependencies[path_idx].end();) {
                    if (!processed[*it]) {
                        dependencies[*it].insert(path_idx);
                        dependencies[path_idx].erase(it++);
                    } else {
                        ++it;
                    }
                }
                processed[path_idx] = true;
                change              = true;
            }
        }
        if (!change) {
            break;
        }
    }

    Point current_point = extra_perims.begin()->first_point();

    ExtrusionPaths sorted_paths{};
    size_t         null_idx = size_t(-1);
    size_t         next_idx = null_idx;
    bool           reverse  = false;
    while (true) {
        if (next_idx == null_idx) { // find next pidx to print
            double dist = std::numeric_limits<double>::max();
            for (size_t path_idx = 0; path_idx < extra_perims.size(); path_idx++) {
                if (!dependencies[path_idx].empty())
                    continue;
                const auto &path   = extra_perims[path_idx];
                double      dist_a = (path.first_point() - current_point).cast<double>().squaredNorm();
                if (dist_a < dist) {
                    dist     = dist_a;
                    next_idx = path_idx;
                    reverse  = false;
                }
                double dist_b = (path.last_point() - current_point).cast<double>().squaredNorm();
                if (dist_b < dist) {
                    dist     = dist_b;
                    next_idx = path_idx;
                    reverse  = true;
                }
            }
            if (next_idx == null_idx) {
                       break;
            }
        } else {
            // we have valid next_idx, add it to the sorted paths, update dependencies, update current point and potentialy set new next_idx
            ExtrusionPath path = extra_perims[next_idx];
            if (reverse) {
                path.reverse();
            }
            sorted_paths.push_back(path);
            assert(dependencies[next_idx].empty());
            dependencies[next_idx].insert(null_idx);
            current_point = sorted_paths.back().last_point();
            for (size_t path_idx = 0; path_idx < extra_perims.size(); path_idx++) {
                dependencies[path_idx].erase(next_idx);
            }
            double dist = std::numeric_limits<double>::max();
            next_idx    = null_idx;

            for (size_t path_idx = next_idx + 1; path_idx < extra_perims.size(); path_idx++) {
                if (!dependencies[path_idx].empty()) {
                    continue;
                }
                const ExtrusionPath &next_path = extra_perims[path_idx];
                double               dist_a    = (next_path.first_point() - current_point).cast<double>().squaredNorm();
                if (dist_a < dist) {
                    dist     = dist_a;
                    next_idx = path_idx;
                    reverse  = false;
                }
                double dist_b = (next_path.last_point() - current_point).cast<double>().squaredNorm();
                if (dist_b < dist) {
                    dist     = dist_b;
                    next_idx = path_idx;
                    reverse  = true;
                }
            }
            if (dist > scale_d(5.0)) {
                next_idx = null_idx;
            }
        }
    }

    ExtrusionPaths reconnected;
    reconnected.reserve(sorted_paths.size());
    for (ExtrusionPath &path : sorted_paths) {
        if (!reconnected.empty() && (reconnected.back().last_point() - path.first_point()).cast<double>().squaredNorm() <
                                        extrusion_spacing * extrusion_spacing * 4.0) {
            assert(reconnected.back().polyline().is_valid());
            assert(path.polyline().is_valid());
            if (reconnected.back().last_point() == path.first_point()) {
            } else if (reconnected.back().last_point().coincides_with_epsilon(path.first_point())) {
                path.polyline().set_front(reconnected.back().last_point());
                if (path.polyline().front().coincides_with_epsilon(path.polyline().get_point(1))) {
                    path.polyline().pop_front();
                    path.polyline().set_front(reconnected.back().last_point());
                }
                assert(path.polyline().is_valid());
            } else {
                // gap is lower than extrusion_spacing, so we can make the jump
                reconnected.back().polyline().append(path.polyline().front());
                assert(reconnected.back().polyline().is_valid());
            }
            if (path.length() > SCALED_EPSILON) {
                reconnected.back().polyline().append(path.polyline());
            }
        } else {
            reconnected.push_back(path);
        }
    }

    ExtrusionPaths filtered;
    filtered.reserve(reconnected.size());
    for (ExtrusionPath &path : reconnected) {
        if (path.length() > 3 * extrusion_spacing) {
            filtered.push_back(std::move(path));
        }
    }

    for (ExtrusionPath &path : filtered) {
        assert(!path.can_reverse());
    }

    //assert all overhang path have overhang attributes
    for (const ExtrusionPath &path : filtered) {
        assert (!path.role().is_overhang() || path.overhang_attributes());
    }

    return filtered;
}

#define EXTRA_PERIMETER_OFFSET_PARAMETERS ClipperLib::jtSquare, 0.
// #define EXTRA_PERIM_DEBUG_FILES
// Function will generate extra perimeters clipped over nonbridgeable areas of the provided surface and returns both the new perimeters and
// Polygons filled by those clipped perimeters
//TODO: not overhang flow over perimeters.
std::tuple<std::vector<ExtrusionPaths>, ExPolygons, ExPolygons> generate_extra_perimeters_over_overhangs(const ExPolygon &island,
                                                                                           const ExPolygons      &infill_area,
                                                                                           const Parameters        &params,
                                                                                           const int                perimeter_count,
                                                                                           coordf_t                 scaled_resolution
)
{
#ifdef EXTRA_PERIM_DEBUG_FILES
    static int iRunst=0;
    int iRun = iRunst++;
#endif
    coord_t perimeter_depth = 0;
    if ((perimeter_count > 0)) {
        //max_margin = this->flow(frExternalPerimeter).scaled_width() + this->flow(frPerimeter).scaled_spacing() * (this->region().config().perimeters.value - 1);
        perimeter_depth = params.ext_perimeter_flow.scaled_width() / 2  + params.ext_perimeter_flow.scaled_width() / 2 + params.perimeter_flow.scaled_spacing() * (perimeter_count -1);
    }
    const coord_t bridged_infill_margin = scale_i(params.config.bridged_infill_margin.get_effective_value(params.ext_perimeter_flow.width()));
    const coord_t anchors_size = std::min(bridged_infill_margin, perimeter_depth);
    const coord_t overhang_scaled_spacing = params.get_overhang_spacing() > 0 ? params.get_overhang_spacing() : params.overhang_flow.scaled_spacing();

    const BoundingBox infill_area_bb = get_extents(infill_area).inflated(SCALED_EPSILON + anchors_size);
    const Polygons optimized_lower_slices = ClipperUtils::clip_clipper_polygons_with_subject_bbox(params.lower_slices_bridge_for_extra_overhangs, infill_area_bb);
    const ExPolygons overhangs  = diff_ex(infill_area, optimized_lower_slices);

    if (overhangs.empty()) { return {}; }

    AABBTreeLines::LinesDistancer<Line> lower_layer_aabb_tree{to_lines(optimized_lower_slices)};
    // use island instead of infill_area, to be able to use already extruded (hopefully not-overhang) perimeters.
    const Polygons                      anchors             = intersection({island}, optimized_lower_slices);
    const ExPolygons                    anchors_no_overhangs= diff_ex(anchors, overhangs);
    const ExPolygons                    inset_anchors       = diff_ex(anchors,
                                                                   offset_ex(overhangs, anchors_size /*+ 0.1 * params.overhang_flow.scaled_width()*/, EXTRA_PERIMETER_OFFSET_PARAMETERS));
    const ExPolygons                    inset_overhang_area = diff_ex(infill_area, inset_anchors);

#ifdef EXTRA_PERIM_DEBUG_FILES
    {
        BoundingBox bbox = get_extents(inset_overhang_area);
        bbox.offset(scale_(1.));
        ::Slic3r::SVG svg(debug_out_path("%d_%d_%d_inset_overhang_area", params.layer->id(), iRun, iInst).c_str(), bbox);
        svg.draw(infill_area, "grey");
        svg.draw(union_ex(params.lower_slices_bridge_for_extra_overhangs), "green");
        svg.draw(union_ex(optimized_lower_slices), "teal");
        svg.draw(to_polylines(overhangs), "orange", scale_(0.3));
        svg.draw(to_polylines(inset_anchors), "purple", scale_(0.25));
        svg.draw(to_polylines(inset_overhang_area), "red", scale_(0.2));
        svg.Close();
    }
#endif

    ExPolygons inset_overhang_area_left_unfilled;

    std::vector<ExtrusionPaths> extra_perims; // overhang region -> extrusion paths
    for (const ExPolygon &overhang : union_ex(inset_overhang_area)) {
        const ExPolygons overhang_to_cover = {overhang};
        const ExPolygons expanded_overhang_to_cover = offset_ex(overhang_to_cover, 1.1 * overhang_scaled_spacing);
        ExPolygons shrinked_overhang_to_cover = offset_ex(overhang_to_cover, -0.1 * overhang_scaled_spacing);

        const Polygons real_overhang = intersection(overhang_to_cover, overhangs);
        if (real_overhang.empty()) {
            inset_overhang_area_left_unfilled.insert(inset_overhang_area_left_unfilled.end(), overhang_to_cover.begin(),
                                                     overhang_to_cover.end());
            continue;
        }
        ExtrusionPaths &overhang_region = extra_perims.emplace_back();

        const ExPolygons anchoring         = intersection_ex(expanded_overhang_to_cover, inset_anchors);
        ExPolygons perimeter_polygon = offset2_ex(union_ex(offset_ex(overhang_to_cover, 0.1 * overhang_scaled_spacing), anchoring),
                                            -overhang_scaled_spacing * (0.1 + 0.5 + 0.1), overhang_scaled_spacing * 0.1);

        const Polygon anchoring_convex_hull = Geometry::convex_hull(intersection_ex(expanded_overhang_to_cover, anchors_no_overhangs));
        double  unbridgeable_area     = area(diff(real_overhang, {anchoring_convex_hull}));

        //try with the quick bridge detector
        auto [dir, unsupp_dist] = detect_bridging_direction(real_overhang, anchors);

#ifdef EXTRA_PERIM_DEBUG_FILES
        {
            BoundingBox bbox = get_extents(anchoring_convex_hull);
            bbox.offset(scale_(1.));
            ::Slic3r::SVG svg(debug_out_path("%d_%d_%d_bridge_check", params.layer->id(), iRun, iInst).c_str(), bbox);
            svg.draw(to_polylines(perimeter_polygon), "purple", scale_(0.25));
            svg.draw(to_polylines(real_overhang), "red", scale_(0.20));
            svg.draw((anchoring_convex_hull.split_at_index(0)), "green", scale_(0.15));
            svg.draw(to_polylines(anchoring), "yellow", scale_(0.10));
            svg.draw(to_polylines(diff_ex(perimeter_polygon, {anchoring_convex_hull})), "black", scale_(0.10));
            svg.draw((diff_pl(to_polylines(diff(real_overhang, anchors)), expand(anchors, float(SCALED_EPSILON)))), "blue", scale_(0.30));
            svg.Close();
        }
#endif
#ifdef _DEBUG
        // this seems unneeded, and seems to create memory crashes (on linux).
        if (unbridgeable_area > 0.2 * area(real_overhang) || unsupp_dist > total_length(real_overhang) * 0.2) {
            // try with the real bridge detector
            BridgeDetector bd(
                union_ex(real_overhang),
                union_ex(anchors),
                params.overhang_flow.scaled_spacing(),
                scale_i(params.print_config.bridge_precision.get_effective_value(params.overhang_flow.spacing())),
                params.layer->id()
            );
            // set angle
            double bridge_angle = 0;
            if (params.config.bridge_angle.is_enabled()) {
                bridge_angle = Geometry::deg2rad(params.config.bridge_angle.value);
            } else if (bd.detect_angle()) {
                bridge_angle = bd.angle;
            }
            // detection
            Polylines unsupported_lines = bd.unsupported_edges(bridge_angle);
            unsupp_dist = 0;
            for (Polyline &polyline : unsupported_lines) {
                unsupp_dist += polyline.length();
            }
            // coverage
            unbridgeable_area = area(diff(real_overhang, bd.coverage(bridge_angle)));
#ifdef EXTRA_PERIM_DEBUG_FILES
        {
            BoundingBox bbox = get_extents(anchoring_convex_hull);
            bbox.offset(scale_(1.));
            ::Slic3r::SVG svg(debug_out_path("%d_%d_%d_bridge_check_v2", params.layer->id(), iRun, iInst).c_str(), bbox);
            svg.draw(to_polylines(perimeter_polygon), "purple", scale_(0.09));
            svg.draw(to_polylines(anchoring), "yellow", scale_(0.08));
            svg.draw(to_polylines(real_overhang), "red", scale_(0.07));
            svg.draw(unsupported_lines, "cyan", scale_(0.06));
            svg.draw(to_polylines(bd.coverage(bridge_angle)), "blue", scale_(0.05));
            svg.Close();
        }
#endif
        }
#endif

        if (unbridgeable_area < 0.2 * area(real_overhang) && unsupp_dist < total_length(real_overhang) * 0.2) {
            inset_overhang_area_left_unfilled.insert(inset_overhang_area_left_unfilled.end(),overhang_to_cover.begin(),overhang_to_cover.end());
            perimeter_polygon.clear();
        } else {
            // ensure you don't go into the infill.
            shrinked_overhang_to_cover = diff_ex(shrinked_overhang_to_cover, offset_ex(inset_anchors, overhang_scaled_spacing * 0.5));
            //  fill the overhang with perimeters
            int continuation_loops = 2;
            while (continuation_loops >= 0) {
                auto prev = perimeter_polygon;
                // prepare next perimeter lines
                Polylines perimeter = intersection_pl(to_polylines(perimeter_polygon), shrinked_overhang_to_cover);

                // do not add the perimeter to result yet, first check if perimeter_polygon is not empty after shrinking - this would mean
                //  that the polygon was possibly too small for full perimeter loop and in that case try gap fill first
                perimeter_polygon = union_ex(perimeter_polygon, anchoring);
                perimeter_polygon = intersection_ex(offset_ex(perimeter_polygon, -overhang_scaled_spacing), expanded_overhang_to_cover);

                //TODO: cut the extrusions to have normal flow in the supported area.
                if (perimeter_polygon.empty()) { // fill possible gaps of single extrusion width
                    ExPolygons shrinked = intersection_ex(offset_ex(prev, -0.3 * overhang_scaled_spacing), expanded_overhang_to_cover);
                    if (!shrinked.empty())
                        extrusion_paths_append(overhang_region,
                                               reconnect_polylines(perimeter, overhang_scaled_spacing,
                                                                   scaled_resolution),
                                               ExtrusionAttributes{ExtrusionRole::OverhangPerimeter,
                                                                   params.overhang_flow},
                                               ExtrusionPropertyOverhang(1, 2, 0, true, true, false, false),
                                               false);

                    Polylines  fills;
                    ExPolygons gap = shrinked.empty() ? offset_ex(prev, overhang_scaled_spacing * 0.5) : shrinked;

                    for (const ExPolygon &ep : gap) {
                        ep.medial_axis(0.75 * params.overhang_flow.scaled_width(), 3.0 * overhang_scaled_spacing, fills);
                    }
                    if (!fills.empty()) {
                        fills = intersection_pl(fills, shrinked_overhang_to_cover);
                        extrusion_paths_append(overhang_region,
                                               reconnect_polylines(fills, overhang_scaled_spacing, scaled_resolution),
                                               ExtrusionAttributes{ExtrusionRole::OverhangPerimeter,
                                                                   params.overhang_flow},
                                               ExtrusionPropertyOverhang(1, 2, 0, true, true, false, false),
                                               false);
                    }
                    break;
                } else {
                    extrusion_paths_append(overhang_region,
                                           reconnect_polylines(perimeter, overhang_scaled_spacing, scaled_resolution),
                                           ExtrusionAttributes{ExtrusionRole::OverhangPerimeter, params.overhang_flow},
                                           ExtrusionPropertyOverhang(1, 2, 0, true, true, false, false),
                                           false);
                }

                if (intersection(perimeter_polygon, real_overhang).empty()) { continuation_loops--; }

                if (prev == perimeter_polygon) {
#ifdef EXTRA_PERIM_DEBUG_FILES
                    BoundingBox bbox = get_extents(perimeter_polygon);
                    bbox.offset(scale_(5.));
                    ::Slic3r::SVG svg(debug_out_path("%d_%d_%d_perimeter_polygon", params.layer->id(), iRun, iInst).c_str(), bbox);
                    svg.draw(to_polylines(perimeter_polygon), "blue", scale_(0.25));
                    svg.draw(to_polylines(overhang_to_cover), "red", scale_(0.20));
                    svg.draw(to_polylines(union_ex(real_overhang)), "green", scale_(0.15));
                    svg.draw(to_polylines(anchoring), "yellow", scale_(0.10));
                    svg.Close();
#endif
                    break;
                }
            }

            perimeter_polygon = offset_ex(perimeter_polygon, 0.5 * overhang_scaled_spacing);
            perimeter_polygon = union_ex(perimeter_polygon, anchoring);
            inset_overhang_area_left_unfilled.insert(inset_overhang_area_left_unfilled.end(), perimeter_polygon.begin(),perimeter_polygon.end());

#ifdef EXTRA_PERIM_DEBUG_FILES
            BoundingBox bbox = get_extents(inset_overhang_area);
            bbox.offset(scale_(2.));
            ::Slic3r::SVG svg(debug_out_path("%d_%d_%d_pre_final", params.layer->id(), iRun, iInst).c_str(), bbox);
            svg.draw(to_polylines(perimeter_polygon), "blue", scale_(0.05));
            svg.draw(to_polylines(anchoring), "green", scale_(0.05));
            svg.draw(to_polylines(overhang_to_cover), "yellow", scale_(0.05));
            svg.draw(to_polylines(inset_overhang_area_left_unfilled), "red", scale_(0.05));
            svg.Close();
#endif
            overhang_region.erase(std::remove_if(overhang_region.begin(), overhang_region.end(),
                                                 [](const ExtrusionPath &p) { return p.empty(); }),
                                  overhang_region.end());

            if (!overhang_region.empty()) {
                Polyline discrete_polyline = overhang_region.front().polyline().to_polyline();
                discrete_polyline.assert_valid();
                // there is a special case, where the first (or last) generated overhang perimeter eats all anchor space.
                // When this happens, the first overhang perimeter is also a closed loop, and needs special check
                // instead of the following simple is_anchored lambda, which checks only the first and last point (not very useful on closed
                // polyline)
                bool first_overhang_is_closed_and_anchored =
                    (overhang_region.front().first_point() == overhang_region.front().last_point() &&
                     !intersection_pl(discrete_polyline, optimized_lower_slices).empty());
                     
                auto is_anchored = [&lower_layer_aabb_tree](const ExtrusionPath &path) {
                    return lower_layer_aabb_tree.distance_from_lines<true>(path.first_point()) <= 0 ||
                           lower_layer_aabb_tree.distance_from_lines<true>(path.last_point()) <= 0;
                };
                if (!first_overhang_is_closed_and_anchored) {
                    std::reverse(overhang_region.begin(), overhang_region.end());
                } else {
                    size_t min_dist_idx = 0;
                    double min_dist = std::numeric_limits<double>::max();
                    for (size_t i = 0; i < discrete_polyline.size(); i++) {
                        Point p = discrete_polyline[i];
                        if (double d = lower_layer_aabb_tree.distance_from_lines<true>(p) < min_dist) {
                            min_dist = d;
                            min_dist_idx = i;
                        }
                    }
                    //std::rotate(overhang_region.front().polyline().begin(), overhang_region.front().polyline().begin() + min_dist_idx,
                    //            overhang_region.front().polyline().end());
                    {
                        discrete_polyline.assert_valid(); 
                        assert(discrete_polyline.front() == discrete_polyline.back());
                        //remove last that is the same as the first, as it's a loop
                        discrete_polyline.points.pop_back();
                        std::rotate(discrete_polyline.begin(), discrete_polyline.begin() + min_dist_idx, discrete_polyline.end());
                        // recreate the loop by adding the first at the end
                        discrete_polyline.points.push_back(discrete_polyline.points.front());
                        discrete_polyline.assert_valid();
                        overhang_region.front().polyline() = ArcPolyline(discrete_polyline);
                    }
                }
                auto first_unanchored          = std::stable_partition(overhang_region.begin(), overhang_region.end(), is_anchored);
                int  index_of_first_unanchored = first_unanchored - overhang_region.begin();
                overhang_region = sort_extra_perimeters(overhang_region, index_of_first_unanchored, overhang_scaled_spacing);
            }
        }
    }

#ifdef EXTRA_PERIM_DEBUG_FILES
    BoundingBox bbox = get_extents(inset_overhang_area);
    bbox.offset(scale_(2.));
    ::Slic3r::SVG svg(debug_out_path("%d_%d_%d_final", params.layer->id(), iRun, iInst).c_str(), bbox);
    svg.draw(to_polylines(inset_overhang_area_left_unfilled), "blue", scale_(0.05));
    svg.draw(to_polylines(inset_overhang_area), "green", scale_(0.05));
    svg.draw(to_polylines(diff(inset_overhang_area, inset_overhang_area_left_unfilled)), "yellow", scale_(0.05));
    svg.Close();
#endif
    //{
    //    static int isaqsdsdfsdfqzfn = 0;
    //    std::stringstream stri;
    //    stri << params.layer->id() << "_3_generate_extra_perimeters_over_overhangs_" << isaqsdsdfsdfqzfn++ << ".svg";
    //    SVG svg(stri.str());
    //    svg.draw(params.lower_slices_bridge_for_extra_overhangs, "grey");
    //    for (ExPolygon &poly : ensure_valid(diff_ex(inset_overhang_area, inset_overhang_area_left_unfilled),
    //                                        coord_t(scaled_resolution))) {
    //        svg.draw(to_polylines(poly), "blue", scale_i(0.2));
    //    }
    //    svg.draw(to_polylines(infill_area), "cyan", scale_i(0.1));
    //    svg.draw(to_polylines(overhangs), "green", scale_i(0.12));
    //    svg.draw(to_polylines(anchors), "orange", scale_i(0.08));
    //    svg.draw(to_polylines(inset_anchors), "yellow", scale_i(0.06));
    //    svg.draw(to_polylines(inset_overhang_area), "brown", scale_i(0.04));
    //    for (ExPolygon &poly : ensure_valid(diff_ex(inset_overhang_area, inset_overhang_area_left_unfilled),
    //                                        coord_t(scaled_resolution))) {
    //        svg.draw(to_polylines(poly), "blue", scale_i(0.02));
    //    }
    //    svg.Close();
    //}

    inset_overhang_area_left_unfilled = union_ex(inset_overhang_area_left_unfilled);
    
    return {extra_perims,
            ensure_valid(
                diff_ex(inset_overhang_area, inset_overhang_area_left_unfilled) /*, coord_t(scaled_resolution)*/),
            ensure_valid(
                union_ex(inset_anchors, inset_overhang_area_left_unfilled) /*, coord_t(scaled_resolution)*/)};
}

#ifdef ARACHNE_DEBUG
static void export_perimeters_to_svg(const std::string &path, const Polygons &contours, const std::vector<Arachne::VariableWidthLines> &perimeters, const ExPolygons &infill_area)
{
    coordf_t    stroke_width = scale_(0.03);
    BoundingBox bbox         = get_extents(contours);
    bbox.offset(scale_(1.));
    ::Slic3r::SVG svg(path.c_str(), bbox);

    svg.draw(infill_area, "cyan");

    for (const Arachne::VariableWidthLines &perimeter : perimeters)
        for (const Arachne::ExtrusionLine &extrusion_line : perimeter) {
            ThickPolyline thick_polyline = to_thick_polyline(extrusion_line);
            svg.draw({thick_polyline}, "green", "blue", stroke_width);
        }

    for (const Line &line : to_lines(contours))
        svg.draw(line, "red", stroke_width);
}
#endif

// Thanks, Cura developers, for implementing an algorithm for generating perimeters with variable width (Arachne) that is based on the paper
// "A framework for adaptive width control of dense contour-parallel toolpaths in fused deposition modeling"
ProcessSurfaceResult PerimeterGenerator::process_arachne(const Parameters &params, int& loop_number, const ExPolygon& surface_expolygon, ExtrusionEntityCollection &loops) {

    ProcessSurfaceResult result;

    coord_t scaled_resolution = get_resolution(0, false);
    coord_t ext_displacement = (params.get_ext_perimeter_width() / 2. - params.get_ext_perimeter_spacing() / 2.);
    ExPolygons last = (ext_displacement != 0)
        ? offset_ex(surface_expolygon.simplify_p(scaled_resolution),  -ext_displacement)
        : union_ex(surface_expolygon.simplify_p(scaled_resolution));
    // bb for checking out-of-bounds points.
    BoundingBox srf_bb;
    for (ExPolygon &expo : last) srf_bb.merge(expo.contour.points);

    //increase surface for milling_post-process
    if (this->mill_extra_size > SCALED_EPSILON) {
        if (unmillable.empty())
            last = offset_ex(last, mill_extra_size);
        else {
            ExPolygons growth = diff_ex(offset_ex(last, mill_extra_size), unmillable, ApplySafetyOffset::Yes);
            last.insert(last.end(), growth.begin(), growth.end());
            last = union_ex(last);
        }
    }

    // only_one_perimeter_top, from orca
    std::vector<Arachne::VariableWidthLines> out_shell;
    bool has_only_one_perimeter_top = false;
    if (loop_number > 0 &&
        (params.region_setting.has_many_config(&params.config.only_one_perimeter_top) ||
         params.region_setting.get_solo_config(&params.config.only_one_perimeter_top).get_bool()) &&
        !params.use_bridge && this->upper_slices != nullptr) {
        this->throw_if_canceled();
        has_only_one_perimeter_top = true;
        // Check if current layer has surfaces that are not covered by upper layer (i.e., top surfaces)
        ExPolygons non_top_polygons;
        for (auto const &[opt_values, areas] : params.region_setting.get_areas(&params.config.only_one_perimeter_top)) {
            if (opt_values.get_bool(&params.config.only_one_perimeter_top)) {
                const ExPolygons *upper_slices = this->upper_slices;
                // has multiple or only one?
                ExPolygons cliped_upper_slices;
                if (!areas.is_accept_all()) {
                    // clip upper_slices
                    cliped_upper_slices = diff_ex({surface_expolygon}, areas.expolys);
                    if (!this->upper_slices->empty()) {
                        cliped_upper_slices = union_ex(cliped_upper_slices, *this->upper_slices);
                    }
                    upper_slices =  &cliped_upper_slices;
                }
                ExPolygons * last_onion = non_top_polygons.empty() ? &last : &non_top_polygons;
                split_top_surfaces(this->lower_slices, upper_slices, offset_ex(*last_onion, -params.get_ext_perimeter_spacing() / 2),
                    result.top_fills, non_top_polygons, result.fill_clip,
                    loop_number - 1,
                    scale_d(opt_values.get_effective_value(unscaled(params.perimeter_flow.scaled_width()), &params.config.min_width_top_surface)),
                    opt_values.get_bool(&params.config.only_one_perimeter_top_other_algo));
            }
        }
        // has to set the outer polygon to the centerline of the external perimeter
        if (result.top_fills.empty()) {
            // No top surfaces, no special handling needed
        } else {
            // First we slice the outer shell
            const Polygons         last_p = to_polygons(last);
            Arachne::WallToolPaths wallToolPaths(last_p, params.get_ext_perimeter_spacing(),
                                                    params.get_ext_perimeter_width(), params.get_perimeter_spacing(),
                                                    params.get_perimeter_width(), 1, coord_t(0),
                                                 params.layer->unscaled_height(), params.config, params.print_config);
            out_shell = wallToolPaths.getToolPaths();
            // Make sure infill not overlap with wall
            // offset the InnerContour as arachne use bounds and not centerline
            result.top_fills = intersection_ex(result.top_fills,
                                                offset_ex(wallToolPaths.getInnerContour(),
                                                            params.get_ext_perimeter_spacing() / 2));

            if (!result.top_fills.empty()) {
                // Then get the inner part that needs more walls
                // reduce the not-top fill to the bound for arachne (as arachne doesn't use the centerline but the
                // boundary) note: you can also diff_ex(offset_ex(result.top_fills, this->perimeter_spacing / 2),
                // wallToolPaths.getInnerContour());  this should have similar results
                last = intersection_ex(offset_ex(non_top_polygons, -params.get_perimeter_spacing() / 2),
                                        wallToolPaths.getInnerContour());
                loop_number--;
            } else {
                // Give up the outer shell because we don't have any meaningful top surface
                out_shell.clear();
            }
        }
    }

    Polygons   last_p = to_polygons(last);
    Arachne::WallToolPaths wallToolPaths(last_p, params.get_ext_perimeter_spacing(), params.get_ext_perimeter_width(),
        params.get_perimeter_spacing(), params.get_perimeter_width(), loop_number, coord_t(0),
        params.layer->unscaled_height(), params.config, params.print_config);
    std::vector<Arachne::VariableWidthLines> perimeters = wallToolPaths.getToolPaths();
    ExPolygons infill_contour = union_ex(wallToolPaths.getInnerContour());

    // append the first shell from only_one_perimeter_top into perimeters (from orca)
    if (!out_shell.empty()) {
        // Combine outer shells
        size_t inset_offset = 0;
        // update inset indexes
        for (auto &p : out_shell) {
            for (auto &l : p) {
                if (l.inset_idx + 1 > inset_offset) {
                    inset_offset = l.inset_idx + 1;
                }
            }
        }
        for (auto &p : perimeters) {
            for (auto &l : p) { l.inset_idx += inset_offset; }
        }
        // append
        perimeters.insert(perimeters.begin(), out_shell.begin(), out_shell.end());
    }

    //TODO better extra_perimeters from surfaces
    if (loop_number > 0 && params.region_setting.has_many_config(&Parameters::OPT_EXTRA_PERIMETER)) {
        std::vector<std::pair<int, ExPolygons>> ordered_settings;
        for (auto const &[extra_perimeters, areas] : params.region_setting.get_areas(&Parameters::OPT_EXTRA_PERIMETER)) {
            if (extra_perimeters.get_int() > 0) {
                //Bunch them: if you have a 0, 2 and 3, bunhc the 2 and 3 for the first 
                ordered_settings.emplace_back(extra_perimeters.get_int(), intersection_ex(infill_contour, areas.expolys));
                infill_contour = diff_ex(infill_contour, areas.expolys);
            }
        }
        std::sort(ordered_settings.begin(), ordered_settings.end(), [](auto &a, auto &b) {return a.first < b.first;});
        int current_perimeter_count = 0;
        for (size_t idx_peri = 0; idx_peri < ordered_settings.size(); ++idx_peri) {
            // merge areas
            ExPolygons areas_to_perimeterize = ordered_settings[idx_peri].second;
            for (size_t i_merge = idx_peri + 1; i_merge < ordered_settings.size(); ++i_merge) {
                append(areas_to_perimeterize, ordered_settings[i_merge].second);
            }
            if (idx_peri + 1 < ordered_settings.size()) {
                areas_to_perimeterize = union_ex(areas_to_perimeterize);
            }
            //create perimeters
            {
                Polygons   last_p = to_polygons(areas_to_perimeterize);
                Arachne::WallToolPaths wallToolPaths_extra(last_p, params.get_ext_perimeter_spacing(),
                                                           params.get_ext_perimeter_width(),
                                                           params.get_perimeter_spacing(),
                                                           params.get_perimeter_width(),
                                                           ordered_settings[idx_peri].first - current_perimeter_count,
                                                           coord_t(0), params.layer->unscaled_height(), params.config,
                                                           params.print_config);
                std::vector<Arachne::VariableWidthLines> perimeters_extra = wallToolPaths_extra.getToolPaths();
                ExPolygons new_infil_areas = union_ex(wallToolPaths_extra.getInnerContour());
                for (size_t i_merge = idx_peri + 1; i_merge < ordered_settings.size(); ++i_merge) {
                    ordered_settings[i_merge].second = intersection_ex(ordered_settings[i_merge].second, new_infil_areas);
                }
                for (size_t i_merge = idx_peri + 1; i_merge < ordered_settings.size(); ++i_merge) {
                    new_infil_areas = diff_ex(new_infil_areas, ordered_settings[i_merge].second);
                }
                append(infill_contour, new_infil_areas);
                // update inset indexes
                size_t inset_offset = 0;
                for (auto &p : perimeters) {
                    for (auto &l : p) {
                        if (l.inset_idx + 1 > inset_offset) {
                            inset_offset = l.inset_idx + 1;
                        }
                    }
                }
                for (auto &p : perimeters_extra) {
                    for (auto &l : p) {
                        l.inset_idx += inset_offset;
                    }
                }
                // append
                append(perimeters, perimeters_extra);
            }
            current_perimeter_count = ordered_settings[idx_peri].first;
        }
    }
     
    // extra_perimeters_count (add extra perimeters on regions)
    if (params.region_setting.has_many_config(&params.config.extra_perimeters_count)) {
        std::vector<std::pair<int, ExPolygons>> ordered_settings;
        for (auto const &[extra_perimeters_count, areas] : params.region_setting.get_areas(&params.config.extra_perimeters_count)) {
            if (extra_perimeters_count.get_int() > 0) {
                //Bunch them: if you have a 0, 2 and 3, bunhc the 2 and 3 for the first 
                ordered_settings.emplace_back(extra_perimeters_count.get_int(), intersection_ex(infill_contour, areas.expolys));
                infill_contour = diff_ex(infill_contour, areas.expolys);
            }
        }
        std::sort(ordered_settings.begin(), ordered_settings.end(), [](auto &a, auto &b) {return a.first < b.first;});
        int current_perimeter_count = 0;
        for (size_t idx_peri = 0; idx_peri < ordered_settings.size(); ++idx_peri) {
            // merge areas
            ExPolygons areas_to_perimeterize = ordered_settings[idx_peri].second;
            for (size_t i_merge = idx_peri + 1; i_merge < ordered_settings.size(); ++i_merge) {
                append(areas_to_perimeterize, ordered_settings[i_merge].second);
            }
            if (idx_peri + 1 < ordered_settings.size()) {
                areas_to_perimeterize = union_ex(areas_to_perimeterize);
            }
            //create perimeters
            {
                Polygons   last_p = to_polygons(areas_to_perimeterize);
                Arachne::WallToolPaths wallToolPaths_extra(last_p, params.get_ext_perimeter_spacing(),
                                                           params.get_ext_perimeter_width(),
                                                           params.get_perimeter_spacing(),
                                                           params.get_perimeter_width(),
                                                           ordered_settings[idx_peri].first - current_perimeter_count,
                                                           coord_t(0), params.layer->unscaled_height(), params.config,
                                                           params.print_config);
                std::vector<Arachne::VariableWidthLines> perimeters_extra = wallToolPaths_extra.getToolPaths();
                ExPolygons new_infil_areas = union_ex(wallToolPaths_extra.getInnerContour());
                for (size_t i_merge = idx_peri + 1; i_merge < ordered_settings.size(); ++i_merge) {
                    ordered_settings[i_merge].second = intersection_ex(ordered_settings[i_merge].second, new_infil_areas);
                }
                for (size_t i_merge = idx_peri + 1; i_merge < ordered_settings.size(); ++i_merge) {
                    new_infil_areas = diff_ex(new_infil_areas, ordered_settings[i_merge].second);
                }
                append(infill_contour, new_infil_areas);
                // update inset indexes
                size_t inset_offset = 0;
                for (auto &p : perimeters) {
                    for (auto &l : p) {
                        if (l.inset_idx + 1 > inset_offset) {
                            inset_offset = l.inset_idx + 1;
                        }
                    }
                }
                for (auto &p : perimeters_extra) {
                    for (auto &l : p) {
                        l.inset_idx += inset_offset;
                    }
                }
                // append
                append(perimeters, perimeters_extra);
            }
            current_perimeter_count = ordered_settings[idx_peri].first;
        }
    }

    // extra_perimeters_below_area
    if (params.region_setting.has_many_config(&params.config.extra_perimeters_below_area) ||
        params.region_setting.get_solo_config(&params.config.extra_perimeters_below_area).get_float() > 0) {
        ExPolygons small_polygons;
        for (auto const &[extra_perimeters_below_area, areas] :
             params.region_setting.get_areas(&params.config.extra_perimeters_below_area)) {
            if (extra_perimeters_below_area.get_float() > 0) {
                double area_mm2 = extra_perimeters_below_area.is_percent() ?
                    sqr(extra_perimeters_below_area.get_effective_value((coordf_t) params.get_perimeter_width())) :
                    scale_d(scale_d(extra_perimeters_below_area.get_float()));
                // gather small areas
                ExPolygons last_onion = union_ex(wallToolPaths.getInnerContour());
                for (ExPolygon &next : areas.intersections(last_onion)) {
                    if (next.area() < area_mm2) {
                        small_polygons.push_back(std::move(next));
                    }
                }
            }
        }
        // create them
        if (!small_polygons.empty()) {
            infill_contour = diff_ex(infill_contour, small_polygons);
            Polygons small_polygons_p = to_polygons(small_polygons);
            Arachne::WallToolPaths wallToolPaths_extra(small_polygons_p, params.get_ext_perimeter_spacing(),
                                                 params.get_ext_perimeter_width(), params.get_perimeter_spacing(),
                                                 params.get_perimeter_width(), 9999, coord_t(0), params.layer->unscaled_height(),
                                                 params.config, params.print_config);
            std::vector<Arachne::VariableWidthLines> perimeters_extra = wallToolPaths_extra.getToolPaths();
            append(infill_contour, union_ex(wallToolPaths_extra.getInnerContour()));
            // update inset indexes
            size_t inset_offset = 0;
            for (auto &p : perimeters) {
                for (auto &l : p) {
                    if (l.inset_idx + 1 > inset_offset) {
                        inset_offset = l.inset_idx + 1;
                    }
                }
            }
            for (auto &p : perimeters_extra) {
                for (auto &l : p) {
                    l.inset_idx += inset_offset;
                }
            }
            // append
            append(perimeters, perimeters_extra);
        }
    }

    // extra_perimeters_odd_layers
    if (params.layer->id() % 2 == 1 &&
        params.region_setting.has_many_config(&params.config.extra_perimeters_odd_layers)) {
        ExPolygons extra_perimeter_areas;
        for (auto const &[is_extra_perimeters_odd_layers, areas] :
             params.region_setting.get_areas(&params.config.extra_perimeters_odd_layers)) {
            if (is_extra_perimeters_odd_layers.get_bool()) {
                ExPolygons last_onion = union_ex(wallToolPaths.getInnerContour());
                append(extra_perimeter_areas, areas.intersections(last_onion));
            }
        }
        // create it
        if (!extra_perimeter_areas.empty()) {
            infill_contour = diff_ex(infill_contour, extra_perimeter_areas);
            Polygons extra_perimeter_areas_p = to_polygons(extra_perimeter_areas);
            Arachne::WallToolPaths wallToolPaths_extra(extra_perimeter_areas_p, params.get_ext_perimeter_spacing(),
                                                 params.get_ext_perimeter_width(), params.get_perimeter_spacing(),
                                                 params.get_perimeter_width(), 1, coord_t(0), params.layer->unscaled_height(),
                                                 params.config, params.print_config);
            std::vector<Arachne::VariableWidthLines> perimeters_extra = wallToolPaths_extra.getToolPaths();
            append(infill_contour, union_ex(wallToolPaths_extra.getInnerContour()));
            // update inset indexes
            size_t inset_offset = 0;
            for (auto &p : perimeters) {
                for (auto &l : p) {
                    if (l.inset_idx + 1 > inset_offset) {
                        inset_offset = l.inset_idx + 1;
                    }
                }
            }
            for (auto &p : perimeters_extra) {
                for (auto &l : p) {
                    l.inset_idx += inset_offset;
                }
            }
            // append
            append(perimeters, perimeters_extra);
        }
    }

#if _DEBUG
    for (auto perimeter : perimeters) {
        for (Arachne::ExtrusionLine &extrusion : perimeter) {
            if (extrusion.is_zero_length())
                continue;
            assert(!(extrusion.is_closed ^ (extrusion.junctions.front().p == extrusion.junctions.back().p)));
            for (Slic3r::Arachne::ExtrusionJunction &junction : extrusion.junctions) {
                Point pt = junction.p;
                assert(unscaled(pt.x()) < 10000 && unscaled(pt.x()) > -10000);
                assert(unscaled(pt.y()) < 10000 && unscaled(pt.y()) > -10000);
            }
        }
    }
#endif

    // hack to fix points that go to the moon. https://github.com/supermerill/SuperSlicer/issues/4032
    // get max dist possible
    const distsqrf_t max_dist_sqr = srf_bb.min.distance_to_square(srf_bb.max);
    //detect astray points and delete them
    for (Arachne::VariableWidthLines &perimeter : perimeters) {
        this->throw_if_canceled();
        for (auto it_extrusion = perimeter.begin(); it_extrusion != perimeter.end();) {
            assert(!it_extrusion->junctions.empty());
            Point last_point = it_extrusion->junctions.front().p;
            for (auto it_junction = it_extrusion->junctions.begin()+1; it_junction != it_extrusion->junctions.end();) {
                distsqrf_t dist_sqr = it_junction->p.distance_to_square(last_point);
                if (dist_sqr > max_dist_sqr) {
                    it_junction = it_extrusion->junctions.erase(it_junction);
                } else {
                    last_point = it_junction->p;
                    ++it_junction;
                }
            }
            if (it_extrusion->junctions.size() < 2) {
                it_extrusion = perimeter.erase(it_extrusion);
            } else {
                ++it_extrusion;
            }
        }
    }

    loop_number = int(perimeters.size());

#ifdef ARACHNE_DEBUG
    {
        static int iRun = 0;
        export_perimeters_to_svg(debug_out_path("arachne-perimeters-%d-%d.svg", layer_id, iRun++), to_polygons(last), perimeters, union_ex(wallToolPaths.getInnerContour()));
    }
#endif

#if _DEBUG
    // All closed ExtrusionLine should have the same the first and the last point.
    for (Arachne::VariableWidthLines &perimeter : perimeters) {
        for (Arachne::ExtrusionLine &el : perimeter) {
            if (el.is_closed && el.junctions.front().p != el.junctions.back().p) {
                assert(false);
                el.is_closed = false;
            }
        }
    }
#endif

    //build perimeter_boundary
    bool has_tw = false;
    for (const Arachne::VariableWidthLines& perimeter : perimeters) {
        for (const Arachne::ExtrusionLine &el : perimeter) {
            // extrenal
            if (el.inset_idx == 0) {
                //add it
                if (el.is_closed) {
                    perimeter_boundary.emplace_back(el.toPolygon());
                } else {
                    // need to create a polygon
                    coord_t biggest_width = 0;
                    for (const Arachne::ExtrusionJunction &ej : el.junctions) {
                        biggest_width = std::max(biggest_width, coord_t(ej.w));
                    }
                    Polyline polyline = el.toPolyline();
                    biggest_width = std::min(biggest_width, coord_t(polyline.length() * 2) / 3);
                    polyline.clip_start(biggest_width / 2);
                    polyline.clip_end(biggest_width / 2);
                    for (Polygon &polygon : offset(polyline, double(biggest_width / 2))) {
                        perimeter_boundary.emplace_back(std::move(polygon));
                    }
                    has_tw = true;
                }
            }
        }
    }
    if (perimeter_boundary.size() > 1) {
        perimeter_boundary = union_ex(perimeter_boundary);
    }
    //offset2 to fusion, as the thin walls need to attach
    if(has_tw) {
        perimeter_boundary = offset2_ex(perimeter_boundary, params.get_ext_perimeter_spacing() / 8,
                                        -params.get_ext_perimeter_spacing() / 8);
    }

    int start_perimeter = int(perimeters.size()) - 1;
    int end_perimeter = -1;
    int direction = -1;

    if (params.config.external_perimeters_first) {
        start_perimeter = 0;
        end_perimeter = int(perimeters.size());
        direction = 1;
    }

    std::vector<Arachne::ExtrusionLine*> all_extrusions;
    for (int perimeter_idx = start_perimeter; perimeter_idx != end_perimeter; perimeter_idx += direction) {
        if (perimeters[perimeter_idx].empty())
            continue;
        for (Arachne::ExtrusionLine& wall : perimeters[perimeter_idx])
            all_extrusions.emplace_back(&wall);
    }

    // Find topological order with constraints from extrusions_constrains.
    std::vector<size_t>              blocked(all_extrusions.size(), 0); // Value indicating how many extrusions it is blocking (preceding extrusions) an extrusion.
    std::vector<std::vector<size_t>> blocking(all_extrusions.size());   // Each extrusion contains a vector of extrusions that are blocked by this extrusion.
    ankerl::unordered_dense::map<const Arachne::ExtrusionLine*, size_t> map_extrusion_to_idx;
    for (size_t idx = 0; idx < all_extrusions.size(); idx++)
        map_extrusion_to_idx.emplace(all_extrusions[idx], idx);

    
    //TODO: order extrusion for contour/hole separatly
    bool reverse_order = params.config.external_perimeters_first.value
        || (params.object_config.brim_width.value > 0 && params.layer->id() == 0)
        || (params.object_config.brim_width_interior.value > 0 && params.layer->id() == 0);
    Arachne::WallToolPaths::ExtrusionLineSet extrusions_constrains = Arachne::WallToolPaths::getRegionOrder(all_extrusions, reverse_order);
    for (auto [before, after] : extrusions_constrains) {
        auto after_it = map_extrusion_to_idx.find(after);
        ++blocked[after_it->second];
        blocking[map_extrusion_to_idx.find(before)->second].emplace_back(after_it->second);
    }

    std::vector<bool> processed(all_extrusions.size(), false);          // Indicate that the extrusion was already processed.
    Point             current_position = all_extrusions.empty() ? Point::Zero() : all_extrusions.front()->junctions.front().p; // Some starting position.
    std::vector<PerimeterGeneratorArachneExtrusion> ordered_extrusions;         // To store our result in. At the end we'll std::swap.
    ordered_extrusions.reserve(all_extrusions.size());

    while (ordered_extrusions.size() < all_extrusions.size()) {
        this->throw_if_canceled();
        size_t best_candidate = 0;
        double best_distance_sqr = std::numeric_limits<double>::max();
        bool   is_best_closed = false;

        std::vector<size_t> available_candidates;
        for (size_t candidate = 0; candidate < all_extrusions.size(); ++candidate) {
            if (processed[candidate] || blocked[candidate])
                continue; // Not a valid candidate.
            available_candidates.push_back(candidate);
        }

        std::sort(available_candidates.begin(), available_candidates.end(), [&all_extrusions](const size_t a_idx, const size_t b_idx) -> bool {
            return all_extrusions[a_idx]->is_closed < all_extrusions[b_idx]->is_closed;
            });

        for (const size_t candidate_path_idx : available_candidates) {
            auto& path = all_extrusions[candidate_path_idx];

            if (path->junctions.empty()) { // No vertices in the path. Can't find the start position then or really plan it in. Put that at the end.
                if (best_distance_sqr == std::numeric_limits<double>::max()) {
                    best_candidate = candidate_path_idx;
                    is_best_closed = path->is_closed;
                }
                continue;
            }

            const Point candidate_position = path->junctions.front().p;
            double      distance_sqr = (current_position - candidate_position).cast<double>().norm();
            if (distance_sqr < best_distance_sqr) { // Closer than the best candidate so far.
                if (path->is_closed || (!path->is_closed && best_distance_sqr != std::numeric_limits<double>::max()) || (!path->is_closed && !is_best_closed)) {
                    best_candidate = candidate_path_idx;
                    best_distance_sqr = distance_sqr;
                    is_best_closed = path->is_closed;
                }
            }
        }

        Arachne::ExtrusionLine* best_path = all_extrusions[best_candidate];
        ordered_extrusions.push_back({ best_path, best_path->is_contour(), false });
        processed[best_candidate] = true;
        for (size_t unlocked_idx : blocking[best_candidate])
            blocked[unlocked_idx]--;

        if (!best_path->junctions.empty()) { //If all paths were empty, the best path is still empty. We don't upate the current position then.
            if (best_path->is_closed)
                current_position = best_path->junctions[0].p; //We end where we started.
            else
                current_position = best_path->junctions.back().p; //Pick the other end from where we started.
        }
    }

    // fuzzify
    if (params.layer->id() > 0 && params.config.fuzzy_skin != FuzzySkinType::None) {
        std::vector<PerimeterGeneratorArachneExtrusion*> closed_loop_extrusions;
        for (PerimeterGeneratorArachneExtrusion &extrusion : ordered_extrusions) {
            if (extrusion.extrusion->inset_idx == 0 || params.config.fuzzy_skin == FuzzySkinType::All) {
                if (extrusion.extrusion->is_closed && params.config.fuzzy_skin == FuzzySkinType::External) {
                    closed_loop_extrusions.emplace_back(&extrusion);
                } else {
                    extrusion.fuzzify = true;
                }
            }
        }

        if (params.config.fuzzy_skin == FuzzySkinType::External) {
            ClipperLib_Z::Paths loops_paths;
            loops_paths.reserve(closed_loop_extrusions.size());
            for (const auto& cl_extrusion : closed_loop_extrusions) {
                assert(cl_extrusion->extrusion->junctions.front() == cl_extrusion->extrusion->junctions.back());
                size_t             loop_idx = &cl_extrusion - &closed_loop_extrusions.front();
                ClipperLib_Z::Path loop_path;
                loop_path.reserve(cl_extrusion->extrusion->junctions.size() - 1);
                for (auto junction_it = cl_extrusion->extrusion->junctions.begin(); junction_it != std::prev(cl_extrusion->extrusion->junctions.end()); ++junction_it)
                    loop_path.emplace_back(junction_it->p.x(), junction_it->p.y(), loop_idx);
                loops_paths.emplace_back(loop_path);
            }

            ClipperLib_Z::Clipper clipper;
            clipper.AddPaths(loops_paths, ClipperLib_Z::ptSubject, true);
            ClipperLib_Z::PolyTree loops_polytree;
            clipper.Execute(ClipperLib_Z::ctUnion, loops_polytree, ClipperLib_Z::pftEvenOdd, ClipperLib_Z::pftEvenOdd);

            for (const ClipperLib_Z::PolyNode* child_node : loops_polytree.Childs) {
                // The whole contour must have the same index.
                coord_t polygon_idx = child_node->Contour.front().z();
                bool    has_same_idx = std::all_of(child_node->Contour.begin(), child_node->Contour.end(),
                    [&polygon_idx](const ClipperLib_Z::IntPoint& point) -> bool { return polygon_idx == point.z(); });
                if (has_same_idx)
                    closed_loop_extrusions[polygon_idx]->fuzzify = true;
            }
        }
    }
    
        this->throw_if_canceled();
    if (ExtrusionEntityCollection extrusion_coll = _traverse_extrusions(params, ordered_extrusions); !extrusion_coll.empty()) {
        extrusion_coll.set_can_sort_reverse(false, false);
        loops.append(extrusion_coll);
    }

    const coord_t spacing = (perimeters.size() == 1) ? params.ext_perimeter_spacing2 : params.get_perimeter_spacing();
    if (offset_ex(infill_contour, -float(spacing / 2.)).empty()) {
        // Infill region is too small, so let's filter it out.
        result.gap_srf = infill_contour;
    } else {
        result.inner_perimeter = infill_contour;
    }

    return result;
}

void PerimeterGenerator::split_top_surfaces(const ExPolygons *lower_slices,
                                            const ExPolygons *upper_slices,
                                            const ExPolygons &orig_polygons,
                                            ExPolygons &top_fills,
                                            ExPolygons &non_top_polygons,
                                            ExPolygons &fill_clip,
                                            int peri_count,
                                            coordf_t min_width,
                                            bool use_old_algorithm_for_min_width) {
    // other perimeters
    const coord_t perimeter_width   = params.perimeter_flow.scaled_width();
    const coord_t perimeter_spacing = params.perimeter_flow.scaled_spacing();

    // external perimeters
    const coord_t ext_perimeter_width   = params.ext_perimeter_flow.scaled_width();
    const coord_t ext_perimeter_spacing = params.ext_perimeter_flow.scaled_spacing();

    const double fill_nozzle_diameter = params.solid_infill_flow.nozzle_diameter();

    const bool has_gap_fill = !params.use_arachne && (params.region_setting.has_many_config(&params.config.gap_fill_enabled) ||
                params.region_setting.get_solo_config(&params.config.gap_fill_enabled).get_bool());

    // split the polygons with top/not_top
    // get the offset from solid surface anchor*
    const double max_perimeters_width = unscaled(double(params.get_ext_perimeter_width() + perimeter_spacing * int(peri_count - 1))); 
    coord_t offset_top_surface = scale_i(params.config.external_infill_margin.get_effective_value(peri_count == 0 ? 0. : max_perimeters_width));
    // if possible, try to not push the extra perimeters inside the sparse infill
    // 469c6f 
    //offset_top_surface = std::min(offset_top_surface, perimeter_spacing / 3);
    // before 469c6f
    if (offset_top_surface > 0.9 * (peri_count <= 1 ? 0. : (perimeter_spacing * (peri_count - 1))))
        offset_top_surface -= coord_t(0.9 * (peri_count <= 1 ? 0. : (perimeter_spacing * (peri_count - 1))));
    else
        offset_top_surface = 0;
    //offset_top_surface = (peri_count + 1) * perimeter_spacing -perimeter_width +
    //      ;
    // don't takes into account too thin areas
    // skip if the exposed area is smaller than "min_width_top_surface"
    coordf_t min_width_top_surface = std::max(coordf_t(params.get_ext_perimeter_spacing() / 2 + 10), min_width);

    Polygons grown_upper_slices;
    if (!use_old_algorithm_for_min_width) {
        // 469c6f
        //grown_upper_slices = offset2(*upper_slices, - min_width_top_surface - offset_top_surface, min_width_top_surface);
        // before 469c6f
        grown_upper_slices = offset(*upper_slices, min_width_top_surface);
    } else {
        ExPolygons grown_accumulator;
        // make thin upper surfaces disapear with -+offset_top_surface
        // do offset2 per island, to avoid big blob merging
        // remove polygon too thin (but don't mess with holes)
        for (const ExPolygon &expoly_to_grow : *this->upper_slices) {
            // only offset the contour, as it can merge holes
            // 469c6f
            //Polygons contour = offset2(ExPolygons{ExPolygon{expoly_to_grow.contour}}, - min_width_top_surface - offset_top_surface,
            //                           min_width_top_surface + (this->mill_extra_size > SCALED_EPSILON ? (double) mill_extra_size : 0));
            // before 469c6f
            Polygons contour = offset2(ExPolygons{ExPolygon{expoly_to_grow.contour}}, -offset_top_surface,
                                       offset_top_surface + min_width_top_surface +
                                           (this->mill_extra_size > SCALED_EPSILON ? (double) mill_extra_size : 0));
            if (!contour.empty()) {
                if (expoly_to_grow.holes.empty()) {
                    for (Polygon &p : contour) grown_accumulator.push_back(ExPolygon{p});
                } else {
                    Polygons holes = expoly_to_grow.holes;
                    for (Polygon &h : holes) h.reverse();
                    // 469c6f
                    //holes = offset(holes, - offset_top_surface
                    //    - ((this->mill_extra_size > SCALED_EPSILON) ? (double) mill_extra_size : 0));
                    // before 469c6f
                    holes = offset(holes,
                                   -min_width_top_surface -
                                       ((this->mill_extra_size > SCALED_EPSILON) ? (double) mill_extra_size : 0));
                    for (ExPolygon p : diff_ex(contour, holes)) grown_accumulator.push_back(p);
                }
            }
        }
        grown_upper_slices = union_(grown_accumulator);
    }

    // get boungding box of last
    BoundingBox last_box = get_extents(orig_polygons);
    last_box.offset(SCALED_EPSILON);

    // get the Polygons upper the polygon this layer
    Polygons upper_polygons_series_clipped = ClipperUtils::clip_clipper_polygons_with_subject_bbox(grown_upper_slices, last_box);

    // set the clip to a virtual "second perimeter"
    fill_clip = offset_ex(orig_polygons, -coordf_t(params.get_ext_perimeter_spacing()));
    // Check whether surface be bridge or not
    ExPolygons bridge_checker;
    // maybe add a parameter to fuse bridge in the one-perimeter area, instead of excuding it.
    if (lower_slices != nullptr) {
        // BBS: get the Polygons below the polygon this layer
        Polygons lower_polygons_series_clipped = ClipperUtils::clip_clipper_polygons_with_subject_bbox(*lower_slices, last_box);
        coordf_t bridge_offset = perimeter_spacing * peri_count;
        // SoftFever: improve bridging
        bridge_offset += scale_d(params.config.bridged_infill_margin.get_effective_value(unscaled(params.get_ext_perimeter_width())));
        bridge_checker = diff_ex(orig_polygons, lower_polygons_series_clipped, ApplySafetyOffset::Yes);
        // increase by a perimeter at a time and clip it to avoid going over a gap
        // these quantum tunneling areas can be erased by a offset2, but that big offset2 may also erase evrything. so it needs to be small.
        while (bridge_offset > SCALED_EPSILON) {
            coordf_t current_offset = perimeter_spacing;
            if (bridge_offset < perimeter_spacing * 1.5) {
                current_offset = bridge_offset;
            }
            bridge_offset -= current_offset;
            bridge_checker = offset_ex(bridge_checker, current_offset);
            // the offset2 reduce a bit the overlap with top infill on the edges with high bridge_offset. can be improved.
            bridge_checker = offset2_ex(intersection_ex(bridge_checker, orig_polygons), -current_offset, current_offset);
        }
    }
    const ExPolygons *orig_poly_without_bridge = &orig_polygons;
    if (!bridge_checker.empty()) {
        bridge_checker = diff_ex(orig_polygons, bridge_checker, ApplySafetyOffset::Yes);
        orig_poly_without_bridge = &bridge_checker;
    }
    // get the real top surface
    ExPolygons top_polygons;
    if (this->mill_extra_size < SCALED_EPSILON) {
        top_polygons = diff_ex(*orig_poly_without_bridge, upper_polygons_series_clipped, ApplySafetyOffset::Yes);
    } else if (this->unmillable.empty()) {
        top_polygons = diff_ex(*orig_poly_without_bridge, offset_ex(upper_polygons_series_clipped, (double) mill_extra_size), ApplySafetyOffset::Yes);
    } else {
        top_polygons = diff_ex(*orig_poly_without_bridge,
                               diff_ex(offset_ex(upper_polygons_series_clipped, (double) mill_extra_size), 
                                   unmillable, ApplySafetyOffset::Yes));
    }
    // save the top area for gap fill, or something. Made by BB/orca, but no comment.
    ExPolygons temp_gap = diff_ex(top_polygons, fill_clip);
    // get the not-top surface, from the "real top" but enlarged by external_infill_margin (and the
    // min_width_top_surface we removed a bit before)
    // also remove the params.get_ext_perimeter_spacing()/2 width because we are faking the external perimeter, and we will remove params.get_ext_perimeter_spacing()2
    // 469c6f
    //ExPolygons inner_polygons = diff_ex(orig_polygons, offset_ex(top_polygons, - double(params.get_ext_perimeter_spacing() / 2)),
    //                                    ApplySafetyOffset::Yes);
    // before 469c6f
    ExPolygons inner_polygons = diff_ex(orig_polygons,
                                        offset_ex(top_polygons,
                                                  offset_top_surface + min_width_top_surface -
                                                      double(params.get_ext_perimeter_spacing() / 2)),
                                        ApplySafetyOffset::Yes);
    
    // get the enlarged top surface, by using inner_polygons instead of upper_slices, and clip it for it to be exactly
    // the polygons to fill.
    top_polygons = diff_ex(fill_clip, inner_polygons, ApplySafetyOffset::Yes);
    // increase by half peri the inner space to fill the frontier between last and stored.
    top_fills = union_ex(top_fills, top_polygons);
    // set the clip to the external wall but go back inside by infill_extrusion_width/2 to be sure the extrusion won't
    // go outside even with a 100% overlap.
    double infill_spacing_unscaled = params.config.infill_extrusion_width.get_effective_value(fill_nozzle_diameter);
    if (infill_spacing_unscaled == 0)
        infill_spacing_unscaled = Flow::auto_extrusion_width(frInfill, fill_nozzle_diameter);
    fill_clip = offset_ex(orig_polygons, double(params.get_ext_perimeter_spacing() / 2) - scale_d(infill_spacing_unscaled / 2));

    ExPolygons new_non_top_polygons = intersection_ex(inner_polygons, orig_polygons);
    // Made by BB/orca, but no comment. Plz test it and report the usefullness.
    if (has_gap_fill)
        new_non_top_polygons = union_ex(new_non_top_polygons, temp_gap);
    non_top_polygons = union_ex(non_top_polygons, new_non_top_polygons);

    //if (!top_fills.empty() && !non_top_polygons.empty()) {
    //    static int i = 0;
    //    i++;
    //    std::stringstream stri;
    //    stri << params.layer->id() << "_1_" << i << "_only_one_peri"
    //         << ".svg";
    //    SVG svg(stri.str());
    //    svg.draw(to_polylines(top_fills), "green");
    //    svg.draw(to_polylines(inner_polygons), "yellow");
    //    svg.draw(to_polylines(top_polygons), "cyan");
    //    svg.draw(to_polylines(orig_polygons), "orange");
    //    svg.draw(to_polylines(non_top_polygons), "red");
    //    svg.Close();
    //}
}

void PerimeterGenerator::process(// Input:
            const ExPolygon          &island_polygon,
            const ExPolygons *       lower_slices,
            const ExPolygons        &slices,
            const ExPolygons *       upper_slices,
            // Output:
            // Loops with the external thin walls
            ExtrusionEntityCollection *loops,
            // Gaps without the thin walls
            ExtrusionEntityCollection *gap_fill,
            // Infills without the gap fills
            ExPolygons &fill_surfaces,
            // mask for "no overlap" area
            ExPolygons &fill_no_overlap)
{
    //TODO: remove these from member
    this->lower_slices = lower_slices;
    this->slices = &slices;
    this->upper_slices = upper_slices;

    // Calculate the minimum required spacing between two adjacent traces.
    // This should be equal to the nominal flow spacing but we experiment
    // with some tolerance in order to avoid triggering medial axis when
    // some squishing might work. Loops are still spaced by the entire
    // flow spacing; this only applies to collapsing parts.
    // For ext_min_spacing we use the params.get_ext_perimeter_spacing() calculated for two adjacent
    // external loops (which is the correct way) instead of using ext_perimeter_spacing2
    // which is the spacing between external and internal, which is not correct
    // and would make the collapsing (thus the details resolution) dependent on 
    // internal flow which is unrelated. <- i don't undertand, so revert to ext_perimeter_spacing2
    //const coord_t min_spacing     = (coord_t)( perimeter_spacing      * (1 - 0.05/*INSET_OVERLAP_TOLERANCE*/) );
    //const coord_t ext_min_spacing = (coord_t)( ext_perimeter_spacing2  * (1 - 0.05/*INSET_OVERLAP_TOLERANCE*/) );
    // now the tolerance is built in thin_perimeter settings

    // prepare grown lower layer slices for overhang detection
    //note: config.overhangs_width can't be enabled (has to be ignored) if config.overhangs_width_speed is disabled (for now)
    bool overhang_enabled = params.region_setting.has_many_config(&params.config.overhangs) ||
        params.region_setting.get_solo_config(&params.config.overhangs).get_bool();
    const bool overhang_extra_enabled = params.region_setting.has_many_config(&params.config.extra_perimeters_on_overhangs) ||
        params.region_setting.get_solo_config(&params.config.extra_perimeters_on_overhangs).get_bool();
    const bool has_no_gapfill_overhang = (params.region_setting.has_many_config(&params.config.gap_fill_no_overhang) ||
                params.region_setting.get_solo_config(&params.config.gap_fill_no_overhang).get_bool());

    if (this->lower_slices != NULL && (overhang_extra_enabled || overhang_enabled || params.get_overhang_spacing() > 0 || has_no_gapfill_overhang)) {
        // We consider overhang any part where the entire nozzle diameter is not supported by the
        // lower layer, so we take lower slices and offset them by overhangs_width of the nozzle diameter used 
        // in the current layer

        coord_t offset_unprintable = scale_i(this->params.overhang_flow.nozzle_diameter() *
                                             (1 - params.config.thin_perimeters.get_effective_value(0.5)));
        if (params.region_setting.has_many_config(&params.config.thin_walls) ||
            params.region_setting.get_solo_config(&params.config.thin_walls).get_bool()) {
            // not ideal...
            coord_t min_width = scale_i(
                params.config.thin_walls_min_width.get_effective_value(params.ext_perimeter_flow.nozzle_diameter()));
            offset_unprintable = std::min(offset_unprintable, min_width / 2);
        }
        offset_unprintable -= SCALED_EPSILON;
        assert(offset_unprintable > SCALED_EPSILON);
        assert_valid(*lower_slices);
        ExPolygons lower_slices_storage = offset2_ex(*lower_slices, -offset_unprintable, offset_unprintable);
        const ExPolygons *simplified = &lower_slices_storage;
        // simplify the lower slices if too high (means low number) resolution
        const coord_t min_feature = scale_i(this->params.overhang_flow.nozzle_diameter()) / 20;
        if (get_resolution(0, false) < min_feature) {
            ExPolygons simplified_storage;
            for (const ExPolygon &expoly : *simplified) {
                expoly.simplify(min_feature, simplified_storage);
            }
            // note: because of the simplify, you can detect shallow overhang if a spike was deleted.
            //simplified_storage = offset_ex(simplified_storage, this->params.overhang_flow.nozzle_diameter() / 100);
            lower_slices_storage = simplified_storage;
        } else {
            ensure_valid(lower_slices_storage, std::max(SCALED_EPSILON * 2, get_resolution(0, false)));
        }

        params.lower_slices_bridge_for_extra_overhangs.clear();
        if (overhang_extra_enabled || params.get_overhang_spacing() > 0) {
            if (params.region_setting.has_many_config(&params.config.extra_perimeters_on_overhangs)) {
                // offset to ensure it avoid detecting fake overhangs.
                ExPolygons expolys = offset_ex(*simplified, min_feature + SCALED_EPSILON);
                for (auto const &[opt_values, areas] :
                     params.region_setting.get_areas(&params.config.extra_perimeters_on_overhangs)) {
                    if (!opt_values.get_bool(&params.config.extra_perimeters_on_overhangs)) {
                        expolys = union_ex(expolys, areas.expolys);
                    }
                }
                assert(params.lower_slices_bridge_for_extra_overhangs.empty());
                params.lower_slices_bridge_for_extra_overhangs = to_polygons(expolys);
            } else {
                params.lower_slices_bridge_for_extra_overhangs = to_polygons(*simplified);
            }
        }
        if (params.get_overhang_spacing() > 0 || has_no_gapfill_overhang) {
            params.overhang_areas.clear();
            coord_t max_offset = 0;
            ExPolygons full_overhangs;
            if (params.region_setting.has_many_config(&params.config.overhangs) ||
                params.region_setting.get_solo_config(&params.config.overhangs).get_bool()) {
                for (auto const &[opt_values, areas] : params.region_setting.get_areas(&params.config.overhangs)) {
                    const bool overhang_flow_enabled = opt_values.is_enabled(&params.config.overhangs_flow_ratio);
                    if (overhang_flow_enabled && opt_values.get_bool(&params.config.overhangs)) {
                        append(full_overhangs, areas.intersections(ExPolygons{island_polygon}));
                        max_offset =
                            std::max(max_offset,
                                     scale_i(opt_values.get_effective_value(this->params.overhang_flow.nozzle_diameter(),
                                                                      &params.config.overhangs_width)));
                    }
                }
            }
            // create the overhangs areas
            full_overhangs = union_ex(full_overhangs);
            full_overhangs = diff_ex(full_overhangs, *simplified);
            full_overhangs = offset2_ex(full_overhangs, SCALED_EPSILON * 10, SCALED_EPSILON * 10);
            // offset2 the params.overhang_areas to remove overhangs that overlap too much (it overlpa inside the not-overhang area so it cna be removed withotu creating holes)
            params.overhang_areas = offset_ex(full_overhangs, double(max_offset));
            // intersection to remove the part over the surface
            params.overhang_areas = intersection_ex(params.overhang_areas, island_polygon);
            //now shrink where needed
            ExPolygons all_shrinks;
            for (auto const &[opt_values, areas] : params.region_setting.get_areas(&params.config.overhangs)) {
                const bool overhang_flow_enabled = opt_values.is_enabled(&params.config.overhangs_flow_ratio);
                if (overhang_flow_enabled && opt_values.get_bool(&params.config.overhangs)) {
                coord_t overhangs_width_flow = scale_i(
                        opt_values.get_effective_value(this->params.overhang_flow.nozzle_diameter(),
                                                 &params.config.overhangs_width));
                    append(all_shrinks, offset_ex(areas.intersections(params.overhang_areas), double(overhangs_width_flow)));
                }
            }
            // intersect with full_overhangs, to remove the part that are still inside the not-overhang area
            params.overhang_areas = intersection_ex(union_ex(all_shrinks), full_overhangs);
            //offset ? 
            //params.overhang_areas = offset_ex(params.overhang_areas, params.get_overhang_spacing() / 2);
            //params.overhang_areas = intersection_ex(params.overhang_areas, island_polygon);
        }

        const coordf_t half_extperi_offset = (coordf_t)(params.get_ext_perimeter_width() / 2);
        params.lower_slices_bridge_dynamic.clear();
        if (overhang_enabled) {
            params.lower_slices_bridge_dynamic = offset(*simplified,
                                                 (coordf_t) (min_feature + SCALED_EPSILON -
                                                             half_extperi_offset));
        }

        // TODO: clip_clipper_polygons_with_subject_bbox(lower_slices);
        params.lower_slices_bridge_speed_small.clear();
        params.lower_slices_bridge_speed_big.clear();
        params.lower_slices_bridge_flow_small.clear();
        params.lower_slices_bridge_flow_big.clear();
        for (auto const &[opt_values, areas] : params.region_setting.get_areas(&params.config.overhangs)) {
            if (opt_values.get_bool(&params.config.overhangs)) {
                const bool overhang_flow_enabled = opt_values.is_enabled(&params.config.overhangs_flow_ratio);
                const bool overhang_speed_separate = opt_values.is_enabled(&params.config.overhangs_width_speed) || !overhang_flow_enabled;

                ExPolygons region_simplified = areas.intersections(*simplified);

                // we use a range to avoid threshold issues.
                coord_t overhangs_width_flow = 0;
                if (overhang_flow_enabled) {
                    overhangs_width_flow = scale_i(
                        opt_values.get_effective_value(this->params.overhang_flow.nozzle_diameter(),
                                                 &params.config.overhangs_width));
                }
                coord_t overhangs_width_speed = 0;
                if (overhang_speed_separate) {
                    if (opt_values.is_enabled(&params.config.overhangs_width_speed)) {
                        overhangs_width_speed = scale_i(
                            opt_values.get_effective_value(this->params.overhang_flow.nozzle_diameter(),
                                                     &params.config.overhangs_width_speed));
                    } else {
                        overhangs_width_speed = scale_i(
                            opt_values.get_effective_value(this->params.overhang_flow.nozzle_diameter(),
                                                     &params.config.overhangs_width));
                    }
                } else {
                    overhangs_width_speed = overhangs_width_flow;
                }
                coord_t overhangs_width_speed_90 = coord_t(overhangs_width_speed * 0.99);
                coord_t overhangs_width_speed_110 = coord_t(overhangs_width_speed * 1.15);
                coord_t overhangs_width_flow_90 = coord_t(overhangs_width_flow * 0.99);
                coord_t overhangs_width_flow_110 = coord_t(overhangs_width_flow * 1.15);
                if (overhangs_width_speed_90 > 0 && overhangs_width_speed_90 < min_feature) {
                    overhangs_width_speed_90 = min_feature + SCALED_EPSILON;
                }
                if (overhangs_width_speed_110 > 0 && overhangs_width_speed_110 < min_feature) {
                    overhangs_width_speed_110 = min_feature + SCALED_EPSILON;
                }
                if (overhangs_width_flow_90 > 0 && overhangs_width_flow_90 < min_feature) {
                    overhangs_width_flow_90 = min_feature + SCALED_EPSILON;
                }
                if (overhangs_width_flow_110 > 0 && overhangs_width_flow_110 < min_feature) {
                    overhangs_width_flow_110 = min_feature + SCALED_EPSILON;
                }
                //overhangs_width_speed_90 -= half_extperi_offset;
                //overhangs_width_speed_110 -= half_extperi_offset;
                //overhangs_width_flow_90 -= half_extperi_offset;
                //overhangs_width_flow_110 -= half_extperi_offset;

                // flow offset should be greater than speed offset because the flow apply also the speed.
                // check if overhangs_width_speed is low enough to be relevant (if flow is activated)
                if (overhang_flow_enabled) {
                    if (overhangs_width_flow_90 < overhangs_width_speed_110) {
                        overhangs_width_speed_110 = overhangs_width_flow_90 = (overhangs_width_speed_110 + overhangs_width_flow_90) / 2;
                    }
                }

                // FIXME: can remove thinalls from support. you need to take them back, but they are computed in // ...
                // for overhangs detection
                Polygons offseted = offset(region_simplified,
                                           (coordf_t) (overhangs_width_speed_90 + SCALED_EPSILON - half_extperi_offset));
                params.lower_slices_bridge_speed_small = union_(params.lower_slices_bridge_speed_small, offseted);
                if (overhangs_width_speed_110 > overhangs_width_speed_90) {
                    offseted = offset(offseted, (coordf_t) (overhangs_width_speed_110 - overhangs_width_speed_90));
                }
                params.lower_slices_bridge_speed_big = union_(params.lower_slices_bridge_speed_big, offseted);
                if (overhang_flow_enabled) {
                    if (overhangs_width_flow_90 > overhangs_width_speed_110) {
                        offseted = offset(offseted, (coordf_t) (overhangs_width_flow_90 - overhangs_width_speed_110));
                    }
                    params.lower_slices_bridge_flow_small = union_(params.lower_slices_bridge_flow_small, offseted);
                    if (overhangs_width_flow_110 > overhangs_width_flow_90) {
                        offseted = offset(offseted, (coordf_t) (overhangs_width_flow_110 - overhangs_width_flow_90));
                    }
                    params.lower_slices_bridge_flow_big = union_(params.lower_slices_bridge_flow_big, offseted);
                } else {
                    // no flow: use the biggest and sipliest bb so all the extrusion will be inside.
                    offseted.clear();
                    if (areas.is_accept_all()) {
                        // only one region, go big.
                        offseted = {get_extents(island_polygon.contour).polygon()};
                    } else {
                        //multiple region, ensure it's inside bounds.
                        offseted = union_(areas.expolys);
                    }
                    params.lower_slices_bridge_flow_small = union_(params.lower_slices_bridge_flow_small, offseted);
                    params.lower_slices_bridge_flow_big = union_(params.lower_slices_bridge_flow_big, offseted);
                }
            }
        }

    }
    this->throw_if_canceled();

    // have to grown the perimeters if mill post-process
    MillingPostProcess miller(&slices, lower_slices, params.config, params.object_config, params.print_config);
    bool have_to_grow_for_miller = miller.can_be_milled(params.layer) && params.config.milling_extra_size.get_effective_value(1) > 0;
    this->mill_extra_size = 0;
    if (have_to_grow_for_miller) {
        this->unmillable = miller.get_unmillable_areas(params.layer);
        double spacing_vs_width = params.ext_perimeter_flow.width() - params.ext_perimeter_flow.spacing();
        this->mill_extra_size = scale_i(params.config.milling_extra_size.get_effective_value(spacing_vs_width));
        have_to_grow_for_miller = this->mill_extra_size > SCALED_EPSILON;
    }

    // we process only one island, it's because we might have different
    // extra perimeters for each one

    ExPolygons new_islands = processs_no_bridge(params, island_polygon, fill_surfaces);

    int surface_idx = 0;
    // extra perimeter if on the right layer id and if the setting is for the whole region. (if it's split in multiple regions, then we ned to clip it after textruding current ones)
    const int extra_odd_perimeter = params.layer->id() % 2 == 1 &&
            !params.region_setting.has_many_config(&params.config.extra_perimeters_odd_layers) &&
            params.region_setting.get_solo_config(&params.config.extra_perimeters_odd_layers).get_bool() ?
        1 :
        0;

    // if only one OPT_EXTRA_PERIMETER, then just add it to nb_loop_contour & nb_loop_hole
    const int extra_surface_perimeter = (!params.region_setting.has_many_config(&Parameters::OPT_EXTRA_PERIMETER)) ?
        params.region_setting.get_solo_config(&Parameters::OPT_EXTRA_PERIMETER).get_int() :
        0;

    for (const ExPolygon& surface_expolygon : new_islands) {
        surface_idx++;

        // detect how many perimeters must be generated for this island
        int nb_loop_contour = params.config.perimeters;
        assert(nb_loop_contour >= 0);
        assert(params.config.perimeters.is_enabled());
        if (nb_loop_contour > 0)
            nb_loop_contour += extra_odd_perimeter + extra_surface_perimeter;
        assert(nb_loop_contour >= 0);
        int nb_loop_holes = params.config.perimeters_hole.value;
        assert(nb_loop_holes >= 0);
        if (params.config.perimeters_hole.is_enabled() && nb_loop_holes > 0)
            nb_loop_holes += extra_odd_perimeter + extra_surface_perimeter;
        assert(nb_loop_holes >= 0);
        
        if (!params.config.perimeters_hole.is_enabled())
            nb_loop_holes = std::max(0, nb_loop_contour);

        if (params.print_config.spiral_vase) {
            if (params.layer->id() >= params.config.bottom_solid_layers) {
                nb_loop_contour = 1;
                nb_loop_holes = 0;
            }
        }

        bool has_only_one_perimeter_top = (params.region_setting.has_many_config(&params.config.only_one_perimeter_top) ||
                                           params.region_setting.get_solo_config(&params.config.only_one_perimeter_top).get_bool());
        if ((params.layer->id() == 0 && params.config.only_one_perimeter_first_layer) ||
            (has_only_one_perimeter_top && this->upper_slices == NULL)) {
            nb_loop_contour = std::min(nb_loop_contour, 1);
            nb_loop_holes = std::min(nb_loop_holes, 1);
        }

        if (!params.region_setting.has_many_config(&params.config.extra_perimeters_count)) {
            int extra_perimeters_count = params.region_setting.get_solo_config(&params.config.extra_perimeters_count).get_int();
            if (extra_perimeters_count > 0) {
                nb_loop_contour += extra_perimeters_count;
                nb_loop_holes += extra_perimeters_count;
            }
        }

        // get first index to add extra overhangs.
        size_t first_loop_coll_index = loops->size();

        ProcessSurfaceResult surface_process_result;
        //core generation
        if (params.use_arachne && !params.config.perimeters_hole.is_enabled()) {
            surface_process_result = process_arachne(params, nb_loop_contour, surface_expolygon, *loops);
            nb_loop_holes = nb_loop_contour; // nb_loop_contour is in/out
        } else {
            surface_process_result = process_classic(params, nb_loop_contour, nb_loop_holes, surface_expolygon, *loops, *gap_fill);
        }
        // boundaries if no perimeters
        if (nb_loop_contour == 0 || nb_loop_holes == 0) {
            perimeter_boundary = union_ex(perimeter_boundary, surface_expolygon);
        }
        this->throw_if_canceled();
        for(auto *peri : loops->entities()) assert(!peri->empty());

        // test for loops
        class ExtrusionTransformPathIntoLoop : public ExtrusionVisitorRecursive {
            std::vector<ExtrusionEntity*> current_entity;
            using ExtrusionVisitorRecursive::use;
            void default_use(ExtrusionEntity &entity) override {
                if (!entity.is_leaf()) {
                    if (entity.is_loop() || entity.is_continuous())
                        return;
                    for (ExtrusionEntityUPtr &child : entity.children()) {
                        current_entity.push_back(child.get());
                        child->visit(*this);
                        if (child.get() != current_entity.back())
                            child.reset(current_entity.back());
                        current_entity.pop_back();
                    }
                    return;
                }
                ExtrusionPath *path = dynamic_cast<ExtrusionPath*>(&entity);
                if (path == nullptr)
                    return;
                if (path->first_point().coincides_with_epsilon(path->last_point())) {
                    assert(false);
                    assert(path == current_entity.back());
                    current_entity.back() = new ExtrusionLoop(ExtrusionPaths{*path}, ExtrusionLoopRole::elrDefault);
                }
            }
        } transformer;
        loops->visit(transformer);
        for(auto *peri : loops->entities()) assert(!peri->empty());

        // create one more offset to be used as boundary for fill
        // we offset by half the perimeter spacing (to get to the actual infill boundary)
        // and then we offset back and forth by half the infill spacing to only consider the
        // non-collapsing regions
        coord_t infill_peri_overlap = 0;
        // only apply infill overlap if we actually have one perimeter
        if (nb_loop_contour > 0 || nb_loop_holes > 0) {
            //give the overlap size to let the infill do his overlap
            //add overlap if at least one perimeter
            coordf_t perimeter_spacing_for_encroach = 0;
            if(params.config.perimeters == 1)
                perimeter_spacing_for_encroach = params.ext_perimeter_flow.spacing();
            else if (has_only_one_perimeter_top)
                //note: use the min of the two to avoid overextrusion if only one perimeter top
                // TODO: only do that if there is a top & a not-top surface
                perimeter_spacing_for_encroach = std::min(params.perimeter_flow.spacing(), params.ext_perimeter_flow.spacing());
            else //if(layerm->region().config().perimeters > 1)
                perimeter_spacing_for_encroach = params.perimeter_flow.spacing();
            infill_peri_overlap = scale_i(params.config.option("infill_overlap")->get_effective_value(perimeter_spacing_for_encroach));
        }

        // simplify infill contours according to resolution
        Polygons not_filled_p;
        coord_t scaled_resolution_infill =
            std::min(params.get_solid_infill_spacing() / 16,
                     std::max(SCALED_EPSILON,
                              scale_i(std::max(params.print_config.resolution_internal.value,
                                               params.print_config.resolution.value))));
        for (const ExPolygon& ex : surface_process_result.inner_perimeter)
            ex.simplify_p(scaled_resolution_infill, not_filled_p);
        ExPolygons not_filled_exp = union_ex(not_filled_p);
        // collapse too narrow infill areas
        coord_t min_perimeter_infill_spacing = (coord_t)(params.get_solid_infill_spacing() * (1. - INSET_OVERLAP_TOLERANCE));
        ExPolygons infill_exp;
        infill_exp = offset2_ex(not_filled_exp,
            double(- min_perimeter_infill_spacing / 2 + infill_peri_overlap - params.get_infill_gap()),
            double(min_perimeter_infill_spacing / 2));
        //special branch if gap : don't inset away from gaps!
        ExPolygons gap_fill_exps;
        if (!surface_process_result.gap_srf.empty()) {
            //remove gaps surfaces
            //not_filled_p.clear();
            gap_fill_exps = surface_process_result.gap_srf;
            ensure_valid(gap_fill_exps, scaled_resolution_infill);
            // infill_exp is already offseted by infill_peri_overlap, so we need to offset ourself by that much.
            gap_fill_exps = offset_ex(gap_fill_exps, infill_peri_overlap);
            infill_exp = diff_ex(infill_exp, gap_fill_exps);
        }
        for(auto *peri : loops->entities()) assert(!peri->empty());

        //if any top_fills, grow them by params.get_ext_perimeter_spacing()/2 to have the real un-anchored fill
        ExPolygons top_infill_exp = intersection_ex(surface_process_result.fill_clip, offset_ex(surface_process_result.top_fills, double(params.get_ext_perimeter_spacing() / 2)));
        if (!surface_process_result.top_fills.empty()) {
            append(infill_exp, offset_ex(top_infill_exp, double(infill_peri_overlap)));
            infill_exp = union_ex(infill_exp);
        }
        
        ExPolygons polyWithoutOverlap;
        if (infill_peri_overlap != 0) {
            if (min_perimeter_infill_spacing / 2 > infill_peri_overlap)
                polyWithoutOverlap = offset2_ex(
                    not_filled_exp,
                    double(- params.get_infill_gap() - min_perimeter_infill_spacing / 2 + infill_peri_overlap),
                    double(min_perimeter_infill_spacing / 2 - infill_peri_overlap));
            else
                polyWithoutOverlap = offset_ex(
                    not_filled_exp,
                    double(- params.get_infill_gap()));
            if (!gap_fill_exps.empty()) {
                polyWithoutOverlap = diff_ex(polyWithoutOverlap, gap_fill_exps);
            }
            if (!surface_process_result.top_fills.empty()) {
                append(polyWithoutOverlap, top_infill_exp);
                polyWithoutOverlap = union_ex(polyWithoutOverlap);
            }
            //{
            //    static int isaqsdsdfsdfqzfn = 0;
            //    std::stringstream stri;
            //    stri << params.layer->id() << "_makeperimeter_no_overlap_" << isaqsdsdfsdfqzfn++ << ".svg";
            //    SVG svg(stri.str());
            //    svg.draw(surface.expolygon, "grey");
            //    svg.draw(loops->polygons_covered_by_spacing(1, SCALED_EPSILON), "red");
            //    svg.draw(to_polylines(infill_exp), "blue", scale_i(0.14));
            //    svg.draw(to_polylines(fill_no_overlap), "cyan", scale_i(0.12));
            //    svg.draw(to_polylines(not_filled_exp), "green", scale_i(0.10));
            //    svg.draw(to_polylines(polyWithoutOverlap), "yellow", scale_i(0.08));
            //    //svg.draw(to_polylines(offset_ex(surface_process_result.fill_clip, ext_perimeter_spacing / 2)), "brown");
            //    svg.draw(to_polylines(top_infill_exp), "orange", scale_i(0.06));
            //    svg.draw(to_polylines(surface_process_result.fill_clip), "purple", scale_i(0.04));
            //    ArcPolylines polys;
            //    loops->collect_polylines(polys);
            //    for(auto & poly : polys)
            //        svg.draw(poly.to_polyline(), "pink", scale_i(0.02));
            //    svg.Close();
            //}
        }
        
        if (lower_slices != nullptr &&
            overhang_extra_enabled &&
            params.config.perimeters > 0 && params.layer->id() > params.object_config.raft_layers) {

            const ExPolygons *infill_area = polyWithoutOverlap.empty() ? &infill_exp : &polyWithoutOverlap;
            ExPolygons infill_areas_without_no_extra_overhangs;
            if (params.region_setting.has_many_config(&params.config.extra_perimeters_on_overhangs)) {
                for (auto const &[opt_values, areas] :
                     params.region_setting.get_areas(&params.config.extra_perimeters_on_overhangs)) {
                    if (!opt_values.get_bool(&params.config.extra_perimeters_on_overhangs)) {
                        infill_areas_without_no_extra_overhangs = diff_ex(*infill_area, areas.expolys);
                    }
                }
                infill_area = &infill_areas_without_no_extra_overhangs;
            }

            // Generate extra perimeters on overhang areas, and cut them to these parts only, to save print time and material
            auto [extra_perimeters, filled_area, unfilled_area] = generate_extra_perimeters_over_overhangs(surface_expolygon,
                                                                                            *infill_area,
                                                                                            params,
                                                                                            std::min(nb_loop_holes, nb_loop_contour) + 1,
                                                                                            scaled_resolution_infill);
            if (!extra_perimeters.empty()) {

                if (params.region_setting.has_many_config(&params.config.extra_perimeters_on_overhangs)) {
                    for (auto const &[opt_values, areas] :
                         params.region_setting.get_areas(&params.config.extra_perimeters_on_overhangs)) {
                        if (!opt_values.get_bool(&params.config.extra_perimeters_on_overhangs)) {
                            unfilled_area = union_ex(unfilled_area, areas.expolys);
                        }
                    }
                }

                //put these new overhangs into their own unsortable collection.
                ExtrusionEntityCollection this_islands_perimeters(false, false);
                // put extra perimeter as first printed
                for (ExtrusionPaths &paths : extra_perimeters) {
                    if(paths.empty()) continue;
                    for (ExtrusionPath &path : paths) {
                        if(path.empty()) continue;
                        if (path.first_point().coincides_with_epsilon(path.last_point())) {
                            //it's a loop!
                            this_islands_perimeters.append(ExtrusionLoop(path, ExtrusionLoopRole::elrDefault));
                        } else {
                            this_islands_perimeters.append(std::move(path));
                        }
                    }
                }
                assert(loops->entities().size() >= first_loop_coll_index);
                if (!this_islands_perimeters.empty()) {
                    for (auto *peri : loops->entities()) assert(!peri->empty());
                    // move the perimeters of the island in the unsortable collection, so the ordering is preserved
                    while (loops->size() > first_loop_coll_index) {
                        assert(!loops->entities()[first_loop_coll_index]->empty());
                        this_islands_perimeters.append(loops->release(first_loop_coll_index));
                    }
                    assert(loops->size() == first_loop_coll_index);
                    // add this_islands_perimeters (back) into loops.
                    loops->append(std::move(this_islands_perimeters));
                    for (auto *peri : loops->entities()) assert(!peri->empty());
                    // clip infill area
                    // TODO: 2.7 test if ok for infill_peri_overlap -> NOT OK FIXME
                    if (infill_peri_overlap != 0) {
                        polyWithoutOverlap = diff_ex(polyWithoutOverlap, filled_area);
                        infill_exp = intersection_ex(infill_exp, offset_ex(unfilled_area, infill_peri_overlap));
                    } else {
                        infill_exp = diff_ex(infill_exp, filled_area);
                    }
            //{
            //    static int isaqsdsdfsdfqzfn = 0;
            //    std::stringstream stri;
            //    stri << params.layer->id() << "_4_end_generate_extra_perimeters_over_overhangs_" << isaqsdsdfsdfqzfn++ << ".svg";
            //    SVG svg(stri.str());
            //    svg.draw(to_polylines(infill_exp_bef), "purple", scale_i(0.16));
            //    svg.draw(to_polylines(filled_area), "pink", scale_i(0.15));
            //    svg.draw(to_polylines(infill_exp), "blue", scale_i(0.14));
            //    svg.draw(to_polylines(fill_no_overlap), "cyan", scale_i(0.13));
            //    svg.draw(to_polylines(not_filled_exp), "green", scale_i(0.12));
            //    //svg.draw(to_polylines(last_no_gaps), "yellow", scale_i(0.11));
            //    //svg.draw(to_polylines(offset_ex(surface_process_result.fill_clip, ext_perimeter_spacing / 2)), "brown");
            //    //svg.draw(to_polylines(top_infill_exp), "orange", scale_i(0.1));
            //    svg.Close();
            //}
                }
            }
        }
         //{
         //       static int isaqsdsdfsdfqzfn = 0;
         //       std::stringstream stri;
         //       stri << params.layer->id() << "_9_end_makeperimeter_" << isaqsdsdfsdfqzfn++ << ".svg";
         //       SVG svg(stri.str());
         //       svg.draw(surface.expolygon, "grey");
         //       svg.draw(loops->polygons_covered_by_spacing(1, SCALED_EPSILON), "red");
         //       svg.draw(to_polylines(infill_exp), "blue", scale_i(0.14));
         //       svg.draw(to_polylines(fill_no_overlap), "cyan", scale_i(0.12));
         //       svg.draw(to_polylines(not_filled_exp), "green", scale_i(0.10));
         //       svg.draw(to_polylines(polyWithoutOverlap), "yellow", scale_i(0.08));
         //       //svg.draw(to_polylines(offset_ex(surface_process_result.fill_clip, ext_perimeter_spacing / 2)), "brown");
         //       svg.draw(to_polylines(top_infill_exp), "orange", scale_i(0.06));
         //       svg.draw(to_polylines(surface_process_result.fill_clip), "purple", scale_i(0.04));
         //       ArcPolylines polys;
         //       loops->collect_polylines(polys);
         //       for(auto & poly : polys)
         //           svg.draw(poly.to_polyline(), "pink", scale_i(0.02));
         //       svg.Close();
         //   }
        // append infill areas to fill_surfaces
        coord_t scaled_resolution = get_resolution(0, false);
        append(fill_surfaces, ensure_valid(std::move(infill_exp), scaled_resolution));
        append(fill_no_overlap, ensure_valid(std::move(polyWithoutOverlap), scaled_resolution));
        
#ifdef _DEBUGINFO
            LoopAssertVisitor().traverse(*loops);
#endif
    }

}

ExPolygons PerimeterGenerator::processs_no_bridge(const Parameters params, const ExPolygon& island_expolygon, ExPolygons &fill_surfaces) {
    //store surface for bridge infill to avoid unsupported perimeters (but the first one, this one is always good)
    if (params.config.no_perimeter_unsupported_algo != npuaNone
        && this->lower_slices != NULL && !this->lower_slices->empty()) {
        coordf_t bridged_infill_margin = scale_d(params.config.bridged_infill_margin.get_effective_value(unscaled(params.get_ext_perimeter_width())));

        ExPolygons last = {island_expolygon};
        BoundingBox last_box = get_extents(last);
        last_box.offset(SCALED_EPSILON);
        // get the Polygons below the polygon this layer
        Polygons lower_polygons_series_clipped = ClipperUtils::clip_clipper_polygons_with_subject_bbox(*lower_slices, last_box);
        //compute our unsupported surface
        ExPolygons unsupported = diff_ex(last, lower_polygons_series_clipped, ApplySafetyOffset::Yes);
        if (!unsupported.empty()) {
            //remove small overhangs
            ExPolygons unsupported_filtered = offset2_ex(unsupported, double(-params.get_perimeter_spacing()), double(params.get_perimeter_spacing()));
            if (!unsupported_filtered.empty()) {
                //to_draw.insert(to_draw.end(), last.begin(), last.end());
                //extract only the useful part of the lower layer. The safety offset is really needed here.
                ExPolygons support = diff_ex(last, unsupported, ApplySafetyOffset::Yes);
                if (!unsupported.empty()) {
                    //only consider the part that can be bridged (really, by the bridge algorithm)
                    //first, separate into islands (ie, each ExPlolygon)
                    int numploy = 0;
                    //only consider the bottom layer that intersect unsupported, to be sure it's only on our island.
                    //a detector per island
                    ExPolygons bridgeable;
                    for (ExPolygon unsupported : unsupported_filtered) {
                        BridgeDetector detector( unsupported,
                            support,
                            params.overhang_flow.scaled_spacing(),
                            scale_i(params.print_config.bridge_precision.get_effective_value(params.overhang_flow.spacing())),
                            params.layer->id());
                        double angle = Geometry::deg2rad(params.config.bridge_angle.value);
                        if (detector.detect_angle(params.config.bridge_angle.is_enabled() ? angle :  -1)) {
                            expolygons_append(bridgeable, union_ex(detector.coverage()));
                        }
                    }
                    if (!bridgeable.empty()) {
                        //check if we get everything or just the bridgeable area
                        if (params.config.no_perimeter_unsupported_algo.value == npuaNoPeri || params.config.no_perimeter_unsupported_algo.value == npuaFilled) {
                            //we bridge everything, even the not-bridgeable bits
                            for (size_t i = 0; i < unsupported_filtered.size();) {
                                ExPolygon& poly_unsupp = *(unsupported_filtered.begin() + i);
                                Polygons contour_simplified = poly_unsupp.contour.simplify(params.get_perimeter_spacing());
                                ExPolygon poly_unsupp_bigger = poly_unsupp;
                                Polygons contour_bigger = offset(poly_unsupp_bigger.contour, bridged_infill_margin);
                                if (contour_bigger.size() == 1) poly_unsupp_bigger.contour = contour_bigger[0];

                                //check convex, has some bridge, not overhang
                                if (contour_simplified.size() == 1 && contour_bigger.size() == 1 && contour_simplified[0].concave_points(0, PI).size() == 0
                                    && intersection_ex(bridgeable, ExPolygons{ poly_unsupp }).size() > 0
                                    && diff_ex(ExPolygons{ poly_unsupp_bigger }, union_ex(for_union(last, offset_ex(bridgeable, bridged_infill_margin + params.get_perimeter_spacing() / 2))), ApplySafetyOffset::Yes).size() == 0
                                    ) {
                                    //ok, keep it
                                    i++;
                                } else {
                                    unsupported_filtered.erase(unsupported_filtered.begin() + i);
                                }
                            }
                            unsupported_filtered = intersection_ex(last,
                                offset2_ex(unsupported_filtered, double(-params.get_perimeter_spacing() / 2), double(bridged_infill_margin + params.get_perimeter_spacing() / 2)));
                            if (params.config.no_perimeter_unsupported_algo.value == npuaFilled) {
                                for (ExPolygon& expol : unsupported_filtered) {
                                    //check if the holes won't be covered by the upper layer
                                    //TODO: if we want to do that, we must modify the geometry before making perimeters.
                                    //if (this->upper_slices != nullptr && !this->upper_slices->expolygons.empty()) {
                                    //    for (Polygon &poly : expol.holes) poly.make_counter_clockwise();
                                    //    float perimeterwidth = params.config.perimeters == 0 ? 0 : (this->ext_perimeter_flow.scaled_width() + (params.config.perimeters - 1) + this->perimeter_flow.scaled_spacing());
                                    //    std::cout << "test upper slices with perimeterwidth=" << perimeterwidth << "=>" << offset_ex(this->upper_slices->expolygons, -perimeterwidth).size();
                                    //    if (intersection(Polygons() = { expol.holes }, to_polygons(offset_ex(this->upper_slices->expolygons, -this->ext_perimeter_flow.scaled_width() / 2))).empty()) {
                                    //        std::cout << " EMPTY";
                                    //        expol.holes.clear();
                                    //    } else {
                                    //    }
                                    //    std::cout << "\n";
                                    //} else {
                                    expol.holes.clear();
                                    //}

                                    ////detect inside volume
                                    //// note: now woring on siland, it's not possible anymore.
                                    //for (size_t surface_idx_other = 0; surface_idx_other < all_surfaces.size(); surface_idx_other++) {
                                    //    if (surface_idx == surface_idx_other) continue;
                                    //    if (intersection_ex(ExPolygons() = { expol }, ExPolygons() = { all_surfaces[surface_idx_other].expolygon }).size() > 0) {
                                    //        //this means that other_surf was inside an expol holes
                                    //        //as we removed them, we need to add a new one
                                    //        ExPolygons new_poly = offset2_ex(ExPolygons{ all_surfaces[surface_idx_other].expolygon }, double(-bridged_infill_margin - params.get_perimeter_spacing()), double(params.get_perimeter_spacing()));
                                    //        if (new_poly.size() == 1) {
                                    //            all_surfaces[surface_idx_other].expolygon = new_poly[0];
                                    //            expol.holes.push_back(new_poly[0].contour);
                                    //            expol.holes.back().make_clockwise();
                                    //        } else {
                                    //            for (size_t idx = 0; idx < new_poly.size(); idx++) {
                                    //                Surface new_surf = all_surfaces[surface_idx_other];
                                    //                new_surf.expolygon = new_poly[idx];
                                    //                all_surfaces.push_back(new_surf);
                                    //                expol.holes.push_back(new_poly[idx].contour);
                                    //                expol.holes.back().make_clockwise();
                                    //            }
                                    //            all_surfaces.erase(all_surfaces.begin() + surface_idx_other);
                                    //            if (surface_idx_other < surface_idx) {
                                    //                surface_idx--;
                                    //                surface = &all_surfaces[surface_idx];
                                    //            }
                                    //            surface_idx_other--;
                                    //        }
                                    //    }
                                    //}
                                }

                            }
                            //TODO: add other polys as holes inside this one (-margin)
                        } else if (params.config.no_perimeter_unsupported_algo.value == npuaBridgesOverhangs || params.config.no_perimeter_unsupported_algo.value == npuaBridges) {
                            //simplify to avoid most of artefacts from printing lines.
                            ExPolygons bridgeable_simplified;
                            for (ExPolygon& poly : bridgeable) {
                                poly.simplify(params.get_perimeter_spacing(), bridgeable_simplified);
                            }
                            bridgeable_simplified = offset2_ex(bridgeable_simplified, -params.get_ext_perimeter_width(), params.get_ext_perimeter_width());
                            //bridgeable_simplified = intersection_ex(bridgeable_simplified, unsupported_filtered);
                            //offset by perimeter spacing because the simplify may have reduced it a bit.
                            //it's not dangerous as it will be intersected by 'unsupported' later
                            //FIXME: add overlap in this->fill_surfaces->append
                            //FIXME: it overlap inside unsuppported not-bridgeable area!

                            //bridgeable_simplified = offset2_ex(bridgeable_simplified, (double)-params.get_perimeter_spacing(), (double)params.get_perimeter_spacing() * 2);
                            //ExPolygons unbridgeable = offset_ex(diff_ex(unsupported, bridgeable_simplified), params.get_perimeter_spacing() * 3 / 2);
                            //ExPolygons unbridgeable = intersection_ex(unsupported, diff_ex(unsupported_filtered, offset_ex(bridgeable_simplified, params.get_ext_perimeter_width() / 2)));
                            //unbridgeable = offset2_ex(unbridgeable, -params.get_ext_perimeter_width(), params.get_ext_perimeter_width());


                            if (params.config.no_perimeter_unsupported_algo.value == npuaBridges) {
                                ExPolygons unbridgeable = unsupported_filtered;
                                for (ExPolygon& expol : unbridgeable)
                                    expol.holes.clear();
                                unbridgeable = diff_ex(unbridgeable, bridgeable_simplified);
                                unbridgeable = offset2_ex(unbridgeable, -params.get_ext_perimeter_width() * 2, params.get_ext_perimeter_width() * 2);
                                ExPolygons bridges_temp = offset2_ex(intersection_ex(last, diff_ex(unsupported_filtered, unbridgeable), ApplySafetyOffset::Yes), 
                                    -params.get_ext_perimeter_width() / 4, params.get_ext_perimeter_width() / 4);
                                //remove the overhangs section from the surface polygons
                                ExPolygons reference = last;
                                last = diff_ex(last, unsupported_filtered);
                                //ExPolygons no_bridge = diff_ex(offset_ex(unbridgeable, params.get_ext_perimeter_width() * 3 / 2), last);
                                //bridges_temp = diff_ex(bridges_temp, no_bridge);
                                coordf_t offset_to_do = bridged_infill_margin;
                                bool first = true;
                                unbridgeable = diff_ex(unbridgeable, offset_ex(bridges_temp, params.get_ext_perimeter_width()));
                                while (offset_to_do > params.get_ext_perimeter_width() * 1.5) {
                                    unbridgeable = offset2_ex(unbridgeable, -params.get_ext_perimeter_width() / 4, params.get_ext_perimeter_width() * 2.25, ClipperLib::jtSquare);
                                    bridges_temp = diff_ex(bridges_temp, unbridgeable);
                                    bridges_temp = offset_ex(bridges_temp, params.get_ext_perimeter_width(), ClipperLib::jtMiter, 6.);
                                    unbridgeable = diff_ex(unbridgeable, offset_ex(bridges_temp, params.get_ext_perimeter_width()));
                                    offset_to_do -= params.get_ext_perimeter_width();
                                    first = false;
                                }
                                unbridgeable = offset_ex(unbridgeable, params.get_ext_perimeter_width() + offset_to_do, ClipperLib::jtSquare);
                                bridges_temp = diff_ex(bridges_temp, unbridgeable);
                                unsupported_filtered = offset_ex(bridges_temp, offset_to_do);
                                unsupported_filtered = intersection_ex(unsupported_filtered, reference);
                            } else {
                                ExPolygons unbridgeable = intersection_ex(unsupported, diff_ex(unsupported_filtered, offset_ex(bridgeable_simplified, params.get_ext_perimeter_width() / 2)));
                                unbridgeable = offset2_ex(unbridgeable, -params.get_ext_perimeter_width(), params.get_ext_perimeter_width());
                                unsupported_filtered = unbridgeable;

                                ////put the bridge area inside the unsupported_filtered variable
                                //unsupported_filtered = intersection_ex(last,
                                //    diff_ex(
                                //    offset_ex(bridgeable_simplified, (double)params.get_perimeter_spacing() / 2),
                                //    unbridgeable
                                //    )
                                //    );
                            }
                        } else {
                            unsupported_filtered.clear();
                        }
                    } else {
                        unsupported_filtered.clear();
                    }
                }

                if (!unsupported_filtered.empty()) {

                    //add this directly to the infill list.
                    // this will avoid to throw wrong offsets into a good polygons
                    append(fill_surfaces, unsupported_filtered);

                    // store the results
                    last = diff_ex(last, unsupported_filtered, ApplySafetyOffset::Yes);
                    //remove "thin air" polygons (note: it assumes that all polygons below will be extruded)
                    for (int i = 0; i < last.size(); i++) {
                        if (intersection_ex(support, ExPolygons() = { last[i] }).empty()) {
                            fill_surfaces.push_back(last[i]);
                            last.erase(last.begin() + i);
                            i--;
                        }
                    }
                }
            }
        }
        return last;
    }
    return {island_expolygon};
}

Polygons get_contours(const ExPolygons &expolys) {
    Polygons polys;
    for (const ExPolygon &expoly : expolys) {
        assert(expoly.contour.is_counter_clockwise());
        polys.push_back(expoly.contour);
    }
    return polys;
}

Polygons as_contour(const Polygons &holes) {
    Polygons out;
    for (const Polygon &hole : holes) {
        assert(hole.is_clockwise());
        out.push_back(hole);
        out.back().make_counter_clockwise();
    }
    return out;
}

Polygons get_holes_as_contour(const ExPolygon &expoly) {
    Polygons polys;
    for(const Polygon hole : expoly.holes){
        assert(hole.is_clockwise());
        polys.push_back(hole);
        polys.back().make_counter_clockwise();
    }
    return polys;
}

Polygons get_holes_as_contour(const ExPolygons &expolys) {
    Polygons polys;
    for(const ExPolygon &expoly : expolys) 
        for(const Polygon hole : expoly.holes){
            assert(hole.is_clockwise());
            polys.push_back(hole);
            polys.back().make_counter_clockwise();
        }
    return polys;
}

// expolygon representing the perimeter path
struct ExPolygonAsynch
{
    enum ExPolygonAsynchType {
        epatGrowHole,
        epatShrinkContour
    };
    ExPolygonAsynchType type;
    ExPolygon expoly;
    // shrink the contour by this value to get the end of the spacing (should be negative, to shrink from centerline or edge)
    coordf_t  offset_contour_inner;
    // shrink the contour by this value to get the external shell (the spacing position) (can be negative to grow from centreline, and be positive to shrink from surface polygon)
    coordf_t  offset_contour_outer;
    // grow the holes by this value to get the end of the spacing (should be negative, to grow from centerline or edge)
    coordf_t  offset_holes_inner;
    // grow the holes by this value to get the external shell (the spacing position) (should be the same value as offset_contour_outer)
    coordf_t  offset_holes_outer;
};

void assert_check_ExPolygonAsynch(const std::vector<ExPolygonAsynch> &polygons_asynchs) {
#if _DEBUG
    for(const auto &polygon_asynch : polygons_asynchs)
        assert_check_polygons(to_polygons(polygon_asynch.expoly));
#endif
}

// next_onion can be partially filled
void grow_holes_only(std::vector<ExPolygonAsynch> &unmoveable_contours,
                     ExPolygons &                      next_onion,
                     coordf_t                          spacing,
                     coordf_t                          overlap_spacing,
                     bool                              round_peri,
                     float                             min_round_spacing = 3.f)
{
    assert(spacing > 0);
    assert(overlap_spacing >= 0);
    Polygons new_contours;
    for (size_t idx_unmoveable = 0; idx_unmoveable < unmoveable_contours.size(); ++idx_unmoveable) {
        ExPolygonAsynch & unmoveable_contour = unmoveable_contours[idx_unmoveable];
        assert(unmoveable_contour.type == ExPolygonAsynch::ExPolygonAsynchType::epatGrowHole);
        ExPolygon &expoly = unmoveable_contour.expoly;
        assert(unmoveable_contour.offset_holes_inner <= 0);
        // grow fake contours, can now have fake holes and/or less fake contours.
        Polygons ok_holes = offset(get_holes_as_contour(expoly),
                                   -unmoveable_contour.offset_holes_inner + spacing / 2 + overlap_spacing,
                                        (round_peri ? ClipperLib::JoinType::jtRound :
                                                        ClipperLib::JoinType::jtMiter),
                                        (round_peri ? min_round_spacing : 3));
        for (size_t i = 0; i < ok_holes.size(); ++i) {
            if (ok_holes[i].is_clockwise()) {
                // hole, it's a new peri, move it.
                new_contours.push_back(std::move(ok_holes[i]));
                ok_holes.erase(ok_holes.begin() + i);
                new_contours.back().make_counter_clockwise();
                i--;
            }
        }
        ok_holes = union_(ok_holes);
        for (const Polygon &p : ok_holes) assert(p.is_counter_clockwise());
        //shrink contour, can now be multiple contour.
        coordf_t computed_offset = unmoveable_contour.offset_contour_inner;
        computed_offset -= spacing / 2;
        computed_offset -= overlap_spacing;
        Polygons ex_contour_offset = offset(Polygons{expoly.contour}, computed_offset);
        bool ex_contour_offset_now_fake_hole = false;
        for (size_t idx_hole = 0; idx_hole < ok_holes.size(); ++idx_hole) {
            const Polygon &hole = ok_holes[idx_hole];
            assert(hole.is_counter_clockwise());
            // Check if it can fuse with contour
            // TODO: bounding box for quicker cut search
            auto it_contour_candidate_for_fuse = ex_contour_offset.begin();
            Polygons fused_contour;
            while (it_contour_candidate_for_fuse != ex_contour_offset.end()) {
                ExPolygons result = diff_ex(Polygons{*it_contour_candidate_for_fuse}, Polygons{hole});
                // Only two options here, it can fuse and then there is 1 or more contour, no holes.
                // Or it don't touch the contour and so nothing happen. (the hole can be inside or outside)
                // SO, we can check it it slip or if the contour has been modified
                if (result.size() > 1 || (result.size() == 1 && result.front().contour != *it_contour_candidate_for_fuse)) {
                    for (ExPolygon &expoly : result) assert(expoly.holes.empty());
                    // now use this one.
                    append(fused_contour, to_polygons(result));
                    ex_contour_offset_now_fake_hole = true;
                    // remove from useful holes
                    ok_holes.erase(ok_holes.begin() + idx_hole);
                    idx_hole--;
                    it_contour_candidate_for_fuse = ex_contour_offset.erase(it_contour_candidate_for_fuse);
                    break; // stop iterating the while, we already fused the hole
                } else {
                    ++it_contour_candidate_for_fuse;
                }
            }
            if(!fused_contour.empty())
                append(ex_contour_offset, std::move(fused_contour));
        }
        // if moved from unmoveable_contours to growing_contours, then move the expoly
        if (ex_contour_offset_now_fake_hole) {
            // add useful holes to the contours, and push them
            if (overlap_spacing != 0)
                append(next_onion, offset_ex(diff_ex(ex_contour_offset, ok_holes), overlap_spacing));
            else
                append(next_onion, diff_ex(ex_contour_offset, ok_holes));
            // remove from unmoveable
            unmoveable_contours.erase(unmoveable_contours.begin() + idx_unmoveable);
            idx_unmoveable--;
        } else {
            // update holes
            // shrink to centerline
            if (overlap_spacing != 0)
                ok_holes = offset(ok_holes, -overlap_spacing);
            /*test*/ for (const Polygon &p : ok_holes) assert(p.is_counter_clockwise());
            ExPolygons new_unmoveable = diff_ex(Polygons{unmoveable_contour.expoly.contour}, ok_holes);
            // check if size is good. It's not possible to split the peri: it isn't srhunk, and holes intersect are alreedy detected (not unmoveable anymore)
            assert(new_unmoveable.size() <= 1);
            if (new_unmoveable.empty()) {
                // remove from unmoveable
                unmoveable_contours.erase(unmoveable_contours.begin() + idx_unmoveable);
                idx_unmoveable--;
            } else if(new_unmoveable.size() == 1){
                // update
                unmoveable_contour.expoly = new_unmoveable.front();
                unmoveable_contour.offset_holes_inner = -spacing / 2;
                unmoveable_contour.offset_holes_outer = spacing / 2;
            } else {
                assert(false);
                //add all
                for(ExPolygon & new_expoly : new_unmoveable)
                    unmoveable_contours.push_back({unmoveable_contour.type, new_expoly, unmoveable_contour.offset_contour_inner,
                                                   unmoveable_contour.offset_contour_outer, -spacing / 2,
                                                   spacing / 2});
                // remove from unmoveable
                unmoveable_contours.erase(unmoveable_contours.begin() + idx_unmoveable);
                idx_unmoveable--;
            }
        }
    }
}


// next_onion can be partially filled
void grow_contour_only(std::vector<ExPolygonAsynch> &unmoveable_holes, coordf_t spacing, coordf_t overlap_spacing, bool round_peri, float min_round_spacing = 3.f) {
    assert(spacing > 0);
    assert(overlap_spacing >= 0);
    Polygons new_contours;
    // mutable size to allow insert at the same time.
    size_t unmoveable_holes_size = unmoveable_holes.size();
    for (size_t idx_unmoveable = 0; idx_unmoveable < unmoveable_holes_size; ++idx_unmoveable) {
        ExPolygonAsynch & unmoveable_hole = unmoveable_holes[idx_unmoveable];
        assert(unmoveable_hole.type == ExPolygonAsynch::ExPolygonAsynchType::epatShrinkContour);
        ExPolygon &expoly = unmoveable_hole.expoly;
        // shrink contour, can now have more contours.
        assert(unmoveable_hole.offset_contour_inner <=0);
        Polygons ok_contours = offset(expoly.contour, unmoveable_hole.offset_contour_inner - spacing/2 - overlap_spacing,
                                        (round_peri ? ClipperLib::JoinType::jtRound :
                                                        ClipperLib::JoinType::jtMiter),
                                        (round_peri ? min_round_spacing : 3));
        //we shrunk -> new peri can appear, holes can disapear, but there is already none.
        if (ok_contours.empty()) {
            // can't grow.
            unmoveable_holes.erase(unmoveable_holes.begin() + idx_unmoveable);
            idx_unmoveable--;
            unmoveable_holes_size--;
            continue;
        }
        for (const Polygon &p : ok_contours) assert(p.is_counter_clockwise());
        //grow holes to right size
        assert(-unmoveable_hole.offset_holes_inner + spacing/2 - overlap_spacing > 0);
        Polygons original_holes = get_holes_as_contour(expoly);
        Polygons offsetted_holes = offset(original_holes, -unmoveable_hole.offset_holes_inner + spacing/2 + overlap_spacing);
        // remove fake perimeter, i don't want them.
        for (size_t i = 0; i < offsetted_holes.size(); ++i) {
            if (offsetted_holes[i].is_clockwise()) {
                offsetted_holes.erase(offsetted_holes.begin() + i);
                new_contours.back().make_counter_clockwise();
                i--;
            }
        }
        offsetted_holes = union_(offsetted_holes);
        for (const Polygon &p : offsetted_holes) assert(p.is_counter_clockwise());

        assert(!ok_contours.empty());
        auto my_type = unmoveable_hole.type;
        auto my_offset_holes_inner = unmoveable_hole.offset_holes_inner;
        auto my_offset_holes_outer = unmoveable_hole.offset_holes_outer;
        {
            Polygon &simple_contour = ok_contours[0];
            // remove holes
            ExPolygons test_expoly = diff_ex(Polygons{simple_contour}, offsetted_holes);
            if (overlap_spacing != 0) {
                test_expoly = offset_ex(test_expoly, overlap_spacing);
            }
            if (test_expoly.size() == 1) {
                // no merge, then i can use the right hole size
                ExPolygons new_unmoveable_hole = diff_ex(Polygons{test_expoly[0].contour}, original_holes);
                // diff with smaller holes, so it has to be only one contour.
                assert(new_unmoveable_hole.size() == 1);
                expoly                               = new_unmoveable_hole[0];
                unmoveable_hole.offset_contour_inner = -spacing / 2;
                unmoveable_hole.offset_contour_outer = spacing / 2;
            } else {
                // a hole cut it, or clear it.
                for (ExPolygon &new_expoly : test_expoly) {
                    ExPolygons new_unmoveable_holes = diff_ex(Polygons{new_expoly.contour}, original_holes);
                    for (ExPolygon &new_unmoveable_hole : new_unmoveable_holes)
                        unmoveable_holes.push_back({my_type, new_unmoveable_hole, -spacing / 2,
                                                    spacing / 2, my_offset_holes_inner,
                                                    my_offset_holes_outer});
                }
                unmoveable_holes.erase(unmoveable_holes.begin() + idx_unmoveable);
                idx_unmoveable--;
                unmoveable_holes_size--;
            }
        }
        // expoly and unmoveable_hole are now invalidated.
        // add the others
        for (size_t idx_contour = 1; idx_contour < ok_contours.size(); idx_contour++) {
            Polygon &simple_contour = ok_contours[idx_contour];
            // remove holes
            ExPolygons test_expoly = diff_ex(Polygons{simple_contour}, offsetted_holes);
            if (overlap_spacing != 0) {
                test_expoly = offset_ex(test_expoly, overlap_spacing);
            }
            if (test_expoly.size() == 1) {
                // no merge, then i can use the right hole size
                ExPolygons new_unmoveable_hole = diff_ex(Polygons{test_expoly[0].contour}, original_holes);
                // diff with smaller holes, so it has to be only one contour.
                assert(new_unmoveable_hole.size() == 1);
                unmoveable_holes.push_back({my_type, new_unmoveable_hole[0], -spacing / 2, spacing / 2,
                                            my_offset_holes_inner, my_offset_holes_outer});
            } else {
                // a hole cut it, or clear it.
                for (ExPolygon &new_expoly : test_expoly) {
                    ExPolygons new_unmoveable_holes = diff_ex(Polygons{new_expoly.contour}, original_holes);
                    for (ExPolygon &new_unmoveable_hole : new_unmoveable_holes)
                        unmoveable_holes.push_back({my_type, new_unmoveable_hole, -spacing / 2,
                                                    spacing / 2, my_offset_holes_inner, my_offset_holes_outer});
                }
            }
        }
        //we shrink perimeter, so it doesn't create holes, so we don't have anythign to add to next_onion.
    }
}

void move_overhangs(const Parameters &params, ExPolygons &last, ExPolygons &last_overhang, coord_t overhang_spacing, coord_t perimeter_spacing, bool need_clip = true) {
    // overhang_extrusion_width
    if (overhang_spacing > 0 && !last_overhang.empty() &&
        !is_approx(overhang_spacing, perimeter_spacing, SCALED_EPSILON)) {
        //first, clip the overhang area
        last_overhang = intersection_ex(last_overhang, last);
        // then do it (only if smaller, if bigger it creates issues with gapfill)
        if (overhang_spacing < perimeter_spacing) {
            // expand the overhang areas
            coord_t expand_value = perimeter_spacing - overhang_spacing;
            ExPolygons temp = offset_ex(last_overhang, double(-expand_value));
            last = union_ex(last, offset_ex(temp, double(expand_value * 2), ClipperLib::JoinType::jtSquare, 0));
        }
    }
}

ProcessSurfaceResult PerimeterGenerator::process_classic(const Parameters &         params,
                                                         int &                      contour_count,
                                                         int &                      holes_count,
                                                         const ExPolygon &          surface_expolygon,
                                                         ExtrusionEntityCollection &loops,
                                                         ExtrusionEntityCollection &gap_fill)
{
    ProcessSurfaceResult results;
    //this var store infill surface removed from last to not add any more perimeters to it.
    // simplification already done at slicing
    //simplify the loop to avoid artifacts when shrinking almost-0 segments
    coord_t resolution = get_resolution(0, false);
    ExPolygons last    = union_ex(surface_expolygon.simplify_p(std::max(SCALED_EPSILON, resolution)));
    ExPolygons last_overhang = params.overhang_areas;
    ExPolygons gaps;
    // to store gap area that is transformed into a perimeter to be able to remove it from infill area.
    ExPolygons perimeter_gaps_ex;
    double last_area   = -1;

    // list of Expolygons where contour or holes aren't growing.
    std::vector<ExPolygonAsynch> last_asynch;
    bool last_asynch_initialized = false;

    // infill area saved while continuing to add perimeter elsewhere.
    ExPolygons saved_infill;

    if (contour_count > 0 || holes_count > 0) {

        //increase surface for milling_post-process
        if (this->mill_extra_size > SCALED_EPSILON) {
            if (unmillable.empty())
                last = offset_ex(last, mill_extra_size);
            else {
                //FIXME only work if mill_extra_size < mill_nozzle/2 (becasue it's the extra offset from unmillable)
                //FIXME overhangs if mill_extra_size is too big
                //FIXME merge with process_arachne?
                ExPolygons growth = diff_ex(offset_ex(last, mill_extra_size), unmillable, ApplySafetyOffset::Yes);
                last.insert(last.end(), growth.begin(), growth.end());
                last = union_ex(last);
            }
        }

        this->throw_if_canceled();
        // Add perimeters on overhangs : initialization
        ExPolygons overhangs_unsupported;
        if ((/*params.config.extra_perimeters_on_overhangs || */(params.config.overhangs_reverse && params.layer->id() % 2 == 1))
            && !last.empty() && this->lower_slices != NULL && !this->lower_slices->empty()) {
            //remove holes from lower layer, we only ant that for overhangs, not bridges!
            ExPolygons lower_without_holes;
            for (const ExPolygon &exp : *this->lower_slices) {
                lower_without_holes.emplace_back(to_expolygon(exp.contour));
            }
            // opening is offset2-+
            overhangs_unsupported = opening_ex(diff_ex(last, lower_without_holes, ApplySafetyOffset::Yes), scale_i(params.print_config.resolution_internal));
            if (!overhangs_unsupported.empty()) {
                //only consider overhangs and let bridges alone
                //only consider the part that can be bridged (really, by the bridge algorithm)
                //first, separate into islands (ie, each ExPlolygon)
                //only consider the bottom layer that intersect unsupported, to be sure it's only on our island.
                const ExPolygons lower_island(diff_ex(last, overhangs_unsupported));
                ExPolygons bridgeable;
                for (ExPolygon unsupported : overhangs_unsupported) {
                    BridgeDetector detector( unsupported,
                        lower_island,
                        params.overhang_flow.scaled_spacing(),
                        scale_i(params.print_config.bridge_precision.get_effective_value(params.overhang_flow.spacing())),
                        params.layer->id());
                    double angle = Geometry::deg2rad(params.config.bridge_angle.value);
                    if (detector.detect_angle(params.config.bridge_angle.is_enabled() ? angle : -1))
                        expolygons_append(bridgeable, union_ex(detector.coverage()));
                }
                if (!bridgeable.empty()) {
                    //simplify to avoid most of artefacts from printing lines.
                    ExPolygons bridgeable_simplified;
                    for (const ExPolygon& poly : bridgeable) {
                        poly.simplify(params.get_perimeter_spacing() / 2, bridgeable_simplified);
                    }

                    //offset by perimeter spacing because the simplify may have reduced it a bit.
                    if (!bridgeable_simplified.empty()) {
                        bridgeable_simplified = offset_ex(bridgeable_simplified, double(params.get_perimeter_spacing()));
                        overhangs_unsupported = diff_ex(overhangs_unsupported, bridgeable_simplified, ApplySafetyOffset::Yes);
                    }
                }
            }
        }
        bool has_steep_overhang = false;
        if (params.layer->id() % 2 == 1 && params.config.overhangs_reverse //check if my option is set and good layer
            && !last.empty() && this->lower_slices != NULL && !this->lower_slices->empty() //has something to work with 
            ) {
            ExPolygons overhangs = diff_ex(last, *lower_slices);
            coord_t offset = scale_i(params.config.overhangs_reverse_threshold.get_effective_value(unscaled(params.get_perimeter_width())));
            //version with: scale_(std::tan(PI * (0.5f / 90) * params.config.overhangs_reverse_threshold.value ) * params.layer->unscaled_height())

            if (offset_ex(overhangs, -offset / 2.).size() > 0) {
                //allow this loop to be printed in reverse
                has_steep_overhang = true;
            }
        }

        // to only add one extra perimter;
        bool already_have_extra_odd_perimeter = false;
        int extra_perimeters_count_printed = 0;
        int extra_perimeters_surface_printed = 0;

        // In case no perimeters are to be generated, contour_count / holes_count will equal to 0.            
        std::vector<PerimeterGeneratorLoops> contours(contour_count);    // depth => loops
        std::vector<PerimeterGeneratorLoops> holes(holes_count);       // depth => loops
        ThickPolylines thin_walls_thickpolys;

        // we loop one time more than needed in order to find gaps after the last perimeter was applied
        for (int perimeter_idx = 0;; ++perimeter_idx) {  // outer loop is 0
            this->throw_if_canceled();

            // We can add more perimeters if there are uncovered overhangs
            // improvement for future: find a way to add perimeters only where it's needed.
            bool has_overhang = false;
            // if (params.config.extra_perimeters_on_overhangs && !last.empty() && !overhangs_unsupported.empty()) {
                // overhangs_unsupported = intersection_ex(overhangs_unsupported, last, ApplySafetyOffset::Yes);
                // if (overhangs_unsupported.size() > 0) {
                    // //please don't stop adding perimeter yet.
                    // has_overhang = true;
                // }
            // }

            // allow this perimeter to overlap itself?
            float thin_perimeter = params.config.thin_perimeters.get_effective_value(1);
            if (perimeter_idx > 0 && thin_perimeter != 0) {
                thin_perimeter = params.config.thin_perimeters_all.get_effective_value(1);
            }
            bool allow_perimeter_anti_hysteresis = thin_perimeter >= 0;
            if (thin_perimeter < 0) {
                thin_perimeter = -thin_perimeter;
            }
            if (thin_perimeter < 0.02) { // can create artifacts
                thin_perimeter = 0;
            }

            // Calculate next onion shell of perimeters.
            // this variable stored the next onion
            ExPolygons next_onion;
            // like next_onion, but with all polygons, even ones that didn't grow and so won't be added as perimeter
            ExPolygons area_used;
            ExPolygons* all_next_onion = &next_onion;

            const coord_t previous_spacing = (perimeter_idx == 0) ? 0 :
                (perimeter_idx == 1)                              ? params.get_ext_perimeter_spacing2() :
                                                                    params.get_perimeter_spacing();

            // add extra periemters
            if (perimeter_idx >= std::max(contour_count, holes_count)) {
                ExPolygons extra_perimeter_next_onion;
                int need_union = 0;
                bool skip_extra_peri = false;

                // extra_perimeters_count (add extra perimeters on regions)
                if (params.region_setting.has_many_config(&params.config.extra_perimeters_count)) {
                    bool add_extra = false;
                    for (auto const &[extra_perimeters_count, areas] :
                         params.region_setting.get_areas(&params.config.extra_perimeters_count)) {
                        if (extra_perimeters_count.get_int() > extra_perimeters_count_printed) {
                            append(extra_perimeter_next_onion, areas.intersections(previous_spacing/2, last));
                            add_extra = true;
                        }
                    }
                    if (add_extra) {
                        extra_perimeter_next_onion = union_ex(extra_perimeter_next_onion);
                        extra_perimeters_count_printed++;
                        need_union++;
                        // add odd/even after these ones.
                        skip_extra_peri = true;
                    }
                }

                // extra_perimeters from surfaces
                if (params.region_setting.has_many_config(&Parameters::OPT_EXTRA_PERIMETER)) {
                    bool add_extra = false;
                    for (auto const &[extra_perimeters, areas] : params.region_setting.get_areas(&Parameters::OPT_EXTRA_PERIMETER)) {
                        if (extra_perimeters.get_int() > extra_perimeters_surface_printed) {
                            append(extra_perimeter_next_onion, areas.intersections(previous_spacing/2, last));
                            add_extra = true;
                        }
                    }
                    if (add_extra) {
                        extra_perimeter_next_onion = union_ex(extra_perimeter_next_onion);
                        extra_perimeters_surface_printed++;
                        need_union++;
                        // add odd/even after these ones.
                        skip_extra_peri = true;
                    }
                }

                // extra_perimeters_odd_layers
                if (perimeter_idx > 0 && !skip_extra_peri &&
                    params.layer->id() % 2 == 1 && !already_have_extra_odd_perimeter &&
                    params.region_setting.has_many_config(&params.config.extra_perimeters_odd_layers)) {
                    for (auto const &[is_extra_perimeters_odd_layers, areas] :
                         params.region_setting.get_areas(&params.config.extra_perimeters_odd_layers)) {
                        if (is_extra_perimeters_odd_layers.get_bool()) {
                            append(extra_perimeter_next_onion, areas.intersections(previous_spacing/2, last));
                        }
                    }
                    already_have_extra_odd_perimeter = true;
                    need_union++;
                }

                // extra_perimeters_below_area
                if (perimeter_idx > 0 && !skip_extra_peri &&
                    (params.region_setting.has_many_config(&params.config.extra_perimeters_below_area) ||
                     params.region_setting.get_solo_config(&params.config.extra_perimeters_below_area).get_float() > 0)) {
                    ExPolygons small_expolygons;
                    for (auto const &[extra_perimeters_below_area, areas] :
                         params.region_setting.get_areas(&params.config.extra_perimeters_below_area)) {
                        if (extra_perimeters_below_area.get_float() > 0) {
                            double area_mm2 = extra_perimeters_below_area.is_percent() ?
                                sqr(extra_perimeters_below_area.get_effective_value(
                                                                          (coordf_t) params.get_perimeter_width())) :
                                scale_d(scale_d(extra_perimeters_below_area.get_float()));
                            // gather small areas
                            for (ExPolygon &next : areas.intersections(previous_spacing/2, last)) {
                                if (next.area() < area_mm2) {
                                    extra_perimeter_next_onion.push_back(std::move(next));
                                }
                            }
                        }
                    }
                    need_union++;
                }
                // if nothing left, stop it.
                if (need_union > 0 && !extra_perimeter_next_onion.empty()) {
                    // split between fill & peri
                    if (need_union > 1) {
                        extra_perimeter_next_onion = union_ex(extra_perimeter_next_onion);
                    }
                    // get infill areas
                    ExPolygons infill_areas = diff_ex(last, extra_perimeter_next_onion);
                    // expand to be able to touch the next perimeters
                    infill_areas = offset_ex(infill_areas, previous_spacing / 2);
                    // stay in last boundaries
                    infill_areas = intersection_ex(infill_areas, offset_ex(last, -previous_spacing / 2));
                    append(saved_infill, std::move(infill_areas));

                    last = std::move(extra_perimeter_next_onion);
                    contour_count++;
                    holes_count++;
                    contours.emplace_back();
                    holes.emplace_back();
                }
            }
            ExPolygons next_overhang;
            if (perimeter_idx == 0) {
                // do overhangs_extrusion_width
                if (params.get_overhang_spacing() > 0 ) {
                    move_overhangs(params, last, last_overhang, params.overhang_flow.scaled_width()/2, params.get_ext_perimeter_width()/2, false);
                    next_overhang = last_overhang;
                }
                // compute next onion
                    // the minimum thickness of a single loop is:
                    // ext_width/2 + ext_spacing/2 + spacing/2 + width/2
                coordf_t good_spacing    = params.get_ext_perimeter_width() / 2;
                coordf_t overlap_spacing = (1 - thin_perimeter) * params.get_ext_perimeter_spacing() / 2;
                if (holes_count == 0 || contour_count == 0) {
                    allow_perimeter_anti_hysteresis = false; // don't erase that complicated next_onion computation
                    if (holes_count == 0) {
                        for (ExPolygon &expoly : last) { 
                            last_asynch.push_back(ExPolygonAsynch{ExPolygonAsynch::ExPolygonAsynchType::epatShrinkContour, expoly,
                                // inner_offset                                                 outer_offset (go spacing limit)
                                -coordf_t(params.get_perimeter_width() - params.get_perimeter_spacing())/2, -coordf_t(params.get_perimeter_width() - params.get_perimeter_spacing())/2,
                                -coordf_t(params.get_perimeter_width() - params.get_perimeter_spacing())/2, -coordf_t(params.get_perimeter_width() - params.get_perimeter_spacing())/2});
                        }
                        last_asynch_initialized = true;
                        grow_contour_only(last_asynch, params.get_perimeter_spacing(), 0 /*no overlap for external*/, false /*no round peri for external*/);
                    } else {
                        for (ExPolygon &expoly : last) { 
                            last_asynch.push_back(ExPolygonAsynch{ExPolygonAsynch::ExPolygonAsynchType::epatGrowHole, expoly,
                                // inner_offset                                                 outer_offset (go spacing limit)
                                -coordf_t(params.get_perimeter_width() - params.get_perimeter_spacing())/2, -coordf_t(params.get_perimeter_width() - params.get_perimeter_spacing())/2,
                                -coordf_t(params.get_perimeter_width() - params.get_perimeter_spacing())/2, -coordf_t(params.get_perimeter_width() - params.get_perimeter_spacing())/2});
                        }
                        last_asynch_initialized = true;
                        grow_holes_only(last_asynch, next_onion, params.get_perimeter_spacing(), 0 /*no overlap for external*/, false /*no round peri for external*/);
                    }
                } else {
                    if (thin_perimeter > 0.98) {
                        next_onion = offset_ex(last, -(float) (params.get_ext_perimeter_width() / 2),
                                               ClipperLib::JoinType::jtMiter, 3);
                    } else {
                        coordf_t good_spacing    = params.get_ext_perimeter_width() / 2;
                        coordf_t overlap_spacing = (1.f - thin_perimeter) * params.get_ext_perimeter_spacing() / 2;
                        next_onion               = offset2_ex(last, -(float) (good_spacing + overlap_spacing - 1),
                                                +(float) (overlap_spacing - 1), ClipperLib::JoinType::jtMiter, 3);
                    }
                    if (thin_perimeter < 0.7) {
                        // offset2_ex can create artifacts, if too big. see superslicer#2428
                        next_onion = intersection_ex(next_onion, offset_ex(last, -(float) (params.get_ext_perimeter_width() / 2),
                                                                           ClipperLib::JoinType::jtMiter, 3));
                    }
                }
                
                bool special_area = contour_count == 0 || holes_count == 0;
                if (special_area &&
                    (params.region_setting.has_many_config(&params.config.thin_walls) ||
                     params.region_setting.get_solo_config(&params.config.thin_walls).get_bool() ||
                     params.spiral_vase)) {
                    area_used = next_onion;
                    for(auto& expolycontainer : last_asynch)
                        area_used.push_back(expolycontainer.expoly);
                    all_next_onion = &area_used;
                }

                // un-hysteresis for thin walls
                if (params.region_setting.has_many_config(&params.config.thin_walls) ||
                    params.region_setting.get_solo_config(&params.config.thin_walls).get_bool()) {
                    // detect edge case where a curve can be split in multiple small chunks.
                    if (allow_perimeter_anti_hysteresis && !special_area &&
                        next_onion.size() > last.size()) {
                        // don't go too far, it's not possible to print thin wall after that
                        std::vector<float> variations = {-.025f, .025f, -.05f, .05f, -.075f, .1f, .15f};
                        const coordf_t good_spacing = params.get_ext_perimeter_width() / 2;
                        const coordf_t overlap_spacing = (1 - thin_perimeter) * params.get_ext_perimeter_spacing() /
                            2;
                        for (size_t idx_variations = 0;
                             next_onion.size() > last.size() && idx_variations < variations.size();
                             idx_variations++) {
                            const coordf_t spacing_change = params.get_ext_perimeter_spacing() *
                                variations[idx_variations];
                            // don't go over 100% overlap
                            if (overlap_spacing + spacing_change < 1) {
                                continue;
                            }
                            // use a sightly bigger spacing to try to drastically improve the split, that can lead to
                            // very thick gapfill
                            ExPolygons next_onion_secondTry = offset2_ex(last,
                                                                         -(float) (good_spacing + overlap_spacing +
                                                                                   spacing_change - 1),
                                                                         +(float) (overlap_spacing + spacing_change) -
                                                                             1);
                            if (next_onion.size() > next_onion_secondTry.size() * 1.2 &&
                                next_onion.size() > next_onion_secondTry.size() + 2) {
                                next_onion = next_onion_secondTry;
                            }
                        }
                    }
                }

                // look for thin walls
                for (auto const &[thin_walls_config, areas] :
                     params.region_setting.get_areas(&params.config.thin_walls)) {
                  if (thin_walls_config.get_bool()) {
                    ExPolygons last_good_areas = areas.intersections(last);

                    // the following offset2 ensures almost nothing in @thin_walls is narrower than $min_width
                    // (actually, something larger than that still may exist due to mitering or other causes)
                    //coord_t min_width = scale_i(params.config.thin_walls_min_width.get_effective_value(params.ext_perimeter_flow.nozzle_diameter()));
                    coord_t min_width = scale_i(thin_walls_config.get_effective_value(params.ext_perimeter_flow.nozzle_diameter(), &params.config.thin_walls_min_width));

                    ExPolygons no_thin_zone = offset_ex(*all_next_onion, double(params.get_ext_perimeter_width() / 2), jtSquare);
                    // medial axis requires non-overlapping geometry
                    const ExPolygons thin_zones = diff_ex(last_good_areas, no_thin_zone, ApplySafetyOffset::Yes);
                    //don't use offset2_ex, because we don't want to merge the zones that have been separated.
                        //a very little bit of overlap can be created here with other thin polygons, but it's more useful than worisome.
                    const ExPolygons half_thins = remove_point_too_close(offset_ex(thin_zones, double(-min_width / 2)),
                                                                   params.get_ext_perimeter_width() / 20);
                    //we push the bits removed and put them into what we will use as our anchor
                    if (half_thins.size() > 0) {
                        no_thin_zone = diff_ex(last_good_areas, offset_ex(half_thins, double(min_width / 2 - SCALED_EPSILON)), ApplySafetyOffset::Yes);
                        no_thin_zone = offset2_ex(no_thin_zone, -params.get_ext_perimeter_width() / 20, params.get_ext_perimeter_width() / 20);
                        remove_point_too_close(no_thin_zone);
                    }
                    ExPolygons thins;
                    // compute a bit of overlap to anchor thin walls inside the print.
                    for (const ExPolygon &half_thin : half_thins) {
                        // growing back the polygon
                        const ExPolygons thin = remove_point_too_close(offset_ex(half_thin, double(min_width / 2)),
                                                                       params.get_ext_perimeter_width() / 10);
                        assert(thin.size() <= 1);
                        if (thin.empty() || thin.front().empty() ||
                            thin[0].area() <=
                                min_width * (params.get_ext_perimeter_width() + params.get_ext_perimeter_spacing())) {
                            continue;
                        }
                        thins.push_back(thin[0]);
                        //const coord_t thin_walls_overlap = scale_i(params.config.thin_walls_overlap.get_effective_value(params.ext_perimeter_flow.nozzle_diameter()));
                        const coord_t thin_walls_overlap = scale_i(thin_walls_config.get_effective_value(params.ext_perimeter_flow.nozzle_diameter(), &params.config.thin_walls_overlap));
                        const ExPolygons full_thin_with_overlap = offset_ex(half_thin,
                                                                            double(min_width / 2) + (float) (thin_walls_overlap),
                                                                            jtSquare);
                        // clip no_thin_zone with bounding box from full_thin_with_overlap, as no_thin_zone can be huge.
                        BoundingBox bbox_full_thin_with_overlap(contours_to_points(full_thin_with_overlap));
                        bbox_full_thin_with_overlap.offset(SCALED_EPSILON * 10);
                        const ExPolygons no_thin_zone_simplified =
                            ClipperUtils::clip_clipper_expolygons_with_subject_bbox(no_thin_zone, bbox_full_thin_with_overlap);
                        const ExPolygons anchor = intersection_ex(full_thin_with_overlap, no_thin_zone_simplified, ApplySafetyOffset::Yes);
                        ExPolygons bounds = union_ex(thin, anchor, ApplySafetyOffset::Yes);
                        for (ExPolygon &bound : bounds) {
                            if (!intersection_ex(thin[0], bound).empty()) {
                                bound.remove_point_too_close(params.get_ext_perimeter_width() / 10);
                                if (bound.empty()) {
                                    continue;
                                }
                                // the maximum thickness of our thin wall area is equal to the minimum thickness
                                // of a single loop (*1.2 because of circles approx. and enlrgment from 'div')
                                Slic3r::Geometry::MedialAxis ma{thin[0],
                                                                (coord_t) ((params.get_ext_perimeter_width() + params.get_ext_perimeter_spacing()) * 1.2),
                                                                min_width,
                                                                scale_i(params.layer->unscaled_height())};
                                size_t next_thin_wall_idx = thin_walls_thickpolys.size();
                                ma.use_bounds(bound)
                                    .use_min_real_width(scale_i(params.ext_perimeter_flow.nozzle_diameter()))
                                    .use_tapers(thin_walls_overlap)
                                    .set_min_length(params.get_ext_perimeter_width() + params.get_ext_perimeter_spacing())
                                    .build(thin_walls_thickpolys);
                                // store thin walls bound in boundary
                                if (next_thin_wall_idx < thin_walls_thickpolys.size()) {
                                    perimeter_boundary.push_back(bound);
                                }
                            }
                            break;
                        }
                    }
                    // use perimeters to extrude area that can't be printed by thin walls
                    // it's a bit like re-add thin area into perimeter area.
                    // it can over-extrude a bit, but it's for a better good.
                    if (!special_area) {
                        if (thin_perimeter > 0.98) {
                            next_onion = union_ex(next_onion, offset_ex(diff_ex(last, thins, ApplySafetyOffset::Yes),
                                                                        -(float) (params.get_ext_perimeter_width() / 2),
                                                                        ClipperLib::JoinType::jtMiter, 3));
                        } else if (thin_perimeter > 0.01) {
                            next_onion = union_ex(next_onion,
                                                  offset2_ex(diff_ex(last, thins, ApplySafetyOffset::Yes),
                                                             -(float) ((params.get_ext_perimeter_width() / 2) +
                                                                       ((1 - thin_perimeter) * params.get_ext_perimeter_spacing() / 4)),
                                                             (float) ((1 - thin_perimeter) * params.get_ext_perimeter_spacing() / 4),
                                                             ClipperLib::JoinType::jtMiter, 3));
                        } else {
                            next_onion = union_ex(next_onion, offset2_ex(diff_ex(last, thins, ApplySafetyOffset::Yes),
                                                                         -(float) ((params.get_ext_perimeter_width() / 2) +
                                                                                   (params.get_ext_perimeter_spacing() / 4)),
                                                                         (float) (params.get_ext_perimeter_spacing() / 4),
                                                                         ClipperLib::JoinType::jtMiter, 3));
                        }
                        //simplify the loop to avoid almost-0 segments
                        resolution = get_resolution(1, false);
                        expolygons_simplify(next_onion, std::max(SCALED_EPSILON, resolution));
                        //mask
                        next_onion = intersection_ex(next_onion, last);
                    }
                  }
                }
                if (params.spiral_vase && all_next_onion->size() > 1) {
                    assert(contour_count > 0);
                    // Remove all but the largest area polygon.
                    keep_largest_contour_only(*all_next_onion);
                }
                //compute boundary
                if (perimeter_boundary.empty()) {
                    perimeter_boundary = offset_ex(next_onion, params.get_ext_perimeter_width() / 2);
                } else {
                    append(perimeter_boundary, offset_ex(next_onion, params.get_ext_perimeter_width() / 2));
                    perimeter_boundary = union_ex(perimeter_boundary);
                }
            } else {
                next_overhang = last_overhang;
                move_overhangs(params, last, next_overhang, params.get_overhang_spacing() / 2,
                                ((perimeter_idx == 1) ? params.get_ext_perimeter_spacing() : params.get_perimeter_spacing()) / 2, true);
                //FIXME Is this offset correct if the line width of the inner perimeters differs
                // from the line width of the infill?
                const coord_t good_spacing = (perimeter_idx == 1) ? params.get_ext_perimeter_spacing2() : params.get_perimeter_spacing();
                if (thin_perimeter <= 0.98) {
                    const coordf_t overlap_spacing = (1 - thin_perimeter) * params.get_perimeter_spacing() / 2;
                    // This path will ensure, that the perimeters do not overfill, as in 
                    // prusa3d/Slic3r GH #32, but with the cost of rounding the perimeters
                    // excessively, creating gaps, which then need to be filled in by the not very
                    // reliable gap fill algorithm.
                    // Also the offset2(perimeter, -x, x) may sometimes lead to a perimeter, which is larger than
                    // the original.
                    next_onion = offset2_ex(last,
                        -(float)(good_spacing + overlap_spacing - 1),
                        +(float)(overlap_spacing - 1),
                        (params.use_round_perimeters() ? ClipperLib::JoinType::jtRound : ClipperLib::JoinType::jtMiter),
                        (params.use_round_perimeters() ? params.get_min_round_spacing() : 3));
                    if (allow_perimeter_anti_hysteresis) {
                        // now try with different min spacing if we fear some hysteresis
                        // TODO, do that for each polygon from last, instead to do for all of them in one go.
                        ExPolygons no_thin_onion = offset_ex(last, double(-good_spacing));
                        if (last_area < 0) {
                            last_area = 0;
                            for (const ExPolygon &expoly : last) { last_area += expoly.area(); }
                        }
                        double new_area = 0;
                        for (const ExPolygon &expoly : next_onion) { new_area += expoly.area(); }

                        std::vector<float> variations = { .025f, .06f, .125f};// don't over-extrude, so don't use negative variations
                        for (size_t idx_variations = 0; (next_onion.size() > no_thin_onion.size() ||
                                                         (new_area != 0 && last_area > new_area * 100)) &&
                             idx_variations < variations.size();
                             idx_variations++) {
                            const coordf_t spacing_change = params.get_ext_perimeter_spacing() * variations[idx_variations];
                            //use a sightly bigger spacing to try to drastically improve the split, that can lead to very thick gapfill
                            ExPolygons next_onion_secondTry = offset2_ex(
                                last,
                                -(float)(good_spacing + overlap_spacing + spacing_change - 1),
                                +(float)(overlap_spacing + spacing_change - 1));
                            if (next_onion.size() > next_onion_secondTry.size() * 1.2 && next_onion.size() > next_onion_secondTry.size() + 2) {
                                // don't get it if it creates too many
                                next_onion = next_onion_secondTry;
                            } else if (next_onion.size() > next_onion_secondTry.size() || last_area > new_area * 100) {
                                // don't get it if it's too small
                                double area_new = 0;
                                for (const ExPolygon &expoly : next_onion_secondTry) { area_new += expoly.area(); }
                                if (last_area > area_new * 100 || new_area == 0) {
                                    next_onion = next_onion_secondTry;
                                }
                            }
                        }
                        last_area = new_area;
                    }
                } else {
                    // If "overlapping_perimeters" is enabled, this paths will be entered, which 
                    // leads to overflows, as in prusa3d/Slic3r GH #32
                    next_onion = offset_ex(last, double(-good_spacing),
                        (params.use_round_perimeters() ? ClipperLib::JoinType::jtRound : ClipperLib::JoinType::jtMiter),
                        (params.use_round_perimeters() ? params.get_min_round_spacing() : 3));
                }
                // do overhangs_extrusion_width
                // next_overhang = last_overhang;
                move_overhangs(params, next_onion, last_overhang, params.get_overhang_spacing() / 2,
                                ((perimeter_idx == 1) ? params.get_ext_perimeter_spacing() : params.get_perimeter_spacing()) / 2, true);

                std::vector<ExPolygonAsynch> *touse = nullptr;
                std::vector<ExPolygonAsynch> copy;
                if (perimeter_idx < std::max(contour_count, holes_count)) {
                    touse = &last_asynch;
                } else {
                    // for gap fill only: use a copy
                    copy = last_asynch;
                    touse = &copy;
                }
                assert(touse);
                assert_check_ExPolygonAsynch(*touse);
                bool round_peri = params.config.perimeter_round_corners.value;
                float min_round_spacing = round_peri ? unscaled(params.get_perimeter_width()) / 10 : 0;
                if (contour_count > perimeter_idx && holes_count <= perimeter_idx) {
                    grow_contour_only(*touse, good_spacing, (1 - thin_perimeter) * params.get_perimeter_spacing() / 2,
                                        round_peri, min_round_spacing);
                }
                if (holes_count > perimeter_idx && contour_count <= perimeter_idx) {
                    grow_holes_only(*touse, next_onion, good_spacing,
                                    (1 - thin_perimeter) * params.get_perimeter_spacing() / 2, round_peri, min_round_spacing);
                }
                assert_check_ExPolygonAsynch(*touse);
                // if no hole/contour : use the object shape, not the perimeter "end" with overlap.
                bool special_area = contour_count == 0 || holes_count == 0;
                if (special_area &&
                    (params.region_setting.has_many_config(&params.config.thin_walls) ||
                     params.region_setting.get_solo_config(&params.config.thin_walls).get_bool() ||
                     params.spiral_vase)) {
                    area_used = next_onion;
                    for (auto &expolycontainer : *touse) area_used.push_back(expolycontainer.expoly);
                    all_next_onion = &area_used;
                }


                // look for gaps
                if (params.region_setting.has_many_config(&params.config.gap_fill_enabled) ||
                    params.region_setting.get_solo_config(&params.config.gap_fill_enabled).get_bool()) {
                    // check if we are going to have an other perimeter
                    if (perimeter_idx < std::max(contour_count, holes_count) || has_overhang ||
                        all_next_onion->empty() ||
                        (params.config.gap_fill_last.value && perimeter_idx == std::max(contour_count, holes_count))) {
                        // not using safety offset here would "detect" very narrow gaps
                        // (but still long enough to escape the area threshold) that gap fill
                        // won't be able to fill but we'd still remove from infill area
                        ExPolygons no_last_gapfill =
                            offset_ex(*all_next_onion, 0.5f * params.get_perimeter_spacing() + 30,
                                      (params.use_round_perimeters() ? ClipperLib::JoinType::jtRound :
                                                                       ClipperLib::JoinType::jtMiter),
                                      (params.use_round_perimeters() ? params.get_min_round_spacing() : 3));
                        ExPolygons gapfill;
                        if (perimeter_idx == 1) {
                            gapfill = ensure_valid(diff_ex(offset_ex(last,
                                                                     -0.5f * params.get_ext_perimeter_spacing() + 30),
                                                           no_last_gapfill),
                                                   resolution); // safety offset
                        } else {
                            gapfill = ensure_valid(diff_ex(offset_ex(last, -0.5f * params.get_perimeter_spacing()),
                                                           no_last_gapfill),
                                                   resolution); // safety offset
                        }
                        // append gapfill where it's enabled
                        if ((!params.region_setting.has_many_config(&params.config.gap_fill_enabled)) &&
                            params.region_setting.get_solo_config(&params.config.gap_fill_enabled).get_bool()) {
                            append(gaps, gapfill);
                        } else {
                            for (auto const &[gap_fill_enabled, areas] :
                                 params.region_setting.get_areas(&params.config.gap_fill_enabled)) {
                                if (gap_fill_enabled.get_bool()) {
                                    append(gaps, areas.intersections(gapfill));
                                }
                            }
                        }
                    }
                }
            }
            //{
            //    static int aodfjiaz = 0;
            //    std::stringstream stri;
            //    stri << params.layer->id() << "_perimeter_loop_" << (aodfjiaz++) << ".svg";
            //    SVG svg(stri.str());
            //    svg.draw(surface.expolygon, "grey");
            //    svg.draw(to_polylines(last), "yellow");
            //    svg.draw(to_polylines(next_onion), "green");
            //    svg.Close();
            //}

            if (next_onion.empty() && last_asynch.empty()) {
                // Store the number of loops actually generated.
                if (perimeter_idx < contour_count) {
                    assert(contours.size() == contour_count);
                    for(size_t i = perimeter_idx; i<contours.size(); i++)
                        assert(contours[perimeter_idx].empty());
                    contour_count = perimeter_idx;
                    contours.resize(contour_count);
                }
                if (perimeter_idx < holes_count) {
                    assert(holes.size() == holes_count);
                    for(size_t i = perimeter_idx; i<holes.size(); i++)
                        assert(holes[perimeter_idx].empty());
                    holes_count = perimeter_idx;
                    holes.resize(holes_count);
                }
                // No region left to be filled in.
                // if the gapfill doesn't fill the empty areas, don't clear last
                if (params.config.gap_fill_enabled.value && params.config.gap_fill_last.value) {
                    last.clear();
                }
                break;
            } else if (perimeter_idx >= std::max(contour_count, holes_count)) {
                if (has_overhang) {
                    contour_count++;
                    holes_count++; // TODO: only increase the ones that are needed (or just use 2.7)
                    contours.emplace_back();
                    holes.emplace_back();
                } else {
                    // If perimeter_idx > loop_number, we were looking just for gaps.
                    break;
                }
            }
            if (contour_count <= perimeter_idx && !next_onion.empty()) {
                assert(contour_count <= perimeter_idx);
                assert(holes_count > perimeter_idx);
                //assert(contours.size() == perimeter_idx);
                contour_count = perimeter_idx + 1;
                while (contours.size() < contour_count) {
                    contours.emplace_back();
                }
            }
            last_overhang = next_overhang;

            assert(contours.size() == contour_count);
            assert(holes.size() == holes_count);

            // fuzzify params
            const bool fuzzify_contours = params.config.fuzzy_skin != FuzzySkinType::None && perimeter_idx == 0 && params.layer->id() > 0;
            const bool fuzzify_holes = params.config.fuzzy_skin == FuzzySkinType::Shell && perimeter_idx == 0 && params.layer->id() > 0 ;
            const bool fuzzify_all = params.config.fuzzy_skin == FuzzySkinType::All && params.layer->id() > 0 ;

            //push last_asynch or next_onion into contours & holes
            assert_check_ExPolygonAsynch(last_asynch);
            assert_check_loops(contours);
            assert_check_loops(holes);
            if (!last_asynch.empty()) {
                // we already put the last hole, now add contours.
                for (auto &exp : last_asynch) {
                    if (exp.type == ExPolygonAsynch::epatShrinkContour) {
                        assert(next_onion.empty());
                        assert(exp.expoly.contour.is_counter_clockwise());
                        if (exp.expoly.contour.length() > SCALED_EPSILON) { // TODO: atleastLength
                            assert_check_polygon(exp.expoly.contour);
                            contours[perimeter_idx].emplace_back(exp.expoly.contour, perimeter_idx, true,
                                                                 has_steep_overhang, fuzzify_contours || fuzzify_all);
                        }
                    } else {
                        // we already put the last contour, now add holes
                        // contours from hole collapse is added via next_onion
                        assert(exp.type == ExPolygonAsynch::epatGrowHole);
                        for (auto &hole : exp.expoly.holes) {
                            assert(hole.is_clockwise());
                            if (hole.length() > SCALED_EPSILON) { // TODO: atleastLength
                                assert_check_polygon(hole);
                                holes[perimeter_idx].emplace_back(hole, perimeter_idx, false, has_steep_overhang,
                                                                  fuzzify_contours || fuzzify_all);
                            }
                        }
                    }
                }
            }

            // simplify the loop to avoid artifacts when shrinking almost-0 segments
            // also it ensure that there is no point at epsilon distance.
            resolution = get_resolution(perimeter_idx + 1, false);
            last.clear();
            for (ExPolygon &exp : next_onion) {
                exp.simplify((resolution < SCALED_EPSILON ? SCALED_EPSILON : resolution), last);
            }
            assert_check_polygons(to_polygons(last));

            // Add contour & holes from last (wich is now simplified next_onion) 
            for (const ExPolygon& expolygon : last) {
                //TODO: add width here to allow variable width (if we want to extrude a sightly bigger perimeter, see thin wall)
                if (contour_count > perimeter_idx && expolygon.contour.length() > SCALED_EPSILON) { // TODO: atleastLength
                    assert_check_polygon(expolygon.contour);
                    contours[perimeter_idx].emplace_back(expolygon.contour, perimeter_idx, true, has_steep_overhang, fuzzify_contours || fuzzify_all);
                }
                if (!expolygon.holes.empty() && holes_count > perimeter_idx) {
                    holes[perimeter_idx].reserve(holes[perimeter_idx].size() + expolygon.holes.size());
                    for (const Polygon &hole : expolygon.holes) {
                        if (hole.length() > SCALED_EPSILON) { // TODO: atleastLength
                            assert_check_polygon(hole);
                            holes[perimeter_idx].emplace_back(hole, perimeter_idx, false, has_steep_overhang, fuzzify_holes || fuzzify_all);
                        }
                    }
                }
            }

            // store surface for top infill if only_one_perimeter_top
            if (perimeter_idx == 0 &&
                (params.region_setting.has_many_config(&params.config.only_one_perimeter_top) ||
                 params.region_setting.get_solo_config(&params.config.only_one_perimeter_top).get_bool()) &&
                !params.use_bridge && this->upper_slices != nullptr && (contour_count > 1 || holes_count > 1)) {
                this->throw_if_canceled();
                // Check if current layer has surfaces that are not covered by upper layer (i.e., top surfaces)
                ExPolygons fill_clip;

                ExPolygons next;
                for (auto const &[opt_values, areas] :
                     params.region_setting.get_areas(&params.config.only_one_perimeter_top)) {
                    if (opt_values.get_bool(&params.config.only_one_perimeter_top)) {
                        const ExPolygons *upper_slices = this->upper_slices;
                        // has multiple or only one?
                        ExPolygons cliped_upper_slices;
                        if (!areas.is_accept_all()) {
                            // clip upper_slices
                            cliped_upper_slices = diff_ex({surface_expolygon}, areas.expolys);
                            if (!this->upper_slices->empty()) {
                                cliped_upper_slices = union_ex(cliped_upper_slices, *this->upper_slices);
                            }
                            upper_slices = &cliped_upper_slices;
                        }

                        split_top_surfaces(this->lower_slices, upper_slices, last,
                                            results.top_fills, next, results.fill_clip,
                                            std::max(contour_count, holes_count) - 1,
                                            scale_d(opt_values.get_effective_value(unscaled(params.perimeter_flow.scaled_width()), &params.config.min_width_top_surface)),
                                            opt_values.get_bool(&params.config.only_one_perimeter_top_other_algo));
                    }
                }
                last = next;
            }
            
            //if next turn we are in asynch mode, move from last to last_asynch
            if ( !last_asynch_initialized && (
                (holes_count == perimeter_idx + 1 && contour_count > perimeter_idx + 1) ||
                (contour_count == perimeter_idx + 1 && holes_count > perimeter_idx + 1))) {
                coordf_t last_spacing = perimeter_idx == 0 ? params.get_ext_perimeter_spacing() / 2 :
                                                             params.get_perimeter_spacing() / 2;
                // populate last_asynch from last
                for (ExPolygon &expoly : last) {
                    last_asynch.push_back(
                        {holes_count == perimeter_idx + 1 ? ExPolygonAsynch::ExPolygonAsynchType::epatShrinkContour :
                                                            ExPolygonAsynch::ExPolygonAsynchType::epatGrowHole,
                         std::move(expoly), -last_spacing, last_spacing, -last_spacing, last_spacing});
                }
                last.clear();
                last_asynch_initialized = true;
            }
        } // for each loop of an island
        assert_check_loops(contours);
        assert_check_loops(holes);

        // fuzzify
        const bool fuzzify_gapfill = params.config.fuzzy_skin == FuzzySkinType::All && params.layer->id() > 0;
        // check for extracting extra perimeters from gapfill
        if (!gaps.empty() && params.config.gap_fill_perimeter.value) {
            // remove overhangs areas
            if (params.region_setting.has_many_config(&params.config.gap_fill_no_overhang) ||
                params.region_setting.get_solo_config(&params.config.gap_fill_no_overhang).get_bool()) {
                for (auto const &[gap_fill_no_overhang, areas] :
                        params.region_setting.get_areas(&params.config.gap_fill_no_overhang)) {
                    if (gap_fill_no_overhang.get_bool()) {
                        gaps = diff_ex(gaps, areas.intersections(params.overhang_areas));
                    }
                }
            }
            // if needed, add it to the first empty contour list
            const size_t contours_size = contour_count;
            assert(contours.size() == contour_count);
            //first, find loops and try to extract a perimeter from them.
            size_t looked_gap = gaps.size();
            for (size_t gap_idx = 0; gap_idx < looked_gap; gap_idx++) {
                const ExPolygon& expoly = gaps[gap_idx];
                if (expoly.holes.size() >= 1) {
                    //this is a a sort of a loop
                    //try to see if it's possible to add a "perimeter"
                    ExPolygons new_contour = offset_ex(expoly, -(float)(params.get_perimeter_spacing() / 2), ClipperLib::jtMiter, 3);
                    // note: don't unoffset only the contour, or you'll have issues with holes validating impossible perimeter (supermerill/SuperSlicer/issues/4696).
                    if (new_contour.size() == 1 && new_contour.front().holes.size() >= 1) {
                        //create our perimeter area
                        ExPolygons contour_gap_area = offset_ex(new_contour.front(), (float)(params.get_perimeter_spacing() / 2));
                        // create centerline
                        new_contour.front().holes.clear();
                        contour_gap_area = diff_ex(contour_gap_area, offset_ex(new_contour, - (float)(params.get_perimeter_spacing() / 2)));
                        // there was an offset, simplify to avoid too small sections
                        new_contour = new_contour.front().simplify(SCALED_EPSILON);
                        if (new_contour.size() == 1 && contour_gap_area.size() == 1) {
                            // OK
                            perimeter_gaps_ex = union_ex(perimeter_gaps_ex, contour_gap_area);
                            // gap fill outside of the new contour
                            append(gaps, ensure_valid(diff_ex(expoly, contour_gap_area), resolution));
                            // remove our old gapfill
                            gaps.erase(gaps.begin() + gap_idx);
                            looked_gap--;
                            gap_idx--;
                            // update list & variable to let the new perimeter be taken into account
                            contour_count = contours_size + 1;
                            if (contours_size >= contours.size()) {
                                contours.emplace_back();
                                holes.emplace_back();
                            }
                            assert(contours.size() == contour_count);
                            // Add the new perimeter
                            contours[contours_size].emplace_back(new_contour.front().contour, contours_size,
                                                                 true, has_steep_overhang, fuzzify_gapfill);
                        }
                    }
                }
            }
        }
        assert_check_loops(contours);
        assert_check_loops(holes);
        assert(contours.size() == contour_count);
        //assert(holes.size() == holes_count);
        // nest loops: holes first
        for (int d = 0; d < holes_count; ++d) {
            PerimeterGeneratorLoops& holes_d = holes[d];
            // loop through all holes having depth == d
            for (int hole_idx = 0; hole_idx < (int)holes_d.size(); ++hole_idx) {
                if (!normalize_contour(holes_d[hole_idx].polygon)) {
                    continue;
                }
                const PerimeterGeneratorLoop& loop = holes_d[hole_idx];
                assert(loop.polygon.length() > SCALED_EPSILON);
                // find the hole loop that contains this one, if any
                for (int t = d + 1; t < holes_count; ++t) {
                    for (int j = 0; j < (int)holes[t].size(); ++j) {
                        PerimeterGeneratorLoop& candidate_parent = holes[t][j];
                        if (candidate_parent.polygon.contains(loop.polygon.first_point())) {
                            candidate_parent.children.push_back(loop);
                            holes_d.erase(holes_d.begin() + hole_idx);
                            --hole_idx;
                            goto NEXT_LOOP;
                        }
                    }
                }
                // if no hole contains this hole, find the contour loop that contains it
                for (int t = contours.size() - 1; t >= 0; --t) {
                    for (int j = 0; j < (int)contours[t].size(); ++j) {
                        PerimeterGeneratorLoop& candidate_parent = contours[t][j];
                        if (candidate_parent.polygon.contains(loop.polygon.first_point())) {
                            candidate_parent.children.push_back(loop);
                            holes_d.erase(holes_d.begin() + hole_idx);
                            --hole_idx;
                            goto NEXT_LOOP;
                        }
                    }
                }
                // no perimeter, then add the hole like a perimeter.
                while(d >= contours.size())
                    contours.emplace_back();
                contours[d].push_back(loop);
                holes_d.erase(holes_d.begin() + hole_idx);
                --hole_idx;
            NEXT_LOOP:;
            }
        }
        // nest contour loops
        for (int d = contours.size() - 1; d >= 1; --d) {
            PerimeterGeneratorLoops& contours_d = contours[d];
            // loop through all contours having depth == d
            for (int contour_idx = 0; contour_idx < (int)contours_d.size(); ++contour_idx) {
                if (!normalize_contour(contours_d[contour_idx].polygon)) {
                    continue;
                }
                const PerimeterGeneratorLoop& loop = contours_d[contour_idx];
                assert(loop.polygon.length() > SCALED_EPSILON);
                // find the contour loop that contains it
                for (int t = d - 1; t >= 0; --t) {
                    for (size_t j = 0; j < contours[t].size(); ++j) {
                        PerimeterGeneratorLoop& candidate_parent = contours[t][j];
                        if (candidate_parent.polygon.contains(loop.polygon.first_point())) {
                            candidate_parent.children.push_back(loop);
                            contours_d.erase(contours_d.begin() + contour_idx);
                            --contour_idx;
                            goto NEXT_CONTOUR;
                        }
                    }
                }
                //can't find one, put in front
                if (contours.front().empty()) {
                    contours.front().push_back(loop);
                } else {
                    contours.front().front().children.push_back(loop);
                }
                contours_d.erase(contours_d.begin() + contour_idx);
                --contour_idx;
            NEXT_CONTOUR:;
            }
        }
        //remove all empty perimeters
        while(contours.size() > 1 && contours.back().empty())
            contours.pop_back();
        while(contours.size() > 1 && contours.front().empty())
            contours.erase(contours.begin());
        // fuse all unfused 
        // at this point, all loops should be in contours[0] (= contours.front() )
        // or no perimeters nor holes have been generated, too small area.
        assert(contours.size()<=1);
        assert(contours.empty() || contours.front().size() >= 1);
        // collection of loops to add into loops
        ExtrusionEntityCollection peri_entities;
        if (!contours.empty()) {
            if (params.config.perimeter_loop.value) {
                // onlyone_perimeter = >fusion all perimeterLoops
                for (PerimeterGeneratorLoop &loop : contours.front()) {
                    ExtrusionLoop extr_loop = this->_traverse_and_join_loops(params, loop, get_all_childs(loop),
                                                                             loop.polygon.points.front());
                    // ExtrusionLoop extr_loop = this->_traverse_and_join_loops_old(loop, loop.polygon.points.front(), true);
                    if (extr_loop.paths().back().polyline().back() != extr_loop.paths().front().polyline().front()) {
                        extr_loop.paths().back().polyline().append(extr_loop.paths().front().polyline().front());
                        assert(false);
                    }
                    peri_entities.append(extr_loop);
                }

                // append thin walls
                if (!thin_walls_thickpolys.empty()) {
                    if (params.object_config.thin_walls_merge.value) {
                        _merge_thin_walls(params, peri_entities, thin_walls_thickpolys);
                    } else {
                        peri_entities.append(
                            Geometry::thin_variable_width(thin_walls_thickpolys, ExtrusionRole::ThinWall, params.ext_perimeter_flow,
                                                          std::max(params.get_ext_perimeter_width() / 4,
                                                                   scale_i(params.print_config.resolution)),
                                                          false));
                    }
                    thin_walls_thickpolys.clear();
                }
            } else {
#if _DEBUG
                for (const PerimeterGeneratorLoop &epl : contours.front()) {
                    assert_check_polygon(epl.polygon);
                }
#endif
                if (params.object_config.thin_walls_merge.value) {
                    ThickPolylines no_thin_walls;
                    peri_entities = this->_traverse_loops_classic(params, contours.front(), no_thin_walls);
#if _DEBUG
                    LoopAssertVisitor().traverse(peri_entities);
#endif
                    _merge_thin_walls(params, peri_entities, thin_walls_thickpolys);
                } else {
                    peri_entities = this->_traverse_loops_classic(params, contours.front(), thin_walls_thickpolys);
                }
            }
        } else {
            // no loop perimeter : ignore perimeter_loop and thin_walls_merge
            peri_entities = this->_traverse_loops_classic(params, {}, thin_walls_thickpolys);
        }
#if _DEBUG
        LoopAssertVisitor().traverse(peri_entities);
#endif
        // remove the un-needed top collection if only one child.
        //peri_entities.visit(CollectionSimplifyVisitor{});
        if (peri_entities.entities().size() == 1) {
            if (ExtrusionEntityCollection *coll_child = dynamic_cast<ExtrusionEntityCollection *>(
                    peri_entities.entities().front());
                coll_child != nullptr) {
                peri_entities.set_can_sort_reverse(coll_child->can_sort(), coll_child->can_reverse());
                peri_entities.append_move_from(*coll_child);
                peri_entities.remove(0);
            }
        }

        //{
        //    static int aodfjiaqsdz = 0;
        //    std::stringstream stri;
        //    stri << params.layer->id() << "_perimeter_loops_" << (aodfjiaqsdz++) << ".svg";
        //    SVG svg(stri.str());
        //    svg.draw(surface_expolygon, "grey");
        //    struct TempVisitor : public ExtrusionVisitorRecursiveConst {
        //        SVG* svg;
        //        virtual void use(const ExtrusionPath& path) override { svg->draw(path.polyline(), "green"); }
        //    } bbvisitor;
        //    bbvisitor.svg = &svg;
        //    peri_entities.visit(bbvisitor);
        //    svg.Close();
        //}

        // append perimeters for this slice as a collection
        if (!peri_entities.empty()) {
            //move it, to avoid to clone evrything and then delete it
            loops.append(peri_entities);
        }
    } // if contour & hole
#ifdef _DEBUGINFO
    LoopAssertVisitor().traverse(loops);
#endif

    // fill gaps
    ExPolygons gaps_ex;
    if (!gaps.empty()) {
        // collapse 
        coordf_t min = 0.2 * params.get_perimeter_width() * (1 - INSET_OVERLAP_TOLERANCE);
        //be sure we don't gapfill where the perimeters are already touching each other (negative spacing).
        min = std::max(min, double(Flow::new_from_spacing((float)EPSILON, (float)params.perimeter_flow.nozzle_diameter(), (float)params.layer->unscaled_height(), (float)params.perimeter_flow.spacing_ratio(), false).scaled_width()));
        coordf_t real_max = 2.5 * params.get_perimeter_spacing();
        const coordf_t minwidth = scale_d(params.config.gap_fill_min_width.get_effective_value(unscaled((double)params.get_perimeter_width())));
        const coordf_t maxwidth = scale_d(params.config.gap_fill_max_width.get_effective_value(unscaled((double)params.get_perimeter_width())));
        const coord_t minlength = scale_i(params.config.gap_fill_min_length.get_effective_value(unscaled((double)params.get_perimeter_width())));
        if (minwidth > 0) {
            min = std::max(min, minwidth);
        }
        coordf_t max = real_max;
        if (maxwidth > 0) {
            max = std::min(max, maxwidth);
        }
        const coord_t gapfill_extension = scale_i(params.config.gap_fill_extension.get_effective_value(unscaled((double)params.get_perimeter_width())));
        //remove areas that are too big (shouldn't occur...)
        ExPolygons too_big = offset2_ex(gaps, double(-max / 2), double(+max / 2));
        ExPolygons gaps_ex_to_test = too_big.empty() ? gaps : diff_ex(gaps, too_big, ApplySafetyOffset::Yes);
        const double minarea = scale_d(scale_d(params.config.gap_fill_min_area.get_effective_value(unscaled((double)params.get_perimeter_width()) * unscaled((double)params.get_perimeter_width()))));
        // check each gapfill area to see if it's printable.
        for (const ExPolygon& expoly : gaps_ex_to_test) {
            this->throw_if_canceled();
            //remove too small gaps that are too hard to fill.
            //ie one that are smaller than an extrusion with width of min and a length of max.
            if (expoly.area() > minarea) {
                const coordf_t offset_test = min * 0.5;
                ExPolygons     expoly_after_shrink_test = offset_ex(ExPolygons{expoly}, -offset_test);
                //if the shrink split the area in multipe bits
                if (expoly_after_shrink_test.size() > 1) {
                    //remove too small bits
                    for (int exp_idx = 0; exp_idx < expoly_after_shrink_test.size(); exp_idx++) {
                        if (expoly_after_shrink_test[exp_idx].area() < (SCALED_EPSILON * SCALED_EPSILON * 4)) {
                            expoly_after_shrink_test.erase(expoly_after_shrink_test.begin() + exp_idx);
                            exp_idx--;
                        } else {
                            ExPolygons wider = offset_ex(ExPolygons{ expoly_after_shrink_test[exp_idx] }, offset_test);
                            if (wider.empty() || wider[0].area() < minarea) {
                                expoly_after_shrink_test.erase(expoly_after_shrink_test.begin() + exp_idx);
                                exp_idx--;
                            }
                        }
                    }
                    //maybe some areas are a just bit too thin, try with just a little more offset to remove them.
                    const coordf_t offset_test_2 = min * 0.8;
                    ExPolygons     expoly_after_shrink_test2 = offset_ex(ExPolygons{expoly}, -offset_test_2);
                    for (int exp_idx = 0; exp_idx < expoly_after_shrink_test2.size(); exp_idx++) {
                        if (expoly_after_shrink_test2[exp_idx].area() < (SCALED_EPSILON * SCALED_EPSILON * 4)) {
                            expoly_after_shrink_test2.erase(expoly_after_shrink_test2.begin() + exp_idx);
                            exp_idx--;

                        } else {
                            ExPolygons wider = offset_ex(ExPolygons{ expoly_after_shrink_test2[exp_idx] }, offset_test_2);
                            if (wider.empty() || wider[0].area() < minarea) {
                                expoly_after_shrink_test2.erase(expoly_after_shrink_test2.begin() + exp_idx);
                                exp_idx--;
                            }
                        }
                    }
                    //it's better if there are significantly less extrusions
                    if (expoly_after_shrink_test.size() / 1.42 > expoly_after_shrink_test2.size()) {
                        expoly_after_shrink_test2 = offset_ex(expoly_after_shrink_test2, offset_test_2);
                        //insert with move instead of copy
                        std::move(expoly_after_shrink_test2.begin(), expoly_after_shrink_test2.end(), std::back_inserter(gaps_ex));
                    } else {
                        expoly_after_shrink_test = offset_ex(expoly_after_shrink_test, offset_test);
                        std::move(expoly_after_shrink_test.begin(), expoly_after_shrink_test.end(), std::back_inserter(gaps_ex));
                    }
                } else {
                    expoly_after_shrink_test = offset_ex(expoly_after_shrink_test, offset_test);
                    std::move(expoly_after_shrink_test.begin(), expoly_after_shrink_test.end(), std::back_inserter(gaps_ex));
                }
            }
        }
        // create lines from the area
        ThickPolylines polylines;
        for (const ExPolygon& ex : gaps_ex) {
            Geometry::MedialAxis md{ ex, coord_t(real_max), coord_t(min), coord_t(params.layer->unscaled_height()) };
            if (minlength > 0) {
                md.set_min_length(minlength);
            }
            if (gapfill_extension > 0) {
                md.set_extension_length(gapfill_extension);
            }
            md.set_biggest_width(max);
            md.build(polylines);
        }
        // create extrusion from lines
        Flow gap_fill_flow = Flow::new_from_width(params.perimeter_flow.width(),
                                                  params.perimeter_flow.nozzle_diameter(),
                                                  params.perimeter_flow.height(),
                                                  params.config.gap_fill_overlap.get_effective_value(1.), false);
        if (!polylines.empty()) {
            gap_fill.append(Geometry::thin_variable_width(
                polylines,
                ExtrusionRole::GapFill, 
                gap_fill_flow, 
                scale_i(params.print_config.resolution_internal),
                true));
            /*  Make sure we don't infill narrow parts that are already gap-filled
                (we only consider this surface's gaps to reduce the diff() complexity).
                Growing actual extrusions ensures that gaps not filled by medial axis
                are not subtracted from fill surfaces (they might be too short gaps
                that medial axis skips but infill might join with other infill regions
                and use zigzag).  */
                // get clean surface of gap
            results.gap_srf = union_ex(offset(gap_fill.polygons_covered_by_width(float(SCALED_EPSILON) / 10), float(SCALED_EPSILON / 2)));
            // intersection to ignore the bits of gapfill tha may be over infill, as it's epsilon and there may be some voids here anyway.
            results.gap_srf = intersection_ex(results.gap_srf, gaps_ex);
            // the diff(last, gap) will be done after, as we have to keep the last un-gapped to avoid unneeded gap/infill offset
        }
        results.gap_srf = union_ex(results.gap_srf, perimeter_gaps_ex);
    }

    if (contour_count == 0 && holes_count == 0) {
        // for the infill shell, move it a little bit inside so the extrusion tip don't go over the sides.
        results.inner_perimeter = offset_ex(last, -(params.get_perimeter_width() - params.get_perimeter_spacing()) / 2);
    } else {
        coordf_t last_spacing = std::max(contour_count, holes_count) == 1 ?
                                                      params.get_ext_perimeter_spacing() / 2 :
                                                      params.get_perimeter_spacing() / 2;
        results.inner_perimeter = offset_ex(last, -last_spacing);
        if (!last_asynch.empty()) {
            // merge with last_async
            for (auto &exp : last_asynch) {
                if (exp.offset_contour_inner == exp.offset_holes_inner) {
                    append(results.inner_perimeter, offset_ex(exp.expoly, exp.offset_contour_inner));
                } else {
                    // offset contour & holes separatly
                    // first holes:
                    assert(exp.offset_holes_inner <= 0);
                    Polygons holes = exp.offset_holes_inner != 0 ?
                        offset(get_holes_as_contour(exp.expoly), -exp.offset_holes_inner) :
                        get_holes_as_contour(exp.expoly);
                    // we are growing (fake) perimeter, so it can creates holes.
                    for (size_t i = 0; i < holes.size(); ++i) {
                        Polygon &fakeperi = holes[i];
                        if (fakeperi.is_clockwise()) {
                            // put real perimeters in results.inner_perimeter
                            fakeperi.make_counter_clockwise();
                            results.inner_perimeter.push_back(ExPolygon(fakeperi));
                            holes.erase(holes.begin() + i);
                            i--;
                        }
                    }
                    // now shrink perimeter
                    Polygons perimeters = offset(exp.expoly.contour, exp.offset_contour_inner);
                    // as it shrink, it can creates more perimeter, not a big deal.
                    for (auto &p : perimeters) assert(p.is_counter_clockwise());
                    
                    // now diff and add
                    append(results.inner_perimeter, diff_ex(perimeters, holes));
                }
            }
        }
    }
    append(results.inner_perimeter, saved_infill);

#ifdef _DEBUGINFO
    LoopAssertVisitor().traverse(loops);
#endif
    return results;
}

void PerimeterGenerator::_merge_thin_walls(const Parameters &params, ExtrusionEntityCollection &extrusions, ThickPolylines &thin_walls) const
{
#if _DEBUG
    LoopAssertVisitor().traverse(extrusions);
#endif
    //TODO: find a way to avoid double copy (from EntityCollection to ChangeFlow to searcher.search_result.loop
    class ChangeFlow : public ExtrusionVisitor {
    public:
        ChangeFlow(coordf_t resolution) : resolution_sqr(resolution * resolution) {}
        float percent_extrusion;
        bool no_seam = false;
        std::vector<ExtrusionPath> paths;
        const Point* first_point = nullptr;
        coordf_t resolution_sqr;
        //TODO real travel with role & width
        void ensure_travel_to(const Point &pt) {
            assert(!paths.empty());
            Point last_point = paths.back().last_point();
            if (last_point != pt) {
                if (last_point.distance_to_square(pt) < resolution_sqr) {
                    paths.back().polyline().set_back(pt);
                } else {
                    //add travel
                    ExtrusionPath travel({ExtrusionRole::Travel}, nullptr, false);
                    travel.attributes_mutable().width = paths.back().width();
                    travel.attributes_mutable().height = paths.back().height();
                    travel.attributes_mutable().mm3_per_mm = 0;
                    travel.attributes_mutable().no_seam = no_seam;
                    travel.polyline().append(last_point);
                    travel.polyline().append(pt);
                    paths.push_back(travel);
                }
            }
        }
        virtual void default_use(ExtrusionEntity &entity) override {
            if (!entity.is_leaf()) {
                for (ExtrusionEntityUPtr &child : entity.children())
                    if (child)
                        child->visit(*this);
                return;
            }
            ExtrusionPath *path_ptr = dynamic_cast<ExtrusionPath*>(&entity);
            ExtrusionPath tmp_path(ExtrusionAttributes(ExtrusionRole::None), nullptr);
            ExtrusionPath &path = path_ptr != nullptr ? *path_ptr : tmp_path;
            if (path_ptr == nullptr) {
                const ExtrusionAttributes *attributes = entity.get_property<ExtrusionAttributes>();
                if (attributes == nullptr || entity.polyline_or_null() == nullptr)
                    return;
                path = ExtrusionPath(entity.polyline_ref(), *attributes, entity.clone_properties(), entity.can_reverse());
            }
            //ensure the loop is continue.
            if (first_point != nullptr) {
                if (*first_point != path.first_point()) {
                    if (first_point->distance_to_square(path.first_point()) < resolution_sqr) {
                        path.polyline().set_front(*first_point);
                    } else {
                        //add travel
                        ExtrusionPath travel(ExtrusionAttributes(ExtrusionRole::Travel, ExtrusionFlow(0, path.width(), path.height())), nullptr, false);
                        travel.polyline().append(*first_point);
                        travel.polyline().append(path.first_point());
                        travel.attributes_mutable().no_seam = no_seam;
                        paths.push_back(travel);
                    }
                }
                first_point = nullptr;
            }
            path.attributes_mutable().mm3_per_mm *= percent_extrusion;
            path.attributes_mutable().width *= percent_extrusion;
            path.attributes_mutable().no_seam = no_seam;
            paths.push_back(path);
        }
    };
    struct BestPoint {
        //Point p;
        ExtrusionPath *path;
        size_t idx_path;
        ExtrusionLoop *loop;
        size_t idx_line;
        Line line;
        double dist;
        bool from_start;
    };
    //use a visitor to found the best point.
    class SearchBestPoint : public ExtrusionVisitor {
    public:
        ThickPolyline* thin_wall;
        BestPoint search_result;
        size_t idx_path;
        ExtrusionLoop *current_loop = nullptr;
        virtual void default_use(ExtrusionEntity &entity) override {
            if (!entity.is_leaf()) {
                ExtrusionLoop *last_loop = current_loop;
                if (ExtrusionLoop *loop = dynamic_cast<ExtrusionLoop*>(&entity))
                    current_loop = loop;
                idx_path = 0;
                for (ExtrusionEntityUPtr &child : entity.children()) {
                    if (child)
                        child->visit(*this);
                    idx_path++;
                }
                current_loop = last_loop;
                return;
            }
            ExtrusionPath *path_ptr = dynamic_cast<ExtrusionPath*>(&entity);
            if (path_ptr == nullptr)
                return;
            ExtrusionPath &path = *path_ptr;
            //don't consider other thin walls.
            if (path.role() == ExtrusionRole::ThinWall) return;
            //for each segment
            //Lines lines = 
            assert(path.polyline().size() > 1);
            Line current_line(path.polyline().front(), path.polyline().front());
            
            for (size_t idx_line = 0; idx_line + 1 < path.polyline().size(); idx_line++) {
                current_line.a = current_line.b;
                current_line.b = path.polyline().get_point(idx_line + 1);
                //look for nearest point
                double dist = current_line.distance_to_squared(thin_wall->front());
                if (dist < search_result.dist) {
                    search_result.path = &path;
                    search_result.idx_path = idx_path;
                    search_result.idx_line = idx_line;
                    search_result.line = current_line;
                    search_result.dist = dist;
                    search_result.from_start = true;
                    search_result.loop = current_loop;
                }
                dist = current_line.distance_to_squared(thin_wall->back());
                if (dist < search_result.dist) {
                    search_result.path = &path;
                    search_result.idx_path = idx_path;
                    search_result.idx_line = idx_line;
                    search_result.line = current_line;
                    search_result.dist = dist;
                    search_result.from_start = false;
                    search_result.loop = current_loop;
                }
            }
        }
    };
    //max dist to branch: ~half external perimeter width
    coord_t max_width = params.ext_perimeter_flow.scaled_width();
    SearchBestPoint searcher;
    ThickPolylines not_added;
    //search the best extusion/point to branch into
     //for each thin wall
    int idx = 0;
    for (ThickPolyline &tw : thin_walls) {
        searcher.thin_wall = &tw;
        searcher.search_result.dist = double(max_width);
        searcher.search_result.dist *= searcher.search_result.dist;
        searcher.search_result.path = nullptr;
        searcher.use(extrusions);
        idx++;
        //now insert thin wall if it has a point
        //it found a segment
        if (searcher.search_result.path != nullptr) {
#if _DEBUG
            LoopAssertVisitor().traverse(*searcher.search_result.loop);
            const ExtrusionLoop orig_loop = *searcher.search_result.loop;
#endif
            if (!searcher.search_result.from_start)
                tw.reverse();
            //save old path, as it may be destroyed before being re-created and we want to keep its parameters.
            ExtrusionPath path_to_split = *searcher.search_result.path; // TODO: 2.7: just save hte pathsettigns
            //get the point
            Point point = tw.front().projection_onto(searcher.search_result.line.a, searcher.search_result.line.b);
            //we have to create 3 paths: 1: thinwall extusion, 2: thinwall return, 3: end of the path
            //create new path : end of the path
            ArcPolyline poly_after;
            ArcPolyline first_part;
            assert(searcher.search_result.path->polyline().length() > SCALED_EPSILON);
            searcher.search_result.path->polyline().split_at_index(searcher.search_result.idx_line, first_part, poly_after);
            first_part.append(point);
            poly_after.append_before(point);
            // remove next point if too near to point for the poly_after
            if (poly_after.size() > 1 && poly_after.front().coincides_with_epsilon(poly_after.get_point(1))) {
                Point pt_front = poly_after.front();
                poly_after.pop_front();
                poly_after.set_front(pt_front);
            }
            // same for first_part
            if (first_part.size() > 2 && first_part.back().coincides_with_epsilon(first_part.get_point(first_part.size() - 2))) {
                Point pt_back = first_part.back();
                first_part.pop_back();
                first_part.set_back(pt_back);
            }
            assert(first_part.size() == 2 || first_part.is_valid());
            assert(poly_after.size() == 2 || poly_after.is_valid());
            assert(first_part.length() > SCALED_EPSILON || poly_after.length() > SCALED_EPSILON);

            size_t idx_path_before = searcher.search_result.idx_path;
            size_t idx_path_to_add = idx_path_before + 1;
            //check if the first part of the split polyline is long enough.
            assert(!first_part.empty());
            bool point_moved = false;
            if (first_part.size() <= 1 || first_part.length() < SCALED_EPSILON) {
                assert(first_part.size() == 2);
                //not long enough, move point to first point and destroy it
                // idx_path_before will be replaced anyway by poly_after
                assert(!searcher.search_result.loop->paths()[idx_path_before].empty());
                point = searcher.search_result.loop->paths()[idx_path_before].first_point();
                assert(first_part.front().coincides_with_epsilon(poly_after.front()));
                poly_after.set_front(first_part.front());
                first_part.clear();
                point_moved = true;
            } else {
                //long enough
                assert(first_part.front() == searcher.search_result.loop->paths()[idx_path_before].polyline().front());
                assert(first_part.back() == point);
                searcher.search_result.loop->paths()[idx_path_before].polyline() = first_part;
            }
            assert(idx_path_before > searcher.search_result.loop->paths().size() || searcher.search_result.loop->paths()[idx_path_before].size() >= 2);
            assert(idx_path_before > searcher.search_result.loop->paths().size() || searcher.search_result.loop->paths()[idx_path_before].length() > SCALED_EPSILON);
            // check if poly_after is big enough to be added
            if (poly_after.size() <= 1 || poly_after.length()  < SCALED_EPSILON) {
                assert(poly_after.size() == 2);
                assert(!point_moved);
                //use last point as the end pos
                assert(searcher.search_result.loop->paths()[idx_path_before].polyline().back() != poly_after.back());
                assert(searcher.search_result.loop->paths()[idx_path_before].polyline().back().coincides_with_epsilon(poly_after.back()));
                searcher.search_result.loop->paths()[idx_path_before].polyline().set_back(poly_after.back());
                point = poly_after.back();
                poly_after.clear();
                point_moved = true;
            } else {
                assert(poly_after.length() > SCALED_EPSILON);
                if (first_part.empty()) {
                    searcher.search_result.loop->paths()[idx_path_before].polyline() = poly_after;
                    idx_path_to_add--;
                    assert(idx_path_to_add < searcher.search_result.loop->paths().size());
                    if (idx_path_to_add >= searcher.search_result.loop->paths().size())
                        idx_path_to_add = searcher.search_result.loop->paths().size() - 1;
                } else {
                    searcher.search_result.loop->paths().insert(searcher.search_result.loop->paths().begin() + idx_path_to_add,
                        ExtrusionPath(poly_after, path_to_split.attributes(), path_to_split.clone_properties(), path_to_split.can_reverse()));
                }
            }
            assert(idx_path_before > searcher.search_result.loop->paths().size() || searcher.search_result.loop->paths()[idx_path_before].polyline().size() > 1);
            assert(poly_after.size() > 0);
#if _DEBUG
            LoopAssertVisitor().traverse(*searcher.search_result.loop);
#endif
            
            //create thin wall path extrusion
            ExtrusionEntityCollection tws;
            tws.append(Geometry::thin_variable_width({tw}, ExtrusionRole::ThinWall, params.ext_perimeter_flow,
                                                     std::max(params.ext_perimeter_flow.scaled_width() / 10,
                                                              scale_i(params.print_config.resolution)),
                                                     false));
            assert(!tws.entities().empty());
#if _DEBUG
            LoopAssertVisitor().traverse(*searcher.search_result.loop);
            LoopAssertVisitor().traverse(tws);
#endif
            ChangeFlow change_flow(std::max(scale_i(params.print_config.resolution.value), SCALED_EPSILON));
            if (tws.entities().size() == 1 && tws.entities()[0]->is_loop()) {
                //loop, just add it 
                change_flow.first_point = &point;
                change_flow.percent_extrusion = 1;
                change_flow.use(tws);
                // ChangeFlow added the first move if needed, now add the second
                change_flow.ensure_travel_to(point);
                //add move around
                searcher.search_result.loop->paths().insert(searcher.search_result.loop->paths().begin() + idx_path_to_add,
                    change_flow.paths.begin(), change_flow.paths.end());
                ////add move to -> ??? i don't remember why i wrote that, so here it's removed.
                assert(poly_after.front() == point);
                //if (poly_after.first_point() != point) {
                //    assert(poly_after.first_point().coincides_with_epsilon(point));
                //    assert(searcher.search_result.loop->paths().size() > idx_path_to_add);
                //    assert(poly_after.first_point().coincides_with_epsilon(searcher.search_result.loop->paths()[idx_path_to_add].polyline().set_points().front()));
                //    searcher.search_result.loop->paths()[idx_path_to_add].polyline().set_points().front() = poly_after.first_point();
                //}

#if _DEBUG
                LoopAssertVisitor().traverse(*searcher.search_result.loop);
#endif
            } else {
                //make these thin wall un-seamable
                change_flow.no_seam = true;

                //first add the return path
                //ExtrusionEntityCollection tws_second = tws; // this does a deep copy
                change_flow.first_point = &poly_after.front(); // end at the start of the next path
                change_flow.percent_extrusion = 0.1f;
                change_flow.use(tws); // tws_second); //does not need the deep copy if the change_flow copy the content instead of re-using it.
                // force reverse
                for (ExtrusionPath &path : change_flow.paths)
                    path.reverse();
                std::reverse(change_flow.paths.begin(), change_flow.paths.end());
                size_t idx_path_to_add_after = idx_path_to_add < searcher.search_result.loop->paths().size() ?
                    idx_path_to_add :
                    searcher.search_result.loop->paths().size() - 1;
                assert(searcher.search_result.loop->paths()[idx_path_to_add_after].polyline().front() == change_flow.paths.back().polyline().back());
                //std::reverse(change_flow.paths.begin(), change_flow.paths.end());
                searcher.search_result.loop->paths().insert(searcher.search_result.loop->paths().begin() + idx_path_to_add,
                    change_flow.paths.begin(), change_flow.paths.end()); //TODO 2.7:change role by a kind of thinwalltravel that won't be considered for seam
                //add the real extrusion path
                change_flow.first_point = &point; // start at the end of previous extrusion
                change_flow.percent_extrusion = 9.f; // 0.9 but as we modified it by 0.1 just before, has to multiply by 10
                change_flow.paths = std::vector<ExtrusionPath>();
                change_flow.use(tws);
#if _DEBUG
                for (ExtrusionPath &path : change_flow.paths)
                    LoopAssertVisitor().traverse(path);
#endif
                size_t idx_path_to_add_before = (idx_path_to_add - 1) < searcher.search_result.loop->paths().size() ?
                    (idx_path_to_add - 1) :
                    searcher.search_result.loop->paths().size() - 1;
                assert(searcher.search_result.loop->paths()[idx_path_to_add_before].polyline().back() == change_flow.paths.front().polyline().front());
                searcher.search_result.loop->paths().insert(searcher.search_result.loop->paths().begin() + idx_path_to_add,
                    change_flow.paths.begin(), change_flow.paths.end());
#if _DEBUG
                LoopAssertVisitor().traverse(*searcher.search_result.loop);
#endif
            }
        } else {
            not_added.push_back(tw);
        }
    }
#if _DEBUG
    LoopAssertVisitor().traverse(extrusions);
#endif
    //now add thinwalls that have no anchor (make them reversable)
    extrusions.append(Geometry::thin_variable_width(not_added, ExtrusionRole::ThinWall, params.ext_perimeter_flow, std::max(params.ext_perimeter_flow.scaled_width() / 4, scale_i(params.print_config.resolution)), true));
#if _DEBUG
    LoopAssertVisitor().traverse(extrusions);
#endif
}

PerimeterIntersectionPoint PerimeterGenerator::_get_nearest_point(const Parameters &params,
                                                                  const PerimeterGeneratorLoops &children,
                                                                  ExtrusionLoop &                myPolylines,
                                                                  const coord_t                  dist_cut,
                                                                  const coord_t                  max_dist) const
{
    //find best points of intersections
    PerimeterIntersectionPoint intersect;
    intersect.distance = 0x7FFFFFFF; // ! assumption on intersect type & max value
    intersect.idx_polyline_outter = -1;
    intersect.idx_children = -1;
    for (size_t idx_child = 0; idx_child < children.size(); idx_child++) {
        const PerimeterGeneratorLoop &child = children[idx_child];
        for (size_t idx_poly = 0; idx_poly < myPolylines.paths().size(); idx_poly++) {
            //if (myPolylines.paths()[idx_poly].extruder_id == (unsigned int)-1) continue;
            if (myPolylines.paths()[idx_poly].length() < dist_cut + params.perimeter_flow.scaled_width()/20) continue;

            if ((myPolylines.paths()[idx_poly].role() == ExtrusionRole::ExternalPerimeter || child.is_external() )
                && (params.object_config.seam_position.value != SeamPosition::spRandom && params.object_config.seam_position.value != SeamPosition::spAllRandom)) {
                //first, try to find 2 point near enough  //TODO: use seam placer or at least an equivalent.
                for (size_t idx_point = 0; idx_point < myPolylines.paths()[idx_poly].polyline().size(); idx_point++) {
                    const Point &p = myPolylines.paths()[idx_poly].polyline().get_point(idx_point);
                    const Point &nearest_p = *child.polygon.closest_point(p);
                    const double dist = nearest_p.distance_to(p);
                    //Try to find a point in the far side, aligning them
                    if (dist + dist_cut / 20 < intersect.distance || 
                        (params.config.perimeter_loop_seam.value == spRear && (intersect.idx_polyline_outter <0 || p.y() > intersect.outter_best.y())
                            && dist <= max_dist && intersect.distance + dist_cut / 20)) {
                        //ok, copy the idx
                        intersect.distance = (coord_t)nearest_p.distance_to(p);
                        intersect.idx_children = idx_child;
                        intersect.idx_polyline_outter = idx_poly;
                        intersect.outter_best = p;
                        intersect.child_best = nearest_p;
                    }
                }
            } else {
                //first, try to find 2 point near enough
                for (size_t idx_point = 0; idx_point < myPolylines.paths()[idx_poly].polyline().size(); idx_point++) {
                    const Point &p = myPolylines.paths()[idx_poly].polyline().get_point(idx_point);
                    const Point &nearest_p = *child.polygon.closest_point(p);
                    const double dist = nearest_p.distance_to(p);
                    if (dist + SCALED_EPSILON < intersect.distance || 
                        (params.config.perimeter_loop_seam.value == spRear && (intersect.idx_polyline_outter<0 || p.y() < intersect.outter_best.y())
                            && dist <= max_dist && intersect.distance + dist_cut / 20)) {
                        //ok, copy the idx
                        intersect.distance = (coord_t)nearest_p.distance_to(p);
                        intersect.idx_children = idx_child;
                        intersect.idx_polyline_outter = idx_poly;
                        intersect.outter_best = p;
                        intersect.child_best = nearest_p;
                    }
                }
            }
        }
    }
    if (intersect.distance <= max_dist) {
        return intersect;
    }

    for (size_t idx_child = 0; idx_child < children.size(); idx_child++) {
        const PerimeterGeneratorLoop &child = children[idx_child];
        for (size_t idx_poly = 0; idx_poly < myPolylines.paths().size(); idx_poly++) {
            //if (myPolylines.paths()[idx_poly].extruder_id == (unsigned int)-1) continue;
            if (myPolylines.paths()[idx_poly].length() < dist_cut + params.perimeter_flow.scaled_width() / 20) continue;

            //second, try to check from one of my points
            //don't check the last point, as it's used to go outter, can't use it to go inner.
            for (size_t idx_point = 1; idx_point < myPolylines.paths()[idx_poly].polyline().size()-1; idx_point++) {
                const Point &p = myPolylines.paths()[idx_poly].polyline().get_point(idx_point);
                Point nearest_p = child.polygon.point_projection(p).first;
                coord_t dist = (coord_t)nearest_p.distance_to(p);
                //if no projection, go to next
                if (dist == 0) continue;
                if (dist + SCALED_EPSILON / 2 < intersect.distance) {
                    //ok, copy the idx
                    intersect.distance = dist;
                    intersect.idx_children = idx_child;
                    intersect.idx_polyline_outter = idx_poly;
                    intersect.outter_best = p;
                    intersect.child_best = nearest_p;
                }
            }
        }
    }
    if (intersect.distance <= max_dist) {
        return intersect;
    }

    for (size_t idx_child = 0; idx_child < children.size(); idx_child++) {
        const PerimeterGeneratorLoop &child = children[idx_child];
        for (size_t idx_poly = 0; idx_poly < myPolylines.paths().size(); idx_poly++) {
            //if (myPolylines.paths()[idx_poly].extruder_id == (unsigned int)-1) continue;
            if (myPolylines.paths()[idx_poly].length() < dist_cut + params.perimeter_flow.scaled_width() / 20) continue;
            Polyline strait_polyline = myPolylines.paths()[idx_poly].polyline().to_polyline(); //TODO: create point_projection into ArcPolyline (can emit exception if arc)
            //lastly, try to check from one of his points
            for (size_t idx_point = 0; idx_point < child.polygon.size(); idx_point++) {
                const Point &p = child.polygon.points[idx_point];
                Point nearest_p = strait_polyline.point_projection(p).first;
                coord_t dist = (coord_t)nearest_p.distance_to(p);
                //if no projection, go to next
                if (dist == 0) continue;
                if (dist + SCALED_EPSILON / 2 < intersect.distance) {
                    //ok, copy the idx
                    intersect.distance = dist;
                    intersect.idx_children = idx_child;
                    intersect.idx_polyline_outter = idx_poly;
                    intersect.outter_best = nearest_p;
                    intersect.child_best = p;
                }
            }
        }
    }
    return intersect;
}


ExtrusionLoop PerimeterGenerator::_extrude_and_cut_loop(const Parameters &params,
                                                        const PerimeterGeneratorLoop &loop,
                                                        const Point                   entry_point,
                                                        const Line &                  direction,
                                                        bool                          enforce_loop) const
{

    bool need_to_reverse = false;
    Polyline initial_polyline;
    coord_t dist_cut = scale_i(params.print_config.nozzle_diameter.get_at(params.config.perimeter_extruder - 1));

    //fuzzify first in this case, as it's a bit complicated to do it after.
    Polygon fuzzy_poly;
    if (loop.fuzzify) {
        fuzzy_poly = loop.polygon;
        double nozle_diameter = loop.is_external() ? params.ext_perimeter_flow.nozzle_diameter() : params.perimeter_flow.nozzle_diameter();
        double fuzzy_skin_thickness = params.config.fuzzy_skin_thickness.get_effective_value(nozle_diameter);
        double fuzzy_skin_point_dist = params.config.fuzzy_skin_point_dist.get_effective_value(nozle_diameter);
        fuzzy_polygon(fuzzy_poly, scale_d(fuzzy_skin_thickness), scale_d(fuzzy_skin_point_dist));
    }
    const Polygon& poly_to_use = loop.fuzzify ? fuzzy_poly : loop.polygon;

    if (poly_to_use.size() < 3) return ExtrusionLoop(elrDefault);
    if (poly_to_use.length() < dist_cut * 2) {
        if (enforce_loop) {
            //do something to still use it
            dist_cut = poly_to_use.length() / 4;
        } else {
            //reduce it ot a single-point loop that will eb emrged inside the complex path
            ExtrusionLoop single_point(elrDefault);
            Polyline poly_point;
            poly_point.append(poly_to_use.centroid());
            single_point.paths().emplace_back(ExtrusionAttributes(loop.is_external() ? ExtrusionRole::ExternalPerimeter :
                                                                                     ExtrusionRole::Perimeter,
                                                                ExtrusionFlow((double) (loop.is_external() ? params.ext_mm3_per_mm() :
                                                                                                             params.mm3_per_mm()),
                                                                              (float) (loop.is_external() ?
                                                                                           params.ext_perimeter_flow.width() :
                                                                                           params.perimeter_flow.width()),
                                                                              (float) (params.layer->unscaled_height()))),
                                            nullptr, false /*can't reverse*/);
            single_point.paths().back().polyline() = poly_point;
            return single_point;
        }
    }
    const size_t idx_closest_from_entry_point = poly_to_use.closest_point_index(entry_point);
    if (poly_to_use.points[idx_closest_from_entry_point].distance_to(entry_point) > SCALED_EPSILON * 2) {
        //create new Point
        //get first point
        size_t idx_before = -1;
        for (size_t idx_p_a = 0; idx_p_a < poly_to_use.points.size(); ++idx_p_a) {
            Line l(poly_to_use.points[idx_p_a], poly_to_use.points[(idx_p_a + 1 == poly_to_use.points.size()) ? 0 : (idx_p_a + 1)]);
            if (l.distance_to(entry_point) < SCALED_EPSILON) {
                idx_before = idx_p_a;
                break;
            }
        }
        if (idx_before == (size_t)-1) std::cerr << "ERROR: _traverse_and_join_loops : idx_before can't be finded to create new point\n";
        initial_polyline = poly_to_use.split_at_index(idx_before);
        initial_polyline.points.push_back(entry_point);
        initial_polyline.points[0] = entry_point;
    } else {
        initial_polyline = poly_to_use.split_at_index(idx_closest_from_entry_point);
    }
   

    //std::vector<PerimeterPolylineNode> myPolylines;
    ExtrusionLoop my_loop;

    //overhang / notoverhang
    {
        bool is_external = loop.is_external();

        ExtrusionRole role = ExtrusionRole::None;
        ExtrusionLoopRole loop_role;
        role = is_external ? ExtrusionRole::ExternalPerimeter : ExtrusionRole::Perimeter;
        if (loop.is_internal_contour()) {
            // Note that we set loop role to ContourInternalPerimeter
            // also when loop is both internal and external (i.e.
            // there's only one contour loop).
            loop_role = elrInternal;
        } else {
            loop_role = elrDefault;
        }
        if (!loop.is_contour) {
            loop_role = (ExtrusionLoopRole)(loop_role | elrHole);
        }

        // detect overhanging/bridging perimeters
        bool need_detect_overhang = false;
        if (params.region_setting.has_many_config(&params.config.overhangs)) {
            need_detect_overhang = true;
        } else {
            const RegionSettings::SettingsValue& opt_values = params.region_setting.get_solo_config(&params.config.overhangs);
            need_detect_overhang = opt_values.get_bool(&params.config.overhangs);
                //&& (opt_values.is_enabled(&params.config.overhangs_width_speed) || opt_values.is_enabled(&params.config.overhangs_flow_ratio));
        }
        if (need_detect_overhang && params.layer->id() > 0
            && !(params.object_config.support_material && params.object_config.support_material_contact_distance_type.value == zdNone)) {
            ExtrusionPaths paths = this->create_overhangs_classic(params, initial_polyline, role, is_external);
            
            if (direction.length() > 0) {
                Polyline direction_polyline;
                for (ExtrusionPath &path : paths) {
                    if(direction_polyline.size() == 0 || direction_polyline.points.back() != path.first_point())
                        append(direction_polyline.points, path.polyline().to_polyline().points);
                }
                for (int i = 0; i < direction_polyline.points.size() - 1; i++)
                    assert(direction_polyline.points[i] != direction_polyline.points[i + 1]);
                if (direction_polyline.length() > params.perimeter_flow.scaled_width() / 8) {
                    direction_polyline.clip_start(params.perimeter_flow.scaled_width() / 20);
                    direction_polyline.clip_end(params.perimeter_flow.scaled_width() / 20);
                }
                coord_t dot = direction.dot(Line(direction_polyline.points.back(), direction_polyline.points.front()));
                need_to_reverse = dot>0;
            }
            if (need_to_reverse) {
                std::reverse(paths.begin(), paths.end());
            }
            //search for the first path
            size_t good_idx = 0; 
            for (size_t idx_path = 0; idx_path < paths.size(); idx_path++) {
                const ExtrusionPath &path = paths[idx_path];
                if (need_to_reverse) {
                    if (path.polyline().back().coincides_with_epsilon(initial_polyline.front())) {
                        good_idx = idx_path;
                        break;
                    }
                } else {
                    if (path.polyline().front().coincides_with_epsilon(initial_polyline.front())) {
                        good_idx = idx_path;
                        break;
                    }
                }
            }
            for (size_t idx_path = good_idx; idx_path < paths.size(); idx_path++) {
                ExtrusionPath &path = paths[idx_path];
                if (need_to_reverse) path.reverse();
                assert(!path.empty());
                my_loop.paths().push_back(path);
            }
            for (size_t idx_path = 0; idx_path < good_idx; idx_path++) {
                ExtrusionPath &path = paths[idx_path];
                if (need_to_reverse) path.reverse();
                assert(!path.empty());
                my_loop.paths().push_back(path);
            }
        } else {

            if (direction.length() > 0) {
                Polyline direction_polyline = initial_polyline;
                direction_polyline.clip_start(params.perimeter_flow.scaled_width() / 20);
                direction_polyline.clip_end(params.perimeter_flow.scaled_width() / 20);
                coord_t dot = direction.dot(Line(direction_polyline.back(), direction_polyline.front()));
                need_to_reverse = dot>0;
            }

            ExtrusionPath path(ExtrusionAttributes{role}, path.clone_properties(), false);
            path.polyline() = initial_polyline;
            if (need_to_reverse) path.polyline().reverse();
            path.attributes_mutable().mm3_per_mm = is_external ? params.ext_mm3_per_mm() : params.mm3_per_mm();
            path.attributes_mutable().width = is_external ? params.ext_perimeter_flow.width() : params.perimeter_flow.width();
            path.attributes_mutable().height = (float)(params.layer->unscaled_height());
            assert(!path.empty());
            my_loop.paths().push_back(path);
        }

    }
    
    return my_loop;
}

ExtrusionLoop PerimeterGenerator::_traverse_and_join_loops(const Parameters &             params,
                                                           const PerimeterGeneratorLoop & loop,
                                                           const PerimeterGeneratorLoops &children,
                                                           const Point                    entry_point) const
{
    //std::cout << " === ==== _traverse_and_join_loops ==== ===\n";
    // other perimeters
    //this->_mm3_per_mm = this->perimeter_flow.mm3_per_mm();
    //coord_t perimeter_width = this->perimeter_flow.scaled_width();
    const coord_t perimeter_spacing = params.perimeter_flow.scaled_spacing();

    //// external perimeters
    //this->_ext_mm3_per_mm = this->ext_perimeter_flow.mm3_per_mm();
    //coord_t params.get_ext_perimeter_width() = this->ext_perimeter_flow.scaled_width();
    //const coord_t ext_perimeter_spacing = this->ext_perimeter_flow.scaled_spacing();
    //coord_t ext_perimeter_spacing2 = this->ext_perimeter_flow.scaled_spacing(this->perimeter_flow);

    //const coord_t dist_cut = (coord_t)scale_(params.print_config.nozzle_diameter.get_at(params.config.perimeter_extruder - 1));
    //TODO change this->external_perimeter_flow.scaled_width() if it's the first one!
    const coord_t max_width_extrusion = params.perimeter_flow.scaled_width();
    ExtrusionLoop my_loop = _extrude_and_cut_loop(params, loop, entry_point, Line{ {0,0},{0,0} }, true);

    int child_idx = 0;
    //Polylines myPolylines = { myPolyline };
    //iterate on each point ot find the best place to go into the child
    PerimeterGeneratorLoops childs = children;
    while (!childs.empty()) {
        child_idx++;
        PerimeterIntersectionPoint nearest = this->_get_nearest_point(params, childs, my_loop, coord_t(params.perimeter_flow.scaled_width()), coord_t(params.perimeter_flow.scaled_width()* 1.42));
        if (nearest.idx_children == (size_t)-1) {
            //return ExtrusionEntityCollection();
            break;
        } else {
            const PerimeterGeneratorLoop &child = childs[nearest.idx_children];
            //std::cout << "c." << child_idx << " === i have " << my_loop.paths().size() << " paths" << " == cut_path_is_ccw size " << path_is_ccw.size() << "\n";
            //std::cout << "change to child " << nearest.idx_children << " @ " << unscale(nearest.outter_best.x) << ":" << unscale(nearest.outter_best.y)
            //    << ", idxpolyline = " << nearest.idx_polyline_outter << "\n";
            //PerimeterGeneratorLoops less_childs = childs;
            //less_childs.erase(less_childs.begin() + nearest.idx_children);
            //create new node with recursive ask for the inner perimeter & COPY of the points, ready to be cut
            assert(nearest.idx_polyline_outter >= 0);
            assert(nearest.idx_polyline_outter < my_loop.paths().size());

            ArcPolyline tosplit = std::move(my_loop.paths()[nearest.idx_polyline_outter].polyline());
            my_loop.paths()[nearest.idx_polyline_outter].polyline() = ArcPolyline();
            my_loop.paths().insert(my_loop.paths().begin() + nearest.idx_polyline_outter + 1, my_loop.paths()[nearest.idx_polyline_outter]);
            assert(nearest.idx_polyline_outter + 1 < my_loop.paths().size());

            // outer_start == outer_end
            ExtrusionPath *outer_start = &my_loop.paths()[nearest.idx_polyline_outter];
            ExtrusionPath *outer_end = &my_loop.paths()[nearest.idx_polyline_outter + 1];
            Line deletedSection;

            assert(outer_start->polyline().empty());
            assert(outer_end->polyline().empty());

            //cut our polyline, so outer_start has one common point with outer_end
            //separate them
            int nearest_idx_outter = outer_start->polyline().find_point(nearest.outter_best, SCALED_EPSILON);
            if (nearest_idx_outter >= 0) {
                tosplit.split_at_index(nearest_idx_outter, outer_start->polyline(), outer_end->polyline());
                assert(outer_end->empty() || outer_start->polyline().back() == outer_end->polyline().front());
            } else {
                tosplit.split_at(nearest.outter_best, outer_start->polyline(), outer_end->polyline());
                assert(outer_end->empty() || outer_start->polyline().back() == outer_end->polyline().front());
                if (outer_start->polyline().back() != nearest.outter_best) {
                    if (outer_start->polyline().back().coincides_with_epsilon(nearest.outter_best)) {
                        outer_start->polyline().set_back(nearest.outter_best);
                        if (!outer_end->empty())
                            outer_end->polyline().set_front(nearest.outter_best);
                    }
                } else {
                    outer_start->polyline().append(nearest.outter_best);
                    if (!outer_end->empty())
                        outer_end->polyline().append_before(nearest.outter_best);
                }
            }
            Polyline to_reduce = outer_start->polyline().to_polyline();
            if (to_reduce.size() > 1 && to_reduce.length() > (params.perimeter_flow.scaled_width() / 10)) to_reduce.clip_end(params.perimeter_flow.scaled_width() / 20);
            deletedSection.a = to_reduce.back();
            if (!outer_end->empty()) {
                to_reduce = outer_end->polyline().to_polyline();
            } else {
                outer_end = outer_start; // for outer_end_spacing
                assert(my_loop.paths()[nearest.idx_polyline_outter + 1].empty());
                my_loop.paths().erase(my_loop.paths().begin() + nearest.idx_polyline_outter + 1);
            }
            assert(nearest.idx_polyline_outter < my_loop.paths().size());
            if (to_reduce.size() > 1 && to_reduce.length() > (params.perimeter_flow.scaled_width() / 10)) to_reduce.clip_start(params.perimeter_flow.scaled_width() / 20);
            deletedSection.b = to_reduce.front();
            
            //get the inner loop to connect to us.
            ExtrusionLoop child_loop = _extrude_and_cut_loop(params, child, nearest.child_best, deletedSection);

            const coord_t inner_child_spacing = child.is_external() ? params.get_ext_perimeter_spacing() : params.get_perimeter_spacing();
            const coord_t outer_start_spacing = scale_i(outer_start->width() - outer_start->height() * (1. - 0.25 * PI));
            const coord_t outer_end_spacing = scale_i(outer_end->width() - outer_end->height() * (1. - 0.25 * PI));

            //FIXME: if child_loop has no point or 1 point or not enough space !!!!!!!
            const size_t child_paths_size = child_loop.paths().size();
            if (child_paths_size == 0) continue;
            my_loop.paths().insert(my_loop.paths().begin() + nearest.idx_polyline_outter + 1, child_loop.paths().begin(), child_loop.paths().end());
            assert(nearest.idx_polyline_outter + 1 < my_loop.paths().size());

            //add paths into my_loop => need to re-get the refs
            outer_start = &my_loop.paths()[nearest.idx_polyline_outter];
            const size_t next_idx_polyline_outter = (nearest.idx_polyline_outter + child_paths_size + 1) % my_loop.paths().size();
            assert(next_idx_polyline_outter >= 0);
            assert(next_idx_polyline_outter < my_loop.paths().size());;
            outer_end = &my_loop.paths()[next_idx_polyline_outter];
            ExtrusionPath *inner_start = &my_loop.paths()[nearest.idx_polyline_outter + 1];
            ExtrusionPath *inner_end = &my_loop.paths()[nearest.idx_polyline_outter + child_paths_size];
            //TRIM
            //choose trim direction
            if (outer_start->polyline().size() == 1 && outer_end->polyline().size() == 1) {
                //do nothing
            } else if (outer_start->polyline().size() == 1) {
                outer_end->polyline().clip_start(double(outer_end_spacing));
                if (inner_end->polyline().length() > inner_child_spacing)
                    inner_end->polyline().clip_end(double(inner_child_spacing));
                else
                    inner_end->polyline().clip_end(inner_end->polyline().length() / 2);
            } else if (outer_end->polyline().size() == 1) {
                outer_start->polyline().clip_end(double(outer_start_spacing));
                if (inner_start->polyline().length() > inner_child_spacing)
                    inner_start->polyline().clip_start(double(inner_child_spacing));
                else
                    inner_start->polyline().clip_start(inner_start->polyline().length()/2);
            } else {
                coord_t length_poly_1 = (coord_t)outer_start->polyline().length();
                coord_t length_poly_2 = (coord_t)outer_end->polyline().length();
                coord_t length_trim_1 = outer_start_spacing / 2;
                coord_t length_trim_2 = outer_end_spacing / 2;
                if (length_poly_1 < length_trim_1) {
                    length_trim_2 = length_trim_1 + length_trim_2 - length_poly_1;
                }
                if (length_poly_2 < length_trim_1) {
                    length_trim_1 = length_trim_1 + length_trim_2 - length_poly_2;
                }
                if (length_poly_1 > length_trim_1) {
                    outer_start->polyline().clip_end(double(length_trim_1));
                } else {
                    outer_start->polyline() = ArcPolyline(Points{outer_start->polyline().front()});
                    //outer_start->polyline().set_points().erase(outer_start->polyline().set_points().begin() + 1, outer_start->polyline().set_points().end());
                }
                if (length_poly_2 > length_trim_2) {
                    outer_end->polyline().clip_start(double(length_trim_2));
                } else {
                    outer_end->polyline() = ArcPolyline(Points{outer_end->polyline().back()});
                    //outer_end->polyline().set_points().erase(outer_end->polyline().set_points().begin(), outer_end->polyline().set_points().end() - 1);
                }
                
                length_poly_1 = coord_t(inner_start->polyline().length());
                length_poly_2 = coord_t(inner_end->polyline().length());
                length_trim_1 = inner_child_spacing / 2;
                length_trim_2 = inner_child_spacing / 2;
                if (length_poly_1 < length_trim_1) {
                    length_trim_2 = length_trim_1 + length_trim_2 - length_poly_1;
                }
                if (length_poly_2 < length_trim_1) {
                    length_trim_1 = length_trim_1 + length_trim_2 - length_poly_2;
                }
                if (length_poly_1 > length_trim_1) {
                    inner_start->polyline().clip_start(double(length_trim_1));
                } else {
                    inner_start->polyline() = ArcPolyline(Points{inner_start->polyline().back()});
                    //inner_start->polyline().set_points().erase(inner_start->polyline().set_points().begin(), inner_start->polyline().set_points().end() - 1);
                }
                if (length_poly_2 > length_trim_2) {
                    inner_end->polyline().clip_end(double(length_trim_2));
                } else {
                    inner_end->polyline() = ArcPolyline(Points{inner_end->polyline().front()});
                    //inner_end->polyline().set_points().erase(inner_end->polyline().set_points().begin() + 1, inner_end->polyline().set_points().end());
                }
            }

            //last check to see if we need a reverse
            {
                Line l1(outer_start->polyline().back(), inner_start->polyline().front());
                Line l2(inner_end->polyline().back(), outer_end->polyline().front());
                Point p_inter(0, 0);
                bool is_interect = l1.intersection(l2, &p_inter);
                if (is_interect && l1.distance_to(p_inter) < SCALED_EPSILON && l2.distance_to(p_inter) < SCALED_EPSILON) {
                    //intersection! need to reverse!
                    std::reverse(my_loop.paths().begin() + nearest.idx_polyline_outter + 1, my_loop.paths().begin() + nearest.idx_polyline_outter + child_paths_size + 1);
                    for (size_t idx = nearest.idx_polyline_outter + 1; idx < nearest.idx_polyline_outter + child_paths_size + 1; idx++) {
                        my_loop.paths()[idx].reverse();
                    }
                    outer_start = &my_loop.paths()[nearest.idx_polyline_outter];
                    inner_start = &my_loop.paths()[nearest.idx_polyline_outter + 1];
                    inner_end = &my_loop.paths()[nearest.idx_polyline_outter + child_paths_size];
                    outer_end = &my_loop.paths()[next_idx_polyline_outter];
                }

            }

            //now add extrusionPAths to connect the two loops
            ExtrusionPaths travel_path_begin;// ( ExtrusionRole::Travel, 0, outer_start->width, outer_start->height);
            //travel_path_begin.extruder_id = -1;
            ExtrusionPaths travel_path_end;// ( ExtrusionRole::Travel, 0, outer_end->width, outer_end->height);
            //travel_path_end.extruder_id = -1;
            double dist_travel = outer_start->polyline().back().distance_to(inner_start->polyline().front());
            if (dist_travel > max_width_extrusion*1.5 && params.config.fill_density.value > 0) {
                travel_path_begin.emplace_back( ExtrusionAttributes(ExtrusionRole::Perimeter, ExtrusionFlow(outer_start->mm3_per_mm(), outer_start->width(), outer_start->height())), nullptr, false);
                travel_path_begin.emplace_back( ExtrusionAttributes(ExtrusionRole::Travel, ExtrusionFlow(0, outer_start->width()/10, outer_start->height())), nullptr, false);
                travel_path_begin.emplace_back( ExtrusionAttributes(ExtrusionRole::Perimeter, ExtrusionFlow(outer_start->mm3_per_mm(), outer_start->width(), outer_start->height())), nullptr, false);
                //travel_path_begin[0].extruder_id = -1;
                //travel_path_begin[1].extruder_id = -1;
                //travel_path_begin[2].extruder_id = -1;
                Line line(outer_start->polyline().back(), inner_start->polyline().front());
                Point p_dist_cut_extrude = (line.b - line.a);
                p_dist_cut_extrude.x() = (coord_t)(p_dist_cut_extrude.x() * ((double)max_width_extrusion) / (line.length() * 2));
                p_dist_cut_extrude.y() = (coord_t)(p_dist_cut_extrude.y() * ((double)max_width_extrusion) / (line.length() * 2));
                //extrude a bit after the turn, to close the loop
                Point p_start_travel = line.a;
                p_start_travel += p_dist_cut_extrude;
                travel_path_begin[0].polyline().append(outer_start->polyline().back());
                travel_path_begin[0].polyline().append(p_start_travel);
                //extrude a bit before the final turn, to close the loop
                Point p_end_travel = line.b;
                p_end_travel -= p_dist_cut_extrude;
                travel_path_begin[2].polyline().append(p_end_travel);
                travel_path_begin[2].polyline().append(inner_start->polyline().front());
                //fake travel in the middle
                travel_path_begin[1].polyline().append(p_start_travel);
                travel_path_begin[1].polyline().append(p_end_travel);
            } else {
                // the path is small enough to extrude all along.
                double flow_mult = 1;
                if (dist_travel > max_width_extrusion && params.config.fill_density.value > 0) {
                    // the path is a bit too long, reduce the extrusion flow.
                    flow_mult = max_width_extrusion / dist_travel;
                }
                travel_path_begin.emplace_back( ExtrusionAttributes(ExtrusionRole::Perimeter, ExtrusionFlow(outer_start->mm3_per_mm() * flow_mult, (float)(outer_start->width() * flow_mult), outer_start->height())), nullptr, false);
                //travel_path_begin[0].extruder_id = -1;
                travel_path_begin[0].polyline().append(outer_start->polyline().back());
                travel_path_begin[0].polyline().append(inner_start->polyline().front());
            }
            dist_travel = inner_end->polyline().back().distance_to(outer_end->polyline().front());
            if (dist_travel > max_width_extrusion*1.5 && params.config.fill_density.value > 0) {
                travel_path_end.emplace_back( ExtrusionAttributes(ExtrusionRole::Perimeter, ExtrusionFlow(outer_end->mm3_per_mm(), outer_end->width(), outer_end->height())), nullptr, false);
                travel_path_end.emplace_back( ExtrusionAttributes(ExtrusionRole::Travel, ExtrusionFlow(0, outer_end->width()/10, outer_end->height())), nullptr, false);
                travel_path_end.emplace_back( ExtrusionAttributes(ExtrusionRole::Perimeter, ExtrusionFlow(outer_end->mm3_per_mm(), outer_end->width(), outer_end->height())), nullptr, false);
                //travel_path_end[0].extruder_id = -1;
                //travel_path_end[1].extruder_id = -1;
                //travel_path_end[2].extruder_id = -1;
                Line line(inner_end->polyline().back(), outer_end->polyline().front());
                Point p_dist_cut_extrude = (line.b - line.a);
                p_dist_cut_extrude.x() = (coord_t)(p_dist_cut_extrude.x() * ((double)max_width_extrusion) / (line.length() * 2));
                p_dist_cut_extrude.y() = (coord_t)(p_dist_cut_extrude.y() * ((double)max_width_extrusion) / (line.length() * 2));
                //extrude a bit after the turn, to close the loop
                Point p_start_travel_2 = line.a;
                p_start_travel_2 += p_dist_cut_extrude;
                travel_path_end[0].polyline().append(inner_end->polyline().back());
                travel_path_end[0].polyline().append(p_start_travel_2);
                //extrude a bit before the final turn, to close the loop
                Point p_end_travel_2 = line.b;
                p_end_travel_2 -= p_dist_cut_extrude;
                travel_path_end[2].polyline().append(p_end_travel_2);
                travel_path_end[2].polyline().append(outer_end->polyline().front());
                //fake travel in the middle
                travel_path_end[1].polyline().append(p_start_travel_2);
                travel_path_end[1].polyline().append(p_end_travel_2);
            } else {
                // the path is small enough to extrude all along.
                double flow_mult = 1;
                if (dist_travel > max_width_extrusion && params.config.fill_density.value > 0) {
                    // the path is a bit too long, reduce the extrusion flow.
                    flow_mult = max_width_extrusion / dist_travel;
                }
                travel_path_end.emplace_back( ExtrusionAttributes(ExtrusionRole::Perimeter, ExtrusionFlow(outer_end->mm3_per_mm() * flow_mult, (float)(outer_end->width() * flow_mult), outer_end->height())), nullptr, false);
                //travel_path_end[0].extruder_id = -1;
                travel_path_end[0].polyline().append(inner_end->polyline().back());
                travel_path_end[0].polyline().append(outer_end->polyline().front());
            }
            //check if we add path or reuse bits
            //FIXME
            /*if (outer_start->polyline().points.size() == 1) {
                outer_start->polyline() = travel_path_begin.front().polyline();
                travel_path_begin.erase(travel_path_begin.begin());
                outer_start->extruder_id = -1;
            } else if (outer_end->polyline().points.size() == 1) {
                outer_end->polyline() = travel_path_end.back().polyline();
                travel_path_end.erase(travel_path_end.end() - 1);
                outer_end->extruder_id = -1;
            }*/
            //add paths into my_loop => after that all ref are wrong!
            for (size_t i = travel_path_end.size() - 1; i < travel_path_end.size(); i--) {
                my_loop.paths().insert(my_loop.paths().begin() + nearest.idx_polyline_outter + child_paths_size + 1, travel_path_end[i]);
            }
            for (size_t i = travel_path_begin.size() - 1; i < travel_path_begin.size(); i--) {
                my_loop.paths().insert(my_loop.paths().begin() + nearest.idx_polyline_outter + 1, travel_path_begin[i]);
            }
        }
        //remove one-point extrusion
        //FIXME prevent this instead of patching here?
        for (size_t i = 0; i < my_loop.paths().size(); i++) {
            if (my_loop.paths()[i].polyline().size() < 2) {
                if (my_loop.paths()[i].polyline().size() == 1)
                    BOOST_LOG_TRIVIAL(warning) << "erase one-point extrusion : layer " << params.layer->id() << " " << my_loop.paths()[i].polyline().front().x() << ":" << my_loop.paths()[i].polyline().front().y() << "\n";
                my_loop.paths().erase(my_loop.paths().begin() + i);
                i--;
            }
        }

        //update for next loop
        childs.erase(childs.begin() + nearest.idx_children);
    }

    return my_loop;
}

coord_t PerimeterGenerator::get_resolution(size_t perimeter_id, bool is_overhang) const
{
    coord_t reso = scale_i(params.print_config.resolution.value);
    if (reso == 0) reso = SCALED_EPSILON;
    return reso;
    //deactivated because with full perimeter on tube, the innermost perimeter can be very rough, and not a circle anymore.
    //if on top or bottom, use external resolution.
    //if (is_overhang || perimeter_id == 0)
    //    return reso;
    //if(srf && srf->has_pos_top())
    //    return reso;
    //// for each perimeter, reduce the precision by a factor 3
    //int mult = (int)std::pow(2, perimeter_id);
    //coord_t reso_internal = scale_i(params.print_config.resolution_internal.value);
    //if(reso_internal < reso * mult)
    //    return reso_internal;
    //return reso * mult;
}

const std::vector<t_config_option_keys> Parameters::perimeter_keys({
    {"extra_perimeters_below_area"},
    {"extra_perimeters_count"},
    {"extra_perimeters_odd_layers"},
    {"extra_perimeters_on_overhangs"},
    {"only_one_perimeter_top", "min_width_top_surface", "only_one_perimeter_top_other_algo"},
    {"thin_walls", "thin_walls_min_width", "thin_walls_overlap"},
    {"overhangs_speed_enforce"},
    {"overhangs", "overhangs_speed", "overhangs_width_speed", "overhangs_flow_ratio", "overhangs_width"},
    {"gap_fill_enabled"},
    {"gap_fill_no_overhang"},
    {"seam_slope_type", "external_perimeters_first", "external_perimeters_first_force", "external_perimeters_nothole", "external_perimeters_hole"},
    });

//TODO: fuse surface's extra_perimeter with extra_perimeters_count
// to be acalled after segregate_regions because segregate regions clear the storage.
void Parameters::segregate_extra_perimeters(const ExPolygon &my_srf, const LayerRegionSetCPtrs &lregions) {
    BoundingBox my_srf_bb(my_srf.contour.points);
    my_srf_bb.offset(SCALED_EPSILON * 3);
    std::map<uint16_t, ExPolygons> extra_peri_to_areas;
    std::map<RegionSettings::SettingsValue, RegionSettings::ClipExpoly> &opt_2_areas =
        this->region_setting.create_extra_setting(&Parameters::OPT_EXTRA_PERIMETER);
    // check if it needs to be added
    bool many_values = false;
    std::map<uint16_t, std::vector<const Surface *>> srf_per_extra_peri;
    if (!lregions.empty()) {
        for (const LayerRegion *region : lregions) {
            for (const Surface &srf : region->slices()) {
                srf_per_extra_peri[srf.extra_perimeters].push_back(&srf);
            }
        }
    }
    if (srf_per_extra_peri.size() > 1) {
        for (auto &entry : srf_per_extra_peri) {
            ExPolygons &areas = extra_peri_to_areas[entry.first];
            for (const Surface *surface : entry.second) {
                BoundingBox test_bb(surface->expolygon.contour.points);
                if (test_bb.overlap(my_srf_bb) /*&& surface.expolygon.overlaps(my_srf)done by clip*/) {
                    append(areas, offset_ex(ClipperUtils::clip_clipper_polygons_with_subject_bbox(surface->expolygon, my_srf_bb),
                                     SCALED_EPSILON * 10));
                }
            }
            if (areas.empty()) {
                extra_peri_to_areas.erase(entry.first);
            } else {
                // union the surfaces and un-offset them.
                areas = offset_ex(union_ex(areas), -SCALED_EPSILON * 10);
            }
        }
    }
    if (extra_peri_to_areas.size() > 1) {
        for (auto &entry : extra_peri_to_areas) {
            extra_perimeters_storage.emplace_back((int32_t) entry.first);
            RegionSettings::SettingsValue settings_group_key = RegionSettings::SettingsValue::create({&Parameters::OPT_EXTRA_PERIMETER},
                                                                                 {&extra_perimeters_storage.back()});
            RegionSettings::ClipExpoly &clipexpoly = opt_2_areas[settings_group_key];
            clipexpoly.expolys = std::move(entry.second);
            clipexpoly.compute_bb();
        }
    } else if (!srf_per_extra_peri.empty()) {
        // only one value for everything
        extra_perimeters_storage.emplace_back((int32_t) (srf_per_extra_peri.begin()->first));
        opt_2_areas[RegionSettings::SettingsValue::create({&Parameters::OPT_EXTRA_PERIMETER},
                                                      {&extra_perimeters_storage.back()})] = {};
    } else {
        // no LayerRegion has any surface, so whatever.
        extra_perimeters_storage.emplace_back(0);
        opt_2_areas[RegionSettings::SettingsValue::create({&Parameters::OPT_EXTRA_PERIMETER},
                                                        {&extra_perimeters_storage.back()})] = {};
    }
    assert(!opt_2_areas.empty());
}

}
