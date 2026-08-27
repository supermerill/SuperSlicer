///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/ Copyright (c) Prusa Research 2016 - 2023 Vojtěch Bubník @bubnikv, Lukáš Hejl @hejllukas
///|/ Copyright (c) Slic3r 2015 - 2016 Alessandro Ranellucci @alranel
///|/ Copyright (c) 2015 Maksim Derbasov @ntfshard
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_PerimeterGenerator_hpp_
#define slic3r_PerimeterGenerator_hpp_

#include <vector>

#include "ClipperUtils.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "Flow.hpp"
#include "libslic3r.h"
#include "Polygon.hpp"
#include "Config/FFFPrintConfig.hpp"
#include "RegionSettings.hpp"
#include "SurfaceCollection.hpp"

//#define _DEBUG_OVERHANGS

namespace Slic3r::Arachne {
struct ExtrusionLine;
}
namespace Slic3r {
class Layer;
}
namespace Slic3r::PerimeterGenerator {

struct Parameters
{
    // Input parameters
    const Layer *            layer;
    const Flow               perimeter_flow;
    const Flow               ext_perimeter_flow;
    const Flow               overhang_flow; // ie bridging flow
    const Flow               solid_infill_flow;
    const PrintRegionConfig &config;
    const PrintObjectConfig &object_config;
    const PrintConfig &      print_config;
    const bool               spiral_vase;
    const bool               use_arachne;
    const bool               use_bridge;

    // computed parameters (from config)
    const double  m_ext_mm3_per_mm;
    double        ext_mm3_per_mm() const { return m_ext_mm3_per_mm; }
    const double  m_mm3_per_mm;
    double        mm3_per_mm() const { return m_mm3_per_mm; }
    const double  m_mm3_per_mm_overhang;
    double        mm3_per_mm_overhang() const { return m_mm3_per_mm_overhang; }
    const coord_t perimeter_width;
    coord_t       get_perimeter_width() const { return perimeter_width; }
    const coord_t perimeter_spacing;
    coord_t       get_perimeter_spacing() const { return perimeter_spacing; }
    const coord_t ext_perimeter_width;
    coord_t       get_ext_perimeter_width() const { return ext_perimeter_width; }
    const coord_t ext_perimeter_spacing;
    coord_t       get_ext_perimeter_spacing() const { return ext_perimeter_spacing; }
    coord_t       ext_perimeter_spacing2;
    coord_t       get_ext_perimeter_spacing2() const { return ext_perimeter_spacing2; }
    const coord_t overhang_spacing;
    coord_t       get_overhang_spacing() const { return overhang_spacing; }
    //const coord_t gap_fill_spacing;
    //coord_t       get_gap_fill_spacing() const { return gap_fill_spacing; }
    //const coord_t gap_fill_spacing_external;
    //coord_t       get_gap_fill_spacing_external() const { return gap_fill_spacing_external; }
    coord_t       infill_gap;
    coord_t       get_infill_gap() const { return infill_gap; }
    const coord_t solid_infill_spacing;
    coord_t       get_solid_infill_spacing() const { return solid_infill_spacing; }
    const bool    round_peri;
    bool          use_round_perimeters() const { return round_peri; }
    const coord_t min_round_spacing;
    coord_t       get_min_round_spacing() const { return min_round_spacing; }

    // cached parameters
    ExPolygons overhang_areas;
    Polygons lower_slices_bridge_for_extra_overhangs;
    Polygons lower_slices_bridge_dynamic;
    Polygons lower_slices_bridge_speed_small;
    Polygons lower_slices_bridge_speed_big;
    Polygons lower_slices_bridge_flow_small;
    Polygons lower_slices_bridge_flow_big;

    static const std::vector<t_config_option_keys> perimeter_keys;
    RegionSettings region_setting;
    static inline ConfigOptionInt OPT_EXTRA_PERIMETER;
    std::vector<ConfigOptionInt> extra_perimeters_storage;

    void segregate_extra_perimeters(const ExPolygon &my_srf, const LayerRegionSetCPtrs &lregions);
    Parameters(Layer *layer,
               Flow perimeter_flow,
               Flow ext_perimeter_flow,
               Flow overhang_flow,
               Flow solid_infill_flow,
               const PrintRegionConfig &config,
               const PrintObjectConfig &object_config,
               const PrintConfig &      print_config,
               const bool               spiral_vase,
               const bool               arachne,
               const bool               surface_bridge)
        : layer(layer)
        , perimeter_flow(perimeter_flow)
        , ext_perimeter_flow(ext_perimeter_flow)
        , overhang_flow(overhang_flow)
        , solid_infill_flow(solid_infill_flow)
        , config(config)
        , object_config(object_config)
        , print_config(print_config)
        , spiral_vase(spiral_vase)
        , use_arachne(arachne)
        , use_bridge(surface_bridge)
        , region_setting(config, perimeter_keys),
        // other perimeters
        m_mm3_per_mm(perimeter_flow.mm3_per_mm()),
        perimeter_width(perimeter_flow.scaled_width()),
        perimeter_spacing(perimeter_flow.scaled_spacing()),
        // external perimeters
        m_ext_mm3_per_mm(ext_perimeter_flow.mm3_per_mm()),
        ext_perimeter_width(ext_perimeter_flow.scaled_width()),
        overhang_spacing(scale_i(config.overhangs_extrusion_spacing.get_effective_value(perimeter_flow.nozzle_diameter()))),
        //spacing between two external perimeter (where you don't have the space to add other loops)
        ext_perimeter_spacing(this->ext_perimeter_flow.scaled_spacing()),
        //spacing between external perimeter and the second
        ext_perimeter_spacing2(ext_perimeter_spacing / 2 + perimeter_spacing / 2), //this->ext_perimeter_flow.scaled_spacing(this->perimeter_flow);
        // overhang perimeters
        m_mm3_per_mm_overhang(this->overhang_flow.mm3_per_mm()),
        //gap fill
        //gap_fill_spacing_external(this->config.gap_fill_overlap.get_effective_value(this->ext_perimeter_flow.with_spacing_ratio_from_width(1).scaled_spacing())
        //    + this->ext_perimeter_flow.scaled_width() * (1 - this->config.gap_fill_overlap.get_effective_value(1.))),
        //gap_fill_spacing(this->config.gap_fill_overlap.get_effective_value(this->perimeter_flow.with_spacing_ratio_from_width(1).scaled_spacing())
        //    + this->perimeter_flow.scaled_width() * (1 - this->config.gap_fill_overlap.get_effective_value(1.))),
        // solid infill
        solid_infill_spacing(this->solid_infill_flow.scaled_spacing()),
        // infill gap to add vs perimeter (useful if using perimeter bonding)
        infill_gap(0),
        //
        round_peri(this->config.perimeter_round_corners.value),
        min_round_spacing(round_peri ? perimeter_width / 10 : 0)
    {
    }

private:
    Parameters() = delete;
};



struct PerimeterIntersectionPoint
{
    size_t  idx_children;
    Point   child_best;
    Point   outter_best;
    size_t  idx_polyline_outter;
    coord_t distance;
};

struct PerimeterGeneratorArachneExtrusion
{
    Arachne::ExtrusionLine *extrusion = nullptr;
    // Indicates if closed ExtrusionLine is a contour or a hole. Used it only when ExtrusionLine is a closed loop.
    bool is_contour = false;
    // Should this extrusion be fuzzyfied on path generation?
    bool fuzzify = false;
};

// Hierarchy of perimeters.
class PerimeterGeneratorLoop
{
public:
    // Polygon of this contour.
    Polygon polygon;
    // Is it a contour or a hole?
    // Contours are CCW oriented, holes are CW oriented.
    bool is_contour;
    // overhang may need to be reversed
    bool is_steep_overhang;
    // Depth in the hierarchy. External perimeter has depth = 0. An external perimeter could be both a contour and a hole.
    unsigned short depth;
    // Should this contur be fuzzyfied on path generation?
    bool fuzzify;
    // Children contour, may be both CCW and CW oriented (outer contours or holes).
    std::vector<PerimeterGeneratorLoop> children;

    PerimeterGeneratorLoop(const Polygon &polygon, unsigned short depth, bool is_contour, bool steep_overhangs, bool fuzzify)
        : polygon(polygon), is_contour(is_contour), is_steep_overhang(steep_overhangs), depth(depth), fuzzify(fuzzify)
    {}
    // External perimeter. It may be CCW or CW oriented (outer contour or hole contour).
    bool is_external() const { return this->depth == 0; }
    // it's the last loop of the contour (not hole), so the first to be printed (if all goes well)
    // An island, which may have holes, but it does not have another internal island.
    bool is_internal_contour() const;
};
typedef std::vector<PerimeterGeneratorLoop> PerimeterGeneratorLoops;

struct ProcessSurfaceResult
{
    ExPolygons inner_perimeter;
    ExPolygons gap_srf;
    ExPolygons top_fills;
    ExPolygons fill_clip;
};

class PerimeterGenerator {
public:
    // Inputs:
    const ExPolygons            *lower_slices;
    const ExPolygons            *slices; // only used by miller
    const ExPolygons            *upper_slices;
    //const Surface               *surface;
    BoundingBox                 surface_bbox;
    Parameters             params;
    std::function<void()>        throw_if_canceled = []() {};
    // Outputs:
    // mask for "inside" perimeter (to remove un-periemter-able areas)
    ExPolygons perimeter_boundary;
    
    PerimeterGenerator(const Parameters &params) : params(params) {}

    void process( // Input:
            const ExPolygon         &island_polygon,
            //LayerRegionIsland        &region_island,
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
            ExPolygons &fill_no_overlap);

    coord_t     get_resolution(size_t perimeter_id, bool is_overhang) const;

private:
    ClipperLib_Z::Paths m_lower_slices_clipperpaths;
    // ClipperLib_Z::Paths _lower_slices_bridge_flow_small_clipperpaths;
    // ClipperLib_Z::Paths _lower_slices_bridge_flow_big_clipperpaths;
    // ClipperLib_Z::Paths _lower_slices_bridge_speed_small_clipperpaths;
    // ClipperLib_Z::Paths _lower_slices_bridge_speed_big_clipperpaths;

    //process data
    // computed params
    ExPolygons unmillable;
    coord_t mill_extra_size;

    ProcessSurfaceResult process_classic(const Parameters &params, int& contour_count, int& holes_count, const ExPolygon& surface_expolygon, ExtrusionEntityCollection &loops, ExtrusionEntityCollection &gapfill);
    ProcessSurfaceResult process_arachne(const Parameters &params, int& loop_number, const ExPolygon& surface_expolygon, ExtrusionEntityCollection &loops);

    ExPolygons processs_no_bridge(const Parameters params, const ExPolygon& island_expolygon, ExPolygons &fill_surfaces);
    ExtrusionPaths create_overhangs_classic(const Parameters &params,
        const Polyline& loop_polygons, const ExtrusionRole role, const bool is_external) const;
    // the bbox is here to accelerate the diffs, loop_polygons is inside it.
    ExtrusionPaths create_overhangs_arachne(const Parameters &params,
        const ClipperLib_Z::Path& loop_polygons, const BoundingBox& bbox, ExtrusionRole role, bool is_external) const;

    enum OverhangType : int{
        NOT_OVERHANG = 0,
        DYNAMIC_OVERHANG,
        SMALL_SPEED_OVERHANG,
        BIG_SPEED_OVERHANG,
        SMALL_FLOW_OVERHANG,
        BIG_FLOW_OVERHANG,
    };
    struct Params_sort_overhangs
    {
        bool is_external;
        bool is_loop;
        bool has_dynamic;
        //size_t layer_height_count;
        Point first_point;
        Point last_point;
        std::vector<int> overhang_type_2_lh;
#ifdef _DEBUG_OVERHANGS
        std::vector<std::string> debug_colors;
        int debug_i = 0;
#endif
    };

    void _sort_overhangs(const Parameters &params,
                         ExtrusionPaths &paths,
                         const ExtrusionRole role,
                         const Params_sort_overhangs &overhangs_params) const;

    // transform loops into ExtrusionEntityCollection, adding also thin walls into it.
    ExtrusionEntityCollection _traverse_loops_classic(const Parameters &params,
        const PerimeterGeneratorLoops &loops, ThickPolylines &thin_walls, int count_since_overhang = -1) const;
    ExtrusionEntityCollection _traverse_extrusions(const Parameters &params,
        std::vector<PerimeterGeneratorArachneExtrusion>& pg_extrusions);
    // try to merge thin walls to a current perimeter exrusion or just add it to the end of the list.
    void _merge_thin_walls(const Parameters &params, ExtrusionEntityCollection &extrusions, ThickPolylines &thin_walls) const;
    // like _traverse_loops but with merging all perimeter into one continuous loop
    ExtrusionLoop _traverse_and_join_loops(const Parameters &params,
        const PerimeterGeneratorLoop &loop, const PerimeterGeneratorLoops &childs, const Point entryPoint) const;
    // sub-function of _traverse_and_join_loops, transform a single loop as a cut extrusion to be merged with an other one.
    ExtrusionLoop _extrude_and_cut_loop(const Parameters &params,
        const PerimeterGeneratorLoop& loop, const Point entryPoint, const Line& direction = Line(Point(0, 0), Point(0, 0)), bool enforce_loop = false) const;
    // sub-function of _traverse_and_join_loops, find the good splot to cut a loop to be able to join it with an other one
    PerimeterIntersectionPoint _get_nearest_point(const Parameters &params,
        const PerimeterGeneratorLoops &children, ExtrusionLoop &myPolylines, const coord_t dist_cut, const coord_t max_dist) const;
    // for one_peri_on_top
    void split_top_surfaces(const ExPolygons *lower_slices,
                            const ExPolygons *upper_slices,
                            const ExPolygons &orig_polygons,
                            ExPolygons &      top_fills,
                            ExPolygons &      non_top_polygons,
                            ExPolygons &      fill_clip,
                            int nb_peri_to_print,
                            coordf_t min_width,
                            bool use_old_algorithm_for_min_width
    );
    // for overhangs_speed_enforce
    bool _enforce_speed_overhangs(ExtrusionPaths &paths, int count_since_overhang) const;

};


} // namespace Slic3r::PerimeterGenerator

#endif
