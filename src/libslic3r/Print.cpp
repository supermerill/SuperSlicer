#include "clipper/clipper_z.hpp"

#include "Exception.hpp"
#include "Print.hpp"
#include "BoundingBox.hpp"
#include "Brim.hpp"
#include "ClipperUtils.hpp"
#include "Extruder.hpp"
#include "Flow.hpp"
#include "Fill/FillBase.hpp"
#include "Geometry/ConvexHull.hpp"
#include "I18N.hpp"
#include "ShortestPath.hpp"
#include "SupportMaterial.hpp"
#include "Thread.hpp"
#include "GCode.hpp"
#include "GCode/WipeTower.hpp"
#include "Utils.hpp"

#include <float.h>

#include <algorithm>
#include <limits>
#include <unordered_set>
#include <boost/filesystem/path.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/regex.hpp>

// Mark string for localization and translate.
#define L(s) Slic3r::I18N::translate(s)

namespace Slic3r {

template class PrintState<PrintStep, psCount>;
template class PrintState<PrintObjectStep, posCount>;

PrintRegion::PrintRegion(const PrintRegionConfig& config) : PrintRegion(config, config.hash()) {}
PrintRegion::PrintRegion(PrintRegionConfig&& config) : PrintRegion(std::move(config), config.hash()) {}

void Print::clear() 
{
    std::scoped_lock<std::mutex> lock(this->state_mutex());
    // The following call should stop background processing if it is running.
    this->invalidate_all_steps();
	for (PrintObject *object : m_objects)
		delete object;
	m_objects.clear();
    m_print_regions.clear();
    m_model.clear_objects();
}

// Called by Print::apply().
// This method only accepts PrintConfig option keys.
bool Print::invalidate_state_by_config_options(const ConfigOptionResolver& /* new_config */, const std::vector<t_config_option_key> &opt_keys)
{
    if (opt_keys.empty())
        return false;

    // Cache the plenty of parameters, which influence the G-code generator only,
    // or they are only notes not influencing the generated G-code.
    static std::unordered_set<std::string> steps_gcode = {
        "avoid_crossing_perimeters",
        "avoid_crossing_perimeters_max_detour",
        "avoid_crossing_not_first_layer",
        "bed_shape",
        "bed_temperature",
        "chamber_temperature",
        "before_layer_gcode",
        "between_objects_gcode",
        "bridge_acceleration",
        "bridge_internal_acceleration",
        "bridge_fan_speed",
        "bridge_internal_fan_speed",
        "colorprint_heights",
        "complete_objects_sort",
        "cooling",
        "default_acceleration",
        "deretract_speed",
        "disable_fan_first_layers",
        "duplicate_distance",
        "enforce_retract_first_layer",
        "end_gcode",
        "end_filament_gcode",
        "external_perimeter_acceleration",
        "external_perimeter_cut_corners",
        "external_perimeter_fan_speed",
        "extrusion_axis",
        "extruder_clearance_height",
        "extruder_clearance_radius",
        "extruder_colour",
        "extruder_offset",
        "extruder_fan_offset"
        "extruder_temperature_offset",
        "extrusion_multiplier",
        "fan_always_on",
        "fan_below_layer_time",
        "fan_kickstart",
        "fan_speedup_overhangs",
        "fan_speedup_time",
        "fan_percentage",
        "filament_colour",
        "filament_custom_variables",
        "filament_diameter",
        "filament_density",
        "filament_notes",
        "filament_cost",
        "filament_spool_weight",
        "first_layer_acceleration",
        "first_layer_acceleration_over_raft",
        "first_layer_bed_temperature",
        "first_layer_flow_ratio",
        "first_layer_speed", // ? delete y prusa here in 2.4
        "first_layer_speed_over_raft",
        "first_layer_infill_speed",
        "first_layer_min_speed",
        "full_fan_speed_layer",
        "gap_fill_acceleration",
        "gap_fill_flow_match_perimeter",
        "gap_fill_speed",
        "gcode_comments",
        "gcode_filename_illegal_char",
        "gcode_label_objects",
        "gcode_precision_xyz",
        "gcode_precision_e",
        "infill_acceleration",
        "ironing_acceleration",
        "layer_gcode",
        "max_fan_speed",
        "max_gcode_per_second",
        "max_print_height",
        "max_print_speed",
        "max_volumetric_speed",
        "min_fan_speed",
        "min_length",
        "min_print_speed",
        "milling_toolchange_end_gcode",
        "milling_toolchange_start_gcode",
        "milling_offset",
        "milling_z_offset",
        "milling_z_lift",
#ifdef HAS_PRESSURE_EQUALIZER
        "max_volumetric_extrusion_rate_slope_positive",
        "max_volumetric_extrusion_rate_slope_negative",
#endif /* HAS_PRESSURE_EQUALIZER */
        "notes",
        "only_retract_when_crossing_perimeters",
        "output_filename_format",
        "overhangs_acceleration",
        "perimeter_acceleration",
        "post_process",
        "gcode_substitutions",
        "printer_notes",
        "retract_before_travel",
        "retract_before_wipe",
        "retract_layer_change",
        "retract_length",
        "retract_length_toolchange",
        "retract_lift",
        "retract_lift_above",
        "retract_lift_below",
        "retract_lift_first_layer",
        "retract_lift_top",
        "retract_restart_extra",
        "retract_restart_extra_toolchange",
        "retract_speed",
        "single_extruder_multi_material_priming",
        "slowdown_below_layer_time",
        "solid_infill_acceleration",
        "support_material_acceleration",
        "support_material_interface_acceleration",
        "support_material_interface_fan_speed",
        "standby_temperature_delta",
        "start_gcode",
        "start_gcode_manual",
        "start_filament_gcode",
        "thin_walls_acceleration",
        "thin_walls_speed",
        "thumbnails",
        "thumbnails_color",
        "thumbnails_custom_color",
        "thumbnails_end_file",
        "thumbnails_format",
        "thumbnails_with_bed",
        "time_estimation_compensation",
        "time_cost",
        "time_start_gcode",
        "time_toolchange",
        "tool_name",
        "toolchange_gcode",
        "top_fan_speed",
        "top_solid_infill_acceleration",
        "threads",
        "travel_acceleration",
        "travel_deceleration_use_target",
        "travel_speed",
        "travel_speed_z",
        "use_firmware_retraction",
        "use_relative_e_distances",
        "use_volumetric_e",
        "variable_layer_height",
        "wipe",
        "wipe_extra_perimeter",
        "wipe_inside_depth",
        "wipe_inside_end",
        "wipe_inside_start",
        "wipe_only_crossing",
        "wipe_speed",
    };

    static std::unordered_set<std::string> steps_ignore;

    std::vector<PrintStep> steps;
    std::vector<PrintObjectStep> osteps;
    bool invalidated = false;

    for (const t_config_option_key &opt_key : opt_keys) {
        if (steps_gcode.find(opt_key) != steps_gcode.end()) {
            // These options only affect G-code export or they are just notes without influence on the generated G-code,
            // so there is nothing to invalidate.
            steps.emplace_back(psGCodeExport);
        } else if (steps_ignore.find(opt_key) != steps_ignore.end()) {
            // These steps have no influence on the G-code whatsoever. Just ignore them.
        } else if (
               opt_key == "brim_inside_holes"
            || opt_key == "brim_width"
            || opt_key == "brim_width_interior"
            || opt_key == "brim_ears"
            || opt_key == "brim_ears_detection_length"
            || opt_key == "brim_ears_max_angle"
            || opt_key == "brim_ears_pattern"
            || opt_key == "brim_separation"
            || opt_key == "complete_objects_one_skirt"
            || opt_key == "complete_objects_one_brim"
            || opt_key == "draft_shield"
            || opt_key == "min_skirt_length"
            || opt_key == "ooze_prevention"
            || opt_key == "skirts"
            || opt_key == "skirt_height"
            || opt_key == "skirt_brim"
            || opt_key == "skirt_distance"
            || opt_key == "skirt_distance_from_brim"
            || opt_key == "wipe_tower_x"
            || opt_key == "wipe_tower_y"
            || opt_key == "wipe_tower_rotation_angle"
            ) {
            steps.emplace_back(psSkirtBrim);
        } else if (
               opt_key == "filament_shrink"
            || opt_key == "first_layer_height"
            || opt_key == "nozzle_diameter"
            || opt_key == "model_precision"
            || opt_key == "resolution"
            || opt_key == "resolution_internal"
            || opt_key == "slice_closing_radius"
            // Spiral Vase forces different kind of slicing than the normal model:
            // In Spiral Vase mode, holes are closed and only the largest area contour is kept at each layer.
            // Therefore toggling the Spiral Vase on / off requires complete reslicing.
            || opt_key == "spiral_vase"
            || opt_key == "z_step") {
            osteps.emplace_back(posSlice);
        } else if (
               opt_key == "complete_objects"
            || opt_key == "filament_type"
            || opt_key == "filament_soluble"
            || opt_key == "first_layer_temperature"
            || opt_key == "filament_loading_speed"
            || opt_key == "filament_loading_speed_start"
            || opt_key == "filament_unloading_speed"
            || opt_key == "filament_unloading_speed_start"
            || opt_key == "filament_toolchange_delay"
            || opt_key == "filament_cooling_moves"
            || opt_key == "filament_minimal_purge_on_wipe_tower"
            || opt_key == "filament_cooling_initial_speed"
            || opt_key == "filament_cooling_final_speed"
            || opt_key == "filament_ramming_parameters"
            || opt_key == "filament_max_speed"
            || opt_key == "filament_max_volumetric_speed"
            || opt_key == "filament_use_skinnydip"        // skinnydip params start
            || opt_key == "filament_use_fast_skinnydip"
            || opt_key == "filament_skinnydip_distance"
            || opt_key == "filament_melt_zone_pause"
            || opt_key == "filament_cooling_zone_pause"
            || opt_key == "filament_toolchange_temp"
            || opt_key == "filament_enable_toolchange_temp"
            || opt_key == "filament_enable_toolchange_part_fan"
            || opt_key == "filament_toolchange_part_fan_speed"
            || opt_key == "filament_dip_insertion_speed"
            || opt_key == "filament_dip_extraction_speed"    //skinnydip params end	
            || opt_key == "gcode_flavor"
            || opt_key == "high_current_on_filament_swap"
            || opt_key == "infill_first"
            || opt_key == "single_extruder_multi_material"
            || opt_key == "temperature"
            || opt_key == "wipe_tower"
            || opt_key == "wipe_tower_width"
            || opt_key == "wipe_tower_brim_width"
            || opt_key == "wipe_tower_bridging"
            || opt_key == "wipe_tower_no_sparse_layers"
            || opt_key == "wipe_tower_speed"
            || opt_key == "wipe_tower_wipe_starting_speed"
            || opt_key == "wiping_volumes_matrix"
            || opt_key == "parking_pos_retraction"
            || opt_key == "cooling_tube_retraction"
            || opt_key == "cooling_tube_length"
            || opt_key == "extra_loading_move"
            || opt_key == "travel_speed"
            || opt_key == "travel_speed_z"
            || opt_key == "first_layer_speed"
            || opt_key == "z_offset") {
            steps.emplace_back(psWipeTower);
            steps.emplace_back(psSkirtBrim);
        } else if (opt_key == "filament_soluble") {
            steps.emplace_back(psWipeTower);
            // Soluble support interface / non-soluble base interface produces non-soluble interface layers below soluble interface layers.
            // Thus switching between soluble / non-soluble interface layer material may require recalculation of supports.
            //FIXME Killing supports on any change of "filament_soluble" is rough. We should check for each object whether that is necessary.
            osteps.emplace_back(posSupportMaterial);
        } else if (
            opt_key == "first_layer_extrusion_width"
            || opt_key == "min_layer_height"
            || opt_key == "max_layer_height"
            || opt_key == "filament_max_overlap"
            || opt_key == "gcode_resolution") {
            osteps.emplace_back(posPerimeters);
            osteps.emplace_back(posInfill);
            osteps.emplace_back(posSupportMaterial);
            steps.emplace_back(psSkirtBrim);
        }
        else if (opt_key == "posSlice")
            osteps.emplace_back(posSlice);
        else if (opt_key == "posPerimeters")
            osteps.emplace_back(posPerimeters);
        else if (opt_key == "posPrepareInfill")
            osteps.emplace_back(posPrepareInfill);
        else if (opt_key == "posInfill")
            osteps.emplace_back(posInfill);
        else if (opt_key == "posSupportMaterial")
            osteps.emplace_back(posSupportMaterial);
        else if (opt_key == "posCount")
            osteps.emplace_back(posCount);
        else {
            // for legacy, if we can't handle this option let's invalidate all steps
            //FIXME invalidate all steps of all objects as well?
            invalidated |= this->invalidate_all_steps();
            // Continue with the other opt_keys to possibly invalidate any object specific steps.
        }
    }

    sort_remove_duplicates(steps);
    for (PrintStep step : steps)
        invalidated |= this->invalidate_step(step);
    sort_remove_duplicates(osteps);
    for (PrintObjectStep ostep : osteps)
        for (PrintObject *object : m_objects)
            invalidated |= object->invalidate_step(ostep);
    if(invalidated)
        m_timestamp_last_change = std::time(0);
    return invalidated;
}

bool Print::invalidate_step(PrintStep step)
{
	bool invalidated = Inherited::invalidate_step(step);
    // Propagate to dependent steps.
    if (step != psGCodeExport)
        invalidated |= Inherited::invalidate_step(psGCodeExport);
    return invalidated;
}

// returns true if an object step is done on all objects
// and there's at least one object
bool Print::is_step_done(PrintObjectStep step) const
{
    if (m_objects.empty())
        return false;
    std::scoped_lock<std::mutex> lock(this->state_mutex());
    for (const PrintObject *object : m_objects)
        if (! object->is_step_done_unguarded(step))
            return false;
    return true;
}

std::set<uint16_t> Print::object_extruders(const ConstPrintObjectPtrs& objects) const
{
    std::set<uint16_t> extruders;
    std::vector<unsigned char> region_used(m_print_regions.size(), false);
    for (const PrintObject* object : objects)
        for (const PrintRegion& region : object->all_regions())
            region.collect_object_printing_extruders(*object->print(), extruders);
    return extruders;
}

// returns 0-based indices of used extruders
std::set<uint16_t> Print::object_extruders(const PrintObjectPtrs &objects) const
{
    std::set<uint16_t> extruders;
    std::vector<unsigned char> region_used(m_print_regions.size(), false);
    for (const PrintObject *object : objects)
        for (const PrintRegion& region : object->all_regions())
            region.collect_object_printing_extruders(*object->print(), extruders);
    return extruders;
}

// returns 0-based indices of used extruders
std::set<uint16_t> Print::support_material_extruders() const
{
    std::set<uint16_t> extruders;
    bool support_uses_current_extruder = false;
    auto num_extruders = (uint16_t)m_config.nozzle_diameter.size();

    for (PrintObject *object : m_objects) {
        if (object->has_support_material()) {
        	assert(object->config().support_material_extruder >= 0);
            if (object->config().support_material_extruder == 0)
                support_uses_current_extruder = true;
            else {
                uint16_t i = (uint16_t)object->config().support_material_extruder - 1;
                extruders.insert((i >= num_extruders) ? 0 : i);
            }
            if (object->config().support_material_interface_layers > 0) {
                assert(object->config().support_material_interface_extruder >= 0);
                if (object->config().support_material_interface_extruder == 0)
                    support_uses_current_extruder = true;
                else {
                    uint16_t i = (uint16_t)object->config().support_material_interface_extruder - 1;
                    extruders.insert((i >= num_extruders) ? 0 : i);
                }
            }
        }
    }

    if (support_uses_current_extruder)
        // Add all object extruders to the support extruders as it is not know which one will be used to print supports.
        append(extruders, this->object_extruders(m_objects));
    
    return extruders;
}

// returns 0-based indices of used extruders
std::set<uint16_t> Print::extruders() const
{
    std::set<uint16_t> extruders = this->object_extruders(m_objects);
    append(extruders, this->support_material_extruders());
    return extruders;
}

uint16_t Print::num_object_instances() const
{
    uint16_t instances = 0;
    for (const PrintObject *print_object : m_objects)
        instances += (uint16_t)print_object->instances().size();
    return instances;
}

double Print::max_allowed_layer_height() const
{
    double nozzle_diameter_max = 0.;
    for (unsigned int extruder_id : this->extruders())
        nozzle_diameter_max = std::max(nozzle_diameter_max, m_config.nozzle_diameter.get_at(extruder_id));
    return nozzle_diameter_max;
}

std::vector<ObjectID> Print::print_object_ids() const 
{ 
    std::vector<ObjectID> out; 
    // Reserve one more for the caller to append the ID of the Print itself.
    out.reserve(m_objects.size() + 1);
    for (const PrintObject *print_object : m_objects)
        out.emplace_back(print_object->id());
    return out;
}

bool Print::has_infinite_skirt() const
{
    return (m_config.draft_shield.value == dsEnabled && m_config.skirts > 0) || (m_config.ooze_prevention && this->extruders().size() > 1);
}

bool Print::has_skirt() const
{
    return (m_config.skirt_height > 0 && m_config.skirts > 0) || this->has_infinite_skirt() || m_config.draft_shield.value != dsDisabled;
    // case dsLimited should only be taken into account when skirt_height and skirts are positive,
    // so it is covered by the first condition.
}

bool Print::has_brim() const
{
    return !this->m_brim.empty() || std::any_of(m_objects.begin(), m_objects.end(), [](PrintObject* object) { return object->has_brim(); });
}

bool Print::sequential_print_horizontal_clearance_valid(const Print &print, Polygons* polygons)
{
    if (print.config().extruder_clearance_radius == 0) {
        return true;
    }
    Polygons convex_hulls_other;
    if (polygons != nullptr) {
        polygons->clear();
    }
    std::vector<size_t> intersecting_idxs;

	std::map<ObjectID, Polygon> map_model_object_to_convex_hull;
    const double dist_grow = min_object_distance(static_cast<const ConfigBase*>(&print.full_print_config()), 0) * 2;
	for (const PrintObject *print_object : print.objects()) {
        const double object_grow = print.config().complete_objects_one_brim ? dist_grow : std::max(dist_grow, print_object->config().brim_width.value);
	    assert(! print_object->model_object()->instances.empty());
	    assert(! print_object->instances().empty());
	    ObjectID model_object_id = print_object->model_object()->id();
	    auto it_convex_hull = map_model_object_to_convex_hull.find(model_object_id);
        // Get convex hull of all printable volumes assigned to this print object.
        ModelInstance *model_instance0 = print_object->model_object()->instances.front();
	    if (it_convex_hull == map_model_object_to_convex_hull.end()) {
	        // Calculate the convex hull of a printable object. 
	        // Grow convex hull with the clearance margin.
	        // FIXME: Arrangement has different parameters for offsetting (jtMiter, limit 2)
	        // which causes that the warning will be showed after arrangement with the
	        // appropriate object distance. Even if I set this to jtMiter the warning still shows up.
	        it_convex_hull = map_model_object_to_convex_hull.emplace_hint(it_convex_hull, model_object_id, 
                offset(print_object->model_object()->convex_hull_2d(
	                        Geometry::assemble_transform(Vec3d{ 0.0, 0.0, model_instance0->get_offset().z() }, model_instance0->get_rotation(), model_instance0->get_scaling_factor(), model_instance0->get_mirror())),
                	// Shrink the extruder_clearance_radius a tiny bit, so that if the object arrangement algorithm placed the objects
	                // exactly by satisfying the extruder_clearance_radius, this test will not trigger collision.
	                float(scale_(0.5 * object_grow - EPSILON)),
	                jtRound, scale_t(0.1)).front());
	    }
	    // Make a copy, so it may be rotated for instances.
        //FIXME seems like the rotation isn't taken into account
	    Polygon convex_hull0 = it_convex_hull->second;
        //this can create bugs in macos, for reasons.
        const double z_diff = Geometry::rotation_diff_z(model_instance0->get_rotation(), print_object->instances().front().model_instance->get_rotation());
		if (std::abs(z_diff) > EPSILON)
			convex_hull0.rotate(z_diff);
        // Now we check that no instance of convex_hull intersects any of the previously checked object instances.
        for (const PrintInstance& instance : print_object->instances()) {
            Polygon convex_hull = convex_hull0;
            // instance.shift is a position of a centered object, while model object may not be centered.
            // Convert the shift from the PrintObject's coordinates into ModelObject's coordinates by removing the centering offset.
            convex_hull.translate(instance.shift - print_object->center_offset());
            // if output needed, collect indices (inside convex_hulls_other) of intersecting hulls
            for (size_t i = 0; i < convex_hulls_other.size(); ++i) {
                if (!intersection(convex_hulls_other[i], convex_hull).empty()) {
                    if (polygons == nullptr)
                        return false;
                    else {
                        intersecting_idxs.emplace_back(i);
                        intersecting_idxs.emplace_back(convex_hulls_other.size());
                    }
                }
            }
            convex_hulls_other.emplace_back(std::move(convex_hull));
        }

        /*
        'old' superslicer sequential_print_horizontal_clearance_valid, that is better at skirts, but need some works, as the arrange has changed.
	    // Now we check that no instance of convex_hull intersects any of the previously checked object instances.
	    for (const PrintInstance &instance : print_object->instances()) {
            Polygons convex_hull = print_object->model_object()->convex_hull_2d(
                Geometry::assemble_transform(Vec3d::Zero(),
                    instance.model_instance->get_rotation(), instance.model_instance->get_scaling_factor(), instance.model_instance->get_mirror()));
                // Shrink the extruder_clearance_radius a tiny bit, so that if the object arrangement algorithm placed the objects
                // exactly by satisfying the extruder_clearance_radius, this test will not trigger collision.
                //float(scale_(0.5 * print.config().extruder_clearance_radius.value - EPSILON)),
                //jtRound, float(scale_(0.1)));
            if (convex_hull.empty())
                continue;
	        // instance.shift is a position of a centered object, while model object may not be centered.
	        // Conver the shift from the PrintObject's coordinates into ModelObject's coordinates by removing the centering offset.
            for(Polygon &poly : convex_hull)
	            poly.translate(instance.shift - print_object->center_offset());
	        if (! intersection(
                    convex_hulls_other, 
                    offset(convex_hull[0], double(scale_(PrintConfig::min_object_distance(&instance.print_object->config(),0.)) - SCALED_EPSILON), jtRound, scale_(0.1))).empty())
	            return false;
            double extra_grow = PrintConfig::min_object_distance(&instance.print_object->config(), 1.);
            if (extra_grow > 0)
                convex_hull = offset(convex_hull, scale_(extra_grow));
	        polygons_append(convex_hulls_other, convex_hull);
	    }
        */
	}

    if (!intersecting_idxs.empty()) {
        // use collected indices (inside convex_hulls_other) to update output
        std::sort(intersecting_idxs.begin(), intersecting_idxs.end());
        intersecting_idxs.erase(std::unique(intersecting_idxs.begin(), intersecting_idxs.end()), intersecting_idxs.end());
        for (size_t i : intersecting_idxs) {
            polygons->emplace_back(std::move(convex_hulls_other[i]));
        }
        return false;
    }
    return true;
}

static inline bool sequential_print_vertical_clearance_valid(const Print &print)
{
	std::vector<const PrintInstance*> print_instances_ordered = sort_object_instances_by_model_order(print);
	// Ignore the last instance printed.
	print_instances_ordered.pop_back();
	// Find the other highest instance.
	auto it = std::max_element(print_instances_ordered.begin(), print_instances_ordered.end(), [](auto l, auto r) {
		return l->print_object->height() < r->print_object->height();
	});
    return it == print_instances_ordered.end() || (*it)->print_object->height() <= scale_(print.config().extruder_clearance_height.value);
}

double Print::get_object_first_layer_height(const PrintObject& object) const {
    //get object first layer height
    double object_first_layer_height = object.config().first_layer_height.value;
    if (object.config().first_layer_height.percent) {
        std::set<uint16_t> object_extruders;
        for (const PrintRegion& region : object.all_regions()) {
            PrintRegion::collect_object_printing_extruders(config(), object.config(), region.config(), object_extruders);
        }
        object_first_layer_height = 1000000000;
        for (uint16_t extruder_id : object_extruders) {
            double nozzle_diameter = config().nozzle_diameter.values[extruder_id];
            object_first_layer_height = std::min(object_first_layer_height, object.config().first_layer_height.get_abs_value(nozzle_diameter));
        }
    }
    return object_first_layer_height;
}

double Print::get_first_layer_height() const
{
    if (m_objects.empty())
        throw Slic3r::InvalidArgument("first_layer_height() can't be called without PrintObjects");

    double min_layer_height = 10000000000.;
    for(PrintObject* obj : m_objects)
        min_layer_height = std::fmin(min_layer_height, get_object_first_layer_height(*obj));

    if(min_layer_height == 10000000000.)
        throw Slic3r::InvalidArgument("first_layer_height() can't be computed");

    return min_layer_height;
}

// Matches "G92 E0" with various forms of writing the zero and with an optional comment.
boost::regex regex_g92e0 { "^[ \\t]*[gG]92[ \\t]*[eE](0(\\.0*)?|\\.0+)[ \\t]*(;.*)?$" };

// Precondition: Print::validate() requires the Print::apply() to be called its invocation.
std::pair<PrintBase::PrintValidationError, std::string> Print::validate(std::string* warning) const
{
    std::set<uint16_t> extruders = this->extruders();

    if (m_objects.empty())
        return { PrintBase::PrintValidationError::pveWrongPosition, L("All objects are outside of the print volume.") };

    if (extruders.empty())
        return { PrintBase::PrintValidationError::pveNoPrint, L("The supplied settings will cause an empty print.") };

    if (m_config.complete_objects) {
    	if (! sequential_print_horizontal_clearance_valid(*this))
            return { PrintBase::PrintValidationError::pveWrongPosition, L("Some objects are too close; your extruder will collide with them.") };
        if (! sequential_print_vertical_clearance_valid(*this))
            return { PrintBase::PrintValidationError::pveWrongPosition,L("Some objects are too tall and cannot be printed without extruder collisions.") };
    }

    if (m_config.spiral_vase) {
        size_t total_copies_count = 0;
        for (const PrintObject *object : m_objects)
            total_copies_count += object->instances().size();
        // #4043
        if (total_copies_count > 1 && ! m_config.complete_objects.value)
            return { PrintBase::PrintValidationError::pveWrongSettings,L("Only a single object may be printed at a time in Spiral Vase mode. "
                     "Either remove all but the last object, or enable sequential mode by \"complete_objects\".") };
        assert(m_objects.size() == 1 || config().complete_objects.value);
        if (m_objects.front()->all_regions().size() > 1)
            return { PrintBase::PrintValidationError::pveWrongSettings,L("The Spiral Vase option can only be used when printing single material objects.") };
    }

    if (this->has_wipe_tower() && ! m_objects.empty()) {
        // Make sure all extruders use same diameter filament and have the same nozzle diameter
        // EPSILON comparison is used for nozzles and 10 % tolerance is used for filaments
        double first_nozzle_diam = m_config.nozzle_diameter.get_at(*extruders.begin());
        double first_filament_diam = m_config.filament_diameter.get_at(*extruders.begin());
        for (const uint16_t& extruder_idx : extruders) {
            double nozzle_diam = m_config.nozzle_diameter.get_at(extruder_idx);
            double filament_diam = m_config.filament_diameter.get_at(extruder_idx);
            if (nozzle_diam - EPSILON > first_nozzle_diam || nozzle_diam + EPSILON < first_nozzle_diam
             || std::abs((filament_diam-first_filament_diam)/first_filament_diam) > 0.1)
                return { PrintBase::PrintValidationError::pveWrongSettings,L("The wipe tower is only supported if all extruders have the same nozzle diameter "
                         "and use filaments of the same diameter.") };
        }

        if (m_config.gcode_flavor != gcfRepRap && m_config.gcode_flavor != gcfSprinter && m_config.gcode_flavor != gcfRepetier 
            && m_config.gcode_flavor != gcfMarlinLegacy && m_config.gcode_flavor != gcfMarlinFirmware
            && m_config.gcode_flavor != gcfKlipper )
            return { PrintBase::PrintValidationError::pveWrongSettings,L("The Wipe Tower is currently only supported for the Marlin, RepRap/Sprinter and Repetier G-code flavors.") };
        if (! m_config.use_relative_e_distances)
            return { PrintBase::PrintValidationError::pveWrongSettings,L("The Wipe Tower is currently only supported with the relative extruder addressing (use_relative_e_distances=1).") };
        if (m_config.ooze_prevention)
            return { PrintBase::PrintValidationError::pveWrongSettings,L("Ooze prevention is currently not supported with the wipe tower enabled.") };
        if (m_config.use_volumetric_e)
            return { PrintBase::PrintValidationError::pveWrongSettings,L("The Wipe Tower currently does not support volumetric E (use_volumetric_e=0).") };
        if (m_config.complete_objects && extruders.size() > 1)
            return { PrintBase::PrintValidationError::pveWrongSettings,L("The Wipe Tower is currently not supported for multimaterial sequential prints.") };
        
        if (m_objects.size() > 1) {
            bool                                has_custom_layering = false;
            std::vector<std::vector<coordf_t>>  layer_height_profiles;
            for (const PrintObject *object : m_objects) {
                has_custom_layering = ! object->model_object()->layer_config_ranges.empty() || ! object->model_object()->layer_height_profile.empty();
                if (has_custom_layering) {
                    layer_height_profiles.assign(m_objects.size(), std::vector<coordf_t>());
                    break;
                }
            }
            const SlicingParameters &slicing_params0 = m_objects.front()->slicing_parameters();
            size_t            tallest_object_idx = 0;
            if (has_custom_layering)
                PrintObject::update_layer_height_profile(*m_objects.front()->model_object(), slicing_params0, layer_height_profiles.front());
            for (size_t i = 1; i < m_objects.size(); ++ i) {
                const PrintObject       *object         = m_objects[i];
                const SlicingParameters &slicing_params = object->slicing_parameters();
                if (std::abs(slicing_params.first_print_layer_height - slicing_params0.first_print_layer_height) > EPSILON ||
                    std::abs(slicing_params.layer_height             - slicing_params0.layer_height            ) > EPSILON)
                    return { PrintBase::PrintValidationError::pveWrongSettings,L("The Wipe Tower is only supported for multiple objects if they have equal layer heights") };
                if (slicing_params.raft_layers() != slicing_params0.raft_layers())
                    return { PrintBase::PrintValidationError::pveWrongSettings,L("The Wipe Tower is only supported for multiple objects if they are printed over an equal number of raft layers") };
                if (object->config().support_material_contact_distance_type != m_objects.front()->config().support_material_contact_distance_type
                    || object->config().support_material_contact_distance.value != m_objects.front()->config().support_material_contact_distance.value
                    || object->config().support_material_bottom_contact_distance.value != m_objects.front()->config().support_material_bottom_contact_distance.value
                    || slicing_params0.gap_object_support != slicing_params.gap_object_support
                    || slicing_params0.gap_support_object != slicing_params.gap_support_object)
                    return { PrintBase::PrintValidationError::pveWrongSettings,L("The Wipe Tower is only supported for multiple objects if they are printed with the same support_material_contact_distance") };
                if (! equal_layering(slicing_params, slicing_params0))
                    return { PrintBase::PrintValidationError::pveWrongSettings,L("The Wipe Tower is only supported for multiple objects if they are sliced equally.") };
                if (has_custom_layering) {
                    PrintObject::update_layer_height_profile(*object->model_object(), slicing_params, layer_height_profiles[i]);
                    if (*(layer_height_profiles[i].end()-2) > *(layer_height_profiles[tallest_object_idx].end()-2))
                        tallest_object_idx = i;
                }
            }

            if (has_custom_layering) {
                for (size_t idx_object = 0; idx_object < m_objects.size(); ++ idx_object) {
                    if (idx_object == tallest_object_idx)
                        continue;
                    // Check that the layer height profiles are equal. This will happen when one object is
                    // a copy of another, or when a layer height modifier is used the same way on both objects.
                    // The latter case might create a floating point inaccuracy mismatch, so compare
                    // element-wise using an epsilon check.
                    size_t i = 0;
                    const coordf_t eps = 0.5 * EPSILON; // layers closer than EPSILON will be merged later. Let's make
                    // this check a bit more sensitive to make sure we never consider two different layers as one.
                    while (i < layer_height_profiles[idx_object].size()
                        && i < layer_height_profiles[tallest_object_idx].size()) {
                        if (i%2 == 0 && layer_height_profiles[tallest_object_idx][i] > layer_height_profiles[idx_object][layer_height_profiles[idx_object].size() - 2 ])
                            break;
                        if (std::abs(layer_height_profiles[idx_object][i] - layer_height_profiles[tallest_object_idx][i]) > eps)
                            return { PrintBase::PrintValidationError::pveWrongSettings, L("The Wipe tower is only supported if all objects have the same variable layer height") };
                        ++i;
                    }
                }
            }
    }
    }
    
	{

		// Find the smallest used nozzle diameter and the number of unique nozzle diameters.
		double min_nozzle_diameter = std::numeric_limits<double>::max();
		double max_nozzle_diameter = 0;
		for (uint16_t extruder_id : extruders) {
			double dmr = m_config.nozzle_diameter.get_at(extruder_id);
			min_nozzle_diameter = std::min(min_nozzle_diameter, dmr);
			max_nozzle_diameter = std::max(max_nozzle_diameter, dmr);
		}

#if 0
        // We currently allow one to assign extruders with a higher index than the number
        // of physical extruders the machine is equipped with, as the Printer::apply() clamps them.
        unsigned int total_extruders_count = m_config.nozzle_diameter.size();
        for (const auto& extruder_idx : extruders)
            if ( extruder_idx >= total_extruders_count )
                return L("One or more object were assigned an extruder that the printer does not have.");
#endif

        const double print_first_layer_height = get_first_layer_height();
        for (PrintObject *object : m_objects) {
            if (object->has_support_material()) {
                if ((object->config().support_material_extruder == 0 || object->config().support_material_interface_extruder == 0) && max_nozzle_diameter - min_nozzle_diameter > EPSILON) {
                    // The object has some form of support and either support_material_extruder or support_material_interface_extruder
                    // will be printed with the current tool without a forced tool change. Play safe, assert that all object nozzles
                    // are of the same diameter.
                    return { PrintBase::PrintValidationError::pveWrongSettings,L("Printing with multiple extruders of differing nozzle diameters. "
                           "If support is to be printed with the current extruder (support_material_extruder == 0 or support_material_interface_extruder == 0), "
                           "all nozzles have to be of the same diameter.") };
                }
                if (this->has_wipe_tower()) {
                    if (object->config().support_material_contact_distance_type.value == zdNone) {
                        // Soluble interface
                        if (! object->config().support_material_synchronize_layers)
                            return { PrintBase::PrintValidationError::pveWrongSettings,L("For the Wipe Tower to work with the soluble supports, the support layers need to be synchronized with the object layers.") };
                    } else {
                        // Non-soluble interface
                        if (object->config().support_material_extruder != 0 || object->config().support_material_interface_extruder != 0)
                            return { PrintBase::PrintValidationError::pveWrongSettings,L("The Wipe Tower currently supports the non-soluble supports only if they are printed with the current extruder without triggering a tool change. "
                                     "(both support_material_extruder and support_material_interface_extruder need to be set to 0).") };
                    }
                }
            }

            // Do we have custom support data that would not be used?
            // Notify the user in that case.
            if (!object->has_support() && warning) {
                for (const ModelVolume* mv : object->model_object()->volumes) {
                    bool has_enforcers = mv->is_support_enforcer() ||
                        (mv->is_model_part() && mv->supported_facets.has_facets(*mv, EnforcerBlockerType::ENFORCER));
                    if (has_enforcers) {
                        *warning = "_SUPPORTS_OFF";
                        break;
                    }
                }
            }
            
            const double object_first_layer_height = get_object_first_layer_height(*object);
            // validate layer_height for each region
            for (const PrintRegion& region : object->all_regions()) {
                std::set<uint16_t> object_extruders;
                PrintRegion::collect_object_printing_extruders(config(), object->config(), region.config(), object_extruders);
                double layer_height = object->config().layer_height.value;
                for (uint16_t extruder_id : object_extruders) {
                    double nozzle_diameter = config().nozzle_diameter.get_at(extruder_id);
                    double min_layer_height = config().min_layer_height.get_abs_value(extruder_id, nozzle_diameter);
                    double max_layer_height = config().max_layer_height.get_abs_value(extruder_id, nozzle_diameter);
                    if (max_layer_height < EPSILON) max_layer_height = nozzle_diameter * 0.75;
                    if (min_layer_height > max_layer_height) return { PrintBase::PrintValidationError::pveWrongSettings, L("Min layer height can't be greater than Max layer height") };
                    if (max_layer_height > nozzle_diameter) return { PrintBase::PrintValidationError::pveWrongSettings, L("Max layer height can't be greater than nozzle diameter") };
                    double skirt_width = Flow::new_from_config_width(frPerimeter, 
                        *Flow::extrusion_option("skirt_extrusion_width", m_default_region_config), 
                        (float)m_config.nozzle_diameter.get_at(extruder_id), 
                        print_first_layer_height,
                        1,0 //don't care, all i want if width from width
                    ).width();
                    //check first layer layer_ranges
                    
                    if (object->shared_regions()->layer_ranges.front().layer_height_range.first < object_first_layer_height) {
                        if (object_first_layer_height + EPSILON < min_layer_height)
                            return { PrintBase::PrintValidationError::pveWrongSettings, (boost::format(L("First layer height can't be lower than %s")) % "min layer height").str() };
                        for (auto tuple : std::vector<std::pair<double, const char*>>{
                                {nozzle_diameter, "nozzle diameter"},
                                {max_layer_height, "max layer height"},
                                {skirt_width, "skirt extrusion width"},
                                {object->config().support_material ? region.width(FlowRole::frSupportMaterial, true, *object) : object_first_layer_height, "support material extrusion width"},
                                {region.width(FlowRole::frPerimeter, true, *object), "perimeter extrusion width"},
                                {region.width(FlowRole::frExternalPerimeter, true, *object), "perimeter extrusion width"},
                                {region.width(FlowRole::frInfill, true, *object), "infill extrusion width"},
                                {region.width(FlowRole::frSolidInfill, true, *object), "solid infill extrusion width"},
                                {region.width(FlowRole::frTopSolidInfill, true, *object), "top solid infill extrusion width"},
                            })
                            if (object_first_layer_height > tuple.first + EPSILON)
                                return { PrintBase::PrintValidationError::pveWrongSettings, (boost::format(L("First layer height can't be greater than %s")) % tuple.second).str() };

                    }
                    //check not-first layer
                    if (object->shared_regions()->layer_ranges.front().layer_height_range.second > layer_height) {
                        if (layer_height + EPSILON < min_layer_height)
                            return { PrintBase::PrintValidationError::pveWrongSettings, (boost::format(L("Layer height can't be lower than %s")) % "min layer height").str() };
                        for (auto tuple : std::vector<std::pair<double, const char*>>{
                                {nozzle_diameter, "nozzle diameter"},
                                {max_layer_height, "max layer height"},
                                {skirt_width, "skirt extrusion width"},
                                {object->config().support_material ? region.width(FlowRole::frSupportMaterial, false, *object) : layer_height, "support material extrusion width"},
                                {region.width(FlowRole::frPerimeter, false, *object), "perimeter extrusion width"},
                                {region.width(FlowRole::frExternalPerimeter, false, *object), "perimeter extrusion width"},
                                {region.width(FlowRole::frInfill, false, *object), "infill extrusion width"},
                                {region.width(FlowRole::frSolidInfill, false, *object), "solid infill extrusion width"},
                                {region.width(FlowRole::frTopSolidInfill, false, *object), "top solid infill extrusion width"},
                            })
                            if (layer_height > tuple.first + EPSILON)
                                return { PrintBase::PrintValidationError::pveWrongSettings, (boost::format(L("Layer height can't be greater than %s")) % tuple.second).str() };
                    }
                }
            }

        }
    }
    {
        bool before_layer_gcode_resets_extruder = boost::regex_search(m_config.before_layer_gcode.value, regex_g92e0);
        bool layer_gcode_resets_extruder        = boost::regex_search(m_config.layer_gcode.value, regex_g92e0);
        if (m_config.use_relative_e_distances) {
            // See GH issues #6336 #5073$
            //merill: should add it in gcode.cpp then!
            //if (! before_layer_gcode_resets_extruder && ! layer_gcode_resets_extruder)
            //    return { PrintBase::PrintValidationError::pveWrongSettings, L("Relative extruder addressing requires resetting the extruder position at each layer to prevent loss of floating point accuracy. Add \"G92 E0\" to layer_gcode.") };
        } else {
            if (before_layer_gcode_resets_extruder)
                return { PrintBase::PrintValidationError::pveWrongSettings, L("\"G92 E0\" was found in before_layer_gcode, which is incompatible with absolute extruder addressing.") };
            else if (layer_gcode_resets_extruder)
                return { PrintBase::PrintValidationError::pveWrongSettings, L("\"G92 E0\" was found in layer_gcode, which is incompatible with absolute extruder addressing.") };
        }
    }

    return { PrintValidationError::pveNone, std::string() };
}

#if 0
// the bounding box of objects placed in copies position
// (without taking skirt/brim/support material into account)
BoundingBox Print::bounding_box() const
{
    BoundingBox bb;
    for (const PrintObject *object : m_objects)
        for (const PrintInstance &instance : object->instances()) {
            BoundingBox bb2(object->bounding_box());
            bb.merge(bb2.min + instance.shift);
            bb.merge(bb2.max + instance.shift);
        }
    return bb;
}

// the total bounding box of extrusions, including skirt/brim/support material
// this methods needs to be called even when no steps were processed, so it should
// only use configuration values
BoundingBox Print::total_bounding_box() const
{
    // get objects bounding box
    BoundingBox bb = this->bounding_box();
    
    // we need to offset the objects bounding box by at least half the perimeters extrusion width
    Flow perimeter_flow = m_objects.front()->get_layer(0)->get_region(0)->flow(frPerimeter);
    double extra = perimeter_flow.width/2;
    
    // consider support material
    if (this->has_support_material()) {
        extra = std::max(extra, SUPPORT_MATERIAL_MARGIN);
    }
    
    // consider brim and skirt
    if (m_config.brim_width.value > 0) {
        Flow brim_flow = this->brim_flow();
        extra = std::max(extra, m_config.brim_width.value + brim_flow.width/2);
    }
    if (this->has_skirt()) {
        int skirts = m_config.skirts.value + m_config.skirt_brim.value;
        if (skirts == 0 && this->has_infinite_skirt()) skirts = 1;
        Flow skirt_flow = this->skirt_flow();
        if (m_config.skirt_distance_from_brim)
            extra += m_config.brim_width.value
                + m_config.skirt_distance.value
                + skirts * skirt_flow.spacing()
                + skirt_flow.width / 2;
        else
            extra = std::max(
                extra,
                m_config.brim_width.value
                    + m_config.skirt_distance.value
                    + skirts * skirt_flow.spacing()
                    + skirt_flow.width/2
            );
    }
    
    if (extra > 0)
        bb.offset(scale_(extra));
    
    return bb;
}
#endif

Flow Print::brim_flow(size_t extruder_id, const PrintObjectConfig& brim_config) const
{
    //use default region, but current object config.
    PrintRegionConfig tempConf = m_default_region_config;
    tempConf.parent = &brim_config;
    return Flow::new_from_config_width(
        frPerimeter,
        *Flow::extrusion_option("brim_extrusion_width", tempConf),
        (float)m_config.nozzle_diameter.get_at(extruder_id),
        (float)get_first_layer_height(),
        (extruder_id < m_config.nozzle_diameter.values.size()) ? brim_config.get_computed_value("filament_max_overlap", extruder_id) : 1
    );
}

Flow Print::skirt_flow(size_t extruder_id, bool first_layer/*=false*/) const
{
    if (m_objects.empty())
        throw Slic3r::InvalidArgument("skirt_first_layer_height() can't be called without PrintObjects");

    //get extruder used to compute first layer height
    double max_nozzle_diam = 0.f;
    for (PrintObject* pobject : m_objects) {
        PrintObject& object = *pobject;
        std::set<uint16_t> object_extruders;
        for (const PrintRegion& region : pobject->all_regions()) {
            PrintRegion::collect_object_printing_extruders(config(), object.config(), region.config(), object_extruders);
        }
        //get object first layer extruder diam
        for (uint16_t extruder_id : object_extruders) {
            double nozzle_diameter = config().nozzle_diameter.values[extruder_id];
            max_nozzle_diam = std::max(max_nozzle_diam, nozzle_diameter);
        }
    }

    
    //send m_default_object_config becasue it's the lowest config needed (extrusion_option need config from object & print)
    return Flow::new_from_config_width(
        frPerimeter,
        *Flow::extrusion_option("skirt_extrusion_width", m_default_region_config),
        (float)max_nozzle_diam,
        (float)get_first_layer_height(),
        1 // hard to say what extruder we have here(many) m_default_region_config.get_computed_value("filament_max_overlap", extruder -1),
    );
    
}

bool Print::has_support_material() const
{
    for (const PrintObject *object : m_objects)
        if (object->has_support_material()) 
            return true;
    return false;
}

/*  This method assigns extruders to the volumes having a material
    but not having extruders set in the volume config. */
void Print::auto_assign_extruders(ModelObject* model_object) const
{
    // only assign extruders if object has more than one volume
    if (model_object->volumes.size() < 2)
        return;
    
//    size_t extruders = m_config.nozzle_diameter.values.size();
    for (size_t volume_id = 0; volume_id < model_object->volumes.size(); ++ volume_id) {
        ModelVolume *volume = model_object->volumes[volume_id];
        //FIXME Vojtech: This assigns an extruder ID even to a modifier volume, if it has a material assigned.
        if ((volume->is_model_part() || volume->is_modifier()) && ! volume->material_id().empty() && ! volume->config.has("extruder"))
            volume->config.set("extruder", int(volume_id + 1));
    }
}

// Slicing process, running at a background thread.
void Print::process()
{
    m_timestamp_last_change = std::time(0);
    name_tbb_thread_pool_threads_set_locale();
    bool something_done = !is_step_done_unguarded(psSkirtBrim);
    BOOST_LOG_TRIVIAL(info) << "Starting the slicing process." << log_memory_info();
    for (PrintObject* obj : m_objects) {
        obj->make_perimeters();
    }
    //note: as object seems to be sliced independantly, it's maybe possible to add a tbb parallel_loop with simple partitioner on infill,
    //  as prepare_infill has some function not // 
    for (PrintObject* obj : m_objects) {
        obj->infill();
    }
    for (PrintObject* obj : m_objects) {
        obj->ironing();
    }
	this->set_status(50, L("Generating support material"));
    for (PrintObject* obj : m_objects) {
        obj->generate_support_material();
    }
    if (this->set_started(psWipeTower)) {
        m_wipe_tower_data.clear();
        m_tool_ordering.clear();
        if (this->has_wipe_tower()) {
            //this->set_status(45, L("Generating wipe tower"));
            this->_make_wipe_tower();
        } else if (! this->config().complete_objects.value) {
            // Initialize the tool ordering, so it could be used by the G-code preview slider for planning tool changes and filament switches.
            m_tool_ordering = ToolOrdering(*this, -1, false);
            if (m_tool_ordering.empty() || m_tool_ordering.last_extruder() == unsigned(-1))
                throw Slic3r::SlicingError("The print is empty. The model is not printable with current print settings.");
        }
        this->set_done(psWipeTower);
    }
    if (this->set_started(psSkirtBrim)) {
        this->set_status(55, L("Generating skirt and brim"));

        m_skirt.clear();
        m_skirt_first_layer.reset();
        //const bool draft_shield = config().draft_shield != dsDisabled;

        //first skirt. If it need the brim area, it will extrapolate it from config.
        m_skirt_convex_hull.clear();
        m_first_layer_convex_hull.points.clear();
        for (PrintObject *obj : m_objects) {
            obj->m_skirt.clear();
            obj->m_skirt_first_layer.reset();
        }
        if (this->has_skirt()) {
            this->set_status(55, L("Generating skirt"));
            if (config().complete_objects && !config().complete_objects_one_skirt){
                for (PrintObject *obj : m_objects) {
                    //create a skirt "pattern" (one per object)
                    const std::vector<PrintInstance> copies{ obj->instances() };
                    obj->m_instances.clear();
                    obj->m_instances.emplace_back();
                    this->_make_skirt({ obj }, obj->m_skirt, obj->m_skirt_first_layer);
                    obj->m_instances = copies;
                }
            } else {
                this->_make_skirt(m_objects, m_skirt, m_skirt_first_layer);
            }
        }

        //now brim
        m_brim.clear();
        //group object per brim settings
        m_first_layer_convex_hull.points.clear();
        std::vector<std::vector<PrintObject*>> obj_groups;
        for (PrintObject *obj : m_objects) {
            obj->m_brim.clear();
            bool added = false;
            for (std::vector<PrintObject*> &obj_group : obj_groups) {
                if (obj_group.front()->config().brim_ears.value == obj->config().brim_ears.value
                    && obj_group.front()->config().brim_ears_max_angle.value == obj->config().brim_ears_max_angle.value
                    && obj_group.front()->config().brim_ears_pattern.value == obj->config().brim_ears_pattern.value
                    && obj_group.front()->config().brim_inside_holes.value == obj->config().brim_inside_holes.value
                    && obj_group.front()->config().brim_separation.value == obj->config().brim_separation.value
                    && obj_group.front()->config().brim_width.value == obj->config().brim_width.value
                    && obj_group.front()->config().brim_width_interior.value == obj->config().brim_width_interior.value
                    && obj_group.front()->config().first_layer_extrusion_width.value == obj->config().first_layer_extrusion_width.value) {
                    added = true;
                    obj_group.push_back(obj);
                }
            }
            if (!added) {
                obj_groups.emplace_back();
                obj_groups.back().push_back(obj);
            }
        }
        ExPolygons brim_area;
        if (obj_groups.size() > 1) {
            for (std::vector<PrintObject*> &obj_group : obj_groups)
                for (const PrintObject *object : obj_group)
                    if (!object->m_layers.empty())
                        for (const PrintInstance &pt : object->m_instances) {
                            size_t first_idx = brim_area.size();
                            brim_area.insert(brim_area.end(), object->m_layers.front()->lslices.begin(), object->m_layers.front()->lslices.end());
                            for (size_t i = first_idx; i < brim_area.size(); i++) {
                                brim_area[i].translate(pt.shift.x(), pt.shift.y());
                            }
                        }
        }
        for (std::vector<PrintObject*> &obj_group : obj_groups) {
            const PrintObjectConfig &brim_config = obj_group.front()->config();
            if (brim_config.brim_width > 0 || brim_config.brim_width_interior > 0) {
                this->set_status(57, L("Generating brim"));
                if (config().complete_objects && !config().complete_objects_one_brim) {
                    for (PrintObject *obj : obj_group) {
                        //get flow
                        std::set<uint16_t> set_extruders = this->object_extruders(PrintObjectPtrs{ obj });
                        append(set_extruders, this->support_material_extruders());
                        Flow        flow = this->brim_flow(set_extruders.empty() ? get_print_region(0).config().perimeter_extruder - 1 : *set_extruders.begin(), obj->config());
                        //don't consider other objects/instances. It's not possible because it's duplicated by some code afterward... i think.
                        brim_area.clear();
                        //create a brim "pattern" (one per object)
                        const std::vector<PrintInstance> copies{ obj->instances() };
                        obj->m_instances.clear();
                        obj->m_instances.emplace_back();
                        if (brim_config.brim_width > 0) {
                            if (brim_config.brim_ears)
                                make_brim_ears(*this, flow, { obj }, brim_area, obj->m_brim);
                            else
                                make_brim(*this, flow, { obj }, brim_area, obj->m_brim);
                        }
                        if (brim_config.brim_width_interior > 0) {
                            make_brim_interior(*this, flow, { obj }, brim_area, obj->m_brim);
                        }
                        obj->m_instances = copies;
                    }
                } else {
                    if (obj_groups.size() > 1)
                        brim_area = union_ex(brim_area);
                    //get the first extruder in the list for these objects... replicating gcode generation
                    std::set<uint16_t> set_extruders = this->object_extruders(m_objects);
                    append(set_extruders, this->support_material_extruders());
                    Flow        flow = this->brim_flow(set_extruders.empty() ? get_print_region(0).config().perimeter_extruder - 1 : *set_extruders.begin(), m_default_object_config);
                    if (brim_config.brim_ears)
                        make_brim_ears(*this, flow, obj_group, brim_area, m_brim);
                    else
                        make_brim(*this, flow, obj_group, brim_area, m_brim);
                    if (brim_config.brim_width_interior > 0)
                        make_brim_interior(*this, flow, obj_group, brim_area, m_brim);
                }
            }
        }
        // store brim hull (used for make_skirt... that is made before)
        // m_first_layer_convex_hull is sued to set the 'first_layer_print_min' placeholder in gcode macros
        for (Polygon& poly : to_polygons(brim_area))
            append(m_first_layer_convex_hull.points, std::move(poly.points));
        this->finalize_first_layer_convex_hull();
        // Brim depends on skirt (brim lines are trimmed by the skirt lines), therefore if
        // the skirt gets invalidated, brim gets invalidated as well and the following line is called.
        this->set_done(psSkirtBrim);
    }
    m_timestamp_last_change = std::time(0);
    BOOST_LOG_TRIVIAL(info) << "Slicing process finished." << log_memory_info();
    //notify gui that the slicing/preview structs are ready to be drawed
    if (something_done)
        this->set_status(60, L("Slicing done"), SlicingStatus::FlagBits::SLICING_ENDED);
}

// G-code export process, running at a background thread.
// The export_gcode may die for various reasons (fails to process output_filename_format,
// write error into the G-code, cannot execute post-processing scripts).
// It is up to the caller to show an error message.
std::string Print::export_gcode(const std::string& path_template, GCodeProcessorResult* result, ThumbnailsGeneratorCallback thumbnail_cb)
{
    // output everything to a G-code file
    // The following call may die if the output_filename_format template substitution fails.
    std::string path = this->output_filepath(path_template);
    std::string message;
    if (!path.empty() && result == nullptr) {
        // Only show the path if preview_data is not set -> running from command line.
        message = L("Exporting G-code");
        message += " to ";
        message += path;
    } else
        message = L("Generating G-code");
    this->set_status(60, message);

    // The following line may die for multiple reasons.
    GCode gcode;
    gcode.do_export(this, path.c_str(), result, thumbnail_cb);
    return path.c_str();
}

void Print::_make_skirt(const PrintObjectPtrs &objects, ExtrusionEntityCollection &out, std::optional<ExtrusionEntityCollection>& out_first_layer)
{
    // First off we need to decide how tall the skirt must be.
    // The skirt_height option from config is expressed in layers, but our
    // object might have different layer heights, so we need to find the print_z
    // of the highest layer involved.
    // Note that unless has_infinite_skirt() == true
    // the actual skirt might not reach this $skirt_height_z value since the print
    // order of objects on each layer is not guaranteed and will not generally
    // include the thickest object first. It is just guaranteed that a skirt is
    // prepended to the first 'n' layers (with 'n' = skirt_height).
    // $skirt_height_z in this case is the highest possible skirt height for safety.
    coordf_t skirt_height_z = 0.;
    for (const PrintObject *object : objects) {
        size_t skirt_layers = this->has_infinite_skirt() ?
            object->layer_count() : 
            std::min(size_t(m_config.skirt_height.value), object->layer_count());
        skirt_height_z = std::max(skirt_height_z, object->m_layers[skirt_layers-1]->print_z);
    }
    // Collect points from all layers contained in skirt height.
    Points points;
    for (const PrintObject *object : objects) {
        Points object_points;
        // Get object layers up to skirt_height_z.
        for (const Layer *layer : object->m_layers) {
            if (layer->print_z > skirt_height_z)
                break;
            for (const ExPolygon &expoly : layer->lslices)
                // Collect the outer contour points only, ignore holes for the calculation of the convex hull.
                append(object_points, expoly.contour.points);
        }
        // Get support layers up to skirt_height_z.
        for (const SupportLayer *layer : object->support_layers()) {
            if (layer->print_z > skirt_height_z)
                break;
            layer->support_fills.collect_points(object_points);
        }
        // if brim, it superseed object & support for first layer
        if (config().skirt_distance_from_brim) {
            // get first layer support
            if (!object->support_layers().empty() && object->support_layers().front()->print_z == object->m_layers[0]->print_z) {
                Points support_points;
                for (const ExtrusionEntity* extrusion_entity : object->support_layers().front()->support_fills.entities()) {
                    Polylines poly;
                    extrusion_entity->collect_polylines(poly);
                    for (const Polyline& polyline : poly)
                        append(support_points, polyline.points);
                }
                Polygon hull_support = Slic3r::Geometry::convex_hull(support_points);
                for (const Polygon& poly : offset(hull_support, scale_(object->config().brim_width)))
                    append(object_points, poly.points);
            }
            // get object
            for (const ExPolygon& expoly : object->m_layers[0]->lslices)
                for (const Polygon& poly : offset(expoly.contour, scale_(object->config().brim_width)))
                    append(object_points, poly.points);
        }
        // Repeat points for each object copy.
        for (const PrintInstance &instance : object->instances()) {
            Points copy_points = object_points;
            for (Point &pt : copy_points)
                pt += instance.shift;
            append(points, copy_points);
        }
    }

    // Include the wipe tower.
    append(points, this->first_layer_wipe_tower_corners());

    // Unless draft shield is enabled, include all brims as well.
    if (config().draft_shield.value == dsDisabled)
        append(points, m_first_layer_convex_hull.points);

    if (points.size() < 3)
        // At least three points required for a convex hull.
        return;
    
    this->throw_if_canceled();
    Polygon convex_hull = Slic3r::Geometry::convex_hull(points);
    
    // Skirt may be printed on several layers, having distinct layer heights,
    // but loops must be aligned so can't vary width/spacing
    
    std::vector<size_t> extruders;
    std::vector<double> extruders_e_per_mm;
    {
        std::set<uint16_t> set_extruders = this->object_extruders(objects);
        append(set_extruders, this->support_material_extruders());
        extruders.reserve(set_extruders.size());
        extruders_e_per_mm.reserve(set_extruders.size());
        for (unsigned int extruder_id : set_extruders) {
            Flow   flow = this->skirt_flow(extruder_id);
            double mm3_per_mm = flow.mm3_per_mm();
            extruders.push_back(extruder_id);
            extruders_e_per_mm.push_back(Extruder((unsigned int)extruder_id, &m_config).e_per_mm(mm3_per_mm));
        }
    }

    // Number of skirt loops per skirt layer.
    size_t n_skirts = m_config.skirts.value;
    size_t n_skirts_first_layer = n_skirts + m_config.skirt_brim.value;
    if (this->has_infinite_skirt() && n_skirts == 0)
        n_skirts = 1;
    if (m_config.skirt_brim.value > 0)
        out_first_layer.emplace();
    // Initial offset of the brim inner edge from the object (possible with a support & raft).
    // The skirt will touch the brim if the brim is extruded.
    float distance = float(scale_(m_config.skirt_distance.value) - this->skirt_flow(extruders[extruders.size() - 1]).spacing() / 2.);


    size_t lines_per_extruder = (n_skirts + extruders.size() - 1) / extruders.size();
    size_t current_lines_per_extruder = n_skirts - lines_per_extruder * (extruders.size() - 1);

    // Draw outlines from outside to inside.
    // Loop while we have less skirts than required or any extruder hasn't reached the min length if any.
    std::vector<coordf_t> extruded_length(extruders.size(), 0.);
    for (size_t i = std::max(n_skirts, n_skirts_first_layer), extruder_idx = 0, nb_skirts = 1; i > 0; -- i) {
        bool first_layer_only = i <= (n_skirts_first_layer - n_skirts);
        Flow   flow = this->skirt_flow(extruders[extruders.size() - (1+ extruder_idx)]);
        float  spacing = flow.spacing();
        double mm3_per_mm = flow.mm3_per_mm();
        this->throw_if_canceled();
        // Offset the skirt outside.
        distance += float(scale_(spacing/2));
        // Generate the skirt centerline.
        Polygon loop;
        {
            Polygons loops = offset(convex_hull, distance, ClipperLib::jtRound, float(scale_(0.1)));
            //make sure the skirt is simple enough
            Geometry::simplify_polygons(loops, flow.scaled_width() / 10, &loops);
			if (loops.empty())
				break;
			loop = loops.front();
        }
        distance += float(scale_(spacing / 2));
        // Extrude the skirt loop.
        ExtrusionLoop eloop(elrSkirt);
        eloop.paths.emplace_back(ExtrusionPath(
            ExtrusionPath(
                erSkirt,
                (float)mm3_per_mm,         // this will be overridden at G-code export time
                flow.width(),
				(float)get_first_layer_height()  // this will be overridden at G-code export time
            )));
        eloop.paths.back().polyline = loop.split_at_first_point();
        //we make it clowkwise, but as it will be reversed, it will be ccw
        eloop.make_clockwise();
        if(!first_layer_only)
            out.append(eloop);
        if(out_first_layer)
            out_first_layer->append(eloop);
        if (m_config.min_skirt_length.value > 0 && !first_layer_only) {
            // The skirt length is limited. Sum the total amount of filament length extruded, in mm.
            extruded_length[extruder_idx] += unscale<double>(loop.length()) * extruders_e_per_mm[extruder_idx];
            if (extruded_length[extruder_idx] < m_config.min_skirt_length.value) {
                // Not extruded enough yet with the current extruder. Add another loop.
                if (i == 1 && extruded_length[extruder_idx] > 0)
                    ++ i;
            } else {
                assert(extruded_length[extruder_idx] >= m_config.min_skirt_length.value);
                // Enough extruded with the current extruder. Extrude with the next one,
                // until the prescribed number of skirt loops is extruded.
                if (extruder_idx + 1 < extruders.size()) {
                    if (nb_skirts < current_lines_per_extruder) {
                        nb_skirts++;
                    } else {
                        current_lines_per_extruder = lines_per_extruder;
                        nb_skirts = 1;
                        ++extruder_idx;
                    }
                }
            }
        } else {
            // The skirt lenght is not limited, extrude the skirt with the 1st extruder only.
        }
    }
    // Brims were generated inside out, reverse to print the outmost contour first.
    out.reverse();
    if (out_first_layer)
        out_first_layer->reverse();

    // Remember the outer edge of the last skirt line extruded as m_skirt_convex_hull.
    for (Polygon &poly : offset(convex_hull, distance + 0.5f * float(this->skirt_flow(extruders[extruders.size() - 1]).scaled_spacing()), ClipperLib::jtRound, float(scale_(0.1))))
        append(m_skirt_convex_hull, std::move(poly.points));
}

Polygons Print::first_layer_islands() const
{
    Polygons islands;
    for (PrintObject *object : m_objects) {
        Polygons object_islands;
        for (ExPolygon &expoly : object->m_layers.front()->lslices)
            object_islands.push_back(expoly.contour);
        if (! object->support_layers().empty())
            //was polygons_covered_by_spacing, but is it really important?
            object->support_layers().front()->support_fills.polygons_covered_by_width(object_islands, float(SCALED_EPSILON));
        islands.reserve(islands.size() + object_islands.size() * object->instances().size());
        for (const PrintInstance &instance : object->instances())
            for (Polygon &poly : object_islands) {
                islands.push_back(poly);
                islands.back().translate(instance.shift);
            }
    }
    return islands;
}

std::vector<Point> Print::first_layer_wipe_tower_corners() const
{
    std::vector<Point> corners;
    if (has_wipe_tower() && ! m_wipe_tower_data.tool_changes.empty()) {
        double width = m_config.wipe_tower_width + 2*m_wipe_tower_data.brim_width;
        double depth = m_wipe_tower_data.depth + 2*m_wipe_tower_data.brim_width;
        Vec2d pt0(-m_wipe_tower_data.brim_width, -m_wipe_tower_data.brim_width);
        for (Vec2d pt : {
                pt0,
                Vec2d(pt0.x()+width, pt0.y()      ),
                Vec2d(pt0.x()+width, pt0.y()+depth),
                Vec2d(pt0.x(),       pt0.y()+depth)
            }) {
            pt = Eigen::Rotation2Dd(Geometry::deg2rad(m_config.wipe_tower_rotation_angle.value)) * pt;
            pt += Vec2d(m_config.wipe_tower_x.value, m_config.wipe_tower_y.value);
            corners.emplace_back(Point(scale_(pt.x()), scale_(pt.y())));
        }
    }
    return corners;
}

void Print::finalize_first_layer_convex_hull()
{
    append(m_first_layer_convex_hull.points, m_skirt_convex_hull);
    if (m_first_layer_convex_hull.empty()) {
        // Neither skirt nor brim was extruded. Collect points of printed objects from 1st layer.
        for (Polygon &poly : this->first_layer_islands())
            append(m_first_layer_convex_hull.points, std::move(poly.points));
    }
    append(m_first_layer_convex_hull.points, this->first_layer_wipe_tower_corners());
    m_first_layer_convex_hull = Geometry::convex_hull(m_first_layer_convex_hull.points);
}

// Wipe tower support.
bool Print::has_wipe_tower() const
{
    return 
        ! m_config.spiral_vase.value &&
        m_config.wipe_tower.value && 
        m_config.nozzle_diameter.values.size() > 1;
}

const WipeTowerData& Print::wipe_tower_data(size_t extruders_cnt, double nozzle_diameter) const
{
    // If the wipe tower wasn't created yet, make sure the depth and brim_width members are set to default.
    if (! is_step_done(psWipeTower) && extruders_cnt !=0) {

        float width = float(m_config.wipe_tower_width);
		float unscaled_brim_width = m_config.wipe_tower_brim_width.get_abs_value(nozzle_diameter);

        const_cast<Print*>(this)->m_wipe_tower_data.depth = (900.f/width) * float(extruders_cnt - 1);
        const_cast<Print*>(this)->m_wipe_tower_data.brim_width = unscaled_brim_width;
    }

    return this->m_wipe_tower_data;
}

void Print::_make_wipe_tower()
{
    m_wipe_tower_data.clear();
    if (! this->has_wipe_tower())
        return;

    // Get wiping matrix to get number of extruders and convert vector<double> to vector<float>:
    std::vector<float> wiping_matrix(cast<float>(m_config.wiping_volumes_matrix.values));
    // Extract purging volumes for each extruder pair:
    std::vector<std::vector<float>> wipe_volumes;
    const unsigned int number_of_extruders = (unsigned int)(sqrt(wiping_matrix.size())+EPSILON);
    for (unsigned int i = 0; i<number_of_extruders; ++i)
        wipe_volumes.push_back(std::vector<float>(wiping_matrix.begin()+i*number_of_extruders, wiping_matrix.begin()+(i+1)*number_of_extruders));

    // Let the ToolOrdering class know there will be initial priming extrusions at the start of the print.
    m_wipe_tower_data.tool_ordering = ToolOrdering(*this, (uint16_t)-1, true);

    if (! m_wipe_tower_data.tool_ordering.has_wipe_tower())
        // Don't generate any wipe tower.
        return;

    // Check whether there are any layers in m_tool_ordering, which are marked with has_wipe_tower,
    // they print neither object, nor support. These layers are above the raft and below the object, and they
    // shall be added to the support layers to be printed.
    // see https://github.com/prusa3d/PrusaSlicer/issues/607
    {
        size_t idx_begin = size_t(-1);
        size_t idx_end   = m_wipe_tower_data.tool_ordering.layer_tools().size();
        // Find the first wipe tower layer, which does not have a counterpart in an object or a support layer.
        for (size_t i = 0; i < idx_end; ++ i) {
            const LayerTools &lt = m_wipe_tower_data.tool_ordering.layer_tools()[i];
            if (lt.has_wipe_tower && ! lt.has_object && ! lt.has_support) {
                idx_begin = i;
                break;
            }
        }
        if (idx_begin != size_t(-1)) {
            // Find the position in m_objects.first()->support_layers to insert these new support layers.
            double wipe_tower_new_layer_print_z_first = m_wipe_tower_data.tool_ordering.layer_tools()[idx_begin].print_z;
            SupportLayerPtrs::const_iterator it_layer = m_objects.front()->support_layers().begin();
            SupportLayerPtrs::const_iterator it_end   = m_objects.front()->support_layers().end();
            for (; it_layer != it_end && (*it_layer)->print_z - EPSILON < wipe_tower_new_layer_print_z_first; ++ it_layer);
            // Find the stopper of the sequence of wipe tower layers, which do not have a counterpart in an object or a support layer.
            for (size_t i = idx_begin; i < idx_end; ++ i) {
                LayerTools &lt = const_cast<LayerTools&>(m_wipe_tower_data.tool_ordering.layer_tools()[i]);
                if (! (lt.has_wipe_tower && ! lt.has_object && ! lt.has_support))
                    break;
                lt.has_support = true;
                // Insert the new support layer.
                double height    = lt.print_z - (i == 0 ? 0. : m_wipe_tower_data.tool_ordering.layer_tools()[i-1].print_z);
                //FIXME the support layer ID is set to -1, as Vojtech hopes it is not being used anyway.
                it_layer = m_objects.front()->insert_support_layer(it_layer, -1, 0, height, lt.print_z, lt.print_z - 0.5 * height);
                ++ it_layer;
            }
        }
    }
    this->throw_if_canceled();

    // Initialize the wipe tower.
    WipeTower wipe_tower(m_config, m_default_object_config, m_default_region_config, wipe_volumes, m_wipe_tower_data.tool_ordering.first_extruder());
    

    //wipe_tower.set_retract();
    //wipe_tower.set_zhop();

    // Set the extruder & material properties at the wipe tower object.
    for (size_t i = 0; i < number_of_extruders; ++i)
        wipe_tower.set_extruder(i);

    m_wipe_tower_data.priming = Slic3r::make_unique<std::vector<WipeTower::ToolChangeResult>>(
        wipe_tower.prime((float)get_first_layer_height(), m_wipe_tower_data.tool_ordering.all_extruders(), false));

    // Lets go through the wipe tower layers and determine pairs of extruder changes for each
    // to pass to wipe_tower (so that it can use it for planning the layout of the tower)
    {
        unsigned int current_extruder_id = m_wipe_tower_data.tool_ordering.all_extruders().back();
        for (auto &layer_tools : m_wipe_tower_data.tool_ordering.layer_tools()) { // for all layers
            if (!layer_tools.has_wipe_tower) continue;
            bool first_layer = &layer_tools == &m_wipe_tower_data.tool_ordering.front();
            wipe_tower.plan_toolchange((float)layer_tools.print_z, (float)layer_tools.wipe_tower_layer_height, current_extruder_id, current_extruder_id, false);
            for (const auto extruder_id : layer_tools.extruders) {
                if ((first_layer && extruder_id == m_wipe_tower_data.tool_ordering.all_extruders().back()) || extruder_id != current_extruder_id) {
                    double volume_to_wipe = wipe_volumes[current_extruder_id][extruder_id];             // total volume to wipe after this toolchange
                    
                    if (m_config.wipe_advanced) {
                        volume_to_wipe = m_config.wipe_advanced_nozzle_melted_volume;
                        float pigmentBef = m_config.filament_wipe_advanced_pigment.get_at(current_extruder_id);
                        float pigmentAft = m_config.filament_wipe_advanced_pigment.get_at(extruder_id);
                        if (m_config.wipe_advanced_algo.value == waLinear) {
                            volume_to_wipe += m_config.wipe_advanced_multiplier.value * (pigmentBef - pigmentAft);
                            BOOST_LOG_TRIVIAL(info) << "advanced wiping (lin) ";
                            BOOST_LOG_TRIVIAL(info) << current_extruder_id << " -> " << extruder_id << " will use " << volume_to_wipe << " mm3\n";
                            BOOST_LOG_TRIVIAL(info) << " calculus : " << m_config.wipe_advanced_nozzle_melted_volume << " + " << m_config.wipe_advanced_multiplier.value
                                << " * ( " << pigmentBef << " - " << pigmentAft << " )\n";
                            BOOST_LOG_TRIVIAL(info) << "    = " << m_config.wipe_advanced_nozzle_melted_volume << " + " << (m_config.wipe_advanced_multiplier.value* (pigmentBef - pigmentAft)) << "\n";
                        } else if (m_config.wipe_advanced_algo.value == waQuadra) {
                            volume_to_wipe += m_config.wipe_advanced_multiplier.value * (pigmentBef - pigmentAft)
                                + m_config.wipe_advanced_multiplier.value * (pigmentBef - pigmentAft) * (pigmentBef - pigmentAft) * (pigmentBef - pigmentAft);
                            BOOST_LOG_TRIVIAL(info) << "advanced wiping (quadra) ";
                            BOOST_LOG_TRIVIAL(info) << current_extruder_id << " -> " << extruder_id << " will use " << volume_to_wipe << " mm3\n";
                            BOOST_LOG_TRIVIAL(info) << " calculus : " << m_config.wipe_advanced_nozzle_melted_volume << " + " << m_config.wipe_advanced_multiplier.value
                                << " * ( " << pigmentBef << " - " << pigmentAft << " ) + " << m_config.wipe_advanced_multiplier.value
                                << " * ( " << pigmentBef << " - " << pigmentAft << " ) ^3 \n";
                            BOOST_LOG_TRIVIAL(info) << "    = " << m_config.wipe_advanced_nozzle_melted_volume << " + " << (m_config.wipe_advanced_multiplier.value* (pigmentBef - pigmentAft))
                                << " + " << (m_config.wipe_advanced_multiplier.value*(pigmentBef - pigmentAft)*(pigmentBef - pigmentAft)*(pigmentBef - pigmentAft))<<"\n";
                        } else if (m_config.wipe_advanced_algo.value == waHyper) {
                            volume_to_wipe += m_config.wipe_advanced_multiplier.value * (0.5 + pigmentBef) / (0.5 + pigmentAft);
                            BOOST_LOG_TRIVIAL(info) << "advanced wiping (hyper) ";
                            BOOST_LOG_TRIVIAL(info) << current_extruder_id << " -> " << extruder_id << " will use " << volume_to_wipe << " mm3\n";
                            BOOST_LOG_TRIVIAL(info) << " calculus : " << m_config.wipe_advanced_nozzle_melted_volume << " + " << m_config.wipe_advanced_multiplier.value
                                << " * ( 0.5 + " << pigmentBef << " ) / ( 0.5 + " << pigmentAft << " )\n";
                            BOOST_LOG_TRIVIAL(info) << "    = " << m_config.wipe_advanced_nozzle_melted_volume << " + " << (m_config.wipe_advanced_multiplier.value * (0.5 + pigmentBef) / (0.5 + pigmentAft)) << "\n";
                        }
                    }
                    //filament_wipe_advanced_pigment
                    
                    // Not all of that can be used for infill purging:
                    volume_to_wipe -= (float)m_config.filament_minimal_purge_on_wipe_tower.get_at(extruder_id);

                    // try to assign some infills/objects for the wiping:
                    volume_to_wipe = layer_tools.wiping_extrusions().mark_wiping_extrusions(*this, current_extruder_id, extruder_id, volume_to_wipe);

                    // add back the minimal amount toforce on the wipe tower:
                    volume_to_wipe += (float)m_config.filament_minimal_purge_on_wipe_tower.get_at(extruder_id);

                    // request a toolchange at the wipe tower with at least volume_to_wipe purging amount
                    wipe_tower.plan_toolchange((float)layer_tools.print_z, (float)layer_tools.wipe_tower_layer_height,
                        current_extruder_id, extruder_id, volume_to_wipe);
                    current_extruder_id = extruder_id;
                }
            }
            layer_tools.wiping_extrusions().ensure_perimeters_infills_order(*this);
            if (&layer_tools == &m_wipe_tower_data.tool_ordering.back() || (&layer_tools + 1)->wipe_tower_partitions == 0)
                break;
        }
    }

    // Generate the wipe tower layers.
    m_wipe_tower_data.tool_changes.reserve(m_wipe_tower_data.tool_ordering.layer_tools().size());
    wipe_tower.generate(m_wipe_tower_data.tool_changes);
    m_wipe_tower_data.depth = wipe_tower.get_depth();
    m_wipe_tower_data.brim_width = wipe_tower.get_brim_width();

    // Unload the current filament over the purge tower.
    coordf_t layer_height = m_objects.front()->config().layer_height.value;
    if (m_wipe_tower_data.tool_ordering.back().wipe_tower_partitions > 0) {
        // The wipe tower goes up to the last layer of the print.
        if (wipe_tower.layer_finished()) {
            // The wipe tower is printed to the top of the print and it has no space left for the final extruder purge.
            // Lift Z to the next layer.
            wipe_tower.set_layer(float(m_wipe_tower_data.tool_ordering.back().print_z + layer_height), float(layer_height), 0, false, true);
        } else {
            // There is yet enough space at this layer of the wipe tower for the final purge.
        }
    } else {
        // The wipe tower does not reach the last print layer, perform the pruge at the last print layer.
        assert(m_wipe_tower_data.tool_ordering.back().wipe_tower_partitions == 0);
        wipe_tower.set_layer(float(m_wipe_tower_data.tool_ordering.back().print_z), float(layer_height), 0, false, true);
    }
    m_wipe_tower_data.final_purge = Slic3r::make_unique<WipeTower::ToolChangeResult>(
        wipe_tower.tool_change((unsigned int)(-1)));

    m_wipe_tower_data.used_filament = wipe_tower.get_used_filament();
    m_wipe_tower_data.number_of_toolchanges = wipe_tower.get_number_of_toolchanges();
}

// Generate a recommended G-code output file name based on the format template, default extension, and template parameters
// (timestamps, object placeholders derived from the model, current placeholder prameters and print statistics.
// Use the final print statistics if available, or just keep the print statistics placeholders if not available yet (before G-code is finalized).
std::string Print::output_filename(const std::string &filename_base) const 
{ 
    // Set the placeholders for the data know first after the G-code export is finished.
    // These values will be just propagated into the output file name.
    DynamicConfig config = this->finished() ? this->print_statistics().config() : this->print_statistics().placeholders();
    config.set_key_value("num_extruders", new ConfigOptionInt((int)m_config.nozzle_diameter.size()));
    return this->PrintBase::output_filename(m_config.output_filename_format.value, ".gcode", filename_base, &config);
}

DynamicConfig PrintStatistics::config() const
{
    DynamicConfig config;
    std::string normal_print_time = short_time(this->estimated_normal_print_time);
    std::string silent_print_time = short_time(this->estimated_silent_print_time);
    config.set_key_value("print_time",                new ConfigOptionString(normal_print_time));
    config.set_key_value("normal_print_time",         new ConfigOptionString(normal_print_time));
    config.set_key_value("silent_print_time",         new ConfigOptionString(silent_print_time));
    config.set_key_value("used_filament",             new ConfigOptionFloat(this->total_used_filament / 1000.));
    config.set_key_value("extruded_volume",           new ConfigOptionFloat(this->total_extruded_volume));
    config.set_key_value("total_cost",                new ConfigOptionFloat(this->total_cost));
    config.set_key_value("total_toolchanges",         new ConfigOptionInt(this->total_toolchanges));
    config.set_key_value("total_weight",              new ConfigOptionFloat(this->total_weight));
    config.set_key_value("total_wipe_tower_cost",     new ConfigOptionFloat(this->total_wipe_tower_cost));
    config.set_key_value("total_wipe_tower_filament", new ConfigOptionFloat(this->total_wipe_tower_filament));
    config.set_key_value("initial_tool",              new ConfigOptionInt(int(this->initial_extruder_id)));
    config.set_key_value("initial_extruder",          new ConfigOptionInt(int(this->initial_extruder_id)));
    config.set_key_value("initial_filament_type",     new ConfigOptionString(this->initial_filament_type));
    config.set_key_value("printing_filament_types",   new ConfigOptionString(this->printing_filament_types));
    config.set_key_value("num_printing_extruders",    new ConfigOptionInt(int(this->printing_extruders.size())));
//    config.set_key_value("printing_extruders",        new ConfigOptionInts(std::vector<int>(this->printing_extruders.begin(), this->printing_extruders.end())));
    
    return config;
}

DynamicConfig PrintStatistics::placeholders()
{
    DynamicConfig config;
    for (const std::string &key : { 
        "print_time", "normal_print_time", "silent_print_time", 
        "used_filament", "extruded_volume", "total_cost", "total_weight", 
        "total_toolchanges", "total_wipe_tower_cost", "total_wipe_tower_filament",
        "initial_tool", "initial_extruder", "initial_filament_type", "printing_filament_types", "num_printing_extruders" })
        config.set_key_value(key, new ConfigOptionString(std::string("{") + key + "}"));
    return config;
}

std::string PrintStatistics::finalize_output_path(const std::string &path_in) const
{
    std::string final_path;
    try {
        boost::filesystem::path path(path_in);
        DynamicConfig cfg = this->config();
        PlaceholderParser pp;
        std::string new_stem = pp.process(path.stem().string(), 0, &cfg);
        final_path = (path.parent_path() / (new_stem + path.extension().string())).string();
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << "Failed to apply the print statistics to the export file name: " << ex.what();
        final_path = path_in;
    }
    return final_path;
}

} // namespace Slic3r
