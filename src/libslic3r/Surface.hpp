///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/ Copyright (c) Prusa Research 2016 - 2023 Vojtěch Bubník @bubnikv, Lukáš Matěna @lukasmatena
///|/ Copyright (c) 2016 Sakari Kapanen @Flannelhead
///|/ Copyright (c) Slic3r 2013 - 2015 Alessandro Ranellucci @alranel
///|/
///|/ ported from lib/Slic3r/Surface.pm:
///|/ Copyright (c) Prusa Research 2022 Vojtěch Bubník @bubnikv
///|/ Copyright (c) Slic3r 2011 - 2014 Alessandro Ranellucci @alranel
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Surface_hpp_
#define slic3r_Surface_hpp_

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "ExPolygon.hpp"
#include "libslic3r.h"

namespace Slic3r {

/// a SurfaceType should be composed of a Position & density flag
/// Position: top, bottom, internal
/// Density: solid, sparse, void
/// optinally, is can also have one bridge modifier (bridge, over-bridge).
enum SurfaceType  : uint16_t { 
    stNone = 0,
    /// Position: Top horizontal surface, visible from the top.
    stPosTop        = 1 << 0,
    /// Position: Bottom horizontal surface, visible from the bottom, printed with a normal extrusion flow.
    stPosBottom     = 1 << 1,
    /// Position: Normal sparse infill.
    stPosInternal   = 1 << 2,
    /// Position: Inner/outer perimeters. Mainly used for coloring
    stPosPerimeter  = 1 << 3,
    /// Density: Solid infill (100%).
    stDensSolid     = 1 << 4,
    /// Density: Sparse infill (>0% & <100%).
    stDensSparse    = 1 << 5,
    /// Density: or if sparse infill layers get combined into a single layer.
    stDensVoid      = 1 << 6,
    /// Bridge Modifier: 1st layer of dense infill over sparse infill, printed with a bridging extrusion flow.
    stModBridge = 1 << 7,
    /// Bridge Modifier: 2nd layer of dense infill over sparse infill/nothing, may be printed with an over-extruding flow.
    stModOverBridge = 1 << 8,
};
inline SurfaceType operator|(SurfaceType a, SurfaceType b) {
    return static_cast<SurfaceType>(static_cast<uint16_t>(a) | static_cast<uint16_t>(b));
}
inline SurfaceType operator&(SurfaceType a, SurfaceType b) {
    return static_cast<SurfaceType>(static_cast<uint16_t>(a) & static_cast<uint16_t>(b));
}
inline SurfaceType operator^(SurfaceType a, SurfaceType b) {
    return static_cast<SurfaceType>(static_cast<uint16_t>(a) ^ static_cast<uint16_t>(b));
}
inline SurfaceType operator|=(SurfaceType& a, SurfaceType b) {
    a = a | b; return a;
}
inline SurfaceType operator&=(SurfaceType& a, SurfaceType b) {
    a = a & b; return a;
}
//inline bool operator==(SurfaceType a, SurfaceType b) {
//    return static_cast<uint16_t>(a) ^ static_cast<uint16_t>(b) == 0;
//}
//inline bool operator!=(SurfaceType a, SurfaceType b) {
//    return static_cast<uint16_t>(a) ^ static_cast<uint16_t>(b) != 0;
//}
std::string surfaceType_to_string(SurfaceType st);


class Surface
{
    coord_t          m_thickness        { -1 };
public:
    SurfaceType     surface_type;
    ExPolygon       expolygon;
    // @Deprecated
    uint16_t        thickness_layers{1_u}; // in layers
    void    set_scaled_thickness(coord_t layer_height) { m_thickness = layer_height; assert(layer_height > 100); }
    coord_t scaled_thickness() const { return m_thickness; }
    double  unscaled_thickness() const { return unscaled(m_thickness); }
    // @Deprecated
    double          bridge_angle     { -1. }; // in radians, ccw, 0 = East, only 0+ (negative means undefined)
    // @Deprecated
    uint16_t        extra_perimeters{0_u};
    //for dense infill
    uint16_t        maxNbSolidLayersOnTop { uint16_t(-1) };
    //for dense infill
    uint16_t        priority              {  0_u };
    
    Surface(const Surface &rhs) :
        surface_type(rhs.surface_type), expolygon(rhs.expolygon),
            m_thickness(rhs.m_thickness), thickness_layers(rhs.thickness_layers), 
            bridge_angle(rhs.bridge_angle), extra_perimeters(rhs.extra_perimeters),
            maxNbSolidLayersOnTop(rhs.maxNbSolidLayersOnTop),
            priority(rhs.priority) {};

    Surface(SurfaceType _surface_type, const ExPolygon &_expolygon)
        : surface_type(_surface_type), expolygon(_expolygon) {};
    Surface(const Surface &other, const ExPolygon &_expolygon)
        : surface_type(other.surface_type), expolygon(_expolygon),
            m_thickness(other.m_thickness), thickness_layers(other.thickness_layers), 
            bridge_angle(other.bridge_angle), extra_perimeters(other.extra_perimeters),
            maxNbSolidLayersOnTop(other.maxNbSolidLayersOnTop),
            priority(other.priority) {};
    Surface(Surface &&rhs)
        : surface_type(rhs.surface_type), expolygon(std::move(rhs.expolygon)),
            m_thickness(rhs.m_thickness), thickness_layers(rhs.thickness_layers), 
            bridge_angle(rhs.bridge_angle), extra_perimeters(rhs.extra_perimeters),
            maxNbSolidLayersOnTop(rhs.maxNbSolidLayersOnTop),
            priority(rhs.priority) {};
    Surface(SurfaceType _surface_type, ExPolygon &&_expolygon)
        : surface_type(_surface_type), expolygon(std::move(_expolygon)) {};
    Surface(const Surface &other, ExPolygon &&_expolygon)
        : surface_type(other.surface_type), expolygon(std::move(_expolygon)),
            m_thickness(other.m_thickness), thickness_layers(other.thickness_layers), 
            bridge_angle(other.bridge_angle), extra_perimeters(other.extra_perimeters),
            maxNbSolidLayersOnTop(other.maxNbSolidLayersOnTop),
            priority(other.priority) {};

    Surface& operator=(const Surface &rhs)
    {
        surface_type     = rhs.surface_type;
        expolygon        = rhs.expolygon;
        m_thickness      = rhs.m_thickness;
        thickness_layers = rhs.thickness_layers;
        bridge_angle     = rhs.bridge_angle;
        extra_perimeters = rhs.extra_perimeters;
        maxNbSolidLayersOnTop = rhs.maxNbSolidLayersOnTop;
        priority         = rhs.priority;
        return *this;
    }

    Surface& operator=(Surface &&rhs)
    {
        surface_type     = rhs.surface_type;
        expolygon        = std::move(rhs.expolygon);
        m_thickness      = rhs.m_thickness;
        thickness_layers = rhs.thickness_layers;
        bridge_angle     = rhs.bridge_angle;
        extra_perimeters = rhs.extra_perimeters;
        maxNbSolidLayersOnTop = rhs.maxNbSolidLayersOnTop;
        priority         = rhs.priority;
        return *this;
    }

	double area() 		 const { return this->expolygon.area(); }
    bool empty() const { return expolygon.empty(); }
    void clear() { expolygon.clear(); }
    bool has(SurfaceType type) const;
    bool has_fill_solid() const;
    bool has_fill_sparse() const;
    bool has_fill_void() const;
    bool has_pos_external() const;
    bool has_pos_top() const;
    bool has_pos_internal() const;
    bool has_pos_bottom() const;
    bool has_mod_bridge() const;
    bool has_mod_overBridge() const;
};

typedef std::vector<Surface> Surfaces;
typedef std::vector<const Surface*> SurfacesPtr;
typedef std::vector<const Surface*> SurfacesConstPtr;

inline Polygons to_polygons(const Surface &surface)
{
    return to_polygons(surface.expolygon);
}

inline Polygons to_polygons(Surface &&surface)
{
    return to_polygons(std::move(surface.expolygon));
}

inline Polygons to_polygons(const Surfaces &src)
{
    size_t num = 0;
    for (Surfaces::const_iterator it = src.begin(); it != src.end(); ++it)
        num += it->expolygon.holes.size() + 1;
    Polygons polygons;
    polygons.reserve(num);
    for (Surfaces::const_iterator it = src.begin(); it != src.end(); ++it) {
        polygons.emplace_back(it->expolygon.contour);
        for (Polygons::const_iterator ith = it->expolygon.holes.begin(); ith != it->expolygon.holes.end(); ++ith)
            polygons.emplace_back(*ith);
    }
    return polygons;
}

inline Polygons to_polygons(const SurfacesConstPtr &src)
{
    size_t num = 0;
    for (SurfacesConstPtr::const_iterator it = src.begin(); it != src.end(); ++it)
        num += (*it)->expolygon.holes.size() + 1;
    Polygons polygons;
    polygons.reserve(num);
    for (SurfacesConstPtr::const_iterator it = src.begin(); it != src.end(); ++it) {
        polygons.emplace_back((*it)->expolygon.contour);
        for (Polygons::const_iterator ith = (*it)->expolygon.holes.begin(); ith != (*it)->expolygon.holes.end(); ++ith)
            polygons.emplace_back(*ith);
    }
    return polygons;
}

inline ExPolygons to_expolygons(const Surfaces &src)
{
    ExPolygons expolygons;
    expolygons.reserve(src.size());
    for (Surfaces::const_iterator it = src.begin(); it != src.end(); ++it)
        expolygons.emplace_back(it->expolygon);
    return expolygons;
}

inline ExPolygons to_expolygons(Surfaces &&src)
{
    ExPolygons expolygons;
    expolygons.reserve(src.size());
	for (auto it = src.begin(); it != src.end(); ++it)
        expolygons.emplace_back(ExPolygon(std::move(it->expolygon)));
    src.clear();
    return expolygons;
}

inline ExPolygons to_expolygons(const SurfacesConstPtr &src)
{
    ExPolygons expolygons;
    expolygons.reserve(src.size());
    for (SurfacesConstPtr::const_iterator it = src.begin(); it != src.end(); ++it)
        expolygons.emplace_back((*it)->expolygon);
    return expolygons;
}

// Count a nuber of polygons stored inside the vector of expolygons.
// Useful for allocating space for polygons when converting expolygons to polygons.
inline size_t number_polygons(const Surfaces &surfaces)
{
    size_t n_polygons = 0;
    for (Surfaces::const_iterator it = surfaces.begin(); it != surfaces.end(); ++ it)
        n_polygons += it->expolygon.holes.size() + 1;
    return n_polygons;
}
inline size_t number_polygons(const SurfacesPtr &surfaces)
{
    size_t n_polygons = 0;
    for (SurfacesPtr::const_iterator it = surfaces.begin(); it != surfaces.end(); ++ it)
        n_polygons += (*it)->expolygon.holes.size() + 1;
    return n_polygons;
}

// Append a vector of Surfaces at the end of another vector of polygons.
inline void polygons_append(Polygons &dst, const Surfaces &src) 
{ 
    dst.reserve(dst.size() + number_polygons(src));
    for (Surfaces::const_iterator it = src.begin(); it != src.end(); ++ it) {
        dst.emplace_back(it->expolygon.contour);
        dst.insert(dst.end(), it->expolygon.holes.begin(), it->expolygon.holes.end());
    }
}

inline void polygons_append(Polygons &dst, Surfaces &&src) 
{ 
    dst.reserve(dst.size() + number_polygons(src));
    for (Surfaces::iterator it = src.begin(); it != src.end(); ++ it) {
        dst.emplace_back(std::move(it->expolygon.contour));
        std::move(std::begin(it->expolygon.holes), std::end(it->expolygon.holes), std::back_inserter(dst));
        it->expolygon.holes.clear();
    }
}

// Append a vector of Surfaces at the end of another vector of polygons.
inline void polygons_append(Polygons &dst, const SurfacesPtr &src) 
{ 
    dst.reserve(dst.size() + number_polygons(src));
    for (SurfacesPtr::const_iterator it = src.begin(); it != src.end(); ++ it) {
        dst.emplace_back((*it)->expolygon.contour);
        dst.insert(dst.end(), (*it)->expolygon.holes.begin(), (*it)->expolygon.holes.end());
    }
}

/*
inline void polygons_append(Polygons &dst, SurfacesPtr &&src) 
{ 
    dst.reserve(dst.size() + number_polygons(src));
    for (SurfacesPtr::const_iterator it = src.begin(); it != src.end(); ++ it) {
        dst.emplace_back(std::move((*it)->expolygon.contour));
        std::move(std::begin((*it)->expolygon.holes), std::end((*it)->expolygon.holes), std::back_inserter(dst));
        (*it)->expolygon.holes.clear();
    }
}
*/

// Append a vector of Surfaces at the end of another vector of polygons.
inline void surfaces_append(Surfaces &dst, const ExPolygons &src, SurfaceType surfaceType) 
{ 
    dst.reserve(dst.size() + src.size());
    for (const ExPolygon &expoly : src)
        dst.emplace_back(Surface(surfaceType, expoly));
}
inline void surfaces_append(Surfaces &dst, const ExPolygons &src, const Surface &surfaceTempl) 
{ 
    dst.reserve(dst.size() + number_polygons(src));
    for (const ExPolygon &expoly : src)
        dst.emplace_back(Surface(surfaceTempl, expoly));
}
inline void surfaces_append(Surfaces &dst, const Surfaces &src) 
{ 
    dst.insert(dst.end(), src.begin(), src.end());
}

inline void surfaces_append(Surfaces &dst, ExPolygons &&src, SurfaceType surfaceType) 
{ 
    dst.reserve(dst.size() + src.size());
    for (ExPolygon &expoly : src) {
        expoly.assert_valid();
        dst.emplace_back(Surface(surfaceType, std::move(expoly)));
    }
    src.clear();
}

inline void surfaces_append(Surfaces &dst, ExPolygons &&src, const Surface &surfaceTempl) 
{ 
    dst.reserve(dst.size() + number_polygons(src));
    for (ExPolygon& explg : src)
        dst.emplace_back(Surface(surfaceTempl, std::move(explg)));
    src.clear();
}

inline void surfaces_append(Surfaces &dst, Surfaces &&src) 
{ 
    if (dst.empty()) {
        dst = std::move(src);
    } else {
        std::move(std::begin(src), std::end(src), std::back_inserter(dst));
        src.clear();
    }
}

extern BoundingBox get_extents(const Surface &surface);
extern BoundingBox get_extents(const Surfaces &surfaces);
extern BoundingBox get_extents(const SurfacesConstPtr &surfaces);

inline bool surfaces_could_merge(const Surface &s1, const Surface &s2)
{
    return 
        s1.surface_type      == s2.surface_type     &&
        s1.scaled_thickness()== s2.scaled_thickness()&&
        s1.thickness_layers  == s2.thickness_layers &&
        s1.bridge_angle      == s2.bridge_angle;
}

// remove any point that are at epsilon  (or resolution) 'distance' (douglas_peuckere algo for now) and all polygons that are too small to be valid
void ensure_valid(Surfaces &surfaces, coord_t resolution = SCALED_EPSILON);


class SVG;

extern const std::string surface_type_to_color_name(const SurfaceType surface_type, float saturation = 1);
extern void export_surface_type_legend_to_svg(SVG &svg, const Point &pos);
extern Point export_surface_type_legend_to_svg_box_size();
extern bool export_to_svg(const char *path, const Surfaces &surfaces, const float transparency = 1.f);

} //namespace Slic3r

#endif
