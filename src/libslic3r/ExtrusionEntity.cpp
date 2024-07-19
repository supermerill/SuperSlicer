///|/ Copyright (c) Prusa Research 2016 - 2023 Vojtěch Bubník @bubnikv, Lukáš Matěna @lukasmatena, Enrico Turri @enricoturri1966
///|/ Copyright (c) SuperSlicer 2023 Remi Durand @supermerill
///|/ Copyright (c) Slic3r 2013 - 2016 Alessandro Ranellucci @alranel
///|/ Copyright (c) 2014 Petr Ledvina @ledvinap
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "ExPolygon.hpp"
#include "ClipperUtils.hpp"
#include "Config.hpp"
#include "Exception.hpp"
#include "Extruder.hpp"
#include "Flow.hpp"
#include <cmath>
#include <limits>
#include <sstream>

namespace Slic3r {

//// extrusion entity visitor
void ExtrusionVisitor::use(ExtrusionPath &path) { default_use(path); };
void ExtrusionVisitor::use(ExtrusionPath3D &path3D) { default_use(path3D); }
void ExtrusionVisitor::use(ExtrusionMultiPath &multipath) { default_use(multipath); }
void ExtrusionVisitor::use(ExtrusionMultiPath3D &multipath3D) { default_use(multipath3D); }
void ExtrusionVisitor::use(ExtrusionLoop &loop) { default_use(loop); }
void ExtrusionVisitor::use(ExtrusionEntityCollection &collection) { default_use(collection); }

void ExtrusionVisitorConst::use(const ExtrusionPath &path) { default_use(path); }
void ExtrusionVisitorConst::use(const ExtrusionPath3D &path3D) { default_use(path3D); }
void ExtrusionVisitorConst::use(const ExtrusionMultiPath &multipath) { default_use(multipath); }
void ExtrusionVisitorConst::use(const ExtrusionMultiPath3D &multipath3D) { default_use(multipath3D); }
void ExtrusionVisitorConst::use(const ExtrusionLoop &loop) { default_use(loop); }
void ExtrusionVisitorConst::use(const ExtrusionEntityCollection &collection) { default_use(collection); }

void ExtrusionPath::intersect_expolygons(const ExPolygons &collection, ExtrusionEntityCollection *retval) const
{
    this->_inflate_collection(intersection_pl(Polylines{this->polyline.to_polyline()}, collection), retval);
}

void ExtrusionPath::subtract_expolygons(const ExPolygons &collection, ExtrusionEntityCollection *retval) const
{
    this->_inflate_collection(diff_pl(Polylines{this->polyline.to_polyline()}, collection), retval);
}

void ExtrusionPath::clip_end(coordf_t distance) { this->polyline.clip_end(distance); }

void ExtrusionPath::simplify(coordf_t tolerance, ArcFittingType with_fitting_arc, double fitting_arc_tolerance)
{
    if (with_fitting_arc != ArcFittingType::Disabled) {
        if (role().is_sparse_infill())
            // Use 3x lower resolution than the object fine detail for sparse infill.
            tolerance *= 3.;
        else if (role().is_support())
            // Use 4x lower resolution than the object fine detail for support.
            tolerance *= 4.;
        else if (role().is_skirt())
            // Brim is currently marked as skirt.
            // Use 4x lower resolution than the object fine detail for skirt & brim.
            tolerance *= 4.;
    }
    this->polyline.simplify(tolerance, with_fitting_arc, fitting_arc_tolerance);
}

void ExtrusionPath3D::simplify(coordf_t tolerance, ArcFittingType with_fitting_arc, double fitting_arc_tolerance)
{
    this->polyline.simplify(tolerance, ArcFittingType::Disabled, fitting_arc_tolerance);
    // TODO: simplify but only for sub-path with same zheight.
    // if (with_fitting_arc) {
    //    this->polyline.simplify(tolerance, with_fitting_arc, fitting_arc_tolerance);
    //}
}

double ExtrusionPath::length() const { return this->polyline.length(); }

void ExtrusionPath::_inflate_collection(const Polylines &polylines, ExtrusionEntityCollection *collection) const
{
    ExtrusionEntitiesPtr to_add;
    for (const Polyline &polyline : polylines)
        to_add.push_back(new ExtrusionPath(ArcPolyline{polyline}, this->attributes(), this->can_reverse()));
    collection->append(std::move(to_add));
}

void ExtrusionPath::polygons_covered_by_width(Polygons &out, const float scaled_epsilon) const
{
    polygons_append(out, offset(this->polyline.to_polyline(), double(scale_(m_attributes.width / 2)) + scaled_epsilon));
}

void ExtrusionPath::polygons_covered_by_spacing(Polygons &out, const float spacing_ratio, const float scaled_epsilon) const
{
    // Instantiating the Flow class to get the line spacing.
    // Don't know the nozzle diameter, setting to zero. It shall not matter it shall be optimized out by the compiler.
    bool bridge = this->role().is_bridge() || (this->width() * 4 < this->height());
    assert(!bridge || m_attributes.width == m_attributes.height);
    // TODO: check BRIDGE_FLOW here
    auto flow = bridge ? Flow::bridging_flow(m_attributes.width, 0.f) :
                         Flow::new_from_width(m_attributes.width, 0.f, m_attributes.height, spacing_ratio);
    polygons_append(out, offset(this->polyline.to_polyline(), 0.5f * float(flow.scaled_spacing()) + scaled_epsilon));
}

//note: don't suppport arc
double ExtrusionLoop::area() const
{
    double a = 0;
    for (const ExtrusionPath &path : this->paths) {
        assert(path.size() >= 2);
        if (path.size() >= 2) {
            // Assumming that the last point of one path segment is repeated at the start of the following path segment.
            Point prev = path.polyline.front();
            for (size_t idx = 1; idx < path.polyline.size(); ++idx) {
                const Point &curr = path.polyline.get_point(idx);
                a += cross2(prev.cast<double>(), curr.cast<double>());
                prev = curr;
            }
        }
    }
    return a * 0.5;
}

void ExtrusionLoop::reverse()
{
    for (ExtrusionPath &path : this->paths)
        path.reverse();
    std::reverse(this->paths.begin(), this->paths.end());
}

Polygon ExtrusionLoop::polygon() const
{
    Polygon polygon;
    for (const ExtrusionPath &path : this->paths) {
        // for each polyline, append all points except the last one (because it coincides with the first one of the next polyline)
        Polyline poly = path.polyline.to_polyline();
        polygon.points.insert(polygon.points.end(), poly.begin(), poly.end() - 1);
    }
    return polygon;
}

ArcPolyline ExtrusionLoop::as_polyline() const
{
    ArcPolyline polyline;
    for (const ExtrusionPath &path : this->paths) {
        polyline.append(path.as_polyline());
    }
    return polyline;
}

double ExtrusionLoop::length() const
{
    double len = 0;
    for (const ExtrusionPath &path : this->paths)
        len += path.polyline.length();
    return len;
}

ExtrusionRole ExtrusionLoop::role() const
{
    if (this->paths.empty())
        return ExtrusionRole::None;
    ExtrusionRole role = this->paths.front().role();
    for (const ExtrusionPath &path : this->paths)
        if (role != path.role()) {
            return ExtrusionRole::Mixed;
        }
    return role;
}

bool ExtrusionLoop::split_at_vertex(const Point &point, const double scaled_epsilon)
{
    for (ExtrusionPaths::iterator path = this->paths.begin(); path != this->paths.end(); ++path) {
        if (int idx = path->polyline.find_point(point, scaled_epsilon); idx != -1) {
            if (this->paths.size() == 1) {
                // just change the order of points
                ArcPolyline p1, p2;
                path->polyline.split_at_index(idx, p1, p2);
                if (p1.is_valid() && p2.is_valid()) {
                    p2.append(std::move(p1));
                    path->polyline.swap(p2); // swap points & fitting result
                }
            } else if (idx > 0) {
                if (idx < path->size() - 1) {
                    // new paths list starts with the second half of current path
                    ExtrusionPaths new_paths;
                    ArcPolyline p1, p2;
                    path->polyline.split_at_index(idx, p1, p2);
                    new_paths.reserve(this->paths.size() + 1);
                    {
                        ExtrusionPath p = *path;
                        p.polyline.swap(p2);
                        if (p.polyline.is_valid())
                            new_paths.push_back(p);
                    }

                    // then we add all paths until the end of current path list
                    new_paths.insert(new_paths.end(), path + 1, this->paths.end()); // not including this path

                    // then we add all paths since the beginning of current list up to the previous one
                    new_paths.insert(new_paths.end(), this->paths.begin(), path); // not including this path

                    // finally we add the first half of current path
                    {
                        ExtrusionPath p = *path;
                        p.polyline.swap(p1);
                        if (p.polyline.is_valid())
                            new_paths.push_back(p);
                    }
                    // we can now override the old path list with the new one and stop looping
                    this->paths = std::move(new_paths);
                } else {
                    // last point
                    assert((path)->last_point().coincides_with_epsilon(point));
                    assert((path + 1)->first_point().coincides_with_epsilon(point));
                    ExtrusionPaths new_paths;
                    new_paths.reserve(this->paths.size());
                    // then we add all paths until the end of current path list
                    new_paths.insert(new_paths.end(), path + 1, this->paths.end()); // not including this path
                    // then we add all paths since the beginning of current list up to the previous one
                    new_paths.insert(new_paths.end(), this->paths.begin(), path + 1); // including this path
                    // we can now override the old path list with the new one and stop looping
                    this->paths = std::move(new_paths);
                }
            } else {
                // else first point ->
                // if first path - nothign to change.
                // else, then impossible as it's also the last point of the previous path.
                assert(path == this->paths.begin());
                assert(path->first_point().coincides_with_epsilon(point));
            }
            return true;
        }
    }
    // The point was not found.
    return false;
}

ExtrusionLoop::ClosestPathPoint ExtrusionLoop::get_closest_path_and_point(const Point &point, bool prefer_non_overhang) const
{
    // Find the closest path and closest point belonging to that path. Avoid overhangs, if asked for.
    ClosestPathPoint out{0, 0};
    double           min2 = std::numeric_limits<double>::max();
    ClosestPathPoint best_non_overhang{0, 0};
    double           min2_non_overhang = std::numeric_limits<double>::max();
    for (const ExtrusionPath &path : this->paths) {
        std::pair<int, Point> foot_pt_ = path.polyline.foot_pt(point);
        double                d2       = (foot_pt_.second - point).cast<double>().squaredNorm();
        if (d2 < min2) {
            out.foot_pt     = foot_pt_.second;
            out.path_idx    = &path - &this->paths.front();
            out.segment_idx = foot_pt_.first;
            min2            = d2;
        }
        if (prefer_non_overhang && !path.role().is_bridge() && d2 < min2_non_overhang) {
            best_non_overhang.foot_pt     = foot_pt_.second;
            best_non_overhang.path_idx    = &path - &this->paths.front();
            best_non_overhang.segment_idx = foot_pt_.first;
            min2_non_overhang             = d2;
        }
    }
    if (prefer_non_overhang && min2_non_overhang != std::numeric_limits<double>::max()) {
        // Only apply the non-overhang point if there is one.
        out = best_non_overhang;
    }
    return out;
}

// Splitting an extrusion loop, possibly made of multiple segments, some of the segments may be bridging.
void ExtrusionLoop::split_at(const Point &point, bool prefer_non_overhang, const double scaled_epsilon)
{
    if (this->paths.empty())
        return;
    ExtrusionLoop::ClosestPathPoint close_p = get_closest_path_and_point(point, prefer_non_overhang);
    // Snap p to start or end of segment_idx if closer than scaled_epsilon.
    {
        const Point p1 = this->paths[close_p.path_idx].polyline.get_point(close_p.segment_idx);
        const Point  p2   = this->paths[close_p.path_idx].polyline.get_point(close_p.segment_idx + 1);
        // Use close_p.foot_pt instead of point for the comparison, as it's the one that will be used.
        double       d2_1 = (close_p.foot_pt - p1).cast<double>().squaredNorm();
        double       d2_2 = (close_p.foot_pt - p2).cast<double>().squaredNorm();
        const double thr2 = scaled_epsilon * scaled_epsilon;
        if (d2_1 < d2_2) {
            if (d2_1 < thr2)
                close_p.foot_pt = p1;
        } else {
            if (d2_2 < thr2)
                close_p.foot_pt = p2;
        }
    }

    // now split path_idx in two parts
    const ExtrusionPath &path = this->paths[close_p.path_idx];
    ExtrusionPath        p1(path.attributes(), can_reverse());
    ExtrusionPath        p2(path.attributes(), can_reverse());
    path.polyline.split_at(close_p.foot_pt, p1.polyline, p2.polyline);

    if (this->paths.size() == 1) {
        if (!p1.polyline.is_valid()) {
            this->paths.front().polyline.swap(p2.polyline);
        } else if (!p2.polyline.is_valid()) {
            this->paths.front().polyline.swap(p1.polyline);
        } else {
            p2.polyline.append(std::move(p1.polyline));
            this->paths.front().polyline.swap(p2.polyline);
        }
    } else {
        // erase the old path
        this->paths.erase(this->paths.begin() + close_p.path_idx);
        // install the two paths
        if (p2.polyline.is_valid() && p2.polyline.length() > 0)
            this->paths.insert(this->paths.begin() + close_p.path_idx, p2);
        if (p1.polyline.is_valid() && p1.polyline.length() > 0)
            this->paths.insert(this->paths.begin() + close_p.path_idx, p1);
        // split at the new vertex
        this->split_at_vertex(close_p.foot_pt, 0.);
    }
    // check if it's doing its job.
#ifdef _DEBUG
    Point last_pt = this->last_point();
    for (const ExtrusionPath &path : paths) {
        assert(last_pt == path.first_point());
        for (int i = 1; i < path.polyline.size(); ++i)
            assert(!path.polyline.get_point(i - 1).coincides_with_epsilon(path.polyline.get_point(i)));
        last_pt = path.last_point();
    }
    assert(close_p.foot_pt == this->first_point());
#endif
}

ExtrusionPaths clip_end(ExtrusionPaths &paths, coordf_t distance)
{
    ExtrusionPaths removed;

    while (distance > 0 && !paths.empty()) {
        ExtrusionPath &last = paths.back();
        removed.push_back(last);
        double len = last.length();
        if (len <= distance) {
            paths.pop_back();
            distance -= len;
        } else {
            last.polyline.clip_end(distance);
            removed.back().polyline.clip_start(removed.back().polyline.length() - distance);
            break;
        }
    }
    std::reverse(removed.begin(), removed.end());
    return removed;
}

//bool ExtrusionLoop::has_overhang_point(const Point &point) const
//{
//    for (const ExtrusionPath &path : this->paths) {
//        int pos = path.polyline.find_point(point);
//        if (pos != -1) {
//            // point belongs to this path
//            // we consider it overhang only if it's not an endpoint
//            return (path.role().is_bridge() && pos > 0 && pos != int(path.polyline.size()) - 1);
//        }
//    }
//    return false;
//}

void ExtrusionLoop::polygons_covered_by_width(Polygons &out, const float scaled_epsilon) const
{
    for (const ExtrusionPath &path : this->paths)
        path.polygons_covered_by_width(out, scaled_epsilon);
}

void ExtrusionLoop::polygons_covered_by_spacing(Polygons &out, const float spacing_ratio, const float scaled_epsilon) const
{
    for (const ExtrusionPath &path : this->paths)
        path.polygons_covered_by_spacing(out, spacing_ratio, scaled_epsilon);
}

//TODO del
//double ExtrusionLoop::min_mm3_per_mm() const
//{
//    double min_mm3_per_mm = std::numeric_limits<double>::max();
//    for (const ExtrusionPath &path : this->paths)
//        min_mm3_per_mm = std::min(min_mm3_per_mm, path.min_mm3_per_mm());
//    return min_mm3_per_mm;
//}

void ExtrusionPrinter::use(const ExtrusionPath &path)
{

    ss << (json?"\"":"") << "ExtrusionPath" << (path.can_reverse()?"":"Oriented") << (json?"_":":") << role_to_code(path.role()) << (json?"\":":"") << "[";
    for (int i = 0; i < path.polyline.size(); i++) {
        if (i != 0)
            ss << ",";
        double x = (mult * (path.polyline.get_point(i).x()));
        double y = (mult * (path.polyline.get_point(i).y()));
        ss << std::fixed << "["<<(trunc>0?(int(x*trunc))/double(trunc):x) << "," << (trunc>0?(int(y*trunc))/double(trunc):y) <<"]";
    }
    ss << "]";
}
void ExtrusionPrinter::use(const ExtrusionPath3D &path3D)
{
    ss << (json?"\"":"") << "ExtrusionPath3D" << (path3D.can_reverse()?"":"Oriented") << (json?"_":":") << role_to_code(path3D.role()) << (json?"\":":"") << "[";
    for (int i = 0; i < path3D.polyline.size(); i++) {
        if (i != 0)
            ss << ",";
        double x = (mult * (path3D.polyline.get_point(i).x()));
        double y = (mult * (path3D.polyline.get_point(i).y()));
        double z = (path3D.z_offsets.size() > i ? mult * (path3D.z_offsets[i]) : -1);
        ss << std::fixed << "[" << (trunc>0?(int(x*trunc))/double(trunc):x) << "," << (trunc>0?(int(y*trunc))/double(trunc):y) << "," << (trunc>0?(int(z*trunc))/double(trunc):z) << "]";
    }
    ss << "]";
}
void ExtrusionPrinter::use(const ExtrusionMultiPath &multipath)
{
    ss << (json?"\"":"") << "ExtrusionMultiPath" << (multipath.can_reverse()?"":"Oriented") << (json?"_":":") << role_to_code(multipath.role()) << (json?"\":":"") << "{";
    for (int i = 0; i < multipath.paths.size(); i++) {
        if (i != 0)
            ss << ",";
        multipath.paths[i].visit(*this);
    }
    ss << "}";
}
void ExtrusionPrinter::use(const ExtrusionMultiPath3D &multipath3D)
{
    ss << (json?"\"":"") << "multipath3D" << (multipath3D.can_reverse()?"":"Oriented") << (json?"_":":") << role_to_code(multipath3D.role()) << (json?"\":":"") << "{";
    for (int i = 0; i < multipath3D.paths.size(); i++) {
        if (i != 0)
            ss << ",";
        multipath3D.paths[i].visit(*this);
    }
    ss << "}";
}
void ExtrusionPrinter::use(const ExtrusionLoop &loop)
{ 
    ss << (json?"\"":"") << "ExtrusionLoop" << (json?"_":":") << role_to_code(loop.role())<<"_" << looprole_to_code(loop.loop_role()) << (json?"\":":"") << "{";
    if(!loop.can_reverse()) ss << (json?"\"":"") << "oriented" << (json?"\":":"=") << "true,";
    for (int i = 0; i < loop.paths.size(); i++) {
        if (i != 0)
            ss << ",";
        loop.paths[i].visit(*this);
    }
    ss << "}";
}
void ExtrusionPrinter::use(const ExtrusionEntityCollection &collection)
{
    ss << (json?"\"":"") << "ExtrusionEntityCollection" << (json?"_":":") << role_to_code(collection.role()) << (json?"\":":"") << "{";
    if(!collection.can_sort()) ss << (json?"\"":"") << "no_sort" << (json?"\":":"=") << "true,";
    if(!collection.can_reverse()) ss << (json?"\"":"") << "oriented" << (json?"\":":"=") << "true,";
    for (int i = 0; i < collection.entities().size(); i++) {
        if (i != 0)
            ss << ",";
        collection.entities()[i]->visit(*this);
    }
    ss << "}";
}

void ExtrusionLength::default_use(const ExtrusionEntity &entity) { dist += entity.length(); };
void ExtrusionLength::use(const ExtrusionEntityCollection &collection)
{
    for (int i = 0; i < collection.entities().size(); i++) {
        collection.entities()[i]->visit(*this);
    }
}

double ExtrusionVolume::get(const ExtrusionEntityCollection &coll) {
    for (const ExtrusionEntity *entity : coll.entities()) entity->visit(*this);
    return volume;
}

void ExtrusionModifyFlow::set(ExtrusionEntityCollection &coll) {
    for (ExtrusionEntity *entity : coll.entities()) entity->visit(*this);
}

void ExtrusionVisitorRecursiveConst::use(const ExtrusionMultiPath& multipath) {
    for (const ExtrusionPath &path : multipath.paths) {
        path.visit(*this);
    }
}
void ExtrusionVisitorRecursiveConst::use(const ExtrusionMultiPath3D &multipath3D)
{
    for (const ExtrusionPath3D &path3D : multipath3D.paths) {
        path3D.visit(*this);
    }
}
void ExtrusionVisitorRecursiveConst::use(const ExtrusionLoop &loop)
{
    for (const ExtrusionPath &path : loop.paths) {
        path.visit(*this);
    }
}
void ExtrusionVisitorRecursiveConst::use(const ExtrusionEntityCollection &collection)
{
    for (const ExtrusionEntity *entity : collection.entities()) {
        entity->visit(*this);
    }
}
void ExtrusionVisitorRecursive::use(ExtrusionMultiPath &multipath)
{
    for (ExtrusionPath &path : multipath.paths) {
        path.visit(*this);
    }
}
void ExtrusionVisitorRecursive::use(ExtrusionMultiPath3D &multipath3D)
{
    for (ExtrusionPath3D &path3D : multipath3D.paths) {
        path3D.visit(*this);
    }
}
void ExtrusionVisitorRecursive::use(ExtrusionLoop &loop)
{
    for (ExtrusionPath &path : loop.paths) {
        path.visit(*this);
    }
}
void ExtrusionVisitorRecursive::use(ExtrusionEntityCollection &collection)
{
    for (ExtrusionEntity *entity : collection.entities()) {
        entity->visit(*this);
    }
}

void HasRoleVisitor::use(const ExtrusionMultiPath& multipath) {
    for (const ExtrusionPath& path : multipath.paths) {
        path.visit(*this);
        if(found) return;
    }
}
void HasRoleVisitor::use(const ExtrusionMultiPath3D& multipath3D) {
    for (const ExtrusionPath3D& path3D : multipath3D.paths) {
        path3D.visit(*this);
        if(found) return;
    }
}
void HasRoleVisitor::use(const ExtrusionLoop& loop) {
    for (const ExtrusionPath& path : loop.paths) {
        path.visit(*this);
        if(found) return;
    }
}
void HasRoleVisitor::use(const ExtrusionEntityCollection& collection) {
    for (const ExtrusionEntity* entity : collection.entities()) {
        entity->visit(*this);
        if(found) return;
    }
}
bool HasRoleVisitor::search(const ExtrusionEntity &entity, HasRoleVisitor&& visitor) {
    entity.visit(visitor);
    return visitor.found;
}
bool HasRoleVisitor::search(const ExtrusionEntitiesPtr &entities, HasRoleVisitor&& visitor) {
    for (ExtrusionEntity *ptr : entities) {
        ptr->visit(visitor);
        if (visitor.found) return true;
    }
    return visitor.found;
}

void SimplifyVisitor::use(ExtrusionPath& path) {
    for (int i = 1; i < path.polyline.size(); ++i)
        assert(!path.polyline.get_point(i - 1).coincides_with_epsilon(path.polyline.get_point(i)));
    size_t old_size = path.size();
    ArcPolyline old_poly = path.polyline;
    path.simplify(m_scaled_resolution, m_use_arc_fitting, scale_d(m_arc_fitting_tolearance->get_abs_value(path.width())));
    for (int i = 1; i < path.polyline.size(); ++i)
        assert(!path.polyline.get_point(i - 1).coincides_with_epsilon(path.polyline.get_point(i)));
}
void SimplifyVisitor::use(ExtrusionPath3D& path3D) {
    path3D.simplify(m_scaled_resolution, m_use_arc_fitting, scale_d(m_arc_fitting_tolearance->get_abs_value(path3D.width())));
}

//class ExtrusionTreeVisitor : ExtrusionVisitor {
//public:
//    //virtual void use(ExtrusionEntity &entity) { assert(false); };
//    virtual void use(ExtrusionPath &path) override { const ExtrusionPath &constpath = path;  use(constpath); };
//    virtual void use(ExtrusionPath3D &path3D) override { const ExtrusionPath3D &constpath3D = path3D;  use(constpath3D); };
//    virtual void use(ExtrusionMultiPath &multipath) override { const ExtrusionMultiPath &constmultipath = multipath;  use(constmultipath);
//    }; virtual void use(ExtrusionMultiPath3D &multipath3D) override { const ExtrusionMultiPath3D &constmultipath3D = multipath3D;
//    use(constmultipath3D); }; virtual void use(ExtrusionLoop &loop) override { const ExtrusionLoop &constloop = loop;  use(constloop); };
//    virtual void use(ExtrusionEntityCollection &collection) { const ExtrusionEntityCollection &constcollection = collection;
//    use(constcollection); }; virtual void use(const ExtrusionPath &path) override { assert(false); }; virtual void use(const
//    ExtrusionPath3D &path3D) override { assert(false); }; virtual void use(const ExtrusionMultiPath &multipath) override { assert(false);
//    }; virtual void use(const ExtrusionMultiPath3D &multipath3D) { assert(false); }; virtual void use(const ExtrusionLoop &loop) override
//    { assert(false); }; virtual void use(const ExtrusionEntityCollection &collection) { assert(false); }; virtual void
//    use_default(ExtrusionEntity &entity) { const ExtrusionEntity &constentity = entity;  use_default(constentity); }; virtual void
//    use_default(const ExtrusionEntity &entity) {};
//
//};

} // namespace Slic3r
