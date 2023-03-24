#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "ExPolygonCollection.hpp"
#include "ClipperUtils.hpp"
#include "Extruder.hpp"
#include "Flow.hpp"
#include <cmath>
#include <limits>
#include <sstream>

#define L(s) (s)

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
    
void
ExtrusionPath::intersect_expolygons(const ExPolygonCollection &collection, ExtrusionEntityCollection* retval) const
{
    this->_inflate_collection(intersection_pl(Polylines{ this->polyline.as_polyline() }, collection.expolygons), retval);
}

void ExtrusionPath::subtract_expolygons(const ExPolygonCollection &collection, ExtrusionEntityCollection* retval) const
{
    this->_inflate_collection(diff_pl(Polylines{ this->polyline.as_polyline() }, collection.expolygons), retval);
}

void ExtrusionPath::clip_end(coordf_t distance)
{
    this->polyline.clip_end(distance);
}

void ExtrusionPath::simplify(coordf_t tolerance, bool with_fitting_arc, double fitting_arc_tolerance)
{
    this->polyline.simplify(tolerance, with_fitting_arc, fitting_arc_tolerance);
}

void ExtrusionPath3D::simplify(coordf_t tolerance, bool with_fitting_arc, double fitting_arc_tolerance)
{
    //TODO: simplify but only for sub-path with same zheight.
    //if (with_fitting_arc) {
    //    this->polyline.simplify(tolerance, with_fitting_arc, fitting_arc_tolerance);
    //}
}

double ExtrusionPath::length() const
{
    return this->polyline.length();
}

void ExtrusionPath::_inflate_collection(const Polylines &polylines, ExtrusionEntityCollection* collection) const
{
    ExtrusionEntitiesPtr to_add;
    for (const Polyline &polyline : polylines)
        to_add.emplace_back(new ExtrusionPath(PolylineOrArc{ polyline }, *this));
    collection->append(std::move(to_add));
}

void ExtrusionPath::polygons_covered_by_width(Polygons &out, const float scaled_epsilon) const
{
    polygons_append(out, offset(this->polyline.as_polyline(), double(scale_(this->width/2)) + scaled_epsilon));
}

void ExtrusionPath::polygons_covered_by_spacing(Polygons &out, const float spacing_ratio, const float scaled_epsilon) const
{
    // Instantiating the Flow class to get the line spacing.
    // Don't know the nozzle diameter, setting to zero. It shall not matter it shall be optimized out by the compiler.
    bool bridge = is_bridge(this->role()) || (this->width * 4 < this->height);
    assert(! bridge || this->width == this->height);
    //TODO: check BRIDGE_FLOW here
    auto flow = bridge 
        ? Flow::bridging_flow(this->width, 0.f) 
        : Flow::new_from_width(this->width, 0.f, this->height, spacing_ratio);
    polygons_append(out, offset(this->polyline.as_polyline(), 0.5f * float(flow.scaled_spacing()) + scaled_epsilon));
}

bool ExtrusionLoop::make_clockwise()
{
    bool was_ccw = this->polygon().is_counter_clockwise();
    if (was_ccw) this->reverse();
    return was_ccw;
}

bool ExtrusionLoop::make_counter_clockwise()
{
    bool was_cw = this->polygon().is_clockwise();
    if (was_cw) this->reverse();
    return was_cw;
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
        polygon.points.insert(polygon.points.end(), path.polyline.get_points().begin(), path.polyline.get_points().end()-1);
    }
    return polygon;
}

PolylineOrArc ExtrusionLoop::as_polyline() const {
    PolylineOrArc polyline;
    for (const ExtrusionPath& path : this->paths) {
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

bool ExtrusionLoop::split_at_vertex(const Point &point, const double scaled_epsilon)
{
    for (ExtrusionPaths::iterator path = this->paths.begin(); path != this->paths.end(); ++path) {
        if (int idx = path->polyline.find_point(point, scaled_epsilon); idx != -1) {
            if (this->paths.size() == 1) {
                // just change the order of points
                PolylineOrArc p1, p2;
                path->polyline.split_at_index(idx, &p1, &p2);
                if (p1.is_valid() && p2.is_valid()) {
                    p2.append(std::move(p1));
                    path->polyline.swap(p2); // swap points & fitting result
                }
            } else {
                // new paths list starts with the second half of current path
                ExtrusionPaths new_paths;
                PolylineOrArc p1, p2;
                path->polyline.split_at_index(idx, &p1, &p2);
                new_paths.reserve(this->paths.size() + 1);
                {
                    ExtrusionPath p = *path;
                    p.polyline.swap(p2);
                    if (p.polyline.is_valid()) new_paths.push_back(p);
                }

                // then we add all paths until the end of current path list
                new_paths.insert(new_paths.end(), path + 1, this->paths.end());  // not including this path

                // then we add all paths since the beginning of current list up to the previous one
                new_paths.insert(new_paths.end(), this->paths.begin(), path);  // not including this path

                // finally we add the first half of current path
                {
                    ExtrusionPath p = *path;
                    p.polyline.swap(p1);
                    if (p.polyline.is_valid()) new_paths.push_back(p);
                }
                // we can now override the old path list with the new one and stop looping
                this->paths = std::move(new_paths);
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
    ClosestPathPoint out { 0, 0 };
    double           min2 = std::numeric_limits<double>::max();
    ClosestPathPoint best_non_overhang { 0, 0 };
    double           min2_non_overhang = std::numeric_limits<double>::max();
    for (const ExtrusionPath& path : this->paths) {
        std::pair<int, Point> foot_pt_ = foot_pt(path.polyline.get_points(), point);
        double d2 = (foot_pt_.second - point).cast<double>().squaredNorm();
        if (d2 < min2) {
            out.foot_pt     = foot_pt_.second;
            out.path_idx    = &path - &this->paths.front();
            out.segment_idx = foot_pt_.first;
            min2            = d2;
            }
        if (prefer_non_overhang && !is_bridge(path.role()) && d2 < min2_non_overhang) {
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
    
    auto [path_idx, segment_idx, p] = get_closest_path_and_point(point, prefer_non_overhang);
    
    // Snap p to start or end of segment_idx if closer than scaled_epsilon.
    {
        const Point *p1 = this->paths[path_idx].polyline.get_points().data() + segment_idx;
        const Point *p2 = p1;
        ++ p2;
        double d2_1 = (point - *p1).cast<double>().squaredNorm();
        double d2_2 = (point - *p2).cast<double>().squaredNorm();
        const double thr2 = scaled_epsilon * scaled_epsilon;
        if (d2_1 < d2_2) {
            if (d2_1 < thr2)
                p = *p1;
        } else {
            if (d2_2 < thr2) 
                p = *p2;
        }
    }
    
    // now split path_idx in two parts
    const ExtrusionPath &path = this->paths[path_idx];
    ExtrusionPath p1(path.role(), path.mm3_per_mm, path.width, path.height);
    ExtrusionPath p2(path.role(), path.mm3_per_mm, path.width, path.height);
    path.polyline.split_at(p, &p1.polyline, &p2.polyline);
    
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
        // install the two paths
        this->paths.erase(this->paths.begin() + path_idx);
        if (p2.polyline.is_valid() && p2.polyline.length() > 0) this->paths.insert(this->paths.begin() + path_idx, p2);
        if (p1.polyline.is_valid() && p1.polyline.length() > 0) this->paths.insert(this->paths.begin() + path_idx, p1);
    }
    
    // split at the new vertex
    this->split_at_vertex(p, 0.);
}

ExtrusionPaths clip_end(ExtrusionPaths& paths, coordf_t distance)
{
    ExtrusionPaths removed;
    
    while (distance > 0 && !paths.empty()) {
        ExtrusionPath& last = paths.back();
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

bool ExtrusionLoop::has_overhang_point(const Point &point) const
{
    for (const ExtrusionPath &path : this->paths) {
        int pos = path.polyline.find_point(point);
        if (pos != -1) {
            // point belongs to this path
            // we consider it overhang only if it's not an endpoint
            return (is_bridge(path.role()) && pos > 0 && pos != (int)(path.polyline.size())-1);
        }
    }
    return false;
}

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

std::string ExtrusionEntity::role_to_string(ExtrusionRole role)
{
    switch (role) {
        case erNone                         : return L("Unknown");
        case erPerimeter                    : return L("Internal perimeter");
        case erExternalPerimeter            : return L("External perimeter");
        case erOverhangPerimeter            : return L("Overhang perimeter");
        case erInternalInfill               : return L("Internal infill");
        case erSolidInfill                  : return L("Solid infill");
        case erTopSolidInfill               : return L("Top solid infill");
        case erIroning                      : return L("Ironing");
        case erBridgeInfill                 : return L("Bridge infill");
        case erInternalBridgeInfill         : return L("Internal bridge infill");
        case erThinWall                     : return L("Thin wall");
        case erGapFill                      : return L("Gap fill");
        case erSkirt                        : return L("Skirt");
        case erSupportMaterial              : return L("Support material");
        case erSupportMaterialInterface     : return L("Support material interface");
        case erWipeTower                    : return L("Wipe tower");
        case erMilling                      : return L("Mill");
        case erCustom                       : return L("Custom");
        case erMixed                        : return L("Mixed");
        default                             : assert(false);
    }

    return "";
}


ExtrusionRole ExtrusionEntity::string_to_role(const std::string_view role)
{
    if (role == L("Perimeter") || role == L("Internal perimeter"))
        return erPerimeter;
    else if (role == L("External perimeter"))
        return erExternalPerimeter;
    else if (role == L("Overhang perimeter"))
        return erOverhangPerimeter;
    else if (role == L("Internal infill"))
        return erInternalInfill;
    else if (role == L("Solid infill"))
        return erSolidInfill;
    else if (role == L("Top solid infill"))
        return erTopSolidInfill;
    else if (role == L("Ironing"))
        return erIroning;
    else if (role == L("Bridge infill"))
        return erBridgeInfill;
    else if (role == L("Internal bridge infill"))
        return erInternalBridgeInfill;
    else if (role == L("Thin wall"))
        return erThinWall;
    else if (role == L("Gap fill"))
        return erGapFill;
    else if (role == L("Skirt") || role == L("Skirt/Brim")) // "Skirt" is for backward compatibility with 2.3.1 and earlier
        return erSkirt;
    else if (role == L("Support material"))
        return erSupportMaterial;
    else if (role == L("Support material interface"))
        return erSupportMaterialInterface;
    else if (role == L("Wipe tower"))
        return erWipeTower;
    else if (role == L("Mill"))
        return erMilling;
    else if (role == L("Custom"))
        return erCustom;
    else if (role == L("Mixed"))
        return erMixed;
    else
        return erNone;
}
void ExtrusionPrinter::use(const ExtrusionPath &path) { 
    ss << "ExtrusionPath:" << (uint16_t)path.role() << "{";
    for (int i = 0; i < path.polyline.size(); i++) {
        if (i != 0) ss << ",";
        double x = (mult * (path.polyline.get_points()[i].x()));
        double y = (mult * (path.polyline.get_points()[i].y()));
        ss << std::fixed << "{"<<(trunc?(int)x:x) << "," << (trunc ? (int)y : y) <<"}";
    }
    ss << "}";
}
void ExtrusionPrinter::use(const ExtrusionPath3D &path3D) {
    ss << "ExtrusionPath3D:" << (uint16_t)path3D.role() << "{";
    for (int i = 0; i < path3D.polyline.size();i++){
        if (i != 0) ss << ",";
        double x = (mult * (path3D.polyline.get_points()[i].x()));
        double y = (mult * (path3D.polyline.get_points()[i].y()));
        double z = (path3D.z_offsets.size() > i ? mult * (path3D.z_offsets[i]) : -1);
        ss << std::fixed << "{" << (trunc ? (int)x : x) << "," << (trunc ? (int)y : y) << "," << (trunc ? (int)z : z) << "}";
    }
    ss << "}";
}
void ExtrusionPrinter::use(const ExtrusionMultiPath &multipath) {
    ss << "ExtrusionMultiPath:" << (uint16_t)multipath.role() << "{";
    for (int i = 0; i < multipath.paths.size(); i++) {
        if (i != 0) ss << ",";
        multipath.paths[i].visit(*this);
    }
    ss << "}";
}
void ExtrusionPrinter::use(const ExtrusionMultiPath3D &multipath3D) {
    ss << "multipath3D:" << (uint16_t)multipath3D.role() << "{";
    for (int i = 0; i < multipath3D.paths.size(); i++) {
        if (i != 0) ss << ",";
        multipath3D.paths[i].visit(*this);
    }
    ss << "}";
}
void ExtrusionPrinter::use(const ExtrusionLoop &loop) { 
    ss << "ExtrusionLoop:" << (uint16_t)loop.role()<<":" <<(uint16_t)loop.loop_role()<<"{";
    for (int i = 0; i < loop.paths.size(); i++) {
        if (i != 0) ss << ",";
        loop.paths[i].visit(*this);
    }
    ss << "}";
}
void ExtrusionPrinter::use(const ExtrusionEntityCollection &collection) {
    ss << "ExtrusionEntityCollection:" << (uint16_t)collection.role() << "{";
    for (int i = 0; i < collection.entities().size(); i++) {
        if (i != 0) ss << ",";
        collection.entities()[i]->visit(*this);
    }
    if(!collection.can_sort()) ss<<", no_sort=true";
    ss << "}";
}


void ExtrusionLength::default_use(const ExtrusionEntity& entity) { dist += entity.length(); };
void ExtrusionLength::use(const ExtrusionEntityCollection& collection) {
    for (int i = 0; i < collection.entities().size(); i++) {
        collection.entities()[i]->visit(*this);
    }
}


void ExtrusionVisitorRecursiveConst::use(const ExtrusionMultiPath& multipath) {
    for (const ExtrusionPath& path : multipath.paths) {
        path.visit(*this);
    }
}
void ExtrusionVisitorRecursiveConst::use(const ExtrusionMultiPath3D& multipath3D) {
    for (const ExtrusionPath3D& path3D : multipath3D.paths) {
        path3D.visit(*this);
    }
}
void ExtrusionVisitorRecursiveConst::use(const ExtrusionLoop& loop) {
    for (const ExtrusionPath& path : loop.paths) {
        path.visit(*this);
    }
}
void ExtrusionVisitorRecursiveConst::use(const ExtrusionEntityCollection& collection) {
    for (const ExtrusionEntity* entity : collection.entities()) {
        entity->visit(*this);
    }
}
void ExtrusionVisitorRecursive::use(ExtrusionMultiPath& multipath) {
    for (ExtrusionPath& path : multipath.paths) {
        path.visit(*this);
    }
}
void ExtrusionVisitorRecursive::use(ExtrusionMultiPath3D& multipath3D) {
    for (ExtrusionPath3D& path3D : multipath3D.paths) {
        path3D.visit(*this);
    }
}
void ExtrusionVisitorRecursive::use(ExtrusionLoop& loop) {
    for (ExtrusionPath& path : loop.paths) {
        path.visit(*this);
    }
}
void ExtrusionVisitorRecursive::use(ExtrusionEntityCollection& collection) {
    for (ExtrusionEntity* entity : collection.entities()) {
        entity->visit(*this);
    }
}

//class ExtrusionTreeVisitor : ExtrusionVisitor {
//public:
//    //virtual void use(ExtrusionEntity &entity) { assert(false); };
//    virtual void use(ExtrusionPath &path) override { const ExtrusionPath &constpath = path;  use(constpath); };
//    virtual void use(ExtrusionPath3D &path3D) override { const ExtrusionPath3D &constpath3D = path3D;  use(constpath3D); };
//    virtual void use(ExtrusionMultiPath &multipath) override { const ExtrusionMultiPath &constmultipath = multipath;  use(constmultipath); };
//    virtual void use(ExtrusionMultiPath3D &multipath3D) override { const ExtrusionMultiPath3D &constmultipath3D = multipath3D;  use(constmultipath3D); };
//    virtual void use(ExtrusionLoop &loop) override { const ExtrusionLoop &constloop = loop;  use(constloop); };
//    virtual void use(ExtrusionEntityCollection &collection) { const ExtrusionEntityCollection &constcollection = collection;  use(constcollection); };
//    virtual void use(const ExtrusionPath &path) override { assert(false); };
//    virtual void use(const ExtrusionPath3D &path3D) override { assert(false); };
//    virtual void use(const ExtrusionMultiPath &multipath) override { assert(false); };
//    virtual void use(const ExtrusionMultiPath3D &multipath3D) { assert(false); };
//    virtual void use(const ExtrusionLoop &loop) override { assert(false); };
//    virtual void use(const ExtrusionEntityCollection &collection) { assert(false); };
//    virtual void use_default(ExtrusionEntity &entity) { const ExtrusionEntity &constentity = entity;  use_default(constentity); };
//    virtual void use_default(const ExtrusionEntity &entity) {};
//
//};

}
