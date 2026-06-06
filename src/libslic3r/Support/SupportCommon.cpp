///|/ Copyright (c) SuperSlicer 2026 Durand R?mi @supermerill
///|/ Copyright (c) Prusa Research 2023 Vojtěch Bubník @bubnikv, Pavel Mikuš @Godrak
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "SupportCommon.hpp"

#include <cassert>
#include <cmath>

#include <boost/container/static_vector.hpp>
#include <oneapi/tbb/parallel_for.h>

#include "libslic3r/Api/internal/LayerAccess.hpp"
#include "libslic3r/Api/internal/LayerRegionAccess.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ClipperZUtils.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/ExtrusionEntityVisitors.hpp"
#include "libslic3r/Fill/FillBase.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/MutablePolygon.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/PointUtils.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintObject.hpp"
#include "libslic3r/PrintRegion.hpp"
#include "libslic3r/Thread.hpp"

#include "SupportLayer.hpp"
#include "SupportParameters.hpp"

// #define SLIC3R_DEBUG
// Make assert active if SLIC3R_DEBUG

#ifdef SLIC3R_DEBUG
    #define DEBUG
    #define _DEBUG
    #undef NDEBUG
    #include "libslic3r/utils.hpp"
    #include "libslic3r/SVG.hpp"
#endif

namespace Slic3r::FFFSupport {

// how much we extend support around the actual contact area
//FIXME this should be dependent on the nozzle diameter!
#define SUPPORT_MATERIAL_MARGIN 1.5 

//#define SUPPORT_SURFACES_OFFSET_PARAMETERS ClipperLib::jtMiter, 3.
//#define SUPPORT_SURFACES_OFFSET_PARAMETERS ClipperLib::jtMiter, 1.5
#define SUPPORT_SURFACES_OFFSET_PARAMETERS ClipperLib::jtSquare, 0.

void remove_bridges_from_contacts(
    const PrintConfig   &print_config, 
    const Layer         &lower_layer,
    const LayerRegion   &layerm,
    coord_t              flow_width,
    ExPolygons          &contact_polygons)
{
    // compute the area of bridging perimeters
    ExPolygons bridges;
    {
        coordf_t nozzle_diameter = scale_i(print_config.nozzle_diameter.get_at(layerm.region().config().perimeter_extruder-1));
        // Surface supporting this layer, expanded by 0.5 * nozzle_diameter, as we consider this kind of overhang to be sufficiently supported.
        Polygons lower_grown_slices = expand(lower_layer.lslices(),
            //FIXME to mimic the decision in the perimeter generator, we should use half the external perimeter width.
            0.5f * float(nozzle_diameter),
            SUPPORT_SURFACES_OFFSET_PARAMETERS);
        // Collect perimeters of this layer.
        //FIXME split_at_first_point() could split a bridge mid-way

        //get all periemters lines in relation with this region
        // they can be outside of the region slices, it's not importnat as it's for removing area.
        ArcPolylines perimeters_arcpolylines;
        for (const LayerSliceIsland &layer_island : layerm.layer()->islands()) {
            for (const LayerRegionIsland &region_island : layer_island.regions_islands()) {
                if (region_island.has_extrusion(LayerRegionIsland::PERIMETERS) &&
                    region_island.regions().find(&layerm) != region_island.regions().end()) {
                    append(perimeters_arcpolylines, region_island.extrusion(LayerRegionIsland::PERIMETERS).as_polylines());
                }
                if (region_island.has_extrusion(LayerRegionIsland::GAP_FILLS) &&
                    region_island.regions().find(&layerm) != region_island.regions().end()) {
                    append(perimeters_arcpolylines, region_island.extrusion(LayerRegionIsland::GAP_FILLS).as_polylines());
                }
            }
        }

    #if 0
        Polylines overhang_perimeters = layerm.perimeters.as_polylines();
        // workaround for Clipper bug, see Slic3r::Polygon::clip_as_polyline()
        for (Polyline &polyline : overhang_perimeters)
            polyline.points[0].x += 1;
        // Trim the perimeters of this layer by the lower layer to get the unsupported pieces of perimeters.
        overhang_perimeters = diff_pl(overhang_perimeters, lower_grown_slices);
    #else
        Polylines overhang_perimeters = diff_pl(to_polylines(perimeters_arcpolylines, nozzle_diameter*2), lower_grown_slices);
    #endif
        
        // only consider straight overhangs
        // only consider overhangs having endpoints inside layer's slices
        // convert bridging polylines into polygons by inflating them with their thickness
        // since we're dealing with bridges, we can't assume width is larger than spacing,
        // so we take the largest value and also apply safety offset to be ensure no gaps
        // are left in between
        Flow perimeter_bridge_flow = layerm.bridging_flow(frPerimeter);
        //FIXME one may want to use a maximum of bridging flow width and normal flow width, as the perimeters are calculated using the normal flow
        // and then turned to bridging flow, thus their centerlines are derived from non-bridging flow and expanding them by a bridging flow
        // may not expand them to the edge of their respective islands.
        const float w = float(0.5 * std::max(perimeter_bridge_flow.scaled_width(), perimeter_bridge_flow.scaled_spacing())) + scale_d(0.001);
        for (Polyline &polyline : overhang_perimeters)
            if (polyline.is_straight()) {
                // This is a bridge 
                polyline.extend_start(distf_t(flow_width));
                polyline.extend_end(distf_t(flow_width));
                // Is the straight perimeter segment supported at both sides?
                //check if the first and last point are supported. If so, it's a bridge.
                Point pts[2]       = { polyline.first_point(), polyline.last_point() };
                bool  supported[2] = { false, false };
                for (size_t i = 0; i < lower_layer.lslices().size() && ! (supported[0] && supported[1]); ++ i)
                    for (int j = 0; j < 2; ++ j)
                        if (! supported[j] && lower_layer.islands()[i].get_bounding_box().contains(pts[j]) && lower_layer.lslices()[i].contains(pts[j]))
                            supported[j] = true;
                if (supported[0] && supported[1])
                    // Offset a polyline into a thick line.
                    append(bridges, to_expolygons(offset(polyline, w)));
            }
        bridges = union_ex(bridges);
    }
    // remove the entire bridges and only support the unsupported edges
    //FIXME the brided regions are already collected as layerm.bridged. Use it?
    for (const Surface &surface : layerm.fill_surfaces()) {
        if (surface.has_pos_bottom() && surface.has_mod_bridge() && surface.bridge_angle >= 0.0) {
            bridges.push_back(surface.expolygon);
        }
    }
    //FIXME add the gap filled areas. Extrude the gaps with a bridge flow?
    // Remove the unsupported ends of the bridges from the bridged areas.
    //FIXME add supports at regular intervals to support long bridges!
    bridges = diff_ex(bridges,
            // Offset unsupported edges into polygons.
            offset(layerm.unsupported_bridge_edges(), scale_d(SUPPORT_MATERIAL_MARGIN), SUPPORT_SURFACES_OFFSET_PARAMETERS));
    // Remove bridged areas from the supported areas.
    contact_polygons = diff_ex(contact_polygons, bridges, ApplySafetyOffset::Yes);

    #ifdef SLIC3R_DEBUG
        static int iRun = 0;
        SVG::export_expolygons(debug_out_path("support-top-contacts-remove-bridges-run%d.svg", iRun ++),
            { { { union_ex(offset(layerm.unsupported_bridge_edges(), scale_(SUPPORT_MATERIAL_MARGIN), SUPPORT_SURFACES_OFFSET_PARAMETERS)) }, { "unsupported_bridge_edges", "orange", 0.5f } },
              { { union_ex(contact_polygons) },            { "contact_polygons",           "blue",   0.5f } },
              { { union_ex(bridges) },                     { "bridges",                    "red",    "black", "", scale_i(0.1f), 0.5f } } });
    #endif /* SLIC3R_DEBUG */
}

// Convert some of the intermediate layers into top/bottom interface layers as well as base interface layers.
std::pair<SupportGeneratorLayersPtr, SupportGeneratorLayersPtr> generate_interface_layers(
    const PrintObjectConfig           &config,
    const SupportParameters           &support_params,
    const SupportGeneratorLayersPtr   &bottom_contacts,
    const SupportGeneratorLayersPtr   &top_contacts,
    // Input / output, will be merged with output. Only provided for Organic supports.
    SupportGeneratorLayersPtr         &top_interface_layers,
    SupportGeneratorLayersPtr         &top_base_interface_layers,
    // Input, will be trimmed with the newly created interface layers.
    SupportGeneratorLayersPtr         &intermediate_layers,
    SupportGeneratorLayerStorage      &layer_storage)
{
    std::pair<SupportGeneratorLayersPtr, SupportGeneratorLayersPtr> base_and_interface_layers;

    if (! intermediate_layers.empty() && support_params.has_interfaces()) {
        // For all intermediate layers, collect top contact surfaces, which are not further than support_material_interface_layers.
        BOOST_LOG_TRIVIAL(debug) << "PrintObjectSupportMaterial::generate_interface_layers() in parallel - start";
        const bool snug_supports   = config.support_material_style.value == smsSnug;
        const bool smooth_supports = config.support_material_style.value != smsGrid;
        SupportGeneratorLayersPtr &interface_layers       = base_and_interface_layers.first;
        SupportGeneratorLayersPtr &base_interface_layers  = base_and_interface_layers.second;
        interface_layers.assign(intermediate_layers.size(), nullptr);
        if (support_params.has_base_interfaces())
            base_interface_layers.assign(intermediate_layers.size(), nullptr);
        const auto smoothing_distance    = support_params.support_material_interface_flow.scaled_spacing() * 1.5;
        const auto minimum_island_radius = support_params.support_material_interface_flow.scaled_spacing() / support_params.interface_density;
        const auto closing_distance      = smoothing_distance; // scale_d(config.support_material_closing_radius.value);
        // Insert a new layer into base_interface_layers, if intersection with base exists.
        auto insert_layer = [&layer_storage, smooth_supports, closing_distance, smoothing_distance, minimum_island_radius, &support_params](
                SupportGeneratorLayer &intermediate_layer, Polygons &bottom, Polygons &&top, SupportGeneratorLayer *top_interface_layer, 
                const Polygons *subtract, SupporLayerType type) -> SupportGeneratorLayer* {
            ensure_valid(bottom);
            ensure_valid(top);
            bool has_top_interface = top_interface_layer && ! top_interface_layer->polygons.empty();
            assert(! bottom.empty() || ! top.empty() || has_top_interface);
            // Merge top into bottom, unite them with a safety offset.
            append(bottom, std::move(top));
            // Merge top / bottom interfaces. For snug supports, merge using closing distance and regularize (close concave corners).
            bottom = intersection(
                smooth_supports ?
                    smooth_outward(closing(std::move(bottom), closing_distance + minimum_island_radius, closing_distance, SUPPORT_SURFACES_OFFSET_PARAMETERS), smoothing_distance) :
                    union_safety_offset(std::move(bottom)),
                intermediate_layer.polygons);
            if (has_top_interface) {
                // Don't trim the precomputed Organic supports top interface with base layer
                // as the precomputed top interface likely expands over multiple tree tips.
                bottom = union_(std::move(top_interface_layer->polygons), bottom);
                top_interface_layer->polygons.clear();
            }
            if (! bottom.empty()) {
                assert(support_params.resolution >= SCALED_EPSILON);
                ensure_valid(bottom, support_params.resolution);
                //FIXME Remove non-printable tiny islands, let them be printed using the base support.
                //bottom = opening(std::move(bottom), minimum_island_radius);
                if (! bottom.empty()) {
                    assert_valid(bottom);
                    SupportGeneratorLayer &layer_new = top_interface_layer ? *top_interface_layer : layer_storage.allocate(type);
                    layer_new.polygons   = std::move(bottom);
                    layer_new.set_scaled_print_z(intermediate_layer.scaled_print_z());
                    layer_new.set_scaled_bottom_z(intermediate_layer.scaled_bottom_z());
                    layer_new.set_scaled_height(intermediate_layer.scaled_height());
                    layer_new.bridging   = intermediate_layer.bridging;
                    layer_new.resolution = intermediate_layer.resolution;
                    // Subtract the interface from the base regions.
                    intermediate_layer.polygons = diff(intermediate_layer.polygons, layer_new.polygons);
                    ensure_valid(intermediate_layer.polygons, support_params.resolution);
                    assert_valid(intermediate_layer.polygons);
                    if (subtract){
                        // Trim the base interface layer with the interface layer.
                        layer_new.polygons = ensure_valid(support_params.resolution, diff(std::move(layer_new.polygons), *subtract));
                    //FIXME filter layer_new.polygons islands by a minimum area?
        //                  $interface_area = [ grep abs($_->area) >= $area_threshold, @$interface_area ];
                    }
                    assert_valid(layer_new.polygons);
                    return &layer_new;
                }
            }
            return nullptr;
        };
        Slic3r::parallel_for(size_t(0), intermediate_layers.size(),
            [&bottom_contacts, &top_contacts, &top_interface_layers, &top_base_interface_layers, &intermediate_layers, &insert_layer, &support_params,
             snug_supports, &interface_layers, &base_interface_layers](size_t idx_intermediate_layer) {                
                // Gather the top / bottom contact layers intersecting with num_interface_layers resp. num_interface_layers_only intermediate layers above / below
                // this intermediate layer.
                // Index of the first top contact layer intersecting the current intermediate layer.
                auto idx_top_contact_first        = -1;
                // Index of the first bottom contact layer intersecting the current intermediate layer.
                auto idx_bottom_contact_first     = -1;
                // Index of the first top interface layer intersecting the current intermediate layer.
                auto idx_top_interface_first      = -1;
                // Index of the first top contact interface layer intersecting the current intermediate layer.
                auto idx_top_base_interface_first = -1;
                auto num_intermediate = int(intermediate_layers.size());
                {
                    SupportGeneratorLayer &intermediate_layer = *intermediate_layers[idx_intermediate_layer];
                    assert_valid(intermediate_layer.polygons);
                    Polygons polygons_top_contact_projected_interface;
                    Polygons polygons_top_contact_projected_base;
                    Polygons polygons_bottom_contact_projected_interface;
                    Polygons polygons_bottom_contact_projected_base;
                    if (support_params.num_top_interface_layers > 0) {
                        // Top Z coordinate of a slab, over which we are collecting the top / bottom contact surfaces
                        coord_t top_z =
                            intermediate_layers[std::min(num_intermediate - 1, int(idx_intermediate_layer + support_params.num_top_interface_layers) - 1)]
                                ->scaled_print_z();
                        coord_t top_inteface_z     = std::numeric_limits<coord_t>::max();
                        if (support_params.num_top_base_interface_layers > 0)
                            // Some top base interface layers will be generated.
                            top_inteface_z = support_params.num_top_interface_layers_only() == 0 ?
                                // Only base interface layers to generate.
                                (-std::numeric_limits<coordf_t>::max()) :
                                intermediate_layers[std::min(num_intermediate - 1, int(idx_intermediate_layer + support_params.num_top_interface_layers_only()) - 1)]
                                    ->scaled_print_z();
                        // Move idx_top_contact_first up until above the current print_z.
                        idx_top_contact_first = idx_higher_or_equal(top_contacts, idx_top_contact_first,
                            [&intermediate_layer](const SupportGeneratorLayer *layer){ return layer->scaled_print_z() >= intermediate_layer.scaled_print_z(); }); //  - EPSILON
                        // Collect the top contact areas above this intermediate layer, below top_z.
                        for (int idx_top_contact = idx_top_contact_first; idx_top_contact < int(top_contacts.size()); ++ idx_top_contact) {
                            const SupportGeneratorLayer &top_contact_layer = *top_contacts[idx_top_contact];
                            //FIXME maybe this adds one interface layer in excess?
                            if (top_contact_layer.scaled_bottom_z() > top_z)
                                break;
                            polygons_append(top_contact_layer.scaled_bottom_z() > top_inteface_z ? polygons_top_contact_projected_base : polygons_top_contact_projected_interface, 
                                // For snug supports, project the overhang polygons covering the whole overhang, so that they will merge without a gap with support polygons of the other layers.
                                // For grid supports, merging of support regions will be performed by the projection into grid.
                                snug_supports ? *top_contact_layer.overhang_polygons : top_contact_layer.polygons);
                        }
                    }
                    if (support_params.num_bottom_interface_layers > 0) {
                        // Bottom Z coordinate of a slab, over which we are collecting the top / bottom contact surfaces
                        coord_t bottom_z =
                            intermediate_layers[std::max(0, int(idx_intermediate_layer - support_params.num_bottom_interface_layers) + 1)]
                                ->scaled_bottom_z();
                        coord_t bottom_interface_z = - std::numeric_limits<coordf_t>::max();
                        if (support_params.num_bottom_base_interface_layers > 0)
                            // Some bottom base interface layers will be generated.
                            bottom_interface_z = support_params.num_bottom_interface_layers_only() == 0 ? 
                                // Only base interface layers to generate.
                                std::numeric_limits<coordf_t>::max() :
                                intermediate_layers[std::max(0, int(idx_intermediate_layer - support_params.num_bottom_interface_layers_only()))]->scaled_bottom_z();
                        // Move idx_bottom_contact_first up until touching bottom_z.
                        idx_bottom_contact_first = idx_higher_or_equal(bottom_contacts, idx_bottom_contact_first,
                            [bottom_z](const SupportGeneratorLayer *layer){ return layer->scaled_print_z() >= bottom_z; });
                        // Collect the top contact areas above this intermediate layer, below top_z.
                        for (int idx_bottom_contact = idx_bottom_contact_first; idx_bottom_contact < int(bottom_contacts.size()); ++ idx_bottom_contact) {
                            const SupportGeneratorLayer &bottom_contact_layer = *bottom_contacts[idx_bottom_contact];
                            if (bottom_contact_layer.scaled_print_z() > intermediate_layer.scaled_bottom_z())
                                break;
                            polygons_append(bottom_contact_layer.scaled_print_z() > bottom_interface_z ?
                                polygons_bottom_contact_projected_interface : polygons_bottom_contact_projected_base, bottom_contact_layer.polygons);
                        }
                    }
                    auto resolve_same_layer = [](SupportGeneratorLayersPtr &layers, int &idx, coord_t print_z) -> SupportGeneratorLayer* {
                        if (! layers.empty()) {
                            idx = idx_higher_or_equal(layers, idx, [print_z](const SupportGeneratorLayer *layer) { return layer->scaled_print_z() >= print_z; });
                            if (idx < int(layers.size()) && layers[idx]->scaled_print_z() <= print_z)
                                return layers[idx];
                        }
                        return nullptr;
                    };
                    SupportGeneratorLayer *top_interface_layer      = resolve_same_layer(top_interface_layers, idx_top_interface_first, intermediate_layer.scaled_print_z());
                    SupportGeneratorLayer *top_base_interface_layer = resolve_same_layer(top_base_interface_layers, idx_top_base_interface_first, intermediate_layer.scaled_print_z());
                    SupportGeneratorLayer *interface_layer          = nullptr;
                    if (! polygons_bottom_contact_projected_interface.empty() || ! polygons_top_contact_projected_interface.empty() ||
                        (top_interface_layer && ! top_interface_layer->polygons.empty())) {
                        interface_layer = insert_layer(
                            intermediate_layer, polygons_bottom_contact_projected_interface, std::move(polygons_top_contact_projected_interface), top_interface_layer,
                            nullptr, polygons_top_contact_projected_interface.empty() ? SupporLayerType::BottomInterface : SupporLayerType::TopInterface);
                        interface_layers[idx_intermediate_layer] = interface_layer;
                        if(interface_layer != nullptr) assert_valid(interface_layers[idx_intermediate_layer]->polygons);
                    }
                    if (! polygons_bottom_contact_projected_base.empty() || ! polygons_top_contact_projected_base.empty() ||
                        (top_base_interface_layer && ! top_base_interface_layer->polygons.empty())){
                        base_interface_layers[idx_intermediate_layer] = insert_layer(
                            intermediate_layer, polygons_bottom_contact_projected_base, std::move(polygons_top_contact_projected_base), top_base_interface_layer,
                            interface_layer ? &interface_layer->polygons : nullptr, SupporLayerType::Base);
                        if(base_interface_layers[idx_intermediate_layer] != nullptr) assert_valid(base_interface_layers[idx_intermediate_layer]->polygons);
                    }
                }
            });

        // Compress contact_out, remove the nullptr items.
        // The parallel_for above may not have merged all the interface and base_interface layers
        // generated by the Organic supports code, do it here.
        auto merge_remove_empty = [](SupportGeneratorLayersPtr &in1, SupportGeneratorLayersPtr &in2) {
            auto remove_empty = [](SupportGeneratorLayersPtr &vec) {
                vec.erase(
                    std::remove_if(vec.begin(), vec.end(), [](const SupportGeneratorLayer *ptr) { return ptr == nullptr || ptr->polygons.empty(); }),
                    vec.end());
            };
            remove_empty(in1);
            remove_empty(in2);
            if (in2.empty())
                return std::move(in1);
            else if (in1.empty())
                return std::move(in2);
            else {
                SupportGeneratorLayersPtr out(in1.size() + in2.size(), nullptr);
                std::merge(in1.begin(), in1.end(), in2.begin(), in2.end(), out.begin(), [](auto* l, auto* r) { return l->scaled_print_z() < r->scaled_print_z(); });
                return out;
            }
        };
        for(auto & layer : interface_layers) if(layer) assert_valid(layer->polygons);
        for(auto & layer : base_interface_layers) if(layer) assert_valid(layer->polygons);
        interface_layers      = merge_remove_empty(interface_layers,      top_interface_layers);
        base_interface_layers = merge_remove_empty(base_interface_layers, top_base_interface_layers);
        for(auto & layer : interface_layers) if(layer) assert_valid(layer->polygons);
        for(auto & layer : base_interface_layers) if(layer) assert_valid(layer->polygons);
        BOOST_LOG_TRIVIAL(debug) << "PrintObjectSupportMaterial::generate_interface_layers() in parallel - end";
    }
    
    return base_and_interface_layers;
}

SupportGeneratorLayersPtr generate_raft_base(
    const PrintObject                 &object,
    const SupportParameters           &support_params,
	const SlicingParameters			  &slicing_params,
    const SupportGeneratorLayersPtr   &top_contacts,
    const SupportGeneratorLayersPtr   &interface_layers,
    const SupportGeneratorLayersPtr   &base_interface_layers,
    const SupportGeneratorLayersPtr   &base_layers,
    SupportGeneratorLayerStorage      &layer_storage)
{
    for (SupportGeneratorLayersPtr layer_ptr :
         {top_contacts, interface_layers, base_interface_layers, base_layers}) {
        for (auto layer : layer_ptr) {
            assert(layer);
            assert_valid(layer->polygons);
        }
    }
    // If there is brim to be generated, calculate the trimming regions.
    Polygons brim;
    if (object.has_brim()) {
        // The object does not have a raft.
        // Calculate the area covered by the brim.
        //const BrimType brim_type       = object.config().brim_type;
        const bool     brim_outer      = object.config().brim_width > 0; // brim_type == btOuterOnly || brim_type == btOuterAndInner;
        const bool     brim_inner      = object.config().brim_width_interior > 0; //brim_type == btInnerOnly || brim_type == btOuterAndInner;
        const auto     brim_separation = scale_d(object.config().brim_separation.value + object.config().brim_width.value);
        for (const ExPolygon &ex : object.layers().front().lslices()) {
            ex.assert_valid();
            if (brim_outer && brim_inner)
                polygons_append(brim, offset(ex, brim_separation));
            else {
                if (brim_outer)
                    polygons_append(brim, offset(ex.contour, brim_separation, ClipperLib::jtRound, float(scale_d(0.1))));
                else
                    brim.emplace_back(ex.contour);
                if (brim_inner) {
                    Polygons holes = ex.holes;
                    polygons_reverse(holes);
                    holes = shrink(holes, brim_separation, ClipperLib::jtRound, float(scale_d(0.1)));
                    polygons_reverse(holes);
                    polygons_append(brim, std::move(holes));
                } else
                    polygons_append(brim, ex.holes);
            }
        }
        brim = union_(brim);
    }
    assert_valid(brim);

    // How much to inflate the support columns to be stable. This also applies to the 1st layer, if no raft layers are to be printed.
    const float inflate_factor_fine      = float(scale_d((slicing_params.raft_layers() > 1) ? 0.5 : EPSILON));
    const float inflate_factor_1st_layer = std::max(0.f, float(scale_d(object.config().raft_first_layer_expansion)) - inflate_factor_fine);
    SupportGeneratorLayer       *contacts         = top_contacts         .empty() ? nullptr : top_contacts         .front();
    SupportGeneratorLayer       *interfaces       = interface_layers     .empty() ? nullptr : interface_layers     .front();
    SupportGeneratorLayer       *base_interfaces  = base_interface_layers.empty() ? nullptr : base_interface_layers.front();
    SupportGeneratorLayer       *columns_base     = base_layers          .empty() ? nullptr : base_layers          .front();
    if (contacts != nullptr &&
        contacts->scaled_print_z() > scale_to_layer_coord(std::max(slicing_params.first_print_layer_height,
                                                                          slicing_params.raft_contact_top_z)))
        // This is not the raft contact layer.
        contacts = nullptr;
    if (interfaces != nullptr &&
        interfaces->scaled_bottom_print_z() > scale_to_layer_coord(slicing_params.raft_interface_top_z))
        // This is not the raft column base layer.
        interfaces = nullptr;
    if (base_interfaces != nullptr &&
        base_interfaces->scaled_bottom_print_z() > scale_to_layer_coord(slicing_params.raft_interface_top_z))
        // This is not the raft column base layer.
        base_interfaces = nullptr;
    if (columns_base != nullptr &&
        columns_base->scaled_bottom_print_z() > scale_to_layer_coord(slicing_params.raft_interface_top_z))
        // This is not the raft interface layer.
        columns_base = nullptr;

    Polygons interface_polygons;
    if (contacts != nullptr && ! contacts->polygons.empty()) {
        assert_valid(contacts->polygons);
        polygons_append(interface_polygons, expand(contacts->polygons, inflate_factor_fine, SUPPORT_SURFACES_OFFSET_PARAMETERS));
    }
    if (interfaces != nullptr && ! interfaces->polygons.empty()) {
        assert_valid(interfaces->polygons);
        polygons_append(interface_polygons, expand(interfaces->polygons, inflate_factor_fine, SUPPORT_SURFACES_OFFSET_PARAMETERS));
    }
    if (base_interfaces != nullptr && ! base_interfaces->polygons.empty()) {
        assert_valid(base_interfaces->polygons);
        polygons_append(interface_polygons, expand(base_interfaces->polygons, inflate_factor_fine, SUPPORT_SURFACES_OFFSET_PARAMETERS));
    }
    ensure_valid(interface_polygons, support_params.resolution);
    assert_valid(interface_polygons);

    // Output vector.
    SupportGeneratorLayersPtr raft_layers;

    if (slicing_params.raft_layers() > 1) {
        Polygons base;
        Polygons columns;
        Polygons first_layer;
        if (columns_base != nullptr) {
            if (columns_base->scaled_bottom_print_z() >= scale_to_layer_coord(slicing_params.raft_interface_top_z)) {
                // Classic supports with colums above the raft interface.
                base = columns_base->polygons;
                columns = base;
                if (! interface_polygons.empty())
                    // Trim the 1st layer columns with the inflated interface polygons.
                    columns = diff(columns, interface_polygons);
            } else {
                // Organic supports with raft on print bed.
                assert(columns_base->scaled_print_z() == scale_to_layer_coord(slicing_params.first_print_layer_height));
                first_layer = columns_base->polygons;
            }
        }
        if (! interface_polygons.empty()) {
            // Merge the untrimmed columns base with the expanded raft interface, to be used for the support base and interface.
            base = union_(base, interface_polygons); 
        }
        // Do not add the raft contact layer, only add the raft layers below the contact layer.
        // Insert the 1st layer.
        {
            SupportGeneratorLayer &new_layer = layer_storage.allocate_unguarded(
                slicing_params.base_raft_layers > 0 ? SupporLayerType::RaftBase : SupporLayerType::RaftInterface);
            raft_layers.push_back(&new_layer);
            new_layer.set_scaled_print_z(scale_to_layer_coord(slicing_params.first_print_layer_height));
            new_layer.set_scaled_height(scale_to_layer_coord(slicing_params.first_print_layer_height));
            new_layer.set_scaled_bottom_z(0);
            new_layer.resolution = support_params.resolution;
            first_layer = union_(std::move(first_layer), base);
            new_layer.polygons = inflate_factor_1st_layer > 0 ? expand(first_layer, inflate_factor_1st_layer) :
                                                                first_layer;
        }
        // Insert the base layers.
        for (size_t i = 1; i < slicing_params.base_raft_layers; ++i) {
            coord_t print_z = raft_layers.back()->scaled_print_z();
            SupportGeneratorLayer &new_layer = layer_storage.allocate_unguarded(SupporLayerType::RaftBase);
            raft_layers.push_back(&new_layer);
            new_layer.set_scaled_print_z(print_z + scale_to_layer_coord(slicing_params.base_raft_layer_height));
            new_layer.set_scaled_height(scale_to_layer_coord(slicing_params.base_raft_layer_height));
            new_layer.set_scaled_bottom_z(print_z);
            new_layer.resolution = support_params.resolution;
            new_layer.polygons = base;
        }
        // Insert the interface layers.
        for (size_t i = 1; i < slicing_params.interface_raft_layers; ++ i) {
            coord_t print_z = raft_layers.back()->scaled_print_z();
            SupportGeneratorLayer &new_layer = layer_storage.allocate_unguarded(SupporLayerType::RaftInterface);
            raft_layers.push_back(&new_layer);
            new_layer.set_scaled_print_z(print_z + scale_to_layer_coord(slicing_params.interface_raft_layer_height));
            new_layer.set_scaled_height(scale_to_layer_coord(slicing_params.interface_raft_layer_height));
            new_layer.set_scaled_bottom_z(print_z);
            new_layer.resolution = support_params.resolution;
            new_layer.polygons = interface_polygons;
            //FIXME misusing contact_polygons for support columns.
            new_layer.contact_polygons = std::make_unique<Polygons>(columns);
        }
    } else {
        if (columns_base != nullptr) {
            // Expand the bases of the support columns in the 1st layer.
            Polygons &raft     = columns_base->polygons;
            Polygons  trimming = offset(object.layers().front().lslices(), (float)support_params._gap_xy, SUPPORT_SURFACES_OFFSET_PARAMETERS);
            if (inflate_factor_1st_layer > SCALED_EPSILON) {
                // Inflate in multiple steps to avoid leaking of the support 1st layer through object walls.
                auto  nsteps = std::max(5, int(ceil(inflate_factor_1st_layer / support_params.first_layer_flow.scaled_width())));
                float step   = inflate_factor_1st_layer / nsteps;
                for (int i = 0; i < nsteps; ++ i)
                    raft = diff(expand(raft, step), trimming);
            } else
                raft = diff(raft, trimming);
            if (! interface_polygons.empty())
                columns_base->polygons = ensure_valid(diff(columns_base->polygons, interface_polygons), support_params.resolution);
            ensure_valid(raft);
            ensure_valid(columns_base->polygons);
        }
        if (! brim.empty()) {
            assert_valid(brim);
            if (columns_base)
                columns_base->polygons = diff(columns_base->polygons, brim);
            if (contacts)
                contacts->polygons = diff(contacts->polygons, brim);
            if (contacts) assert_valid(contacts->polygons);
            if (interfaces)
                interfaces->polygons = diff(interfaces->polygons, brim);
            if (interfaces) assert_valid(interfaces->polygons);
            if (base_interfaces)
                base_interfaces->polygons = diff(base_interfaces->polygons, brim);
            if (base_interfaces) assert_valid(base_interfaces->polygons);
        }
    }
    
    for (auto layer : raft_layers) {
        assert(layer);
        assert_valid(layer->polygons);
    }
    return raft_layers;
}

static inline void fill_expolygon_generate_paths(
    ExtrusionEntitiesPtr    &dst,
    ExPolygon              &&expolygon,
    Fill                    *filler,
    const FillParams        &fill_params,
    float                    density,
    ExtrusionRole            role,
    const Flow              &flow,
    double                   spacing)
{
#ifdef _DEBUGINFO
    for(auto entity : dst) LoopAssertVisitor().traverse(*entity);
    expolygon.assert_valid();
#endif
    assert(!fill_params.use_arachne);
    FillParams new_params = fill_params;
    new_params.flow       = flow;
    new_params.role       = role;
    filler->init_spacing(spacing, new_params);
    {
        assert(!fill_params.use_arachne);
        Surface surface(stPosInternal | stDensSparse, std::move(expolygon));
        // TODO: catch exception here?
        filler->fill_surface_extrusion(&surface, new_params, dst);
    }
#ifdef _DEBUGINFO
    for(auto entity : dst) LoopAssertVisitor().traverse(*entity);
#endif
}

static inline void fill_expolygons_generate_paths(
    ExtrusionEntitiesPtr    &dst,
    ExPolygons             &&expolygons,
    Fill                    *filler,
    const FillParams        &fill_params,
    float                    density,
    ExtrusionRole            role,
    const Flow              &flow,
    double                   spacing)
{
    for (ExPolygon &expoly : expolygons) {
        fill_expolygon_generate_paths(dst, std::move(expoly), filler, fill_params, density, role, flow, spacing);
    }
}

static inline void fill_expolygons_generate_paths(
    ExtrusionEntitiesPtr    &dst,
    ExPolygons             &&expolygons,
    Fill                    *filler,
    float                    density,
    ExtrusionRole            role,
    const Flow              &flow,
    double                   spacing,
    const PrintRegionConfig& region_config)
{
    FillParams fill_params;
    fill_params.density     = density;
    fill_params.dont_adjust = true;
    fill_params.config      = &region_config;
    fill_expolygons_generate_paths(dst, std::move(expolygons), filler, fill_params, density, role, flow, spacing);
}

static inline void fill_expolygons_generate_paths(
    ExtrusionEntityCollection &dst,
    ExPolygons               &&expolygons,
    Fill                      *filler,
    float                      density,
    ExtrusionRole              role,
    const Flow                &flow,
    double                     spacing,
    const PrintRegionConfig   &region_config)
{
    ExtrusionEntitiesPtr entities;
    fill_expolygons_generate_paths(entities, std::move(expolygons), filler, density, role, flow, spacing, region_config);
    dst.append(std::move(entities));
}

static Polylines draw_perimeters(const ExPolygon &expoly, double clip_length)
{
    // Draw the perimeters.
    Polylines polylines;
    polylines.reserve(expoly.holes.size() + 1);
    for (size_t i = 0; i <= expoly.holes.size();  ++ i) {
        Polyline pl(i == 0 ? expoly.contour.points : expoly.holes[i - 1].points);
        pl.points.emplace_back(pl.points.front());
        if (i > 0)
            // It is a hole, reverse it.
            pl.reverse();
        // so that all contours are CCW oriented.
        pl.clip_end(clip_length);
#ifdef _DEBUG
        pl.assert_valid();
#endif
        polylines.emplace_back(std::move(pl));
    }
    return polylines;
}

static inline void tree_supports_generate_paths(
    ExtrusionEntityCollection &dst,
    const Polygons            &polygons,
    const Flow                &flow, 
    const SupportParameters   &support_params)
{
#ifdef _DEBUGINFO
    for (const auto extr : dst.entities()) LoopAssertVisitor().traverse(*extr);
    assert_valid(polygons);
#endif
    // Offset expolygon inside, returns number of expolygons collected (0 or 1).
    // Vertices of output paths are marked with Z = source contour index of the expoly.
    // Vertices at the intersection of source contours are marked with Z = -1.
    auto shrink_expolygon_with_contour_idx = [](const Slic3r::ExPolygon &expoly, const float delta, ClipperLib::JoinType joinType, double miterLimit, ClipperLib_Z::Paths &out) -> int
    {
        assert(delta > 0);
        auto append_paths_with_z = [](ClipperLib::Paths &src, coord_t contour_idx, ClipperLib_Z::Paths &dst) {
            dst.reserve(next_highest_power_of_2(dst.size() + src.size()));
            for (const ClipperLib::Path &contour : src) {
                ClipperLib_Z::Path tmp;
                tmp.reserve(contour.size());
                for (const Point &p : contour)
                    tmp.emplace_back(p.x(), p.y(), contour_idx);
                dst.emplace_back(std::move(tmp));
            }
        };

        // 1) Offset the outer contour.
        ClipperLib_Z::Paths contours;
        {
            ClipperLib::ClipperOffset co;
            if (joinType == jtRound)
                co.ArcTolerance = miterLimit;
            else
                co.MiterLimit = miterLimit;
            co.ShortestEdgeLength = double(delta * 0.005);
            co.AddPath(expoly.contour.points, joinType, ClipperLib::etClosedPolygon);
            ClipperLib::Paths contours_raw;
            co.Execute(contours_raw, - delta);
            if (contours_raw.empty())
                // No need to try to offset the holes.
                return 0;
            append_paths_with_z(contours_raw, 0, contours);
        }

        if (expoly.holes.empty()) {
            // No need to subtract holes from the offsetted expolygon, we are done.
            append(out, std::move(contours));
        } else {
            // 2) Offset the holes one by one, collect the offsetted holes.
            ClipperLib_Z::Paths holes;
            {
                for (const Polygon &hole : expoly.holes) {
                    ClipperLib::ClipperOffset co;
                    if (joinType == jtRound)
                        co.ArcTolerance = miterLimit;
                    else
                        co.MiterLimit = miterLimit;
                    co.ShortestEdgeLength = double(delta * 0.005);
                    co.AddPath(hole.points, joinType, ClipperLib::etClosedPolygon);
                    ClipperLib::Paths out2;
                    // Execute reorients the contours so that the outer most contour has a positive area. Thus the output
                    // contours will be CCW oriented even though the input paths are CW oriented.
                    // Offset is applied after contour reorientation, thus the signum of the offset value is reversed.
                    co.Execute(out2, delta);
                    append_paths_with_z(out2, 1 + (&hole - expoly.holes.data()), holes);
                }
            }

            // 3) Subtract holes from the contours.
            if (holes.empty()) {
                // No hole remaining after an offset. Just copy the outer contour.
                append(out, std::move(contours));
            } else {
                // Negative offset. There is a chance, that the offsetted hole intersects the outer contour. 
                // Subtract the offsetted holes from the offsetted contours.
                ClipperLib_Z::Clipper clipper;
                clipper.ZFillFunction([](const ClipperLib_Z::IntPoint &e1bot, const ClipperLib_Z::IntPoint &e1top, const ClipperLib_Z::IntPoint &e2bot, const ClipperLib_Z::IntPoint &e2top, ClipperLib_Z::IntPoint &pt) {
                        //pt.z() = std::max(std::max(e1bot.z(), e1top.z()), std::max(e2bot.z(), e2top.z()));
                        // Just mark the intersection.
                        pt.z() = -1;
                    });
                clipper.AddPaths(contours, ClipperLib_Z::ptSubject, true);
                clipper.AddPaths(holes,    ClipperLib_Z::ptClip,    true);
                ClipperLib_Z::Paths output;
                clipper.Execute(ClipperLib_Z::ctDifference, output, ClipperLib_Z::pftNonZero, ClipperLib_Z::pftNonZero);
                if (! output.empty()) {
                    append(out, std::move(output));
                } else {
                    // The offsetted holes have eaten up the offsetted outer contour.
                    return 0;
                }
            }
        }

        return 1;
    };

    const double spacing = flow.scaled_spacing();
    // Clip the sheath path to avoid the extruder to get exactly on the first point of the loop.
    const double clip_length = spacing * 0.15;
    const double anchor_length = spacing * 6.;
    ClipperLib_Z::Paths anchor_candidates;
    ExPolygons expolys = closing_ex(polygons, float(SCALED_EPSILON), float(SCALED_EPSILON + 0.5 * flow.scaled_width()));
    ensure_valid(expolys, support_params.resolution);
    for (ExPolygon& expoly : expolys) {
        std::unique_ptr<ExtrusionEntityCollection> eec;
        if (support_params.tree_branch_diameter_double_wall_area_scaled > 0)
            if (double area = expoly.area(); area > support_params.tree_branch_diameter_double_wall_area_scaled) {
                eec = std::make_unique<ExtrusionEntityCollection>();
                // Don't reoder internal / external loops of the same island, always start with the internal loop.
                eec->set_can_sort_reverse(false, false);
                // Make the tree branch stable by adding another perimeter.
                ExPolygons level2 = offset2_ex({ expoly }, -1.5 * flow.scaled_width(), 0.5 * flow.scaled_width());
                ensure_valid(level2, support_params.resolution);
                if (level2.size() == 1) {
                    extrusion_entities_append_paths(*eec, ensure_valid(draw_perimeters(expoly, clip_length), support_params.resolution), { ExtrusionRole::SupportMaterial, flow },
                        // Disable reversal of the path, always start with the anchor, always print CCW.
                        false);
                    expoly = level2.front();
                }
            }

        // Try to produce one more perimeter to place the seam anchor.
        // First genrate a 2nd perimeter loop as a source for anchor candidates.
        // The anchor candidate points are annotated with an index of the source contour or with -1 if on intersection.
        anchor_candidates.clear();
        shrink_expolygon_with_contour_idx(expoly, flow.scaled_width(), DefaultJoinType, 1.2, anchor_candidates);
        // Orient all contours CW.
        for (auto &path : anchor_candidates)
            if (ClipperLib_Z::Area(path) > 0)
                std::reverse(path.begin(), path.end());

        // Draw the perimeters.
        Polylines polylines;
        polylines.reserve(expoly.holes.size() + 1);
        for (int idx_loop = 0; idx_loop < int(expoly.num_contours()); ++ idx_loop) {
            // Open the loop with a seam.
            const Polygon &loop = expoly.contour_or_hole(idx_loop);
            loop.assert_valid();
            Polyline pl(loop.points);
            // Orient all contours CW, because the anchor will be added to the end of polyline while we want to start a loop with the anchor.
            if (idx_loop == 0)
                // It is an outer contour.
                pl.reverse();
            pl.points.emplace_back(pl.points.front());
            pl.clip_end(clip_length);
            if (pl.size() < 2)
                continue;
            // Find the foot of the seam point on anchor_candidates. Only pick an anchor point that was created by offsetting the source contour.
            ClipperLib_Z::Path *closest_contour = nullptr;
            Vec2d               closest_point;
            int                 closest_point_idx = -1;
            double              closest_point_t = 0.;
            double              d2min = std::numeric_limits<double>::max();
            Vec2d               seam_pt = pl.back().cast<double>();
            for (ClipperLib_Z::Path &path : anchor_candidates)
                for (int i = 0; i < int(path.size()); ++ i) {
                    int j = next_idx_modulo(i, path);
                    if (path[i].z() == idx_loop || path[j].z() == idx_loop) {
                        Vec2d pi(path[i].x(), path[i].y());
                        Vec2d pj(path[j].x(), path[j].y());
                        Vec2d v = pj - pi;
                        Vec2d w = seam_pt - pi;
                        auto   l2  = v.squaredNorm();
                        auto   t   = std::clamp((l2 == 0) ? 0 : v.dot(w) / l2, 0., 1.);
                        if ((path[i].z() == idx_loop || t > EPSILON) && (path[j].z() == idx_loop || t < 1. - EPSILON)) {
                            // Closest point.
                            Vec2d fp = pi + v * t;
                            double d2 = (fp - seam_pt).squaredNorm();
                            if (d2 < d2min) {
                                d2min = d2;
                                closest_contour   = &path;
                                closest_point     = fp;
                                closest_point_idx = i;
                                closest_point_t   = t;
                            }
                        }
                    }
                }
            if (d2min < sqr(flow.scaled_width() * 3.)) {
                // Try to cut an anchor from the closest_contour.
                // Both closest_contour and pl are CW oriented.
                pl.points.emplace_back(closest_point.cast<coord_t>());
                const ClipperLib_Z::Path &path = *closest_contour;
                double remaining_length = anchor_length - (seam_pt - closest_point).norm();
                int i = closest_point_idx;
                int j = next_idx_modulo(i, *closest_contour);
                Vec2d pi(path[i].x(), path[i].y());
                Vec2d pj(path[j].x(), path[j].y());
                Vec2d v = pj - pi;
                double l = v.norm();
                if (remaining_length < (1. - closest_point_t) * l) {
                    // Just trim the current line.
                    pl.points.emplace_back((closest_point + v * (remaining_length / l)).cast<coord_t>());
                } else {
                    // Take the rest of the current line, continue with the other lines.
                    Point pt_path_j(path[j].x(),path[j].y());
                    if (!pl.points.back().coincides_with_epsilon(pt_path_j)) {
                        pl.points.emplace_back(pt_path_j);
                    }
                    pi = pj;
                    for (i = j; path[i].z() == idx_loop && remaining_length > 0; i = j, pi = pj) {
                        j = next_idx_modulo(i, path);
                        pj = Vec2d(path[j].x(), path[j].y());
                        v = pj - pi;
                        l = v.norm();
                        if (i == closest_point_idx) {
                            // Back at the first segment. Most likely this should not happen and we may end the anchor.
                            break;
                        }
                        if (remaining_length <= l) {
                            pl.points.emplace_back((pi + v * (remaining_length / l)).cast<coord_t>());
                            break;
                        }
                        pl.points.emplace_back(path[j].x(), path[j].y());
                        remaining_length -= l;
                    }
                }
            }
            // Start with the anchor.
            pl.reverse();
            ensure_valid(pl, support_params.resolution);
            if (!pl.empty()) {
                polylines.emplace_back(std::move(pl));
            }
        }

        ExtrusionEntityCollection &out = eec ? *eec : dst;
#ifdef _DEBUGINFO
        LoopAssertVisitor().traverse(out);
        assert_valid(polylines);
#endif
        extrusion_entities_append_paths(out, std::move(polylines), { ExtrusionRole::SupportMaterial, flow },
            // Disable reversal of the path, always start with the anchor, always print CCW.
            false);
#ifdef _DEBUGINFO
        LoopAssertVisitor().traverse(out);
#endif
        if (eec) {
            std::reverse(eec->children().begin(), eec->children().end());
#ifdef _DEBUGINFO
            LoopAssertVisitor().traverse(*eec);
#endif
            dst.append(ExtrusionEntitiesPtr{eec.release()});
        }
    }
#ifdef _DEBUGINFO
    for (const auto extr : dst.entities()) LoopAssertVisitor().traverse(*extr);
#endif
}
// now use FillWithPerimeter
//static inline void fill_expolygons_with_sheath_generate_paths(
//    ExtrusionEntitiesPtr    &dst,
//    const Polygons          &polygons,
//    Fill                    *filler,
//    float                    density,
//    ExtrusionRole            role,
//    const Flow              &flow,
//    bool                     with_sheath,
//    bool                     no_sort)
//{
//    if (polygons.empty())
//        return;
//
//    if (! with_sheath) {
//        fill_expolygons_generate_paths(dst, closing_ex(polygons, float(SCALED_EPSILON)), filler, density, role, flow);
//        return;
//    }
//
//    FillParams fill_params;
//    fill_params.density     = density;
//    fill_params.dont_adjust = true;
//
//    const double spacing = flow.scaled_spacing();
//    // Clip the sheath path to avoid the extruder to get exactly on the first point of the loop.
//    const double clip_length = spacing * 0.15;
//
//    for (ExPolygon &expoly : closing_ex(polygons, float(SCALED_EPSILON), float(SCALED_EPSILON + 0.5*flow.scaled_width()))) {
//        // Don't reorder the skirt and its infills.
//        std::unique_ptr<ExtrusionEntityCollection> eec;
//        if (no_sort) {
//            eec = std::make_unique<ExtrusionEntityCollection>();
//            eec->no_sort = true;
//        }
//        ExtrusionEntitiesPtr &out = no_sort ? eec->entities : dst;
//        extrusion_entities_append_paths(out, draw_perimeters(expoly, clip_length), { ExtrusionRole::SupportMaterial, flow });
//        // Fill in the rest.
//        fill_expolygons_generate_paths(out, offset_ex(expoly, float(-0.4 * spacing)), filler, fill_params, density, role, flow);
//        if (no_sort && ! eec->empty())
//            dst.emplace_back(eec.release());
//    }
//}

// Support layers, partially processed.
struct SupportGeneratorLayerExtruded
{
    SupportGeneratorLayerExtruded& operator=(SupportGeneratorLayerExtruded &&rhs) {
        this->layer = rhs.layer;
        this->extrusions = std::move(rhs.extrusions);
        m_polygons_to_extrude = std::move(rhs.m_polygons_to_extrude);
        rhs.layer = nullptr;
        return *this;
    }

    bool empty() const {
        return layer == nullptr || layer->polygons.empty();
    }

    void set_polygons_to_extrude(Polygons &&polygons) { 
        if (m_polygons_to_extrude == nullptr) 
            m_polygons_to_extrude = std::make_unique<Polygons>(std::move(polygons));
        else
            *m_polygons_to_extrude = std::move(polygons);
    }
    Polygons& polygons_to_extrude() { return (m_polygons_to_extrude == nullptr) ? layer->polygons : *m_polygons_to_extrude; }
    const Polygons& polygons_to_extrude() const { return (m_polygons_to_extrude == nullptr) ? layer->polygons : *m_polygons_to_extrude; }

    bool could_merge(const SupportGeneratorLayerExtruded &other) const {
        return ! this->empty() && ! other.empty() && 
            this->layer->scaled_height() == other.layer->scaled_height() &&
            this->layer->bridging == other.layer->bridging; 
    }

    // Merge regions, perform boolean union over the merged polygons.
    void merge(SupportGeneratorLayerExtruded &&other, coord_t resolution) {
        assert(this->could_merge(other));
        assert_valid(other.polygons_to_extrude());
        // 1) Merge the rest polygons to extrude, if there are any.
        if (other.m_polygons_to_extrude != nullptr) {
            if (m_polygons_to_extrude == nullptr) {
                // This layer has no extrusions generated yet, if it has no m_polygons_to_extrude (its area to extrude was not reduced yet).
                assert(this->extrusions.empty());
                m_polygons_to_extrude = std::make_unique<Polygons>(this->layer->polygons);
            }
            Slic3r::polygons_append(*m_polygons_to_extrude, std::move(*other.m_polygons_to_extrude));
            *m_polygons_to_extrude = union_safety_offset(*m_polygons_to_extrude);
            other.m_polygons_to_extrude.reset();
        } else if (m_polygons_to_extrude != nullptr) {
            assert(other.m_polygons_to_extrude == nullptr);
            // The other layer has no extrusions generated yet, if it has no m_polygons_to_extrude (its area to extrude was not reduced yet).
            assert(other.extrusions.empty());
            Slic3r::polygons_append(*m_polygons_to_extrude, other.layer->polygons);
            *m_polygons_to_extrude = union_safety_offset(*m_polygons_to_extrude);
        }
        if (m_polygons_to_extrude) {
            ensure_valid(*m_polygons_to_extrude, resolution);
        }
        // 2) Merge the extrusions.
        this->extrusions.append_move_from(other.extrusions);
#ifdef _DEBUGINFO
        LoopAssertVisitor().traverse(this->extrusions);
#endif
        // 3) Merge the infill polygons.
        Slic3r::polygons_append(this->layer->polygons, std::move(other.layer->polygons));
        this->layer->polygons = union_safety_offset(this->layer->polygons);
        ensure_valid(this->layer->polygons, resolution);
        other.layer->polygons.clear();
    }

    void polygons_append(Polygons &dst) const {
        if (layer != NULL && ! layer->polygons.empty())
            Slic3r::polygons_append(dst, layer->polygons);
    }

    // The source layer. It carries the height and extrusion type (bridging / non bridging, extrusion height).
    SupportGeneratorLayer  *layer { nullptr };
    // Collect extrusions. They will be exported sorted by the bottom height.
    ExtrusionEntityCollection extrusions;

private:
    // In case the extrusions are non-empty, m_polygons_to_extrude may contain the rest areas yet to be filled by additional support.
    // This is useful mainly for the loop interfaces, which are generated before the zig-zag infills.
    std::unique_ptr<Polygons>             m_polygons_to_extrude;
};

typedef std::vector<SupportGeneratorLayerExtruded*> SupportGeneratorLayerExtrudedPtrs;

struct LoopInterfaceProcessor
{
    LoopInterfaceProcessor(coordf_t circle_r) :
        n_contact_loops(0),
        circle_radius(circle_r),
        circle_distance(circle_r * 3.)
    {
        // Shape of the top contact area.
        circle.points.reserve(6);
        for (size_t i = 0; i < 6; ++ i) {
            double angle = double(i) * M_PI / 3.;
            circle.points.push_back(Point(circle_radius * cos(angle), circle_radius * sin(angle)));
        }
    }

    // Generate loop contacts at the top_contact_layer,
    // trim the top_contact_layer->polygons with the areas covered by the loops.
    void generate(SupportGeneratorLayerExtruded &top_contact_layer, const Flow &interface_flow_src, const coord_t resolution) const;

    int         n_contact_loops;
    coordf_t    circle_radius;
    coordf_t    circle_distance;
    Polygon     circle;
};

void LoopInterfaceProcessor::generate(SupportGeneratorLayerExtruded &top_contact_layer, const Flow &interface_flow_src, const coord_t resolution) const
{
    if (n_contact_loops == 0 || top_contact_layer.empty())
        return;

    Flow flow = interface_flow_src.with_height((float)top_contact_layer.layer->unscaled_height());

    Polygons overhang_polygons;
    if (top_contact_layer.layer->overhang_polygons != nullptr)
        overhang_polygons = std::move(*top_contact_layer.layer->overhang_polygons);

    // Generate the outermost loop.
    // Find centerline of the external loop (or any other kind of extrusions should the loop be skipped)
    ExPolygons top_contact_expolygons = offset_ex(union_ex(top_contact_layer.layer->polygons), - 0.5f * flow.scaled_width());

    // Grid size and bit shifts for quick and exact to/from grid coordinates manipulation.
    coord_t circle_grid_resolution = 1;
    coord_t circle_grid_powerof2 = 0;
    {
        // epsilon to account for rounding errors
        coord_t circle_grid_resolution_non_powerof2 = coord_t(2. * circle_distance + 3.);
        while (circle_grid_resolution < circle_grid_resolution_non_powerof2) {
            circle_grid_resolution <<= 1;
            ++ circle_grid_powerof2;
        }
    }

    struct PointAccessor {
        const Point* operator()(const Point &pt) const { return &pt; }
    };
    typedef ClosestPointInRadiusLookup<Point, PointAccessor> ClosestPointLookupType;
    
    Polygons loops0;
    {
        // find centerline of the external loop of the contours
        // Only consider the loops facing the overhang.
        Polygons external_loops;
        // Holes in the external loops.
        Polygons circles;
        Polygons overhang_with_margin = offset(union_ex(overhang_polygons), 0.5f * flow.scaled_width());
        for (ExPolygons::iterator it_contact_expoly = top_contact_expolygons.begin(); it_contact_expoly != top_contact_expolygons.end(); ++ it_contact_expoly) {
            // Store the circle centers placed for an expolygon into a regular grid, hashed by the circle centers.
            ClosestPointLookupType circle_centers_lookup(coord_t(circle_distance - SCALED_EPSILON));
            Points circle_centers;
            Point  center_last;
            // For each contour of the expolygon, start with the outer contour, continue with the holes.
            for (size_t i_contour = 0; i_contour <= it_contact_expoly->holes.size(); ++ i_contour) {
                Polygon     &contour = (i_contour == 0) ? it_contact_expoly->contour : it_contact_expoly->holes[i_contour - 1];
                const Point *seg_current_pt = nullptr;
                coordf_t     seg_current_t  = 0.;
                if (! intersection_pl(contour.split_at_first_point(), overhang_with_margin).empty()) {
                    // The contour is below the overhang at least to some extent.
                    //FIXME ideally one would place the circles below the overhang only.
                    // Walk around the contour and place circles so their centers are not closer than circle_distance from each other.
                    if (circle_centers.empty()) {
                        // Place the first circle.
                        seg_current_pt = &contour.points.front();
                        seg_current_t  = 0.;
                        center_last    = *seg_current_pt;
                        circle_centers_lookup.insert(center_last);
                        circle_centers.push_back(center_last);
                    }
                    for (Points::const_iterator it = contour.points.begin() + 1; it != contour.points.end(); ++it) {
                        // Is it possible to place a circle on this segment? Is it not too close to any of the circles already placed on this contour?
                        const Point &p1 = *(it-1);
                        const Point &p2 = *it;
                        // Intersection of a ray (p1, p2) with a circle placed at center_last, with radius of circle_distance.
                        const Vec2d v_seg(coordf_t(p2(0)) - coordf_t(p1(0)), coordf_t(p2(1)) - coordf_t(p1(1)));
                        const Vec2d v_cntr(coordf_t(p1(0) - center_last(0)), coordf_t(p1(1) - center_last(1)));
                        coordf_t a = v_seg.squaredNorm();
                        coordf_t b = 2. * v_seg.dot(v_cntr);
                        coordf_t c = v_cntr.squaredNorm() - circle_distance * circle_distance;
                        coordf_t disc = b * b - 4. * a * c;
                        if (disc > 0.) {
                            // The circle intersects a ray. Avoid the parts of the segment inside the circle.
                            coordf_t t1 = (-b - sqrt(disc)) / (2. * a);
                            coordf_t t2 = (-b + sqrt(disc)) / (2. * a);
                            coordf_t t0 = (seg_current_pt == &p1) ? seg_current_t : 0.;
                            // Take the lowest t in <t0, 1.>, excluding <t1, t2>.
                            coordf_t t;
                            if (t0 <= t1)
                                t = t0;
                            else if (t2 <= 1.)
                                t = t2;
                            else {
                                // Try the following segment.
                                seg_current_pt = nullptr;
                                continue;
                            }
                            seg_current_pt = &p1;
                            seg_current_t  = t;
                            center_last    = Point(p1(0) + coord_t(v_seg(0) * t), p1(1) + coord_t(v_seg(1) * t));
                            // It has been verified that the new point is far enough from center_last.
                            // Ensure, that it is far enough from all the centers.
                            std::pair<const Point*, coordf_t> circle_closest = circle_centers_lookup.find(center_last);
                            if (circle_closest.first != nullptr) {
                                -- it;
                                continue;
                            }
                        } else {
                            // All of the segment is outside the circle. Take the first point.
                            seg_current_pt = &p1;
                            seg_current_t  = 0.;
                            center_last    = p1;
                        }
                        // Place the first circle.
                        circle_centers_lookup.insert(center_last);
                        circle_centers.push_back(center_last);
                    }
                    external_loops.push_back(std::move(contour));
                    for (const Point &center : circle_centers) {
                        circles.push_back(circle);
                        circles.back().translate(center);
                    }
                }
            }
        }
        // Apply a pattern to the external loops.
        loops0 = diff(external_loops, circles);
    }

    Polylines loop_lines;
    {
        // make more loops
        Polygons loop_polygons = loops0;
        for (int i = 1; i < n_contact_loops; ++ i)
            polygons_append(loop_polygons,
                opening(
                    loops0, 
                    i * flow.scaled_spacing() + 0.5f * flow.scaled_spacing(),
                    0.5f * flow.scaled_spacing()));
        // Clip such loops to the side oriented towards the object.
        // Collect split points, so they will be recognized after the clipping.
        // At the split points the clipped pieces will be stitched back together.
        loop_lines.reserve(loop_polygons.size());
        std::unordered_map<Point, int, PointHash> map_split_points;
        for (Polygons::const_iterator it = loop_polygons.begin(); it != loop_polygons.end(); ++ it) {
            assert(map_split_points.find(it->first_point()) == map_split_points.end());
            map_split_points[it->first_point()] = -1;
            loop_lines.push_back(it->split_at_first_point());
        }
        loop_lines = intersection_pl(loop_lines, expand(overhang_polygons, scale_d(SUPPORT_MATERIAL_MARGIN)));
        // Because a closed loop has been split to a line, loop_lines may contain continuous segments split to 2 pieces.
        // Try to connect them.
        for (int i_line = 0; i_line < int(loop_lines.size()); ++ i_line) {
            Polyline &polyline = loop_lines[i_line];
            auto it = map_split_points.find(polyline.first_point());
            if (it != map_split_points.end()) {
                // This is a stitching point.
                // If this assert triggers, multiple source polygons likely intersected at this point.
                assert(it->second != -2);
                if (it->second < 0) {
                    // First occurence.
                    it->second = i_line;
                } else {
                    // Second occurence. Join the lines.
                    Polyline &polyline_1st = loop_lines[it->second];
                    assert(polyline_1st.first_point() == it->first || polyline_1st.last_point() == it->first);
                    if (polyline_1st.first_point() == it->first)
                        polyline_1st.reverse();
                    polyline_1st.append(std::move(polyline));
                    it->second = -2;
                }
                continue;
            }
            it = map_split_points.find(polyline.last_point());
            if (it != map_split_points.end()) {
                // This is a stitching point.
                // If this assert triggers, multiple source polygons likely intersected at this point.
                assert(it->second != -2);
                if (it->second < 0) {
                    // First occurence.
                    it->second = i_line;
                } else {
                    // Second occurence. Join the lines.
                    Polyline &polyline_1st = loop_lines[it->second];
                    assert(polyline_1st.first_point() == it->first || polyline_1st.last_point() == it->first);
                    if (polyline_1st.first_point() == it->first)
                        polyline_1st.reverse();
                    polyline.reverse();
                    polyline_1st.append(std::move(polyline));
                    it->second = -2;
                }
            }
        }
        // Remove empty lines.
        remove_degenerate(loop_lines);
    }
    
    // add the contact infill area to the interface area
    // note that growing loops by $circle_radius ensures no tiny
    // extrusions are left inside the circles; however it creates
    // a very large gap between loops and contact_infill_polygons, so maybe another
    // solution should be found to achieve both goals
    // Store the trimmed polygons into a separate polygon set, so the original infill area remains intact for
    // "modulate by layer thickness".
    top_contact_layer.set_polygons_to_extrude(
        ensure_valid(
            diff(top_contact_layer.layer->polygons, offset(loop_lines, double(circle_radius * 1.1)))
            , resolution));

    ensure_valid(loop_lines, resolution);

    // Transform loops into ExtrusionPath objects.
    extrusion_entities_append_paths(
        top_contact_layer.extrusions,
        std::move(loop_lines),
        { ExtrusionRole::SupportMaterialInterface, flow });
#ifdef _DEBUGINFO
        LoopAssertVisitor().traverse(top_contact_layer.extrusions);
#endif
}

#ifdef SLIC3R_DEBUG
static std::string dbg_index_to_color(int idx)
{
    if (idx < 0)
        return "yellow";
    idx = idx % 3;
    switch (idx) {
        case 0: return "red";
        case 1: return "green";
        default: return "blue";
    }
}
#endif /* SLIC3R_DEBUG */

class GetFirstPath : public ExtrusionVisitorConst
{
public:
    const ExtrusionPath *extrusion_path_template = nullptr;
    virtual void default_use(const ExtrusionEntity &entity) override
    {
        if (const ExtrusionPath *path = dynamic_cast<const ExtrusionPath*>(&entity)) {
            extrusion_path_template = path;
            return;
        }
        if (!entity.is_leaf())
            for (const ExtrusionEntityUPtr &child : entity.children()) {
                if (extrusion_path_template != nullptr)
                    return;
                if (child)
                    child->visit(*this);
            }
    }
};

// When extruding a bottom interface layer over an object, the bottom interface layer is extruded in a thin air, therefore
// it is being extruded with a bridging flow to not shrink excessively (the die swell effect).
// Tiny extrusions are better avoided and it is always better to anchor the thread to an existing support structure if possible.
// Therefore the bottom interface spots are expanded a bit. The expanded regions may overlap with another bottom interface layers,
// leading to over extrusion, where they overlap. The over extrusion is better avoided as it often makes the interface layers
// to stick too firmly to the object.
//
// Modulate thickness (increase bottom_z) of extrusions_in_out generated for this_layer
// if they overlap with overlapping_layers, whose print_z is above this_layer.bottom_z() and below this_layer.print_z.
static void modulate_extrusion_by_overlapping_layers(
    // Extrusions generated for this_layer.
    ExtrusionEntityCollection            &extrusions_in_out,
    const SupportGeneratorLayer          &this_layer,
    // Multiple layers overlapping with this_layer, sorted bottom up.
    const SupportGeneratorLayersPtr      &overlapping_layers)
{
    size_t n_overlapping_layers = overlapping_layers.size();
    if (n_overlapping_layers == 0 || extrusions_in_out.empty())
        // The extrusions do not overlap with any other extrusion.
        return;

    // TODO: should preserve the unsortable things
    ExtrusionEntityCollection flatten_extrusions_in_out = extrusions_in_out.flatten(false);

    // Get the initial extrusion parameters.
    GetFirstPath getFirstPathVisitor;
    flatten_extrusions_in_out.entities().front()->visit(getFirstPathVisitor);
    const ExtrusionPath *extrusion_path_template = getFirstPathVisitor.extrusion_path_template;
    assert(extrusion_path_template != nullptr);
    ExtrusionRole extrusion_role  = extrusion_path_template->role();
    float         extrusion_width = extrusion_path_template->width();

    struct ExtrusionPathFragment
    {
        ExtrusionFlow   flow;
        Polylines       polylines;
    };

    // Split the extrusions by the overlapping layers, reduce their extrusion rate.
    // The last path_fragment is from this_layer.
    std::vector<ExtrusionPathFragment> path_fragments(
        n_overlapping_layers + 1, 
        ExtrusionPathFragment{ extrusion_path_template->attributes() });
    // Don't use it, it will be released.
    extrusion_path_template = nullptr;
    for(auto & frag : path_fragments) assert(frag.polylines.empty());

#ifdef SLIC3R_DEBUG
    static int iRun = 0;
    ++ iRun;
    BoundingBox bbox;
    for (size_t i_overlapping_layer = 0; i_overlapping_layer < n_overlapping_layers; ++ i_overlapping_layer) {
        const SupportGeneratorLayer &overlapping_layer = *overlapping_layers[i_overlapping_layer];
        bbox.merge(get_extents(overlapping_layer.polygons));
    }
    for (ExtrusionEntitiesPtr::const_iterator it = extrusions_in_out.entities().begin(); it != extrusions_in_out.entities().end(); ++ it) {
        ExtrusionPath *path = dynamic_cast<ExtrusionPath*>(*it);
        assert(path != nullptr);
        bbox.merge(get_extents(path->polyline().as_polyline()));
    }
    SVG svg(debug_out_path("support-fragments-%d-%lf.svg", iRun, this_layer.unscaled_print_z()).c_str(), bbox);
    const float transparency = 0.5f;
    // Filled polygons for the overlapping regions.
    svg.draw(union_ex(this_layer.polygons), dbg_index_to_color(-1), transparency);
    for (size_t i_overlapping_layer = 0; i_overlapping_layer < n_overlapping_layers; ++ i_overlapping_layer) {
        const SupportGeneratorLayer &overlapping_layer = *overlapping_layers[i_overlapping_layer];
        svg.draw(union_ex(overlapping_layer.polygons), dbg_index_to_color(int(i_overlapping_layer)), transparency);
    }
    // Contours of the overlapping regions.
    svg.draw(to_polylines(this_layer.polygons), dbg_index_to_color(-1), scale_(0.2));
    for (size_t i_overlapping_layer = 0; i_overlapping_layer < n_overlapping_layers; ++ i_overlapping_layer) {
        const SupportGeneratorLayer &overlapping_layer = *overlapping_layers[i_overlapping_layer];
        svg.draw(to_polylines(overlapping_layer.polygons), dbg_index_to_color(int(i_overlapping_layer)), scale_(0.1));
    }
    // Fill extrusion, the source.
    for (ExtrusionEntitiesPtr::const_iterator it = extrusions_in_out.entities().begin(); it != extrusions_in_out.entities().end(); ++ it) {
        ExtrusionPath *path = dynamic_cast<ExtrusionPath*>(*it);
        std::string color_name;
        switch ((it - extrusions_in_out.entities().begin()) % 9) {
            case 0: color_name = "magenta"; break;
            case 1: color_name = "deepskyblue"; break;
            case 2: color_name = "coral"; break;
            case 3: color_name = "goldenrod"; break;
            case 4: color_name = "orange"; break;
            case 5: color_name = "olivedrab"; break;
            case 6: color_name = "blueviolet"; break;
            case 7: color_name = "brown"; break;
            default: color_name = "orchid"; break;
        }
        svg.draw(path->polyline().as_polyline(), color_name, scale_(0.2));
    }
#endif /* SLIC3R_DEBUG */

    // End points of the original paths.
    std::vector<std::pair<Point, Point>> path_ends; 
    // Collect the paths of this_layer.
    {
        Polylines &polylines = path_fragments.back().polylines;
        for (const ExtrusionEntity *ee : flatten_extrusions_in_out.entities()) {
            for (ArcPolyline &polyline : ee->as_polylines()) {
                polylines.emplace_back(polyline.to_polyline());
            }
            path_ends.emplace_back(std::pair<Point, Point>(polylines.back().points.front(), polylines.back().points.back()));
        }
        assert_valid(polylines);
    }
    // Destroy the original extrusion paths, their polylines were moved to path_fragments already.
    // This will be the destination for the new paths.
    extrusions_in_out.clear();

    // Fragment the path segments by overlapping layers. The overlapping layers are sorted by an increasing print_z.
    // Trim by the highest overlapping layer first.
    for (int i_overlapping_layer = int(n_overlapping_layers) - 1; i_overlapping_layer >= 0; -- i_overlapping_layer) {
        const SupportGeneratorLayer &overlapping_layer = *overlapping_layers[i_overlapping_layer];
        ExtrusionPathFragment &frag = path_fragments[i_overlapping_layer];
        Polygons polygons_trimming      = offset(union_ex(overlapping_layer.polygons), scale_d(0.5 * extrusion_width));
        ensure_valid(polygons_trimming, this_layer.resolution);
        assert_valid(polygons_trimming);
        frag.polylines = intersection_pl(path_fragments.back().polylines, polygons_trimming);
        ensure_valid(frag.polylines, this_layer.resolution);
        assert_valid(frag.polylines);
        path_fragments.back().polylines = diff_pl(path_fragments.back().polylines, polygons_trimming);
        assert_valid(path_fragments.back().polylines);
        // Adjust the extrusion parameters for a reduced layer height and a non-bridging flow (nozzle_dmr = -1, does not matter).
        assert(this_layer.scaled_print_z() > overlapping_layer.scaled_print_z());
        float old_height = frag.flow.height;
        frag.flow.height = float(unscaled(this_layer.scaled_print_z() - overlapping_layer.scaled_print_z()));
        // don't recompute the flow, just use a simple % reduction/increase, as it can wreak havoc as the flow is compute internally by the
        // fill algo. We can't recompute it!
        frag.flow.mm3_per_mm = frag.flow.mm3_per_mm * frag.flow.height / old_height; //Flow(frag.flow.width, frag.flow.height, -1.f).mm3_per_mm();
#ifdef SLIC3R_DEBUG
        svg.draw(frag.polylines, dbg_index_to_color(i_overlapping_layer), scale_(0.1));
#endif /* SLIC3R_DEBUG */
    }

#ifdef SLIC3R_DEBUG
    svg.draw(path_fragments.back().polylines, dbg_index_to_color(-1), scale_(0.1));
    svg.Close();
#endif /* SLIC3R_DEBUG */

    // Now chain the split segments using hashing and a nearly exact match, maintaining the order of segments.
    // Create a single ExtrusionPath or ExtrusionEntityCollection per source ExtrusionPath.
    // Map of fragment start/end points to a pair of <i_overlapping_layer, i_polyline_in_layer>
    // Because a non-exact matching is used for the end points, a multi-map is used.
    // As the clipper library may reverse the order of some clipped paths, store both ends into the map.
    struct ExtrusionPathFragmentEnd
    {
        ExtrusionPathFragmentEnd(size_t alayer_idx, size_t apolyline_idx, bool ais_start) :
            layer_idx(alayer_idx), polyline_idx(apolyline_idx), is_start(ais_start) {}
        size_t layer_idx;
        size_t polyline_idx;
        bool   is_start;
    };
    class ExtrusionPathFragmentEndPointAccessor {
    public:
        ExtrusionPathFragmentEndPointAccessor(const std::vector<ExtrusionPathFragment> &path_fragments) : m_path_fragments(path_fragments) {}
        // Return an end point of a fragment, or nullptr if the fragment has been consumed already.
        const Point* operator()(const ExtrusionPathFragmentEnd &fragment_end) const {
            const Polyline &polyline = m_path_fragments[fragment_end.layer_idx].polylines[fragment_end.polyline_idx];
            return polyline.points.empty() ? nullptr :
                (fragment_end.is_start ? &polyline.points.front() : &polyline.points.back());
        }
    private:
        ExtrusionPathFragmentEndPointAccessor& operator=(const ExtrusionPathFragmentEndPointAccessor&) {
            return *this;
        }

        const std::vector<ExtrusionPathFragment> &m_path_fragments;
    };
    const coord_t search_radius = 7;
    ClosestPointInRadiusLookup<ExtrusionPathFragmentEnd, ExtrusionPathFragmentEndPointAccessor> map_fragment_starts(
        search_radius, ExtrusionPathFragmentEndPointAccessor(path_fragments));
    for (size_t i_overlapping_layer = 0; i_overlapping_layer <= n_overlapping_layers; ++ i_overlapping_layer) {
        const Polylines &polylines = path_fragments[i_overlapping_layer].polylines;
        for (size_t i_polyline = 0; i_polyline < polylines.size(); ++ i_polyline) {
            polylines[i_polyline].assert_valid();
            // Map a starting point of a polyline to a pair of <layer, polyline>
            if (polylines[i_polyline].points.size() >= 2) {
                map_fragment_starts.insert(ExtrusionPathFragmentEnd(i_overlapping_layer, i_polyline, true));
                map_fragment_starts.insert(ExtrusionPathFragmentEnd(i_overlapping_layer, i_polyline, false));
            }
        }
    }

    // For each source path:
    for (size_t i_path = 0; i_path < path_ends.size(); ++ i_path) {
        const Point &pt_start = path_ends[i_path].first;
        const Point &pt_end   = path_ends[i_path].second;
        Point pt_current = pt_start;
        // Find a chain of fragments with the original / reduced print height.
        ExtrusionMultiPath multipath;
        for (;;) {
            // Find a closest end point to pt_current.
            std::pair<const ExtrusionPathFragmentEnd*, coordf_t> end_and_dist2 = map_fragment_starts.find(pt_current);
            // There may be a bug in Clipper flipping the order of two last points in a fragment?
            // assert(end_and_dist2.first != nullptr);
            assert(end_and_dist2.first == nullptr || end_and_dist2.second < search_radius * search_radius);
            if (end_and_dist2.first == nullptr) {
                // New fragment connecting to pt_current was not found.
                // Verify that the last point found is close to the original end point of the unfragmented path.
                //const double d2 = (pt_end - pt_current).cast<double>.squaredNorm();
                //assert(d2 < coordf_t(search_radius * search_radius));
                // End of the path.
                break;
            }
            const ExtrusionPathFragmentEnd &fragment_end_min = *end_and_dist2.first;
            // Fragment to consume.
            ExtrusionPathFragment &frag = path_fragments[fragment_end_min.layer_idx];
            Polyline              &frag_polyline = frag.polylines[fragment_end_min.polyline_idx];
            frag_polyline.assert_valid();
            // Path to append the fragment to.
            ExtrusionPath         *path = multipath.paths().empty() ? nullptr : &multipath.paths().back();
            if (path != nullptr) {
                // Verify whether the path is compatible with the current fragment.
                assert(this_layer.layer_type == SupporLayerType::BottomContact || path->height() != frag.flow.height || path->mm3_per_mm() != frag.flow.mm3_per_mm);
                if (path->height() != frag.flow.height || path->mm3_per_mm() != frag.flow.mm3_per_mm)
                    path = nullptr;
                // Merging with the previous path. This can only happen if the current layer was reduced by a base layer, which was split into a base and interface layer.
            }
            if (path == nullptr) {
                // Allocate a new path.
                multipath.paths().emplace_back(ExtrusionAttributes{extrusion_role, frag.flow}, nullptr, multipath.can_reverse());
                path = &multipath.paths().back();
            }
            // The Clipper library may flip the order of the clipped polylines arbitrarily.
            // Reverse the source polyline, if connecting to the end.
            if (! fragment_end_min.is_start)
                frag_polyline.reverse();
            // Enforce exact overlap of the end points of successive fragments.
            //assert(frag_polyline.front() == pt_current); // find an exemple where there's a 1 unit diff in y. worth figthing for?
            assert(frag_polyline.front().coincides_with_epsilon(pt_current));
            frag_polyline.points.front() = pt_current;
            // Don't repeat the first point.
            if (! path->polyline().empty())
                path->polyline().pop_back();
            // Consume the fragment's polyline, remove it from the input fragments, so it will be ignored the next time.
            frag_polyline.assert_valid();
            path->polyline().append(std::move(frag_polyline));
            assert(path->polyline().is_valid());
            frag_polyline.points.clear();
            pt_current = path->polyline().back();
            if (pt_current == pt_end) {
                // End of the path.
                break;
            }
        }
        if (!multipath.paths().empty()) {
            if (multipath.paths().size() == 1) {
                // This path was not fragmented.
                extrusions_in_out.append(ExtrusionEntitiesPtr{ new ExtrusionPath(std::move(multipath.paths().front())) });
            } else {
                // This path was fragmented. Copy the collection as a whole object, so the order inside the collection will not be changed
                // during the chaining of extrusions_in_out.
                extrusions_in_out.append(ExtrusionEntitiesPtr{ new ExtrusionMultiPath(std::move(multipath)) });
            }
        }
    }
    // If there are any non-consumed fragments, add them separately.
    //FIXME this shall not happen, if the Clipper works as expected and all paths split to fragments could be re-connected.
    for (ExtrusionPathFragment &fragment : path_fragments) {
        extrusion_entities_append_paths(extrusions_in_out, std::move(fragment.polylines),
                                        {extrusion_role, fragment.flow});
    }
}

// Support layer that is covered by some form of dense interface.
static constexpr const std::initializer_list<SupporLayerType> support_types_interface{
    SupporLayerType::RaftInterface, SupporLayerType::BottomContact, SupporLayerType::BottomInterface, SupporLayerType::TopContact, SupporLayerType::TopInterface
};

SupportGeneratorLayersPtr generate_support_layers(
    PrintObject                         &object,
    const SupportGeneratorLayersPtr     &raft_layers,
    const SupportGeneratorLayersPtr     &bottom_contacts,
    const SupportGeneratorLayersPtr     &top_contacts,
    const SupportGeneratorLayersPtr     &intermediate_layers,
    const SupportGeneratorLayersPtr     &interface_layers,
    const SupportGeneratorLayersPtr     &base_interface_layers)
{
    for (SupportGeneratorLayersPtr layer_ptr :
         {raft_layers, top_contacts, bottom_contacts, intermediate_layers, interface_layers, base_interface_layers}) {
        for (auto layer : layer_ptr) {
            assert(layer);
            assert_valid(layer->polygons);
        }
    }
    // Install support layers into the object.
    // A support layer installed on a PrintObject has a unique print_z.
    SupportGeneratorLayersPtr layers_sorted;
    layers_sorted.reserve(raft_layers.size() + bottom_contacts.size() + top_contacts.size() + intermediate_layers.size() + interface_layers.size() + base_interface_layers.size());
    append(layers_sorted, raft_layers);
    append(layers_sorted, bottom_contacts);
    append(layers_sorted, top_contacts);
    append(layers_sorted, intermediate_layers);
    append(layers_sorted, interface_layers);
    append(layers_sorted, base_interface_layers);
    // Sort the layers lexicographically by a raising print_z and a decreasing height.
    std::sort(layers_sorted.begin(), layers_sorted.end(), [](auto *l1, auto *l2) { return *l1 < *l2; });
    int layer_id = 0;
    int layer_id_interface = 0;
    assert(object.auxiliary_layers().empty());
    for (size_t i = 0; i < layers_sorted.size();) {
        // Find the last layer with roughly the same print_z, find the minimum layer height of all.
        size_t j = i + 1;
        const coord_t layer_z = layers_sorted[i]->scaled_print_z();
        for (; j < layers_sorted.size() && layers_sorted[j]->scaled_print_z() <= layer_z; ++j) {
            assert(layers_sorted[j]->scaled_print_z() == layer_z);
        }
        // Assign an average print_z to the set of layers with nearly equal print_z.
        coord_t min_layer_height = layers_sorted[i]->scaled_height();
        bool     empty = true;
        // For snug supports, layers where the direction of the support interface shall change are accounted for.
        size_t   num_interfaces = 0;
        size_t   num_top_contacts = 0;
        coord_t   top_contact_bottom_z = 0;
        for (size_t u = i; u < j; ++u) {
            SupportGeneratorLayer &layer = *layers_sorted[u];
            if (! layer.polygons.empty()) {
                empty             = false;
                num_interfaces   += one_of(layer.layer_type, support_types_interface);
                if (layer.layer_type == SupporLayerType::TopContact) {
                    ++ num_top_contacts;
                    assert(num_top_contacts <= 1);
                    // All top contact layers sharing this print_z shall also share bottom_z.
                    //assert(num_top_contacts == 1 || (top_contact_bottom_z - layer.bottom_z) < EPSILON);
                    top_contact_bottom_z = layer.scaled_bottom_z();
                }
            }
            layer.set_scaled_print_z(layer_z);
            assert(layer_z == layer.scaled_print_z());
            min_layer_height = std::min(min_layer_height, layer.scaled_height());
        }
        if (! empty) {
            // Here the upper_layer and lower_layer pointers are left to null at the support layers, 
            // as they are never used. These pointers are candidates for removal.
            bool   this_layer_contacts_only = num_top_contacts > 0 && num_top_contacts == num_interfaces;
            size_t this_layer_id_interface  = layer_id_interface;
            if (this_layer_contacts_only) {
                // Find a supporting layer for its interface ID.
                for (auto it = object.auxiliary_layers().rbegin(); it != object.auxiliary_layers().rend(); ++ it)
                    if (const Layer &other_layer = *it; other_layer.scaled_print_z() == top_contact_bottom_z) {
                        // other_layer supports this top contact layer. Assign a different support interface direction to this layer
                        // from the layer that supports it.
                        const LayerSupportProperty *support_property = other_layer.get_property<LayerSupportProperty>();
                        if (support_property != nullptr)
                            this_layer_id_interface = support_property->interface_id + 1;
                    }
            }
            Layer &support_layer = object.add_auxiliary_layer(layer_id ++, min_layer_height, layer_z);
            LayerSupportProperty &support_property = support_layer.get_or_add_property<LayerSupportProperty>();
            support_property.interface_id = static_cast<uint32_t>(this_layer_id_interface);
            support_property.reserved = 0;
            if (num_interfaces && ! this_layer_contacts_only)
                ++ layer_id_interface;
        }
        i = j;
    }
    return layers_sorted;
}

#ifndef NDEBUG
class verify_nonempty : public ExtrusionVisitorRecursiveConst
{
public:
    virtual void default_use(const ExtrusionEntity &entity) override
    {
        ExtrusionVisitorRecursiveConst::default_use(entity);
        assert(!entity.empty());
    }
} verifier;
#endif

void generate_support_toolpaths(
    PrintObject                         &object,
    LayerUPtrs                   &support_layers,
    const PrintObjectConfig             &config,
    const SupportParameters             &support_params,
    const SlicingParameters             &slicing_params,
    const SupportGeneratorLayersPtr     &raft_layers,
    const SupportGeneratorLayersPtr     &bottom_contacts,
    const SupportGeneratorLayersPtr     &top_contacts,
    const SupportGeneratorLayersPtr     &intermediate_layers,
    const SupportGeneratorLayersPtr     &interface_layers,
    const SupportGeneratorLayersPtr     &base_interface_layers)
{
    for (SupportGeneratorLayersPtr layer_ptr :
         {raft_layers, top_contacts, bottom_contacts, intermediate_layers, interface_layers, base_interface_layers}) {
        for (auto layer : layer_ptr) {
            assert(layer);
            assert_valid(layer->polygons);
        }
    }

    const int16_t support_extruder = int16_t(config.support_material_extruder.value - 1);
    const int16_t support_interface_extruder = int16_t(config.support_material_interface_extruder.value - 1);

    // loop_interface_processor with a given circle radius.
    LoopInterfaceProcessor loop_interface_processor(1.5 * support_params.support_material_interface_flow.scaled_width());
    loop_interface_processor.n_contact_loops = config.support_material_interface_contact_loops ? 1 : 0;

    BoundingBox bbox_object(Point(-scale_i(1.), -scale_i(1.0)), Point(scale_i(1.), scale_i(1.)));

//    const coordf_t link_max_length_factor = 3.;
    const coordf_t link_max_length_factor = 0.;
    // layer -> support & support_interface
    std::map<const Layer*, std::pair<ExtrusionEntityCollection, ExtrusionEntityCollection>> raft_cache;

    // Insert the raft base layers.
    auto n_raft_layers = std::min<size_t>(support_layers.size(), std::max(0, int(slicing_params.raft_layers()) - 1));
    std::vector<BoundingBox> raft_layers_bb(n_raft_layers);

    tbb::parallel_for(tbb::blocked_range<size_t>(0, n_raft_layers),
        [&support_layers, &raft_layers, &intermediate_layers, &config, &support_params, &slicing_params,
            &bbox_object, &raft_cache, &raft_layers_bb, link_max_length_factor]
            (const tbb::blocked_range<size_t>& range) {
        for (size_t support_layer_id = range.begin(); support_layer_id < range.end(); ++ support_layer_id)
        {
            assert(support_layer_id < raft_layers.size());
            Layer               &support_layer = *support_layers[support_layer_id];
            assert(!support_layer.has_extrusions());
            SupportGeneratorLayer      &raft_layer    = *raft_layers[support_layer_id];

            std::unique_ptr<Fill> filler_interface = std::unique_ptr<Fill>(Fill::new_from_type(support_params.raft_interface_fill_pattern)); // m_support_params.contact_top_fill_pattern)); FIXME choose
            std::unique_ptr<Fill> filler_support   = std::unique_ptr<Fill>(Fill::new_from_type(support_params.base_fill_pattern));
            std::unique_ptr<FillWithPerimeter> filler_support_with_sheath = std::make_unique<FillWithPerimeter>(
                (Fill::new_from_type(support_params.base_fill_pattern)));
            filler_support_with_sheath->overlap = support_params._gap_xy == 0 ? 0 : 1; // don't allow perimater overlap to touch perimeters
            filler_support_with_sheath->ratio_fill_inside   = 0.2f;
            std::unique_ptr<FillWithPerimeter> filler_dense = std::make_unique<FillWithPerimeter>((Fill::new_from_type(ipRectilinear)));
            filler_dense->overlap = support_params._gap_xy == 0 ? 0 : 1;
            filler_dense->ratio_fill_inside                 = 0.2f;
            filler_interface->set_bounding_box(bbox_object);
            filler_support->set_bounding_box(bbox_object);

            // Print the tree supports cutting through the raft with the exception of the 1st layer, where a full support layer will be printed below
            // both the raft and the trees.
            // Trim the raft layers with the tree polygons.
            const Polygons &tree_polygons =
                (support_layer_id > 0 && support_layer_id < intermediate_layers.size() &&
                    intermediate_layers[support_layer_id]->scaled_print_z() == support_layer.scaled_print_z()) ?
                intermediate_layers[support_layer_id]->polygons : Polygons();

            // Print the support base below the support columns, or the support base for the support columns plus the contacts.
            if (support_layer_id > 0) {
                const Polygons &to_infill_polygons = (support_layer_id < slicing_params.base_raft_layers) ? 
                    raft_layer.polygons :
                    //FIXME misusing contact_polygons for support columns.
                    ((raft_layer.contact_polygons == nullptr) ? Polygons() : *raft_layer.contact_polygons);
                // Trees may cut through the raft layers down to a print bed.
                //Flow flow(float(support_params.support_material_flow.width()), float(raft_layer.height), support_params.support_material_flow.nozzle_diameter());
                Flow  flow   = Flow::new_from_width(float(support_params.raft_flow.width()), support_params.raft_flow.nozzle_diameter(),
                                                 float(raft_layer.unscaled_height()), support_params.raft_flow.spacing_ratio());
                assert(!raft_layer.bridging);
                assert(support_layer.islands().empty());
                //assert(!support_layer.islands().front()->regions_islands().empty());
                // create fake island, islands are created after the extrusions
                //support_layer.set_islands(ExPolygons{ExPolygon()});
                //support_layer.add_region(object.regions().front());
                //support_layer.get_mutable_island(0)->
                //ExtrusionEntityCollection &support_storage = support_layer.islands().front()->regions_islands().front()->mutable_extrusion(LayerRegionIsland::SUPPORT);
                if (! to_infill_polygons.empty()) {
                    Fill *filler = support_params.with_sheath ? filler_support_with_sheath.get() : filler_support.get();
                    filler->angle = support_params.raft_angle_base;
                    filler->link_max_length = scale_i(support_params.raft_flow.spacing() * link_max_length_factor /
                                                      support_params.support_density);
                    fill_expolygons_generate_paths(
                        // Destination
                        raft_cache[&support_layer].first,
                        // Regions to fill
                        closing_ex(tree_polygons.empty() ? to_infill_polygons : diff(to_infill_polygons, tree_polygons),
                                   float(SCALED_EPSILON), float(SCALED_EPSILON + 0.5 * flow.scaled_width())),
                        // Filler and its parameters
                        filler, float(support_params.support_density),
                        // Extrusion parameters
                        ExtrusionRole::SupportMaterial, flow, support_params.raft_flow.spacing(), support_params.default_region_config);
#ifndef NDEBUG
                    raft_cache[&support_layer].first.visit(verifier);
#endif // NDEBUG
                }
                if (! tree_polygons.empty())
                    tree_supports_generate_paths(raft_cache[&support_layer].first, tree_polygons, flow, support_params);
            }

            Fill *filler = filler_interface.get();
            Flow  flow = support_params.first_layer_flow;
            float density = 0.f;
            double spacing = 0.f;
            if (support_layer_id == 0) {
                // Base flange.
                filler        = filler_dense.get();
                filler->angle = support_params.raft_angle_1st_layer;
                spacing       = support_params.first_layer_flow.spacing();
                density       = float(config.raft_first_layer_density.value * 0.01);
            } else if (support_layer_id >= slicing_params.base_raft_layers) {
                const LayerSupportProperty *support_property = support_layer.get_property<LayerSupportProperty>();
                assert(support_property != nullptr);
                filler->angle = support_params.raft_interface_angle(support_property->interface_id) +
                                ((support_layer_id - slicing_params.base_raft_layers) * support_params.interface_angle_incr);
                // We don't use $base_flow->spacing because we need a constant spacing
                // value that guarantees that all layers are correctly aligned.
                spacing = support_params.raft_interface_flow.spacing();
                assert(!raft_layer.bridging);
                float nzd     = support_params.raft_interface_flow.nozzle_diameter();
                flow          = !raft_layer.bridging ?
                                    Flow::new_from_width(float(support_params.raft_interface_flow.width()), nzd, float(raft_layer.unscaled_height()),
                                                support_params.raft_interface_flow.spacing_ratio()) :
                                    Flow::bridging_flow(nzd * std::sqrt(support_params.raft_bridge_flow_ratio), nzd);
                //flow          = Flow(float(support_params.raft_interface_flow.width()), float(raft_layer.height), support_params.raft_interface_flow.nozzle_diameter());
                density       = float(support_params.raft_interface_density);
            } else
                continue;
            filler->link_max_length = scale_i(spacing * link_max_length_factor / density);
            //fill_expolygons_with_sheath_generate_paths( //TODO 2.7 test if the pattern contains the sheath
            ExtrusionEntityCollection &support_storage =
                    (support_layer_id < slicing_params.base_raft_layers) ? raft_cache[&support_layer].first :
                                                                           raft_cache[&support_layer].second;
            fill_expolygons_generate_paths(
                // Destination
                support_storage,
                // Regions to fill
                closing_ex(tree_polygons.empty() ? raft_layer.polygons : diff(raft_layer.polygons, tree_polygons), float(SCALED_EPSILON),
                           float(SCALED_EPSILON + 0.5 * flow.scaled_width())),
                // Filler and its parameters
                filler, density,
                // Extrusion parameters
                (support_layer_id < slicing_params.base_raft_layers) ? ExtrusionRole::SupportMaterial :
                                                                       ExtrusionRole::SupportMaterialInterface,
                flow, spacing, support_params.default_region_config);
#ifndef NDEBUG
            support_storage.visit(verifier);
#endif // NDEBUG
            raft_layers_bb[support_layer_id] = CreateBoundingBoxVisitor::create(raft_cache[&support_layer].first);
            raft_layers_bb[support_layer_id].merge(CreateBoundingBoxVisitor::create(raft_cache[&support_layer].second));
        }
    });

    BoundingBox raft_bb;
    for (size_t i = 0; i < raft_layers_bb.size(); i++) {
        raft_bb.merge(raft_layers_bb[i]);
    }

    tbb::parallel_for(tbb::blocked_range<size_t>(0, n_raft_layers),
        [&support_layers, &raft_bb, &object, &raft_cache, support_extruder, support_interface_extruder]
            (const tbb::blocked_range<size_t>& range) {
        for (size_t support_layer_id = range.begin(); support_layer_id < range.end(); ++support_layer_id) {
            Layer &support_layer = *support_layers[support_layer_id];
            if (!raft_bb.empty()) {
                //support_layer.set_islands(ExPolygons{ExPolygon(raft_bb.polygon())});
                ApiInternal::LayerAccess::set_islands(support_layer, ExPolygons{ExPolygon(raft_bb.polygon())});
                // add regions
                // note: region slice is on the object, not the support, so we only get the first region, wich should
                // be the default one.
                // for (const std::reference_wrapper<const PrintRegion> &pregion : object.all_regions()) {
                //    support_layer.add_region(&pregion.get());
                //}
                ApiInternal::LayerAccess::add_region(support_layer, object.printing_region(0));
                BoundingBox bb_region = raft_bb;
                //support_layer.get_region(0)->set_raw_slices(ExPolygons{ExPolygon(bb_region.polygon())});
                ApiInternal::LayerRegionAccess::slices_mutable(support_layer.region(0)) = ExPolygons{ExPolygon(bb_region.polygon())};
                // construct island region
                support_layer.add_regions_to_islands();
                for (const LayerSliceIsland &island : support_layer.islands()) {
                    assert(!island.regions().empty());
                }
                // push extrusions
                assert(raft_cache.find(&support_layer) != raft_cache.end());
                std::pair<ExtrusionEntityCollection, ExtrusionEntityCollection> &supp_and_interface =
                    raft_cache[&support_layer];
                // add raft that is already computed.
                for (ExtrusionEntity *ee : supp_and_interface.first.entities()) {
                    int added = 0;
                    for (LayerSliceIsland &island : support_layer.islands()) {
                        // if ee is in this island
                        BoundingBox ee_bb = CreateBoundingBoxVisitor::create(*ee);
                        if (ee_bb.overlap(island.get_bounding_box())) {
                            if (!intersection_pl(to_polylines(ee->as_polylines()), island.get_slice()).empty()) {
                                island.get_or_add_region_island({&support_layer.region(0)}, support_extruder)
                                    .mutable_extrusion(LayerRegionIsland::SUPPORT)
                                    .append(std::move(*ee));
                                added++;
                                break;
                            }
                        }
                    }
                    assert(added == 1);
                }
                for (ExtrusionEntity *ee : supp_and_interface.second.entities()) {
                    int added = 0;
                    for (LayerSliceIsland &island : support_layer.islands()) {
                        // if ee is in this island
                        BoundingBox ee_bb = CreateBoundingBoxVisitor::create(*ee);
                        if (ee_bb.overlap(island.get_bounding_box())) {
                            if (!intersection_pl(to_polylines(ee->as_polylines()), island.get_slice()).empty()) {
                                island.get_or_add_region_island({&support_layer.region(0)}, support_interface_extruder)
                                    .mutable_extrusion(LayerRegionIsland::SUPPORT_INTERFACE)
                                    .append(std::move(*ee));
                                added++;
                                break;
                            }
                        }
                    }
                    assert(added == 1);
                }
            }
        }
        });

    struct LayerCacheItem {
        LayerCacheItem(SupportGeneratorLayerExtruded *layer_extruded = nullptr) : layer_extruded(layer_extruded) {}
        SupportGeneratorLayerExtruded         *layer_extruded;
        std::vector<SupportGeneratorLayer*>    overlapping;
    };
    struct LayerCache {
        SupportGeneratorLayerExtruded                                     bottom_contact_layer;
        SupportGeneratorLayerExtruded                                     top_contact_layer;
        SupportGeneratorLayerExtruded                                     base_layer;
        SupportGeneratorLayerExtruded                                     interface_layer;
        SupportGeneratorLayerExtruded                                     base_interface_layer;
        boost::container::static_vector<LayerCacheItem, 5>  nonempty;

        void add_nonempty_and_sort() {
            for (SupportGeneratorLayerExtruded *item : { &bottom_contact_layer, &top_contact_layer, &interface_layer, &base_interface_layer, &base_layer })
                if (! item->empty())
                    this->nonempty.emplace_back(item);
            // Sort the layers with the same print_z coordinate by their heights, thickest first.
            std::stable_sort(this->nonempty.begin(), this->nonempty.end(),
                [](const LayerCacheItem &lc1, const LayerCacheItem &lc2) { return lc1.layer_extruded->layer->scaled_height() > lc2.layer_extruded->layer->scaled_height(); });
        }
    };

    std::vector<LayerCache>             layer_caches(support_layers.size());
    // Filler for the 1st layer interface.
    std::unique_ptr<FillWithPerimeter> filler_first_layer = std::make_unique<FillWithPerimeter>(Fill::new_from_type(ipRectilinear));
    filler_first_layer->ratio_fill_inside                 = 0.2f;
    filler_first_layer->set_bounding_box(bbox_object);

    size_t raft_top_interface_idx                         = slicing_params.interface_raft_layers == 0 ?
                                                                size_t(-1) :
                                                                (slicing_params.base_raft_layers + slicing_params.interface_raft_layers - 1);

    tbb::parallel_for(tbb::blocked_range<size_t>(n_raft_layers, support_layers.size()),
        [&config, &slicing_params, &support_params, &support_layers, &bottom_contacts, &top_contacts,
         &intermediate_layers, &interface_layers, &base_interface_layers, &layer_caches, &loop_interface_processor,
         &bbox_object, n_raft_layers, link_max_length_factor, &filler_first_layer, &object, &raft_bb,
         raft_top_interface_idx]
            (const tbb::blocked_range<size_t>& range) {
        // Indices of the 1st layer in their respective container at the support layer height.
        size_t idx_layer_bottom_contact   = size_t(-1);
        size_t idx_layer_top_contact      = size_t(-1);
        size_t idx_layer_intermediate     = size_t(-1);
        size_t idx_layer_interface        = size_t(-1);
        size_t idx_layer_base_interface   = size_t(-1);
        std::unique_ptr<Fill> filler_top_interface          = std::unique_ptr<Fill>(Fill::new_from_type(support_params.contact_top_fill_pattern));
        std::unique_ptr<Fill> filler_bottom_interface       = std::unique_ptr<Fill>(Fill::new_from_type(support_params.contact_bottom_fill_pattern));
        std::unique_ptr<Fill> filler_intermediate_interface = std::unique_ptr<Fill>(Fill::new_from_type(ipRectilinear));
        // Filler for the raft contact layer
        std::unique_ptr<Fill> filler_raft_contact = std::unique_ptr<Fill>(
            range.begin() == n_raft_layers && config.support_material_interface_layers.value == 0 ?
                Fill::new_from_type(support_params.raft_interface_fill_pattern) :
                Fill::new_from_type(support_params.contact_top_fill_pattern)); // ??? why not raft_interface_fill_pattern?
        // Filler for the base interface (to be used for soluble interface / non soluble base, to produce non soluble interface layer below soluble interface layer).
        std::unique_ptr<Fill> filler_base_interface = base_interface_layers.empty() ?
                                                          std::unique_ptr<Fill>{} :
                                                          std::unique_ptr<Fill>{Fill::new_from_type(
                                                              support_params.interface_density > 0.95 || support_params.with_sheath ?
                                                                  ipRectilinear :
                                                                  ipSupportBase)};
        std::unique_ptr<Fill> filler_support;
        if (support_params.with_sheath) {
            FillWithPerimeter *ptr = new FillWithPerimeter(Fill::new_from_type(support_params.base_fill_pattern));
            ptr->ratio_fill_inside = 0.2f;
            filler_support.reset(ptr);
        } else {
            filler_support.reset(Fill::new_from_type(support_params.base_fill_pattern));
        }
        filler_top_interface->set_bounding_box(bbox_object);
        filler_bottom_interface->set_bounding_box(bbox_object);
        filler_raft_contact->set_bounding_box(bbox_object);
        filler_intermediate_interface->set_bounding_box(bbox_object);
        filler_support->set_bounding_box(bbox_object);
        if(filler_base_interface)
            filler_base_interface->set_bounding_box(bbox_object);
        for (size_t support_layer_id = range.begin(); support_layer_id < range.end(); ++ support_layer_id)
        {
            Layer &support_layer = *support_layers[support_layer_id];
            LayerCache   &layer_cache   = layer_caches[support_layer_id];
            float         interface_angle_delta = 0;
            //const float   support_interface_angle = config.support_material_style.value == smsGrid ?
            //    support_params.interface_angle : support_params.raft_interface_angle(support_property->interface_id);
            const LayerSupportProperty *support_property = support_layer.get_property<LayerSupportProperty>();
            assert(support_property != nullptr);
            if (support_params.interface_angle_incr == 0 && config.support_material_style.value == smsSnug) {
                interface_angle_delta = support_params.raft_interface_angle(support_property->interface_id);
            } else if (support_params.interface_angle_incr > 0) {
                interface_angle_delta = support_params.interface_angle + support_property->interface_id * support_params.interface_angle_incr;
            }

            // compute if the support has to switch its angle
            float suppport_angle = support_params.base_angle;
            if (config.support_material_pattern.value == smpRectilinearGrid && support_layer_id % 2 == 1) {
                suppport_angle = support_params.interface_angle;
            } else if (support_params.base_angle_height > 0 && (int(support_layer.unscaled_print_z() / support_params.base_angle_height)) % 2 == 1) {
                suppport_angle += float(M_PI) / 2;
            }

            // Find polygons with the same print_z.
            SupportGeneratorLayerExtruded &bottom_contact_layer = layer_cache.bottom_contact_layer;
            SupportGeneratorLayerExtruded &top_contact_layer    = layer_cache.top_contact_layer;
            SupportGeneratorLayerExtruded &base_layer           = layer_cache.base_layer;
            SupportGeneratorLayerExtruded &interface_layer      = layer_cache.interface_layer;
            SupportGeneratorLayerExtruded &base_interface_layer = layer_cache.base_interface_layer;
            // Increment the layer indices to find a layer at support_layer.print_z.
            {
                auto fun = [&support_layer](const SupportGeneratorLayer *l){ return l->scaled_print_z() >= support_layer.scaled_print_z() - EPSILON; };
                idx_layer_bottom_contact  = idx_higher_or_equal(bottom_contacts,     idx_layer_bottom_contact,  fun);
                idx_layer_top_contact     = idx_higher_or_equal(top_contacts,        idx_layer_top_contact,     fun);
                idx_layer_intermediate    = idx_higher_or_equal(intermediate_layers, idx_layer_intermediate,    fun);
                idx_layer_interface       = idx_higher_or_equal(interface_layers,    idx_layer_interface,       fun);
                idx_layer_base_interface  = idx_higher_or_equal(base_interface_layers, idx_layer_base_interface,fun);
            }
            // Copy polygons from the layers.
            if (idx_layer_bottom_contact < bottom_contacts.size() && bottom_contacts[idx_layer_bottom_contact]->scaled_print_z() <= support_layer.scaled_print_z())
                bottom_contact_layer.layer = bottom_contacts[idx_layer_bottom_contact];
            if (idx_layer_top_contact < top_contacts.size() && top_contacts[idx_layer_top_contact]->scaled_print_z() <= support_layer.scaled_print_z())
                top_contact_layer.layer = top_contacts[idx_layer_top_contact];
            if (idx_layer_interface < interface_layers.size() && interface_layers[idx_layer_interface]->scaled_print_z() <= support_layer.scaled_print_z())
                interface_layer.layer = interface_layers[idx_layer_interface];
            if (idx_layer_base_interface < base_interface_layers.size() && base_interface_layers[idx_layer_base_interface]->scaled_print_z() <= support_layer.scaled_print_z())
                base_interface_layer.layer = base_interface_layers[idx_layer_base_interface];
            if (idx_layer_intermediate < intermediate_layers.size() && intermediate_layers[idx_layer_intermediate]->scaled_print_z() <= support_layer.scaled_print_z())
                base_layer.layer = intermediate_layers[idx_layer_intermediate];

            if(bottom_contact_layer.layer)  assert_valid(bottom_contact_layer.polygons_to_extrude());
            if(top_contact_layer.layer)     assert_valid(top_contact_layer.polygons_to_extrude());
            bool has_top_contact_layer = top_contact_layer.layer != nullptr;
            if(base_layer.layer)            assert_valid(base_layer.polygons_to_extrude());
            if(interface_layer.layer)       assert_valid(interface_layer.polygons_to_extrude());
            if(base_interface_layer.layer)  assert_valid(base_interface_layer.polygons_to_extrude());

            // This layer is a raft contact layer. Any contact polygons at this layer are raft contacts.
            bool raft_layer = slicing_params.interface_raft_layers && top_contact_layer.layer &&
                top_contact_layer.layer->scaled_print_z() == scale_to_layer_coord(slicing_params.raft_contact_top_z);
            if (config.support_material_interface_layers == 0) {
                // If no top interface layers were requested, we treat the contact layer exactly as a generic base layer.
                // Don't merge the raft contact layer though.
                if (support_params.can_merge_support_regions && !raft_layer) {
                    if (base_layer.could_merge(top_contact_layer)) {
                        base_layer.merge(std::move(top_contact_layer), support_params.resolution);
                    } else if (base_layer.empty()) {
                        base_layer = std::move(top_contact_layer);
                    }
                }
            } else {
                loop_interface_processor.generate(top_contact_layer, support_params.support_material_interface_flow, support_params.resolution);
                // If no loops are allowed, we treat the contact layer exactly as a generic interface layer.
                // Merge interface_layer into top_contact_layer, as the top_contact_layer is not synchronized and therefore it will be used
                // to trim other layers.
                if (top_contact_layer.could_merge(interface_layer) && !raft_layer) {
                    if(interface_layer.layer)       assert_valid(interface_layer.polygons_to_extrude());
#ifdef _DEBUGINFO
                    LoopAssertVisitor().traverse(top_contact_layer.extrusions);
                    LoopAssertVisitor().traverse(interface_layer.extrusions);
                    if(top_contact_layer.layer)     assert_valid(top_contact_layer.polygons_to_extrude());
                    if(interface_layer.layer)     assert_valid(interface_layer.polygons_to_extrude());
#endif
                    top_contact_layer.merge(std::move(interface_layer), support_params.resolution);
                    if(top_contact_layer.layer)     assert_valid(top_contact_layer.polygons_to_extrude());
                }
            }
            if ( (config.support_material_interface_layers == 0 ||
                    (config.support_material_bottom_interface_layers.is_enabled() && config.support_material_bottom_interface_layers.value == 0))
                && support_params.can_merge_support_regions) {
                if (base_layer.could_merge(bottom_contact_layer))
                    base_layer.merge(std::move(bottom_contact_layer), support_params.resolution);
                else if (base_layer.empty() && ! bottom_contact_layer.empty() && ! bottom_contact_layer.layer->bridging)
                    base_layer = std::move(bottom_contact_layer);
            } else if (bottom_contact_layer.could_merge(top_contact_layer) && !raft_layer) {
                if(bottom_contact_layer.layer)  assert_valid(bottom_contact_layer.polygons_to_extrude());
                top_contact_layer.merge(std::move(bottom_contact_layer), support_params.resolution);
                if(top_contact_layer.layer)     assert_valid(top_contact_layer.polygons_to_extrude());
            } else if (bottom_contact_layer.could_merge(interface_layer)) {
                bottom_contact_layer.merge(std::move(interface_layer), support_params.resolution);
            }

            if(bottom_contact_layer.layer)  assert_valid(bottom_contact_layer.polygons_to_extrude());
            if(top_contact_layer.layer)     assert_valid(top_contact_layer.polygons_to_extrude());
            if(base_layer.layer)            assert_valid(base_layer.polygons_to_extrude());
            if(interface_layer.layer)       assert_valid(interface_layer.polygons_to_extrude());
            if(base_interface_layer.layer)  assert_valid(base_interface_layer.polygons_to_extrude());

#if 0
            if ( ! interface_layer.empty() && ! base_layer.empty()) {
                // turn base support into interface when it's contained in our holes
                // (this way we get wider interface anchoring)
                //FIXME The intention of the code below is unclear. One likely wanted to just merge small islands of base layers filling in the holes
                // inside interface layers, but the code below fills just too much, see GH #4570
                Polygons islands = top_level_islands(interface_layer.layer->polygons);
                polygons_append(interface_layer.layer->polygons, intersection(base_layer.layer->polygons, islands));
                base_layer.layer->polygons = diff(base_layer.layer->polygons, islands);
            }
#endif
            // Top and bottom contacts, interface layers.
            enum class InterfaceLayerType { TopContact, BottomContact, RaftContact, Interface, InterfaceAsBase };
            auto extrude_interface = [&](SupportGeneratorLayerExtruded &layer_ex, InterfaceLayerType interface_layer_type) {
#ifdef _DEBUGINFO
                for (const auto extr : layer_ex.extrusions.entities()) LoopAssertVisitor().traverse(*extr);
#endif
                if (! layer_ex.empty() && ! layer_ex.polygons_to_extrude().empty()) {
                    assert_valid(layer_ex.polygons_to_extrude());
                    bool interface_as_base = interface_layer_type == InterfaceLayerType::InterfaceAsBase;
                    bool raft_contact      = interface_layer_type == InterfaceLayerType::RaftContact;
                    //FIXME Bottom interfaces are extruded with the briding flow. Some bridging layers have its height slightly reduced, therefore
                    // the bridging flow does not quite apply. Reduce the flow to area of an ellipse? (A = pi * a * b)
                    Fill *filler = interface_layer_type == InterfaceLayerType::TopContact ?    filler_top_interface.get() :
                                   interface_layer_type == InterfaceLayerType::BottomContact ? filler_bottom_interface.get():
                                                                                               filler_intermediate_interface.get();
                    if (raft_contact)
                        filler = filler_raft_contact.get();
                    Flow  interface_flow = layer_ex.layer->bridging ?
                        Flow::bridging_flow((float)layer_ex.layer->unscaled_height(), support_params.support_material_bottom_interface_flow.nozzle_diameter()) :
                        (raft_contact ? &support_params.raft_interface_flow : 
                         interface_as_base ? &support_params.support_material_flow : &support_params.support_material_interface_flow)
                                                  ->with_height(float(layer_ex.layer->unscaled_height()));
                    // filler->layer_id = support_layer_id; // don't do that, or the filler will rotate thigns from that layerid
                    filler->z             = (double)support_layer.unscaled_print_z();
                    float    supp_density = support_params.interface_density;
                    double   filler_spacing;
                    // if first layer and solid first layer : draw concentric with 100% density
                    if (support_layer.id() == 0 && layer_ex.layer->scaled_bottom_z() <= 0) {
                        filler         = filler_first_layer.get();
                        supp_density   = float(config.raft_first_layer_density.get_effective_value(1.));
                        interface_flow = support_params.first_layer_flow;
                        filler->angle  = 0;
                        filler_spacing = interface_flow.spacing();
                    } else {
                        if (raft_top_interface_idx == support_layer_id) {
                            filler->angle  = support_params.raft_angle_interface + ((support_layer_id - slicing_params.base_raft_layers) *
                                                                    support_params.interface_angle_incr);
                            supp_density   = interface_as_base ? support_params.support_density : support_params.interface_density;
                            filler_spacing = interface_as_base ? support_params.raft_flow.spacing() :
                                                                 support_params.raft_interface_flow.spacing();
                        } else {
                            filler->angle  = interface_as_base ?
                                                 // If zero interface layers are configured, use the same angle as for the base layers.
                                                suppport_angle :
                                                // Use interface angle for the interface layers.
                                                support_params.interface_angle + interface_angle_delta;
                            supp_density   = interface_as_base ? support_params.support_density : support_params.interface_density;
                            filler_spacing = interface_as_base ? support_params.support_material_flow.spacing() :
                                                                 support_params.support_material_interface_flow.spacing();
                        }
                        filler->link_max_length = scale_i(filler_spacing * link_max_length_factor / supp_density);
                    }
                
                    //filler->angle = interface_as_base ?
                    //        // If zero interface layers are configured, use the same angle as for the base layers.
                    //        angles[support_layer_id % angles.size()] :
                    //        // Use interface angle for the interface layers.
                    //        raft_contact ? 
                    //            support_params.raft_interface_angle(support_property->interface_id) :
                    //            support_interface_angle;
                    //double density = raft_contact ? support_params.raft_interface_density : interface_as_base ? support_params.support_density : support_params.interface_density;
                    //filler->spacing = raft_contact ? support_params.raft_interface_flow.spacing() :
                    //    interface_as_base ? support_params.support_material_flow.spacing() : support_params.support_material_interface_flow.spacing();
                    //filler->link_max_length = coord_t(scale_(filler->spacing * link_max_length_factor / density));
                    ExPolygons fused_layer_polys = union_safety_offset_ex(layer_ex.polygons_to_extrude());
                    ensure_valid(fused_layer_polys, support_params.resolution);
                    assert_valid(fused_layer_polys);
                    fill_expolygons_generate_paths(
                        // Destination
                        layer_ex.extrusions,
                        // Regions to fill
                        std::move(fused_layer_polys),
                        // Filler and its parameters
                        filler, float(supp_density),
                        // Extrusion parameters
                        interface_as_base ? ExtrusionRole::SupportMaterial : ExtrusionRole::SupportMaterialInterface, interface_flow,
                        filler_spacing, support_params.default_region_config);
                }
            };
            const bool top_interfaces = config.support_material_interface_layers.value != 0;
            const bool bottom_interfaces = config.support_material_bottom_interface_layers.is_enabled() ?
                top_interfaces && config.support_material_bottom_interface_layers.value != 0 :
                top_interfaces;
            extrude_interface(top_contact_layer,    raft_layer ? InterfaceLayerType::RaftContact : top_interfaces ? InterfaceLayerType::TopContact : InterfaceLayerType::InterfaceAsBase);
            extrude_interface(bottom_contact_layer, bottom_interfaces ? InterfaceLayerType::BottomContact : InterfaceLayerType::InterfaceAsBase);
            extrude_interface(interface_layer,      top_interfaces ? InterfaceLayerType::Interface : InterfaceLayerType::InterfaceAsBase);

            // Base interface layers under soluble interfaces
            if ( ! base_interface_layer.empty() && ! base_interface_layer.polygons_to_extrude().empty()) {
                Fill *filler = filler_base_interface.get();
                double filler_spacing = filler->get_spacing();
                //FIXME Bottom interfaces are extruded with the briding flow. Some bridging layers have its height slightly reduced, therefore
                // the bridging flow does not quite apply. Reduce the flow to area of an ellipse? (A = pi * a * b)
                assert(! base_interface_layer.layer->bridging);
                Flow interface_flow     = support_params.support_material_flow.with_height(float(base_interface_layer.layer->unscaled_height()));
                filler->angle           = suppport_angle + interface_angle_delta;
                filler_spacing  = support_params.support_material_interface_flow.spacing();
                filler->link_max_length = scale_i(filler->get_spacing() * link_max_length_factor / support_params.interface_density);
                fill_expolygons_generate_paths(
                    // Destination
                    base_interface_layer.extrusions,
                    //base_layer_interface.extrusions,
                    // Regions to fill
                    union_safety_offset_ex(base_interface_layer.polygons_to_extrude()),
                    // Filler and its parameters
                    filler, float(support_params.interface_density),
                    // Extrusion parameters
                    ExtrusionRole::SupportMaterial, interface_flow, filler_spacing, support_params.default_region_config);
            }

            // Base support or flange.
            if (! base_layer.empty() && ! base_layer.polygons_to_extrude().empty()) {
                Fill *filler = filler_support.get();
                double filler_spacing = support_params.support_material_flow.spacing();
                // We don't use $base_flow->spacing because we need a constant spacing
                // value that guarantees that all layers are correctly aligned.
                assert(! base_layer.layer->bridging);
                auto flow = support_params.support_material_flow.with_height(float(base_layer.layer->unscaled_height()));
                float density = float(support_params.support_density);
                bool  done    = false;
                if (base_layer.layer->scaled_bottom_z() <= 0) {
                    // Base flange (the 1st layer).
                    filler                  = filler_first_layer.get();
                    filler->angle           = Geometry::deg2rad(float(config.support_material_angle.value + 90.));
                    density                 = float(config.raft_first_layer_density.get_effective_value(1.f));
                    filler->link_max_length = scale_i(filler_spacing * link_max_length_factor / density);
                    flow = support_params.first_layer_flow;
                    // use the proper spacing for first layer as we don't need to align
                    // its pattern to the other layers
                    //FIXME When paralellizing, each thread shall have its own copy of the fillers.
                    filler_spacing          = flow.spacing();
                    filler->link_max_length = scale_i(filler_spacing * link_max_length_factor / density);
                } else if (config.support_material_style.value == SupportMaterialStyle::smsOrganic) {
                    tree_supports_generate_paths(base_layer.extrusions, base_layer.polygons_to_extrude(), flow, support_params);
                    done = true;
                } else {
                    filler->angle           = suppport_angle;
                    filler->link_max_length = scale_i(filler_spacing * link_max_length_factor / support_params.support_density);
                }
                if (! done) {
                    ExPolygons base_expolys = closing_ex(base_layer.polygons_to_extrude(), float(SCALED_EPSILON),
                                   float(SCALED_EPSILON /*+ 0.5 * flow.scaled_width()*/));
                    ensure_valid(base_expolys, support_params.resolution);
                    //fill_expolygons_with_sheath_generate_paths(
                    fill_expolygons_generate_paths( //TODO: 2.7 test if pattern has the sheath
                        // Destination
                        base_layer.extrusions,
                        // Regions to fill
                        std::move(base_expolys),
                        // Filler and its parameters
                        filler, density,
                        // Extrusion parameters
                        ExtrusionRole::SupportMaterial, flow, filler_spacing, support_params.default_region_config);
#ifdef _DEBUGINFO
                    for(auto entity : base_layer.extrusions) LoopAssertVisitor().traverse(*entity);
#endif
                }
            }

            // Merge base_interface_layers to base_layers to avoid unneccessary retractions
            if (! base_layer.empty() && ! base_interface_layer.empty() && ! base_layer.polygons_to_extrude().empty() && ! base_interface_layer.polygons_to_extrude().empty() &&
                base_layer.could_merge(base_interface_layer))
                base_layer.merge(std::move(base_interface_layer), support_params.resolution);

            layer_cache.add_nonempty_and_sort();

            // Collect the support areas with this print_z into islands, as there is no need
            // for retraction over these islands.
            ExPolygons islands_expolys;
            // Collect the extrusions, sorted by the bottom extrusion height.
            for (LayerCacheItem &layer_cache_item : layer_cache.nonempty) {
                // Collect islands to polys.
                if (layer_cache_item.layer_extruded->layer != nullptr &&
                    !layer_cache_item.layer_extruded->layer->polygons.empty()) {
                    islands_expolys = union_ex(islands_expolys, layer_cache_item.layer_extruded->layer->polygons);
                }
                // The print_z of the top contact surfaces and bottom_z of the bottom contact surfaces are "free"
                // in a sense that they are not synchronized with other support layers. As the top and bottom contact surfaces
                // are inflated to achieve a better anchoring, it may happen, that these surfaces will at least partially
                // overlap in Z with another support layers, leading to over-extrusion.
                // Mitigate the over-extrusion by modulating the extrusion rate over these regions.
                // The print head will follow the same print_z, but the layer thickness will be reduced
                // where it overlaps with another support layer.
                //FIXME When printing a briging path, what is an equivalent height of the squished extrudate of the same width?
                // Collect overlapping top/bottom surfaces.
                layer_cache_item.overlapping.reserve(20);
                coord_t bottom_z = layer_cache_item.layer_extruded->layer->scaled_bottom_print_z();
                auto add_overlapping = [&layer_cache_item, bottom_z](const SupportGeneratorLayersPtr &layers, size_t idx_top) {
                    for (int i = int(idx_top) - 1; i >= 0 && layers[i]->scaled_print_z() > bottom_z; -- i)
                        layer_cache_item.overlapping.push_back(layers[i]);
                };
                add_overlapping(top_contacts, idx_layer_top_contact);
                if (layer_cache_item.layer_extruded->layer->layer_type == SupporLayerType::BottomContact) {
                    // Bottom contact layer may overlap with a base layer, which may be changed to interface layer.
                    add_overlapping(intermediate_layers,   idx_layer_intermediate);
                    add_overlapping(interface_layers,      idx_layer_interface);
                    add_overlapping(base_interface_layers, idx_layer_base_interface);
                }
                // Order the layers by lexicographically by an increasing print_z and a decreasing layer height.
                std::stable_sort(layer_cache_item.overlapping.begin(), layer_cache_item.overlapping.end(), [](auto *l1, auto *l2) { return *l1 < *l2; });
            }
            assert(support_layer.islands().empty());
            if (!islands_expolys.empty()) {
                BoundingBox bb_region = raft_bb;
                bb_region.merge(get_extents(islands_expolys));
                assert(!bb_region.empty());
                //support_layer.set_islands(std::move(islands_expolys));
                ApiInternal::LayerAccess::set_islands(support_layer, std::move(islands_expolys));
                // add regions
                // note: region slice is on the object, not the support, so we only get the first region, wich should be the default one.
                ApiInternal::LayerAccess::add_region(support_layer, object.printing_region(0));
                //support_layer.get_region(0)->set_raw_slices(ExPolygons{ExPolygon(bb_region.polygon())});
                ApiInternal::LayerRegionAccess::slices_mutable(support_layer.region(0)) = ExPolygons{ExPolygon(bb_region.polygon())};
                // construct island region
                support_layer.add_regions_to_islands();
                for (const LayerSliceIsland &island : support_layer.islands()) {
                    assert(!island.regions().empty());
                }

            } else {
                // is it possible???
                assert(false);
            }
        } // for each support_layer_id
    });

    // Now modulate the support layer height in parallel.
    tbb::parallel_for(tbb::blocked_range<size_t>(n_raft_layers, support_layers.size()),
        [&support_layers, &layer_caches, support_extruder, support_interface_extruder]
            (const tbb::blocked_range<size_t>& range) {
        for (size_t support_layer_id = range.begin(); support_layer_id < range.end(); ++ support_layer_id) {
            Layer &support_layer = *support_layers[support_layer_id];
            LayerCache   &layer_cache   = layer_caches[support_layer_id];
            // For all extrusion types at this print_z, ordered by decreasing layer height:
            for (LayerCacheItem &layer_cache_item : layer_cache.nonempty) {
                if (layer_cache_item.layer_extruded->extrusions.empty())
                    continue;
#ifdef _DEBUGINFO
                for(auto entity : layer_cache_item.layer_extruded->extrusions) LoopAssertVisitor().traverse(*entity);
#endif
                // Trim the extrusion height from the bottom by the overlapping layers.
                modulate_extrusion_by_overlapping_layers(layer_cache_item.layer_extruded->extrusions, *layer_cache_item.layer_extruded->layer, layer_cache_item.overlapping);
                if (layer_cache_item.layer_extruded->extrusions.empty())
                    continue;
#ifndef NDEBUG
                layer_cache_item.layer_extruded->extrusions.visit(verifier);
#endif // NDEBUG
                for (ExtrusionEntity *ee : layer_cache_item.layer_extruded->extrusions) {
                    int added = 0;
                    for (LayerSliceIsland &island : support_layer.islands()) {
                        // if ee is in this island
                        BoundingBox ee_bb = CreateBoundingBoxVisitor::create(*ee);
                        if (ee_bb.overlap(island.get_bounding_box())) {
                            if (!intersection_pl(to_polylines(ee->as_polylines()), island.get_slice()).empty()) {
                                assert(!ee->role().is_mixed());
                                assert(ee->role().is_support());
                                if (ee->role().is_support_base()) {
                                    island.get_or_add_region_island({&support_layer.region(0)}, support_extruder)
                                        .mutable_extrusion(LayerRegionIsland::SUPPORT)
                                        .append(std::move(*ee));
                                } else {
                                    island.get_or_add_region_island({&support_layer.region(0)}, support_interface_extruder)
                                        .mutable_extrusion(LayerRegionIsland::SUPPORT_INTERFACE)
                                        .append(std::move(*ee));
                                }
                                added++;
                                break;
                            }
                        }
                    }
                    assert(added == 1);
                }
#ifndef NDEBUG
                for (const LayerSliceIsland &island : support_layer.islands()) {
                    for (const LayerRegionIsland &region_island : island.regions_islands()) {
                        if (region_island.has_extrusion(LayerRegionIsland::SUPPORT)) {
                            region_island.extrusion(LayerRegionIsland::SUPPORT).visit(verifier);
                        }
                        if (region_island.has_extrusion(LayerRegionIsland::SUPPORT_INTERFACE)) {
                            region_island.extrusion(LayerRegionIsland::SUPPORT_INTERFACE).visit(verifier);
                        }
                    }
                }
#endif // NDEBUG
            }
        }
    });

    // link support islands beetween layers
    tbb::parallel_for(
        tbb::blocked_range<size_t>(1, support_layers.size()),
        [&support_layers](const tbb::blocked_range<size_t> &range) {
            for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++ layer_idx) {
                Layer::build_up_down_graph(*support_layers[layer_idx - 1], *support_layers[layer_idx]);
            }
        }
    );

#ifndef NDEBUG
    const Layer *support_layer_current = nullptr;
    int                 idx                   = 0;
    for (const LayerUPtr &support_layer : support_layers) {
        assert(support_layer->object() == &object);
        support_layer_current = support_layer.get();

        for (const LayerSliceIsland &island : support_layer->islands()) {
            for (const LayerRegionIsland &region_island : island.regions_islands()) {
                if (region_island.has_extrusion(LayerRegionIsland::SUPPORT)) {
                    region_island.extrusion(LayerRegionIsland::SUPPORT).visit(verifier);
                }
                if (region_island.has_extrusion(LayerRegionIsland::SUPPORT_INTERFACE)) {
                    region_island.extrusion(LayerRegionIsland::SUPPORT_INTERFACE).visit(verifier);
                }
            }
        }
        idx++;
    }
#endif // NDEBUG
}

/*
void PrintObjectSupportMaterial::clip_by_pillars(
    const PrintObject   &object,
    LayersPtr           &bottom_contacts,
    LayersPtr           &top_contacts,
    LayersPtr           &intermediate_contacts);

{
    // this prevents supplying an empty point set to BoundingBox constructor
    if (top_contacts.empty())
        return;

    coord_t pillar_size    = scale_(PILLAR_SIZE);
    coord_t pillar_spacing = scale_(PILLAR_SPACING);
    
    // A regular grid of pillars, filling the 2D bounding box.
    Polygons grid;
    {
        // Rectangle with a side of 2.5x2.5mm.
        Polygon pillar;
        pillar.points.push_back(Point(0, 0));
        pillar.points.push_back(Point(pillar_size, 0));
        pillar.points.push_back(Point(pillar_size, pillar_size));
        pillar.points.push_back(Point(0, pillar_size));
        
        // 2D bounding box of the projection of all contact polygons.
        BoundingBox bbox;
        for (LayersPtr::const_iterator it = top_contacts.begin(); it != top_contacts.end(); ++ it)
            bbox.merge(get_extents((*it)->polygons));
        grid.reserve(size_t(ceil(bb.size()(0) / pillar_spacing)) * size_t(ceil(bb.size()(1) / pillar_spacing)));
        for (coord_t x = bb.min(0); x <= bb.max(0) - pillar_size; x += pillar_spacing) {
            for (coord_t y = bb.min(1); y <= bb.max(1) - pillar_size; y += pillar_spacing) {
                grid.push_back(pillar);
                for (size_t i = 0; i < pillar.points.size(); ++ i)
                    grid.back().points[i].translate(Point(x, y));
            }
        }
    }
    
    // add pillars to every layer
    for my $i (0..n_support_z) {
        $shape->[$i] = [ @$grid ];
    }
    
    // build capitals
    for my $i (0..n_support_z) {
        my $z = $support_z->[$i];
        
        my $capitals = intersection(
            $grid,
            $contact->{$z} // [],
        );
        
        // work on one pillar at time (if any) to prevent the capitals from being merged
        // but store the contact area supported by the capital because we need to make 
        // sure nothing is left
        my $contact_supported_by_capitals = [];
        foreach my $capital (@$capitals) {
            // enlarge capital tops
            $capital = offset([$capital], +($pillar_spacing - $pillar_size)/2);
            push @$contact_supported_by_capitals, @$capital;
            
            for (my $j = $i-1; $j >= 0; $j--) {
                my $jz = $support_z->[$j];
                $capital = offset($capital, -$self->interface_flow->scaled_width/2);
                last if !@$capitals;
                push @{ $shape->[$j] }, @$capital;
            }
        }
        
        // Capitals will not generally cover the whole contact area because there will be
        // remainders. For now we handle this situation by projecting such unsupported
        // areas to the ground, just like we would do with a normal support.
        my $contact_not_supported_by_capitals = diff(
            $contact->{$z} // [],
            $contact_supported_by_capitals,
        );
        if (@$contact_not_supported_by_capitals) {
            for (my $j = $i-1; $j >= 0; $j--) {
                push @{ $shape->[$j] }, @$contact_not_supported_by_capitals;
            }
        }
    }
}

sub clip_with_shape {
    my ($self, $support, $shape) = @_;
    
    foreach my $i (keys %$support) {
        // don't clip bottom layer with shape so that we 
        // can generate a continuous base flange 
        // also don't clip raft layers
        next if $i == 0;
        next if $i < $self->object_config->raft_layers;
        $support->{$i} = intersection(
            $support->{$i},
            $shape->[$i],
        );
    }
}
*/

} // namespace Slic3r
