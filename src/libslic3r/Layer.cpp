#include "Layer.hpp"
#include "ClipperUtils.hpp"
#include "Print.hpp"
#include "Fill/Fill.hpp"
#include "ShortestPath.hpp"
#include "SVG.hpp"
#include "BoundingBox.hpp"
#include "ExtrusionEntity.hpp"

#include <boost/log/trivial.hpp>

namespace Slic3r {

Layer::~Layer()
{
    this->lower_layer = this->upper_layer = nullptr;
    for (LayerRegion *region : m_regions)
        delete region;
    m_regions.clear();
}

// Test whether whether there are any slices assigned to this layer.
bool Layer::empty() const
{
    for (const LayerRegion *layerm : m_regions)
        if (layerm != nullptr && ! layerm->slices().empty())
            // Non empty layer.
            return false;
    return true;
}

LayerRegion* Layer::add_region(const PrintRegion *print_region)
{
    m_regions.emplace_back(new LayerRegion(this, print_region));
    return m_regions.back();
}

// merge all regions' slices to get islands
void Layer::make_slices()
{
    ExPolygons slices;
    if (m_regions.size() == 1) {
        // optimization: if we only have one region, take its slices
        slices = to_expolygons(m_regions.front()->slices().surfaces);
    } else {
        Polygons slices_p;
        for (LayerRegion *layerm : m_regions)
            polygons_append(slices_p, to_polygons(layerm->slices().surfaces));
        slices = union_safety_offset_ex(slices_p);
    }
    
    this->lslices.clear();
    this->lslices.reserve(slices.size());
    
    // prepare ordering points
    Points ordering_points;
    ordering_points.reserve(slices.size());
    for (const ExPolygon &ex : slices)
        ordering_points.push_back(ex.contour.first_point());
    
    // sort slices
    std::vector<Points::size_type> order = chain_points(ordering_points);
    
    // populate slices vector
    for (size_t i : order)
        this->lslices.emplace_back(std::move(slices[i]));
}

static inline bool layer_needs_raw_backup(const Layer *layer)
{
    return ! (layer->regions().size() == 1 && (layer->id() > 0 || layer->object()->config().first_layer_size_compensation.value == 0));
}

void Layer::backup_untyped_slices()
{
    if (layer_needs_raw_backup(this)) {
        for (LayerRegion *layerm : m_regions)
            layerm->raw_slices = to_expolygons(layerm->slices().surfaces);
    } else {
        assert(m_regions.size() == 1);
        m_regions.front()->raw_slices.clear();
    }
}

void Layer::restore_untyped_slices()
{
    if (layer_needs_raw_backup(this)) {
        for (LayerRegion *layerm : m_regions)
            layerm->m_slices.set(layerm->raw_slices, stPosInternal | stDensSparse);
    } else {
        assert(m_regions.size() == 1);
        m_regions.front()->m_slices.set(this->lslices, stPosInternal | stDensSparse);
    }
}

// Similar to Layer::restore_untyped_slices()
// To improve robustness of detect_surfaces_type() when reslicing (working with typed slices), see GH issue #7442.
// Only resetting layerm->slices if Slice::extra_perimeters is always zero or it will not be used anymore
// after the perimeter generator.
// there is now much more things used by prepare_infill, i'm not confident on this optimisation.
void Layer::restore_untyped_slices_no_extra_perimeters()
{
    restore_untyped_slices();
//    if (layer_needs_raw_backup(this)) {
//        for (LayerRegion *layerm : m_regions)
//        	if (! layerm->region().config().extra_perimeters.value)
//            	layerm->slices.set(layerm->raw_slices, stPosInternal | stDensSparse);
//    } else {
//    	assert(m_regions.size() == 1);
//    	LayerRegion *layerm = m_regions.front();
//    	// This optimization is correct, as extra_perimeters are only reused by prepare_infill() with multi-regions.
//        //if (! layerm->region().config().extra_perimeters.value)
//        	layerm->slices.set(this->lslices, stPosInternal | stDensSparse);
//    }
}

ExPolygons Layer::merged(float offset_scaled) const
{
    assert(offset_scaled >= 0.f);
    // If no offset is set, apply EPSILON offset before union, and revert it afterwards.
    float offset_scaled2 = 0;
    if (offset_scaled == 0.f) {
        offset_scaled  = float(  EPSILON);
        offset_scaled2 = float(- EPSILON);
    }
    Polygons polygons;
	for (LayerRegion *layerm : m_regions) {
		const PrintRegionConfig &config = layerm->region().config();
		// Our users learned to bend Slic3r to produce empty volumes to act as subtracters. Only add the region if it is non-empty.
		if (config.bottom_solid_layers > 0 || config.top_solid_layers > 0 || config.fill_density > 0. || config.perimeters > 0)
			append(polygons, offset(layerm->slices().surfaces, offset_scaled));
	}
    ExPolygons out = union_ex(polygons);
	if (offset_scaled2 != 0.f)
		out = offset_ex(out, offset_scaled2);
    return out;
}

// Here the perimeters are created cummulatively for all layer regions sharing the same parameters influencing the perimeters.
// The perimeter paths and the thin fills (ExtrusionEntityCollection) are assigned to the first compatible layer region.
// The resulting fill surface is split back among the originating regions.
void Layer::make_perimeters()
{
    BOOST_LOG_TRIVIAL(trace) << "Generating perimeters for layer " << this->id();
    
    // keep track of regions whose perimeters we have already generated
    std::vector<unsigned char> done(m_regions.size(), false);
    
    for (LayerRegionPtrs::iterator layerm = m_regions.begin(); layerm != m_regions.end(); ++ layerm) {
      if ((*layerm)->slices().empty()) {
          (*layerm)->perimeters.clear();
          (*layerm)->fills.clear();
          (*layerm)->ironings.clear();
          (*layerm)->thin_fills.clear();
      } else {
        size_t region_id = layerm - m_regions.begin();
        if (done[region_id])
            continue;
        BOOST_LOG_TRIVIAL(trace) << "Generating perimeters for layer " << this->id() << ", region " << region_id;
        done[region_id] = true;
        const PrintRegionConfig &config = (*layerm)->region().config();
        
        // find compatible regions
        LayerRegionPtrs layerms;
        layerms.push_back(*layerm);
        for (LayerRegionPtrs::const_iterator it = layerm + 1; it != m_regions.end(); ++it) {
            LayerRegion* other_layerm = *it;
            const PrintRegionConfig &other_config = other_layerm->region().config();
            if (other_layerm->slices().empty()) continue;
            /// !!! add here the settings you want to be added in the per-object menu.
            /// if you don't do that, objects will share the same region, and the same settings.
            if (config.perimeter_extruder           == other_config.perimeter_extruder
                && config.perimeters                == other_config.perimeters
                && config.external_perimeter_extrusion_width == other_config.external_perimeter_extrusion_width
                && config.external_perimeter_overlap == other_config.external_perimeter_overlap
                && config.external_perimeter_speed == other_config.external_perimeter_speed // it os mandatory? can't this be set at gcode.cpp?
                && config.external_perimeters_first == other_config.external_perimeters_first
                && config.external_perimeters_hole  == other_config.external_perimeters_hole
                && config.external_perimeters_nothole == other_config.external_perimeters_nothole
                && config.external_perimeters_vase == other_config.external_perimeters_vase
                && config.extra_perimeters_odd_layers == other_config.extra_perimeters_odd_layers
                && config.extra_perimeters_overhangs == other_config.extra_perimeters_overhangs
                && config.gap_fill_enabled          == other_config.gap_fill_enabled
                && ((config.gap_fill_speed          == other_config.gap_fill_speed) || !config.gap_fill_enabled)
                && config.gap_fill_last             == other_config.gap_fill_last
                && config.gap_fill_flow_match_perimeter == other_config.gap_fill_flow_match_perimeter
                && config.gap_fill_extension         == other_config.gap_fill_extension
                && config.gap_fill_max_width         == other_config.gap_fill_max_width
                && config.gap_fill_min_area         == other_config.gap_fill_min_area
                && config.gap_fill_min_length         == other_config.gap_fill_min_length
                && config.gap_fill_min_width         == other_config.gap_fill_min_width
                && config.gap_fill_overlap          == other_config.gap_fill_overlap
                && config.infill_dense              == other_config.infill_dense
                && config.infill_dense_algo         == other_config.infill_dense_algo
                && config.no_perimeter_unsupported_algo == other_config.no_perimeter_unsupported_algo
                && (this->id() == 0 || config.only_one_perimeter_first_layer == other_config.only_one_perimeter_first_layer)
                && config.only_one_perimeter_top    == other_config.only_one_perimeter_top
                && config.only_one_perimeter_top_other_algo == other_config.only_one_perimeter_top_other_algo
                && config.overhangs_width_speed     == other_config.overhangs_width_speed
                && config.overhangs_width           == other_config.overhangs_width
                && config.overhangs_reverse         == other_config.overhangs_reverse
                && config.overhangs_reverse_threshold == other_config.overhangs_reverse_threshold
                && config.perimeter_extrusion_width == other_config.perimeter_extrusion_width
                && config.perimeter_loop            == other_config.perimeter_loop
                && config.perimeter_loop_seam       == other_config.perimeter_loop_seam
                && config.perimeter_overlap         == other_config.perimeter_overlap
                && config.perimeter_speed           == other_config.perimeter_speed // it os mandatory? can't this be set at gcode.cpp?
                && config.small_perimeter_speed     == other_config.small_perimeter_speed
                && config.small_perimeter_min_length == other_config.small_perimeter_min_length
                && config.small_perimeter_max_length == other_config.small_perimeter_max_length
                && config.thin_walls                == other_config.thin_walls
                && config.thin_walls_min_width      == other_config.thin_walls_min_width
                && config.thin_walls_overlap        == other_config.thin_walls_overlap
                && config.thin_perimeters           == other_config.thin_perimeters
                && config.thin_perimeters_all       == other_config.thin_perimeters_all
                && config.thin_walls_speed          == other_config.thin_walls_speed
                && config.infill_overlap            == other_config.infill_overlap
                && config.perimeter_loop            == other_config.perimeter_loop

                && config.fuzzy_skin                == other_config.fuzzy_skin
                && config.fuzzy_skin_thickness      == other_config.fuzzy_skin_thickness
                && config.fuzzy_skin_point_dist     == other_config.fuzzy_skin_point_dist
                ) {
                layerms.push_back(other_layerm);
                done[it - m_regions.begin()] = true;
            }
        }
        
        if (layerms.size() == 1) {  // optimization
            (*layerm)->fill_surfaces.surfaces.clear();
            (*layerm)->make_perimeters((*layerm)->slices(), &(*layerm)->fill_surfaces);
            (*layerm)->fill_expolygons = to_expolygons((*layerm)->fill_surfaces.surfaces);
        } else {
            SurfaceCollection new_slices;
            // Use the region with highest infill rate, as the make_perimeters() function below decides on the gap fill based on the infill existence.
            LayerRegion *layerm_config = layerms.front();
            {
                // group slices (surfaces) according to number of extra perimeters
                std::map<unsigned short, Surfaces> slices;  // extra_perimeters => [ surface, surface... ]
                for (LayerRegion *layerm : layerms) {
                    for (const Surface &surface : layerm->slices().surfaces)
                        slices[surface.extra_perimeters].emplace_back(surface);
                    if (layerm->region().config().fill_density > layerm_config->region().config().fill_density)
                        layerm_config = layerm;
                    //clean list as only the layerm_config will have these ones recomputed
                    layerm->perimeters.clear();
                    layerm->thin_fills.clear();
                    layerm->fill_no_overlap_expolygons.clear();
                }
                // merge the surfaces assigned to each group
                for (std::pair<const unsigned short,Surfaces> &surfaces_with_extra_perimeters : slices)
                    new_slices.append(/*union_ex(surfaces_with_extra_perimeters.second, true) same as -?>*/offset_ex(surfaces_with_extra_perimeters.second, ClipperSafetyOffset), surfaces_with_extra_perimeters.second.front());
            }
            
            // make perimeters
            SurfaceCollection fill_surfaces;
            layerm_config->make_perimeters(new_slices, &fill_surfaces);

            // assign fill_surfaces to each LayerRegion
            if (!fill_surfaces.surfaces.empty()) { 
                for (LayerRegionPtrs::iterator l = layerms.begin(); l != layerms.end(); ++l) {
                    // Separate the fill surfaces.
                    ExPolygons slices = to_expolygons((*l)->slices().surfaces);
                    ExPolygons expp = intersection_ex(to_expolygons(fill_surfaces.surfaces), slices);
                    (*l)->fill_expolygons = expp;
                    (*l)->fill_no_overlap_expolygons = (layerm_config)->fill_no_overlap_expolygons;
                    //(*l)->perimeters = (layerm_config)->perimeters;
                    //(*l)->thin_fills = (layerm_config)->thin_fills;
                    (*l)->fill_surfaces.clear();
                    for (Surface &surf: fill_surfaces.surfaces) {
                        ExPolygons exp = intersection_ex(ExPolygons{ surf.expolygon }, slices);
                        (*l)->fill_surfaces.append(std::move(exp), surf);
                    }
                }
            }
        }
      }
    }
    BOOST_LOG_TRIVIAL(trace) << "Generating perimeters for layer " << this->id() << " - Done";
}

void Layer::make_milling_post_process() {
    if (this->object()->print()->config().milling_diameter.empty()) return;

    BOOST_LOG_TRIVIAL(trace) << "Generating milling_post_process for layer " << this->id();

    // keep track of regions whose perimeters we have already generated
    std::vector<unsigned char> done(m_regions.size(), false);

    for (LayerRegionPtrs::iterator layerm = m_regions.begin(); layerm != m_regions.end(); ++layerm) {
        if ((*layerm)->slices().empty()) {
            (*layerm)->milling.clear();
        } else {
            size_t region_id = layerm - m_regions.begin();
            if (done[region_id])
                continue;
            BOOST_LOG_TRIVIAL(trace) << "Generating milling_post_process for layer " << this->id() << ", region " << region_id;
            done[region_id] = true;
            const PrintRegionConfig& config = (*layerm)->region().config();

            // find compatible regions
            LayerRegionPtrs layerms;
            layerms.push_back(*layerm);
            for (LayerRegionPtrs::const_iterator it = layerm + 1; it != m_regions.end(); ++it) {
                LayerRegion* other_layerm = *it;
                const PrintRegionConfig& other_config = other_layerm->region().config();
                if (other_layerm->slices().empty()) continue;
                /// !!! add here the settings you want to be added in the per-object menu.
                /// if you don't do that, objects will share the same region, and the same settings.
                if (config.milling_post_process == other_config.milling_post_process
                    && config.milling_extra_size == other_config.milling_extra_size
                    && (config.milling_after_z == other_config.milling_after_z ||
                        this->bottom_z() > std::min(config.milling_after_z.get_abs_value(this->object()->print()->config().milling_diameter.values[0]),
                            other_config.milling_after_z.get_abs_value(this->object()->print()->config().milling_diameter.values[0])))) {
                    layerms.push_back(other_layerm);
                    done[it - m_regions.begin()] = true;
                }
            }

            if (layerms.size() == 1) {  // optimization
                (*layerm)->make_milling_post_process((*layerm)->slices());
            } else {
                SurfaceCollection new_slices;
                // Use a region if you ned a specific settgin, be sure to choose the good one, curtly there is o need.
                LayerRegion* layerm_config = layerms.front();
                {
                    // group slices (surfaces) according to number of extra perimeters
                    std::map<unsigned short, Surfaces> slices;  // extra_perimeters => [ surface, surface... ]
                    for (LayerRegion* layerm : layerms) {
                        for (const Surface& surface : layerm->slices().surfaces)
                            slices[surface.extra_perimeters].emplace_back(surface);
                        layerm->milling.clear();
                    }
                    // merge the surfaces assigned to each group
                    for (std::pair<const unsigned short, Surfaces>& surfaces_with_extra_perimeters : slices)
                        new_slices.append(union_safety_offset_ex(to_expolygons(surfaces_with_extra_perimeters.second)), surfaces_with_extra_perimeters.second.front());
                }

                // make perimeters
                layerm_config->make_milling_post_process(new_slices);

            }
        }
    }
    BOOST_LOG_TRIVIAL(trace) << "Generating milling_post_process for layer " << this->id() << " - Done";
}

void Layer::export_region_slices_to_svg(const char *path) const
{
    BoundingBox bbox;
    for (const LayerRegion *region : m_regions)
        for (const Surface &surface : region->slices().surfaces)
            bbox.merge(get_extents(surface.expolygon));
    Point legend_size = export_surface_type_legend_to_svg_box_size();
    Point legend_pos(bbox.min(0), bbox.max(1));
    bbox.merge(Point(std::max(bbox.min(0) + legend_size(0), bbox.max(0)), bbox.max(1) + legend_size(1)));

    SVG svg(path, bbox);
    const float transparency = 0.5f;
    for (const LayerRegion *region : m_regions)
        for (const Surface &surface : region->slices().surfaces)
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
    for (const LayerRegion *region : m_regions)
        for (const Surface &surface : region->slices().surfaces)
            bbox.merge(get_extents(surface.expolygon));
    Point legend_size = export_surface_type_legend_to_svg_box_size();
    Point legend_pos(bbox.min(0), bbox.max(1));
    bbox.merge(Point(std::max(bbox.min(0) + legend_size(0), bbox.max(0)), bbox.max(1) + legend_size(1)));

    SVG svg(path, bbox);
    const float transparency = 0.5f;
    for (const LayerRegion *region : m_regions)
        for (const Surface &surface : region->slices().surfaces)
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

void SupportLayer::simplify_support_extrusion_path() {
    const PrintConfig& print_config = this->object()->print()->config();
    const bool spiral_mode = print_config.spiral_vase;
    const bool enable_arc_fitting = print_config.arc_fitting && !spiral_mode;
    coordf_t scaled_resolution = scale_d(print_config.resolution.value);
    if (scaled_resolution == 0) scaled_resolution = enable_arc_fitting ? SCALED_EPSILON * 2 : SCALED_EPSILON;

    SimplifyVisitor visitor{ scaled_resolution , enable_arc_fitting, &print_config.arc_fitting_tolerance };
    this->support_fills.visit(visitor);
}

BoundingBox get_extents(const LayerRegion &layer_region)
{
    BoundingBox bbox;
    const Surfaces& srfs = layer_region.slices().surfaces;
    if (!srfs.empty()) {
        bbox = get_extents(srfs.front());
        for (auto it = srfs.cbegin() + 1; it != srfs.cend(); ++it)
            bbox.merge(get_extents(*it));
    }
    return bbox;
}

BoundingBox get_extents(const LayerRegionPtrs &layer_regions)
{
    BoundingBox bbox;
    if (!layer_regions.empty()) {
        bbox = get_extents(*layer_regions.front());
        for (auto it = layer_regions.begin() + 1; it != layer_regions.end(); ++it)
            bbox.merge(get_extents(**it));
    }
    return bbox;
}

}
