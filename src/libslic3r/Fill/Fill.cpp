#include <assert.h>
#include <stdio.h>
#include <memory>

#include "../ClipperUtils.hpp"
#include "../Geometry.hpp"
#include "../Layer.hpp"
#include "../Print.hpp"
#include "../PrintConfig.hpp"
#include "../Surface.hpp"

#include "FillBase.hpp"
#include "FillRectilinear.hpp"
#include "FillLightning.hpp"
#include "FillConcentric.hpp"

namespace Slic3r {

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
        RETURN_COMPARE_NON_EQUAL_TYPED(unsigned, connection);
        RETURN_COMPARE_NON_EQUAL_TYPED(unsigned, dont_adjust);

        RETURN_COMPARE_NON_EQUAL(anchor_length);
        RETURN_COMPARE_NON_EQUAL(fill_exactly);
        RETURN_COMPARE_NON_EQUAL(flow.width());
        RETURN_COMPARE_NON_EQUAL(flow.height());
        RETURN_COMPARE_NON_EQUAL(flow.nozzle_diameter());
        RETURN_COMPARE_NON_EQUAL_TYPED(unsigned, flow.bridge());
        RETURN_COMPARE_NON_EQUAL_TYPED(unsigned, role);
        RETURN_COMPARE_NON_EQUAL_TYPED(int32_t, priority);
        assert(this->config != nullptr);
        assert(rhs.config != nullptr);
        if (config != nullptr && rhs.config != nullptr) {
            RETURN_COMPARE_NON_EQUAL(config->infill_speed);
            RETURN_COMPARE_NON_EQUAL(config->solid_infill_speed);
            RETURN_COMPARE_NON_EQUAL(config->top_solid_infill_speed);
            RETURN_COMPARE_NON_EQUAL(config->ironing_speed);
            RETURN_COMPARE_NON_EQUAL(config->default_speed);
            RETURN_COMPARE_NON_EQUAL(config->bridge_speed);
            RETURN_COMPARE_NON_EQUAL(config->bridge_speed_internal);
            RETURN_COMPARE_NON_EQUAL(config->gap_fill_speed);
            RETURN_COMPARE_NON_EQUAL(config->print_extrusion_multiplier);
        }
        assert(*this == rhs);
        return false;
    }

    bool operator==(const SurfaceFillParams &rhs) const {
        // first check speed via config
        if ((config != nullptr) != (rhs.config != nullptr))
            return false;
        if(config != nullptr && (
            config->infill_speed != rhs.config->infill_speed
            || config->solid_infill_speed != rhs.config->solid_infill_speed
            || config->top_solid_infill_speed != rhs.config->top_solid_infill_speed
            || config->ironing_speed != rhs.config->ironing_speed
            || config->default_speed != rhs.config->default_speed
            || config->bridge_speed != rhs.config->bridge_speed
            || config->bridge_speed_internal != rhs.config->bridge_speed_internal
            || config->gap_fill_speed != rhs.config->gap_fill_speed))
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
                this->dont_adjust           == rhs.dont_adjust      &&
                this->anchor_length         == rhs.anchor_length    &&
                this->anchor_length_max     == rhs.anchor_length_max&&
                this->fill_exactly          == rhs.fill_exactly     &&
                this->flow                  == rhs.flow             &&
                this->role                  == rhs.role             &&
                this->priority              == rhs.priority;
    }
};

struct SurfaceFill {
    SurfaceFill(const SurfaceFillParams& params) : region_id(size_t(-1)), surface(stNone, ExPolygon()), params(params) {}

    size_t                 region_id;
    Surface             surface;
    ExPolygons           expolygons;
    SurfaceFillParams    params;
};

float compute_fill_angle(const PrintRegionConfig &region_config, size_t layer_id)
{
    float angle = 0;
    if (!region_config.fill_angle_template.empty()) {
        // fill pattern: replace fill angle
        size_t idx   = layer_id % region_config.fill_angle_template.values.size();
        angle = region_config.fill_angle_template.values[idx];
    } else {
        angle = region_config.fill_angle.value;
    }
    angle += region_config.fill_angle_increment.value * layer_id;
    // make compute in degre, then normalize and convert into rad.
    angle = float(Geometry::deg2rad(angle));
    
    return angle;
}

std::vector<SurfaceFill> group_fills(const Layer &layer)
{
    std::vector<SurfaceFill> surface_fills;

    // Fill in a map of a region & surface to SurfaceFillParams.
    std::set<SurfaceFillParams>                         set_surface_params;
    std::vector<std::vector<const SurfaceFillParams*>>     region_to_surface_params(layer.regions().size(), std::vector<const SurfaceFillParams*>());
    SurfaceFillParams                                    params;
    bool                                                 has_internal_voids = false;
    for (size_t region_id = 0; region_id < layer.regions().size(); ++ region_id) {
        const LayerRegion  &layerm = *layer.regions()[region_id];
        region_to_surface_params[region_id].assign(layerm.fill_surfaces.size(), nullptr);
        for (const Surface &surface : layerm.fill_surfaces.surfaces)
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
                    params.pattern = ipRectilinear;
                    params.connection = region_config.infill_connection_solid.value;
                    if (surface.has_pos_top()) {
                        params.connection = region_config.infill_connection_top.value;
                    }
                    if (surface.has_pos_bottom()) {
                        params.connection = region_config.infill_connection_bottom.value;
                    }
                    //FIXME for non-thick bridges, shall we allow a bottom surface pattern?
                    if (is_bridge) {
                        params.pattern = region_config.bridge_fill_pattern.value;
                        params.connection = region_config.infill_connection_bridge.value;
                        params.bridge_type = region_config.bridge_type.value;
                    }
                    if (surface.has_pos_external() && !is_bridge) {
                        params.pattern = surface.has_pos_top() ? region_config.top_fill_pattern.value : region_config.bottom_fill_pattern.value;
                    } else if (!is_bridge) {
                        params.pattern = region_config.solid_fill_pattern.value;
                    }
                } else {
                    if (is_bridge) {
                        params.pattern = region_config.bridge_fill_pattern.value;
                        params.connection = region_config.infill_connection_bridge.value;
                    }
                    if (region_config.infill_dense.get_bool()
                        && region_config.fill_density < 40
                        && surface.maxNbSolidLayersOnTop == 1) {
                        params.density = 0.42f;
                        is_denser = true;
                        is_bridge = true;
                        params.pattern = ipRectiWithPerimeter;
                        params.priority = surface.priority;
                        params.dont_adjust = true; // keep the 42% density
                        params.connection = InfillConnection::icConnected;
                    }
                    if (params.density <= 0 && !is_denser)
                        continue;
                }
                //adjust spacing/density (to over-extrude when needed)
                if (surface.has_mod_overBridge()) {
                    params.density = float(region_config.over_bridge_flow_ratio.get_abs_value(1));
                }

                //note: same as getRoleFromSurfaceType()
                params.role = erInternalInfill;
                if (is_bridge) {
                    if(surface.has_pos_bottom())
                        params.role = erBridgeInfill;
                    else
                        params.role = erInternalBridgeInfill;
                } else if (surface.has_fill_solid()) {
                    if (surface.has_pos_top()) {
                        params.role = erTopSolidInfill;
                    } else {
                        params.role = erSolidInfill;
                    }
                }
                params.fill_exactly = region_config.enforce_full_fill_volume.get_bool();
                params.bridge_angle = float(surface.bridge_angle);
                params.angle         = (is_denser) ? 0 : compute_fill_angle(region_config, layerm.layer()->id());
                params.can_angle_cross = region_config.fill_angle_cross;
		        params.anchor_length = std::min(params.anchor_length, params.anchor_length_max);

                //adjust flow (to over-extrude when needed)
                params.flow_mult = 1;
                if (surface.has_pos_top())
                    params.flow_mult *= float(region_config.fill_top_flow_ratio.get_abs_value(1));

                params.config = &layerm.region().config();

                // calculate the actual flow we'll be using for this infill
                //FIXME FLOW decide what to use
                //params.flow = params.bridge ?
                //    layerm.bridging_flow(extrusion_role) :
                //    layerm.flow(extrusion_role, (surface.thickness == -1) ? layer.height : surface.thickness);
                if (is_bridge) {
                    float nozzle_diameter = layer.object()->print()->config().nozzle_diameter.get_at(layerm.region().extruder(extrusion_role, *layer.object()) - 1);
                    double diameter = 0;
                    if (region_config.bridge_type == BridgeType::btFromFlow) {
                        Flow reference_flow = layerm.flow(FlowRole::frSolidInfill);
                        diameter = sqrt(4 * reference_flow.mm3_per_mm() / PI);
                    } else if (region_config.bridge_type == BridgeType::btFromHeight) {
                        diameter = layerm.layer()->height;
                    } else /*if (region_config.bridge_type == BridgeType::btFromNozzle)*/ {
                        diameter = nozzle_diameter;
                    }
                    params.flow = Flow::bridging_flow((float)(diameter * std::sqrt(region_config.bridge_flow_ratio.get_abs_value(1))), nozzle_diameter);
                } else {
                    params.flow = layerm.region().flow(
                        *layer.object(),
                        extrusion_role,
                        (surface.thickness == -1) ? layer.height : surface.thickness,   // extrusion height
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
                    //params.spacing = layerm.region().flow(*layer.object(), frInfill, layer.object()->config().layer_height, false).spacing();
                    // it's internal infill, so we can calculate a generic flow spacing 
                    // for all layers, for avoiding the ugly effect of
                    // misaligned infill on first layer because of different extrusion width and
                    // layer height
                    params.spacing = layerm.region().flow(
                            *layer.object(),
                            frInfill,
                            layer.object()->config().layer_height.value,  // TODO: handle infill_every_layers?
                            layer.id()
                        ).spacing();

                    // Anchor a sparse infill to inner perimeters with the following anchor length:
                    params.anchor_length = float(region_config.infill_anchor);
                    if (region_config.infill_anchor.percent)
                        params.anchor_length = float(params.anchor_length * 0.01 * params.spacing);
                    params.anchor_length_max = float(region_config.infill_anchor_max);
                    if (region_config.infill_anchor_max.percent)
                        params.anchor_length_max = float(params.anchor_length_max * 0.01 * params.spacing);
                    params.anchor_length = std::min(params.anchor_length, params.anchor_length_max);
                }

                auto it_params = set_surface_params.find(params);
                if (it_params == set_surface_params.end())
                    it_params = set_surface_params.insert(it_params, params);
                region_to_surface_params[region_id][&surface - &layerm.fill_surfaces.surfaces.front()] = &(*it_params);
            }
    }

    surface_fills.reserve(set_surface_params.size());
    for (const SurfaceFillParams &params : set_surface_params) {
        const_cast<SurfaceFillParams&>(params).idx = surface_fills.size();
        surface_fills.emplace_back(params);
    }

    for (size_t region_id = 0; region_id < layer.regions().size(); ++ region_id) {
        const LayerRegion &layerm = *layer.regions()[region_id];
        for (const Surface &surface : layerm.fill_surfaces.surfaces)
            if (surface.surface_type != (stPosInternal | stDensVoid)) {
                const SurfaceFillParams *params = region_to_surface_params[region_id][&surface - &layerm.fill_surfaces.surfaces.front()];
                if (params != nullptr) {
                    SurfaceFill &fill = surface_fills[params->idx];
                    if (fill.region_id == size_t(-1)) {
                        fill.region_id = region_id;
                        fill.surface = surface;
                        fill.expolygons.emplace_back(std::move(fill.surface.expolygon));
                    } else
                        fill.expolygons.emplace_back(surface.expolygon);
                }
            }
    }

    {
        Polygons all_polygons;
        for (SurfaceFill &fill : surface_fills)
            if (! fill.expolygons.empty()) {
                if (fill.params.priority > 0) {
                    append(all_polygons, to_polygons(fill.expolygons));
                }else if (fill.expolygons.size() > 1 || !all_polygons.empty()) {
                    Polygons polys = to_polygons(std::move(fill.expolygons));
                    // Make a union of polygons, use a safety offset, subtract the preceding polygons.
                    // Bridges are processed first (see SurfaceFill::operator<())
                    fill.expolygons = all_polygons.empty() ? union_safety_offset_ex(polys) : diff_ex(polys, all_polygons, ApplySafetyOffset::Yes);
                    append(all_polygons, std::move(polys));
                } else if (&fill != &surface_fills.back())
                    append(all_polygons, to_polygons(fill.expolygons));
            }
    }

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
        int      region_internal_infill = -1;
        int         region_solid_infill = -1;
        int         region_some_infill = -1;
        for (SurfaceFill &surface_fill : surface_fills)
            if (! surface_fill.expolygons.empty()) {
                distance_between_surfaces = std::max(distance_between_surfaces, surface_fill.params.flow.scaled_spacing());
                append((surface_fill.surface.surface_type == (stPosInternal | stDensVoid)) ? voids : surfaces_polygons, to_polygons(surface_fill.expolygons));
                if (surface_fill.surface.surface_type == (stPosInternal | stDensSolid))
                    region_internal_infill = (int)surface_fill.region_id;
                if (surface_fill.surface.has_fill_solid())
                    region_solid_infill = (int)surface_fill.region_id;
                if (surface_fill.surface.surface_type != (stPosInternal | stDensVoid))
                    region_some_infill = (int)surface_fill.region_id;
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
            unsigned int region_id = 0;
            if (region_internal_infill != -1)
                region_id = region_internal_infill;
            else if (region_solid_infill != -1)
                region_id = region_solid_infill;
            else if (region_some_infill != -1)
                region_id = region_some_infill;
            const LayerRegion& layerm = *layer.regions()[region_id];
            for (SurfaceFill &surface_fill : surface_fills)
                if (surface_fill.surface.surface_type == (stPosInternal | stDensVoid) && std::abs(layer.height - surface_fill.params.flow.height()) < EPSILON) {
                    internal_solid_fill = &surface_fill;
                    break;
                }
            if (internal_solid_fill == nullptr) {
                // Produce another solid fill.
                params.extruder = layerm.region().extruder(frSolidInfill, *layer.object());
                params.pattern  = layerm.region().config().solid_fill_pattern.value;
                params.density  = 100.f;
                params.role     = erInternalInfill;
                params.angle    = compute_fill_angle(layerm.region().config(), layerm.layer()->id());
                //FIXME FLOW decide what to use
                //params.flow = layerm.flow(frSolidInfill);
                // calculate the actual flow we'll be using for this infill
                params.flow = layerm.region().flow(
                    *layer.object(),
                    frSolidInfill,
                    layer.height,         // extrusion height
                    layer.id()
                );
                params.spacing = params.flow.spacing();            
                surface_fills.emplace_back(params);
                surface_fills.back().surface.surface_type = (stPosInternal | stDensSolid);
                surface_fills.back().surface.thickness = layer.height;
                surface_fills.back().expolygons = std::move(extensions);
            } else {
                append(extensions, std::move(internal_solid_fill->expolygons));
                internal_solid_fill->expolygons = union_ex(extensions);
            }
        }
    }

    return surface_fills;
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

// friend to Layer
void Layer::make_fills(FillAdaptive::Octree* adaptive_fill_octree, FillAdaptive::Octree* support_fill_octree, FillLightning::Generator* lightning_generator)
{
    for (LayerRegion* layerm : m_regions) {
        layerm->fills.clear();
        layerm->ironings.clear();
    }


#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
//    this->export_region_fill_surfaces_to_svg_debug("10_fill-initial");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */

    std::vector<SurfaceFill>  surface_fills  = group_fills(*this);
    const Slic3r::BoundingBox bbox           = this->object()->bounding_box();
    //const auto                resolution     = this->object()->print()->config().gcode_resolution.value;
    const auto                perimeter_generator = this->object()->config().perimeter_generator;

    std::sort(surface_fills.begin(), surface_fills.end(), [](SurfaceFill& s1, SurfaceFill& s2) {
        if (s1.region_id == s2.region_id)
            return s1.params.priority < s2.params.priority;
        return s1.region_id < s2.region_id;
        });

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    {
        static int iRun = 0;
        export_group_fills_to_svg(debug_out_path("Layer-fill_surfaces-10_fill-final-%d.svg", iRun ++).c_str(), surface_fills);
    }
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
    std::vector<ExtrusionEntityCollection*> fills_by_priority;
    auto store_fill = [&fills_by_priority, this](size_t region_id) {
        if (fills_by_priority.size() == 1) {
            m_regions[region_id]->fills.append(fills_by_priority[0]->entities());
            delete fills_by_priority[0];
        } else {
            m_regions[region_id]->fills.set_can_sort_reverse(false, false);
            ExtrusionEntityCollection* eec = new ExtrusionEntityCollection();
            eec->set_can_sort_reverse(false, false);
            for (ExtrusionEntityCollection* per_priority : fills_by_priority) {
                if (!per_priority->entities().empty())
                    eec->append(ExtrusionEntitiesPtr{per_priority});
                else
                    delete per_priority;
            }
            m_regions[region_id]->fills.append(ExtrusionEntitiesPtr{ eec });
        }
        fills_by_priority.clear();
    };
    //surface_fills is sorted by region_id
    size_t current_region_id = -1;
    for (SurfaceFill &surface_fill : surface_fills) {
        // store the region fill when changing region. 
        if (current_region_id != size_t(-1) && current_region_id != surface_fill.region_id) {
            store_fill(current_region_id);
        }
        current_region_id = surface_fill.region_id;
        const LayerRegion* layerm = this->m_regions[surface_fill.region_id];
        
        // Create the filler object.
        std::unique_ptr<Fill> f = std::unique_ptr<Fill>(Fill::new_from_type(surface_fill.params.pattern));
        f->set_bounding_box(bbox);
        f->layer_id = this->id();
        f->z        = this->print_z;
        f->angle    = surface_fill.params.angle;
        f->can_angle_cross   = surface_fill.params.can_angle_cross;
        f->adapt_fill_octree = (surface_fill.params.pattern == ipSupportCubic) ? support_fill_octree : adaptive_fill_octree;

        if (surface_fill.params.pattern == ipLightning)
            dynamic_cast<FillLightning::Filler*>(f.get())->generator = lightning_generator;

        if (perimeter_generator.value == PerimeterGeneratorType::Arachne && surface_fill.params.pattern == ipConcentric) {
            FillConcentric *fill_concentric = dynamic_cast<FillConcentric *>(f.get());
            assert(fill_concentric != nullptr);
            fill_concentric->print_config        = &this->object()->print()->config();
            fill_concentric->print_object_config = &this->object()->config();
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
            link_max_length = layerm.region().config().get_abs_value(surface.is_external() ? "external_fill_link_max_length" : "fill_link_max_length", flow.spacing());
//            printf("flow spacing: %f,  is_external: %d, link_max_length: %lf\n", flow.spacing(), int(surface.is_external()), link_max_length);
#else
            if (surface_fill.params.density > .8) // 80%
                link_max_length = 3. * f->get_spacing();
#endif
        }

        // Maximum length of the perimeter segment linking two infill lines.
        f->link_max_length = (coord_t)scale_(link_max_length);

        //give the overlap size to let the infill do his overlap
        //add overlap if at least one perimeter
        float perimeter_spacing = 0;
        if(layerm->region().config().perimeters == 1)
            perimeter_spacing = layerm->flow(frExternalPerimeter).spacing();
        else if(layerm->region().config().only_one_perimeter_top)
            //note: use the min of the two to avoid overextrusion if only one perimeter top
            perimeter_spacing = std::min(layerm->flow(frPerimeter).spacing(), layerm->flow(frExternalPerimeter).spacing());
        else //if(layerm->region().config().perimeters > 1)
            perimeter_spacing = layerm->flow(frPerimeter).spacing();

        // Used by the concentric infill pattern to clip the loops to create extrusion paths.
        f->loop_clipping = scale_t(layerm->region().config().get_computed_value("seam_gap", surface_fill.params.extruder - 1) * surface_fill.params.flow.nozzle_diameter());

        // apply half spacing using this flow's own spacing and generate infill
        //FillParams params;
        //params.density         = float(0.01 * surface_fill.params.density);
        //params.dont_adjust     = false; //surface_fill.params.dont_adjust; // false
        //params.anchor_length   = surface_fill.params.anchor_length;
        //params.anchor_length_max = surface_fill.params.anchor_length_max;
        //params.resolution        = resolution;
        surface_fill.params.use_arachne = perimeter_generator == PerimeterGeneratorType::Arachne && surface_fill.params.pattern == ipConcentric;
        //params.layer_height      = m_regions[surface_fill.region_id]->layer()->height;

        //union with safety offset to avoid separation from the appends of different surface with same settings.
        surface_fill.expolygons = union_safety_offset_ex(surface_fill.expolygons);

        //store default values, before modification.
        bool dont_adjust = surface_fill.params.dont_adjust;
        float density = surface_fill.params.density;
        for (ExPolygon &expoly : surface_fill.expolygons) {
            //set overlap polygons
            f->no_overlap_expolygons.clear();
            if (surface_fill.params.config->perimeters > 0) {
                f->overlap = surface_fill.params.config->infill_overlap.get_abs_value((perimeter_spacing + (f->get_spacing())) / 2);
                if (f->overlap != 0) {
                    f->no_overlap_expolygons = intersection_ex(layerm->fill_no_overlap_expolygons, ExPolygons() = { expoly });
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
            surface_fill.params.layer_height = m_regions[surface_fill.region_id]->layer()->height;

            //init the surface with the current polygon
            if (!expoly.contour.empty()) {
                surface_fill.surface.expolygon = std::move(expoly);

                //adjust the bridge density
                if (surface_fill.params.flow.bridge() && surface_fill.params.density > 0.99 /*&& layerm->region()->config().bridge_overlap.get_abs_value(1) != 1*/) {
                    ////varies the overlap to have the best coverage for the bridge
                    //surface_fill.params.density *= float(layerm->region()->config().bridge_overlap.get_abs_value(1));
                    double min_spacing = 0.999 * surface_fill.params.spacing / surface_fill.params.config->bridge_overlap.get_abs_value(surface_fill.params.density);
                    double max_spacing = 1.001 * surface_fill.params.spacing / surface_fill.params.config->bridge_overlap_min.get_abs_value(surface_fill.params.density);
                    double factor = 1.00001;
                    if (min_spacing < max_spacing * 1.01) {
                        // create a bouding box of the rotated surface
                        coord_t bounding_box_size_x = 0;
                        coord_t bounding_box_min_x = 0;
                        ExPolygons expolys;
                        if (surface_fill.params.bridge_angle > 0 && !f->no_overlap_expolygons.empty()) {
                            //take only the no-overlap area
                            expolys = offset_ex(intersection_ex(ExPolygons{ ExPolygon{surface_fill.surface.expolygon.contour} }, f->no_overlap_expolygons), -scale_t(surface_fill.params.spacing) / 2 - 10);
                        } else {
                            expolys = offset_ex(ExPolygon{surface_fill.surface.expolygon.contour}, -scale_t(surface_fill.params.spacing) / 2 - 10);
                        }
                        // if nothing after collapse, then go to next surface_fill.expolygon
                        if (expolys.empty())
                            continue;

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
                        double new_spacing = unscaled(f->_adjust_solid_spacing(bounding_box_size_x, scale_t(min_spacing), 2));
                        if (new_spacing <= max_spacing) {
                            surface_fill.params.density = factor * surface_fill.params.spacing / new_spacing;
                        } else {
                            double new_spacing2 = unscaled(f->_adjust_solid_spacing(bounding_box_size_x, scale_t(min_spacing * 1.999 - new_spacing), 2));
                            if (new_spacing2 < min_spacing) {
                                if (min_spacing - new_spacing2 < new_spacing - max_spacing) {
                                    surface_fill.params.density = surface_fill.params.config->bridge_overlap.get_abs_value(surface_fill.params.density);
                                } else {
                                    surface_fill.params.density = surface_fill.params.config->bridge_overlap_min.get_abs_value(surface_fill.params.density);
                                }
                            } else {
                                //use the highest density
                                surface_fill.params.density = surface_fill.params.config->bridge_overlap.get_abs_value(surface_fill.params.density);
                            }
                        }
                        Polygon poly = surface_fill.surface.expolygon.contour;
                        poly.rotate(PI / 2 + (surface_fill.params.bridge_angle < 0 ? surface_fill.params.angle : surface_fill.params.bridge_angle));
                        surface_fill.params.dont_adjust = true;
                        surface_fill.params.bridge_offset = std::abs(poly.bounding_box().min.x() - bounding_box_min_x);
                    }
                }

                //make fill
                while ((size_t)surface_fill.params.priority >= fills_by_priority.size())
                    fills_by_priority.push_back(new ExtrusionEntityCollection());
#if _DEBUG
                const size_t idx_start = fills_by_priority[(size_t)surface_fill.params.priority]->entities().size();
#endif
                f->fill_surface_extrusion(&surface_fill.surface, surface_fill.params, fills_by_priority[(size_t)surface_fill.params.priority]->set_entities());
#if _DEBUG
                //check no over or underextrusion if fill_exactly
                if(surface_fill.params.fill_exactly && surface_fill.params.density == 1) {
                    ExtrusionVolume compute_volume;
                    ExtrusionVolume compute_volume_no_gap_fill(false);
                    const size_t idx_end = fills_by_priority[(size_t)surface_fill.params.priority]->entities().size();
                    //check that it doesn't overextrude
                    for(size_t idx = idx_start; idx < idx_end; ++idx){
                        fills_by_priority[(size_t)surface_fill.params.priority]->entities()[idx]->visit(compute_volume);
                        fills_by_priority[(size_t)surface_fill.params.priority]->entities()[idx]->visit(compute_volume_no_gap_fill);
                    }
                    ExPolygons temp = f->no_overlap_expolygons.empty() ?
                                        ExPolygons{surface_fill.surface.expolygon} :
                                        intersection_ex(ExPolygons{surface_fill.surface.expolygon}, f->no_overlap_expolygons);
                    double real_surface = 0;
                    for(auto &t : temp) real_surface += t.area();
                    assert(compute_volume.volume < unscaled(unscaled(surface_fill.surface.area())) * surface_fill.params.layer_height + EPSILON);
                    double area = unscaled(unscaled(real_surface));
                    assert(compute_volume.volume <= area * surface_fill.params.layer_height * 1.001 || f->debug_verify_flow_mult <= 0.8);
                    if(compute_volume.volume > 0) //can fail for thin regions
                        assert(compute_volume.volume >= area * surface_fill.params.layer_height * 0.999 || f->debug_verify_flow_mult >= 1.3 || area < std::max(1.,surface_fill.params.config->solid_infill_below_area.value));
                }
#endif
            }
        }
    }
    if(current_region_id != size_t(-1))
        store_fill(current_region_id);

    // add thin fill regions
    // Unpacks the collection, creates multiple collections per path.
    // The path type could be ExtrusionPath, ExtrusionLoop or ExtrusionEntityCollection.
    // Why the paths are unpacked?
    for (LayerRegion *layerm : m_regions) {
        for (const ExtrusionEntity *thin_fill : layerm->thin_fills.entities()) {
            ExtrusionEntityCollection *collection = new ExtrusionEntityCollection();
            if (!layerm->fills.can_sort() && layerm->fills.entities().size() > 0 && layerm->fills.entities()[0]->is_collection()) {
                //for dense_infill58c73b1, to print fills in the right sequence. Seems weird,  TODO: check & test it.
                ExtrusionEntityCollection* no_sort_fill = static_cast<ExtrusionEntityCollection*>(layerm->fills.entities()[0]);
                if (!no_sort_fill->can_sort() && no_sort_fill->entities().size() > 0 && no_sort_fill->entities()[0]->is_collection())
                    static_cast<ExtrusionEntityCollection*>(no_sort_fill->entities()[0])->append(ExtrusionEntitiesPtr{ collection });
                else
                    layerm->fills.append(ExtrusionEntitiesPtr{ collection });
            } else
                layerm->fills.append(ExtrusionEntitiesPtr{ collection });
            collection->append(*thin_fill);
        }
    }

}

// Create ironing extrusions over top surfaces.
void Layer::make_ironing()
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

        bool operator==(const IroningParams &rhs) const {
            return this->extruder == rhs.extruder && this->just_infill == rhs.just_infill &&
                   this->line_spacing == rhs.line_spacing && this->height == rhs.height && this->speed == rhs.speed &&
                this->angle == rhs.angle &&
                this->type == rhs.type;
        }

        LayerRegion *layerm        = nullptr;

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
    double default_layer_height = this->object()->config().layer_height;

    for (LayerRegion *layerm : m_regions)
        if (! layerm->slices().empty()) {
            IroningParams ironing_params;
            const PrintRegionConfig &config = layerm->region().config();
            if (config.ironing && 
                (config.ironing_type == IroningType::AllSolid ||
                     (config.top_solid_layers > 0 && 
                        (config.ironing_type == IroningType::TopSurfaces ||
                         (config.ironing_type == IroningType::TopmostOnly && layerm->layer()->upper_layer == nullptr))))) {
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
                ironing_params.type              = config.ironing_type;
                ironing_params.just_infill     = false;
                ironing_params.line_spacing = config.ironing_spacing;
                ironing_params.height         = default_layer_height * 0.01 * config.ironing_flowrate;
                ironing_params.speed         = config.ironing_speed;
                ironing_params.angle         = config.ironing_angle <0 ?
                    compute_fill_angle(config, layerm->layer()->id()) :
                    float(Geometry::deg2rad(config.ironing_angle.value));
                ironing_params.layerm         = layerm;
                by_extruder.emplace_back(ironing_params);
            }
        }
    std::sort(by_extruder.begin(), by_extruder.end());

    FillRectilinear 	fill;
    FillParams             fill_params;
    fill.set_bounding_box(this->object()->bounding_box());
    fill.layer_id           = this->id();
    fill.z                  = this->print_z;
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
        double nozzle_dmr = this->object()->print()->config().nozzle_diameter.values[ironing_params.extruder - 1];
        const PrintRegionConfig& region_config = ironing_params.layerm->region().config();
        if (ironing_params.just_infill) {
            // Just infill.
        } else {
            // Infill and perimeter.
            // Merge top surfaces with the same ironing parameters.
            Polygons polys;
            Polygons infills;
            for (size_t k = i; k < j; ++k) {
                const IroningParams& ironing_params = by_extruder[k];
                bool					  iron_everything = region_config.ironing_type == IroningType::AllSolid;
                bool					  iron_completely = iron_everything;
                if (iron_everything) {
                    // Check whether there is any non-solid hole in the regions.
                    bool internal_infill_solid = region_config.fill_density.value > 95.;
                    for (const Surface& surface : ironing_params.layerm->fill_surfaces.surfaces)
                        // stInternal or stInternalBridge or stInternalVoid
                        if ((!internal_infill_solid && surface.surface_type == (stPosInternal | stDensSparse)) || surface.surface_type == (stPosInternal | stDensSolid | stModBridge) || surface.surface_type == (stPosInternal | stDensVoid)) {
                            // Some fill region is not quite solid. Don't iron over the whole surface.
                            iron_completely = false;
                            break;
                        }
                }
                if (iron_completely) {
                    // Iron everything. This is likely only good for solid transparent objects.
                    for (const Surface& surface : ironing_params.layerm->slices().surfaces)
                        polygons_append(polys, surface.expolygon);
                } else {
                    for (const Surface& surface : ironing_params.layerm->slices().surfaces)
                        if (surface.surface_type == (stPosTop | stDensSolid) || (iron_everything && surface.surface_type == (stPosBottom | stDensSolid)))
                            // stBottomBridge is not being ironed on purpose, as it would likely destroy the bridges.
                            polygons_append(polys, surface.expolygon);
                }
                if (iron_everything && !iron_completely) {
                    // Add solid fill surfaces. This may not be ideal, as one will not iron perimeters touching these
                    // solid fill surfaces, but it is likely better than nothing.
                    for (const Surface& surface : ironing_params.layerm->fill_surfaces.surfaces)
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
            ironing_areas = intersection_ex(polys, offset(this->lslices, -float(scale_(0.5 * nozzle_dmr))));
        }

        // Create the filler object.
        fill.init_spacing(ironing_params.line_spacing, fill_params);
        fill.angle = float(ironing_params.angle + 0.25 * M_PI);
        fill.link_max_length = (coord_t)scale_(3. * fill.get_spacing());
        double extrusion_height = ironing_params.height * fill.get_spacing() / nozzle_dmr;
        //FIXME FLOW decide if it's good
        double max_overlap = region_config.get_computed_value("filament_max_overlap", ironing_params.extruder - 1);
        double overlap = std::min(max_overlap, region_config.solid_infill_overlap.get_abs_value(1.));
        float  extrusion_width = Flow::rounded_rectangle_extrusion_width_from_spacing(float(nozzle_dmr), float(extrusion_height), float(overlap));
        double flow_mm3_per_mm = nozzle_dmr * extrusion_height;
        //Flow flow = Flow::new_from_spacing(float(nozzle_dmr), 0., float(height), 1.f, false);
        //double flow_mm3_per_mm = flow.mm3_per_mm();
        Surface surface_fill((stPosTop | stDensSolid), ExPolygon());
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
                ExtrusionEntityCollection *eec = new ExtrusionEntityCollection();
                ironing_params.layerm->ironings.append(ExtrusionEntitiesPtr{ eec });
                // Don't sort the ironing infill lines as they are monotonicly ordered.
                eec->set_can_sort_reverse(false, false);
                extrusion_entities_append_paths(
                    *eec, std::move(polylines),
                    erIroning,
                    //FIXME FLOW decide if it's good
                    flow_mm3_per_mm, extrusion_width/*float(flow.width())*/, float(extrusion_height)/*float(height)*/);
            }
        }
        // Regions up to j were processed.
        i = j;
    }
}

} // namespace Slic3r
