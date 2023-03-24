#ifndef slic3r_PerimeterGenerator_hpp_
#define slic3r_PerimeterGenerator_hpp_

#include "libslic3r.h"
#include <vector>
#include "ExPolygonCollection.hpp"
#include "Flow.hpp"
#include "Layer.hpp"
#include "Polygon.hpp"
#include "PrintConfig.hpp"
#include "SurfaceCollection.hpp"
#include "ExtrusionEntityCollection.hpp"

namespace Slic3r {

    namespace Arachne {
        struct ExtrusionLine;
    }

struct PerimeterIntersectionPoint {
    size_t idx_children;
    Point child_best;
    Point outter_best;
    size_t idx_polyline_outter;
    coord_t distance;
};

struct PerimeterGeneratorArachneExtrusion
{
    Arachne::ExtrusionLine* extrusion = nullptr;
    // Indicates if closed ExtrusionLine is a contour or a hole. Used it only when ExtrusionLine is a closed loop.
    bool is_contour = false;
    // Should this extrusion be fuzzyfied on path generation?
    bool fuzzify = false;
};

// Hierarchy of perimeters.
class PerimeterGeneratorLoop {
public:
    // Polygon of this contour.
    Polygon polygon;
    // Is it a contour or a hole?
    // Contours are CCW oriented, holes are CW oriented.
    bool is_contour;
    //overhang may need to be reversed
    bool is_steep_overhang;
    // Depth in the hierarchy. External perimeter has depth = 0. An external perimeter could be both a contour and a hole.
    unsigned short depth;
    // Children contour, may be both CCW and CW oriented (outer contours or holes).
    std::vector<PerimeterGeneratorLoop> children;
    // can be fuzzified?
    bool fuzzify;


    PerimeterGeneratorLoop(Polygon polygon, unsigned short depth, bool is_contour) :
        polygon(polygon), is_contour(is_contour), depth(depth), is_steep_overhang(false), fuzzify(false) {}
    PerimeterGeneratorLoop(Polygon polygon, unsigned short depth, bool is_contour, bool is_steep_overhang, bool is_fuzzy) :
        polygon(polygon), is_contour(is_contour), depth(depth), is_steep_overhang(is_steep_overhang), fuzzify(is_fuzzy) {}
    // External perimeter. It may be CCW or CW oriented (outer contour or hole contour).
    bool is_external() const { return this->depth == 0; }
    // it's the last loop of the contour (not hol), so the first to be printed (if all goes well)
    bool is_internal_contour() const;
};

typedef std::vector<PerimeterGeneratorLoop> PerimeterGeneratorLoops;

struct ProcessSurfaceResult {
    ExPolygons inner_perimeter;
    ExPolygons gap_srf;
    ExPolygons top_fills;
    ExPolygons fill_clip;
};

class PerimeterGenerator {
public:
    // Inputs:
    const SurfaceCollection     *slices;
    const ExPolygons            *upper_slices;
    const ExPolygons            *lower_slices;
    Layer                       *layer;
    Flow                         perimeter_flow;
    Flow                         ext_perimeter_flow;
    Flow                         overhang_flow;
    Flow                         solid_infill_flow;
    const PrintRegionConfig     *config;
    const PrintObjectConfig     *object_config;
    const PrintConfig           *print_config;
    bool                         use_arachne = false;
    // Outputs:
    ExtrusionEntityCollection   *loops;
    ExtrusionEntityCollection   *gap_fill;
    SurfaceCollection           *fill_surfaces;
    ExPolygons fill_no_overlap;
    
    PerimeterGenerator(
        // Input:
        const SurfaceCollection*    slices,
        Flow                        flow,
        const PrintRegionConfig*    config,
        const PrintObjectConfig*    object_config,
        const PrintConfig*          print_config,
        const bool                  spiral_vase,
        // Output:
        // Loops with the external thin walls
        ExtrusionEntityCollection*  loops,
        // Gaps without the thin walls
        ExtrusionEntityCollection*  gap_fill,
        // Infills without the gap fills
        SurfaceCollection*          fill_surfaces)
        : slices(slices), lower_slices(nullptr), upper_slices(nullptr), layer(nullptr),
            perimeter_flow(flow), ext_perimeter_flow(flow),
            overhang_flow(flow), solid_infill_flow(flow),
            config(config), object_config(object_config), print_config(print_config),
            m_spiral_vase(spiral_vase),
            loops(loops), gap_fill(gap_fill), fill_surfaces(fill_surfaces),
            m_ext_mm3_per_mm(-1), m_mm3_per_mm(-1), m_mm3_per_mm_overhang(-1)
        {}

    void        process();

    double      ext_mm3_per_mm()        const { return m_ext_mm3_per_mm; }
    double      mm3_per_mm()            const { return m_mm3_per_mm; }
    double      mm3_per_mm_overhang()   const { return m_mm3_per_mm_overhang; }

    coord_t     get_resolution(size_t perimeter_id, bool is_overhang, const Surface* srf) const;

private:
    bool        m_spiral_vase;
    double      m_ext_mm3_per_mm;
    double      m_mm3_per_mm;
    double      m_mm3_per_mm_overhang;
    Polygons    _lower_slices_bridge_flow_small;
    Polygons    _lower_slices_bridge_flow_big;
    Polygons    _lower_slices_bridge_speed_small;
    Polygons    _lower_slices_bridge_speed_big;
    ClipperLib_Z::Paths m_lower_slices_clipperpaths;
    ClipperLib_Z::Paths _lower_slices_bridge_flow_small_clipperpaths;
    ClipperLib_Z::Paths _lower_slices_bridge_flow_big_clipperpaths;
    ClipperLib_Z::Paths _lower_slices_bridge_speed_small_clipperpaths;
    ClipperLib_Z::Paths _lower_slices_bridge_speed_big_clipperpaths;

    //process data
    coord_t perimeter_width; coord_t get_perimeter_width() { return perimeter_width; }
    coord_t perimeter_spacing; coord_t get_perimeter_spacing() { return perimeter_spacing; }
    coord_t ext_perimeter_width; coord_t get_ext_perimeter_width() { return ext_perimeter_width; }
    coord_t ext_perimeter_spacing; coord_t get_ext_perimeter_spacing() { return ext_perimeter_spacing; }
    coord_t ext_perimeter_spacing2; coord_t get_ext_perimeter_spacing2() { return ext_perimeter_spacing2; }
    coord_t gap_fill_spacing; coord_t get_gap_fill_spacing() { return gap_fill_spacing; }
    coord_t gap_fill_spacing_external; coord_t get_gap_fill_spacing_external() { return gap_fill_spacing_external; }
    coord_t infill_gap; coord_t get_infill_gap() { return infill_gap; }
    coord_t solid_infill_spacing; coord_t get_solid_infill_spacing() { return solid_infill_spacing; }
    bool round_peri;
    coord_t min_round_spacing; coord_t get_min_round_spacing() { return min_round_spacing; }
    ExPolygons unmillable;
    coord_t mill_extra_size;

    ProcessSurfaceResult process_classic(int& loop_number, const Surface& surface);
    ProcessSurfaceResult process_arachne(int& loop_number, const Surface& surface);
    
    void        processs_no_bridge(Surfaces& all_surfaces);
    ExtrusionPaths create_overhangs(const Polyline& loop_polygons, ExtrusionRole role, bool is_external) const;
    ExtrusionPaths create_overhangs(const ClipperLib_Z::Path& loop_polygons, ExtrusionRole role, bool is_external) const;

    // transform loops into ExtrusionEntityCollection, adding also thin walls into it.
    ExtrusionEntityCollection _traverse_loops(const PerimeterGeneratorLoops &loops, ThickPolylines &thin_walls, int count_since_overhang = 0) const;
    ExtrusionEntityCollection _traverse_extrusions(std::vector<PerimeterGeneratorArachneExtrusion>& pg_extrusions);
    // try to merge thin walls to a current periemter exrusion or just add it to the end of the list.
    void _merge_thin_walls(ExtrusionEntityCollection &extrusions, ThickPolylines &thin_walls) const;
    // like _traverse_loops but with merging all periemter into one continuous loop
    ExtrusionLoop _traverse_and_join_loops(const PerimeterGeneratorLoop &loop, const PerimeterGeneratorLoops &childs, const Point entryPoint) const;
    // sub-function of _traverse_and_join_loops, transform a single loop as a cut extrusion to be merged with an other one.
    ExtrusionLoop _extrude_and_cut_loop(const PerimeterGeneratorLoop& loop, const Point entryPoint, const Line& direction = Line(Point(0, 0), Point(0, 0)), bool enforce_loop = false) const;
    // sub-function of _traverse_and_join_loops, find the good splot to cut a loop to be able to join it with an other one
    PerimeterIntersectionPoint _get_nearest_point(const PerimeterGeneratorLoops &children, ExtrusionLoop &myPolylines, const coord_t dist_cut, const coord_t max_dist) const;


};



}

#endif
