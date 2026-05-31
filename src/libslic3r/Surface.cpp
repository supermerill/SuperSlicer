///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/ Copyright (c) Prusa Research 2016 - 2019 Vojtěch Bubník @bubnikv
///|/ Copyright (c) Slic3r 2013 - 2015 Alessandro Ranellucci @alranel
///|/ Copyright (c) 2014 Petr Ledvina @ledvinap
///|/
///|/ ported from lib/Slic3r/Surface.pm:
///|/ Copyright (c) Prusa Research 2022 Vojtěch Bubník @bubnikv
///|/ Copyright (c) Slic3r 2011 - 2014 Alessandro Ranellucci @alranel
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "Surface.hpp"

#include "BoundingBox.hpp"
#include "SVG.hpp"

#include <atomic>

namespace Slic3r {

uint64_t Surface::next_runtime_id()
{
    // The id only has to be unique inside this process. It is assigned from
    // host code, so one atomic counter is enough even when surface plugins build
    // or move collections from several worker threads.
    static std::atomic<uint64_t> next_id { 1 };
    return next_id.fetch_add(1, std::memory_order_relaxed);
}

bool Surface::has(SurfaceType type) const {
    return (this->surface_type & type) == type;
}

bool
Surface::has_fill_void() const {
    return has(stDensVoid);
}

bool
Surface::has_fill_sparse() const {
    return has(stDensSparse);
}

bool
Surface::has_fill_solid() const {
    return has(stDensSolid);
}

bool
Surface::has_pos_external() const
{
    return has_pos_top() || has_pos_bottom();
}

bool
Surface::has_pos_top() const
{
    return has(stPosTop);
}

bool
Surface::has_pos_internal() const
{
    return has(stPosInternal);
}

bool
Surface::has_pos_bottom() const
{
    return has(stPosBottom);
}

bool
Surface::has_mod_bridge() const
{
    return has(stModBridge);
}
bool
Surface::has_mod_overBridge() const
{
    return has(stModOverBridge);
}

BoundingBox get_extents(const Surface &surface)
{
    return get_extents(surface.expolygon.contour);
}

BoundingBox get_extents(const Surfaces &surfaces)
{
    BoundingBox bbox;
    if (! surfaces.empty()) {
        bbox = get_extents(surfaces.front());
        for (size_t i = 1; i < surfaces.size(); ++ i)
            bbox.merge(get_extents(surfaces[i]));
    }
    return bbox;
}

BoundingBox get_extents(const SurfacesConstPtr &surfaces)
{
    BoundingBox bbox;
    if (! surfaces.empty()) {
        bbox = get_extents(*surfaces.front());
        for (size_t i = 1; i < surfaces.size(); ++ i)
            bbox.merge(get_extents(*surfaces[i]));
    }
    return bbox;
}

void ensure_valid(Surfaces &surfaces, coord_t resolution /*= SCALED_EPSILON*/)
{
    for (size_t i = 0; i < surfaces.size(); ++i) {
        ExPolygons to_simplify = {surfaces[i].expolygon};
        ensure_valid(to_simplify, resolution);
        if (to_simplify.empty()) {
            surfaces.erase(surfaces.begin() + i);
            --i;
        } else if (to_simplify.size() == 1) {
            surfaces[i].expolygon = to_simplify.front();
        } else {
            surfaces[i].expolygon = to_simplify.front();
            for (size_t idx = 1; idx < to_simplify.size(); idx++) {
                surfaces.insert(surfaces.begin() + i + idx,
                                Surface{surfaces[i], to_simplify[idx]});
                i++;
            }
        }
    }
}

const std::string surface_type_to_color_name(const SurfaceType surface_type, float saturation)
{
    if ((surface_type & stPosTop) != 0) return (std::string("rgb(")+std::to_string(int(saturation*255))+",0,0)"); // "red";
    if (surface_type == (stPosBottom | stDensSolid | stModBridge)) return (std::string("rgb(0,0,")+std::to_string(int(saturation*255))+")"); // "blue";
    if ((surface_type & stPosBottom) != 0) return (std::string("rgb(0,")+std::to_string(int(saturation*255))+",0)"); // "green";
    if (surface_type == (stPosInternal | stDensSparse | stModBridge)) return (std::string("rgb(")+std::to_string(int(saturation*64))+","+std::to_string(int(saturation*128))+","+std::to_string(int(saturation*255))+")"); // light blue
    if (surface_type == (stPosInternal | stDensSolid | stModBridge)) return (std::string("rgb(0,")+std::to_string(int(saturation*255))+","+std::to_string(int(saturation*255))+")"); // cyan
    if (surface_type == (stPosInternal | stDensSolid | stModOverBridge)) return (std::string("rgb(0,")+std::to_string(int(saturation*255))+",128)"); // green-cyan
    if (surface_type == (stPosInternal | stDensSolid)) return (std::string("rgb(")+std::to_string(int(saturation*255))+",0,"+std::to_string(int(saturation*255))+")"); // magenta
    if (surface_type == (stPosInternal | stDensVoid)) return (std::string("rgb(")+std::to_string(int(saturation*128))+","+std::to_string(int(saturation*128))+","+std::to_string(int(saturation*128))+")"); // gray
    if ((surface_type & (stPosInternal | stDensSparse)) == (stPosInternal | stDensSparse)) return (std::string("rgb(")+std::to_string(int(saturation*255))+","+std::to_string(int(saturation*255))+",128)"); // yellow 
    if ((surface_type & stPosPerimeter) != 0) return (std::string("rgb(")+std::to_string(int(saturation*128))+",0,0)"); // maroon
    return "rgb(64,64,64)"; //dark gray
}

Point export_surface_type_legend_to_svg_box_size()
{
    return Point(scale_i(1.+10.*8.), scale_i(3.)); 
}

void export_surface_type_legend_to_svg(SVG &svg, const Point &pos)
{
    // 1st row
    coord_t pos_x0 = pos(0) + scale_i(1.);
    coord_t pos_x = pos_x0;
    coord_t pos_y = pos(1) + scale_i(1.5);
    coord_t step_x = scale_i(10.);
    svg.draw_legend(Point(pos_x, pos_y), "perimeter"      , surface_type_to_color_name(stPosPerimeter));
    pos_x += step_x;
    svg.draw_legend(Point(pos_x, pos_y), "top"            , surface_type_to_color_name(stPosTop));
    pos_x += step_x;
    svg.draw_legend(Point(pos_x, pos_y), "bottom"         , surface_type_to_color_name(stPosBottom));
    pos_x += step_x;
    svg.draw_legend(Point(pos_x, pos_y), "bottom bridge"  , surface_type_to_color_name(stPosBottom | stModBridge));
    pos_x += step_x;
    svg.draw_legend(Point(pos_x, pos_y), "invalid"        , surface_type_to_color_name(SurfaceType(-1)));
    // 2nd row
    pos_x = pos_x0;
    pos_y = pos(1) + scale_i(2.8);
    svg.draw_legend(Point(pos_x, pos_y), "internal"       , surface_type_to_color_name(stPosInternal | stDensSparse));
    pos_x += step_x;
    svg.draw_legend(Point(pos_x, pos_y), "dense bridge", surface_type_to_color_name(stPosInternal | stDensSparse | stModBridge));
    pos_x += step_x;
    svg.draw_legend(Point(pos_x, pos_y), "internal solid" , surface_type_to_color_name(stPosInternal | stDensSolid));
    pos_x += step_x;
    svg.draw_legend(Point(pos_x, pos_y), "internal bridge", surface_type_to_color_name(stPosInternal | stDensSolid | stModBridge));
    pos_x += step_x;
    svg.draw_legend(Point(pos_x, pos_y), "internal over bridge", surface_type_to_color_name(stPosInternal| stDensSolid | stModOverBridge));
    pos_x += step_x;
    svg.draw_legend(Point(pos_x, pos_y), "internal void"  , surface_type_to_color_name(stPosInternal | stDensVoid));
}

bool export_to_svg(const char *path, const Surfaces &surfaces, const float transparency)
{
    BoundingBox bbox;
    for (Surfaces::const_iterator surface = surfaces.begin(); surface != surfaces.end(); ++surface)
        bbox.merge(get_extents(surface->expolygon));

    SVG svg(path, bbox);
    for (Surfaces::const_iterator surface = surfaces.begin(); surface != surfaces.end(); ++surface)
        svg.draw(surface->expolygon, surface_type_to_color_name(surface->surface_type), transparency);
    svg.Close();
    return true;
}


std::string surfaceType_to_string(SurfaceType st)
{
    std::string str;
    if ((st & stPosTop) != 0)
        str += "posTop";
    if ((st & stPosBottom) != 0) {
        if (!str.empty())
            str += "||";
        str += "posBottom";
    }
    if ((st & stPosInternal) != 0) {
        if (!str.empty())
            str += "||";
        str += "posInternal";
    }
    if ((st & stPosPerimeter) != 0) {
        if (!str.empty())
            str += "||";
        str += "posPerimeter";
    }
    if ((st & stDensSolid) != 0) {
        if (!str.empty())
            str += "||";
        str += "densSolid";
    }
    if ((st & stDensSparse) != 0) {
        if (!str.empty())
            str += "||";
        str += "densSparse";
    }
    if ((st & stDensVoid) != 0) {
        if (!str.empty())
            str += "||";
        str += "densVoid";
    }
    if ((st & stModBridge) != 0) {
        if (!str.empty())
            str += "||";
        str += "modBridge";
    }
    if ((st & stModOverBridge) != 0) {
        if (!str.empty())
            str += "||";
        str += "modOverBridge";
    }
    return str.empty() ? "none" : str;
}

}
