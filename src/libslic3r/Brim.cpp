#include "clipper/clipper_z.hpp"

#include "ClipperUtils.hpp"
#include "EdgeGrid.hpp"
#include "Fill/Fill.hpp"
#include "Print.hpp"
#include "Layer.hpp"
#include "PrintConfig.hpp"
#include "ShortestPath.hpp"
#include "libslic3r.h"

#include <algorithm>
#include <numeric>
#include <unordered_set>
#include <mutex>

#include <tbb/parallel_for.h>
#include <boost/thread/lock_guard.hpp>

#ifndef NDEBUG
    // #define BRIM_DEBUG_TO_SVG
#endif

#if defined(BRIM_DEBUG_TO_SVG)
    #include "SVG.hpp"
#endif

namespace Slic3r {

// prusaslicer
#if 0 
static void append_and_translate(ExPolygons &dst, const ExPolygons &src, const PrintInstance &instance) {
    size_t dst_idx = dst.size();
    expolygons_append(dst, src);
    for (; dst_idx < dst.size(); ++dst_idx)
        dst[dst_idx].translate(instance.shift.x(), instance.shift.y());
}

static void append_and_translate(Polygons &dst, const Polygons &src, const PrintInstance &instance) {
    size_t dst_idx = dst.size();
    polygons_append(dst, src);
    for (; dst_idx < dst.size(); ++dst_idx)
        dst[dst_idx].translate(instance.shift.x(), instance.shift.y());
}

static float max_brim_width(const ConstPrintObjectPtrsAdaptor &objects)
{
    assert(!objects.empty());
    return float(std::accumulate(objects.begin(), objects.end(), 0.,
                                 [](double partial_result, const PrintObject *object) {
                                     return std::max(partial_result, object->config().brim_type == btNoBrim ? 0. : object->config().brim_width.value);
                                 }));
}

// Returns ExPolygons of the bottom layer of the print object after elephant foot compensation.
static ExPolygons get_print_object_bottom_layer_expolygons(const PrintObject &print_object)
{
    ExPolygons ex_polygons;
    for (LayerRegion *region : print_object.layers().front()->regions())
        Slic3r::append(ex_polygons, closing_ex(region->slices.surfaces, float(SCALED_EPSILON)));
    return ex_polygons;
}

// Returns ExPolygons of bottom layer for every print object in Print after elephant foot compensation.
static std::vector<ExPolygons> get_print_bottom_layers_expolygons(const Print &print)
{
    std::vector<ExPolygons> bottom_layers_expolygons;
    bottom_layers_expolygons.reserve(print.objects().size());
    for (const PrintObject *object : print.objects())
        bottom_layers_expolygons.emplace_back(get_print_object_bottom_layer_expolygons(*object));

    return bottom_layers_expolygons;
}

static ConstPrintObjectPtrs get_top_level_objects_with_brim(const Print &print, const std::vector<ExPolygons> &bottom_layers_expolygons)
{
    assert(print.objects().size() == bottom_layers_expolygons.size());
    Polygons             islands;
    ConstPrintObjectPtrs island_to_object;
    for(size_t print_object_idx = 0; print_object_idx < print.objects().size(); ++print_object_idx) {
        const PrintObject *object = print.objects()[print_object_idx];
        Polygons islands_object;
        islands_object.reserve(bottom_layers_expolygons[print_object_idx].size());
        for (const ExPolygon &ex_poly : bottom_layers_expolygons[print_object_idx])
            islands_object.emplace_back(ex_poly.contour);

        islands.reserve(islands.size() + object->instances().size() * islands_object.size());
        for (const PrintInstance &instance : object->instances())
            for (Polygon &poly : islands_object) {
                islands.emplace_back(poly);
                islands.back().translate(instance.shift);
                island_to_object.emplace_back(object);
            }
    }
    assert(islands.size() == island_to_object.size());

    ClipperLib_Z::Paths islands_clip;
    islands_clip.reserve(islands.size());
    for (const Polygon &poly : islands) {
        islands_clip.emplace_back();
        ClipperLib_Z::Path &island_clip = islands_clip.back();
        island_clip.reserve(poly.points.size());
        int island_idx = int(&poly - &islands.front());
        // The Z coordinate carries index of the island used to get the pointer to the object.
        for (const Point &pt : poly.points)
            island_clip.emplace_back(pt.x(), pt.y(), island_idx + 1);
    }

    // Init Clipper
    ClipperLib_Z::Clipper clipper;
    // Assign the maximum Z from four points. This values is valid index of the island
    clipper.ZFillFunction([](const ClipperLib_Z::IntPoint &e1bot, const ClipperLib_Z::IntPoint &e1top, const ClipperLib_Z::IntPoint &e2bot,
                             const ClipperLib_Z::IntPoint &e2top, ClipperLib_Z::IntPoint &pt) {
        pt.z() = std::max(std::max(e1bot.z(), e1top.z()), std::max(e2bot.z(), e2top.z()));
    });
    // Add islands
    clipper.AddPaths(islands_clip, ClipperLib_Z::ptSubject, true);
    // Execute union operation to construct polytree
    ClipperLib_Z::PolyTree islands_polytree;
    //FIXME likely pftNonZero or ptfPositive would be better. Why are we using ptfEvenOdd for Unions?
    clipper.Execute(ClipperLib_Z::ctUnion, islands_polytree, ClipperLib_Z::pftEvenOdd, ClipperLib_Z::pftEvenOdd);

    std::unordered_set<size_t> processed_objects_idx;
    ConstPrintObjectPtrs       top_level_objects_with_brim;
    for (int i = 0; i < islands_polytree.ChildCount(); ++i) {
        for (const ClipperLib_Z::IntPoint &point : islands_polytree.Childs[i]->Contour) {
            if (point.z() != 0 && processed_objects_idx.find(island_to_object[point.z() - 1]->id().id) == processed_objects_idx.end()) {
                top_level_objects_with_brim.emplace_back(island_to_object[point.z() - 1]);
                processed_objects_idx.insert(island_to_object[point.z() - 1]->id().id);
            }
        }
    }
    return top_level_objects_with_brim;
}

static Polygons top_level_outer_brim_islands(const ConstPrintObjectPtrs &top_level_objects_with_brim, const double scaled_resolution)
{
    Polygons islands;
    for (const PrintObject *object : top_level_objects_with_brim) {
        if (!object->has_brim())
            continue;

        //FIXME how about the brim type?
        auto     brim_separation = float(scale_(object->config().brim_separation.value));
        Polygons islands_object;
        for (const ExPolygon &ex_poly : get_print_object_bottom_layer_expolygons(*object)) {
            Polygons contour_offset = offset(ex_poly.contour, brim_separation, ClipperLib::jtSquare);
            for (Polygon &poly : contour_offset)
                poly.douglas_peucker(scaled_resolution);

            polygons_append(islands_object, std::move(contour_offset));
        }

        for (const PrintInstance &instance : object->instances())
            append_and_translate(islands, islands_object, instance);
    }
    return islands;
}

static ExPolygons top_level_outer_brim_area(const Print                   &print,
                                            const ConstPrintObjectPtrs    &top_level_objects_with_brim,
                                            const std::vector<ExPolygons> &bottom_layers_expolygons,
                                            const float                    no_brim_offset)
{
    assert(print.objects().size() == bottom_layers_expolygons.size());
    std::unordered_set<size_t> top_level_objects_idx;
    top_level_objects_idx.reserve(top_level_objects_with_brim.size());
    for (const PrintObject *object : top_level_objects_with_brim)
        top_level_objects_idx.insert(object->id().id);

    ExPolygons brim_area;
    ExPolygons no_brim_area;
    for(size_t print_object_idx = 0; print_object_idx < print.objects().size(); ++print_object_idx) {
        const PrintObject *object            = print.objects()[print_object_idx];
        const BrimType     brim_type         = object->config().brim_type.value;
        const float        brim_separation   = scale_(object->config().brim_separation.value);
        const float        brim_width        = scale_(object->config().brim_width.value);
        const bool         is_top_outer_brim = top_level_objects_idx.find(object->id().id) != top_level_objects_idx.end();

        ExPolygons brim_area_object;
        ExPolygons no_brim_area_object;
        for (const ExPolygon &ex_poly : bottom_layers_expolygons[print_object_idx]) {
            if ((brim_type == BrimType::btOuterOnly || brim_type == BrimType::btOuterAndInner) && is_top_outer_brim)
                append(brim_area_object, diff_ex(offset(ex_poly.contour, brim_width + brim_separation, ClipperLib::jtSquare), offset(ex_poly.contour, brim_separation, ClipperLib::jtSquare)));

            // After 7ff76d07684858fd937ef2f5d863f105a10f798e offset and shrink don't work with CW polygons (holes), so let's make it CCW.
            Polygons ex_poly_holes_reversed = ex_poly.holes;
            polygons_reverse(ex_poly_holes_reversed);
            if (brim_type == BrimType::btOuterOnly || brim_type == BrimType::btNoBrim)
                append(no_brim_area_object, shrink_ex(ex_poly_holes_reversed, no_brim_offset, ClipperLib::jtSquare));

            if (brim_type == BrimType::btInnerOnly || brim_type == BrimType::btNoBrim)
                append(no_brim_area_object, diff_ex(offset(ex_poly.contour, no_brim_offset, ClipperLib::jtSquare), ex_poly_holes_reversed));

            if (brim_type != BrimType::btNoBrim)
                append(no_brim_area_object, offset_ex(ExPolygon(ex_poly.contour), brim_separation, ClipperLib::jtSquare));

            no_brim_area_object.emplace_back(ex_poly.contour);
        }

        for (const PrintInstance &instance : object->instances()) {
            append_and_translate(brim_area, brim_area_object, instance);
            append_and_translate(no_brim_area, no_brim_area_object, instance);
        }
    }

    return diff_ex(brim_area, no_brim_area);
}

// Return vector of booleans indicated if polygons from bottom_layers_expolygons contain another polygon or not.
// Every ExPolygon is counted as several Polygons (contour and holes). Contour polygon is always processed before holes.
static std::vector<bool> has_polygons_nothing_inside(const Print &print, const std::vector<ExPolygons> &bottom_layers_expolygons)
{
    assert(print.objects().size() == bottom_layers_expolygons.size());
    Polygons islands;
    for(size_t print_object_idx = 0; print_object_idx < print.objects().size(); ++print_object_idx) {
        const PrintObject *object         = print.objects()[print_object_idx];
        const Polygons     islands_object = to_polygons(bottom_layers_expolygons[print_object_idx]);

        islands.reserve(islands.size() + object->instances().size() * islands_object.size());
        for (const PrintInstance &instance : object->instances())
            append_and_translate(islands, islands_object, instance);
    }

    ClipperLib_Z::Paths islands_clip;
    islands_clip.reserve(islands.size());
    for (const Polygon &poly : islands) {
        size_t             island_idx = &poly - &islands.front();
        ClipperLib_Z::Path island_clip;
        for (const Point &pt : poly.points)
            island_clip.emplace_back(pt.x(), pt.y(), island_idx + 1);
        islands_clip.emplace_back(island_clip);
    }

    ClipperLib_Z::Clipper clipper;
    // Always assign zero to detect cases when two polygons are overlapping.
    clipper.ZFillFunction([](const ClipperLib_Z::IntPoint &e1bot, const ClipperLib_Z::IntPoint &e1top, const ClipperLib_Z::IntPoint &e2bot, const ClipperLib_Z::IntPoint &e2top, ClipperLib_Z::IntPoint &pt) {
        pt.z() = 0;
    });

    clipper.AddPaths(islands_clip, ClipperLib_Z::ptSubject, true);
    ClipperLib_Z::PolyTree islands_polytree;
    clipper.Execute(ClipperLib_Z::ctUnion, islands_polytree, ClipperLib_Z::pftEvenOdd, ClipperLib_Z::pftEvenOdd);

    std::vector<bool> has_nothing_inside(islands.size());
    std::function<void(const ClipperLib_Z::PolyNode&)> check_contours = [&check_contours, &has_nothing_inside](const ClipperLib_Z::PolyNode &parent_node)->void {
        if (!parent_node.Childs.empty())
            for(const ClipperLib_Z::PolyNode *child_node : parent_node.Childs)
                check_contours(*child_node);

        if (parent_node.Childs.empty() && !parent_node.Contour.empty() && parent_node.Contour.front().z() != 0) {
            int polygon_idx = parent_node.Contour.front().z();
            assert(polygon_idx > 0 && polygon_idx <= int(has_nothing_inside.size()));

            // The whole contour must have the same ID. In other cases, some counters overlap.
            for (const ClipperLib_Z::IntPoint &point : parent_node.Contour)
                if (polygon_idx != point.z())
                    return;

            has_nothing_inside[polygon_idx - 1] = true;
        }
    };

    check_contours(islands_polytree);
    return has_nothing_inside;
}

// INNERMOST means that ExPolygon doesn't contain any other ExPolygons.
// NORMAL is for other cases.
enum class InnerBrimType {NORMAL, INNERMOST};

struct InnerBrimExPolygons
{
    ExPolygons    brim_area;
    InnerBrimType type       = InnerBrimType::NORMAL;
    double        brim_width = 0.;
};

static std::vector<InnerBrimExPolygons> inner_brim_area(const Print                   &print,
                                                        const ConstPrintObjectPtrs    &top_level_objects_with_brim,
                                                        const std::vector<ExPolygons> &bottom_layers_expolygons,
                                                        const float                    no_brim_offset)
{
    assert(print.objects().size() == bottom_layers_expolygons.size());
    std::vector<bool>          has_nothing_inside = has_polygons_nothing_inside(print, bottom_layers_expolygons);
    std::unordered_set<size_t> top_level_objects_idx;
    top_level_objects_idx.reserve(top_level_objects_with_brim.size());
    for (const PrintObject *object : top_level_objects_with_brim)
        top_level_objects_idx.insert(object->id().id);

    std::vector<ExPolygons> brim_area_innermost(print.objects().size());
    ExPolygons              brim_area;
    ExPolygons              no_brim_area;
    Polygons                holes_reversed;

    // polygon_idx must correspond to idx generated inside has_polygons_nothing_inside()
    size_t polygon_idx = 0;
    for(size_t print_object_idx = 0; print_object_idx < print.objects().size(); ++print_object_idx) {
        const PrintObject *object          = print.objects()[print_object_idx];
        const BrimType     brim_type       = object->config().brim_type.value;
        const float        brim_separation = scale_(object->config().brim_separation.value);
        const float        brim_width      = scale_(object->config().brim_width.value);
        const bool         top_outer_brim  = top_level_objects_idx.find(object->id().id) != top_level_objects_idx.end();

        ExPolygons brim_area_innermost_object;
        ExPolygons brim_area_object;
        ExPolygons no_brim_area_object;
        Polygons   holes_reversed_object;
        for (const ExPolygon &ex_poly : bottom_layers_expolygons[print_object_idx]) {
            if (brim_type == BrimType::btOuterOnly || brim_type == BrimType::btOuterAndInner) {
                if (top_outer_brim)
                    no_brim_area_object.emplace_back(ex_poly);
                else
                    append(brim_area_object, diff_ex(offset(ex_poly.contour, brim_width + brim_separation, ClipperLib::jtSquare), offset(ex_poly.contour, brim_separation, ClipperLib::jtSquare)));
            }

            // After 7ff76d07684858fd937ef2f5d863f105a10f798e offset and shrink don't work with CW polygons (holes), so let's make it CCW.
            Polygons ex_poly_holes_reversed = ex_poly.holes;
            polygons_reverse(ex_poly_holes_reversed);
            for ([[maybe_unused]] const PrintInstance &instance : object->instances()) {
                ++polygon_idx; // Increase idx because of the contour of the ExPolygon.

                if (brim_type == BrimType::btInnerOnly || brim_type == BrimType::btOuterAndInner)
                    for(const Polygon &hole : ex_poly_holes_reversed) {
                        size_t hole_idx = &hole - &ex_poly_holes_reversed.front();
                        if (has_nothing_inside[polygon_idx + hole_idx])
                            append(brim_area_innermost_object, shrink_ex({hole}, brim_separation, ClipperLib::jtSquare));
                        else
                            append(brim_area_object, diff_ex(shrink_ex({hole}, brim_separation, ClipperLib::jtSquare), shrink_ex({hole}, brim_width + brim_separation, ClipperLib::jtSquare)));
                    }

                polygon_idx += ex_poly.holes.size(); // Increase idx for every hole of the ExPolygon.
            }

            if (brim_type == BrimType::btInnerOnly || brim_type == BrimType::btNoBrim)
                append(no_brim_area_object, diff_ex(offset(ex_poly.contour, no_brim_offset, ClipperLib::jtSquare), ex_poly_holes_reversed));

            if (brim_type == BrimType::btOuterOnly || brim_type == BrimType::btNoBrim)
                append(no_brim_area_object, diff_ex(ExPolygon(ex_poly.contour), shrink_ex(ex_poly_holes_reversed, no_brim_offset, ClipperLib::jtSquare)));

            append(holes_reversed_object, ex_poly_holes_reversed);
        }
        append(no_brim_area_object, offset_ex(bottom_layers_expolygons[print_object_idx], brim_separation, ClipperLib::jtSquare));

        for (const PrintInstance &instance : object->instances()) {
            append_and_translate(brim_area_innermost[print_object_idx], brim_area_innermost_object, instance);
            append_and_translate(brim_area, brim_area_object, instance);
            append_and_translate(no_brim_area, no_brim_area_object, instance);
            append_and_translate(holes_reversed, holes_reversed_object, instance);
        }
    }
    assert(polygon_idx == has_nothing_inside.size());

    ExPolygons brim_area_innermost_merged;
    // Append all innermost brim areas.
    std::vector<InnerBrimExPolygons> brim_area_out;
    for (size_t print_object_idx = 0; print_object_idx < print.objects().size(); ++print_object_idx)
        if (const double brim_width = print.objects()[print_object_idx]->config().brim_width.value; !brim_area_innermost[print_object_idx].empty()) {
            append(brim_area_innermost_merged, brim_area_innermost[print_object_idx]);
            brim_area_out.push_back({std::move(brim_area_innermost[print_object_idx]), InnerBrimType::INNERMOST, brim_width});
        }

    // Append all normal brim areas.
    brim_area_out.push_back({diff_ex(intersection_ex(to_polygons(std::move(brim_area)), holes_reversed), no_brim_area), InnerBrimType::NORMAL});

    // Cut out a huge brim areas that overflows into the INNERMOST holes.
    brim_area_out.back().brim_area = diff_ex(brim_area_out.back().brim_area, brim_area_innermost_merged);
    return brim_area_out;
}

// Flip orientation of open polylines to minimize travel distance.
static void optimize_polylines_by_reversing(Polylines *polylines)
{
    for (size_t poly_idx = 1; poly_idx < polylines->size(); ++poly_idx) {
        const Polyline &prev = (*polylines)[poly_idx - 1];
        Polyline &      next = (*polylines)[poly_idx];

        if (!next.is_closed()) {
            double dist_to_start = (next.first_point() - prev.last_point()).cast<double>().norm();
            double dist_to_end   = (next.last_point() - prev.last_point()).cast<double>().norm();

            if (dist_to_end < dist_to_start) 
                next.reverse();
        }
    }
}

static Polylines connect_brim_lines(Polylines &&polylines, const Polygons &brim_area, float max_connection_length)
{
    if (polylines.empty())
        return {};

    BoundingBox bbox = get_extents(polylines);
    bbox.merge(get_extents(brim_area));

    EdgeGrid::Grid grid(bbox.inflated(SCALED_EPSILON));
    grid.create(brim_area, polylines, coord_t(scale_(10.)));

    struct Visitor
    {
        explicit Visitor(const EdgeGrid::Grid &grid) : grid(grid) {}

        bool operator()(coord_t iy, coord_t ix)
        {
            // Called with a row and colum of the grid cell, which is intersected by a line.
            auto cell_data_range = grid.cell_data_range(iy, ix);
            this->intersect      = false;
            for (auto it_contour_and_segment = cell_data_range.first; it_contour_and_segment != cell_data_range.second; ++it_contour_and_segment) {
                // End points of the line segment and their vector.
                auto segment = grid.segment(*it_contour_and_segment);
                if (Geometry::segments_intersect(segment.first, segment.second, brim_line.a, brim_line.b)) {
                    this->intersect = true;
                    return false;
                }
            }
            // Continue traversing the grid along the edge.
            return true;
        }

        const EdgeGrid::Grid &grid;
        Line                  brim_line;
        bool                  intersect = false;

    } visitor(grid);

    // Connect successive polylines if they are open, their ends are closer than max_connection_length.
    // Remove empty polylines.
    {
        // Skip initial empty lines.
        size_t poly_idx = 0;
        for (; poly_idx < polylines.size() && polylines[poly_idx].empty(); ++ poly_idx) ;
        size_t end = ++ poly_idx;
        double max_connection_length2 = Slic3r::sqr(max_connection_length);
        for (; poly_idx < polylines.size(); ++poly_idx) {
            Polyline &next = polylines[poly_idx];
            if (! next.empty()) {
                Polyline &prev = polylines[end - 1];
                bool   connect = false;
                if (! prev.is_closed() && ! next.is_closed()) {
                    double dist2 = (prev.last_point() - next.first_point()).cast<double>().squaredNorm();
                    if (dist2 <= max_connection_length2) {
                        visitor.brim_line.a = prev.last_point();
                        visitor.brim_line.b = next.first_point();
                        // Shrink the connection line to avoid collisions with the brim centerlines.
                        visitor.brim_line.extend(-SCALED_EPSILON);
                        grid.visit_cells_intersecting_line(visitor.brim_line.a, visitor.brim_line.b, visitor);
                        connect = ! visitor.intersect;
                    }
                }
                if (connect) {
                    append(prev.points, std::move(next.points));
                } else {
                    if (end < poly_idx)
                        polylines[end] = std::move(next);
                    ++ end;
                }
            }
        }
        if (end < polylines.size())
            polylines.erase(polylines.begin() + int(end), polylines.end());
    }

    return std::move(polylines);
}

static void make_inner_brim(const Print                   &print,
                            const ConstPrintObjectPtrs    &top_level_objects_with_brim,
                            const std::vector<ExPolygons> &bottom_layers_expolygons,
                            ExtrusionEntityCollection     &brim)
{
    assert(print.objects().size() == bottom_layers_expolygons.size());
    const auto                       scaled_resolution = scaled<double>(print.config().resolution_internal.value);
    Flow                             flow              = print.brim_flow();
    std::vector<InnerBrimExPolygons> inner_brims_ex    = inner_brim_area(print, top_level_objects_with_brim, bottom_layers_expolygons, float(flow.scaled_spacing()));
    Polygons                         loops;
    std::mutex                       loops_mutex;
    tbb::parallel_for(tbb::blocked_range<size_t>(0, inner_brims_ex.size()), [&inner_brims_ex, &flow, &scaled_resolution, &loops, &loops_mutex](const tbb::blocked_range<size_t> &range) {
        for (size_t brim_idx = range.begin(); brim_idx < range.end(); ++brim_idx) {
            const InnerBrimExPolygons &inner_brim_ex = inner_brims_ex[brim_idx];
            auto                       num_loops     = size_t(floor(inner_brim_ex.brim_width / flow.spacing()));
            ExPolygons                 islands_ex    = offset_ex(inner_brim_ex.brim_area, -0.5f * float(flow.scaled_spacing()), ClipperLib::jtSquare);
            for (size_t i = 0; (inner_brim_ex.type == InnerBrimType::INNERMOST ? i < num_loops : !islands_ex.empty()); ++i) {
                for (ExPolygon &poly_ex : islands_ex)
                    poly_ex.douglas_peucker(scaled_resolution);

                {
                    boost::lock_guard<std::mutex> lock(loops_mutex);
                    polygons_append(loops, to_polygons(islands_ex));
                }
                islands_ex = offset_ex(islands_ex, -float(flow.scaled_spacing()), ClipperLib::jtSquare);
            }
        }
    }); // end of parallel_for

    loops = union_pt_chained_outside_in(loops);
    std::reverse(loops.begin(), loops.end());
    extrusion_entities_append_loops(brim.entities, std::move(loops), erSkirt, float(flow.mm3_per_mm()),
                                    float(flow.width()), float(print.skirt_first_layer_height()));
}

// Produce brim lines around those objects, that have the brim enabled.
// Collect islands_area to be merged into the final 1st layer convex hull.
ExtrusionEntityCollection make_brim(const Print &print, PrintTryCancel try_cancel, Polygons &islands_area)
{
    const auto              scaled_resolution           = scaled<double>(print.config().resolution_internal.value);
    Flow                    flow                        = print.brim_flow();
    std::vector<ExPolygons> bottom_layers_expolygons    = get_print_bottom_layers_expolygons(print);
    ConstPrintObjectPtrs    top_level_objects_with_brim = get_top_level_objects_with_brim(print, bottom_layers_expolygons);
    Polygons                islands                     = top_level_outer_brim_islands(top_level_objects_with_brim, scaled_resolution);
    ExPolygons              islands_area_ex             = top_level_outer_brim_area(print, top_level_objects_with_brim, bottom_layers_expolygons, float(flow.scaled_spacing()));
    islands_area                                        = to_polygons(islands_area_ex);

    Polygons        loops;
    size_t          num_loops = size_t(floor(max_brim_width(print.objects()) / flow.spacing()));
    for (size_t i = 0; i < num_loops; ++i) {
        try_cancel();
        islands = expand(islands, float(flow.scaled_spacing()), ClipperLib::jtSquare);
        for (Polygon &poly : islands) 
            poly.douglas_peucker(scaled_resolution);
        polygons_append(loops, shrink(islands, 0.5f * float(flow.scaled_spacing())));
    }
    loops = union_pt_chained_outside_in(loops);

    std::vector<Polylines> loops_pl_by_levels;
    {
        Polylines              loops_pl = to_polylines(loops);
        loops_pl_by_levels.assign(loops_pl.size(), Polylines());
        tbb::parallel_for(tbb::blocked_range<size_t>(0, loops_pl.size()),
            [&loops_pl_by_levels, &loops_pl, &islands_area](const tbb::blocked_range<size_t> &range) {
                for (size_t i = range.begin(); i < range.end(); ++i) {
                    loops_pl_by_levels[i] = chain_polylines(intersection_pl({ std::move(loops_pl[i]) }, islands_area));
                }
            });
    }

    // output
    ExtrusionEntityCollection brim;

    // Reduce down to the ordered list of polylines.
    Polylines all_loops;
    for (Polylines &polylines : loops_pl_by_levels)
        append(all_loops, std::move(polylines));
    loops_pl_by_levels.clear();

    // Flip orientation of open polylines to minimize travel distance.
    optimize_polylines_by_reversing(&all_loops);

#ifdef BRIM_DEBUG_TO_SVG
    static int irun = 0;
    ++ irun;

    {
        SVG svg(debug_out_path("brim-%d.svg", irun).c_str(), get_extents(all_loops));
        svg.draw(union_ex(islands), "blue");
        svg.draw(islands_area_ex, "green");
        svg.draw(all_loops, "black", coord_t(scale_(0.1)));
    }
#endif // BRIM_DEBUG_TO_SVG

    all_loops = connect_brim_lines(std::move(all_loops), offset(islands_area_ex, float(SCALED_EPSILON)), float(flow.scaled_spacing()) * 2.f);

#ifdef BRIM_DEBUG_TO_SVG
    {
        SVG svg(debug_out_path("brim-connected-%d.svg", irun).c_str(), get_extents(all_loops));
        svg.draw(union_ex(islands), "blue");
        svg.draw(islands_area_ex, "green");
        svg.draw(all_loops, "black", coord_t(scale_(0.1)));
    }
#endif // BRIM_DEBUG_TO_SVG

    const bool could_brim_intersects_skirt = std::any_of(print.objects().begin(), print.objects().end(), [&print](PrintObject *object) {
        const BrimType &bt = object->config().brim_type;
        return (bt == btOuterOnly || bt == btOuterAndInner) && print.config().skirt_distance.value < object->config().brim_width;
    });

    const bool draft_shield = print.config().draft_shield != dsDisabled;


    // If there is a possibility that brim intersects skirt, go through loops and split those extrusions
    // The result is either the original Polygon or a list of Polylines
    if (draft_shield && ! print.skirt().empty() && could_brim_intersects_skirt)
    {
        // Find the bounding polygons of the skirt
        const Polygons skirt_inners = offset(dynamic_cast<ExtrusionLoop*>(print.skirt().entities.back())->polygon(),
                                              -float(scale_(print.skirt_flow().spacing()))/2.f,
                                              ClipperLib::jtRound,
                                              float(scale_(0.1)));
        const Polygons skirt_outers = offset(dynamic_cast<ExtrusionLoop*>(print.skirt().entities.front())->polygon(),
                                              float(scale_(print.skirt_flow().spacing()))/2.f,
                                              ClipperLib::jtRound,
                                              float(scale_(0.1)));

        // First calculate the trimming region.
		ClipperLib_Z::Paths trimming;
		{
		    ClipperLib_Z::Paths input_subject;
		    ClipperLib_Z::Paths input_clip;
		    for (const Polygon &poly : skirt_outers) {
		    	input_subject.emplace_back();
		    	ClipperLib_Z::Path &out = input_subject.back();
		    	out.reserve(poly.points.size());
			    for (const Point &pt : poly.points)
					out.emplace_back(pt.x(), pt.y(), 0);
		    }
		    for (const Polygon &poly : skirt_inners) {
		    	input_clip.emplace_back();
		    	ClipperLib_Z::Path &out = input_clip.back();
		    	out.reserve(poly.points.size());
			    for (const Point &pt : poly.points)
					out.emplace_back(pt.x(), pt.y(), 0);
		    }
		    // init Clipper
		    ClipperLib_Z::Clipper clipper;
		    // add polygons
		    clipper.AddPaths(input_subject, ClipperLib_Z::ptSubject, true);
		    clipper.AddPaths(input_clip,    ClipperLib_Z::ptClip,    true);
		    // perform operation
		    clipper.Execute(ClipperLib_Z::ctDifference, trimming, ClipperLib_Z::pftNonZero, ClipperLib_Z::pftNonZero);
		}

		// Second, trim the extrusion loops with the trimming regions.
		ClipperLib_Z::Paths loops_trimmed;
		{
            // Produce ClipperLib_Z::Paths from polylines (not necessarily closed).
			ClipperLib_Z::Paths input_clip;
			for (const Polyline &loop_pl : all_loops) {
				input_clip.emplace_back();
				ClipperLib_Z::Path& out = input_clip.back();
				out.reserve(loop_pl.points.size());
				int64_t loop_idx = &loop_pl - &all_loops.front();
				for (const Point& pt : loop_pl.points)
					// The Z coordinate carries index of the source loop.
					out.emplace_back(pt.x(), pt.y(), loop_idx + 1);
			}
			// init Clipper
			ClipperLib_Z::Clipper clipper;
			clipper.ZFillFunction([](const ClipperLib_Z::IntPoint& e1bot, const ClipperLib_Z::IntPoint& e1top, const ClipperLib_Z::IntPoint& e2bot, const ClipperLib_Z::IntPoint& e2top, ClipperLib_Z::IntPoint& pt) {
				// Assign a valid input loop identifier. Such an identifier is strictly positive, the next line is safe even in case one side of a segment
				// hat the Z coordinate not set to the contour coordinate.
				pt.z() = std::max(std::max(e1bot.z(), e1top.z()), std::max(e2bot.z(), e2top.z()));
			});
			// add polygons
			clipper.AddPaths(input_clip, ClipperLib_Z::ptSubject, false);
			clipper.AddPaths(trimming,   ClipperLib_Z::ptClip,    true);
			// perform operation
			ClipperLib_Z::PolyTree loops_trimmed_tree;
			clipper.Execute(ClipperLib_Z::ctDifference, loops_trimmed_tree, ClipperLib_Z::pftNonZero, ClipperLib_Z::pftNonZero);
			ClipperLib_Z::PolyTreeToPaths(loops_trimmed_tree, loops_trimmed);
		}

		// Third, produce the extrusions, sorted by the source loop indices.
		{
			std::vector<std::pair<const ClipperLib_Z::Path*, size_t>> loops_trimmed_order;
			loops_trimmed_order.reserve(loops_trimmed.size());
			for (const ClipperLib_Z::Path &path : loops_trimmed) {
				size_t input_idx = 0;
				for (const ClipperLib_Z::IntPoint &pt : path)
					if (pt.z() > 0) {
						input_idx = (size_t)pt.z();
						break;
					}
				assert(input_idx != 0);
				loops_trimmed_order.emplace_back(&path, input_idx);
			}
			std::stable_sort(loops_trimmed_order.begin(), loops_trimmed_order.end(),
				[](const std::pair<const ClipperLib_Z::Path*, size_t> &l, const std::pair<const ClipperLib_Z::Path*, size_t> &r) {
					return l.second < r.second;
				});

			Point last_pt(0, 0);
			for (size_t i = 0; i < loops_trimmed_order.size();) {
				// Find all pieces that the initial loop was split into.
				size_t j = i + 1;
                for (; j < loops_trimmed_order.size() && loops_trimmed_order[i].second == loops_trimmed_order[j].second; ++ j) ;
                const ClipperLib_Z::Path &first_path = *loops_trimmed_order[i].first;
				if (i + 1 == j && first_path.size() > 3 && first_path.front().x() == first_path.back().x() && first_path.front().y() == first_path.back().y()) {
					auto *loop = new ExtrusionLoop();
                    brim.entities.emplace_back(loop);
					loop->paths.emplace_back(erSkirt, float(flow.mm3_per_mm()), float(flow.width()), float(print.skirt_first_layer_height()));
		            Points &points = loop->paths.front().polyline.points;
		            points.reserve(first_path.size());
		            for (const ClipperLib_Z::IntPoint &pt : first_path)
		            	points.emplace_back(coord_t(pt.x()), coord_t(pt.y()));
		            i = j;
				} else {
			    	//FIXME The path chaining here may not be optimal.
			    	ExtrusionEntityCollection this_loop_trimmed;
					this_loop_trimmed.entities.reserve(j - i);
			    	for (; i < j; ++ i) {
			            this_loop_trimmed.entities.emplace_back(new ExtrusionPath(erSkirt, float(flow.mm3_per_mm()), float(flow.width()), float(print.skirt_first_layer_height())));
						const ClipperLib_Z::Path &path = *loops_trimmed_order[i].first;
			            Points &points = dynamic_cast<ExtrusionPath*>(this_loop_trimmed.entities.back())->polyline.points;
			            points.reserve(path.size());
			            for (const ClipperLib_Z::IntPoint &pt : path)
			            	points.emplace_back(coord_t(pt.x()), coord_t(pt.y()));
		           	}
		           	chain_and_reorder_extrusion_entities(this_loop_trimmed.entities, &last_pt);
                    brim.entities.reserve(brim.entities.size() + this_loop_trimmed.entities.size());
		           	append(brim.entities, std::move(this_loop_trimmed.entities));
		           	this_loop_trimmed.entities.clear();
		        }
		        last_pt = brim.last_point();
			}
		}
    } else {
        extrusion_entities_append_loops_and_paths(brim.entities, std::move(all_loops), erSkirt, float(flow.mm3_per_mm()), float(flow.width()), float(print.skirt_first_layer_height()));
    }

    make_inner_brim(print, top_level_objects_with_brim, bottom_layers_expolygons, brim);
    return brim;
}
#endif

//superslicer

void extrude_brim_from_tree(const Print& print, std::vector<std::vector<BrimLoop>>& loops, const Polygons& frontiers, const Flow& flow, ExtrusionEntityCollection& out, bool reversed/*= false*/) {
    if (loops.empty())
        return;

    // nest contour loops (same as in perimetergenerator)
    for (int d = loops.size() - 1; d >= 1; --d) {
        std::vector<BrimLoop>& contours_d = loops[d];
        // loop through all contours having depth == d
        for (int i = 0; i < (int)contours_d.size(); ++i) {
            const BrimLoop& loop = contours_d[i];
            // find the contour loop that contains it
            for (int t = d - 1; t >= 0; --t) {
                for (size_t j = 0; j < loops[t].size(); ++j) {
                    BrimLoop& candidate_parent = loops[t][j];
                    bool test = reversed
                        ? loop.polygon().contains(candidate_parent.lines.front().first_point())
                        : candidate_parent.polygon().contains(loop.lines.front().first_point());
                    if (test) {
                        candidate_parent.children.push_back(loop);
                        contours_d.erase(contours_d.begin() + i);
                        --i;
                        goto NEXT_CONTOUR;
                    }
                }
            }
            //didn't find a contour: add it as a root loop
            loops[0].push_back(loop);
            contours_d.erase(contours_d.begin() + i);
            --i;
        NEXT_CONTOUR:;
        }
    }
    for (int i = loops.size() - 1; i > 0; --i) {
        if (loops[i].empty()) {
            loops.erase(loops.begin() + i);
        }
    }

    //def
    //cut loops if they go inside a forbidden region
    std::function<void(BrimLoop&)> cut_loop = [&frontiers, &flow, reversed](BrimLoop& to_cut) {
        Polylines result;
        if (to_cut.is_loop) {
            result = intersection_pl(Polygons{ to_cut.polygon() }, frontiers);
        } else {
            result = intersection_pl(to_cut.lines, frontiers);
        }
        //remove too small segments
        for (int i = 0; i < result.size(); i++) {
            if (result[i].length() < flow.scaled_width() * 2) {
                result.erase(result.begin() + i);
                i--;
            }
        }
        if (result.empty()) {
            to_cut.lines.clear();
        } else {
            //if (to_cut.lines != result) {
                to_cut.lines = result;
                if (reversed) {
                    std::reverse(to_cut.lines.begin(), to_cut.lines.end());
                }
                to_cut.is_loop = false;
            //}
        }

    };
    //calls, deep-first
    std::list< std::pair<BrimLoop*, int>> cut_child_first;
    for (std::vector<BrimLoop>& loops : loops) {
        for (BrimLoop& loop : loops) {
            cut_child_first.emplace_front(&loop, 0);
            //flat recurtion
            while (!cut_child_first.empty()) {
                if (cut_child_first.front().first->children.size() <= cut_child_first.front().second) {
                    //if no child to cut, cut ourself and pop
                    cut_loop(*cut_child_first.front().first);
                    cut_child_first.pop_front();
                } else {
                    // more child to cut, push the next
                    cut_child_first.front().second++;
                    cut_child_first.emplace_front(&cut_child_first.front().first->children[cut_child_first.front().second - 1], 0);
                }
            }
        }
    }

    print.throw_if_canceled();


    //def: push into extrusions, in the right order
    float mm3_per_mm = float(flow.mm3_per_mm());
    float width = float(flow.width());
    float height = float(print.get_first_layer_height());
    int nextIdx = 0;
    std::function<void(BrimLoop&, ExtrusionEntityCollection*)>* extrude_ptr;
    std::function<void(BrimLoop&, ExtrusionEntityCollection*) > extrude = [&mm3_per_mm, &width, &height, &extrude_ptr, &nextIdx](BrimLoop& to_cut, ExtrusionEntityCollection* parent) {
        int idx = nextIdx++;
        //bool i_have_line = !to_cut.line.points.empty() && to_cut.line.is_valid();
        bool i_have_line = to_cut.lines.size() > 0 && to_cut.lines.front().size() > 0 && to_cut.lines.front().is_valid();
        if (!i_have_line && to_cut.children.empty()) {
            //nothing
        } else if (i_have_line && to_cut.children.empty()) {
            ExtrusionEntitiesPtr to_add;
            for (Polyline& pline : to_cut.lines) {
                assert(pline.size() > 0);
                if (pline.back() == pline.front()) {
                    ExtrusionPath path(erSkirt, mm3_per_mm, width, height);
                    path.polyline = pline;
                    to_add.push_back(new ExtrusionLoop(std::move(path), elrSkirt));
                } else {
                    ExtrusionPath* extrusion_path = new ExtrusionPath(erSkirt, mm3_per_mm, width, height);
                    to_add.push_back(extrusion_path);
                    extrusion_path->polyline = pline;
                }
            }
            parent->append(std::move(to_add));
        } else if (!i_have_line && !to_cut.children.empty()) {
            if (to_cut.children.size() == 1) {
                (*extrude_ptr)(to_cut.children[0], parent);
            } else {
                ExtrusionEntityCollection* mycoll = new ExtrusionEntityCollection();
                //mycoll->no_sort = true;
                for (BrimLoop& child : to_cut.children)
                    (*extrude_ptr)(child, mycoll);
                //remove un-needed collection if possible
                if (mycoll->entities().size() == 1) {
                    parent->append(*mycoll->entities().front()); //add clone
                    delete mycoll; // remove coll & content
                } else if (mycoll->entities().size() == 0) {
                    delete mycoll;// remove coll & content
                } else {
                    parent->append(ExtrusionEntitiesPtr{ mycoll });
                }
            }
        } else {
            ExtrusionEntityCollection* print_me_first = new ExtrusionEntityCollection();
            print_me_first->set_can_sort_reverse(false, false);
            parent->append({ print_me_first });
            ExtrusionEntitiesPtr to_add;
            for (Polyline& pline : to_cut.lines) {
                assert(pline.size() > 0);
                if (pline.back() == pline.front()) {
                    ExtrusionPath path(erSkirt, mm3_per_mm, width, height);
                    path.polyline = pline;
                    to_add.push_back(new ExtrusionLoop(std::move(path), elrSkirt));
                } else {
                    ExtrusionPath* extrusion_path = new ExtrusionPath(erSkirt, mm3_per_mm, width, height);
                    to_add.push_back(extrusion_path);
                    extrusion_path->polyline = pline;
                }
            }
            print_me_first->append(std::move(to_add));
            if (to_cut.children.size() == 1) {
                (*extrude_ptr)(to_cut.children[0], print_me_first);
            } else {
                ExtrusionEntityCollection* children = new ExtrusionEntityCollection();
                //children->no_sort = true;
                for (BrimLoop& child : to_cut.children)
                    (*extrude_ptr)(child, children);
                //remove un-needed collection if possible
                if (children->entities().size() == 1) {
                    print_me_first->append(*children->entities().front());
                    delete children;
                } else if (children->entities().size() == 0) {
                    delete children;
                } else {
                    print_me_first->append(ExtrusionEntitiesPtr{ children });
                }
            }
            assert(print_me_first->entities().size() > 0);
        }
    };
    extrude_ptr = &extrude;

    if (loops.empty()) {
        BOOST_LOG_TRIVIAL(error) << "Failed to extrude brim: no loops to extrude, are you sure your settings are ok?";
        return;
    }

    //launch extrude
    for (BrimLoop& loop : loops[0]) {
        extrude(loop, &out);
    }
}

/// reorder & join polyline if their ending are near enough, then extrude the brim from the polyline into 'out'.
Polylines reorder_brim_polyline(Polylines lines, ExtrusionEntityCollection& out, const Flow& flow) {
    //reorder them
    std::sort(lines.begin(), lines.end(), [](const Polyline& a, const Polyline& b)->bool { return a.closest_point(Point(0, 0))->y() < b.closest_point(Point(0, 0))->y(); });
    Polylines lines_sorted;
    Polyline* previous = NULL;
    Polyline* best = NULL;
    double best_dist = -1;
    size_t best_idx = 0;
    while (lines.size() > 0) {
        if (previous == NULL) {
            lines_sorted.push_back(lines.back());
            previous = &lines_sorted.back();
            lines.erase(lines.end() - 1);
        } else {
            best = NULL;
            best_dist = -1;
            best_idx = 0;
            for (size_t i = 0; i < lines.size(); ++i) {
                Polyline& viewed_line = lines[i];
                double dist = viewed_line.points.front().distance_to(previous->points.front());
                dist = std::min(dist, viewed_line.points.front().distance_to(previous->points.back()));
                dist = std::min(dist, viewed_line.points.back().distance_to(previous->points.front()));
                dist = std::min(dist, viewed_line.points.back().distance_to(previous->points.back()));
                if (dist < best_dist || best == NULL) {
                    best = &viewed_line;
                    best_dist = dist;
                    best_idx = i;
                }
            }
            if (best != NULL) {
                //copy new line inside the sorted array.
                lines_sorted.push_back(lines[best_idx]);
                lines.erase(lines.begin() + best_idx);

                //connect if near enough
                if (lines_sorted.size() > 1) {
                    size_t idx = lines_sorted.size() - 2;
                    bool connect = false;
                    if (lines_sorted[idx].points.back().distance_to(lines_sorted[idx + 1].points.front()) < flow.scaled_spacing() * 2) {
                        connect = true;
                    } else if (lines_sorted[idx].points.back().distance_to(lines_sorted[idx + 1].points.back()) < flow.scaled_spacing() * 2) {
                        lines_sorted[idx + 1].reverse();
                        connect = true;
                    } else if (lines_sorted[idx].points.front().distance_to(lines_sorted[idx + 1].points.front()) < flow.scaled_spacing() * 2) {
                        lines_sorted[idx].reverse();
                        connect = true;
                    } else if (lines_sorted[idx].points.front().distance_to(lines_sorted[idx + 1].points.back()) < flow.scaled_spacing() * 2) {
                        lines_sorted[idx].reverse();
                        lines_sorted[idx + 1].reverse();
                        connect = true;
                    }

                    if (connect) {
                        //connect them
                        lines_sorted[idx].points.insert(
                            lines_sorted[idx].points.end(),
                            lines_sorted[idx + 1].points.begin(),
                            lines_sorted[idx + 1].points.end());
                        lines_sorted.erase(lines_sorted.begin() + idx + 1);
                        idx--;
                    }
                }

                //update last position
                previous = &lines_sorted.back();
            }
        }
    }

    return lines_sorted;
}


//note: unbrimmable must keep its ordering. don't union_ex it.

//TODO: test if no regression vs old _make_brim.
// this new one can extrude brim for an object inside an other object.
void make_brim(const Print& print, const Flow& flow, const PrintObjectPtrs& objects, ExPolygons& unbrimmable, ExtrusionEntityCollection& out) {
    const coord_t scaled_spacing = flow.scaled_spacing();
    const PrintObjectConfig& brim_config = objects.front()->config();
    coord_t brim_offset = scale_t(brim_config.brim_separation.value);
    ExPolygons    islands;
    for (PrintObject* object : objects) {
        ExPolygons object_islands;
        for (ExPolygon& expoly : object->layers().front()->lslices)
            if (brim_config.brim_inside_holes && brim_config.brim_width_interior == 0) {
                if (brim_offset == 0) {
                    object_islands.push_back(expoly);
                } else {
                    for (ExPolygon& grown_expoly : offset_ex(expoly, brim_offset)) {
                        object_islands.push_back(std::move(grown_expoly));
                    }
                }
            } else {
                if (brim_offset == 0) {
                    object_islands.push_back(to_expolygon(expoly.contour));
                } else {
                    for (ExPolygon& grown_expoly : offset_ex(to_expolygon(expoly.contour), brim_offset)) {
                        object_islands.push_back(std::move(grown_expoly));
                    }
                }
            }
        if (!object->support_layers().empty()) {
            ExPolygons polys = union_ex(object->support_layers().front()->support_fills.polygons_covered_by_spacing(flow.spacing_ratio(), float(SCALED_EPSILON)));
            for (ExPolygon& poly : polys) {
                if (brim_offset == 0) {
                    object_islands.push_back(std::move(poly));
                } else {
                    append(object_islands, offset_ex(ExPolygons{ poly }, brim_offset));
                }
            }
        }
        islands.reserve(islands.size() + object_islands.size() * object->instances().size());
        for (const PrintInstance& pt : object->instances()) {
            for (ExPolygon& poly : object_islands) {
                islands.push_back(poly);
                islands.back().translate(pt.shift.x(), pt.shift.y());
            }
        }
    }

    print.throw_if_canceled();

    //simplify & merge
    //get brim resolution (lower resolution if no arc fitting)
    coordf_t scaled_resolution_brim = (print.config().arc_fitting.value)? scale_d(print.config().resolution) : scale_d(print.config().resolution_internal) / 10;
    ExPolygons unbrimmable_areas;
    for (ExPolygon& expoly : islands)
        for (ExPolygon& expoly : expoly.simplify(scaled_resolution_brim))
            unbrimmable_areas.emplace_back(std::move(expoly));
    islands = union_safety_offset_ex(unbrimmable_areas);
    unbrimmable_areas = islands;

    //get the brimmable area
    const size_t num_loops = size_t(floor(std::max(0., (brim_config.brim_width.value - brim_config.brim_separation.value)) / flow.spacing()));
    ExPolygons brimmable_areas;
    for (ExPolygon& expoly : islands) {
        for (Polygon poly : offset(expoly.contour, num_loops* scaled_spacing, jtSquare)) {
            brimmable_areas.emplace_back();
            brimmable_areas.back().contour = poly;
            brimmable_areas.back().contour.make_counter_clockwise();
            brimmable_areas.back().holes.push_back(expoly.contour);
            brimmable_areas.back().holes.back().make_clockwise();
        }
    }
    brimmable_areas = union_ex(brimmable_areas);
    print.throw_if_canceled();

    //don't collide with objects
    brimmable_areas = diff_ex(brimmable_areas, unbrimmable_areas,   ApplySafetyOffset::Yes);
    brimmable_areas = diff_ex(brimmable_areas, unbrimmable,         ApplySafetyOffset::Yes);

    print.throw_if_canceled();

    //now get all holes, use them to create loops
    std::vector<std::vector<BrimLoop>> loops;
    ExPolygons bigger_islands;
    //grow a half of spacing, to go to the first extrusion polyline.
    Polygons unbrimmable_polygons;
    for (ExPolygon& expoly : islands) {
        unbrimmable_polygons.push_back(expoly.contour);
        //do it separately because we don't want to union them
        for (ExPolygon& big_expoly : offset_ex(expoly, double(scaled_spacing) * 0.5, jtSquare)) {
            bigger_islands.emplace_back(big_expoly);
            unbrimmable_polygons.insert(unbrimmable_polygons.end(), big_expoly.holes.begin(), big_expoly.holes.end());
        }
    }
    islands = bigger_islands;
    ExPolygons last_islands;
    for (size_t i = 0; i < num_loops; ++i) {
        loops.emplace_back();
        print.throw_if_canceled();
        // only grow the contour, not holes
        bigger_islands.clear();
        if (i > 0) {
            for (ExPolygon& expoly : last_islands) {
                for (ExPolygon& big_contour : offset_ex(expoly, double(scaled_spacing), jtSquare)) {
                    bigger_islands.push_back(big_contour);
                    Polygons simplifiesd_big_contour = big_contour.contour.simplify(scaled_resolution_brim);
                    if (simplifiesd_big_contour.size() == 1) {
                        bigger_islands.back().contour = simplifiesd_big_contour.front();
                    }
                }
            }
        } else bigger_islands = islands;
        last_islands = union_ex(bigger_islands);
        for (ExPolygon& expoly : last_islands) {
            loops.back().emplace_back(expoly.contour);
            // also add hole, in case of it's merged with a contour. see supermerill/SuperSlicer/issues/3050
            for (Polygon &hole : expoly.holes)
                //but remove the points that are inside the holes of islands
                for (ExPolygon& pl : diff_ex(Polygons{ hole }, unbrimmable_polygons))
                    loops[i].emplace_back(pl.contour);
        }
    }

    std::reverse(loops.begin(), loops.end());

    // Using offset(expoly.contour, num_loops* scaled_spacing, jtSquare) create a different result than the incremental 'loops' creation.
    //   so i have to restrict with the biggest (first) loop from loops (last_islands).
    //intersection
    brimmable_areas = intersection_ex(brimmable_areas, offset_ex(last_islands, double(scaled_spacing) * 0.5, jtSquare));
    Polygons frontiers;
    //use contour from brimmable_areas (external frontier)
    for (ExPolygon& expoly : brimmable_areas) {
        frontiers.push_back(expoly.contour);
        frontiers.back().make_counter_clockwise();
    }
    // add internal frontier
    frontiers.insert(frontiers.begin(), unbrimmable_polygons.begin(), unbrimmable_polygons.end());

    extrude_brim_from_tree(print, loops, frontiers, flow, out, false);

    unbrimmable.insert(unbrimmable.end(), brimmable_areas.begin(), brimmable_areas.end());
}

void make_brim_ears(const Print& print, const Flow& flow, const PrintObjectPtrs& objects, ExPolygons& unbrimmable, ExtrusionEntityCollection& out) {
    const PrintObjectConfig& brim_config = objects.front()->config();
    Points pt_ears;
    coord_t brim_offset = scale_t(brim_config.brim_separation.value);
    ExPolygons islands;
    ExPolygons unbrimmable_with_support = unbrimmable;
    for (PrintObject* object : objects) {
        ExPolygons object_islands;
        ExPolygons support_island;
        for (const ExPolygon& expoly : object->layers().front()->lslices) {
            if (brim_config.brim_inside_holes && brim_config.brim_width_interior == 0) {
                if (brim_offset == 0) {
                    object_islands.push_back(expoly);
                } else {
                    for (ExPolygon& grown_expoly : offset_ex(expoly, brim_offset)) {
                        object_islands.push_back(std::move(grown_expoly));
                    }
                }
            } else {
                if (brim_offset == 0) {
                    object_islands.push_back(to_expolygon(expoly.contour));
                } else {
                    for (const ExPolygon& grown_expoly : offset_ex(to_expolygon(expoly.contour), brim_offset)) {
                        object_islands.push_back(std::move(grown_expoly));
                    }
                }
            }
        }

        if (!object->support_layers().empty()) {
            ExPolygons polys = union_ex(object->support_layers().front()->support_fills.polygons_covered_by_spacing(flow.spacing_ratio(), float(SCALED_EPSILON)));
            //put ears over supports unless it's more than 30% fill
            if (object->config().raft_first_layer_density.get_abs_value(1.) > 0.3) {
                for (ExPolygon& poly : polys) {
                    if (brim_offset == 0) {
                        object_islands.push_back(std::move(poly));
                    } else {
                        append(object_islands, offset_ex(ExPolygons{ poly }, brim_offset));
                    }
                }
            } else {
                // offset2+- to avoid bits of brim inside the raft
                append(support_island, closing_ex(polys, flow.scaled_width() * 2));
            }
        }
        islands.reserve(islands.size() + object_islands.size() * object->instances().size());
        coord_t ear_detection_length = scale_t(object->config().brim_ears_detection_length.value);
        // duplicate & translate for each instance
        for (const PrintInstance& copy_pt : object->instances()) {
            for (const ExPolygon& poly : object_islands) {
                islands.push_back(poly);
                islands.back().translate(copy_pt.shift.x(), copy_pt.shift.y());
                Polygon decimated_polygon = poly.contour;
                // brim_ears_detection_length codepath
                if (ear_detection_length > 0) {
                    //decimate polygon
                    Points points = poly.contour.points;
                    points.push_back(points.front());
                    points = MultiPoint::_douglas_peucker(points, ear_detection_length);
                    if (points.size() > 4) { //don't decimate if it's going to be below 4 points, as it's surely enough to fill everything anyway
                        points.erase(points.end() - 1);
                        decimated_polygon.points = points;
                    }
                }
                for (const Point& p : decimated_polygon.convex_points(brim_config.brim_ears_max_angle.value* PI / 180.0)) {
                    pt_ears.push_back(p);
                    pt_ears.back() += (copy_pt.shift);
                }
            }
            // also for support-fobidden area
            for (const ExPolygon& poly : support_island) {
                unbrimmable_with_support.push_back(poly);
                unbrimmable_with_support.back().translate(copy_pt.shift.x(), copy_pt.shift.y());
            }
        }
    }

    islands = union_safety_offset_ex(islands);

    //get the brimmable area (for the return value only)
    const size_t num_loops = size_t(floor((brim_config.brim_width.value - brim_config.brim_separation.value) / flow.spacing()));
    ExPolygons brimmable_areas;
    Polygons contours;
    Polygons holes;
    for (ExPolygon& expoly : islands) {
        for (Polygon poly : offset(expoly.contour, num_loops* flow.scaled_width(), jtSquare)) {
            contours.push_back(poly);
        }
        holes.push_back(expoly.contour);
    }
    brimmable_areas = diff_ex(union_(contours), union_(holes));
    brimmable_areas = diff_ex(brimmable_areas, unbrimmable_with_support, ApplySafetyOffset::Yes);

    print.throw_if_canceled();

    //get brim resolution (low resolution if no arc fitting)
    coordf_t scaled_resolution_brim = (print.config().arc_fitting.value) ? scale_d(print.config().resolution) : scale_d(print.config().resolution_internal) / 10;
    if (brim_config.brim_ears_pattern.value == InfillPattern::ipConcentric) {

        //create loops (same as standard brim)
        Polygons loops;
        islands = offset_ex(islands, -0.5f * double(flow.scaled_spacing()));
        for (size_t i = 0; i < num_loops; ++i) {
            print.throw_if_canceled();
            islands = offset_ex(islands, double(flow.scaled_spacing()), jtSquare);
            for (ExPolygon& expoly : islands) {
                Polygon poly = expoly.contour;
                poly.points.push_back(poly.points.front());
                Points p = MultiPoint::_douglas_peucker(poly.points, scaled_resolution_brim);
                p.pop_back();
                poly.points = std::move(p);
                loops.push_back(poly);
            }
        }
        //order path with least travel possible
        loops = union_pt_chained_outside_in(loops);

        //create ear pattern
        coord_t size_ear = (scale_t((brim_config.brim_width.value - brim_config.brim_separation.value)) - flow.scaled_spacing());
        Polygon point_round;
        for (size_t i = 0; i < POLY_SIDES; i++) {
            double angle = (2.0 * PI * i) / POLY_SIDES;
            point_round.points.emplace_back(size_ear * cos(angle), size_ear * sin(angle));
        }

        //create ears
        ExPolygons mouse_ears_ex;
        for (Point pt : pt_ears) {
            mouse_ears_ex.emplace_back();
            mouse_ears_ex.back().contour = point_round;
            mouse_ears_ex.back().contour.translate(pt);
        }

        //intersection
        ExPolygons mouse_ears_area = intersection_ex(mouse_ears_ex, brimmable_areas);
        Polylines lines = intersection_pl(loops, to_polygons(mouse_ears_area));
        print.throw_if_canceled();

        //reorder & extrude them
        Polylines lines_sorted = reorder_brim_polyline(lines, out, flow);

        //push into extrusions
        extrusion_entities_append_paths(
            out.set_entities(),
            lines_sorted,
            erSkirt,
            float(flow.mm3_per_mm()),
            float(flow.width()),
            float(print.get_first_layer_height())
        );

        append(unbrimmable, offset_ex(mouse_ears_ex, flow.scaled_spacing() / 2));

    } else /* brim_config.brim_ears_pattern.value == InfillPattern::ipRectilinear */ {

        //create ear pattern
        coord_t size_ear = (scale_t((brim_config.brim_width.value - brim_config.brim_separation.value)) - flow.scaled_spacing());
        Polygon point_round;
        for (size_t i = 0; i < POLY_SIDES; i++) {
            double angle = (2.0 * PI * i) / POLY_SIDES;
            point_round.points.emplace_back(size_ear * cos(angle), size_ear * sin(angle));
        }

        //create ears
        ExPolygons mouse_ears_ex;
        for (Point pt : pt_ears) {
            mouse_ears_ex.emplace_back();
            mouse_ears_ex.back().contour = point_round;
            mouse_ears_ex.back().contour.translate(pt);
        }

        ExPolygons new_brim_area = intersection_ex(brimmable_areas, mouse_ears_ex);

        std::unique_ptr<Fill> filler = std::unique_ptr<Fill>(Fill::new_from_type(ipRectiWithPerimeter));
        filler->angle = 0;

        FillParams fill_params;
        fill_params.density = 1.f;
        fill_params.fill_exactly = true;
        fill_params.flow = flow;
        fill_params.role = erSkirt;
        filler->init_spacing(flow.spacing(), fill_params);
        for (const ExPolygon& expoly : new_brim_area) {
            Surface surface(stPosInternal | stDensSparse, expoly);
            filler->fill_surface_extrusion(&surface, fill_params, out.set_entities());
        }

        unbrimmable.insert(unbrimmable.end(), new_brim_area.begin(), new_brim_area.end());
    }

}

void make_brim_interior(const Print& print, const Flow& flow, const PrintObjectPtrs& objects, ExPolygons& unbrimmable_areas, ExtrusionEntityCollection& out) {
    // Brim is only printed on first layer and uses perimeter extruder.

    const PrintObjectConfig& brim_config = objects.front()->config();
    coord_t brim_offset = scale_t(brim_config.brim_separation.value);
    ExPolygons    islands;
    coordf_t spacing;
    for (PrintObject* object : objects) {
        ExPolygons object_islands;
        for (const ExPolygon& expoly : object->layers().front()->lslices){
            if (brim_offset == 0) {
                object_islands.push_back(expoly);
            } else {
                for (const ExPolygon& grown_expoly : offset_ex(ExPolygons{ expoly }, brim_offset)) {
                    object_islands.push_back(std::move(grown_expoly));
                }
            }
        }
        if (!object->support_layers().empty()) {
            spacing = scaled(object->config().support_material_interface_spacing.value) + support_material_flow(object, float(print.get_first_layer_height())).scaled_width() * 1.5;
            ExPolygons polys = closing_ex(
                union_ex(object->support_layers().front()->support_fills.polygons_covered_by_spacing(flow.spacing_ratio(), float(SCALED_EPSILON)))
                , spacing);
            for (ExPolygon& poly : polys) {
                if (brim_offset == 0) {
                    object_islands.push_back(std::move(poly));
                } else {
                    append(object_islands, offset_ex(ExPolygons{ poly }, brim_offset));
                }
            }
        }
        islands.reserve(islands.size() + object_islands.size() * object->instances().size());
        for (const PrintInstance& instance : object->instances())
            for (ExPolygon& poly : object_islands) {
                islands.push_back(poly);
                islands.back().translate(instance.shift.x(), instance.shift.y());
            }
    }

    islands = union_ex(islands);

    //to have the brimmable areas, get all holes, use them as contour , add smaller hole inside and make a diff with unbrimmable
    const size_t num_loops = size_t(floor((brim_config.brim_width_interior.value - brim_config.brim_separation.value) / flow.spacing()));
    ExPolygons brimmable_areas;
    Polygons islands_to_loops;
    for (const ExPolygon& expoly : islands) {
        for (const Polygon& hole : expoly.holes) {
            brimmable_areas.emplace_back();
            brimmable_areas.back().contour = hole;
            brimmable_areas.back().contour.make_counter_clockwise();
            for (Polygon poly : offset(brimmable_areas.back().contour, -flow.scaled_width() * (double)num_loops, jtSquare)) {
                brimmable_areas.back().holes.push_back(poly);
                brimmable_areas.back().holes.back().make_clockwise();
            }
            islands_to_loops.insert(islands_to_loops.begin(), brimmable_areas.back().contour);
        }
    }

    brimmable_areas = diff_ex(brimmable_areas, islands, ApplySafetyOffset::Yes);
    brimmable_areas = diff_ex(brimmable_areas, unbrimmable_areas, ApplySafetyOffset::Yes);

    //now get all holes, use them to create loops
    //get brim resolution (low resolution if no arc fitting)
    coordf_t scaled_resolution_brim = (print.config().arc_fitting.value) ? scale_d(print.config().resolution) : scale_d(print.config().resolution_internal) / 10;
    std::vector<std::vector<BrimLoop>> loops;
    for (size_t i = 0; i < num_loops; ++i) {
        print.throw_if_canceled();
        loops.emplace_back();
        Polygons islands_to_loops_offseted;
        for (Polygon& poly : islands_to_loops) {
            Polygons temp = offset(poly, double(-flow.scaled_spacing()), jtSquare);
            for (Polygon& poly : temp) {
                poly.points.push_back(poly.points.front());
                Points p = MultiPoint::_douglas_peucker(poly.points, scaled_resolution_brim);
                p.pop_back();
                poly.points = std::move(p);
            }
            for (Polygon& poly : offset(temp, 0.5f * double(flow.scaled_spacing())))
                loops[i].emplace_back(poly);
            islands_to_loops_offseted.insert(islands_to_loops_offseted.end(), temp.begin(), temp.end());
        }
        islands_to_loops = islands_to_loops_offseted;
    }
    //loops = union_pt_chained_outside_in(loops, false);
    std::reverse(loops.begin(), loops.end());

    //intersection
    Polygons frontiers;
    for (ExPolygon& expoly : brimmable_areas) {
        for (Polygon& big_contour : offset(expoly.contour, 0.1f * flow.scaled_width())) {
            frontiers.push_back(big_contour);
            for (Polygon& hole : expoly.holes) {
                frontiers.push_back(hole);
                //don't reverse it! back! or it will be ignored by intersection_pl. 
                //frontiers.back().reverse();
            }
        }
    }

    extrude_brim_from_tree(print, loops, frontiers, flow, out, true);

    unbrimmable_areas.insert(unbrimmable_areas.end(), brimmable_areas.begin(), brimmable_areas.end());
}


} // namespace Slic3r
