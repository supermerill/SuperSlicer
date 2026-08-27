///|/ Copyright (c) SuperSlicer 2019-2025 Remi Durand @supermerill
///|/ Copyright (c) Prusa Research 2016 - 2023 Lukáš Matěna @lukasmatena, Vojtěch Bubník @bubnikv, Pavel Mikuš @Godrak, Lukáš Hejl @hejllukas
///|/ Copyright (c) 2016 Sakari Kapanen @Flannelhead
///|/ Copyright (c) Slic3r 2011 - 2015 Alessandro Ranellucci @alranel
///|/ Copyright (c) 2013 Mark Hindess
///|/ Copyright (c) 2011 Michael Moon
///|/
///|/ SuperSlicer, PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include <cassert>
#include <cstdio>
#include <memory>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntityVisitors.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Layer.hpp"
// for Arachne based infills
#include "libslic3r/PerimeterGenerator.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Config/PrintConfig.hpp"
#include "libslic3r/PrintObject.hpp"
#include "libslic3r/PrintRegion.hpp"
#include "libslic3r/Surface.hpp"

#include "FillBase.hpp"
#include "FillRectilinear.hpp"
#include "FillLightning.hpp"
#include "FillConcentric.hpp"
#include "FillEnsuring.hpp"

namespace Slic3r {

struct NormalizeVisitor : public ExtrusionVisitor {
    bool need_remove = false;
    //TODO path3D normalize
    virtual void default_use(ExtrusionEntity& entity) override {
        if (ArcPolyline *polyline = entity.polyline_or_null()) {
            need_remove = !polyline->normalize();
#ifdef _DEBUG
            for (size_t i = 1; i < polyline->size(); i++)
                assert(!polyline->get_point(i - 1).coincides_with_epsilon(polyline->get_point(i)));
#endif
            return;
        }

        if (entity.is_leaf()) {
            need_remove = false;
            return;
        }
#ifdef _DEBUG
        if (entity.is_continuous()) {
        for (size_t idx = 0; idx < entity.children().size(); idx++) {
            if (idx > 0)
                assert(entity.child(idx - 1).last_point() == entity.child(idx).first_point());
        }
        }
#endif
        for (size_t idx_p = 0; idx_p < entity.children().size(); idx_p++) {
            ExtrusionEntity &child = entity.child(idx_p);
            child.visit(*this);
            if (need_remove || child.empty()) {
                // ensure continuity
                if (entity.is_continuous() && idx_p + 1 < entity.children().size()) {
                    ArcPolyline *next_polyline = entity.child(idx_p + 1).polyline_or_null();
                    assert(next_polyline != nullptr);
                    if (next_polyline != nullptr)
                        next_polyline->append_before(child.first_point());
                } else if (entity.is_continuous() && idx_p > 0) {
                    ArcPolyline *previous_polyline = entity.child(idx_p - 1).polyline_or_null();
                    assert(previous_polyline != nullptr);
                    if (previous_polyline != nullptr && entity.child(idx_p - 1).last_point().coincides_with_epsilon(child.last_point())) {
                        previous_polyline->set_back(child.last_point());
                    } else if (previous_polyline != nullptr) {
                        previous_polyline->append(child.last_point());
                    }
                }
                // remove
                entity.remove_child(idx_p);
                --idx_p;
                need_remove = false;
            }
        }
#ifdef _DEBUG
        if (entity.is_continuous()) {
        for (size_t idx = 0; idx < entity.children().size(); idx++) {
            if (idx > 0)
                assert(entity.child(idx - 1).last_point() == entity.child(idx).first_point());
        }
        }
#endif
    }
} normalize_visitor;

struct SurfaceFillParams : FillParams
{
    // Infill pattern, adjusted for the density etc.
    InfillPattern   pattern = InfillPattern(0);

    // FillBase
    // in unscaled coordinates
    double        spacing = 0.;
    // infill / perimeter overlap, in unscaled coordinates
//    double        overlap = 0.;
    // Angle as provided by the region config, in radians.
    float           angle = 0.f;
    // If the region config allow, it's possible to rotate 90deg in odd layers
    bool            can_angle_cross =true;
    // Non-negative for a bridge.
    float           bridge_angle = 0.f;
    BridgeType      bridge_type = BridgeType::btFromNozzle;

    // Various print settings?

    // Index of this entry in a linear vector.
    size_t          idx = 0;


    bool operator<(const SurfaceFillParams &rhs) const {
#define RETURN_COMPARE_NON_EQUAL(KEY) if (this->KEY < rhs.KEY) return true; if (this->KEY > rhs.KEY) return false;
#define RETURN_COMPARE_NON_EQUAL_TYPED(TYPE, KEY) if (TYPE(this->KEY) < TYPE(rhs.KEY)) return true; if (TYPE(this->KEY) > TYPE(rhs.KEY)) return false;

        // Sort first by decreasing bridging angle, so that the bridges are processed with priority when trimming one layer by the other.
        if (this->bridge_angle > rhs.bridge_angle) return true; 
        if (this->bridge_angle < rhs.bridge_angle) return false;

        RETURN_COMPARE_NON_EQUAL(bridge_type);
        RETURN_COMPARE_NON_EQUAL(extruder);
        RETURN_COMPARE_NON_EQUAL_TYPED(unsigned, pattern);
        RETURN_COMPARE_NON_EQUAL(spacing);
//        RETURN_COMPARE_NON_EQUAL(overlap);
        RETURN_COMPARE_NON_EQUAL(angle);
        RETURN_COMPARE_NON_EQUAL(can_angle_cross);
        RETURN_COMPARE_NON_EQUAL(density);
        RETURN_COMPARE_NON_EQUAL(monotonic);
        RETURN_COMPARE_NON_EQUAL(max_sparse_infill_spacing);
        RETURN_COMPARE_NON_EQUAL_TYPED(unsigned, connection);
        RETURN_COMPARE_NON_EQUAL(add_gap_fill);
        RETURN_COMPARE_NON_EQUAL(dont_adjust);

        RETURN_COMPARE_NON_EQUAL(anchor_length);
        RETURN_COMPARE_NON_EQUAL(fill_exactly);
        RETURN_COMPARE_NON_EQUAL(flow.height());
        RETURN_COMPARE_NON_EQUAL(flow.nozzle_diameter());
        RETURN_COMPARE_NON_EQUAL_TYPED(unsigned, flow.bridge());
        RETURN_COMPARE_NON_EQUAL_TYPED(int32_t, priority);
        assert(this->config != nullptr);
        assert(rhs.config != nullptr);
        if (config != nullptr && rhs.config != nullptr) {
            RETURN_COMPARE_NON_EQUAL(config->infill_acceleration);
            RETURN_COMPARE_NON_EQUAL(config->infill_extruder);
            RETURN_COMPARE_NON_EQUAL(config->infill_speed);
            RETURN_COMPARE_NON_EQUAL(config->solid_infill_acceleration);
            RETURN_COMPARE_NON_EQUAL(config->solid_infill_speed);
            RETURN_COMPARE_NON_EQUAL(config->top_solid_infill_acceleration);
            RETURN_COMPARE_NON_EQUAL(config->top_solid_infill_speed);
            RETURN_COMPARE_NON_EQUAL(config->default_acceleration);
            RETURN_COMPARE_NON_EQUAL(config->default_speed);
            RETURN_COMPARE_NON_EQUAL(config->bridge_acceleration);
            RETURN_COMPARE_NON_EQUAL(config->bridge_speed);
            RETURN_COMPARE_NON_EQUAL(config->internal_bridge_acceleration);
            RETURN_COMPARE_NON_EQUAL(config->internal_bridge_speed);
            RETURN_COMPARE_NON_EQUAL(config->gap_fill_acceleration);
            RETURN_COMPARE_NON_EQUAL(config->gap_fill_speed);
            RETURN_COMPARE_NON_EQUAL(config->print_extrusion_multiplier);
            RETURN_COMPARE_NON_EQUAL(config->region_gcode.value)
            RETURN_COMPARE_NON_EQUAL(config->small_area_infill_flow_compensation_model.is_enabled())
            RETURN_COMPARE_NON_EQUAL(config->small_area_infill_flow_compensation_model.value);
            RETURN_COMPARE_NON_EQUAL(config->solid_infill_extruder);
            RETURN_COMPARE_NON_EQUAL(config->wipe_into_infill);
            // print modifier, because region are fused in gode wiew if not.
            RETURN_COMPARE_NON_EQUAL(config->print_extrusion_multiplier.value);
            RETURN_COMPARE_NON_EQUAL(config->print_first_layer_temperature.value);
            RETURN_COMPARE_NON_EQUAL(config->print_retract_length.value);
            RETURN_COMPARE_NON_EQUAL(config->print_retract_lift.value);
            RETURN_COMPARE_NON_EQUAL(config->print_temperature.value);
            // end print modifier
            if (this->pattern == ipConcentric || rhs.pattern == ipConcentric) {
                // arachne if concentric
                RETURN_COMPARE_NON_EQUAL(config->perimeter_generator.value);
                RETURN_COMPARE_NON_EQUAL(config->min_bead_width.value);
                RETURN_COMPARE_NON_EQUAL(config->min_feature_size.value);
                RETURN_COMPARE_NON_EQUAL(config->wall_distribution_count.value);
                RETURN_COMPARE_NON_EQUAL(config->wall_transition_angle.value);
                RETURN_COMPARE_NON_EQUAL(config->wall_transition_filter_deviation.value);
                RETURN_COMPARE_NON_EQUAL(config->wall_transition_length.value);
            }
        }
        if (config == nullptr || rhs.config == nullptr || max_sparse_infill_spacing == 0)
            RETURN_COMPARE_NON_EQUAL(flow.width());
        assert(*this == rhs);
        return this->role < rhs.role;
        // return this->extrusion_role.lower(rhs.extrusion_role);
    }

    bool operator==(const SurfaceFillParams &rhs) const {
        // first check speed via config
        if ((config != nullptr) != (rhs.config != nullptr))
            return false;
        if(config != nullptr && (
            config->infill_acceleration != rhs.config->infill_acceleration
            || config->infill_extruder != rhs.config->infill_extruder
            || config->infill_speed != rhs.config->infill_speed
            || config->solid_infill_acceleration != rhs.config->solid_infill_acceleration
            || config->solid_infill_speed != rhs.config->solid_infill_speed
            || config->top_solid_infill_acceleration != rhs.config->top_solid_infill_acceleration
            || config->top_solid_infill_speed != rhs.config->top_solid_infill_speed
            || config->default_acceleration != rhs.config->default_acceleration
            || config->default_speed != rhs.config->default_speed
            || config->bridge_acceleration != rhs.config->bridge_acceleration
            || config->bridge_speed != rhs.config->bridge_speed
            || config->internal_bridge_acceleration != rhs.config->internal_bridge_acceleration
            || config->internal_bridge_speed != rhs.config->internal_bridge_speed
            || config->gap_fill_acceleration != rhs.config->gap_fill_acceleration
            || config->gap_fill_speed != rhs.config->gap_fill_speed
            || config->print_extrusion_multiplier != rhs.config->print_extrusion_multiplier
            || config->region_gcode != rhs.config->region_gcode
            || config->small_area_infill_flow_compensation_model.is_enabled() != rhs.config->small_area_infill_flow_compensation_model.is_enabled()
            || config->small_area_infill_flow_compensation_model != rhs.config->small_area_infill_flow_compensation_model
            || config->solid_infill_extruder != rhs.config->solid_infill_extruder
            || config->wipe_into_infill != rhs.config->wipe_into_infill
            // print modifier, because region are fused in gode wiew if not.
            || config->print_extrusion_multiplier        != rhs.config->print_extrusion_multiplier
            || config->print_first_layer_temperature     != rhs.config->print_first_layer_temperature
            || config->print_retract_length              != rhs.config->print_retract_length
            || config->print_retract_lift                != rhs.config->print_retract_lift
            || config->print_temperature                 != rhs.config->print_temperature
            // end print modifier
            ))
            return false;
        if (config != nullptr && (this->pattern == ipConcentric || rhs.pattern == ipConcentric) &&
            // arachne if concentric
            (config->perimeter_generator != rhs.config->perimeter_generator
            || config->min_bead_width != rhs.config->min_bead_width
            || config->min_feature_size != rhs.config->min_feature_size
            || config->wall_distribution_count != rhs.config->wall_distribution_count
            || config->wall_transition_length != rhs.config->wall_transition_length
            || config->wall_transition_filter_deviation != rhs.config->wall_transition_filter_deviation
            || config->wall_transition_angle != rhs.config->wall_transition_angle))
            return false;
        // then check params
        return  this->extruder              == rhs.extruder         &&
                this->pattern               == rhs.pattern          &&
                this->spacing               == rhs.spacing          &&
//                this->overlap               == rhs.overlap          &&
                this->angle                 == rhs.angle            &&
                this->can_angle_cross        == rhs.can_angle_cross   &&
                this->bridge_type           == rhs.bridge_type      &&
                this->density               == rhs.density          &&
                this->monotonic             == rhs.monotonic        &&
                this->connection            == rhs.connection       &&
                this->add_gap_fill          == rhs.add_gap_fill     &&
                this->dont_adjust           == rhs.dont_adjust      &&
                this->anchor_length         == rhs.anchor_length    &&
                this->anchor_length_max     == rhs.anchor_length_max&&
                this->fill_exactly          == rhs.fill_exactly     &&
                this->flow                  == rhs.flow             &&
                this->role                  == rhs.role             &&
                this->max_sparse_infill_spacing == rhs.max_sparse_infill_spacing &&
                this->priority              == rhs.priority;
    }
};

struct SurfaceFill {
    SurfaceFill(const SurfaceFillParams& params) : /*region_id(size_t(-1)),*/ surface(stNone, ExPolygon()), params(params) {}

    //size_t               region_id;
    Surface              surface;
    ExPolygons           expolygons;
    SurfaceFillParams    params;
    LayerRegionSetCPtrs regions;
};

float compute_fill_angle(const PrintRegionConfig &region_config, size_t layer_id)
{
    float angle = 0;
    if (!region_config.fill_angle_template.empty()) {
        // fill pattern: replace fill angle
        size_t idx   = layer_id % region_config.fill_angle_template.size();
        angle = region_config.fill_angle_template.get_at(idx);
    } else {
        angle = region_config.fill_angle.value;
    }
    angle += region_config.fill_angle_increment.value * layer_id;
    // make compute in degre, then normalize and convert into rad.
    angle = float(Geometry::deg2rad(angle));
    
    return angle;
}

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
void export_group_fills_to_svg(const char *path, const std::vector<SurfaceFill> &fills)
{
    BoundingBox bbox;
    for (const auto &fill : fills)
        for (const auto &expoly : fill.expolygons)
            bbox.merge(get_extents(expoly));
    Point legend_size = export_surface_type_legend_to_svg_box_size();
    Point legend_pos(bbox.min(0), bbox.max(1));
    bbox.merge(Point(std::max(bbox.min(0) + legend_size(0), bbox.max(0)), bbox.max(1) + legend_size(1)));

    SVG svg(path, bbox);
    const float transparency = 0.5f;
    for (const auto &fill : fills)
        for (const auto &expoly : fill.expolygons)
            svg.draw(expoly, surface_type_to_color_name(fill.surface.surface_type), transparency);
    export_surface_type_legend_to_svg(svg, legend_pos);
    svg.Close(); 
}
#endif

std::vector<SurfaceFill> group_fills(const Layer &layer, const LayerSliceIsland& island)
{
    std::vector<SurfaceFill> surface_fills;
    std::vector<const LayerRegion*> ordered_regions(island.regions().begin(), island.regions().end());
    // Fill in a map of a region & surface to SurfaceFillParams.
    std::set<SurfaceFillParams>                         set_surface_params;
    std::vector<std::vector<const SurfaceFillParams*>>  region_to_surface_params(ordered_regions.size(), std::vector<const SurfaceFillParams*>());
    bool                                                has_internal_voids = false;
    for (size_t region_id = 0; region_id < ordered_regions.size(); ++ region_id) {
        const LayerRegion  &layerm = *ordered_regions[region_id];
        region_to_surface_params[region_id].assign(layerm.fill_surfaces().size(), nullptr);
        for (size_t idx_fill_srf=0; idx_fill_srf < layerm.fill_surfaces().size(); ++idx_fill_srf) {
            const Surface &surface = layerm.fill_surfaces().at(idx_fill_srf);
            SurfaceFillParams params;
            if (surface.surface_type == (stPosInternal | stDensVoid)) {
                has_internal_voids = true;
            } else {
                const PrintRegionConfig &region_config = layerm.region().config();
                FlowRole extrusion_role = surface.has_pos_top() ? frTopSolidInfill : (surface.has_fill_solid() ? frSolidInfill : frInfill);
                bool     is_bridge      = layer.id() > 0 && surface.has_mod_bridge();
                bool     is_denser      = false;
                params.extruder         = layerm.region().extruder(extrusion_role, *layer.object());
                params.pattern          = region_config.fill_pattern.value;
                params.density          = float(region_config.fill_density) / 100.f;
                params.dont_adjust      = false;
                params.connection       = region_config.infill_connection.value;
                params.priority         = 0;

                if (surface.has_fill_solid()) {
                    params.density = 1.f;
                    params.pattern = region_config.solid_fill_pattern.value;
                    params.connection = region_config.infill_connection_solid.value;
                    params.add_gap_fill = region_config.infill_filled_solid.value;
                    if (surface.has_pos_top()) {
                        params.pattern = region_config.top_fill_pattern.value;
                        params.connection = region_config.infill_connection_top.value;
                        params.add_gap_fill = region_config.infill_filled_top.value;
                    }
                    if (surface.has_pos_bottom()) {
                        params.pattern = region_config.bottom_fill_pattern.value;
                        params.connection = region_config.infill_connection_bottom.value;
                        params.add_gap_fill = region_config.infill_filled_bottom.value;
                    }
                    //FIXME for non-thick bridges, shall we allow a bottom surface pattern?
                    if (is_bridge) {
                        params.pattern = region_config.bridge_fill_pattern.value;
                        params.connection = region_config.infill_connection_bridge.value;
                        params.add_gap_fill = false;
                        params.bridge_type = region_config.bridge_type.value;
                    }
                } else {
                    if (is_bridge) {
                        params.pattern = region_config.bridge_fill_pattern.value;
                        params.connection = region_config.infill_connection_bridge.value;
                    }
                    if (region_config.infill_dense.get_bool()
                        && region_config.fill_density < 40
                        && surface.maxNbSolidLayersOnTop == 1) {
                        assert(surface.has(stPosInternal | stDensSparse | stModBridge));
                        params.density = 0.42f;
                        is_denser = true;
                        is_bridge = true;
                        params.pattern = ipRectiWithPerimeter;
                        params.priority = surface.priority;
                        params.dont_adjust = true; // keep the 42% density
                        params.connection = InfillConnection::icConnected;
                    } else {
                        assert(region_config.fill_density >= 40 || !surface.has(stPosInternal | stDensSparse | stModBridge));
                    }
                    if (params.density <= 0 && !is_denser)
                        continue;
                }
                //adjust spacing/density (to over-extrude when needed)
                if (surface.has_mod_overBridge()) {
                    params.density = float(region_config.over_bridge_flow_ratio.get_effective_value(1));
                }

                //note: same as getRoleFromSurfaceType()
                params.role = ExtrusionRole::InternalInfill;
                if (is_bridge) {
                    if(surface.has_pos_bottom())
                        params.role = ExtrusionRole::BridgeInfill;
                    else
                        params.role = ExtrusionRole::InternalBridgeInfill;
                } else if (surface.has_fill_solid()) {
                    if (surface.has_pos_top()) {
                        params.role = ExtrusionRole::TopSolidInfill;
                    } else {
                        params.role = ExtrusionRole::SolidInfill;
                    }
                }
                params.fill_exactly = region_config.enforce_full_fill_volume.get_bool();
                params.bridge_angle = float(surface.bridge_angle);
                params.angle         = (is_denser) ? 0 : compute_fill_angle(region_config, layer.id());
                params.can_angle_cross = region_config.fill_angle_cross;
                params.anchor_length = std::min(params.anchor_length, params.anchor_length_max);

                //adjust flow (to over-extrude when needed)
                params.flow_mult = 1;
                if (surface.has_pos_top())
                    params.flow_mult *= float(region_config.fill_top_flow_ratio.get_effective_value(1));

                params.config = &layerm.region().config();

                // Calculate the actual flow we'll be using for this infill.
                // params.bridge = is_bridge || Fill::use_bridge_flow(params.pattern);
                // params.flow   = params.bridge ?
                //     // Always enable thick bridges for internal bridges.
                //     layerm.bridging_flow(extrusion_role, surface.is_bridge() && ! surface.is_external()) :
                //     layerm.flow(extrusion_role, (surface.thickness == -1) ? layer.height : surface.thickness);
                if (is_bridge) {
                    float nozzle_diameter = layer.object()->print()->config().nozzle_diameter.get_at(layerm.region().extruder(extrusion_role, *layer.object()) - 1);
                    double diameter = 0;
                    if (region_config.bridge_type == BridgeType::btFromFlow) {
                        Flow reference_flow = layerm.flow(FlowRole::frSolidInfill);
                        diameter = sqrt(4 * reference_flow.mm3_per_mm() / PI);
                    } else if (region_config.bridge_type == BridgeType::btFromHeight) {
                        diameter = layer.unscaled_height();
                    } else /*if (region_config.bridge_type == BridgeType::btFromNozzle)*/ {
                        diameter = nozzle_diameter;
                    }
                    params.flow = Flow::bridging_flow((float)(diameter * std::sqrt(region_config.bridge_flow_ratio.get_effective_value(1))), nozzle_diameter);
                } else {
                    params.flow = layerm.region().flow(
                        *layer.object(),
                        extrusion_role,
                        (float)(surface.scaled_thickness() == -1) ? layer.unscaled_height() : surface.unscaled_thickness(),   // extrusion height
                        layer.id()
                    );
                }
                
                // Calculate flow spacing for infill pattern generation.
                if (surface.has_fill_solid() || is_bridge) {
                    params.spacing = params.flow.spacing();
                    // Don't limit anchor length for solid or bridging infill.
                    // use old algo for bridging to prevent possible weird stuff from 'new' connections optimized for sparse stuff.
                    params.anchor_length = is_bridge ? 0 : 1000.f;
                    params.anchor_length_max = is_bridge ? 0 : 1000.f;
                } else {
                    //FIXME FLOW decide what to use
                    // Internal infill. Calculating infill line spacing independent of the current layer height and 1st layer status,
                    // so that internall infill will be aligned over all layers of the current region.
                    //params.spacing = layerm.region().flow(*layer.object(), frInfill, layer.heigh, false).spacing();
                    // it's internal infill, so we can calculate a generic flow spacing 
                    // for all layers, for avoiding the ugly effect of
                    // misaligned infill on first layer because of different extrusion width and
                    // layer height
                    Flow infill_flow = layerm.region().flow(
                            *layer.object(),
                            frInfill,
                            (float)layer.unscaled_height(),  // TODO: handle infill_every_layers?
                            layer.id()
                        );
                    params.spacing = infill_flow.spacing();

                    // Anchor a sparse infill to inner perimeters with the following anchor length:
                    params.anchor_length = float(region_config.infill_anchor);
                    if (region_config.infill_anchor.percent)
                        params.anchor_length = float(params.anchor_length * 0.01 * params.spacing);
                    params.anchor_length_max = float(region_config.infill_anchor_max);
                    if (region_config.infill_anchor_max.percent)
                        params.anchor_length_max = float(params.anchor_length_max * 0.01 * params.spacing);
                    params.anchor_length = std::min(params.anchor_length, params.anchor_length_max);
                    
                    //sparse infill, compute the max width if needed
                    if (region_config.fill_aligned_z) {
                        //don't use fill_aligned_z if the pattern can't use it.
                        if (params.pattern != ipHilbertCurve && params.pattern != ipArchimedeanChords &&
                            params.pattern != ipOctagramSpiral && params.pattern != ipScatteredRectilinear &&
                            params.pattern != ipLightning) {
                            params.max_sparse_infill_spacing = unscaled(layer.object()->get_sparse_max_spacing());
                        }
                    }
                }

                auto it_params = set_surface_params.find(params);
                if (it_params == set_surface_params.end()) {
                    it_params = set_surface_params.insert(it_params, params);
                }
                region_to_surface_params[region_id][idx_fill_srf] = &(*it_params);
            }
        }
    }

    surface_fills.reserve(set_surface_params.size());
    for (const SurfaceFillParams &params : set_surface_params) {
        const_cast<SurfaceFillParams&>(params).idx = surface_fills.size();
        surface_fills.emplace_back(params);
    }
    for (size_t region_id = 0; region_id < ordered_regions.size(); ++region_id) {
        const LayerRegion &layerm = *ordered_regions[region_id];
        for (const Surface &surface : layerm.fill_surfaces()) {
            surface.expolygon.assert_valid();
        }
    }

    for (size_t region_id = 0; region_id < ordered_regions.size(); ++ region_id) {
        const LayerRegion &layerm = *ordered_regions[region_id];
        for (const Surface &surface : layerm.fill_surfaces()) {
            // if a fill is needed for this island in this surface.
            if (surface.surface_type != (stPosInternal | stDensVoid) &&
                island.is_expolygons_from_region(surface.expolygon)) {
                const SurfaceFillParams *params =
                    region_to_surface_params[region_id][&surface - &layerm.fill_surfaces().surfaces.front()];
                if (params != nullptr) {
                    SurfaceFill &fill = surface_fills[params->idx];
                    if (fill.regions.empty()) {
                        // fill.region_id = region_id;
                        fill.surface = surface;
                        fill.surface.expolygon.assert_valid();
                        fill.expolygons.emplace_back(std::move(fill.surface.expolygon));
                        fill.regions.insert(&layerm);
                    } else {
                        surface.expolygon.assert_valid();
                        fill.expolygons.emplace_back(surface.expolygon);
                        fill.regions.insert(&layerm);
                    }
                }
            }
        }
    }
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    {
        static int iRun = 0;
        export_group_fills_to_svg(debug_out_path("Layer-fill_surfaces-2_fill-in_progress-%d.svg", iRun ++).c_str(), surface_fills);
    }
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */

    // merge polygons and ensure no fill overlap.
    {
        ExPolygons all_expolygons;
        for (SurfaceFill &fill : surface_fills) {
            const coord_t resolution = std::min(fill.params.flow.scaled_width() / 16, 
                std::max(SCALED_EPSILON, scale_i(layer.object()->print()->config().resolution_internal.value)));
            assert_valid(fill.expolygons);
            // note: Bridges are processed first (see SurfaceFill::operator<())
            if (!fill.expolygons.empty()) {
                if (fill.expolygons.size() > 1) {
                    // ensure it's fused (should be union_safety_offset_ex, but something in slicing set bridges area farther apart than normal).
                    fill.expolygons = offset2_ex(fill.expolygons, fill.params.flow.scaled_width() / 8, -fill.params.flow.scaled_width() / 8);
                    // need safety thing or there is self-interscting things (may use offset_remove_narrow instead of offset2_ex)
                    fill.expolygons = union_safety_offset_ex(fill.expolygons);
                    ensure_valid(fill.expolygons, resolution);
                }
                if (fill.params.priority > 0) {
                    //allow infill overlap if priority is not 0, as it's for dense infill.
                    all_expolygons = union_ex(all_expolygons, fill.expolygons);
                } else if (fill.expolygons.size() > 1 || !all_expolygons.empty()) {
                    assert_valid(fill.expolygons);
                    // subtract the preceding polygons, to avoid overlapping infills.
                    if (!all_expolygons.empty()) {
                        fill.expolygons = diff_ex(fill.expolygons, all_expolygons, ApplySafetyOffset::Yes);
                        ensure_valid(fill.expolygons, resolution);
                    }
                    all_expolygons = union_ex(all_expolygons, fill.expolygons);
                } else if (&fill != &surface_fills.back()) {
                    //still add it to all_expolygons for the next surface_fill
                    all_expolygons = union_ex(all_expolygons, fill.expolygons);
                }
            }
            assert_valid(fill.expolygons);
        }
    }
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    {
        static int iRun = 0;
        export_group_fills_to_svg(debug_out_path("Layer-fill_surfaces-3_fill-in_progress-%d.svg", iRun ++).c_str(), surface_fills);
    }
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */

    // we need to detect any narrow surfaces that might collapse
    // when adding spacing below
    // such narrow surfaces are often generated in sloping walls
    // by bridge_over_infill() and combine_infill() as a result of the
    // subtraction of the combinable area from the layer infill area,
    // which leaves small areas near the perimeters
    // we are going to grow such regions by overlapping them with the void (if any)
    // TODO: detect and investigate whether there could be narrow regions without
    // any void neighbors
    if (has_internal_voids) {
        // Internal voids are generated only if "infill_only_where_needed" or "infill_every_layers" are active.
        coord_t  distance_between_surfaces = 0;
        Polygons surfaces_polygons;
        Polygons voids;
        LayerRegionSetCPtrs regions_internal_infill;
        LayerRegionSetCPtrs regions_solid_infill;
        LayerRegionSetCPtrs regions_some_infill;
        for (SurfaceFill &surface_fill : surface_fills)
            if (! surface_fill.expolygons.empty()) {
                distance_between_surfaces = std::max(distance_between_surfaces, surface_fill.params.flow.scaled_spacing());
                append((surface_fill.surface.surface_type == (stPosInternal | stDensVoid)) ? voids : surfaces_polygons, to_polygons(surface_fill.expolygons));
                if (surface_fill.surface.surface_type == (stPosInternal | stDensSolid))
                    regions_internal_infill = surface_fill.regions;
                if (surface_fill.surface.has_fill_solid())
                    regions_solid_infill = surface_fill.regions;
                if (surface_fill.surface.surface_type != (stPosInternal | stDensVoid))
                    regions_some_infill = surface_fill.regions;
            }
        if (! voids.empty() && ! surfaces_polygons.empty()) {
            // First clip voids by the printing polygons, as the voids were ignored by the loop above during mutual clipping.
            voids = diff(voids, surfaces_polygons);
            // Corners of infill regions, which would not be filled with an extrusion path with a radius of distance_between_surfaces/2
            Polygons collapsed = diff(
                surfaces_polygons,
                //offset2(surfaces_polygons, (float)-distance_between_surfaces/2, (float)+distance_between_surfaces/2),
                opening(surfaces_polygons, float(distance_between_surfaces / 2), float(distance_between_surfaces / 2 + ClipperSafetyOffset)),
                ApplySafetyOffset::Yes);
            //FIXME why the voids are added to collapsed here? First it is expensive, second the result may lead to some unwanted regions being
            // added if two offsetted void regions merge.
            // polygons_append(voids, collapsed);
            //ExPolygons extensions = intersection_ex(offset(collapsed, (float)distance_between_surfaces), voids, true);
            ExPolygons extensions = intersection_ex(expand(collapsed, float(distance_between_surfaces)), voids, ApplySafetyOffset::Yes);
            // Now find an internal infill SurfaceFill to add these extrusions to.
            SurfaceFill *internal_solid_fill = nullptr;
            LayerRegionSetCPtrs *regions = nullptr;
            if (!regions_internal_infill.empty())
                regions = &regions_internal_infill;
            else if (!regions_solid_infill.empty())
                regions = &regions_solid_infill;
            else if (!regions_some_infill.empty())
                regions = &regions_some_infill;
            for (SurfaceFill &surface_fill : surface_fills) {
                if (surface_fill.surface.surface_type == (stPosInternal | stDensVoid) &&
                    std::abs(layer.unscaled_height() - surface_fill.params.flow.height()) < EPSILON) {
                    internal_solid_fill = &surface_fill;
                    break;
                }
            }
            if (internal_solid_fill == nullptr) {
                // get one of them, ok as extruder, fill_angle & solid_fill_pattern must always be the same.
                const LayerRegion *one_layerm = *regions->begin();
                // Produce another solid fill.
                SurfaceFillParams params;
                params.extruder = one_layerm->region().extruder(frSolidInfill, *layer.object());
                params.pattern  = one_layerm->region().config().solid_fill_pattern.value;
                params.density  = 100.f;
                params.role     = ExtrusionRole::InternalInfill;
                params.angle    = compute_fill_angle(one_layerm->region().config(), layer.id());
                //FIXME FLOW decide what to use
                //params.flow = one_layerm->flow(frSolidInfill);
                // calculate the actual flow we'll be using for this infill
                params.flow = one_layerm->region().flow(
                    *layer.object(),
                    frSolidInfill,
                    (float)layer.unscaled_height(),         // extrusion height
                    layer.id()
                );
                params.spacing = params.flow.spacing();            
                surface_fills.emplace_back(params);
                surface_fills.back().surface.surface_type = (stPosInternal | stDensSolid);
                surface_fills.back().surface.set_scaled_thickness(layer.scaled_height());
                assert_valid(extensions);
                surface_fills.back().expolygons = std::move(extensions);
                surface_fills.back().regions = *regions;
            } else {
                //append(extensions, std::move(internal_solid_fill->expolygons));
                //assert_valid(extensions);
                //internal_solid_fill->expolygons = union_ex(extensions);
                internal_solid_fill->expolygons = union_ex(extensions, internal_solid_fill->expolygons);
                assert_valid(internal_solid_fill->expolygons);
            }
        }
    }

    // Use ipEnsuring pattern for all internal Solids. TODO remove after merge.
    /*{
        for (size_t surface_fill_id = 0; surface_fill_id < surface_fills.size(); ++surface_fill_id)
            if (SurfaceFill &fill = surface_fills[surface_fill_id]; fill.surface.surface_type == stInternalSolid) {
                fill.params.pattern = ipEnsuring;
            }
    }*/
    // remove empty fills
    for (size_t i = 0; i < surface_fills.size(); i++) {
        if (surface_fills[i].expolygons.empty()) {
            surface_fills.erase(surface_fills.begin() + i);
            i--;
        }
    }

    //union with safety offset to avoid separation from the appends of different surface with same settings.
    for (auto &surface_fill : surface_fills) {
        surface_fill.expolygons = union_safety_offset_ex(surface_fill.expolygons);
        //assert_valid(surface_fill.expolygons); //TODO: uncomment when union_safety_offset_ex will be improve
        //simplify (also, it's possible rn that some point are below EPSILON distance).
        //ensure_valid(surface_fill.expolygons, surface_fill.params.fill_resolution);
        ensure_valid(surface_fill.expolygons);
        surface_fill.expolygons = simplify_polygons_ex(to_polygons(surface_fill.expolygons));
        ensure_valid(surface_fill.expolygons);
        //assert_valid(surface_fill.expolygons); //TODO: uncomment when union_safety_offset_ex will be improve
    }

    return surface_fills;
}

void Layer::clear_fills()
{
    for (LayerSliceIslandUPtr &layer_island_ptr : m_islands) {
        for (LayerRegionIsland &region_island_ptr : layer_island_ptr->regions_islands()) {
            region_island_ptr.m_extrusion_regions.erase(LayerRegionIsland::INFILLS);
            region_island_ptr.m_extrusion_regions.erase(LayerRegionIsland::IRONINGS);
        }
    }
}

void Layer::make_fills(FillAdaptive::Octree *adaptive_fill_octree,
                       FillAdaptive::Octree *support_fill_octree,
                       FillLightning::Generator *lightning_generator) {
    this->clear_fills();

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    this->export_region_fill_surfaces_to_svg_debug("10_fill-initial");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */

    for (LayerSliceIslandUPtr &layer_island_ptr : m_islands) {
        this->_make_fills(*layer_island_ptr, adaptive_fill_octree, support_fill_octree, lightning_generator);
    }
}

void Layer::_make_fills(LayerSliceIsland& island,
                        FillAdaptive::Octree *adaptive_fill_octree,
                        FillAdaptive::Octree *support_fill_octree,
                        FillLightning::Generator *lightning_generator)
{
    std::vector<SurfaceFill> surface_fills   = group_fills(*this, island);
    if (surface_fills.empty()) {
        return;
    }
    const Slic3r::BoundingBox bbox           = this->object()->bounding_box();

    //sort by priority. Needed for dense support, as the order is very important.
    std::sort(surface_fills.begin(), surface_fills.end(), [](SurfaceFill& s1, SurfaceFill& s2) {
        if (s1.regions == s2.regions)
            return s1.params.priority < s2.params.priority;
        return s1.regions < s2.regions;
        });

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    {
        static int iRun = 0;
        export_group_fills_to_svg(debug_out_path("Layer-fill_surfaces-10_fill-final-%d.svg", iRun ++).c_str(), surface_fills);
    }
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
    std::vector<std::vector<std::pair<const SurfaceFill*, std::unique_ptr<ExtrusionEntityCollection>>>> fills_by_priority;
    auto store_fill = [&fills_by_priority, &island, this](LayerRegionSetCPtrs regions) {
        if (fills_by_priority.empty()) {
            assert(false);//not possible
        }
        int nb_sparse = 0;
        int nb_solid = 0;
        int nb_others = 0;
        for (std::vector<std::pair<const SurfaceFill *, std::unique_ptr<ExtrusionEntityCollection>>>
                 &fills_same_prio : fills_by_priority) {
            for (std::pair<const SurfaceFill *, std::unique_ptr<ExtrusionEntityCollection>> &ecc : fills_same_prio) {
                if (ecc.first->surface.has_fill_sparse()) {
                    nb_sparse++;
                } else if (ecc.first->surface.has_fill_solid()) {
                    nb_solid++;
                } else {
                    nb_others++;
                }
            }
        }
        assert(nb_others == 0);
        // find our LayerRegionIsland or create it
        assert(!regions.empty());
        int16_t sparse_fill_extruder = int16_t((*regions.begin())->region().config().infill_extruder.value - 1);
        int16_t solid_fill_extruder = int16_t((*regions.begin())->region().config().solid_infill_extruder.value - 1);
        assert(sparse_fill_extruder >= 0);
        assert(solid_fill_extruder >= 0);
        LayerRegionIsland *region_island_sparse = nb_sparse == 0 ?
            nullptr :
            &island.get_or_add_region_island(regions, uint16_t(sparse_fill_extruder));
        LayerRegionIsland *region_island_solid = nb_solid == 0 ?
            nullptr :
            &island.get_or_add_region_island(regions, uint16_t(solid_fill_extruder));
        assert(!region_island_sparse || !region_island_sparse->has_extrusion(LayerRegionIsland::INFILLS));
        assert(!region_island_solid || !region_island_solid->has_extrusion(LayerRegionIsland::INFILLS));
        ExtrusionEntityCollection *root_sparse = region_island_sparse ?
            &region_island_sparse->mutable_extrusion(LayerRegionIsland::INFILLS) :
            nullptr;
        ExtrusionEntityCollection *root_solid = region_island_solid ?
            &region_island_solid->mutable_extrusion(LayerRegionIsland::INFILLS) :
            nullptr;
        if (fills_by_priority.size() > 1) {
            // multiple priority, create an intermediate collection
            if (root_sparse) {
                root_sparse->set_can_sort_reverse(false, false);
                assert(root_sparse->empty());
            }
            if (root_solid) {
                root_solid->set_can_sort_reverse(false, false);
                assert(root_solid->empty());
            }
            // note: it's a bit complicated because can be  root_sparse == root_solid
            for (std::vector<std::pair<const SurfaceFill*, std::unique_ptr<ExtrusionEntityCollection>>> &fills_same_prio : fills_by_priority) {
                if (fills_same_prio.size() == 1) {
                    if (!fills_same_prio.front().second->empty()) {
                        assert(fills_same_prio.front().first->surface.has_fill_sparse() ? root_sparse : root_solid);
                        (fills_same_prio.front().first->surface.has_fill_sparse() ? root_sparse : root_solid)->append(fills_same_prio.front().second);
                    }
                } else if (fills_same_prio.size() > 1) {
                    ExtrusionEntityCollection eec_same_prio_sparse;
                    ExtrusionEntityCollection eec_same_prio_solid;
                    for (std::pair<const SurfaceFill*, std::unique_ptr<ExtrusionEntityCollection>> &ecc : fills_same_prio) {
                        if (!ecc.second->empty()) {
                            // if sparse_fill_extruder == solid_fill_extruder, force to go in the same collection
                            ((ecc.first->surface.has_fill_sparse() || root_solid == root_sparse) ?
                                 eec_same_prio_sparse :
                                 eec_same_prio_solid)
                                .append(ecc.second);
                        }
                    }
                    assert(!eec_same_prio_sparse.empty() || !eec_same_prio_solid.empty());
                    if (!eec_same_prio_sparse.empty()) {
                        if (root_sparse) {
                            root_sparse->append(eec_same_prio_sparse);
                        } else {
                            assert(root_solid);
                            // if sparse_fill_extruder == solid_fill_extruder and nb_sparse == 0
                            root_solid->append(eec_same_prio_sparse);
                        }
                    }
                    if (!eec_same_prio_solid.empty()) {
                        assert(root_solid);
                        root_solid->append(eec_same_prio_solid);
                    }
                }
            }
            assert(!root_sparse || !root_sparse->empty());
            assert(!root_solid || !root_solid->empty());
        } else {
            std::vector<std::pair<const SurfaceFill*, std::unique_ptr<ExtrusionEntityCollection>>> &fills_same_prio = fills_by_priority.front();
            if (fills_same_prio.size() == 1) {
                if (fills_same_prio.front().first->surface.has_fill_sparse()) {
                    assert(root_sparse);
                    root_sparse->append_move_from(*fills_same_prio.front().second);
                } else {
                    assert(root_solid);
                    root_solid->append_move_from(*fills_same_prio.front().second);
                }
            } else {
                for (std::pair<const SurfaceFill*, std::unique_ptr<ExtrusionEntityCollection>> &ecc : fills_same_prio) {
                    if (!ecc.second->empty()) {
                        if (ecc.first->surface.has_fill_sparse()) {
                            assert(root_sparse);
                            root_sparse->append(ecc.second);
                        } else {
                            assert(root_solid);
                            root_solid->append(ecc.second);
                        }
                    }
                }
            }
        }
        fills_by_priority.clear();
    };
    //surface_fills is sorted by region_id
    LayerRegionSetCPtrs current_regions;
    uint16_t current_extruder = -1;
    size_t first_object_layer_id = this->object()->layer(0).id();
    for (SurfaceFill &surface_fill : surface_fills) {
        // store the region fill when changing region (if there is something to store in fills_by_priority).
        if (current_regions != surface_fill.regions || current_extruder != surface_fill.params.extruder) {
            if (!current_regions.empty() && !fills_by_priority.empty()) {
                store_fill(current_regions);
            }
            //current_regions.clear();
            //current_regions.insert(current_regions.end(), surface_fill.regions.begin(), surface_fill.regions.end());
            current_regions = surface_fill.regions;
            current_extruder = surface_fill.params.extruder;
        }
        assert(!surface_fill.regions.empty());
        const LayerRegion *layerm_for_flow = *surface_fill.regions.begin();
        const PrintRegionConfig &common_region_config = layerm_for_flow->region().config();

        // Create the filler object.
        std::unique_ptr<Fill> f = std::unique_ptr<Fill>(Fill::new_from_type(surface_fill.params.pattern));
        f->set_bounding_box(bbox);
        // Layer ID is used for orienting the infill in alternating directions.
        // Layer::id() returns layer ID including raft layers, subtract them to make the infill direction independent
        // from raft.
        f->layer_id = this->id() - first_object_layer_id;
        f->z        = this->unscaled_print_z();
        f->angle    = surface_fill.params.angle;
        f->can_angle_cross   = surface_fill.params.can_angle_cross;
        f->adapt_fill_octree = (surface_fill.params.pattern == ipSupportCubic) ? support_fill_octree : adaptive_fill_octree;
        f->set_config(&this->object()->print()->config(), &this->object()->config());

        // add object angle if needed
        double z_object_angle = this->m_object->trafo().rotation().eulerAngles(0,1,2).z();
        if (this->object()->config().fill_angle_follow_model.value) {
            f->angle += z_object_angle;
        }

        if (surface_fill.params.pattern == ipLightning)
            dynamic_cast<FillLightning::Filler*>(f.get())->generator = lightning_generator;

        if (surface_fill.params.pattern == ipEnsuring) {
            auto *fill_ensuring = dynamic_cast<FillEnsuring *>(f.get());
            assert(fill_ensuring != nullptr);
            fill_ensuring->print_region_config = &common_region_config;
        }

        // calculate flow spacing for infill pattern generation
        //FIXME FLOW decide if using surface_fill.params.flow.bridge() or surface_fill.params.bridge (default but deleted)
        bool using_internal_flow = ! surface_fill.surface.has_fill_solid() && !surface_fill.params.flow.bridge();
        //init spacing, it may also use & modify a bit the surface_fill.params, so most of these should be set before.
        // note that the bridge overlap is applied here via the rectilinear init_spacing. 
        f->init_spacing(surface_fill.params.spacing, surface_fill.params);
        double link_max_length = 0.;
        //FIXME FLOW decide if using surface_fill.params.flow.bridge() or surface_fill.params.bridge (default but deleted)
        if (! surface_fill.params.flow.bridge()) {
#if 0
            link_max_length = common_region_config.option(surface.is_external() ? "external_fill_link_max_length" : "fill_link_max_length")->get_effective_value(flow.spacing());
//            printf("flow spacing: %f,  is_external: %d, link_max_length: %lf\n", flow.spacing(), int(surface.is_external()), link_max_length);
#else
            if (surface_fill.params.density > .8) // 80%
                link_max_length = 3. * f->get_spacing();
#endif
        }

        // Maximum length of the perimeter segment linking two infill lines.
        f->link_max_length = scale_i(link_max_length);

        //give the overlap size to let the infill do his overlap
        //add overlap if at least one perimeter
        float perimeter_spacing = 0;
        if(common_region_config.perimeters == 1)
            perimeter_spacing = layerm_for_flow->flow(frExternalPerimeter).spacing();
        else if(common_region_config.only_one_perimeter_top)
            //note: use the min of the two to avoid overextrusion if only one perimeter top
            perimeter_spacing = std::min(layerm_for_flow->flow(frPerimeter).spacing(), layerm_for_flow->flow(frExternalPerimeter).spacing());
        else //if(common_region_config.perimeters > 1)
            perimeter_spacing = layerm_for_flow->flow(frPerimeter).spacing();

        // Used by the concentric infill pattern to clip the loops to create extrusion paths.
        f->loop_clipping = scale_i(common_region_config.get_computed_value("seam_gap", surface_fill.params.extruder - 1) * surface_fill.params.flow.nozzle_diameter());

        // apply half spacing using this flow's own spacing and generate infill
        //FillParams params;
        //params.density           = float(0.01 * surface_fill.params.density);
        //params.dont_adjust       = false; //  surface_fill.params.dont_adjust;
        //params.anchor_length     = surface_fill.params.anchor_length;
        //params.anchor_length_max = surface_fill.params.anchor_length_max;
        //params.resolution        = resolution;
        //params.use_arachne       = (perimeter_generator == PerimeterGeneratorType::Arachne && surface_fill.params.pattern == ipConcentric) || surface_fill.params.pattern == ipEnsuring;
        //params.layer_height      = this->height;
        surface_fill.params.fill_resolution = std::min(surface_fill.params.flow.scaled_width() / 16, 
            std::max(SCALED_EPSILON, scale_i(this->object()->print()->config().resolution_internal.value)));


        //store default values, before modification.
        bool dont_adjust = surface_fill.params.dont_adjust;
        float density = surface_fill.params.density;
        assert(!surface_fill.expolygons.empty());
        for (ExPolygon &expoly : surface_fill.expolygons) {
            //set overlap polygons
            f->no_overlap_expolygons.clear();
            if (surface_fill.params.config->perimeters > 0) {
                f->overlap = surface_fill.params.config->infill_overlap.get_effective_value((perimeter_spacing + (f->get_spacing())) / 2);
                if (f->overlap != 0) {
                    f->no_overlap_expolygons = intersection_ex(island.infill_free_areas(), ExPolygons() = {expoly});
                } else {
                    f->no_overlap_expolygons.push_back(expoly);
                }
            } else {
                f->overlap = 0;
                f->no_overlap_expolygons.push_back(expoly);
            }

            //set default param (that can be modified by bridge thing)
            surface_fill.params.dont_adjust = dont_adjust;
            surface_fill.params.bridge_offset = 0;
            surface_fill.params.density = density;
            surface_fill.params.layer_height = float(this->unscaled_height());
            surface_fill.params.use_arachne   = (common_region_config.perimeter_generator == PerimeterGeneratorType::Arachne &&
                                               surface_fill.params.pattern == ipConcentric) ||
                                              surface_fill.params.pattern == ipEnsuring;

            //init the surface with the current polygon
            if (!expoly.contour.empty()) {
                surface_fill.surface.expolygon = std::move(expoly);

                //adjust the bridge density
                if (surface_fill.params.flow.bridge() && surface_fill.params.density > 0.99 /*&& common_region_config.bridge_overlap.get_effective_value(1) != 1*/) {
                    // bridge have their own spacing, don't try to align it with normal infill.
                    surface_fill.params.max_sparse_infill_spacing = 0;
                    ////varies the overlap to have the best coverage for the bridge
                    //surface_fill.params.density *= float(common_region_config.bridge_overlap.get_effective_value(1));
                    double min_spacing = 0.999 * surface_fill.params.spacing / surface_fill.params.config->bridge_overlap.get_effective_value(surface_fill.params.density);
                    double max_spacing = 1.001 * surface_fill.params.spacing / surface_fill.params.config->bridge_overlap_min.get_effective_value(surface_fill.params.density);
                    double factor = 1.00001;
                    if (min_spacing < max_spacing * 1.01) {
                        // create a bouding box of the rotated surface
                        coord_t bounding_box_size_x = 0;
                        coord_t bounding_box_min_x = 0;
                        ExPolygons expolys;
                        if (surface_fill.params.bridge_angle > 0 && !f->no_overlap_expolygons.empty()) {
                            //take only the no-overlap area
                            expolys = offset_ex(intersection_ex(ExPolygons{ ExPolygon{surface_fill.surface.expolygon.contour} }, f->no_overlap_expolygons), -scale_i(surface_fill.params.spacing) / 2 - 10);
                        } else {
                            expolys = offset_ex(ExPolygon{surface_fill.surface.expolygon.contour}, -scale_i(surface_fill.params.spacing) / 2 - 10);
                        }
                        // if nothing after collapse, then go to next surface_fill.expolygon
                        if (expolys.empty()) {
                            continue;
                        }

                        BoundingBox bb;
                        bool first = true;
                        for (ExPolygon& expoly : expolys) {
                            expoly.holes.clear();
                            expoly.rotate(PI / 2 + (surface_fill.params.bridge_angle < 0 ? surface_fill.params.angle : surface_fill.params.bridge_angle));
                            if (first) {
                                bb = expoly.contour.bounding_box();
                                first = false;
                            } else {
                                bb.merge(expoly.contour.points);
                            }
                        }
                        bounding_box_size_x = bb.size().x();
                        bounding_box_min_x = bb.min.x();

                        //compute the dist
                        double new_spacing = unscaled(f->_adjust_solid_spacing(bounding_box_size_x, scale_i(min_spacing), 2));
                        if (new_spacing <= max_spacing) {
                            surface_fill.params.density = factor * surface_fill.params.spacing / new_spacing;
                        } else {
                            double new_spacing2 = unscaled(f->_adjust_solid_spacing(bounding_box_size_x, scale_i(min_spacing * 1.999 - new_spacing), 2));
                            if (new_spacing2 < min_spacing) {
                                if (min_spacing - new_spacing2 < new_spacing - max_spacing) {
                                    surface_fill.params.density = surface_fill.params.config->bridge_overlap.get_effective_value(surface_fill.params.density);
                                } else {
                                    surface_fill.params.density = surface_fill.params.config->bridge_overlap_min.get_effective_value(surface_fill.params.density);
                                }
                            } else {
                                //use the highest density
                                surface_fill.params.density = surface_fill.params.config->bridge_overlap.get_effective_value(surface_fill.params.density);
                            }
                        }
                        Polygon poly = surface_fill.surface.expolygon.contour;
                        poly.rotate(PI / 2 + (surface_fill.params.bridge_angle < 0 ? surface_fill.params.angle : surface_fill.params.bridge_angle));
                        surface_fill.params.dont_adjust = true;
                        surface_fill.params.bridge_offset = std::abs(poly.bounding_box().min.x() - bounding_box_min_x);
                    }
                }

                //make fill
                while ((size_t) surface_fill.params.priority >= fills_by_priority.size()) {
                    fills_by_priority.emplace_back();
                }
                // note: fills_by_priority[idx] is a vector that store all the entities of this priority, but that can be in multiple islands
                //       the collection in fills_by_priority[idx][idx2) can be reordered, so put evrythgin in a new unorderable collection if it'ts needed
                fills_by_priority[(size_t)surface_fill.params.priority].emplace_back(&surface_fill, new ExtrusionEntityCollection());
                f->fill_surface_extrusion(&surface_fill.surface, surface_fill.params, *fills_by_priority[(size_t)surface_fill.params.priority].back().second);
                // normalize result, just in case the filling algorihtm is messing things up (some are).
                fills_by_priority[(size_t)surface_fill.params.priority].back().second->visit(normalize_visitor);
#if _DEBUG
                //check no over or underextrusion if fill_exactly
                if(surface_fill.params.fill_exactly && surface_fill.params.density == 1 && !surface_fill.params.flow.bridge()) {
                    ExtrusionVolume compute_volume;
                    ExtrusionVolume compute_volume_no_gap_fill;
                    compute_volume_no_gap_fill.set_use_gap_fill(false);
                    double ratio = 1.;
                    if (surface_fill.params.flow.spacing_ratio() != 1) {
                        Flow bigger_flow = Flow::new_from_spacing(surface_fill.params.flow.spacing(), surface_fill.params.flow.nozzle_diameter(), surface_fill.params.flow.height(), 1.f , false);
                        ratio = bigger_flow.mm3_per_mm() / surface_fill.params.flow.mm3_per_mm();
                        compute_volume.set_flow_mult(ratio);
                        compute_volume_no_gap_fill.set_flow_mult(ratio);
                    }
                    //check that it doesn't overextrude
                    for(size_t idx = 0; idx < fills_by_priority[(size_t)surface_fill.params.priority].back().second->size(); ++idx){
                        compute_volume.traverse(*fills_by_priority[(size_t)surface_fill.params.priority].back().second->entities()[idx]);
                        compute_volume_no_gap_fill.traverse(*fills_by_priority[(size_t)surface_fill.params.priority].back().second->entities()[idx]);
                    }
                    ExPolygons temp = f->no_overlap_expolygons.empty() ?
                                        ExPolygons{surface_fill.surface.expolygon} :
                                        intersection_ex(ExPolygons{surface_fill.surface.expolygon}, f->no_overlap_expolygons);
                    double real_surface = 0;
                    for(auto &t : temp) real_surface += t.area();
                    assert(compute_volume.volume < unscaled(unscaled(surface_fill.surface.area())) * surface_fill.params.layer_height * surface_fill.params.flow_mult + EPSILON
                        || f->debug_verify_flow_mult <= 0.80001);
                    double area = unscaled(unscaled(real_surface));
                    if(surface_fill.surface.has_pos_top())
                        area *= surface_fill.params.config->fill_top_flow_ratio.get_effective_value(1);
                    //TODO: over-bridge mod
                    if(surface_fill.params.config->over_bridge_flow_ratio.get_effective_value(1) == 1){
                        assert(compute_volume.volume <= area * surface_fill.params.layer_height * 1.001 ||
                                f->debug_verify_flow_mult <= 0.8);
                        if(compute_volume.volume > 0) //can fail for thin regions
                            assert(
                                compute_volume.volume >= area * surface_fill.params.layer_height * 0.999 ||
                                f->debug_verify_flow_mult >= 1.3 ||
                                f->debug_verify_flow_mult ==
                                    0 // sawtooth output more filament,as it's 3D (debug_verify_flow_mult==0)
                                || area < std::max(1., surface_fill.params.config->solid_infill_below_area.value) ||
                                area < std::max(1., surface_fill.params.config->solid_infill_below_layer_area.value));
                        }
                    }
#endif
            }
        }
    }
    // add last infill if a region is selected and any infill was created in it.
    if (!current_regions.empty() && !fills_by_priority.empty()) {
        store_fill(current_regions);
    }
}

//TODO: 
Polylines Layer::generate_sparse_infill_polylines_for_anchoring(FillAdaptive::Octree *adaptive_fill_octree,
                                                                FillAdaptive::Octree *support_fill_octree,
                                                                FillLightning::Generator *lightning_generator) const {
    Polylines all_polylines;
    for (const LayerSliceIsland &island_ptr : islands()) {
        append(all_polylines,
               this->_generate_sparse_infill_polylines_for_anchoring(island_ptr, adaptive_fill_octree,
                                                                      support_fill_octree, lightning_generator));
    }
    return all_polylines;
}

Polylines Layer::_generate_sparse_infill_polylines_for_anchoring(const LayerSliceIsland &island,
                                                                FillAdaptive::Octree *adaptive_fill_octree,
                                                                FillAdaptive::Octree *support_fill_octree,
                                                                FillLightning::Generator *lightning_generator) const {
    std::vector<SurfaceFill>  surface_fills = group_fills(*this, island);
    const Slic3r::BoundingBox bbox          = this->object()->bounding_box();
    const coord_t             resolution    = std::max(SCALED_EPSILON, scale_i(this->object()->print()->config().resolution_internal.value));

    Polylines sparse_infill_polylines{};

    for (SurfaceFill &surface_fill : surface_fills) {
        // expolygons are copied from surface(s) to surface_fill by group_fills
        assert_valid(surface_fill.expolygons);
        assert(surface_fill.surface.expolygon.empty());
        if (!surface_fill.surface.has(stPosInternal | stDensSparse)) {
            continue;
        }

        switch (surface_fill.params.pattern) {
        case ipCount: continue; break;
        case ipSupportBase: continue; break;
        case ipEnsuring: continue; break;
        case ipSmooth: assert(false); continue; break; // it's supposed to be solid
        case ipLightning:
        case ipAdaptiveCubic:
        case ipSupportCubic:
        case ipRectilinear:
        case ipRectiWithPerimeter:
        case ipSawtooth:
        case ipScatteredRectilinear:
        case ipMonotonic:
        case ipMonotonicLines:
        case ipAlignedRectilinear:
        case ipGrid:
        case ipTriangles:
        case ipStars:
        case ipCubic:
        case ipLine:
        case ipConcentric:
        case ipHoneycomb:
        case ip3DHoneycomb:
        case ipGyroid:
        case ipHilbertCurve:
        case ipArchimedeanChords:
        case ipOctagramSpiral: break;
        default: {
                // all pattern should be in this list
                assert(false);
                continue; break;
            }
        }
        //LayerRegion &layerm = *m_regions[surface_fill.region_id];
        assert(!surface_fill.regions.empty());
        const LayerRegion *layerm_for_flow = *surface_fill.regions.begin();
        const PrintRegionConfig &common_region_config = layerm_for_flow->region().config();

        // Create the filler object.
        std::unique_ptr<Fill> f = std::unique_ptr<Fill>(Fill::new_from_type(surface_fill.params.pattern));
        if (surface_fill.surface.has(stPosInternal | stDensSparse | stModBridge) && surface_fill.params.density < 40) {
            assert(surface_fill.params.config);
            assert(surface_fill.params.config->infill_dense.get_bool());
            assert(surface_fill.surface.maxNbSolidLayersOnTop == 1);
            assert(ipRectiWithPerimeter == surface_fill.params.pattern);
            // dense infill : ipRectiWithPerimeter
            //f = std::unique_ptr<Fill>(Fill::new_from_type(ipRectiWithPerimeter));
        }
        f->set_bounding_box(bbox);
        f->layer_id = this->id() - this->object()->layer(0).id(); // We need to subtract raft layers.
        f->z        = this->unscaled_print_z();
        f->angle    = surface_fill.params.angle;
        f->adapt_fill_octree   = (surface_fill.params.pattern == ipSupportCubic) ? support_fill_octree : adaptive_fill_octree;
        f->set_config(&this->object()->print()->config(), &this->object()->config());

        if (surface_fill.params.pattern == ipLightning)
            dynamic_cast<FillLightning::Filler *>(f.get())->generator = lightning_generator;

        f->init_spacing(surface_fill.params.spacing, surface_fill.params);

        // calculate flow spacing for infill pattern generation
        double link_max_length = 0.;
        if (!surface_fill.params.flow.bridge()) {
#if 0
            link_max_length = common_region_config.option(surface.is_external() ? "external_fill_link_max_length" : "fill_link_max_length")->get_effective_value(flow.spacing());
//            printf("flow spacing: %f,  is_external: %d, link_max_length: %lf\n", flow.spacing(), int(surface.is_external()), link_max_length);
#else
            if (surface_fill.params.density > .8) // 80%
                link_max_length = 3. * f->get_spacing();
#endif
        }

        // Maximum length of the perimeter segment linking two infill lines.
        f->link_max_length = scale_i(link_max_length);
        // Used by the concentric infill pattern to clip the loops to create extrusion paths.
        f->loop_clipping = scale_i(common_region_config.get_computed_value("seam_gap", surface_fill.params.extruder - 1) * surface_fill.params.flow.nozzle_diameter());


        // apply half spacing using this flow's own spacing and generate infill
        FillParams params = surface_fill.params;
        params.density           = surface_fill.params.density;
        params.dont_adjust       = false; //  surface_fill.params.dont_adjust;
        params.anchor_length     = surface_fill.params.anchor_length;
        params.anchor_length_max = surface_fill.params.anchor_length_max;
        params.fill_resolution   = resolution;
        params.use_arachne       = false;
        params.layer_height      = (float)this->unscaled_height();

        for (ExPolygon &expoly : surface_fill.expolygons) {
            surface_fill.surface.expolygon = std::move(expoly);
            try {
                Polylines polylines;
                if (f->can_fill_surface_single) {
                    polylines = f->fill_surface(&surface_fill.surface, params);
                } else {
                    ExtrusionEntityCollection coll;
                    f->fill_surface_extrusion(&surface_fill.surface, params, coll);
                    //extract polylines from paths
                    GetPathsVisitor visitor;
                    visitor.traverse(coll);
                    for (ExtrusionEntity *path : visitor.paths) {
                        polylines.push_back(path->as_polyline().to_polyline());
                    }
                }
                sparse_infill_polylines.insert(sparse_infill_polylines.end(), polylines.begin(), polylines.end());
            } catch (InfillFailedException &) {}
        }
    }

    return sparse_infill_polylines;
}


void Layer::make_ironing() {
    for (LayerSliceIslandUPtr &layer_island_ptr : m_islands) {
        this->_make_ironing(*layer_island_ptr);
    }
}
// Create ironing extrusions over top surfaces.
void Layer::_make_ironing(LayerSliceIsland &island)
{
    // LayerRegion::slices contains surfaces marked with SurfaceType.
    // Here we want to collect top surfaces extruded with the same extruder.
    // A surface will be ironed with the same extruder to not contaminate the print with another material leaking from the nozzle.

    // First classify regions based on the extruder used.
    struct IroningParams {
        int         extruder     = -1;
        bool         just_infill = false;
        // Spacing of the ironing lines, also to calculate the extrusion flow from.
        double         line_spacing;
        // Height of the extrusion, to calculate the extrusion flow from.
        double         height;
        double         acceleration;
        double         speed;
        double         angle;
        IroningType    type;

        bool operator<(const IroningParams &rhs) const {
            if (this->extruder < rhs.extruder)
                return true;
            if (this->extruder > rhs.extruder)
                return false;
            if (int(this->just_infill) < int(rhs.just_infill))
                return true;
            if (int(this->just_infill) > int(rhs.just_infill))
                return false;
            if (this->line_spacing < rhs.line_spacing)
                return true;
            if (this->line_spacing > rhs.line_spacing)
                return false;
            if (this->height < rhs.height)
                return true;
            if (this->height > rhs.height)
                return false;
            if (this->acceleration < rhs.acceleration)
                return true;
            if (this->acceleration > rhs.acceleration)
                return false;
            if (this->speed < rhs.speed)
                return true;
            if (this->speed > rhs.speed)
                return false;
            if (this->angle < rhs.angle)
                return true;
            if (this->angle > rhs.angle)
                return false;
            return false;
        }

        bool operator==(const IroningParams &rhs) const
        {
            return this->extruder == rhs.extruder && this->just_infill == rhs.just_infill &&
                   this->line_spacing == rhs.line_spacing && this->height == rhs.height &&
                   this->acceleration == rhs.acceleration && this->speed == rhs.speed &&
                   this->angle == rhs.angle && this->type == rhs.type;
        }

        const LayerRegion *layerm;
        // id of the region into the layer where the fill is made.
        // uint32_t     layer_region_id;
        //LayerRegionSetCPtrs lregions;

        // IdeaMaker: ironing
        // ironing flowrate (5% percent)
        // ironing speed (10 mm/sec)

        // Kisslicer: 
        // iron off, Sweep, Group
        // ironing speed: 15 mm/sec

        // Cura:
        // Pattern (zig-zag / concentric)
        // line spacing (0.1mm)
        // flow: from normal layer height. 10%
        // speed: 20 mm/sec
    };

    std::vector<IroningParams> by_extruder;
    // not using layer.height?
    double default_layer_height = this->object()->config().layer_height;
    //for (uint32_t layer_region_id = 0; layer_region_id < uint32_t(this->m_regions.size()); ++layer_region_id) {
        //LayerRegion *layerm = m_regions[layer_region_id];
    for (const LayerRegion *layerm : island.regions()) {
        if (! layerm->slices().empty()) {
            IroningParams ironing_params;
            const PrintRegionConfig &config = layerm->region().config();
            if (config.ironing && 
                (config.ironing_type == IroningType::AllSolid ||
                     ((config.top_solid_layers > 0 || (config.solid_infill_every_layers.value == 1 && config.fill_density.value > 0)) && 
                        (config.ironing_type == IroningType::TopSurfaces ||
                         (config.ironing_type == IroningType::TopmostOnly && this->upper_layer == nullptr))))) {
                if (config.perimeter_extruder == config.solid_infill_extruder || config.perimeters == 0) {
                    // Iron the whole face.
                    ironing_params.extruder = config.solid_infill_extruder;
                } else {
                    // Iron just the infill.
                    ironing_params.extruder = config.solid_infill_extruder;
                }
            }
            if (ironing_params.extruder != -1) {
                //TODO just_infill is currently not used.
                ironing_params.type         = config.ironing_type;
                ironing_params.just_infill  = false;
                ironing_params.line_spacing = config.get_computed_value("ironing_spacing", ironing_params.extruder);
                ironing_params.height       = default_layer_height * 0.01 * config.ironing_flowrate;
                ironing_params.acceleration = config.ironing_acceleration;
                ironing_params.speed        = config.ironing_speed;
                if (config.ironing_angle.value >= 0) {
                    ironing_params.angle = float(Geometry::deg2rad(config.ironing_angle.value));
                } else {
                    ironing_params.angle = compute_fill_angle(config, this->id());
                    if (config.ironing_angle.value < -1) {
                        ironing_params.angle += float(Geometry::deg2rad(-config.ironing_angle.value));
                    }
                }
                ironing_params.layerm         = layerm;
                //ironing_params.layer_region_id= layer_region_id;
                //ironing_params.lregions.insert(layerm);
                by_extruder.emplace_back(ironing_params);
            }
        }
    }
    std::sort(by_extruder.begin(), by_extruder.end());

    FillRectilinear 	fill;
    FillParams             fill_params;
    fill.set_bounding_box(this->object()->bounding_box());
	// Layer ID is used for orienting the infill in alternating directions.
	// Layer::id() returns layer ID including raft layers, subtract them to make the infill direction independent
	// from raft.
	//FIXME ironing does not take fill angle into account. Shall it? Does it matter?
	fill.layer_id 			 = this->id() - this->object()->layer(0).id();
    fill.z                  = this->unscaled_print_z();
    fill.overlap            = 0;
    fill_params.density     = 1.;
    fill_params.connection  = InfillConnection::icConnected;
    fill_params.monotonic  = true;

    for (size_t i = 0; i < by_extruder.size();) {
        // Find span of regions equivalent to the ironing operation.
        IroningParams &ironing_params = by_extruder[i];
        size_t j = i;
        for (++ j; j < by_extruder.size() && ironing_params == by_extruder[j]; ++ j) ;

        // Create the ironing extrusions for regions <i, j)
        ExPolygons ironing_areas;
        LayerRegionSetCPtrs region_ironed;
        double nozzle_dmr = this->object()->print()->config().nozzle_diameter.get_at(ironing_params.extruder - 1);
        const PrintRegionConfig& region_config = ironing_params.layerm->region().config();
        if (ironing_params.just_infill) {
            // TODO just_infill is currently not used.
            // Just infill.
        } else {
            // Infill and perimeter.
            // Merge top surfaces with the same ironing parameters.
            Polygons polys;
            Polygons infills;
            for (size_t k = i; k < j; ++k) {
                const IroningParams& ironing_params = by_extruder[k];
                region_ironed.insert(ironing_params.layerm);
                bool iron_everything = region_config.ironing_type == IroningType::AllSolid;
                bool iron_completely = iron_everything;
                if (iron_everything) {
                    // Check whether there is any non-solid hole in the regions.
                    bool internal_infill_solid = region_config.fill_density.value > 95.;
                    for (const Surface &surface : ironing_params.layerm->fill_surfaces())
                        // stInternal or stInternalBridge or stInternalVoid
                        if ((!internal_infill_solid && surface.has(stPosInternal | stDensSparse)) ||
                            surface.surface_type == (stPosInternal | stDensSolid | stModBridge) ||
                            surface.has(stPosInternal | stDensVoid)) {
                            // Some fill region is not quite solid. Don't iron over the whole surface.
                            iron_completely = false;
                            break;
                        }
                }
                if (iron_completely) {
                    // Iron everything. This is likely only good for solid transparent objects.
                    for (const Surface &surface : ironing_params.layerm->slices())
                        polygons_append(polys, surface.expolygon);
                } else {
                    for (const Surface &surface : ironing_params.layerm->slices())
                        if (surface.surface_type == (stPosTop | stDensSolid) || (iron_everything && surface.surface_type == (stPosBottom | stDensSolid)))
                            // stBottomBridge is not being ironed on purpose, as it would likely destroy the bridges.
                            polygons_append(polys, surface.expolygon);
                }
                if (iron_everything && !iron_completely) {
                    // Add solid fill surfaces. This may not be ideal, as one will not iron perimeters touching these
                    // solid fill surfaces, but it is likely better than nothing.
                    for (const Surface &surface : ironing_params.layerm->fill_surfaces())
                        if (surface.surface_type == (stPosInternal | stDensSolid))
                            polygons_append(infills, surface.expolygon);
                }
            }

            if (!infills.empty() || j > i + 1) {
                // Ironing over more than a single region or over solid internal infill.
                if (!infills.empty())
                    // For IroningType::AllSolid only:
                    // Add solid infill areas for layers, that contain some non-ironable infil (sparse infill, bridge infill).
                    append(polys, std::move(infills));
                polys = union_safety_offset(polys);
            }
            // Trim the top surfaces with half the nozzle diameter.
            ironing_areas = intersection_ex(polys, offset(island.get_slice(), -float(scale_d(0.5 * nozzle_dmr))));
        }

        // Create the filler object.
        fill.init_spacing(ironing_params.line_spacing, fill_params);
        fill.can_angle_cross = region_config.fill_angle_cross.value;
        fill.angle = float(ironing_params.angle);
        fill.link_max_length = scale_i(3. * fill.get_spacing());
        double extrusion_height = ironing_params.height * fill.get_spacing() / nozzle_dmr;
        //FIXME FLOW decide if it's good
        // note: don't use filament_max_overlap, as it's a top surface
        double overlap = region_config.top_solid_infill_overlap.get_effective_value(1.);
        float  extrusion_width = Flow::rounded_rectangle_extrusion_width_from_spacing(float(nozzle_dmr), float(extrusion_height), float(overlap));
        double flow_mm3_per_mm = nozzle_dmr * extrusion_height;
        //Flow flow = Flow::new_from_spacing(float(nozzle_dmr), 0., float(height), 1.f, false);
        //double flow_mm3_per_mm = flow.mm3_per_mm();
        Surface surface_fill((stPosTop | stDensSolid), ExPolygon());
        // get or create the LayerRegionIsland
        int16_t fill_extruder = int16_t(region_config.solid_infill_extruder.value - 1);
        assert(fill_extruder >= 0);
        LayerRegionIsland &region_island = island.get_or_add_region_island(region_ironed, fill_extruder);
        ExtrusionEntityCollection &eec_root = region_island.mutable_extrusion(LayerRegionIsland::IRONINGS);
        assert(eec_root.empty());
        eec_root.set_can_sort_reverse(true, true);
        for (ExPolygon &expoly : ironing_areas) {
            surface_fill.expolygon = std::move(expoly);
            Polylines polylines;
            try {
                assert(!fill_params.use_arachne);
                polylines = fill.fill_surface(&surface_fill, fill_params);
            } catch (InfillFailedException &) {
            }
            if (! polylines.empty()) {
                // Save into layer.
                ExtrusionEntityCollection *eec = &eec_root;
                if (ironing_areas.size() > 1) {
                    eec = new ExtrusionEntityCollection();
                    eec_root.append(ExtrusionEntityUPtr(eec));
                }
                // Don't sort the ironing infill lines as they are monotonicly ordered.
                eec->set_can_sort_reverse(false, false);
                extrusion_entities_append_paths(
                    *eec, std::move(polylines),
                    ExtrusionAttributes{ExtrusionRole::Ironing,
                                        // TODO check FLOW, decide if it's good for an ironing?
                                        ExtrusionFlow{flow_mm3_per_mm, extrusion_width, float(extrusion_height)}}, false);
            }
        }
        // Regions up to j were processed.
        i = j;
    }
}

} // namespace Slic3r
