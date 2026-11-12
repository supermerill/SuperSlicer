///|/ Copyright (c) superslicer 2019 - 2025 Durand Rémi @supermerill
///|/ Copyright (c) Prusa Research 2016 - 2023 Vojtěch Bubník @bubnikv, Pavel Mikuš @Godrak, Lukáš Matěna @lukasmatena, Lukáš Hejl @hejllukas
///|/ Copyright (c) Slic3r 2014 - 2016 Alessandro Ranellucci @alranel
///|/
///|/ SuperSlicer, PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "ExPolygon.hpp"
#include "Flow.hpp"
#include "Layer.hpp"
#include "BridgeDetector.hpp"
#include "ClipperUtils.hpp"
#include "Geometry.hpp"
#include "Milling/MillingPostProcess.hpp"
#include "PerimeterGenerator.hpp"
#include "Print.hpp"
#include "Surface.hpp"
#include "BoundingBox.hpp"
#include "SVG.hpp"
#include "Algorithm/RegionExpansion.hpp"

#include <algorithm>
#include <string>
#include <map>

#include <boost/log/trivial.hpp>

namespace Slic3r {

void LayerRegion::clear() {
    this->m_unsupported_bridge_edges.clear();
    this->m_fill_surfaces.clear();
}

Flow LayerRegion::flow(FlowRole role) const
{
    return this->flow(role, (float)m_layer->unscaled_height());
}

Flow LayerRegion::flow(FlowRole role, double layer_height) const
{
    return m_region->flow(*m_layer->object(), role, layer_height, m_layer->id());
}

// Average diameter of nozzles participating on extruding this region.
double LayerRegion::bridging_height_avg_mm() const
{
    const PrintRegionConfig& region_config = this->region().config();
    if (region_config.bridge_type == BridgeType::btFromNozzle) {
        const PrintConfig& print_config = this->layer()->object()->print()->config();
        return region().nozzle_dmr_avg(print_config) * sqrt(region_config.bridge_flow_ratio.get_abs_value(1));
    } else if (region_config.bridge_type == BridgeType::btFromHeight) {
        return this->layer()->unscaled_height();
    } else if (region_config.bridge_type == BridgeType::btFromFlow) {
        return this->bridging_flow(FlowRole::frInfill).height();
    }
    throw Slic3r::InvalidArgument("Unknown BridgeType");
}

Flow LayerRegion::bridging_flow(FlowRole role, BridgeType force_type) const
{
    const PrintRegion       &region         = this->region();
    const PrintRegionConfig &region_config  = region.config();
    const PrintObject       &print_object   = *this->layer()->object();
    const bool is_perimeter = role == frExternalPerimeter || role == frPerimeter;
    // Here this->extruder(role) - 1 may underflow to MAX_INT, but then the get_at() will follback to zero'th element, so everything is all right.
    const float nozzle_diameter = float(print_object.print()->config().nozzle_diameter.get_at(region.extruder(role, *this->layer()->object()) - 1));
    double diameter = 0;
    BridgeType bridge_type = force_type == BridgeType::btNone ?
        (is_perimeter ? region_config.overhangs_type : region_config.bridge_type) :
        force_type;
    if (bridge_type == BridgeType::btFromFlow ) {
        Flow reference_flow = flow(role);
        diameter = sqrt(4 * reference_flow.mm3_per_mm() / PI);
    } else if (bridge_type == BridgeType::btFromHeight) {
        diameter = m_layer->unscaled_height();
    } else /*if (bridge_type == BridgeType::btFromNozzle)*/ {
        // The good Slic3r way: Use rounded extrusions.
        // Get the configured nozzle_diameter for the extruder associated to the flow role requested.
        // Applies default bridge spacing.
        diameter =  nozzle_diameter;
    }
    //assert(!is_perimeter || (region_config.overhangs.get_bool() && region_config.overhangs_flow_ratio.is_enabled()));
    return Flow::bridging_flow(float(sqrt(force_type == BridgeType::btNone ?
                                              (is_perimeter ? region_config.overhangs_flow_ratio.get_abs_value(1.) :
                                                              region_config.bridge_flow_ratio.get_abs_value(1.)) :
                                              0.95f) *
                                     diameter),
                               nozzle_diameter);
    /* else {
        // The same way as other slicers: Use normal extrusions. Apply bridge_flow_ratio while maintaining the original spacing.
        return this->flow(role).with_flow_ratio(region_config.bridge_flow_ratio, overlap_percent);
    }*/
}

const ExPolygons& LayerRegion::fill_expolygons() const {
    uint64_t hash_islands = 0;
    int shift = 0;
    for (const LayerSliceIslandPtr &layer_island_ptr : this->layer()->islands()) {
        if (layer_island_ptr->regions().find(this) != layer_island_ptr->regions().end()) {
            for (const ExPolygon &expoly : layer_island_ptr->fill_expolygons()) {
                hash_islands ^= (uint64_t(&expoly) << shift);
                shift++;
            }
        }
        shift++;
    }
    if (m_cache_fill_expolygons_computed == 0 || m_cache_fill_expolygons_computed != hash_islands) {
        m_cache_fill_expolygons_computed = hash_islands;
        m_cache_fill_expolygons = _compute_fill_expolygons();
    }
    return m_cache_fill_expolygons;
}

ExPolygons LayerRegion::_compute_fill_expolygons() const {
    ExPolygons my_fill_expolygons;
    size_t nb_add = 0;
    for (const LayerSliceIslandPtr &layer_island_ptr : this->layer()->islands()) {
        if (auto &regions = layer_island_ptr->regions(); regions.find(this) != regions.end()) {
            append(my_fill_expolygons, layer_island_ptr->fill_expolygons());
            nb_add++;
        }
    }
    if (nb_add > 1) {
        my_fill_expolygons = union_safety_offset_ex(my_fill_expolygons);
        my_fill_expolygons = intersection_ex(my_fill_expolygons, this->get_raw_slices());
    }
    ensure_valid(my_fill_expolygons);
    return my_fill_expolygons;
}

// Fill in layerm->m_fill_surfaces by trimming the layerm->slices by layerm->fill_expolygons.
void LayerRegion::slices_to_fill_surfaces_clipped(coord_t opening_offset)
{
    const coord_t scaled_resolution = std::max(SCALED_EPSILON, scale_t(this->layer()->object()->print()->config().resolution.value));
    // Collect polygons per surface type.
    std::map<SurfaceType, ExPolygons> polygons_by_surface;
    for (const Surface &surface : this->slices().surfaces) {
        polygons_by_surface[surface.surface_type].push_back(surface.expolygon);
    }
    // Trim surfaces by the fill_boundaries.
    m_fill_surfaces.surfaces.clear();
    for (auto const& [srf_type, expoly] : polygons_by_surface) {
        if (!expoly.empty()) {
            for (ExPolygon &expoly_to_test : ensure_valid(intersection_ex(expoly, fill_expolygons()))) {
                ExPolygons expolys_to_test = expoly_to_test.simplify(
                    std::max(SCALED_EPSILON, scale_t(this->layer()->object()->print()->config().resolution.value)));
                if (!opening_ex(expolys_to_test, opening_offset).empty()) {
                    this->m_fill_surfaces.append({expoly_to_test}, srf_type);
                }
            }
        }
    }
}

#if 1

// Extract surfaces of given type from surfaces, extract fill (layer) thickness of one of the surfaces.
static ExPolygons fill_surfaces_extract_expolygons(Surfaces &surfaces, std::initializer_list<SurfaceType> surface_types, coord_t &thickness)
{
    assert(!surfaces.empty());
    size_t cnt = 0;
    for (const Surface &surface : surfaces)
        if (std::find(surface_types.begin(), surface_types.end(), surface.surface_type) != surface_types.end()) {
            ++cnt;
            assert(surface.scaled_thickness() != 0);
            thickness = surface.scaled_thickness();
        }
    if (cnt == 0)
        return {};

    ExPolygons out;
    out.reserve(cnt);
    for (Surface &surface : surfaces)
        if (std::find(surface_types.begin(), surface_types.end(), surface.surface_type) != surface_types.end())
            out.emplace_back(std::move(surface.expolygon));
    return out;
}

// Extract bridging surfaces from "surfaces", expand them into "shells" using expansion_params,
// detect bridges.
// Trim "shells" by the expanded bridges.
// only used by the new process_external_surfaces
Surfaces expand_bridges_detect_orientations(
    Surfaces                                    &surfaces,
    ExPolygons                                  &shells,
    const Algorithm::RegionExpansionParameters  &expansion_params_into_solid_infill,
    ExPolygons                                  &sparse,
    const Algorithm::RegionExpansionParameters  &expansion_params_into_sparse_infill,
    const float                                 closing_radius,
    const coord_t                               scaled_resolution)
{
    using namespace Slic3r::Algorithm;

    coord_t thickness;
    ExPolygons bridges_ex = fill_surfaces_extract_expolygons(surfaces, {stPosBottom | stDensSolid | stModBridge}, thickness);
    if (bridges_ex.empty())
        return {};

    // Calculate bridge anchors and their expansions in their respective shell region.
    WaveSeeds                       bridge_anchors           = wave_seeds(bridges_ex, shells, expansion_params_into_solid_infill.tiny_expansion, true);
    std::vector<RegionExpansionEx>  bridge_expansions        = propagate_waves_ex(bridge_anchors, shells, expansion_params_into_solid_infill);
    bool                            expanded_into_shells     = ! bridge_expansions.empty();
    bool                            expanded_into_sparse     = false;
    {
        WaveSeeds                       bridge_anchors_sparse    = wave_seeds(bridges_ex, sparse, expansion_params_into_sparse_infill.tiny_expansion, true);
        std::vector<RegionExpansionEx>  bridge_expansions_sparse = propagate_waves_ex(bridge_anchors_sparse, sparse, expansion_params_into_sparse_infill);
        if (! bridge_expansions_sparse.empty()) {
            expanded_into_sparse = true;
            for (WaveSeed &seed : bridge_anchors_sparse)
                seed.boundary += uint32_t(shells.size());
            for (RegionExpansionEx &expansion : bridge_expansions_sparse)
                expansion.boundary_id += uint32_t(shells.size());
            append(bridge_anchors,    std::move(bridge_anchors_sparse));
            append(bridge_expansions, std::move(bridge_expansions_sparse));
        }
    }

    // Cache for detecting bridge orientation and merging regions with overlapping expansions.
    struct Bridge {
        ExPolygon                                       expolygon;
        uint32_t                                        group_id;
        std::vector<RegionExpansionEx>::const_iterator  bridge_expansion_begin;
        double                                          angle = -1;
    };
    std::vector<Bridge> bridges;
    {
        bridges.reserve(bridges_ex.size());
        uint32_t group_id = 0;
        for (ExPolygon &ex : bridges_ex)
            bridges.push_back({ std::move(ex), group_id ++, bridge_expansions.end() });
        bridges_ex.clear();
    }

    // Group the bridge surfaces by overlaps.
    auto group_id = [&bridges](uint32_t src_id) {
        uint32_t group_id = bridges[src_id].group_id;
        while (group_id != src_id) {
            src_id = group_id;
            group_id = bridges[src_id].group_id;
        }
        bridges[src_id].group_id = group_id;
        return group_id;
    };

    {
        // Cache of bboxes per expansion boundary.
        std::vector<BoundingBox> bboxes;
        // Detect overlaps of bridge anchors inside their respective shell regions.
        // bridge_expansions are sorted by boundary id and source id.
        for (auto it = bridge_expansions.begin(); it != bridge_expansions.end();) {
            // For each boundary region:
            auto it_begin = it;
            auto it_end   = std::next(it_begin);
            for (; it_end != bridge_expansions.end() && it_end->boundary_id == it_begin->boundary_id; ++ it_end) ;
            bboxes.clear();
            bboxes.reserve(it_end - it_begin);
            for (auto it2 = it_begin; it2 != it_end; ++ it2)
                bboxes.emplace_back(get_extents(it2->expolygon.contour));
            // For each bridge anchor of the current source:
            for (; it != it_end; ++ it) {
                // A grup id for this bridge.
                for (auto it2 = std::next(it); it2 != it_end; ++ it2)
                    if (it->src_id != it2->src_id &&
                        bboxes[it - it_begin].overlap(bboxes[it2 - it_begin]) &&
                        // One may ignore holes, they are irrelevant for intersection test.
                        ! intersection(it->expolygon.contour, it2->expolygon.contour).empty()) {
                        // The two bridge regions intersect. Give them the same (lower) group id.
                        uint32_t id  = group_id(it->src_id);
                        uint32_t id2 = group_id(it2->src_id);
                        if (id < id2)
                            bridges[id2].group_id = id;
                        else
                            bridges[id].group_id = id2;
                    }
            }
        }
    }

    // Detect bridge directions.
    {
        std::sort(bridge_anchors.begin(), bridge_anchors.end(), Algorithm::lower_by_src_and_boundary);
        auto it_bridge_anchor = bridge_anchors.begin();
        Lines lines;
        Polygons anchor_areas;
        for (uint32_t bridge_id = 0; bridge_id < uint32_t(bridges.size()); ++ bridge_id) {
            Bridge &bridge = bridges[bridge_id];
//            lines.clear();
            anchor_areas.clear();
            int32_t last_anchor_id = -1;
            for (; it_bridge_anchor != bridge_anchors.end() && it_bridge_anchor->src == bridge_id; ++ it_bridge_anchor) {
                if (last_anchor_id != int(it_bridge_anchor->boundary)) {
                    last_anchor_id = int(it_bridge_anchor->boundary);
                    append(anchor_areas, to_polygons(last_anchor_id < int32_t(shells.size()) ? shells[last_anchor_id] : sparse[last_anchor_id - int32_t(shells.size())]));
                }
//                if (Points &polyline = it_bridge_anchor->path; polyline.size() >= 2) {
//                    reserve_more_power_of_2(lines, polyline.size() - 1);
//                    for (size_t i = 1; i < polyline.size(); ++ i)
//                        lines.push_back({ polyline[i - 1], polyline[1] });
//                }
            }
            lines = to_lines(diff_pl(to_polylines(bridge.expolygon), expand(anchor_areas, float(SCALED_EPSILON))));
            auto [bridging_dir, unsupported_dist] = detect_bridging_direction(lines, to_polygons(bridge.expolygon));
            bridge.angle = M_PI + std::atan2(bridging_dir.y(), bridging_dir.x());
#if 0
            coordf_t    stroke_width = scale_(0.06);
            BoundingBox bbox         = get_extents(anchor_areas);
            bbox.merge(get_extents(bridge.expolygon));
            bbox.offset(scale_(1.));
            ::Slic3r::SVG
                svg(debug_out_path(("bridge" + std::to_string(bridge.angle) + "_" /* + std::to_string(this->layer()->bottom_z())*/).c_str()),
                bbox);
            svg.draw(bridge.expolygon, "cyan");
            svg.draw(lines, "green", stroke_width);
            svg.draw(anchor_areas, "red");
#endif
        }
    }

    // Merge the groups with the same group id, produce surfaces by merging source overhangs with their newly expanded anchors.
    Surfaces out;
    {
        Polygons acc;
        Surface templ{ stPosBottom | stDensSolid | stModBridge, {} };
        std::sort(bridge_expansions.begin(), bridge_expansions.end(), [](auto &l, auto &r) {
            return l.src_id < r.src_id || (l.src_id == r.src_id && l.boundary_id < r.boundary_id);
        });
        for (auto it = bridge_expansions.begin(); it != bridge_expansions.end(); ) {
            bridges[it->src_id].bridge_expansion_begin = it;
            uint32_t src_id = it->src_id;
            for (++ it; it != bridge_expansions.end() && it->src_id == src_id; ++ it) ;
        }
        for (uint32_t bridge_id = 0; bridge_id < uint32_t(bridges.size()); ++ bridge_id)
            if (group_id(bridge_id) == bridge_id) {
                // Head of the group.
                acc.clear();
                for (uint32_t bridge_id2 = bridge_id; bridge_id2 < uint32_t(bridges.size()); ++ bridge_id2)
                    if (group_id(bridge_id2) == bridge_id) {
                        append(acc, to_polygons(std::move(bridges[bridge_id2].expolygon)));
                        auto it_bridge_expansion = bridges[bridge_id2].bridge_expansion_begin;
                        assert(it_bridge_expansion == bridge_expansions.end() || it_bridge_expansion->src_id == bridge_id2);
                        for (; it_bridge_expansion != bridge_expansions.end() && it_bridge_expansion->src_id == bridge_id2; ++ it_bridge_expansion)
                            append(acc, to_polygons(std::move(it_bridge_expansion->expolygon)));
                    }
                //FIXME try to be smart and pick the best bridging angle for all?
                templ.bridge_angle = bridges[bridge_id].angle;
                //NOTE: The current regularization of the shells can create small unasigned regions in the object (E.G. benchy)
                // without the following closing operation, those regions will stay unfilled and cause small holes in the expanded surface.
                // look for narrow_ensure_vertical_wall_thickness_region_radius filter.
                ExPolygons final = closing_ex(acc, closing_radius);
                // without safety offset, artifacts are generated (GH #2494)
                // union_safety_offset_ex(acc)
                for (ExPolygon &ex : final)
                    out.emplace_back(templ, std::move(ex));
            }
    }
    ensure_valid(out, scaled_resolution);

    // Clip by the expanded bridges.
    if (expanded_into_shells)
        shells = diff_ex(shells, out);
    if (expanded_into_sparse)
        sparse = diff_ex(sparse, out);
    return out;
}

// Extract bridging surfaces from "surfaces", expand them into "shells" using expansion_params.
// Trim "shells" by the expanded bridges.
static Surfaces expand_merge_surfaces(
    Surfaces                                   &surfaces,
    SurfaceType                                 surface_type,
    ExPolygons                                  &shells,
    const Algorithm::RegionExpansionParameters  &expansion_params_into_solid_infill,
    ExPolygons                                  &sparse,
    const Algorithm::RegionExpansionParameters  &expansion_params_into_sparse_infill,
    const coordf_t                              closing_radius,
    const coord_t                               scaled_resolution,
    const double                                bridge_angle
#ifdef _DEBUG
    ,std::string                                 svg_name
#endif
    )
{
    using namespace Slic3r::Algorithm;

    coord_t thickness;
    ExPolygons src = fill_surfaces_extract_expolygons(surfaces, {surface_type}, thickness);
    if (src.empty())
        return {};
    ExPolygons init_src = src;
    ExPolygons init_shells = shells;
    ExPolygons sparse_shells = sparse;
    std::vector<RegionExpansion> expansions = propagate_waves(src, shells, expansion_params_into_solid_infill);
    bool                         expanded_into_shells = !expansions.empty();
    bool                         expanded_into_sparse = false;
    {
        std::vector<RegionExpansion> expansions2 = propagate_waves(src, sparse, expansion_params_into_sparse_infill);
        if (! expansions2.empty()) {
            expanded_into_sparse = true;
            for (RegionExpansion &expansion : expansions2)
                expansion.boundary_id += uint32_t(shells.size());
            append(expansions, std::move(expansions2));
        }
    }

    std::vector<ExPolygon> expanded = merge_expansions_into_expolygons(std::move(src), std::move(expansions));
    //NOTE: The current regularization of the shells can create small unasigned regions in the object (E.G. benchy)
    // without the following closing operation, those regions will stay unfilled and cause small holes in the expanded surface.
    // look for narrow_ensure_vertical_wall_thickness_region_radius filter.
    expanded = closing_ex(expanded, closing_radius);
    ensure_valid(expanded/*, scaled_resolution*/);
    // Trim the shells by the expanded expolygons.
    if (expanded_into_shells)
        shells = diff_ex(shells, expanded);
    if (expanded_into_sparse)
        sparse = diff_ex(sparse, expanded);

    Surface templ{ surface_type, {} };
    templ.bridge_angle = bridge_angle;
    Surfaces out;
    out.reserve(expanded.size());
    for (auto &expoly : expanded)
        out.emplace_back(templ, std::move(expoly));
    return out;
}

void LayerRegion::process_external_surfaces(const Layer *lower_layer, const Polygons *lower_layer_covered)
{
    using namespace Slic3r::Algorithm;
#ifdef _DEBUG
    //assert each surface is not on top of each other (or almost)
    for (auto &srf : m_fill_surfaces.surfaces) {
        for (auto &srf2 : m_fill_surfaces.surfaces) {
            if (&srf != &srf2) {
                ExPolygons intersect = intersection_ex(srf.expolygon, srf2.expolygon);
                intersect = offset2_ex(intersect, -SCALED_EPSILON * 2, SCALED_EPSILON);
                double area = 0;
                for (auto &expoly : intersect) {
                    area += expoly.area();
                }
                // assert(area < SCALED_EPSILON * SCALED_EPSILON /** 100*/);
                assert(area < scale_t(1) * scale_t(1));
            }
        }
    }
#endif

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    export_region_fill_surfaces_to_svg_debug("4_process_external_surfaces-initial");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */

    // Width of the perimeters.
    coord_t shell_width = 0;
    coord_t expansion_min = 0;
    if (int num_perimeters = this->region().config().perimeters; num_perimeters > 0) {
        Flow external_perimeter_flow = this->flow(frExternalPerimeter);
        Flow perimeter_flow          = this->flow(frPerimeter);
        shell_width  = 0.5f * external_perimeter_flow.scaled_width() + external_perimeter_flow.scaled_spacing();
        shell_width += perimeter_flow.scaled_spacing() * (num_perimeters - 1);
        expansion_min = perimeter_flow.scaled_spacing();
    } else {
        // TODO: Maybe there is better solution when printing with zero perimeters, but this works reasonably well, given the situation
        shell_width   = 0;//SCALED_EPSILON;
        expansion_min = 0;//SCALED_EPSILON;
    }
    
    coord_t                         expansion_solid = shell_width;
    coord_t                         expansion_bottom_bridge = shell_width;
    const bool has_infill = this->region().config().fill_density.value > 0.;
    //if no infill, reduce the margin for everything to only the perimeter
    if (!has_infill) {
        coord_t margin = scale_t(this->region().config().external_infill_margin.get_abs_value(unscaled(shell_width)));
        coord_t margin_bridged = scale_t(this->region().config().bridged_infill_margin.get_abs_value(this->flow(frExternalPerimeter).width()));
        expansion_solid = std::min(margin, shell_width);
        expansion_bottom_bridge = std::min(margin_bridged, shell_width);
    } else {
        expansion_solid = scale_t(this->region().config().external_infill_margin.get_abs_value(unscaled(shell_width)));
        expansion_bottom_bridge = scale_t(this->region().config().bridged_infill_margin.get_abs_value(this->flow(frExternalPerimeter).width()));
    }
    if (expansion_min <= 0) {
        expansion_min = SCALED_EPSILON;
    }
    if (expansion_solid <= 0) {
        expansion_solid = SCALED_EPSILON;
    }
    if (expansion_bottom_bridge <= 0) {
        expansion_bottom_bridge = SCALED_EPSILON;
    }
    expansion_min = std::min(expansion_min, expansion_solid);

    // Scaled expansions of the respective external surfaces.
    coord_t                         expansion_top           = expansion_solid;
    coord_t                         expansion_bottom        = expansion_top;
    // Don't take more than max_nr_steps for small expansion_step.
    static constexpr const size_t   max_nr_expansion_steps  = 5;
    // Expand by waves of expansion_step size (expansion_step is scaled), but with no more steps than max_nr_expansion_steps.
    coord_t                         expansion_step          = std::max(coord_t(expansion_solid / max_nr_expansion_steps), expansion_min / 2);
        //std::min(this->flow(frPerimeter).scaled_width() / 4, expansion_min);
    // Radius (with added epsilon) to absorb empty regions emering from regularization of ensuring, viz  const float narrow_ensure_vertical_wall_thickness_region_radius = 0.5f * 0.65f * min_perimeter_infill_spacing;
    const coordf_t closing_radius = 0.55f * 0.65f * 1.05f * this->flow(frSolidInfill).scaled_spacing();
    const coord_t scaled_resolution = std::max(SCALED_EPSILON, scale_t(this->layer()->object()->print()->config().resolution.value));

    // Expand the top / bottom / bridge surfaces into the shell thickness solid infills.
    coord_t     layer_solid_thickness  = 0;
    coord_t     layer_sparse_thickness = 0;
    ExPolygons shells = union_ex(fill_surfaces_extract_expolygons(m_fill_surfaces.surfaces, { stPosInternal | stDensSolid }, layer_solid_thickness));
    ExPolygons sparse = union_ex(fill_surfaces_extract_expolygons(m_fill_surfaces.surfaces, { stPosInternal | stDensSparse }, layer_sparse_thickness));
    ExPolygons init_shells = shells;
    ExPolygons init_sparse = sparse;
#ifdef _DEBUG
    for (auto &srf : m_fill_surfaces.surfaces) {
        assert(srf.surface_type == (stPosInternal | stDensSolid) ||
            srf.surface_type == (stPosInternal | stDensSparse) ||
            //srf.surface_type == (stPosInternal | stDensSolid | stModBridge) || // not created yet
            //srf.surface_type == (stPosInternal | stDensSparse | stModBridge) || // not created yet
            srf.surface_type == (stPosInternal | stDensVoid) ||
            srf.surface_type == (stPosTop | stDensSolid) ||
            srf.surface_type == (stPosBottom | stDensSolid) ||
            srf.surface_type == (stPosBottom | stDensSolid | stModBridge));

    }
#endif
    SurfaceCollection bridges;
    const auto expansion_params_into_sparse_infill = RegionExpansionParameters::build(expansion_min, expansion_step, max_nr_expansion_steps);
    {
        BOOST_LOG_TRIVIAL(trace) << "Processing external surface, detecting bridges. layer" << this->layer()->unscaled_print_z();
        const double custom_angle = this->region().config().bridge_angle.value;
        const auto   expansion_params_into_solid_infill  = RegionExpansionParameters::build(expansion_bottom_bridge, expansion_step, max_nr_expansion_steps);
        if (this->region().config().bridge_angle.is_enabled()) {
            bridges.surfaces = expand_merge_surfaces(m_fill_surfaces.surfaces, stPosBottom | stDensSolid | stModBridge, shells, expansion_params_into_solid_infill, sparse, expansion_params_into_sparse_infill, closing_radius, scaled_resolution, Geometry::deg2rad(custom_angle)
#ifdef _DEBUG
            , std::to_string(this->layer()->id()) + "_expand_merge_surfaces_bridge_"
#endif
            );
            for(auto&srf : bridges.surfaces) srf.expolygon.assert_valid();
        } else {
            bridges.surfaces = expand_bridges_detect_orientations(m_fill_surfaces.surfaces, shells, expansion_params_into_solid_infill, sparse, expansion_params_into_sparse_infill, closing_radius, scaled_resolution);
            for(auto&srf : bridges.surfaces) srf.expolygon.assert_valid();
        }
        BOOST_LOG_TRIVIAL(trace) << "Processing external surface, detecting bridges - done";
#if 0
        {
            static int iRun = 0;
            bridges.export_to_svg(debug_out_path("bridges-after-grouping-%d.svg", iRun++), true);
        }
#endif
    }

    Surfaces    bottoms = expand_merge_surfaces(m_fill_surfaces.surfaces, stPosBottom | stDensSolid, shells,
        RegionExpansionParameters::build(expansion_bottom, expansion_bottom/max_nr_expansion_steps , max_nr_expansion_steps), 
        sparse, expansion_params_into_sparse_infill, closing_radius, scaled_resolution, -1
#ifdef _DEBUG
            , std::to_string(this->layer()->id()) + "_expand_merge_surfaces_bot_"
#endif
            );
    Surfaces    tops    = expand_merge_surfaces(m_fill_surfaces.surfaces, stPosTop | stDensSolid, shells,
        RegionExpansionParameters::build(expansion_top, expansion_top / max_nr_expansion_steps, max_nr_expansion_steps), 
        sparse, expansion_params_into_sparse_infill, closing_radius, scaled_resolution, -1
#ifdef _DEBUG
            , std::to_string(this->layer()->id()) + "_expand_merge_surfaces_top_"
#endif
            );

    m_fill_surfaces.remove_types({
        stPosBottom | stDensSolid | stModBridge,
        stPosBottom | stDensSolid,
        stPosTop | stDensSolid,
        stPosInternal | stDensSparse,
        //stPosInternal | stDensSparse | stModBridge, // not yet created
        stPosInternal | stDensSolid });
#ifdef _DEBUG
    for (auto &srf : m_fill_surfaces.surfaces) {
        assert(srf.surface_type == (stPosInternal | stDensVoid));
    }
#endif
    reserve_more(m_fill_surfaces.surfaces, shells.size() + sparse.size() + bridges.size() + bottoms.size() + tops.size());
    if (!shells.empty()) {
        Surface solid_templ(stPosInternal | stDensSolid, {});
        assert(layer_solid_thickness == -1); // should be >0 but it's set on infill, well after here, we do that for nothing.
        if (layer_solid_thickness > 0) {
            solid_templ.set_scaled_thickness(layer_solid_thickness);
        }
        ensure_valid(shells/*, scaled_resolution*/);
        m_fill_surfaces.append(std::move(shells), solid_templ);
    } else {
        //assert(layer_solid_thickness == 0);
    }
    if (!sparse.empty()) {
        Surface sparse_templ(stPosInternal | stDensSparse, {});
        assert(layer_sparse_thickness -1); // should be >0 but it's set on infill, well after here, we do that for nothing.
        if (layer_sparse_thickness > 0) {
            sparse_templ.set_scaled_thickness(layer_sparse_thickness);
        }
        ensure_valid(sparse/*, scaled_resolution*/);
        m_fill_surfaces.append(std::move(sparse), sparse_templ);
    } else {
        // assert(layer_sparse_thickness == 0);
    }
    for(auto&srf : bridges.surfaces) srf.expolygon.assert_valid();
    for(auto&srf : bottoms) srf.expolygon.assert_valid();
    for(auto&srf : tops) srf.expolygon.assert_valid();
    m_fill_surfaces.append(std::move(bridges.surfaces));
    m_fill_surfaces.append(std::move(bottoms));
    m_fill_surfaces.append(std::move(tops));

#ifdef _DEBUG
    //assert each surface is not on top of each other (or almost)
    for (auto &srf : m_fill_surfaces.surfaces) {
        for (auto &srf2 : m_fill_surfaces.surfaces) {
            if (&srf != &srf2) {
                ExPolygons intersect = intersection_ex(srf.expolygon, srf2.expolygon);
                intersect = offset2_ex(intersect, -SCALED_EPSILON * 2, SCALED_EPSILON);
                double area = 0;
                for (auto &expoly : intersect) {
                    area += expoly.area();
                }
                // assert(area < SCALED_EPSILON * SCALED_EPSILON /** 100*/);
                assert(area < scale_t(1) * scale_t(1));
            }
        }
    }
#endif

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    export_region_fill_surfaces_to_svg_debug("4_process_external_surfaces-final");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
}
//#else

//#define EXTERNAL_SURFACES_OFFSET_PARAMETERS ClipperLib::jtMiter, 3.
//#define EXTERNAL_SURFACES_OFFSET_PARAMETERS ClipperLib::jtMiter, 1.5
#define EXTERNAL_SURFACES_OFFSET_PARAMETERS ClipperLib::jtSquare, 0.

size_t get_island_idx(const Polygon &contour,
                      const std::vector<BoundingBox> &bboxes,
                      const ExPolygons &fill_boundaries) {
    assert(bboxes.size() == fill_boundaries.size());
    std::vector<size_t> candidates;
    for (size_t idx = 0; idx < bboxes.size(); ++idx) {
        if (bboxes[idx].contains(contour.front()) && bboxes[idx].contains(contour.points[contour.size() / 2])) {
            candidates.push_back(idx);
        }
    }
    assert(!candidates.empty());
    if (candidates.size() > 1) {
        for (size_t i = 0; i < candidates.size(); ++i) {
            if (!bboxes[candidates[i]].contains(contour.points)) {
                candidates.erase(candidates.begin() + i);
                --i;
            }
        }
    }
    assert(!candidates.empty());
    // note: fill_boundaries don't overlap, you only need to test one point.
    if (candidates.size() > 1) {
        for (size_t i = 0; i < candidates.size(); ++i) {
            if (!fill_boundaries[candidates[i]].contains(contour.front())) {
                candidates.erase(candidates.begin() + i);
                --i;
            }
        }
    }
    if (candidates.size() < 0) {
        //failed becasue of some epsilon, try with another point
        for (size_t idx = 0; idx < bboxes.size(); ++idx) {
            if (bboxes[idx].contains(contour.points[1])) {
                candidates.push_back(idx);
            }
        }
        if (candidates.size() > 1) {
            for (size_t i = 0; i < candidates.size(); ++i) {
                if (!fill_boundaries[candidates[i]].contains(contour.points[1])) {
                    candidates.erase(candidates.begin() + i);
                    --i;
                }
            }
        }
    }
    if (candidates.size() < 0) {
        //failed because of some margins, try with shrunk polygon
        const Polygons contours_shrunk = offset(contour, -scale_t(0.05));
        if (!contours_shrunk.empty()) {
            const Polygon &contour_shrunk = contours_shrunk.front();
            for (size_t idx = 0; idx < bboxes.size(); ++idx) {
                if (bboxes[idx].contains(contour_shrunk.front())) {
                    candidates.push_back(idx);
                }
            }
            if (candidates.size() > 1) {
                for (size_t i = 0; i < candidates.size(); ++i) {
                    if (!fill_boundaries[candidates[i]].contains(contour_shrunk.front())) {
                        candidates.erase(candidates.begin() + i);
                        --i;
                    }
                }
            }
        }
    }
    assert(candidates.size() == 1);
    return candidates.size() == 1 ? candidates.front() : -1;
}

void LayerRegion::process_external_surfaces_old(const Layer *lower_layer, const Polygons *lower_layer_covered)
{

    coord_t max_margin = 0;
    if ((this->region().config().perimeters > 0)) {
        max_margin = (this->flow(frExternalPerimeter).scaled_width() + this->flow(frPerimeter).scaled_spacing()) /2 +
            this->flow(frPerimeter).scaled_spacing() * (this->region().config().perimeters.value - 1);
    }
    const Surfaces &surfaces = this->m_fill_surfaces.surfaces;
    const bool has_infill = this->region().config().fill_density.value > 0.;
    coord_t margin = scale_t(this->region().config().external_infill_margin.get_abs_value(unscaled(max_margin)));
    coord_t margin_bridged = scale_t(this->region().config().bridged_infill_margin.get_abs_value(this->flow(frExternalPerimeter).width()));
    //if no infill, reduce the margin for everything to only the perimeter
    if (!has_infill) {
        margin = std::min(margin, max_margin);
        margin_bridged = std::min(margin_bridged, max_margin);
    }
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    export_region_fill_surfaces_to_svg_debug("4_process_external_surfaces-initial");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */

    // 1) Collect bottom and bridge surfaces, each of them grown by a parametrised ~3mm offset
    // for better anchoring.
    // Bottom surfaces, grown.
    Surfaces                    bottom;
    // Bridge surfaces, initialy not grown.
    Surfaces                    bridges;
    // Top surfaces, grown.
    Surfaces                    top;
    // Internal surfaces, not grown.
    Surfaces                    internal;
    // Areas, where an infill of various types (top, bottom, bottom bride, sparse, void) could be placed.
    Polygons                    lower_layer_covered_tmp;
    ExPolygons                  fill_boundaries = this->fill_expolygons();
    assert_valid(fill_boundaries);

    // Collect top surfaces and internal surfaces.
    // Collect fill_boundaries: If we're slicing with no infill, we can't extend external surfaces over non-existent infill.
    // This loop destroys the surfaces (aliasing this->fill_surfaces.surfaces) by moving into top/internal/fill_boundaries!
    {
        coord_t min_half_width = this->flow(frSolidInfill).scaled_width() / 2;
        // to search your fill island
        std::vector<BoundingBox> fill_ex_bboxes = get_extents_vector(this->fill_expolygons());
        // Voids are sparse infills if infill rate is zero.
        Polygons voids;
        bool has_infill = this->region().config().fill_density.value > 0.;
        //transfert surfaces from m_fill_surfaces to specialized collection (bottom, bridges, top, internal, ...)
        for (Surface &surface : this->set_fill_surfaces()) {
            assert(! surface.empty());
            if (! surface.empty()) {
                if (surface.has_pos_top()) {
                    // Collect the top surfaces, inflate them and trim them by the bottom surfaces.
                    // This gives the priority to bottom surfaces.
                    // collapse & grow
                    ExPolygons shrunk_expoly = offset_ex({surface.expolygon},
                                                         double(-min_half_width / 10),
                                                         EXTERNAL_SURFACES_OFFSET_PARAMETERS);
                    if (!shrunk_expoly.empty()) {
                        ExPolygons grown_expoly = offset_ex(shrunk_expoly, double(margin + min_half_width / 10),
                                                            EXTERNAL_SURFACES_OFFSET_PARAMETERS);
                        // ensure it's printable.
                        if (margin < min_half_width) {
                            grown_expoly = offset2_ex(shrunk_expoly, -double(min_half_width), double(min_half_width),
                                                      EXTERNAL_SURFACES_OFFSET_PARAMETERS);
                        }
                        if (!grown_expoly.empty()) {
                            // intersect with our island to avoid growing inside another island
                            // note: this is done on one of the shrunk_expoly instead of the surface.expolygon, as this may fail because of epsilons.
                            size_t island_idx = get_island_idx(shrunk_expoly.front().contour, fill_ex_bboxes, this->fill_expolygons());
                            if (island_idx < this->fill_expolygons().size()) { // sanity check.
                                grown_expoly = intersection_ex(grown_expoly, {this->fill_expolygons()[island_idx]});
                            }
                            surfaces_append(top, std::move(grown_expoly), surface);
                        }
                    }
                } else if (surface.has_pos_bottom() && (!surface.has_mod_bridge() || lower_layer == nullptr)) {
                    // collapse & grow
                    ExPolygons shrunk_expoly = offset_ex({surface.expolygon},
                                                         double(-min_half_width / 10),
                                                         EXTERNAL_SURFACES_OFFSET_PARAMETERS);
                    if (!shrunk_expoly.empty()) {
                        ExPolygons grown_expoly = offset_ex(shrunk_expoly, double(margin + min_half_width / 10),
                                                            EXTERNAL_SURFACES_OFFSET_PARAMETERS);
                        // ensure it's printable.
                        if (margin < min_half_width) {
                            grown_expoly = offset2_ex(shrunk_expoly, -double(min_half_width), double(min_half_width),
                                                      EXTERNAL_SURFACES_OFFSET_PARAMETERS);
                        }
                        if (!grown_expoly.empty()) {
                            // intersect with our island to avoid growing inside another island
                            // note: this is done on one of the shrunk_expoly instead of the surface.expolygon, as this may fail because of epsilons.
                            size_t island_idx = get_island_idx(shrunk_expoly.front().contour, fill_ex_bboxes, this->fill_expolygons());
                            if (island_idx < this->fill_expolygons().size()) { // sanity check.
                                grown_expoly = intersection_ex(grown_expoly, {this->fill_expolygons()[island_idx]});
                            }
                            surfaces_append(bottom, std::move(grown_expoly), surface);
                        }
                    }
                } else if (surface.has_pos_bottom() && surface.has_mod_bridge()) {
                    bridges.emplace_back(std::move(surface));
                } else if (has_infill || !(surface.has_pos_internal())) { //i'm totally confused here.
                    assert(surface.has_pos_internal());
                    if (!surface.has_pos_external())
                        // Make a copy as the following line uses the move semantics.
                        internal.push_back(surface);
                    fill_boundaries.push_back(std::move(surface.expolygon));
                } else {
                    assert(surface.has_pos_internal());
                    //push surface as perimeter-only inside the fill_boundaries
                    if (margin_bridged > 0) {
                        ExPolygons peri_poly = diff_ex(ExPolygons() = { surface.expolygon }, offset_ex(surface.expolygon, -margin_bridged));
                        append(fill_boundaries, peri_poly);
                    }
                    if (!surface.has_pos_external()){
                        if (!has_infill && lower_layer != nullptr)
                            polygons_append(voids, surface.expolygon);
                        internal.push_back(std::move(surface));
                    }
                }
            }
        }
        set_fill_surfaces().clear();
        if (!voids.empty()) {
            // There are some voids (empty infill regions) on this layer. Usually one does not want to expand
            // any infill into these voids, with the exception the expanded infills are supported by layers below
            // with nonzero inill.
            assert(! has_infill && lower_layer != nullptr);
            // Remove voids from fill_boundaries, that are not supported by the layer below.
            if (lower_layer_covered == nullptr) {
                lower_layer_covered = &lower_layer_covered_tmp;
            	lower_layer_covered_tmp = to_polygons(lower_layer->lslices());
            }
            if (! lower_layer_covered->empty())
                // Allow the top / bottom surfaces to expand into the voids of this layer if supported by the layer below.
            	voids = diff(voids, *lower_layer_covered);
            if (! voids.empty())
                fill_boundaries = diff_ex(fill_boundaries, voids);
        }
    }

#if 0
    {
        static int iRun = 0;
        bridges.export_to_svg(debug_out_path("bridges-before-grouping-%d.svg", iRun ++), true);
    }
#endif
    
    fill_boundaries = union_safety_offset_ex(fill_boundaries);
    if (!bridges.empty())
    {
        // 1) Calculate the inflated bridge regions, each constrained to its island.
        std::vector<Polygons>    bridges_grown;
        std::vector<BoundingBox> bridge_bboxes;

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
        {
            static int iRun = 0;
            SVG svg(debug_out_path("4_process_external_surfaces-fill_regions-%d.svg", iRun ++).c_str(), get_extents(fill_boundaries));
            svg.draw(fill_boundaries);
            svg.draw_outline(fill_boundaries, "black", "blue", scale_(0.05)); 
            svg.Close();
        }
//        export_region_fill_surfaces_to_svg_debug("4_process_external_surfaces-initial");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
 
        {
            coord_t min_half_width = this->flow(frSolidInfill).scaled_width() / 2;
            // Bridge expolygons, grown, to be tested for intersection with other bridge regions.
            std::vector<BoundingBox> fill_boundaries_bboxes = get_extents_vector(fill_boundaries);
            bridges_grown.reserve(bridges.size());
            bridge_bboxes.reserve(bridges.size());
            for (size_t i = 0; i < bridges.size(); ++ i) {
                // Find the island of this bridge.
                const Point pt = bridges[i].expolygon.contour.points.front();
                int idx_island = get_island_idx(bridges[i].expolygon.contour, fill_boundaries_bboxes, fill_boundaries);
                for (int j = 0; j < int(fill_boundaries.size()); ++ j)
                    if (fill_boundaries_bboxes[j].contains(pt) && 
                        fill_boundaries[j].contains(pt)) {
                        idx_island = j;
                        break;
                    }
                // Grown by bridged_infill_margin.
                Polygons polys;
                if (idx_island == -1) {
                    BOOST_LOG_TRIVIAL(trace) << "Bridge did not fall into the source region!";
                } else {
                    // also, remove all bridge area that are thinner than a single line.
                    ExPolygons expoly_collapsed = offset2_ex(ExPolygons{bridges[i].expolygon}, (-min_half_width),
                                                             (min_half_width) + SCALED_EPSILON,
                                                             EXTERNAL_SURFACES_OFFSET_PARAMETERS);
                    // is there something left?
                    if (!expoly_collapsed.empty()) {
                        // check if there is something cut
                        ExPolygons cut = diff_ex(ExPolygons{bridges[i].expolygon}, expoly_collapsed,
                                                 ApplySafetyOffset::Yes);
                        //double area_cut = 0;
                        //for (ExPolygon &c : cut) {
                        //    area_cut += c.area();
                        //}
                        // can remove thin panhandle , very useful in some cases.
                        if (expoly_collapsed.size() != 1 && 
                            !cut.empty()
                            //area_cut > min_half_width * min_half_width
                            ) {
                            // if area not negligible, we will consider it.
                            // compute the bridge area, if any.
                            ExPolygons ex_polys = offset_ex(expoly_collapsed, float(margin_bridged),
                                                            EXTERNAL_SURFACES_OFFSET_PARAMETERS);
                            polys = to_polygons(ex_polys);
                            // add the cut section as solid infill
                            Surface srf_bottom = bridges[i];
                            srf_bottom.surface_type = stPosBottom | stDensSolid;
                            // clip it to the infill area and remove the bridge part.
                            surfaces_append(bottom,
                                            diff_ex(intersection_ex(ExPolygons{fill_boundaries[idx_island]},
                                                                    offset_ex(cut, double(margin),
                                                                              EXTERNAL_SURFACES_OFFSET_PARAMETERS)),
                                                    ex_polys),
                                            srf_bottom);
                        } else {
                            // negligible, don't offset2
                            polys = offset(to_polygons(bridges[i].expolygon), float(margin_bridged),
                                           EXTERNAL_SURFACES_OFFSET_PARAMETERS);
                        }
                        // Found an island, to which this bridge region belongs. Trim the expanded bridging region
                        // with its source region, so it does not overflow into a neighbor region.
                        polys = intersection(polys, fill_boundaries[idx_island]);
                    } else {
                        // add the cut section as solid infill
                        Surface &srf_bottom = bridges[i];
                        srf_bottom.surface_type = stPosBottom | stDensSolid;
                        // will be removed just below, we can move it
                        bottom.push_back(std::move(srf_bottom));
                    }
                }
                //keep bridges & bridge_bboxes & bridges_grown the SAME SIZE
                if (!polys.empty()) {
                    bridge_bboxes.push_back(get_extents(polys));
                    bridges_grown.push_back(std::move(polys));
                } else {
                    bridges.erase(bridges.begin() + i);
                    --i;
                }
            }
        }
        if (bridges.empty())
        {
            fill_boundaries = union_safety_offset_ex(fill_boundaries);
        } else {
            // 2) Group the bridge surfaces by overlaps.
            std::vector<size_t> bridge_group(bridges.size(), (size_t)-1);
            size_t n_groups = 0; 
            for (size_t i = 0; i < bridges.size(); ++ i) {
                assert(!bridges[i].expolygon.empty());
                // A group id for this bridge.
                size_t group_id = (bridge_group[i] == size_t(-1)) ? (n_groups ++) : bridge_group[i];
                bridge_group[i] = group_id;
                // For all possibly overlaping bridges:
                for (size_t j = i + 1; j < bridges.size(); ++ j) {
                    if (! bridge_bboxes[i].overlap(bridge_bboxes[j]))
                        continue;
                    if (intersection(bridges_grown[i], bridges_grown[j]).empty())
                        continue;
                    // The two bridge regions intersect. Give them the same group id.
                    if (bridge_group[j] != size_t(-1)) {
                        // The j'th bridge has been merged with some other bridge before.
                        size_t group_id_new = bridge_group[j];
                        for (size_t k = 0; k < j; ++ k)
                            if (bridge_group[k] == group_id)
                                bridge_group[k] = group_id_new;
                        group_id = group_id_new;
                    }
                    bridge_group[j] = group_id;
                }
            }

            // 3) Merge the groups with the same group id, detect bridges.
            {
                BOOST_LOG_TRIVIAL(trace) << "Processing external surface, detecting bridges. layer" << this->layer()->unscaled_print_z() << ", bridge groups: " << n_groups;
                for (size_t group_id = 0; group_id < n_groups; ++ group_id) {
                    size_t n_bridges_merged = 0;
                    size_t idx_last = (size_t)-1;
                    for (size_t i = 0; i < bridges.size(); ++ i) {
                        if (bridge_group[i] == group_id) {
                            ++ n_bridges_merged;
                            idx_last = i;
                        }
                    }
                    if (n_bridges_merged == 0)
                        // This group has no regions assigned as these were moved into another group.
                        continue;
                    // Collect the initial ungrown regions and the grown polygons.
                    ExPolygons  initial;
                    ExPolygons    grown;
                    for (size_t i = 0; i < bridges.size(); ++ i) {
                        if (bridge_group[i] != group_id)
                            continue;
                        initial.push_back(std::move(bridges[i].expolygon));
                        append(grown, union_ex(bridges_grown[i]));
                    }
                    // detect bridge direction before merging grown surfaces otherwise adjacent bridges
                    // would get merged into a single one while they need different directions
                    // also, supply the original expolygon instead of the grown one, because in case
                    // of very thin (but still working) anchors, the grown expolygon would go beyond them

// new fast bridge direction estimation which "minimizes amount of unanchored bridge endpoints"
#if 0 
                    if (this->region().config().bridge_angle.is_enabled()) {
                        double custom_angle = Geometry::deg2rad(this->region().config().bridge_angle.value);
                        // Bridge was not detected (likely it is only supported at one side). Still it is a surface filled in
                        // using a bridging flow, therefore it makes sense to respect the custom bridging direction.
                        bridges[idx_last].bridge_angle = custom_angle;
                    } else {
                        auto [bridging_dir, unsupported_dist] = detect_bridging_direction(to_polygons(initial), to_polygons(lower_layer->lslices()));
                        bridges[idx_last].bridge_angle = PI + std::atan2(bridging_dir.y(), bridging_dir.x());

                        // #if 1
                        //     coordf_t    stroke_width = scale_(0.06);
                        //     BoundingBox bbox         = get_extents(initial);
                        //     bbox.offset(scale_(1.));
                        //     ::Slic3r::SVG
                        //     svg(debug_out_path(("bridge"+std::to_string(bridges[idx_last].bridge_angle)+"_"+std::to_string(this->layer()->bottom_z())).c_str()),
                        //     bbox);

                        //     svg.draw(initial, "cyan");
                        //     svg.draw(to_lines(lower_layer->lslices()), "green", stroke_width);
                        // #endif
                    }

// old bridge direction (that fill m_unsupported_bridge_edges as intended)
#else
                    BridgeDetector bd(
                        initial,
                        lower_layer->lslices(),
                        this->bridging_flow(frInfill).scaled_spacing(),
                        scale_t(this->layer()->object()->print()->config().bridge_precision.get_abs_value(this->bridging_flow(frInfill).spacing())),
                        this->layer()->id()
                    );
                    #ifdef SLIC3R_DEBUG
                    printf("Processing bridge at layer %zu:\n", this->layer()->id());
                    #endif
                    if (this->region().config().bridge_angle.is_enabled()) {
                        double custom_angle = Geometry::deg2rad(this->region().config().bridge_angle.value);
                        // Bridge was not detected (likely it is only supported at one side). Still it is a surface filled in
                        // using a bridging flow, therefore it makes sense to respect the custom bridging direction.
                        bridges[idx_last].bridge_angle = custom_angle;
                    } else if (bd.detect_angle()) {
                        bridges[idx_last].bridge_angle = bd.angle;
                        if (this->layer()->object()->has_support()) {
//                        polygons_append(this->bridged, intersection(bd.coverage(), to_polygons(initial)));
                            append(this->m_unsupported_bridge_edges, bd.unsupported_edges());
                        }
                    } else {
                        bridges[idx_last].bridge_angle = 0;
                    }
#endif 
                    // without safety offset, artifacts are generated (GH #2494)
                    surfaces_append(bottom, union_safety_offset_ex(grown), bridges[idx_last]);
                }
                BOOST_LOG_TRIVIAL(trace) << "Processing external surface, detecting bridges - done";
            }
        }
    }

    Surfaces new_surfaces;
    const coord_t scaled_resolution = std::max(SCALED_EPSILON, scale_t(this->layer()->object()->print()->config().resolution.value));
    {
        // Intersect the grown surfaces with the actual fill boundaries.
        ExPolygons bottom_expolygons = to_expolygons(bottom);
        // Merge top and bottom in a single collection.
        surfaces_append(top, std::move(bottom));
        for (size_t i = 0; i < top.size(); ++ i) {
            Surface &s1 = top[i];
            if (s1.empty())
                continue;
            ExPolygons expolys;
            expolys.push_back(std::move(s1.expolygon));
            for (size_t j = i + 1; j < top.size(); ++ j) {
                Surface &s2 = top[j];
                if (! s2.empty() && surfaces_could_merge(s1, s2)) {
                    //polygons_append(polys, to_polygons(std::move(s2)));
                    expolys.push_back(std::move(s2.expolygon));
                    s2.clear();
                }
            }
            if (s1.has_pos_top()) {
                // Trim the top surfaces by the bottom surfaces. This gives the priority to the bottom surfaces.
                expolys = diff_ex(expolys, bottom_expolygons);
            }
            surfaces_append(
                new_surfaces,
                // Don't use a safety offset as fill_boundaries were already united using the safety offset.
                ensure_valid(intersection_ex(expolys, fill_boundaries)/*, scaled_resolution*/),
                s1);
            //s1.clear();
        }
    }

    // Subtract the new top surfaces from the other non-top surfaces and re-add them.
    ExPolygons new_expolygons = to_expolygons(new_surfaces);
    for (size_t i = 0; i < internal.size(); ++ i) {
        Surface &s1 = internal[i];
        if (s1.empty())
            continue;
        ExPolygons expolys;
        expolys.push_back(std::move(s1.expolygon));
        for (size_t j = i + 1; j < internal.size(); ++ j) {
            Surface &s2 = internal[j];
            if (! s2.empty() && surfaces_could_merge(s1, s2)) {
                expolys.push_back(std::move(s2.expolygon));
                s2.clear();
            }
        }
        ExPolygons new_expolys = diff_ex(expolys, new_expolygons);
        new_expolygons = union_ex(new_expolygons, new_expolys);
        surfaces_append(new_surfaces, ensure_valid(std::move(new_expolys)/*, scaled_resolution*/), s1);
        //s1.clear();
    }
    
    set_fill_surfaces().surfaces = std::move(new_surfaces);

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    export_region_fill_surfaces_to_svg_debug("4_process_external_surfaces-final");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
}
#endif

void LayerRegion::prepare_fill_surfaces()
{
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    export_region_slices_to_svg_debug("2_prepare_fill_surfaces-initial");
    export_region_fill_surfaces_to_svg_debug("2_prepare_fill_surfaces-initial");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */ 

    /*  Note: in order to make the psPrepareInfill step idempotent, we should never
        alter fill_surfaces boundaries on which our idempotency relies since that's
        the only meaningful information returned by psPerimeters. */
    
    bool spiral_vase = this->layer()->object()->print()->config().spiral_vase;
    coordf_t scaled_resolution = std::max(SCALED_EPSILON, scale_t(this->layer()->object()->print()->config().resolution.value));

    // if no solid layers are requested, turn top/bottom surfaces to internal
    // For Lightning infill, infill_only_where_needed is ignored because both
    // do a similar thing, and their combination doesn't make much sense.
    if (!spiral_vase &&
        (this->region().config().top_solid_layers == 0 &&
         (this->region().config().solid_infill_every_layers.value != 1 ||
          this->region().config().fill_density.value == 0))) {
        for (Surface &surface : m_fill_surfaces)
            if (surface.has_pos_top())
                surface.surface_type =
                        /*( this->layer()->object()->config().infill_only_where_needed &&
                        !this->region().config().infill_dense.value
                        && this->region().config().fill_pattern != ipLightning) ?
                    stPosInternal | stDensVoid :*/ stPosInternal | stDensSparse;
    }
    if (this->region().config().bottom_solid_layers == 0) {
        for (Surface &surface : m_fill_surfaces)
            if (surface.has_pos_bottom())
                surface.surface_type = stPosInternal | stDensSparse;
    }

    // turn too small internal regions into solid regions according to the user setting
    if (!spiral_vase && this->region().config().fill_density.value > 0) {
        // apply solid_infill_below_area
        // scaling an area requires two calls!
        double min_area = scale_(scale_(this->region().config().solid_infill_below_area.value));
        for (Surface &surface : m_fill_surfaces)
            if (surface.has_fill_sparse() && surface.has_pos_internal() && surface.area() <= min_area)
                surface.surface_type = stPosInternal | stDensSolid;
        // also Apply solid_infill_below_width
        double   spacing            = this->flow(frSolidInfill).spacing();
        coordf_t scaled_spacing     = scale_d(spacing);
        coordf_t min_half_width = scale_d(this->region().config().solid_infill_below_width.get_abs_value(spacing)) / 2;
        if (min_half_width > 0) {
            Surfaces srfs_to_add;
            for (Surfaces::iterator surface = this->m_fill_surfaces.surfaces.begin();
                 surface != this->m_fill_surfaces.surfaces.end(); ++surface) {
                if (surface->has_fill_sparse() && surface->has_pos_internal()) {
                    // try to collapse the surface
                    // grow it a bit more to have an easy time to intersect
                    ExPolygons results = offset2_ex({surface->expolygon}, -min_half_width - SCALED_EPSILON,
                                                    min_half_width + SCALED_EPSILON +
                                                        std::min(scaled_spacing / 5, min_half_width / 5));
                    // TODO: find a way to have both intersect & cut
                    ExPolygons cut = diff_ex(ExPolygons{surface->expolygon}, results);
                    ExPolygons intersect = intersection_ex(ExPolygons{surface->expolygon}, results);
                    if (intersect.size() == 1 && cut.empty())
                        continue;
                    if (!intersect.empty()) {
                        ensure_valid(intersect/*, scaled_resolution*/);
                        ensure_valid(cut/*, scaled_resolution*/);
                        //not possible to have empty cut with more than one intersect
                        assert(!cut.empty());
                        surface->expolygon = std::move(intersect[0]);
                        for (int i = 1; i < intersect.size(); i++) {
                            srfs_to_add.emplace_back(*surface, std::move(intersect[i]));
                        }
                        for (ExPolygon& expoly : cut) {
                            srfs_to_add.emplace_back(*surface, std::move(expoly));
                            srfs_to_add.back().surface_type = stPosInternal | stDensSolid;
                        }
                    } else {
                        //no intersec => all in solid
                        assert(cut.size() == 1);
                        surface->surface_type = stPosInternal | stDensSolid;
                    }
                }
            }
            for(auto &srf : srfs_to_add) srf.expolygon.assert_valid();
            append(this->m_fill_surfaces.surfaces, std::move(srfs_to_add));
        }
    }
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    export_region_slices_to_svg_debug("2_prepare_fill_surfaces-final");
    export_region_fill_surfaces_to_svg_debug("2_prepare_fill_surfaces-final");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
}



double LayerRegion::infill_area_threshold() const
{
    double ss = this->flow(frSolidInfill).scaled_spacing();
    return ss*ss;
}

void LayerRegion::trim_surfaces(const Polygons &trimming_polygons)
{
#ifndef NDEBUG
    for (const Surface &surface : this->slices()) {
        assert(surface.surface_type == (stPosInternal | stDensSparse));
        surface.expolygon.assert_valid();
    }
#endif /* NDEBUG */
    coordf_t scaled_resolution = std::max(SCALED_EPSILON, scale_t(this->layer()->object()->print()->config().resolution.value));
    this->m_slices.set(ensure_valid(intersection_ex(this->slices().surfaces, trimming_polygons)/*, scaled_resolution*/), stPosInternal | stDensSparse);
    for(auto &srf : this->m_slices) srf.expolygon.assert_valid();
}

void LayerRegion::elephant_foot_compensation_step(const float elephant_foot_compensation_perimeter_step, const Polygons &trimming_polygons)
{
#ifndef NDEBUG
    for (const Surface &surface : this->slices()) {
        assert(surface.surface_type == (stPosInternal | stDensSparse));
        surface.expolygon.assert_valid();
    }
#endif /* NDEBUG */
    assert(elephant_foot_compensation_perimeter_step >= 0);
    Polygons tmp = intersection(this->slices().surfaces, trimming_polygons);
    append(tmp, diff(this->slices().surfaces, opening(this->slices().surfaces, elephant_foot_compensation_perimeter_step)));
    this->m_slices.set(union_ex(tmp), stPosInternal | stDensSparse);
    for(auto &srf : this->m_slices) srf.expolygon.assert_valid();
}

void LayerRegion::export_region_slices_to_svg(const char *path) const
{
    BoundingBox bbox;
    for (const Surface& surface : this->slices())
        bbox.merge(get_extents(surface.expolygon));
    Point legend_size = export_surface_type_legend_to_svg_box_size();
    Point legend_pos(bbox.min(0), bbox.max(1));
    bbox.merge(Point(std::max(bbox.min(0) + legend_size(0), bbox.max(0)), bbox.max(1) + legend_size(1)));

    SVG svg(path, bbox);
    const float transparency = 0.5f;
    for (const Surface &surface : this->slices())
        svg.draw(surface.expolygon, surface_type_to_color_name(surface.surface_type, 0.9f), transparency);
    for (const Surface &surface : this->fill_surfaces())
        svg.draw(to_polylines(surface.expolygon), surface_type_to_color_name(surface.surface_type), scale_t(0.1));
    export_surface_type_legend_to_svg(svg, legend_pos);
    svg.Close();
}

// Export to "out/LayerRegion-name-%d.svg" with an increasing index with every export.
void LayerRegion::export_region_slices_to_svg_debug(const char *name) const
{
    static std::map<std::string, size_t> idx_map;
    size_t &idx = idx_map[name];
    this->export_region_slices_to_svg(debug_out_path("LayerRegion-slices-%s-%d.svg", name, idx ++).c_str());
}

void LayerRegion::export_region_fill_surfaces_to_svg(const char *path) const
{
    BoundingBox bbox;
    for (const Surface &surface : this->fill_surfaces())
        bbox.merge(get_extents(surface.expolygon));
    Point legend_size = export_surface_type_legend_to_svg_box_size();
    Point legend_pos(bbox.min(0), bbox.max(1));
    bbox.merge(Point(std::max(bbox.min(0) + legend_size(0), bbox.max(0)), bbox.max(1) + legend_size(1)));

    SVG svg(path, bbox);
    const float transparency = 0.5f;
    for (const Surface &surface : this->fill_surfaces()) {
        svg.draw(surface.expolygon, surface_type_to_color_name(surface.surface_type), transparency);
        svg.draw_outline(surface.expolygon, "black", "blue", scale_(0.05)); 
    }
    export_surface_type_legend_to_svg(svg, legend_pos);
    svg.Close();
}

// Export to "out/LayerRegion-name-%d.svg" with an increasing index with every export.
void LayerRegion::export_region_fill_surfaces_to_svg_debug(const char *name) const
{
    static std::map<std::string, size_t> idx_map;
    size_t &idx = idx_map[name];
    this->export_region_fill_surfaces_to_svg(debug_out_path("LayerRegion-fill_surfaces-%s-%d.svg", name, idx ++).c_str());
}

}
 
