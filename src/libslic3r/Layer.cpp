///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/ Copyright (c) superslicer 2019 - 2025 Durand Rémi @supermerill
///|/ Copyright (c) Prusa Research 2016 - 2023 Vojtěch Bubník @bubnikv, Pavel Mikuš @Godrak, Lukáš Hejl @hejllukas
///|/ Copyright (c) Slic3r 2014 - 2015 Alessandro Ranellucci @alranel
///|/ Copyright (c) 2015 Maksim Derbasov @ntfshard
///|/
///|/ ported from lib/Slic3r/Layer.pm:
///|/ Copyright (c) Prusa Research 2016 - 2022 Vojtěch Bubník @bubnikv
///|/ Copyright (c) Slic3r 2011 - 2016 Alessandro Ranellucci @alranel
///|/
///|/ SuperSlicer, PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "Layer.hpp"

#include <boost/log/trivial.hpp>
#include <clipper/clipper.hpp>

#include "Api/internal/LayerAccess.hpp"
#include "Api/internal/LayerIslandAccess.hpp"
#include "BoundingBox.hpp"
#include "ClipperUtils.hpp"
#include "ClipperZUtils.hpp"
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityVisitors.hpp"
#include "Milling/MillingPostProcess.hpp"
#include "PerimeterGenerator.hpp"
#include "Point.hpp"
#include "Polygon.hpp"
#include "Print.hpp"
#include "PrintObject.hpp"
#include "PrintObjectRegion.hpp"
#include "PrintRegion.hpp"
#include "ShortestPath.hpp"
#include "Surface.hpp"
#include "SVG.hpp"

namespace Slic3r {

ExPolygons &ApiInternal::LayerAccess::slices_mutable(Layer &layer) { return layer.m_lslices; }

void ApiInternal::LayerAccess::set_islands(Layer &layer, ExPolygons &&new_islands) {
    assert(!layer.m_islands_locked);
    layer.m_lslices = std::move(new_islands);
    layer.m_islands.clear();
    for (size_t i = 0; i < layer.m_lslices.size(); ++i) {
        layer.m_islands.emplace_back(new LayerSliceIsland(layer.m_lslices[i]));
    }
    assert(layer.lslices().size() == layer.m_islands.size());
}

void ApiInternal::LayerAccess::recompute_slices_from_islands(Layer &layer) {
    assert(!layer.m_islands_locked);
    layer.m_lslices.clear();
    for (LayerSliceIslandUPtr &island : layer.m_islands) {
        layer.m_lslices.push_back(island->get_slice());
    }
    assert(layer.lslices().size() == layer.m_islands.size());
}

void ApiInternal::LayerAccess::init_regions_from_object(Layer &layer)
{
    const PrintObject *object = layer.object();
    assert(object != nullptr);

    layer.m_regions.clear();
    layer.m_regions.reserve(object->shared_regions()->all_regions.size());
    for (const std::unique_ptr<PrintRegion> &pr : object->shared_regions()->all_regions)
        layer.m_regions.emplace_back(new LayerRegion(&layer, pr.get()));
}

void ApiInternal::LayerAccess::add_region(Layer &layer, const PrintRegion &region)
{
    layer.m_regions.emplace_back(new LayerRegion(&layer, &region));
}

// was Layer::make_slices()
void ApiInternal::LayerAccess::recompute_slices_from_layer_regions(Layer &layer) {
    ExPolygons slices;
    if (layer.m_regions.size() == 1) {
        // If there is a single region, the layer islands are exactly the raw
        // slices owned by that region. SurfaceCollections may be empty at this
        // point for plugin-driven slicing steps.
        slices = layer.m_regions.front()->get_raw_slices();
    } else {
        ExPolygons slices_exp;
        for (LayerRegionUPtr &layerm : layer.m_regions) {
            for (const ExPolygon &expolygon : layerm->get_raw_slices())
                expolygon.assert_valid();
            append(slices_exp, layerm->get_raw_slices());
        }
        slices = union_safety_offset_ex(slices_exp);
    }
    for (ExPolygon &poly : slices)
        for (auto &hole : poly.holes)
            assert(hole.is_clockwise());
    ensure_valid(slices, std::max(scale_i(layer.object()->print()->config().resolution), SCALED_EPSILON));
    for (ExPolygon &poly : slices)
        poly.assert_valid();
    // lslices are sorted by topological order from outside to inside from the clipper union used above
#ifdef _DEBUG
    if (slices.size() > 1) {
        std::vector<BoundingBox> bboxes;
        bboxes.emplace_back(slices[0].contour.points);
        for (size_t check_idx = 1; check_idx < slices.size(); ++check_idx) {
            assert(bboxes.size() == check_idx);
            bboxes.emplace_back(slices[check_idx].contour.points);
            for (size_t bigger_idx = 0; bigger_idx < check_idx; ++bigger_idx) {
                // higher idx can be inside holes, but not the opposite!
                if (bboxes[check_idx].contains(bboxes[bigger_idx])) {
                    assert(!slices[check_idx].contour.contains(slices[bigger_idx].contour.first_point()));
                }
            }
        }
    }
#endif
    ApiInternal::LayerAccess::set_islands(layer, std::move(slices));
}

ExPolygon &ApiInternal::LayerIslandAccess::slice_mutable(LayerSliceIsland &island)
{
    return island.m_slice;
}

void ApiInternal::LayerIslandAccess::set_infill_areas(LayerSliceIsland &island, ExPolygons &&infill_areas)
{
    island.m_infill_areas = std::move(infill_areas);
    island.m_infill_areas_bboxes = get_extents_vector(island.m_infill_areas);
}

ExPolygons &ApiInternal::LayerIslandAccess::infill_free_areas_mutable(LayerSliceIsland &island)
{
    return island.m_infill_free_areas;
}

ExPolygons &ApiInternal::LayerIslandAccess::perimeter_slices_mutable(LayerSliceIsland &island)
{
    return island.m_perimeter_slices;
}

Layer::Layer(size_t id, PrintObject *object, coord_t height, coord_t print_z, double slice_z, bool /*scaledok*/)
    : upper_layer(nullptr)
    , lower_layer(nullptr)
    , slice_z(slice_z)
    , m_print_z(print_z)
    , m_height(height)
    , m_id(id)
    , m_object(object) {
    assert(print_z > 100);
    assert(height > 100);
    assert(scale_to_layer_coord(unscaled(print_z)) == print_z);
}

LayerSliceIsland::LayerSliceIsland(const ExPolygon &slice) {
    m_slice = slice;
    m_bbox = get_extents(m_slice);
}

void LayerSliceIsland::fill_regions(Layer &layer) {
    assert(!layer.regions().empty());
    // not sure if the offset is really needed
    BoundingBox bb_bigger = m_bbox;
    bb_bigger.offset(SCALED_EPSILON * 10);
    for (const LayerRegion &lr : layer.regions()) {
        // intersection
        for (const ExPolygon &region_expoly : lr.get_raw_slices()) {
            BoundingBox bb_region = get_extents(region_expoly);
            if (bb_bigger.overlap(bb_region)) {
                ExPolygons intersections = intersection_ex(m_slice, region_expoly);
                if (!intersections.empty()) {
                    this->m_regions.insert(&lr);
                }
            }
        }
    }
    m_layer = &layer;
}

bool LayerSliceIsland::is_expolygons_from_region(const ExPolygon &expolygon) const {
    assert(!expolygon.contour.empty());
    BoundingBox bb_bigger = m_bbox;
    bb_bigger.offset(scale_i(0.2));

    //quick check
    if (!bb_bigger.contains(expolygon.contour.points.front())) {
        return false;
    }

    // check if not outside of the island's contour
    BoundingBox bb_expoly(expolygon.contour.points);
    if (!bb_bigger.contains(bb_expoly)) {
        return false;
    }

    ExPolygons intersection = intersection_ex(expolygon, m_slice);
    if (intersection.empty()) {
        return false;
    }
    ExPolygons difference = diff_ex(expolygon, m_slice);
    if (difference.empty()) {
        return true;
    }
    // shouldn't happen, but it's possible with precision issues
    double area_inter = 0;
    double area_diff = 0;
    for (ExPolygon &inter : intersection) {
        area_inter += inter.area();
    }
    for (ExPolygon &diff : difference) {
        area_diff += diff.area();
    }
    return area_inter > area_diff;
}

bool LayerSliceIsland::has_extrusions() const {
    for (const LayerRegionIsland &region_island : this->regions_islands()) {
        if (region_island.has_extrusions()) {
            return true;
        }
    }
    return false;
}

LayerRegionIsland &LayerSliceIsland::add_region_island(const LayerRegionSetCPtrs &region_set,
                                                            uint16_t extruder_id) {
    // these region are all inside this islands
    for (auto regionptr : region_set) {
        assert(this->regions().find(regionptr) != this->regions().end());
    }
    // add it
    m_extrusions.emplace_back(new LayerRegionIsland(region_set, extruder_id));
    return *m_extrusions.back();
}

LayerRegionIsland &LayerSliceIsland::get_or_add_region_island(const LayerRegionSetCPtrs &region_set,
                                                            uint16_t extruder_id) {
    LayerRegionIsland *region_island = nullptr;
    for (LayerRegionIsland &ri : this->regions_islands()) {
        if (ri.regions() == region_set && ri.extruder_id() == extruder_id) {
            region_island = &ri;
            break;
        }
    }
    if (region_island == nullptr) {
        region_island = &this->add_region_island(region_set, extruder_id);
    }
    return *region_island;
}

Layer::~Layer()
{
    this->lower_layer = this->upper_layer = nullptr;
    m_regions.clear();
}

void Layer::add_regions_to_islands() {
    assert(!this->m_islands_locked);
    //  note: regions taht don't intersect with this layer are kept (even if empty) to keep region-index ordering.

    // fill LayerRegion's m_slices
    for (LayerRegionUPtr &lr : m_regions) {
#if _DEBUG
        lr->surfaces_filled = true;
#endif
    }
    // create RegionIsland 
    assert(!m_islands.empty());
    for (LayerSliceIslandUPtr &li_ptr : m_islands) {
        li_ptr->fill_regions(*this);
    }

    this->m_islands_locked = true;
}

// Test whether whether there are any slices assigned to this layer.
bool Layer::empty() const
{
    for (const LayerRegionUPtr &layerm : m_regions) {
        if (layerm != nullptr && !layerm->slices().empty()) {
            // Non empty layer.
            return false;
        }
    }
    return true;
}

bool Layer::has_extrusions() const {
    for (const LayerSliceIsland &layer_island : this->islands()) {
        if (layer_island.has_extrusions()) {
            return true;
        }
    }
    return false;
}

void Layer::simplify_extrusion_path() {
    for (LayerSliceIslandUPtr &island_ptr : m_islands)
        for (LayerRegionIsland &regisland : island_ptr->regions_islands())
            regisland.simplify_extrusion_entity(*this);
}

// merge all regions' slices to get islands
void Layer::make_slices()
{
    {
        ExPolygons slices;
        if (m_regions.size() == 1) {
            // optimization: if we only have one region, take its slices
            slices = to_expolygons(m_regions.front()->slices().surfaces);
        } else {
            ExPolygons slices_exp;
            for (LayerRegionUPtr &layerm : m_regions) {
                for (const Surface &srf : layerm->slices().surfaces) srf.expolygon.assert_valid();
                append(slices_exp, to_expolygons(layerm->slices().surfaces));
            }
            slices = union_safety_offset_ex(slices_exp);
        }
        for (ExPolygon &poly : slices) for(auto &hole :poly.holes) assert(hole.is_clockwise());
        ensure_valid(slices, std::max(scale_i(this->object()->print()->config().resolution), SCALED_EPSILON));
        for (ExPolygon &poly : slices) poly.assert_valid();
        // lslices are sorted by topological order from outside to inside from the clipper union used above
#ifdef _DEBUG
        if (slices.size() > 1) {
            std::vector<BoundingBox> bboxes;
            bboxes.emplace_back(slices[0].contour.points);
            for (size_t check_idx = 1; check_idx < slices.size(); ++check_idx) {
                assert(bboxes.size() == check_idx);
                bboxes.emplace_back(slices[check_idx].contour.points);
                for (size_t bigger_idx = 0; bigger_idx < check_idx; ++bigger_idx) {
                    // higher idx can be inside holes, but not the opposite!
                    if (bboxes[check_idx].contains(bboxes[bigger_idx])) {
                        assert(!slices[check_idx].contour.contains(slices[bigger_idx].contour.first_point()));
                    }
                }
            }
        }
#endif
        ApiInternal::LayerAccess::set_islands(*this, std::move(slices));
    }

    //this->lslice_indices_sorted_by_print_order = chain_expolygons(this->lslices());
    //assert(this->lslices().size() == this->lslice_indices_sorted_by_print_order.size());
}


// used by Layer::build_up_down_graph()
// Shrink source polygons one by one, so that they will be separated if they were touching
// at vertices (non-manifold situation).
// Then convert them to Z-paths with Z coordinate indicating index of the source expolygon.
[[nodiscard]] static ClipperLib_Z::Paths expolygons_to_zpaths_shrunk(const ExPolygons &expolygons, coord_t isrc)
{
    size_t num_paths = 0;
    for (const ExPolygon &expolygon : expolygons)
        num_paths += expolygon.num_contours();

    ClipperLib_Z::Paths out;
    out.reserve(num_paths);

    ClipperLib::Paths           contours;
    ClipperLib::Paths           holes;
    ClipperLib::Clipper         clipper;
    ClipperLib::ClipperOffset   co;
    ClipperLib::Paths           out2;

    // Top / bottom surfaces must overlap more than 2um to be chained into a Z graph.
    // Also a larger offset will likely be more robust on non-manifold input polygons.
    static constexpr const float delta = scale_d(0.001);
    // Don't scale the miter limit, it is a factor, not an absolute length!
    co.MiterLimit = 3.;
// Use the default zero edge merging distance. For this kind of safety offset the accuracy of normal direction is not important.
//    co.ShortestEdgeLength = delta * ClipperOffsetShortestEdgeFactor;
//    static constexpr const double accept_area_threshold_ccw = sqr(scale_d(0.1 * delta));
    // Such a small hole should not survive the shrinkage, it should grow over 
//    static constexpr const double accept_area_threshold_cw  = sqr(scale_d(0.2 * delta));

    for (const ExPolygon &expoly : expolygons) {
        contours.clear();
        co.Clear();
        co.AddPath(expoly.contour.points, ClipperLib::jtMiter, ClipperLib::etClosedPolygon);
        co.Execute(contours, - delta);
//        size_t num_prev = out.size();
        if (! contours.empty()) {
            holes.clear();
            for (const Polygon &hole : expoly.holes) {
                co.Clear();
                co.AddPath(hole.points, ClipperLib::jtMiter, ClipperLib::etClosedPolygon);
                // Execute reorients the contours so that the outer most contour has a positive area. Thus the output
                // contours will be CCW oriented even though the input paths are CW oriented.
                // Offset is applied after contour reorientation, thus the signum of the offset value is reversed.
                out2.clear();
                co.Execute(out2, delta);
                append(holes, std::move(out2));
            }
            // Subtract holes from the contours.
            if (! holes.empty()) {
                clipper.Clear();
                clipper.AddPaths(contours, ClipperLib::ptSubject, true);
                clipper.AddPaths(holes, ClipperLib::ptClip, true);
                contours.clear();
                clipper.Execute(ClipperLib::ctDifference, contours, ClipperLib::pftNonZero, ClipperLib::pftNonZero);
            }
            for (const auto &contour : contours) {
                bool accept = true;
                // Trying to get rid of offset artifacts, that may be created due to numerical issues in offsetting algorithm
                // or due to self-intersections in the source polygons.
                //FIXME how reliable is it? Is it helpful or harmful? It seems to do more harm than good as it tends to punch holes
                // into existing ExPolygons.
#if 0
                if (contour.size() < 8) {
                    // Only accept contours with area bigger than some threshold.
                    double a = ClipperLib::Area(contour);
                    // Polygon has to be bigger than some threshold to be accepted.
                    // Hole to be accepted has to have an area slightly bigger than the non-hole, so it will not happen due to rounding errors,
                    // that a hole will be accepted without its outer contour.
                    accept = a > 0 ? a > accept_area_threshold_ccw : a < - accept_area_threshold_cw;
                }
#endif
                if (accept) {
                    out.emplace_back();
                    ClipperLib_Z::Path &path = out.back();
                    path.reserve(contour.size());
                    for (const Point &p : contour)
                        path.push_back({ p.x(), p.y(), isrc });
                }
            }
        }
#if 0 // #ifndef NDEBUG
        // Test whether the expolygons in a single layer overlap.
        Polygons test;
        for (size_t i = num_prev; i < out.size(); ++ i)
            test.emplace_back(ClipperZUtils::from_zpath(out[i]));
        Polygons outside = diff(test, to_polygons(expoly));
        if (! outside.empty()) {
            BoundingBox bbox(get_extents(expoly));
            bbox.merge(get_extents(test));
            SVG svg(debug_out_path("expolygons_to_zpaths_shrunk-self-intersections.svg").c_str(), bbox);
            svg.draw(expoly, "blue");
            svg.draw(test, "green");
            svg.draw(outside, "red");
        }
        assert(outside.empty());
#endif // NDEBUG
        ++ isrc;
    }

    return out;
}

// used by Layer::build_up_down_graph()
static void connect_layer_slices(
    Layer                                           &below,
    Layer                                           &above,
    const ClipperLib_Z::PolyTree                    &polytree,
    const std::vector<std::pair<coord_t, coord_t>>  &intersections,
    const coord_t                                    offset_below,
    const coord_t                                    offset_above
#ifndef NDEBUG
    , const coord_t                                  offset_end
#endif // NDEBUG
    )
{
    class Visitor {
    public:
        Visitor(const std::vector<std::pair<coord_t, coord_t>> &intersections, 
            Layer &below, Layer &above, const coord_t offset_below, const coord_t offset_above
#ifndef NDEBUG
            , const coord_t offset_end
#endif // NDEBUG
            ) :
            m_intersections(intersections), m_below(below), m_above(above), m_offset_below(offset_below), m_offset_above(offset_above)
#ifndef NDEBUG
            , m_offset_end(offset_end) 
#endif // NDEBUG
            {}

        void visit(const ClipperLib_Z::PolyNode &polynode)
        {
#ifndef NDEBUG
            auto assert_intersection_valid = [this](int i, int j) {
                assert(i < j);
                assert(i >= m_offset_below);
                assert(i < m_offset_above);
                assert(j >= m_offset_above);
                assert(j < m_offset_end);
                return true;
            };
#endif // NDEBUG
            if (polynode.Contour.size() >= 3) {
                // If there is an intersection point, it should indicate which contours (one from layer below, the other from layer above) intersect.
                // Otherwise the contour is fully inside another contour.
                auto [i, j] = this->find_top_bottom_contour_ids_strict(polynode);
                bool found = false;
                if (i < 0 && j < 0) {
                    // This should not happen. It may only happen if the source contours had just self intersections or intersections with contours at the same layer.
                    // We may safely ignore such cases where the intersection area is meager.
                    double a = ClipperLib_Z::Area(polynode.Contour);
                    if (a < sqr(scale_d(0.001))) {
                        // Ignore tiny overlaps. They are not worth resolving.
                    } else {
                        // We should not ignore large cases. Try to resolve the conflict by a majority of references.
                        std::tie(i, j) = this->find_top_bottom_contour_ids_approx(polynode);
                        // At least top or bottom should be resolved.
                        assert(i >= 0 || j >= 0);
                    }
                }
                if (j < 0) {
                    if (i < 0) {
                        // this->find_top_bottom_contour_ids_approx() shoudl have made sure this does not happen.
                        assert(false);
                    } else {
                        assert(i >= m_offset_below && i < m_offset_above);
                        i -= m_offset_below;
                        j = this->find_other_contour_costly(polynode, m_above, j == -2);
                        found = j >= 0;
                    }
                } else if (i < 0) {
                    assert(j >= m_offset_above && j < m_offset_end);
                    j -= m_offset_above;
                    i = this->find_other_contour_costly(polynode, m_below, i == -2);
                    found = i >= 0;
                } else {
                    assert(assert_intersection_valid(i, j));
                    i -= m_offset_below;
                    j -= m_offset_above;
                    assert(i >= 0 && i < m_below.islands().size());
                    assert(j >= 0 && j < m_above.islands().size());
                    found = true;
                }
                if (found) {
                    assert(i >= 0 && i < m_below.islands().size());
                    assert(j >= 0 && j < m_above.islands().size());
                    // Subtract area of holes from the area of outer contour.
                    double area = ClipperLib_Z::Area(polynode.Contour);
                    for (int icontour = 0; icontour < polynode.ChildCount(); ++ icontour)
                        area -= ClipperLib_Z::Area(polynode.Childs[icontour]->Contour);
                    // Store the links and area into the contours.
                    std::vector<LayerSliceIsland::Link> &links_below = m_below.island(i).overlaps_above;
                    std::vector<LayerSliceIsland::Link> &links_above = m_above.island(j).overlaps_below;
                    auto it_below = std::find_if(links_below.begin(), links_below.end(),
                        [&] (const LayerSliceIsland::Link& lsi) { return &m_above.islands()[j] == lsi.to; });
                    if (it_below != links_below.end()/* && it_below->slice_idx == j*/) {
                        it_below->area += area;
                    } else {
                        auto it_above = std::find_if(links_above.begin(), links_above.end(),
                            [&] (const LayerSliceIsland::Link& lsi) { return &m_below.islands()[i] == lsi.to; });
                        if (it_above != links_above.end()/* && it_above->slice_idx == i*/) {
                            it_above->area += area;
                        } else {
                            // Insert into one of the two vectors.
                            bool take_below = false;
                            if (links_below.size() < /*LayerSlice::LinksStaticSize*/4)
                                take_below = false;
                            else if (links_above.size() >= /*LayerSlice::LinksStaticSize*/ 4) {
                                size_t shift_below = links_below.end() - it_below;
                                size_t shift_above = links_above.end() - it_above;
                                take_below = shift_below < shift_above;
                            }
                            if (take_below)
                                links_below.insert(it_below, { &m_above.island(j), float(area) });
                            else
                                links_above.insert(it_above, { &m_below.island(i), float(area) });
                        }
                    }
                }
            }
            for (int i = 0; i < polynode.ChildCount(); ++ i)
                for (int j = 0; j < polynode.Childs[i]->ChildCount(); ++ j)
                    this->visit(*polynode.Childs[i]->Childs[j]);
        }

    private:
        // Find the indices of the contour below & above for an expolygon created as an intersection of two expolygons, one below, the other above.
        // Returns -1 if there is no point on the intersection refering bottom resp. top source expolygon.
        // Returns -2 if the intersection refers to multiple source expolygons on bottom resp. top layers.
        std::pair<int32_t, int32_t> find_top_bottom_contour_ids_strict(const ClipperLib_Z::PolyNode &polynode) const
        {
            // If there is an intersection point, it should indicate which contours (one from layer below, the other from layer above) intersect.
            // Otherwise the contour is fully inside another contour.
            int32_t i = -1, j = -1;
            auto process_i = [&i, &j](coord_t k) {
                if (i == -1)
                    i = k;
                else if (i >= 0) {
                    if (i != k) {
                        // Error: Intersection contour contains points of two or more source bottom contours.
                        i = -2;
                        if (j == -2)
                            // break
                            return true;
                    }
                } else
                    assert(i == -2);
                return false;
            };
            auto process_j = [&i, &j](coord_t k) {
                if (j == -1)
                    j = k;
                else if (j >= 0) {
                    if (j != k) {
                        // Error: Intersection contour contains points of two or more source top contours.
                        j = -2;
                        if (i == -2)
                            // break
                            return true;
                    }
                } else
                    assert(j == -2);
                return false;
            };
            for (int icontour = 0; icontour <= polynode.ChildCount(); ++ icontour) {
                const ClipperLib_Z::Path &contour = icontour == 0 ? polynode.Contour : polynode.Childs[icontour - 1]->Contour;
                if (contour.size() >= 3) {
                    for (const ClipperLib_Z::IntPoint &pt : contour)
                        if (coord_t k = pt.z(); k < 0) {
                            const auto &intersection = m_intersections[-k - 1];
                            assert(intersection.first <= intersection.second);
                            if (intersection.first < m_offset_above ? process_i(intersection.first) : process_j(intersection.first))
                                goto end;
                            if (intersection.second < m_offset_above ? process_i(intersection.second) : process_j(intersection.second))
                                goto end;
                        } else if (k < m_offset_above ? process_i(k) : process_j(k))
                            goto end;
                }
            }
        end:
            return { i, j };
        }

        // Find the indices of the contour below & above for an expolygon created as an intersection of two expolygons, one below, the other above.
        // This variant expects that the source expolygon assingment is not unique, it counts the majority.
        // Returns -1 if there is no point on the intersection refering bottom resp. top source expolygon.
        // Returns -2 if the intersection refers to multiple source expolygons on bottom resp. top layers.
        std::pair<int32_t, int32_t> find_top_bottom_contour_ids_approx(const ClipperLib_Z::PolyNode &polynode) const
        {
            // 1) Collect histogram of contour references.
            struct HistoEl {
                int32_t id;
                int32_t count;
            };
            std::vector<HistoEl> histogram;
            {
                auto increment_counter = [&histogram](const int32_t i) {
                    auto it = std::lower_bound(histogram.begin(), histogram.end(), i, [](auto l, auto r){ return l.id < r; });
                    if (it == histogram.end() || it->id != i)
                        histogram.insert(it, HistoEl{ i, int32_t(1) });
                    else
                        ++ it->count;
                };
                for (int icontour = 0; icontour <= polynode.ChildCount(); ++ icontour) {
                    const ClipperLib_Z::Path &contour = icontour == 0 ? polynode.Contour : polynode.Childs[icontour - 1]->Contour;
                    if (contour.size() >= 3) {
                        for (const ClipperLib_Z::IntPoint &pt : contour)
                            if (coord_t k = pt.z(); k < 0) {
                                const auto &intersection = m_intersections[-k - 1];
                                assert(intersection.first <= intersection.second);
                                increment_counter(intersection.first);
                                increment_counter(intersection.second);
                            } else
                                increment_counter(k);
                    }
                }
                assert(! histogram.empty());
            }
            int32_t i = -1;
            int32_t j = -1;
            if (! histogram.empty()) {
                // 2) Split the histogram to bottom / top.
                auto mid          = std::upper_bound(histogram.begin(), histogram.end(), m_offset_above, [](auto l, auto r){ return l < r.id; });
                // 3) Sort the bottom / top parts separately.
                auto bottom_begin = histogram.begin();
                auto bottom_end   = mid;
                auto top_begin    = mid;
                auto top_end      = histogram.end();
                std::sort(bottom_begin, bottom_end, [](auto l, auto r) { return l.count > r.count; });
                std::sort(top_begin,    top_end,    [](auto l, auto r) { return l.count > r.count; });
                double i_quality = 0;
                double j_quality = 0;
                if (bottom_begin != bottom_end) {
                    i = bottom_begin->id;
                    i_quality = std::next(bottom_begin) == bottom_end ? std::numeric_limits<double>::max() : double(bottom_begin->count) / std::next(bottom_begin)->count;
                }
                if (top_begin != top_end) {
                    j = top_begin->id;
                    j_quality = std::next(top_begin) == top_end ? std::numeric_limits<double>::max() : double(top_begin->count) / std::next(top_begin)->count;
                }
                // Expected to be called only if there are duplicate references to be resolved by the histogram.
                assert(i >= 0 || j >= 0);
                assert(i_quality < std::numeric_limits<double>::max() || j_quality < std::numeric_limits<double>::max());
                if (i >= 0 && i_quality < j_quality) {
                    // Force the caller to resolve the bottom references the costly but robust way.
                    assert(j >= 0);
                    // Twice the number of references for the best contour.
                    assert(j_quality >= 2.);
                    i = -2;
                } else if (j >= 0) {
                    // Force the caller to resolve the top reference the costly but robust way.
                    assert(i >= 0);
                    // Twice the number of references for the best contour.
                    assert(i_quality >= 2.);
                    j = -2;
                }

            }
            return { i, j };
        }

        static int32_t find_other_contour_costly(const ClipperLib_Z::PolyNode &polynode, const Layer &other_layer, bool other_has_duplicates)
        {
            if (! other_has_duplicates) {
                // The contour below is likely completely inside another contour above. Look-it up in the island above.
                Point pt(polynode.Contour.front().x(), polynode.Contour.front().y());
                for (int i = int(other_layer.islands().size()) - 1; i >= 0; -- i)
                    if (other_layer.islands()[i].get_bounding_box().contains(pt) && other_layer.lslices()[i].contains(pt))
                        return i;
                // The following shall not happen now as the source expolygons are being shrunk a bit before intersecting,
                // thus each point of each intersection polygon should fit completely inside one of the original (unshrunk) expolygons.
                assert(false);
            }
            // The comment below may not be valid anymore, see the comment above. However the code is used in case the polynode contains multiple references 
            // to other_layer expolygons, thus the references are not unique.
            //
            // The check above might sometimes fail when the polygons overlap only on points, which causes the clipper to detect no intersection.
            // The problem happens rarely, mostly on simple polygons (in terms of number of points), but regardless of size!
            // example of failing link on two layers, each with single polygon without holes.
            // layer A = Polygon{(-24931238,-11153865),(-22504249,-8726874),(-22504249,11477151),(-23261469,12235585),(-23752371,12727276),(-25002495,12727276),(-27502745,10227026),(-27502745,-12727274),(-26504645,-12727274)}
            // layer B = Polygon{(-24877897,-11100524),(-22504249,-8726874),(-22504249,11477151),(-23244827,12218916),(-23752371,12727276),(-25002495,12727276),(-27502745,10227026),(-27502745,-12727274),(-26504645,-12727274)}
            // note that first point is not identical, and the check above picks (-24877897,-11100524) as the first contour point (polynode.Contour.front()).
            // that point is sadly slightly outisde of the layer A, so no link is detected, eventhough they are overlaping "completely"
            Polygons contour_poly{ Polygon{ClipperZUtils::from_zpath(polynode.Contour)} };
            BoundingBox contour_aabb{contour_poly.front().points};
            int32_t i_largest = -1;
            double  a_largest = 0;
            for (int i = int(other_layer.islands().size()) - 1; i >= 0; -- i)
                if (contour_aabb.overlap(other_layer.islands()[i].get_bounding_box()))
                    // it is potentially slow, but should be executed rarely
                    if (Polygons overlap = intersection(contour_poly, other_layer.lslices()[i]); ! overlap.empty()) {
                        if (other_has_duplicates) {
                            // Find the contour with the largest overlap. It is expected that the other overlap will be very small.
                            double a = area(overlap);
                            if (a > a_largest) {
                                a_largest = a;
                                i_largest = i;
                            }
                        } else {
                            // Most likely there is just one contour that overlaps, however it is not guaranteed.
                            i_largest = i;
                            break;
                        }
                    }
            assert(i_largest >= 0);
            return i_largest;
        }

        const std::vector<std::pair<coord_t, coord_t>> &m_intersections;
        Layer                                          &m_below;
        Layer                                          &m_above;
        const coord_t                                   m_offset_below;
        const coord_t                                   m_offset_above;
#ifndef NDEBUG
        const coord_t                                   m_offset_end;
#endif // NDEBUG
    } visitor(intersections, below, above, offset_below, offset_above
#ifndef NDEBUG
        , offset_end
#endif // NDEBUG
    );

    for (int i = 0; i < polytree.ChildCount(); ++ i)
        visitor.visit(*polytree.Childs[i]);

#ifndef NDEBUG
    // Verify that only one directional link is stored: either from bottom slice up or from upper slice down.
    for (int32_t islice = 0; islice < below.islands().size(); ++ islice) {
        std::vector<LayerSliceIsland::Link> &links1 = below.island(islice).overlaps_above;
        for (LayerSliceIsland::Link &link1 : links1) {
            std::vector<LayerSliceIsland::Link> &links2 = link1.to->overlaps_below;
            assert(! std::binary_search(links2.begin(), links2.end(), link1, [](auto &l, auto &r){ return uint64_t(l.to) < uint64_t(r.to); }));
        }
    }
    for (int32_t islice = 0; islice < above.islands().size(); ++ islice) {
        std::vector<LayerSliceIsland::Link> &links1 = above.island(islice).overlaps_below;
        for (LayerSliceIsland::Link &link1 : links1) {
            std::vector<LayerSliceIsland::Link> &links2 = link1.to->overlaps_above;
            assert(! std::binary_search(links2.begin(), links2.end(), link1, [](auto &l, auto &r){ return uint64_t(l.to) < uint64_t(r.to); }));
        }
    }
#endif // NDEBUG

    // Scatter the links, but don't sort them yet.
    for (int32_t islice = 0; islice < int32_t(below.islands().size()); ++ islice)
        for (LayerSliceIsland::Link &link : below.island(islice).overlaps_above)
            link.to->overlaps_below.push_back({ &below.island(islice), link.area });
    for (int32_t islice = 0; islice < int32_t(above.islands().size()); ++ islice)
        for (LayerSliceIsland::Link &link : above.island(islice).overlaps_below)
            link.to->overlaps_above.push_back({ &above.island(islice), link.area });
    // Sort the links.
    for (size_t i = 0; i < below.islands().size(); ++i) {
        LayerSliceIsland &lslice = below.island(i);
        std::sort(lslice.overlaps_above.begin(), lslice.overlaps_above.end(), [](const LayerSliceIsland::Link &l, const LayerSliceIsland::Link &r){ return uint64_t(l.to) < uint64_t(r.to); });
    }
    for (size_t i = 0; i < above.islands().size(); ++i) {
        LayerSliceIsland &lslice = above.island(i);
        std::sort(lslice.overlaps_below.begin(), lslice.overlaps_below.end(), [](const LayerSliceIsland::Link &l, const LayerSliceIsland::Link &r){ return uint64_t(l.to) < uint64_t(r.to); });
    }
}

void Layer::build_up_down_graph(Layer& below, Layer& above)
{
    coord_t             paths_below_offset = 0;
    ClipperLib_Z::Paths paths_below = expolygons_to_zpaths_shrunk(below.lslices(), paths_below_offset);
    coord_t             paths_above_offset = paths_below_offset + coord_t(below.lslices().size());
    ClipperLib_Z::Paths paths_above = expolygons_to_zpaths_shrunk(above.lslices(), paths_above_offset);
#ifndef NDEBUG
    coord_t             paths_end = paths_above_offset + coord_t(above.lslices().size());
#endif // NDEBUG

    ClipperLib_Z::Clipper  clipper;
    ClipperLib_Z::PolyTree result;
    ClipperZUtils::ClipperZIntersectionVisitor::Intersections intersections;
    ClipperZUtils::ClipperZIntersectionVisitor visitor(intersections);
    clipper.ZFillFunction(visitor.clipper_callback());
    clipper.AddPaths(paths_below, ClipperLib_Z::ptSubject, true);
    clipper.AddPaths(paths_above, ClipperLib_Z::ptClip, true);
    clipper.Execute(ClipperLib_Z::ctIntersection, result, ClipperLib_Z::pftNonZero, ClipperLib_Z::pftNonZero);

    connect_layer_slices(below, above, result, intersections, paths_below_offset, paths_above_offset
#ifndef NDEBUG
        , paths_end
#endif // NDEBUG
        );
}

void Layer::restore_untyped_slices() {
    for (LayerRegionUPtr &layerm : m_regions) {
        layerm->clear();
        layerm->m_slices.set(layerm->get_raw_slices(), stPosInternal | stDensSparse);
        for (auto &srf : layerm->m_slices)
            srf.expolygon.assert_valid();
    }
}

ExPolygons Layer::merged(coordf_t offset_scaled) const
{
    assert(offset_scaled >= 0.f);
    // If no offset is set, apply EPSILON offset before union, and revert it afterwards.
    float offset_scaled2 = 0;
    if (offset_scaled == 0.f) {
        offset_scaled  = float(  SCALED_EPSILON);
        offset_scaled2 = float(- SCALED_EPSILON);
    }
    ExPolygons expolygons;
	for (const LayerRegionUPtr &layerm : m_regions) {
		const PrintRegionConfig &config = layerm->region().config();
		// Our users learned to bend Slic3r to produce empty volumes to act as subtracters. Only add the region if it is non-empty.
        if (config.bottom_solid_layers > 0 || config.top_solid_layers > 0 || config.fill_density > 0. ||
            config.perimeters > 0 || (config.solid_infill_every_layers.value > 0 && config.fill_density.value > 0)) {
            append(expolygons, offset_ex(layerm->slices().surfaces, offset_scaled));
	}
    }
    expolygons = union_ex(expolygons);
	if (offset_scaled2 != 0.f)
        expolygons = offset_ex(expolygons, offset_scaled2);

    // with +- offset, you can have dupicated points. ensure_valid will remove them.
    ensure_valid(expolygons);
    return expolygons;
}

bool config_compatible_for_perimeter(const PrintRegionConfig &config, const PrintRegionConfig &other_config, bool is_first_layer) {
    if (config.perimeter_extruder             == other_config.perimeter_extruder
        && config.perimeters                  == other_config.perimeters
        && config.external_perimeter_acceleration == other_config.external_perimeter_acceleration
        && config.external_perimeter_extrusion_width == other_config.external_perimeter_extrusion_width
        && config.external_perimeter_overlap == other_config.external_perimeter_overlap
        && config.external_perimeter_speed == other_config.external_perimeter_speed // it os mandatory? can't this be set at gcode.cpp?
        && config.external_perimeters_first == other_config.external_perimeters_first
        && config.external_perimeters_first_force == other_config.external_perimeters_first_force
        && config.external_perimeters_hole  == other_config.external_perimeters_hole
        && config.external_perimeters_nothole == other_config.external_perimeters_nothole
        && config.seam_notch_all            == other_config.seam_notch_all
        && config.seam_notch_angle          == other_config.seam_notch_angle
        && config.seam_notch_inner          == other_config.seam_notch_inner
        && config.seam_notch_outer          == other_config.seam_notch_outer
        //&& config.extra_perimeters_below_area == other_config.extra_perimeters_below_area // can be used in modifiers
        //&& config.extra_perimeters_odd_layers == other_config.extra_perimeters_odd_layers // can be used in modifiers
        //&& config.extra_perimeters_on_overhangs == other_config.extra_perimeters_on_overhangs // can be used in modifiers
        //&& config.gap_fill_enabled          == other_config.gap_fill_enabled
        && ((config.gap_fill_speed          == other_config.gap_fill_speed) || (!config.gap_fill_enabled && !other_config.gap_fill_enabled))
        && config.gap_fill_acceleration     == other_config.gap_fill_acceleration
        && config.gap_fill_last             == other_config.gap_fill_last
        && config.gap_fill_flow_match_perimeter == other_config.gap_fill_flow_match_perimeter
        && config.gap_fill_extension         == other_config.gap_fill_extension
        && config.gap_fill_max_width         == other_config.gap_fill_max_width
        && config.gap_fill_min_area         == other_config.gap_fill_min_area
        && config.gap_fill_min_length         == other_config.gap_fill_min_length
        && config.gap_fill_min_width         == other_config.gap_fill_min_width
        // && config.gap_fill_no_overhang         == other_config.gap_fill_no_overhang
        && config.gap_fill_overlap          == other_config.gap_fill_overlap
        && config.gap_fill_perimeter          == other_config.gap_fill_perimeter
        && config.infill_dense              == other_config.infill_dense
        && config.infill_dense_algo         == other_config.infill_dense_algo
        && config.infill_overlap            == other_config.infill_overlap
        && config.no_perimeter_unsupported_algo == other_config.no_perimeter_unsupported_algo
        //&& config.min_width_top_surface == other_config.min_width_top_surface // can be used in modifiers with only_one_perimeter_top
        && (!is_first_layer || config.only_one_perimeter_first_layer == other_config.only_one_perimeter_first_layer)
        //&& config.only_one_perimeter_top    == other_config.only_one_perimeter_top // can be used in modifiers
        //&& config.only_one_perimeter_top_other_algo == other_config.only_one_perimeter_top_other_algo // can be used in modifiers with only_one_perimeter_top
        && config.overhangs_acceleration    == other_config.overhangs_acceleration
        // && config.overhang   == other_config.overhangs
        && config.overhangs_dynamic_speed   == other_config.overhangs_dynamic_speed // need regionSettings in gcode
        && config.overhangs_extrusion_spacing == other_config.overhangs_extrusion_spacing
        && config.overhangs_width_speed     == other_config.overhangs_width_speed
        // && config.overhangs_speed     == other_config.overhangs_speed
        && config.overhangs_dynamic_flow   == other_config.overhangs_dynamic_flow // need regionSettings in printobject::calculateoverhangs
        // && config.overhangs_width           == other_config.overhangs_width
        && config.overhangs_flow_ratio           == other_config.overhangs_flow_ratio // need regionSettings in printobject::calculateoverhangs
     // && config.overhangs_speed_enforce   == other_config.overhangs_speed_enforce
        && config.overhangs_reverse         == other_config.overhangs_reverse
        && config.overhangs_reverse_threshold == other_config.overhangs_reverse_threshold
        && config.perimeter_acceleration    == other_config.perimeter_acceleration
        && config.perimeter_direction       == other_config.perimeter_direction
        && config.perimeter_extrusion_width == other_config.perimeter_extrusion_width
        && config.perimeter_generator       == other_config.perimeter_generator
        && config.perimeter_loop            == other_config.perimeter_loop
        && config.perimeter_loop_seam       == other_config.perimeter_loop_seam
        && config.perimeter_overlap         == other_config.perimeter_overlap
        && config.perimeter_reverse         == other_config.perimeter_reverse
        && config.perimeter_speed           == other_config.perimeter_speed // it is mandatory? can't this be set at gcode.cpp?
        // print -modifier, because region are fused in gode wiew if not.
        && config.print_extrusion_multiplier        == other_config.print_extrusion_multiplier
        && config.print_first_layer_temperature     == other_config.print_first_layer_temperature
        && config.print_retract_length              == other_config.print_retract_length
        && config.print_retract_lift                == other_config.print_retract_lift
        && config.print_temperature                 == other_config.print_temperature
        // end print modifier
        && config.region_gcode              == other_config.region_gcode
        && config.small_perimeter_speed     == other_config.small_perimeter_speed
        && config.small_perimeter_min_length == other_config.small_perimeter_min_length
        && config.small_perimeter_max_length == other_config.small_perimeter_max_length
        //&& config.thin_walls                == other_config.thin_walls // can be used in modifiers
        && config.thin_walls_acceleration   == other_config.thin_walls_acceleration
        //&& config.thin_walls_min_width      == other_config.thin_walls_min_width // can be used in modifiers with thin_walls
        //&& config.thin_walls_overlap        == other_config.thin_walls_overlap // can be used in modifiers with thin_walls
        && config.thin_perimeters           == other_config.thin_perimeters
        && config.thin_perimeters_all       == other_config.thin_perimeters_all
        && config.thin_walls_speed          == other_config.thin_walls_speed
        && config.fuzzy_skin                == other_config.fuzzy_skin
        && config.fuzzy_skin_thickness      == other_config.fuzzy_skin_thickness
        && config.fuzzy_skin_point_dist     == other_config.fuzzy_skin_point_dist)
    {
        if (config.perimeter_generator != PerimeterGeneratorType::Arachne || (
                config.min_bead_width                    == other_config.min_bead_width
            && config.min_feature_size                  == other_config.min_feature_size
            && config.wall_distribution_count           == other_config.wall_distribution_count
            && config.wall_transition_angle             == other_config.wall_transition_angle
            && config.wall_transition_filter_deviation  == other_config.wall_transition_filter_deviation
            && config.wall_transition_length            == other_config.wall_transition_length
            )) {
            return true;
        }
    }
    return false;
}

// Here the perimeters are created cummulatively for all layer regions sharing the same parameters influencing the perimeters.
// The perimeter paths and the thin fills (ExtrusionEntityCollection) are assigned to the first compatible layer region.
// The resulting fill surface is split back among the originating regions.
void Layer::make_perimeters() {
    BOOST_LOG_TRIVIAL(trace) << "Generating perimeters for layer " << this->id();

    if (lslices().empty()) {
        assert(false);
        // there is nothing to make perimeter with.
        return;
    }


    //auto layer_region_reset_perimeters = [](LayerRegion &lregion) { lregion.clear(); };

    // Remove layer islands, remove references to perimeters and fills from these layer islands to LayerRegion
    // ExtrusionEntities.
    // clear
    for (LayerSliceIslandUPtr &island : this->m_islands) {
        island->mutable_regions_islands().clear();
    }
    ExPolygons all_fill_no_overlap_expolygon;
    ExPolygons all_fill_expolygon;

    for (LayerSliceIslandUPtr &island : this->m_islands) {
        // try to merge layerregions
        std::vector<LayerRegionSetCPtrs> regions_groups;
        for (const LayerRegion *lregion : island->regions()) {
            const PrintRegionConfig &config = lregion->region().config();
            // check if compatible with an existing region
            for (LayerRegionSetCPtrs &layer_group : regions_groups) {
                const LayerRegion *other_layerm = *layer_group.begin();
                const PrintRegionConfig &other_config = other_layerm->region().config();
                if (config_compatible_for_perimeter(config, other_config, this->id() == 0)) {
                    layer_group.insert(lregion);
                    goto finished_merging;
                }
            }
            // didn't found any merge, create my own
            regions_groups.emplace_back();
            regions_groups.back().insert(lregion);
        finished_merging:;
        }
        // now do periemter for each group
        //if (regions_groups.size() == 1) {
        //    // use the island expolygon
        //} else {
            for (LayerRegionSetCPtrs &regions : regions_groups) {
                int16_t perimeter_extruder = int16_t((*regions.begin())->region().config().perimeter_extruder) - 1;
                assert(perimeter_extruder >= 0);
                LayerRegionIsland &region_island = island->get_or_add_region_island(regions, uint16_t(perimeter_extruder));
                island->make_perimeters(region_island);
            }
            append(all_fill_expolygon, island->infill_areas());
            if (island->infill_free_areas().empty()) {
                append(all_fill_no_overlap_expolygon, island->infill_areas());
            } else {
                append(all_fill_no_overlap_expolygon, island->infill_free_areas());
            }
        //}
    }

    // fill layerregion surfaces.
    // It's put inside layerregion, because it's easier to do manipulation wher ethe settings are, and merge afterwards.
    // or maybe because it's how it's done and it needs to be revamp.
    all_fill_no_overlap_expolygon = union_safety_offset_ex(all_fill_no_overlap_expolygon);
    for (LayerRegion &lregion : this->regions()) {
        lregion.set_fill_surfaces().clear();
        for (const Surface &raw_srf : lregion.slices()) {
            ExPolygons exp = intersection_ex({raw_srf.expolygon}, all_fill_expolygon);
            lregion.set_fill_surfaces().append(std::move(exp), raw_srf);
        }
        // create all fill_no_overlap_expolygon for LayerRegion (to simplify some computation afterwards)
        lregion.m_fill_no_overlap_expolygons = intersection_ex(lregion.get_raw_slices(), all_fill_no_overlap_expolygon);
        if (lregion.m_fill_no_overlap_expolygons == lregion.get_raw_slices()) {
            ensure_valid(lregion.m_fill_no_overlap_expolygons);
        }
    }
    BOOST_LOG_TRIVIAL(trace) << "Generating perimeters for layer " << this->id() << " - Done";

}

void LayerSliceIsland::make_perimeters(LayerRegionIsland &region_island) {

    const PrintConfig       &print_config  = this->m_layer->object()->print()->config();
    const LayerRegion &default_layerm = **region_island.regions().begin();
    const PrintRegionConfig &region_config = default_layerm.region().config();
    // This needs to be in sync with PrintObject::_slice() slicing_mode_normal_below_layer!
    bool spiral_vase = print_config.spiral_vase &&
        //FIXME account for raft layers.
        (m_layer->id() >= size_t(region_config.bottom_solid_layers.value) &&
         m_layer->scaled_print_z() >= scale_to_layer_coord(region_config.bottom_solid_min_thickness.value));

    Flow bridging_flow = (region_config.overhangs.get_bool() && region_config.overhangs_flow_ratio.is_enabled()) ?
        default_layerm.bridging_flow(frExternalPerimeter) :
        default_layerm.flow(frPerimeter);

    //this is a factory, the content will be copied into the PerimeterGenerator
    PerimeterGenerator::Parameters params(
        m_layer,
        default_layerm.flow(frPerimeter),
        default_layerm.flow(frExternalPerimeter),
        bridging_flow,
        default_layerm.flow(frSolidInfill),
        region_config,
        m_layer->object()->config(),
        print_config,
        spiral_vase,
        (region_config.perimeter_generator.value == PerimeterGeneratorType::Arachne), //use_arachne
        false //has_mod_bridge (don't know yet, toremove)
    );

    // perimeter bonding set.
    if (params.perimeter_flow.spacing_ratio() == 1
        && params.ext_perimeter_flow.spacing_ratio() == 1
        && params.config.external_perimeters_first
        && params.object_config.perimeter_bonding.value > 0) {
        params.infill_gap = (1 - params.object_config.perimeter_bonding.get_effective_value(1)) * params.get_ext_perimeter_spacing();
        params.ext_perimeter_spacing2 -= params.infill_gap;
    }

    //TODO: only get the upper/lower islands taht intersect us (from overlaps_above / overlaps_below)
    const ExPolygons *lower_slices = m_layer->lower_layer ? &m_layer->lower_layer->lslices() : nullptr;
    const ExPolygons *upper_slices = m_layer->upper_layer ? &m_layer->upper_layer->lslices() : nullptr;
    std::vector<BoundingBox> lower_bbox;
    std::vector<BoundingBox> upper_bbox;
    if (lower_slices != nullptr) {
        for (const ExPolygon &lowerp : *lower_slices) {
            lower_bbox.emplace_back(lowerp.contour.points);
        }
    }
    if (upper_slices != nullptr) {
        for (const ExPolygon &upperp : *upper_slices) {
            upper_bbox.emplace_back(upperp.contour.points);
        }
    }
    assert((lower_slices == nullptr && lower_bbox.empty()) || (lower_slices->size() == lower_bbox.size()));
    assert((upper_slices == nullptr && upper_bbox.empty()) || (upper_slices->size() == upper_bbox.size()));

    ExPolygons slices;
    if (region_island.regions() == this->regions()) {
        slices.push_back(this->get_slice());
    } else {
        ExPolygons region_area;
        for (const LayerRegion *lregion : region_island.regions()) {
            append(region_area, lregion->get_raw_slices());
        }
        region_area = union_safety_offset_ex(region_area);
        slices = intersection_ex(region_area, {this->get_slice()});
    }
    for (const ExPolygon &expolygon : slices) {

        // should be on PerimeterGenerator, as it's dependant on the expolygon
        params.region_setting.segregate_regions(expolygon, region_island.regions());
        params.segregate_extra_perimeters(expolygon, region_island.regions());

        PerimeterGenerator::PerimeterGenerator g{params};
        g.throw_if_canceled = [this]() { m_layer->object()->print()->throw_if_canceled(); };

        // only send lower & upper slcies that are overlapping the surfaces bb
        BoundingBox surface_bbox(expolygon.contour.points);
        ExPolygons lower_slices_srf;
        for (size_t i = 0; i < lower_bbox.size(); ++i) {
            if (lower_bbox[i].overlap(surface_bbox)) {
                lower_slices_srf.push_back((*lower_slices)[i]);
            }
        }
        ExPolygons upper_slices_srf;
        for (size_t i = 0; i < upper_bbox.size(); ++i) {
            if (upper_bbox[i].overlap(surface_bbox)) {
                upper_slices_srf.push_back((*upper_slices)[i]);
            }
        }

        ExtrusionEntityCollection &perimeters = region_island.mutable_extrusion(LayerRegionIsland::PERIMETERS);
        ExtrusionEntityCollection &gap_fills = region_island.mutable_extrusion(LayerRegionIsland::GAP_FILLS);

        g.process(
            // input:
            expolygon, &lower_slices_srf, slices, &upper_slices_srf,
            // output:
                // Loops with the external thin walls
            &perimeters,
                // Gaps without the thin walls
            &gap_fills,
                // Infills without the gap fills
            m_infill_areas,
                // mask for "no overlap" area
            m_infill_free_areas
        );

        DEBUG_TREE_VISIT(perimeters, LoopAssertVisitor());

        //remove extrusion collection if empty
        if (perimeters.empty()) {
            assert(gap_fills.empty());
        }
        region_island.remove_empty_extrusions();

        append(m_perimeter_slices, g.perimeter_boundary);
    }
    m_perimeter_slices = union_ex(m_perimeter_slices);
}

bool config_compatible_for_milling(const PrintConfig &print_config, coord_t bottom_z, const PrintRegionConfig &config, const PrintRegionConfig &other_config) {
    if (config.milling_post_process == other_config.milling_post_process &&
        config.milling_extra_size == other_config.milling_extra_size &&
        (config.milling_after_z == other_config.milling_after_z ||
         bottom_z > scale_to_layer_coord(
                                       std::min(config.milling_after_z.get_effective_value(
                                                    print_config.milling_diameter.get_at(0)),
                                                other_config.milling_after_z.get_effective_value(
                                                    print_config.milling_diameter.get_at(0)))))) {
        return true;
    }
    return false;
}
void Layer::make_milling_post_process() {
    if (this->object()->print()->config().milling_diameter.empty()) return;

    BOOST_LOG_TRIVIAL(trace) << "Generating milling_post_process for layer " << this->id();

    // keep track of regions whose perimeters we have already generated
    std::vector<unsigned char> done(m_regions.size(), false);


    for (LayerSliceIslandUPtr &island : this->m_islands) {
        // try to merge layerregions
        std::vector<LayerRegionSetCPtrs> regions_groups;
        for (const LayerRegion *layerm : island->regions()) {
            const PrintRegionConfig &config = layerm->region().config();
            // check if compatible with an existing region
            for (LayerRegionSetCPtrs &layer_group : regions_groups) {
                const LayerRegion *other_layerm = *layer_group.begin();
                const PrintRegionConfig &other_config = other_layerm->region().config();
                if (config_compatible_for_milling(this->object()->print()->config(), this->scaled_bottom_z(), config, other_config)) {
                    layer_group.insert(layerm);
                    goto finished_merging;
                }
            }
            // didn't found any merge, create my own
            regions_groups.emplace_back();
            regions_groups.back().insert(layerm);
        finished_merging:;
        }
        // now do periemter for each group
        if (regions_groups.size() == 1) {
            // use the island expolygon
        } else {
            for (LayerRegionSetCPtrs &regions : regions_groups) {
                LayerRegionIsland &region_island = island->get_or_add_region_island(regions);
                ExPolygons slices;
                if (region_island.regions() == island->regions()) {
                    slices.push_back(island->get_slice());
                } else {
                    ExPolygons region_area;
                    for (const LayerRegion *lregion : region_island.regions()) {
                        append(region_area, lregion->get_raw_slices());
                    }
                    region_area = union_safety_offset_ex(region_area);
                    slices = intersection_ex(region_area, {island->get_slice()});
                }
                MillingPostProcess mill(// input:
                    &slices,
                    (this->lower_layer != nullptr) ? &this->lower_layer->lslices() : nullptr,
                    (*regions.begin())->region().config(),
                    this->object()->config(),
                    this->object()->print()->config()
                );
                region_island.mutable_extrusion(LayerRegionIsland::MILLS) = mill.process(this);
            }
        }
    }
    BOOST_LOG_TRIVIAL(trace) << "Generating milling_post_process for layer " << this->id() << " - Done";
}

void Layer::export_region_slices_to_svg(const char *path) const
{
    BoundingBox bbox;
    for (const LayerRegionUPtr &region : m_regions)
        for (const Surface &surface : region->slices())
            bbox.merge(get_extents(surface.expolygon));
    Point legend_size = export_surface_type_legend_to_svg_box_size();
    Point legend_pos(bbox.min(0), bbox.max(1));
    bbox.merge(Point(std::max(bbox.min(0) + legend_size(0), bbox.max(0)), bbox.max(1) + legend_size(1)));

    SVG svg(path, bbox);
    const float transparency = 0.5f;
    for (const LayerRegionUPtr &region : m_regions)
        for (const Surface &surface : region->slices())
            svg.draw(surface.expolygon, surface_type_to_color_name(surface.surface_type), transparency);
    export_surface_type_legend_to_svg(svg, legend_pos);
    svg.Close(); 
}

// Export to "out/LayerRegion-name-%d.svg" with an increasing index with every export.
void Layer::export_region_slices_to_svg_debug(const char *name) const
{
    static size_t idx = 0;
    this->export_region_slices_to_svg(debug_out_path("Layer-slices-%s-%d.svg", name, idx ++).c_str());
}

void Layer::export_region_fill_surfaces_to_svg(const char *path) const
{
    BoundingBox bbox;
    for (const LayerRegionUPtr &region : m_regions)
        for (const Surface &surface : region->slices())
            bbox.merge(get_extents(surface.expolygon));
    Point legend_size = export_surface_type_legend_to_svg_box_size();
    Point legend_pos(bbox.min(0), bbox.max(1));
    bbox.merge(Point(std::max(bbox.min(0) + legend_size(0), bbox.max(0)), bbox.max(1) + legend_size(1)));

    SVG svg(path, bbox);
    const float transparency = 0.5f;
    for (const LayerRegionUPtr &region : m_regions)
        for (const Surface &surface : region->slices())
            svg.draw(surface.expolygon, surface_type_to_color_name(surface.surface_type), transparency);
    export_surface_type_legend_to_svg(svg, legend_pos);
    svg.Close();
}

// Export to "out/LayerRegion-name-%d.svg" with an increasing index with every export.
void Layer::export_region_fill_surfaces_to_svg_debug(const char *name) const
{
    static size_t idx = 0;
    this->export_region_fill_surfaces_to_svg(debug_out_path("Layer-fill_surfaces-%s-%d.svg", name, idx ++).c_str());
}

void simplify_support_extrusion_path(Layer &layer) {
    const PrintConfig& print_config = layer.object()->print()->config();
    const bool spiral_mode = print_config.spiral_vase;
    const bool enable_arc_fitting = print_config.arc_fitting != ArcFittingType::Disabled && !spiral_mode;
    coordf_t scaled_resolution = scale_d(print_config.resolution.value);
    if (enable_arc_fitting) {
        scaled_resolution = scale_d(print_config.arc_fitting_resolution.get_effective_value(unscaled(scaled_resolution)));
    }
    if (scaled_resolution == 0) scaled_resolution = enable_arc_fitting ? SCALED_EPSILON * 2 : SCALED_EPSILON;
    SimplifyVisitor visitor{scaled_resolution,
                            enable_arc_fitting ? print_config.arc_fitting : ArcFittingType::Disabled,
                            false,
                            &print_config.arc_fitting_tolerance,
                            enable_arc_fitting ? SCALED_EPSILON * 2 : SCALED_EPSILON};
    for (LayerSliceIsland &island : layer.islands()) {
        for (LayerRegionIsland &region_island : island.regions_islands()) {
            if (region_island.has_extrusion(LayerRegionIsland::SUPPORT)) {
                visitor.traverse(region_island.mutable_extrusion(LayerRegionIsland::SUPPORT));
            }
            if (region_island.has_extrusion(LayerRegionIsland::SUPPORT_INTERFACE)) {
                visitor.traverse(region_island.mutable_extrusion(LayerRegionIsland::SUPPORT_INTERFACE));
            }
        }
    }
}

ExtrusionRole support_layer_role(const Layer &layer) {
    ExtrusionRole role = ExtrusionRole::None;
    for (const LayerSliceIsland &island : layer.islands()) {
        for (const LayerRegionIsland &region_island : island.regions_islands()) {
            if (region_island.has_extrusion(LayerRegionIsland::SUPPORT)) {
                role |= ExtrusionRole::SupportMaterial;
            }
            if (region_island.has_extrusion(LayerRegionIsland::SUPPORT_INTERFACE)) {
                role |= ExtrusionRole::SupportMaterialInterface;
            }
        }
    }
    return role;
}
}
