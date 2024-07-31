///|/ Copyright (c) Prusa Research 2016 - 2023 Lukáš Matěna @lukasmatena, Tomáš Mészáros @tamasmeszaros, Enrico Turri @enricoturri1966, Vojtěch Bubník @bubnikv, Pavel Mikuš @Godrak, Oleksandra Iushchenko @YuSanka, Lukáš Hejl @hejllukas, Filip Sykala @Jony01, Roman Beránek @zavorka, David Kocík @kocikdav
///|/ Copyright (c) BambuStudio 2023 manch1n @manch1n
///|/ Copyright (c) SuperSlicer 2023 Remi Durand @supermerill
///|/ Copyright (c) 2021 Martin Budden
///|/ Copyright (c) 2020 Paul Arden @ardenpm
///|/ Copyright (c) 2019 Thomas Moore
///|/ Copyright (c) 2019 Bryan Smith
///|/ Copyright (c) Slic3r 2013 - 2016 Alessandro Ranellucci @alranel
///|/ Copyright (c) 2014 Petr Ledvina @ledvinap
///|/
///|/ ported from lib/Slic3r/Print.pm:
///|/ Copyright (c) Prusa Research 2016 - 2018 Vojtěch Bubník @bubnikv, Tomáš Mészáros @tamasmeszaros
///|/ Copyright (c) Slic3r 2011 - 2016 Alessandro Ranellucci @alranel
///|/ Copyright (c) 2012 - 2013 Mark Hindess
///|/ Copyright (c) 2013 Devin Grady
///|/ Copyright (c) 2012 - 2013 Mike Sheldrake @mesheldrake
///|/ Copyright (c) 2012 Henrik Brix Andersen @henrikbrixandersen
///|/ Copyright (c) 2012 Michael Moon
///|/ Copyright (c) 2011 Richard Goodwin
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "Exception.hpp"
#include "Print.hpp"
#include "BoundingBox.hpp"
#include "Brim.hpp"
#include "ClipperUtils.hpp"
#include "clipper/clipper_z.hpp"
#include "Extruder.hpp"
#include "Flow.hpp"
#include "Fill/FillBase.hpp"
#include "Geometry/ConvexHull.hpp"
#include "I18N.hpp"
#include "ShortestPath.hpp"
#include "Thread.hpp"
#include "GCode.hpp"
#include "GCode/WipeTower.hpp"
#include "GCode/ConflictChecker.hpp"
#include "Utils.hpp"
#include "BuildVolume.hpp"
#include "format.hpp"

#include <float.h>

#include <algorithm>
#include <limits>
#include <string>
#include <unordered_set>

#include <boost/filesystem/path.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/regex.hpp>

#include <tbb/parallel_for.h>

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
// This method only accepts PrintConfig option keys. Not PrintObjectConfig or PrintRegionConfig, go to PrintObject for these
bool Print::invalidate_state_by_config_options(const ConfigOptionResolver& /* new_config */, const std::vector<t_config_option_key> &opt_keys)
{
    if (opt_keys.empty())
        return false;


    // Cache the plenty of parameters, which influence the G-code generator only,
    // or they are only notes not influencing the generated G-code.
    static std::unordered_set<std::string> steps_gcode = {
        "allow_empty_layers",
        "autoemit_temperature_commands",
        "avoid_crossing_perimeters",
        "avoid_crossing_perimeters_max_detour",
        "avoid_crossing_not_first_layer",
        "bed_shape",
        "bed_temperature",
        "before_layer_gcode",
        "between_objects_gcode",
        "binary_gcode",
        "bridge_fan_speed",
        "chamber_temperature",
        "color_change_gcode",
        "colorprint_heights",
        "complete_objects_sort",
        "complete_objects_one_brim",
        //"cooling",
        "default_fan_speed",
        "deretract_speed",
        "disable_fan_first_layers",
        "duplicate_distance",
        "enable_dynamic_fan_speeds",
          "overhang_fan_speed_0",
          "overhang_fan_speed_1",
          "overhang_fan_speed_2",
          "overhang_fan_speed_3",
        "enforce_retract_first_layer",
        "end_gcode",
        "end_filament_gcode",
        "external_perimeter_fan_speed",
        "extrusion_axis",
        "extruder_clearance_height",
        "extruder_clearance_radius",
        "extruder_colour",
        "extruder_extrusion_multiplier_speed",
        "extruder_offset",
        "extruder_fan_offset"
        "extruder_temperature_offset",
        "extrusion_multiplier",
        "fan_below_layer_time",
        "fan_kickstart",
        "fan_speedup_overhangs",
        "fan_speedup_time",
        "feature_gcode",
        "fan_percentage",
        "fan_printer_min_speed",
        "filament_colour",
        "filament_custom_variables",
        "filament_diameter",
        "filament_density",
        "filament_load_time",
        "filament_notes",
        "filament_cost",
        "filament_spool_weight",
        "filament_unload_time",
        "filament_wipe_advanced_pigment",
        "first_layer_bed_temperature",
        "full_fan_speed_layer",
        "gap_fill_fan_speed",
        "gcode_ascii",
        "gcode_command_buffer",
        "gcode_comments",
        "gcode_filename_illegal_char",
        "gcode_label_objects",
        "gcode_min_length",
        "gcode_precision_xyz",
        "gcode_precision_e",
        "gcode_substitutions",
        "infill_fan_speed",
        "internal_bridge_fan_speed",
        "layer_gcode",
        "lift_min",
        "max_fan_speed",
        "max_gcode_per_second",
        "max_print_height",
        "max_print_speed",
        "max_speed_reduction",
        "max_volumetric_speed",
        "min_print_speed",
        "milling_diameter",
        "milling_toolchange_end_gcode",
        "milling_toolchange_start_gcode",
        "max_volumetric_extrusion_rate_slope_positive",
        "max_volumetric_extrusion_rate_slope_negative",
        "notes",
        "only_retract_when_crossing_perimeters",
        "output_filename_format",
        "overhangs_fan_speed",
        "parallel_objects_step",
        "pause_print_gcode",
        "post_process",
        "print_custom_variables",
        "printer_custom_variables",
        "perimeter_fan_speed",
        "printer_notes",
        "remaining_times",
        "remaining_times_type",
        "travel_ramping_lift",
        "travel_initial_part_length",
        "travel_slope",
        "travel_max_lift",
        "travel_lift_before_obstacle",
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
        "retract_lift_before_travel",
        "retract_restart_extra",
        "retract_restart_extra_toolchange",
        "retract_speed",
        "silent_mode",
        "single_extruder_multi_material_priming",
        "slowdown_below_layer_time",
        "solid_infill_fan_speed",
        "support_material_acceleration",
        "support_material_fan_speed",
        "support_material_interface_acceleration",
        "support_material_interface_fan_speed",
        "standby_temperature_delta",
        "start_gcode",
        "start_gcode_manual",
        "start_filament_gcode",
        "template_custom_gcode",
        "thumbnails",
        "thumbnails_color",
        "thumbnails_custom_color",
        "thumbnails_end_file",
        "thumbnails_format",
        "thumbnails_tag_format",
        "thumbnails_with_bed",
        "time_estimation_compensation",
        "time_cost",
        "time_start_gcode",
        "time_toolchange",
        "tool_name",
        "toolchange_gcode",
        "top_fan_speed",
        "threads",
        "use_firmware_retraction",
        "use_relative_e_distances",
        "use_volumetric_e",
        "variable_layer_height",
        "wipe",
        "wipe_advanced",
        "wipe_advanced_algo",
        "wipe_advanced_multiplier",
        "wipe_advanced_nozzle_melted_volume",
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
        //this one isn't even use in slicing, only for import.
        if (opt_key == "init_z_rotate")
            continue;
        if (steps_gcode.find(opt_key) != steps_gcode.end()) {
            // These options only affect G-code export or they are just notes without influence on the generated G-code,
            // so there is nothing to invalidate.
            steps.emplace_back(psGCodeExport);
        } else if (steps_ignore.find(opt_key) != steps_ignore.end()) {
            // These steps have no influence on the G-code whatsoever. Just ignore them.
        } else if (
            opt_key == "complete_objects_one_skirt"
            || opt_key == "draft_shield"
            || opt_key == "min_skirt_length"
            || opt_key == "ooze_prevention"
            || opt_key == "skirts"
            || opt_key == "skirt_brim"
            || opt_key == "skirt_distance"
            || opt_key == "skirt_distance_from_brim"
            || opt_key == "skirt_extrusion_width"
            || opt_key == "skirt_height"
            || opt_key == "wipe_tower_x"
            || opt_key == "wipe_tower_y"
            || opt_key == "wipe_tower_rotation_angle"
            ) {
            steps.emplace_back(psSkirtBrim);
        } else if (
               opt_key == "bridge_precision"
            || opt_key == "filament_shrink"
            || opt_key == "nozzle_diameter"
            || opt_key == "resolution"
            || opt_key == "resolution_internal"
            // Spiral Vase forces different kind of slicing than the normal model:
            // In Spiral Vase mode, holes are closed and only the largest area contour is kept at each layer.
            // Therefore toggling the Spiral Vase on / off requires complete reslicing.
            || opt_key == "spiral_vase"
            || opt_key == "z_step") {
            osteps.emplace_back(posSlice);
        } else if (
               opt_key == "complete_objects"
            || opt_key == "filament_type"
            || opt_key == "filament_loading_speed"
            || opt_key == "filament_loading_speed_start"
            || opt_key == "filament_unloading_speed"
            || opt_key == "filament_unloading_speed_start"
            || opt_key == "filament_toolchange_delay"
            || opt_key == "filament_cooling_moves"
            || opt_key == "filament_max_wipe_tower_speed"
            || opt_key == "filament_minimal_purge_on_wipe_tower"
            || opt_key == "filament_cooling_initial_speed"
            || opt_key == "filament_cooling_final_speed"
            || opt_key == "filament_ramming_parameters"
            || opt_key == "filament_max_speed"
            || opt_key == "filament_max_volumetric_speed"
            || opt_key == "filament_multitool_ramming"
            || opt_key == "filament_multitool_ramming_volume"
            || opt_key == "filament_multitool_ramming_flow"
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
            || opt_key == "first_layer_temperature"
            || opt_key == "gcode_flavor"
            || opt_key == "high_current_on_filament_swap"
            || opt_key == "priming_position"
            || opt_key == "single_extruder_multi_material"
            || opt_key == "temperature"
            || opt_key == "idle_temperature"
            || opt_key == "wipe_tower"
            || opt_key == "wipe_tower_width"
            || opt_key == "wipe_tower_brim_width"
            || opt_key == "wipe_tower_cone_angle"
            || opt_key == "wipe_tower_bridging"
            || opt_key == "wipe_tower_extra_spacing"
            || opt_key == "wipe_tower_no_sparse_layers"
            || opt_key == "wipe_tower_extruder"
            || opt_key == "wipe_tower_per_color_wipe"
            || opt_key == "wipe_tower_speed"
            || opt_key == "wipe_tower_wipe_starting_speed"
            || opt_key == "wiping_volumes_extruders"
            || opt_key == "wiping_volumes_matrix"
            || opt_key == "parking_pos_retraction"
            || opt_key == "cooling_tube_retraction"
            || opt_key == "cooling_tube_length"
            || opt_key == "extra_loading_move"
            || opt_key == "travel_speed"
            || opt_key == "travel_speed_z"
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
            opt_key == "arc_fitting"
            || opt_key == "arc_fitting_tolerance"
            || opt_key == "min_layer_height"
            || opt_key == "max_layer_height"
            || opt_key == "filament_max_overlap"
            || opt_key == "gcode_min_resolution") {
            osteps.emplace_back(posPerimeters);
            osteps.emplace_back(posInfill);
            osteps.emplace_back(posSimplifyPath);
            osteps.emplace_back(posSupportMaterial);
            steps.emplace_back(psSkirtBrim);
        } else if (opt_key == "seam_gap" || opt_key == "seam_gap_external") {
            osteps.emplace_back(posInfill);
        }
        else if (opt_key == "avoid_crossing_curled_overhangs")
            osteps.emplace_back(posEstimateCurledExtrusions);
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

// returns 0-based indices of used extruders
std::set<uint16_t> Print::object_extruders(const PrintObjectPtrs &objects) const
{
    std::set<uint16_t> extruders;
    for (const PrintObject *object : objects)
        for (const PrintRegion& region : object->all_regions())
            region.collect_object_printing_extruders(*object->print(), extruders);
    return extruders;
}
std::set<uint16_t> Print::object_extruders() const
{
    std::set<uint16_t> extruders;
    for (const PrintObject *object : m_objects)
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

    // The wipe tower extruder can also be set. When the wipe tower is enabled and it will be generated,
    // append its extruder into the list too.
    if (has_wipe_tower() && config().wipe_tower_extruder != 0 && extruders.size() > 1) {
        assert(config().wipe_tower_extruder > 0 && config().wipe_tower_extruder < int(config().nozzle_diameter.size()));
        extruders.insert(uint16_t(config().wipe_tower_extruder.value - 1)); // the config value is 1-based
    }

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
    return (m_config.draft_shield.value == dsEnabled && m_config.skirts > 0)/* || (m_config.ooze_prevention && this->extruders().size() > 1)*/;
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
        const double object_grow = (print.config().complete_objects && !print_object->config().brim_per_object) ? dist_grow : std::max(dist_grow, print_object->config().brim_width.value);
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
            Geometry::Transformation trafo = model_instance0->get_transformation();
            trafo.set_offset(Vec3d{ 0.0, 0.0, model_instance0->get_offset().z() });
            Polygon ch2d = print_object->model_object()->convex_hull_2d(trafo.get_matrix());
            Polygons offs_ch2d = offset(ch2d,
                // Shrink the extruder_clearance_radius a tiny bit, so that if the object arrangement algorithm placed the objects
                // exactly by satisfying the extruder_clearance_radius, this test will not trigger collision.
                scale_d(0.5 * object_grow - BuildVolume::BedEpsilon), jtRound, scale_d(0.1));
            // for invalid geometries the vector returned by offset() may be empty
            if (!offs_ch2d.empty())
                it_convex_hull = map_model_object_to_convex_hull.emplace_hint(it_convex_hull, model_object_id, offs_ch2d.front());
        }
        if (it_convex_hull != map_model_object_to_convex_hull.end()) {
            // Make a copy, so it may be rotated for instances.
            //FIXME seems like the rotation isn't taken into account
            Polygon convex_hull0 = it_convex_hull->second;
            //this can create bugs in macos, for reasons.
            const double z_diff = Geometry::rotation_diff_z(model_instance0->get_matrix(), print_object->instances().front().model_instance->get_matrix());
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
        }
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
            double nozzle_diameter = config().nozzle_diameter.get_at(extruder_id);
            object_first_layer_height = std::min(object_first_layer_height, object.config().first_layer_height.get_abs_value(nozzle_diameter));
        }
    }
    assert(object_first_layer_height < 1000000000);
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
std::pair<PrintBase::PrintValidationError, std::string> Print::validate(std::vector<std::string>* warnings) const
{
    std::set<uint16_t> extruders = this->extruders();

    if (warnings) {
        for (auto it_a = extruders.begin(); it_a != extruders.end() ;++it_a)
            for (auto it_b = std::next(it_a); it_b != extruders.end() ;++it_b)
                if (std::abs(m_config.bed_temperature.get_at(*it_a) - m_config.bed_temperature.get_at(*it_b)) > 15
                 || std::abs(m_config.first_layer_bed_temperature.get_at(*it_a) - m_config.first_layer_bed_temperature.get_at(*it_b)) > 15) {
                    warnings->emplace_back("_BED_TEMPS_DIFFER");
                    goto DONE;
                }
        DONE:;
    }

    if (m_objects.empty())
        return { PrintBase::PrintValidationError::pveWrongPosition, _u8L("All objects are outside of the print volume.") };

    if (extruders.empty())
        return { PrintBase::PrintValidationError::pveNoPrint, _u8L("The supplied settings will cause an empty print.") };

    if (m_config.complete_objects || m_config.parallel_objects_step > 0) {
    	if (! sequential_print_horizontal_clearance_valid(*this, const_cast<Polygons*>(&m_sequential_print_clearance_contours)))
            return { PrintBase::PrintValidationError::pveWrongPosition, _u8L("Some objects are too close; your extruder will collide with them.") };
        if (m_config.complete_objects && ! sequential_print_vertical_clearance_valid(*this))
            return { PrintBase::PrintValidationError::pveWrongPosition,_u8L("Some objects are too tall and cannot be printed without extruder collisions.") };
    }
    else
        const_cast<Polygons*>(&m_sequential_print_clearance_contours)->clear();

    if (m_config.avoid_crossing_perimeters && m_config.avoid_crossing_curled_overhangs) {
        return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("Avoid crossing perimeters option and avoid crossing curled overhangs option cannot be both enabled together.") };
    }    

    if (m_config.spiral_vase) {
        size_t total_copies_count = 0;
        for (const PrintObject *object : m_objects)
            total_copies_count += object->instances().size();
        // #4043
        if (total_copies_count > 1 && ! m_config.complete_objects.value)
            return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("Only a single object may be printed at a time in Spiral Vase mode. "
                     "Either remove all but the last object, or enable sequential mode by \"complete_objects\".") };
        assert(m_objects.size() == 1 || config().complete_objects.value);
        if (m_objects.front()->all_regions().size() > 1)
            return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("The Spiral Vase option can only be used when printing single material objects.") };
    }

    //if (m_config.machine_limits_usage == MachineLimitsUsage::EmitToGCode && m_config.gcode_flavor == gcfKlipper)
    //    return _u8L("Machine limits cannot be emitted to G-Code when Klipper firmware flavor is used. "
    //             "Change the value of machine_limits_usage.");

    // Cache of layer height profiles for checking:
    // 1) Whether all layers are synchronized if printing with wipe tower and / or unsynchronized supports.
    // 2) Whether layer height is constant for Organic supports.
    // 3) Whether build volume Z is not violated.
    std::vector<std::vector<coordf_t>> layer_height_profiles;
    auto layer_height_profile = [this, &layer_height_profiles](const size_t print_object_idx) -> const std::vector<coordf_t>& {
        const PrintObject       &print_object = *m_objects[print_object_idx];
        if (layer_height_profiles.empty())
            layer_height_profiles.assign(m_objects.size(), std::vector<coordf_t>());
        std::vector<coordf_t>   &profile      = layer_height_profiles[print_object_idx];
        if (profile.empty())
            PrintObject::update_layer_height_profile(*print_object.model_object(), print_object.slicing_parameters(), profile);
        return profile;
    };

    // Checks that the print does not exceed the max print height
    for (size_t print_object_idx = 0; print_object_idx < m_objects.size(); ++ print_object_idx) {
        const PrintObject &print_object = *m_objects[print_object_idx];
        //FIXME It is quite expensive to generate object layers just to get the print height!
        if (auto layers = generate_object_layers(print_object.slicing_parameters(), layer_height_profile(print_object_idx));
            ! layers.empty() && layers.back() > this->config().max_print_height + EPSILON) {
            return { PrintBase::PrintValidationError::pveWrongPosition, 
                // Test whether the last slicing plane is below or above the print volume.
                0.5 * (layers[layers.size() - 2] + layers.back()) > this->config().max_print_height + EPSILON ?
                format(_u8L("The object %1% exceeds the maximum build volume height."), print_object.model_object()->name) :
                format(_u8L("While the object %1% itself fits the build volume, its last layer exceeds the maximum build volume height."), print_object.model_object()->name) +
                " " + _u8L("You might want to reduce the size of your model or change current print settings and retry.") };
        }
    }

    // Some of the objects has variable layer height applied by painting or by a table.
    bool has_custom_layering = std::find_if(m_objects.begin(), m_objects.end(), 
        [](const PrintObject *object) { return object->model_object()->has_custom_layering(); }) 
        != m_objects.end();

    // Custom layering is not allowed for tree supports as of now.
    for (size_t print_object_idx = 0; print_object_idx < m_objects.size(); ++ print_object_idx)
        if (const PrintObject &print_object = *m_objects[print_object_idx];
            print_object.has_support_material() && print_object.config().support_material_style.value == smsOrganic &&
            print_object.model_object()->has_custom_layering()) {
            if (const std::vector<coordf_t> &layers = layer_height_profile(print_object_idx); ! layers.empty())
                if (! check_object_layers_fixed(print_object.slicing_parameters(), layers))
                    return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("Variable layer height is not supported with Organic supports.") };
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
                return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("The wipe tower is only supported if all extruders have the same nozzle diameter "
                         "and use filaments of the same diameter.") };
        }

        if (m_config.gcode_flavor != gcfRepRap 
            && m_config.gcode_flavor != gcfSprinter
            && m_config.gcode_flavor != gcfRepetier 
            && m_config.gcode_flavor != gcfMarlinLegacy
            && m_config.gcode_flavor != gcfMarlinFirmware
            && m_config.gcode_flavor != gcfKlipper )
            return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("The Wipe Tower is currently only supported for the Marlin, Klipper, RepRap/Sprinter and Repetier G-code flavors.") };
        if (! m_config.use_relative_e_distances)
            return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("The Wipe Tower is currently only supported with the relative extruder addressing (use_relative_e_distances=1).") };
        if (m_config.ooze_prevention && m_config.single_extruder_multi_material)
            return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("Ooze prevention is currently not supported with the wipe tower enabled.") };
        if (m_config.use_volumetric_e)
            return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("The Wipe Tower currently does not support volumetric E (use_volumetric_e=0).") };
        if (m_config.complete_objects && extruders.size() > 1)
            return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("The Wipe Tower is currently not supported for multimaterial sequential prints.") };
        
        if (m_objects.size() > 1) {
            const SlicingParameters     &slicing_params0       = m_objects.front()->slicing_parameters();
            size_t                       tallest_object_idx    = 0;
            for (size_t i = 1; i < m_objects.size(); ++ i) {
                const PrintObject       *object         = m_objects[i];
                const SlicingParameters &slicing_params = object->slicing_parameters();
                if (std::abs(slicing_params.first_print_layer_height - slicing_params0.first_print_layer_height) > EPSILON ||
                    std::abs(slicing_params.layer_height             - slicing_params0.layer_height            ) > EPSILON)
                    return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("The Wipe Tower is only supported for multiple objects if they have equal layer heights") };
                if (slicing_params.raft_layers() != slicing_params0.raft_layers())
                    return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("The Wipe Tower is only supported for multiple objects if they are printed over an equal number of raft layers") };
                if (object->config().support_material_contact_distance_type != m_objects.front()->config().support_material_contact_distance_type
                    || object->config().support_material_contact_distance.value != m_objects.front()->config().support_material_contact_distance.value
                    || object->config().support_material_bottom_contact_distance.value != m_objects.front()->config().support_material_bottom_contact_distance.value
                    || slicing_params0.gap_object_support != slicing_params.gap_object_support
                    || slicing_params0.gap_support_object != slicing_params.gap_support_object)
                    return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("The Wipe Tower is only supported for multiple objects if they are printed with the same support_material_contact_distance") };
                if (! equal_layering(slicing_params, slicing_params0))
                    return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("The Wipe Tower is only supported for multiple objects if they are sliced equally.") };
                if (has_custom_layering) {
                    auto &lh         = layer_height_profile(i);
                    auto &lh_tallest = layer_height_profile(tallest_object_idx);
                    if (*(lh.end()-2) > *(lh_tallest.end()-2))
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
                            return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("The Wipe tower is only supported if all objects have the same variable layer height") };
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
                return _u8L("One or more object were assigned an extruder that the printer does not have.");
#endif

        const double print_first_layer_height = get_first_layer_height();
        for (PrintObject *object : m_objects) {
            if (object->has_support_material()) {
                if ((object->config().support_material_extruder == 0 || object->config().support_material_interface_extruder == 0) && max_nozzle_diameter - min_nozzle_diameter > EPSILON) {
                    // The object has some form of support and either support_material_extruder or support_material_interface_extruder
                    // will be printed with the current tool without a forced tool change. Play safe, assert that all object nozzles
                    // are of the same diameter.
                    return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("Printing with multiple extruders of differing nozzle diameters. "
                           "If support is to be printed with the current extruder (support_material_extruder == 0 or support_material_interface_extruder == 0), "
                           "all nozzles have to be of the same diameter.") };
                }
                if (this->has_wipe_tower() && object->config().support_material_style != smsOrganic) {
                    if (object->config().support_material_contact_distance_type.value == zdNone) {
                        // Soluble interface
                        if (! object->config().support_material_synchronize_layers)
                            return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("For the Wipe Tower to work with the soluble supports, the support layers need to be synchronized with the object layers.") };
                    } else {
                        // Non-soluble interface
                        if (object->config().support_material_extruder != 0 || object->config().support_material_interface_extruder != 0)
                            return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("The Wipe Tower currently supports the non-soluble supports only if they are printed with the current extruder without triggering a tool change. "
                                     "(both support_material_extruder and support_material_interface_extruder need to be set to 0).") };
                    }
                }
                if (object->config().support_material_style.value == smsOrganic) {
                    float extrusion_width = std::min(
                        support_material_flow(object).width(),
                        support_material_interface_flow(object).width());
                    if (object->config().support_tree_tip_diameter < extrusion_width - EPSILON)
                        return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("Organic support tree tip diameter must not be smaller than support material extrusion width.") };
                    if (object->config().support_tree_branch_diameter < 2. * extrusion_width - EPSILON)
                        return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("Organic support branch diameter must not be smaller than 2x support material extrusion width.") };
                    if (object->config().support_tree_branch_diameter < object->config().support_tree_tip_diameter)
                        return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("Organic support branch diameter must not be smaller than support tree tip diameter.") };
                }
            }

            // Do we have custom support data that would not be used?
            // Notify the user in that case.
            if (! object->has_support() && warnings) {
                for (const ModelVolume* mv : object->model_object()->volumes) {
                    bool has_enforcers = mv->is_support_enforcer() ||
                        (mv->is_model_part() && mv->supported_facets.has_facets(*mv, EnforcerBlockerType::ENFORCER));
                    if (has_enforcers) {
                        warnings->emplace_back("_SUPPORTS_OFF");
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
                    if (min_layer_height > max_layer_height) return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("Min layer height can't be greater than Max layer height") };
                    if (max_layer_height > nozzle_diameter) return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("Max layer height can't be greater than nozzle diameter") };
                    double skirt_width = Flow::new_from_config_width(frPerimeter,
                        *Flow::extrusion_width_option("skirt", m_default_region_config),
                        *Flow::extrusion_spacing_option("skirt", m_default_region_config),
                        (float)m_config.nozzle_diameter.get_at(extruder_id), 
                        print_first_layer_height,
                        1,0 //don't care, all i want if width from width
                    ).width();
                    //check first layer layer_ranges
                    
                    if (object->shared_regions()->layer_ranges.front().layer_height_range.first < object_first_layer_height) {
                        if (object_first_layer_height + EPSILON < min_layer_height)
                            return { PrintBase::PrintValidationError::pveWrongSettings, format(_u8L("First layer height can't be lower than %s"), "min layer height") };
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
                                return { PrintBase::PrintValidationError::pveWrongSettings, format(_u8L("First layer height can't be greater than %s"), tuple.second) };

                    }
                    //check not-first layer
                    if (object->shared_regions()->layer_ranges.front().layer_height_range.second > layer_height) {
                        if (layer_height + EPSILON < min_layer_height)
                            return { PrintBase::PrintValidationError::pveWrongSettings, format(_u8L("Layer height can't be lower than %s"), "min layer height") };
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
                                return { PrintBase::PrintValidationError::pveWrongSettings, format(_u8L("Layer height can't be greater than %s"), tuple.second) };
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
            //    return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("Relative extruder addressing requires resetting the extruder position at each layer to prevent loss of floating point accuracy. Add \"G92 E0\" to layer_gcode.") };
        } else {
            if (before_layer_gcode_resets_extruder)
                return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("\"G92 E0\" was found in before_layer_gcode, which is incompatible with absolute extruder addressing.") };
            else if (layer_gcode_resets_extruder)
                return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("\"G92 E0\" was found in layer_gcode, which is incompatible with absolute extruder addressing.") };
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
        double max_skirt_width = 0;
        double max_skirt_spacing = 0;
        for (unsigned int extruder_id : this->object_extruders()) {
            Flow   flow = this->skirt_flow(extruder_id);
            max_skirt_width = std::max(max_skirt_width, flow.width());
            max_skirt_spacing = std::max(max_skirt_spacing, flow.spacing());
        }
        if (m_config.skirt_distance_from_brim)
            extra += m_config.brim_width.value
                + m_config.skirt_distance.value
                + skirts * max_skirt_spacing
                + max_skirt_width / 2;
        else
            extra += std::max(
                m_config.brim_width.value,
                m_config.skirt_distance.value
                    + skirts * max_skirt_spacing
                    + max_skirt_width / 2);
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
        *Flow::extrusion_width_option("brim", tempConf),
        *Flow::extrusion_spacing_option("brim", tempConf),
        (float)m_config.nozzle_diameter.get_at(extruder_id),
        (float)get_first_layer_height(),
        (extruder_id < m_config.nozzle_diameter.size()) ? brim_config.get_computed_value("filament_max_overlap", extruder_id) : 1
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
            double nozzle_diameter = config().nozzle_diameter.get_at(extruder_id);
            max_nozzle_diam = std::max(max_nozzle_diam, nozzle_diameter);
        }
    }

    
    //send m_default_object_config becasue it's the lowest config needed (extrusion_option need config from object & print)
    return Flow::new_from_config_width(
        frPerimeter,
        *Flow::extrusion_width_option("skirt", m_default_region_config),
        *Flow::extrusion_spacing_option("skirt", m_default_region_config),
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
    
//    size_t extruders = m_config.nozzle_diameter.size();
    for (size_t volume_id = 0; volume_id < model_object->volumes.size(); ++ volume_id) {
        ModelVolume *volume = model_object->volumes[volume_id];
        //FIXME Vojtech: This assigns an extruder ID even to a modifier volume, if it has a material assigned.
        if ((volume->is_model_part() || volume->is_modifier()) && ! volume->material_id().empty() && ! volume->config.has("extruder"))
            volume->config.set("extruder", int(volume_id + 1));
    }
}

#ifdef _DEBUG
class CheckOrientation : public ExtrusionVisitorRecursiveConst
{
public:
    bool ccw;
    CheckOrientation(bool is_ccw) : ExtrusionVisitorRecursiveConst() {ccw = (is_ccw);}
    void use(const ExtrusionLoop &loop) override {
        assert(loop.is_counter_clockwise() == ccw);
    }
};
#endif

// Slicing process, running at a background thread.
void Print::process()
{
    m_timestamp_last_change = std::time(0);
    name_tbb_thread_pool_threads_set_locale();
    bool something_done = !is_step_done_unguarded(psSkirtBrim);
    BOOST_LOG_TRIVIAL(info) << "Starting the slicing process." << log_memory_info();

    tbb::parallel_for(tbb::blocked_range<size_t>(0, m_objects.size(), 1), [this](const tbb::blocked_range<size_t> &range) {
        for (size_t idx = range.begin(); idx < range.end(); ++idx) {
            m_objects[idx]->make_perimeters();
            m_objects[idx]->infill();
            m_objects[idx]->ironing();
        }
    }, tbb::simple_partitioner());

    // The following step writes to m_shared_regions, it should not run in parallel.
    for (PrintObject *obj : m_objects)
        obj->generate_support_spots();
    // check data from previous step, format the error message(s) and send alert to ui
    // this also has to be done sequentially.
    alert_when_supports_needed();

    this->set_status(50, L("Generating support material"));
    tbb::parallel_for(tbb::blocked_range<size_t>(0, m_objects.size(), 1), [this](const tbb::blocked_range<size_t> &range) {
        for (size_t idx = range.begin(); idx < range.end(); ++idx) {
            PrintObject &obj = *m_objects[idx];
            obj.generate_support_material();
            obj.estimate_curled_extrusions();
            obj.calculate_overhanging_perimeters();
        }
    }, tbb::simple_partitioner());

    if (this->set_started(psWipeTower)) {
        m_wipe_tower_data.clear();
        m_tool_ordering.clear();
        if (this->has_wipe_tower()) {
            //this->set_status(50, _u8L("Generating wipe tower"));
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
        this->set_status(60, L("Generating skirt and brim"));

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
            this->set_status(60, L("Generating skirt"));
            if (config().complete_objects && !config().complete_objects_one_skirt){
                for (PrintObject *obj : m_objects) {
                    //create a skirt "pattern" (one per object)
                    const std::vector<PrintInstance> copies{ obj->instances() };
                    obj->m_instances.clear();
                    obj->m_instances.emplace_back();
                    this->_make_skirt({ obj }, obj->m_skirt, obj->m_skirt_first_layer);
                    obj->m_instances = copies;
#ifdef _DEBUG
                    obj->m_skirt.start_visit(CheckOrientation(true));
#endif
                }
            } else {
                this->_make_skirt(m_objects, m_skirt, m_skirt_first_layer);
#ifdef _DEBUG
                m_skirt.start_visit(CheckOrientation(true));
#endif
            }
        }

        //now brim
        m_brim.clear();
        //group object per brim settings
        m_first_layer_convex_hull.points.clear();
        std::vector<std::vector<PrintObject*>> obj_groups;
        bool brim_per_object = false;
        for (PrintObject *obj : m_objects) {
            obj->m_brim.clear();
            brim_per_object = brim_per_object || obj->config().brim_per_object.value;
            bool added = false;
            for (std::vector<PrintObject*> &obj_group : obj_groups) {
                if (obj_group.front()->config().brim_ears.value == obj->config().brim_ears.value
                    && obj_group.front()->config().brim_ears_max_angle.value == obj->config().brim_ears_max_angle.value
                    && obj_group.front()->config().brim_ears_pattern.value == obj->config().brim_ears_pattern.value
                    && obj_group.front()->config().brim_inside_holes.value == obj->config().brim_inside_holes.value
                    && obj_group.front()->config().brim_per_object.value == obj->config().brim_per_object.value
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
        //get the objects areas, to not print brim on it (if needed)
        if (obj_groups.size() > 1 || brim_per_object) {
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
        //print brim per brim region
        for (std::vector<PrintObject*> &obj_group : obj_groups) {
            const PrintObjectConfig &brim_config = obj_group.front()->config();
            if (brim_config.brim_width > 0 || brim_config.brim_width_interior > 0) {
                this->set_status(70, L("Generating brim"));
                if (brim_config.brim_per_object) {
                    for (PrintObject *obj : obj_group) {
                        //get flow
                        std::set<uint16_t> set_extruders = this->object_extruders(PrintObjectPtrs{ obj });
                        append(set_extruders, this->support_material_extruders());
                        Flow        flow = this->brim_flow(set_extruders.empty() ? get_print_region(0).config().perimeter_extruder - 1 : *set_extruders.begin(), obj->config());
                        //if complete objects
                        if (config().complete_objects) {
                            //don't consider other objects/instances, as they aren't colliding.
                            brim_area.clear();
                            const std::vector<PrintInstance> copies = obj->instances();
                            obj->m_instances.clear();
                            obj->m_instances.emplace_back();
                            //create a brim "pattern" (one per object)
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
                        } else {
                            brim_area = union_ex(brim_area);
                            // create a brim per instance
                            const std::vector<PrintInstance> copies = obj->instances();
                            for (const PrintInstance& instance : copies) {
                                obj->m_instances.clear();
                                obj->m_instances.push_back(instance);
                                ExtrusionEntityCollection entity_brim;
                                if (brim_config.brim_width > 0) {
                                    if (brim_config.brim_ears)
                                        make_brim_ears(*this, flow, { obj }, brim_area, entity_brim);
                                    else
                                        make_brim(*this, flow, { obj }, brim_area, entity_brim);
                                }
                                if (brim_config.brim_width_interior > 0) {
                                    make_brim_interior(*this, flow, { obj }, brim_area, entity_brim);
                                }
                                obj->m_brim.append(std::move(entity_brim));
                            }
                            obj->m_instances = copies;
                        }
                    }
                } else {
                    if (obj_groups.size() > 1) {
                        brim_area = union_ex(brim_area);
                    }
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

    if (this->has_wipe_tower()) {
        // These values have to be updated here, not during wipe tower generation.
        // When the wipe tower is moved/rotated, it is not regenerated.
        m_wipe_tower_data.position = { m_config.wipe_tower_x, m_config.wipe_tower_y };
        m_wipe_tower_data.rotation_angle = m_config.wipe_tower_rotation_angle;
    }
    auto conflictRes = ConflictChecker::find_inter_of_lines_in_diff_objs(objects(), m_wipe_tower_data);

    m_conflict_result = conflictRes;
    if (conflictRes.has_value())
        BOOST_LOG_TRIVIAL(error) << boost::format("gcode path conflicts found between %1% and %2%") % conflictRes->_objName1 % conflictRes->_objName2;

#ifdef _DEBUG
    struct PointAssertVisitor : public ExtrusionVisitorRecursiveConst {
        virtual void default_use(const ExtrusionEntity& entity) override {};
        virtual void use(const ExtrusionPath &path) override {
            for (size_t idx = 1; idx < path.size(); ++idx)
                assert(!path.polyline.get_point(idx - 1).coincides_with_epsilon(path.polyline.get_point(idx)));
        }
        virtual void use(const ExtrusionLoop& loop) override {
            Point last_pt = loop.last_point();
            for (const ExtrusionPath &path : loop.paths) {
                assert(path.polyline.size() >= 2);
                assert(path.first_point() == last_pt);
                for (size_t idx = 1; idx < path.size(); ++idx)
                    assert(!path.polyline.get_point(idx - 1).coincides_with_epsilon(path.polyline.get_point(idx)));
                last_pt = path.last_point();
            }
            assert(loop.paths.front().first_point() == loop.paths.back().last_point());
        }
    } ptvisitor;
    for (PrintObject* obj : m_objects)
        for (Layer* lay : obj->layers())
            for (LayerRegion* lr : lay->regions())
                lr->perimeters().visit(ptvisitor);
#endif
    //simplify / make arc fitting
    {
        const bool spiral_mode = config().spiral_vase;
        const bool enable_arc_fitting = config().arc_fitting.value != ArcFittingType::Disabled && !spiral_mode;
        if (enable_arc_fitting) {
            this->set_status(80, L("Creating arcs"));
        } else {
            this->set_status(80, L("Simplifying paths"));
        }
        for (PrintObject* obj : m_objects) {
            obj->simplify_extrusion_path();
        }
        //also simplify object skirt & brim
        if (enable_arc_fitting && (!this->m_skirt.empty() || !this->m_brim.empty())) {
            coordf_t scaled_resolution = scale_d(config().resolution.value);
            if (scaled_resolution == 0) scaled_resolution = enable_arc_fitting ? SCALED_EPSILON * 2 : SCALED_EPSILON;
            const ConfigOptionFloatOrPercent& arc_fitting_tolerance = config().arc_fitting_tolerance;

            this->set_status(0, L("Optimizing skirt & brim %s%%"), { std::to_string(0) }, PrintBase::SlicingStatus::SECONDARY_STATE);
            std::atomic<int> atomic_count{ 0 };
            GetPathsVisitor visitor;
            this->m_skirt.visit(visitor);
            this->m_brim.visit(visitor);
            tbb::parallel_for(
                tbb::blocked_range<size_t>(0, visitor.paths.size() + visitor.paths3D.size()),
                [this, &visitor, scaled_resolution, &arc_fitting_tolerance, &atomic_count](const tbb::blocked_range<size_t>& range) {
                    size_t path_idx = range.begin();
                    for (; path_idx < range.end() && path_idx < visitor.paths.size(); ++path_idx) {
                        visitor.paths[path_idx]->simplify(scaled_resolution, config().arc_fitting.value, scale_d(arc_fitting_tolerance.get_abs_value(visitor.paths[path_idx]->width())));
                        int nb_items_done = (++atomic_count);
                        this->set_status(int((nb_items_done * 100) / (visitor.paths.size() + visitor.paths3D.size())), L("Optimizing skirt & brim %s%%"), { std::to_string(int(100*nb_items_done / double(visitor.paths.size() + visitor.paths3D.size()))) }, PrintBase::SlicingStatus::SECONDARY_STATE);
                    }
                    for (; path_idx < range.end() && path_idx - visitor.paths.size() < visitor.paths3D.size(); ++path_idx) {
                        visitor.paths3D[path_idx - visitor.paths.size()]->simplify(scaled_resolution, config().arc_fitting.value, scale_d(arc_fitting_tolerance.get_abs_value(visitor.paths[path_idx]->width())));
                        int nb_items_done = (++atomic_count);
                        this->set_status(int((nb_items_done * 100) / (visitor.paths.size() + visitor.paths3D.size())), L("Optimizing skirt & brim %s%%"), { std::to_string(int(100*nb_items_done / double(visitor.paths.size() + visitor.paths3D.size()))) }, PrintBase::SlicingStatus::SECONDARY_STATE);
                    }
                }
            );
        }
    }
    
#if _DEBUG
    for (PrintObject* obj : m_objects) {
        for (auto &l : obj->m_layers) {
            for (auto &reg : l->regions()) { LoopAssertVisitor lav; reg->perimeters().visit(lav); }
        }
    }
#endif

    m_timestamp_last_change = std::time(0);
    BOOST_LOG_TRIVIAL(info) << "Slicing process finished." << log_memory_info();
    //notify gui that the slicing/preview structs are ready to be drawed
    if (something_done)
        this->set_status(85, L("Slicing done"), SlicingStatus::FlagBits::SLICING_ENDED);
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
        message = _u8L("Exporting G-code");
        message += " to ";
        message += path;
    } else
        message = _u8L("Generating G-code");
    this->set_status(90, message);

    // Create GCode on heap, it has quite a lot of data.
    std::unique_ptr<GCodeGenerator> gcode(new GCodeGenerator());
    gcode->do_export(this, path.c_str(), result, thumbnail_cb);

    if (m_conflict_result.has_value())
        result->conflict_result = *m_conflict_result;

    return path.c_str();
}

void Print::_make_skirt(const PrintObjectPtrs &objects, ExtrusionEntityCollection &out, std::optional<ExtrusionEntityCollection>& out_first_layer)
{
    assert(out.empty());
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
                    ArcPolylines polys;
                    extrusion_entity->collect_polylines(polys);
                    for (const ArcPolyline &polyline : polys) {
                        assert(!polyline.has_arc());
                        append(support_points, polyline.to_polyline().points);
                    }
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
            extruders_e_per_mm.push_back(Extruder((unsigned int)extruder_id, m_config).e_per_mm(mm3_per_mm));
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
        eloop.paths.emplace_back(
            ExtrusionAttributes{
                ExtrusionRole::Skirt,
                ExtrusionFlow{
                    float(mm3_per_mm),        // this will be overridden at G-code export time
                    flow.width(),
                    float(get_first_layer_height()) // this will be overridden at G-code export time
                }
            },
            false
        );
        eloop.paths.back().polyline = loop.split_at_first_point();
        //we make it counter-clowkwise, as loop aren't reversed
        //eloop.make_clockwise();
        if(eloop.is_clockwise())
            eloop.reverse();
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
#ifdef _DEBUG
    out.start_visit(CheckOrientation{true});
#endif
    // Brims were generated inside out, reverse to print the outmost contour first.
    out.reverse();
    if (out_first_layer)
        out_first_layer->reverse();
#ifdef _DEBUG
    out.start_visit(CheckOrientation(true));
#endif

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

Points Print::first_layer_wipe_tower_corners() const
{
    Points pts_scaled;

    if (has_wipe_tower() && ! m_wipe_tower_data.tool_changes.empty()) {
        double width = m_config.wipe_tower_width + 2*m_wipe_tower_data.brim_width;
        double depth = m_wipe_tower_data.depth + 2*m_wipe_tower_data.brim_width;
        Vec2d pt0(-m_wipe_tower_data.brim_width, -m_wipe_tower_data.brim_width);
        
        // First the corners.
        std::vector<Vec2d> pts = { pt0,
                                   Vec2d(pt0.x()+width, pt0.y()),
                                   Vec2d(pt0.x()+width, pt0.y()+depth),
                                   Vec2d(pt0.x(),pt0.y()+depth)
                                 };

        // Now the stabilization cone.
        Vec2d center = (pts[0] + pts[2])/2.;
        const auto [cone_R, cone_x_scale] = WipeTower::get_wipe_tower_cone_base(m_config.wipe_tower_width, m_wipe_tower_data.height, m_wipe_tower_data.depth, m_config.wipe_tower_cone_angle);
        double r = cone_R + m_wipe_tower_data.brim_width;
        for (double alpha = 0.; alpha<2*M_PI; alpha += M_PI/20.)
            pts.emplace_back(center + r*Vec2d(std::cos(alpha)/cone_x_scale, std::sin(alpha)));

        for (Vec2d& pt : pts) {
            pt = Eigen::Rotation2Dd(Geometry::deg2rad(m_config.wipe_tower_rotation_angle.value)) * pt;
            pt += Vec2d(m_config.wipe_tower_x.value, m_config.wipe_tower_y.value);
            pts_scaled.emplace_back(Point(scale_(pt.x()), scale_(pt.y())));
        }
    }
    return pts_scaled;
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

void Print::alert_when_supports_needed()
{
    if (this->set_started(psAlertWhenSupportsNeeded)) {
        BOOST_LOG_TRIVIAL(debug) << "psAlertWhenSupportsNeeded - start";
        set_status(69, _u8L("Alert if supports needed"));

        auto issue_to_alert_message = [](SupportSpotsGenerator::SupportPointCause cause, bool critical) {
            std::string message;
            switch (cause) {
            //TRN Alert when support is needed. Describes that the model has long bridging extrusions which may print badly 
            case SupportSpotsGenerator::SupportPointCause::LongBridge: message = _u8L("Long bridging extrusions"); break;
            //TRN Alert when support is needed. Describes bridge anchors/turns in the air, which will definitely print badly
            case SupportSpotsGenerator::SupportPointCause::FloatingBridgeAnchor: message = _u8L("Floating bridge anchors"); break;
            case SupportSpotsGenerator::SupportPointCause::FloatingExtrusion:
                if (critical) {
                     //TRN Alert when support is needed. Describes that the print has large overhang area which will print badly or not print at all.
                    message = _u8L("Collapsing overhang");
                } else {
                    //TRN Alert when support is needed. Describes extrusions that are not supported enough and come out curled or loose.
                    message = _u8L("Loose extrusions");
                }
                break;
            //TRN Alert when support is needed. Describes that the print has low bed adhesion and may became loose.
            case SupportSpotsGenerator::SupportPointCause::SeparationFromBed: message = _u8L("Low bed adhesion"); break;
            //TRN Alert when support is needed. Describes that the object has part that is not connected to the bed and will not print at all without supports.
            case SupportSpotsGenerator::SupportPointCause::UnstableFloatingPart: message = _u8L("Floating object part"); break;
            //TRN Alert when support is needed. Describes that the object has thin part that may brake during printing 
            case SupportSpotsGenerator::SupportPointCause::WeakObjectPart: message = _u8L("Thin fragile part"); break;
            }

            return message;
        };

        // TRN this translation rule is used to translate lists of uknown size on single line. The first argument is element of the list,
        // the second argument may be element or rest of the list. For most languages, this does not need translation, but some use different 
        // separator than comma and some use blank space in front of the separator.
        auto single_line_list_rule = L("%1%, %2%");
        auto multiline_list_rule   = "%1%\n%2%";

        auto elements_to_translated_list = [](const std::vector<std::string> &translated_elements, std::string expansion_rule) {
            if (expansion_rule.find("%1%") == expansion_rule.npos || expansion_rule.find("%2%") == expansion_rule.npos) {
                BOOST_LOG_TRIVIAL(error) << "INCORRECT EXPANSION RULE FOR LIST TRANSLATION: " << expansion_rule
                                         << " - IT SHOULD CONTAIN %1% and %2%!";
                expansion_rule = "%1% %2%";
            }
            if (translated_elements.size() == 0) {
                return std::string{};
            }
            if (translated_elements.size() == 1) {
                return translated_elements.front();
            }

            std::string translated_list = expansion_rule;
            for (int i = 0; i < int(translated_elements.size()) - 1; ++ i) {
                auto first_elem = translated_list.find("%1%");
                assert(first_elem != translated_list.npos);
                translated_list.replace(first_elem, 3, translated_elements[i]);

                // expand the translated list by another application of the same rule
                auto second_elem = translated_list.find("%2%");
                assert(second_elem != translated_list.npos);
                if (i < int(translated_elements.size()) - 2) {
                    translated_list.replace(second_elem, 3, expansion_rule);
                } else {
                    translated_list.replace(second_elem, 3, translated_elements[i + 1]);
                }
            }

            return translated_list;
        };

        // vector of pairs of object and its issues, where each issue is a pair of type and critical flag
        std::vector<std::pair<const PrintObject *, std::vector<std::pair<SupportSpotsGenerator::SupportPointCause, bool>>>> objects_isssues;

        for (const PrintObject *object : m_objects) {
            std::unordered_set<const ModelObject *> checked_model_objects;
            if (!object->has_support() && checked_model_objects.find(object->model_object()) == checked_model_objects.end()) {
                if (object->m_shared_regions->generated_support_points.has_value()) {
                    SupportSpotsGenerator::SupportPoints  supp_points = object->m_shared_regions->generated_support_points->support_points;
                    SupportSpotsGenerator::PartialObjects partial_objects = object->m_shared_regions->generated_support_points
                                                                                ->partial_objects;
                    auto issues = SupportSpotsGenerator::gather_issues(supp_points, partial_objects);
                    if (issues.size() > 0) {
                        objects_isssues.emplace_back(object, issues);
                    }
                }
                checked_model_objects.emplace(object->model_object());
            }
        }

        bool                                                                                                  recommend_brim = false;
        std::map<std::pair<SupportSpotsGenerator::SupportPointCause, bool>, std::vector<const PrintObject *>> po_by_support_issues;
        for (const auto &obj : objects_isssues) {
            for (const auto &issue : obj.second) {
                po_by_support_issues[issue].push_back(obj.first);
                if (issue.first == SupportSpotsGenerator::SupportPointCause::SeparationFromBed && !obj.first->has_brim()) {
                    recommend_brim = true;
                }
            }
        }

        std::vector<std::pair<std::string, std::vector<std::string>>> message_elements;
        if (objects_isssues.size() > po_by_support_issues.size()) {
            // there are more objects than causes, group by issues
            for (const auto &issue : po_by_support_issues) {
                auto &pair = message_elements.emplace_back(issue_to_alert_message(issue.first.first, issue.first.second),
                                                           std::vector<std::string>{});
                for (const auto &obj : issue.second) {
                    pair.second.push_back(obj->m_model_object->name);
                }
            }
        } else {
            // more causes than objects, group by objects
            for (const auto &obj : objects_isssues) {
                auto &pair = message_elements.emplace_back(obj.first->model_object()->name,  std::vector<std::string>{});
                for (const auto &issue : obj.second) {
                    pair.second.push_back(issue_to_alert_message(issue.first, issue.second));
                }
            }
        }

        // first, gather sublements into single line list, store in first subelement
        for (auto &pair : message_elements) {
            pair.second.front() = elements_to_translated_list(pair.second, single_line_list_rule);
        }

        // then gather elements to create multiline list
        std::vector<std::string> lines = {};
        for (auto &pair : message_elements) {
            lines.push_back(""); // empty line for readability
            lines.push_back(pair.first);
            lines.push_back(pair.second.front());
        }

        lines.push_back("");
        lines.push_back(_u8L("Consider enabling supports."));
        if (recommend_brim) {
            lines.push_back(_u8L("Also consider enabling brim."));
        }

        // TRN Alert message for detected print issues. first argument is a list of detected issues.
        auto message = Slic3r::format(_u8L("Detected print stability issues:\n%1%"), elements_to_translated_list(lines, multiline_list_rule));

        if (objects_isssues.size() > 0) {
            this->active_step_add_warning(PrintStateBase::WarningLevel::NON_CRITICAL, message);
        }

        BOOST_LOG_TRIVIAL(debug) << "psAlertWhenSupportsNeeded - end";
        this->set_done(psAlertWhenSupportsNeeded);
    }
}

// Wipe tower support.
bool Print::has_wipe_tower() const
{
    return 
        ! m_config.spiral_vase.value &&
        m_config.wipe_tower.value && 
        m_config.nozzle_diameter.size() > 1;
}

const WipeTowerData& Print::wipe_tower_data(size_t extruders_cnt, double nozzle_diameter) const
{
    // If the wipe tower wasn't created yet, make sure the depth and brim_width members are set to default.
    if (! is_step_done(psWipeTower) && extruders_cnt !=0) {
        const_cast<Print*>(this)->m_wipe_tower_data.brim_width = m_config.wipe_tower_brim_width;

        // Calculating depth should take into account currently set wiping volumes.
        // For a long time, the initial preview would just use 900/width per toolchange (15mm on a 60mm wide tower)
        // and it worked well enough. Let's try to do slightly better by accounting for the purging volumes.
        std::vector<std::vector<float>> wipe_volumes = WipeTower::extract_wipe_volumes(m_config);
        std::vector<float> max_wipe_volumes;
        for (const std::vector<float>& v : wipe_volumes)
            max_wipe_volumes.emplace_back(*std::max_element(v.begin(), v.end()));
        float maximum = std::accumulate(max_wipe_volumes.begin(), max_wipe_volumes.end(), 0.f);
        maximum = maximum * extruders_cnt / max_wipe_volumes.size();

        float width = float(m_config.wipe_tower_width);
        float unscaled_brim_width = m_config.wipe_tower_brim_width.get_abs_value(nozzle_diameter);
        // use min layer height, as it's what wil disctate the wipe tower width.
        // FIXME: get layer height from layers instead of config.
        float layer_height = std::min(default_object_config().layer_height.value, get_first_layer_height());

        const_cast<Print*>(this)->m_wipe_tower_data.depth = (maximum/layer_height)/width;
        const_cast<Print*>(this)->m_wipe_tower_data.height = -1.f; // unknown yet
        const_cast<Print*>(this)->m_wipe_tower_data.brim_width = unscaled_brim_width;
    }

    return this->m_wipe_tower_data;
}

void Print::_make_wipe_tower()
{
    m_wipe_tower_data.clear();
    if (! this->has_wipe_tower())
        return;

    std::vector<std::vector<float>> wipe_volumes = WipeTower::extract_wipe_volumes(m_config);

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
            SupportLayerPtrs::const_iterator it_layer = m_objects.front()->edit_support_layers().begin();
            for (; it_layer != m_objects.front()->edit_support_layers().end() && (*it_layer)->print_z - EPSILON < wipe_tower_new_layer_print_z_first; ++ it_layer);
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

    // Set the extruder & material properties at the wipe tower object.
    for (size_t i = 0; i < m_config.nozzle_diameter.size(); ++ i)
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
                    
                    // START filament_wipe_advanced_pigment
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
                    // END filament_wipe_advanced_pigment
                    
                    // Not all of that can be used for infill purging:
                    volume_to_wipe -= (float)m_config.filament_minimal_purge_on_wipe_tower.get_at(extruder_id);

                    // try to assign some infills/objects for the wiping:
                    volume_to_wipe = layer_tools.wiping_extrusions_nonconst().mark_wiping_extrusions(*this, layer_tools, current_extruder_id, extruder_id, volume_to_wipe);

                    // add back the minimal amount toforce on the wipe tower:
                    volume_to_wipe += (float)m_config.filament_minimal_purge_on_wipe_tower.get_at(extruder_id);

                    // request a toolchange at the wipe tower with at least volume_to_wipe purging amount
                    wipe_tower.plan_toolchange((float)layer_tools.print_z, (float)layer_tools.wipe_tower_layer_height,
                                               current_extruder_id, extruder_id, volume_to_wipe);
                    current_extruder_id = extruder_id;
                }
            }
            layer_tools.wiping_extrusions_nonconst().ensure_perimeters_infills_order(*this, layer_tools);
            if (&layer_tools == &m_wipe_tower_data.tool_ordering.back() || (&layer_tools + 1)->wipe_tower_partitions == 0)
                break;
        }
    }

    // Generate the wipe tower layers.
    m_wipe_tower_data.tool_changes.reserve(m_wipe_tower_data.tool_ordering.layer_tools().size());
    wipe_tower.generate(m_wipe_tower_data.tool_changes);
    m_wipe_tower_data.depth = wipe_tower.get_depth();
    m_wipe_tower_data.z_and_depth_pairs = wipe_tower.get_z_and_depth_pairs();
    m_wipe_tower_data.brim_width = wipe_tower.get_brim_width();
    m_wipe_tower_data.height = wipe_tower.get_wipe_tower_height();

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

    m_wipe_tower_data.used_filament_until_layer = wipe_tower.get_used_filament_until_layer();
    m_wipe_tower_data.number_of_toolchanges = wipe_tower.get_number_of_toolchanges();
    m_wipe_tower_data.width = wipe_tower.width();
    m_wipe_tower_data.first_layer_height = get_first_layer_height();
    m_wipe_tower_data.cone_angle = config().wipe_tower_cone_angle;
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
    config.set_key_value("default_output_extension", new ConfigOptionString(".gcode"));

    // Handle output_filename_format. There is a hack related to binary G-codes: gcode / bgcode substitution.
    std::string output_filename_format = m_config.output_filename_format.value;
    if (m_config.binary_gcode && boost::iends_with(output_filename_format, ".gcode"))
        output_filename_format.insert(output_filename_format.end()-5, 'b');
    if (! m_config.binary_gcode && boost::iends_with(output_filename_format, ".bgcode"))
        output_filename_format.erase(output_filename_format.end()-6);

    return this->PrintBase::output_filename(output_filename_format, ".gcode", filename_base, &config);
}

const std::string PrintStatistics::FilamentUsedG     = "filament used [g]";
const std::string PrintStatistics::FilamentUsedGMask = "; filament used [g] =";

const std::string PrintStatistics::TotalFilamentUsedG          = "total filament used [g]";
const std::string PrintStatistics::TotalFilamentUsedGMask      = "; total filament used [g] =";
const std::string PrintStatistics::TotalFilamentUsedGValueMask = "; total filament used [g] = %.2lf\n";

const std::string PrintStatistics::FilamentUsedCm3     = "filament used [cm3]";
const std::string PrintStatistics::FilamentUsedCm3Mask = "; filament used [cm3] =";

const std::string PrintStatistics::FilamentUsedMm     = "filament used [mm]";
const std::string PrintStatistics::FilamentUsedMmMask = "; filament used [mm] =";

const std::string PrintStatistics::FilamentCost     = "filament cost";
const std::string PrintStatistics::FilamentCostMask = "; filament cost =";

const std::string PrintStatistics::TotalFilamentCost          = "total filament cost";
const std::string PrintStatistics::TotalFilamentCostMask      = "; total filament cost =";
const std::string PrintStatistics::TotalFilamentCostValueMask = "; total filament cost = %.2lf\n";

const std::string PrintStatistics::TotalFilamentUsedWipeTower     = "total filament used for wipe tower [g]";
const std::string PrintStatistics::TotalFilamentUsedWipeTowerValueMask = "; total filament used for wipe tower [g] = %.2lf\n";



DynamicConfig PrintStatistics::config() const
{
    DynamicConfig config;
    if (this->estimated_print_time_str.find(static_cast<uint8_t>(PrintEstimatedStatistics::ETimeMode::Normal)) !=
        this->estimated_print_time_str.end()) {
        std::string normal_print_time = short_time(
            this->estimated_print_time_str.at(static_cast<uint8_t>(PrintEstimatedStatistics::ETimeMode::Normal)));
        config.set_key_value("print_time", new ConfigOptionString(normal_print_time));
        config.set_key_value("normal_print_time", new ConfigOptionString(normal_print_time));
    } else if (this->estimated_print_time_str.find(static_cast<uint8_t>(
                   PrintEstimatedStatistics::ETimeMode::Stealth)) != this->estimated_print_time_str.end()) {
        std::string silent_print_time = short_time(
            this->estimated_print_time_str.at(static_cast<uint8_t>(PrintEstimatedStatistics::ETimeMode::Stealth)));
        config.set_key_value("print_time", new ConfigOptionString(silent_print_time));
    }
    if (this->estimated_print_time_str.find(static_cast<uint8_t>(PrintEstimatedStatistics::ETimeMode::Stealth)) !=
        this->estimated_print_time_str.end()) {
        std::string silent_print_time = short_time(
            this->estimated_print_time_str.at(static_cast<uint8_t>(PrintEstimatedStatistics::ETimeMode::Stealth)));
        config.set_key_value("silent_print_time", new ConfigOptionString(silent_print_time));
    }
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
    for (const char *key : { 
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
