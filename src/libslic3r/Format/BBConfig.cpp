#include "BBConfig.hpp"

#include "../Model.hpp"
#include "../PrintConfig.hpp"
#include "../Config.hpp"

#include "../../nlohmann/json.hpp"

#include <map>
#include <string>

#include <boost/algorithm/string/replace.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>

namespace Slic3r {
    
namespace BBConfiguration {
// BBS: add json support
#define BBL_JSON_KEY_VERSION "version"
#define BBL_JSON_KEY_IS_CUSTOM "is_custom_defined"
#define BBL_JSON_KEY_URL "url"
#define BBL_JSON_KEY_NAME "name"
#define BBL_JSON_KEY_DESCRIPTION "description"
#define BBL_JSON_KEY_FORCE_UPDATE "force_update"
#define BBL_JSON_KEY_MACHINE_MODEL_LIST "machine_model_list"
#define BBL_JSON_KEY_PROCESS_LIST "process_list"
#define BBL_JSON_KEY_SUB_PATH "sub_path"
#define BBL_JSON_KEY_FILAMENT_LIST "filament_list"
#define BBL_JSON_KEY_MACHINE_LIST "machine_list"
#define BBL_JSON_KEY_TYPE "type"
#define BBL_JSON_KEY_FROM "from"
#define BBL_JSON_KEY_SETTING_ID "setting_id"
#define BBL_JSON_KEY_BASE_ID "base_id"
#define BBL_JSON_KEY_USER_ID "user_id"
#define BBL_JSON_KEY_FILAMENT_ID "filament_id"
#define BBL_JSON_KEY_UPDATE_TIME "updated_time"
#define BBL_JSON_KEY_INHERITS "inherits"
#define BBL_JSON_KEY_INSTANTIATION "instantiation"
#define BBL_JSON_KEY_NOZZLE_DIAMETER "nozzle_diameter"
#define BBL_JSON_KEY_PRINTER_TECH "machine_tech"
#define BBL_JSON_KEY_FAMILY "family"
#define BBL_JSON_KEY_BED_MODEL "bed_model"
#define BBL_JSON_KEY_BED_TEXTURE "bed_texture"
#define BBL_JSON_KEY_HOTEND_MODEL "hotend_model"
#define BBL_JSON_KEY_DEFAULT_MATERIALS "default_materials"
#define BBL_JSON_KEY_MODEL_ID "model_id"

std::map<std::string, std::string> key_translation_map;
std::map<std::string, std::map<std::string, std::string>> value_translation_map;
std::vector<std::pair<std::string, std::string>> custom_gcode_replace; // vector<pair>, as i want to keep the ordering
enum BBSettingType: uint8_t{
    bbstFFF_PRINT = 0<<0,
    bbstFFF_FILAMENT = 0<<1,
    bbstFFF_PRINTER = 0<<2,
    bbstSLA_PRINT = 0<<3,
    bbstSLA_MATERIAL = 0<<4,
    bbstSLA_PRINTER = 0<<5,

};
std::map<std::string, BBSettingType> key_custom_settings_translation_map;
// std::map<std::string, std::function<void(std::map<std::string, std::string>&, std::map<std::string,
// std::vector<std::string>>&, const std::string&, const std::string&)>> transform_complicated; std::map<std::string,
// std::function<void(std::map<std::string, std::string>&, std::map<std::string, std::vector<std::string>>&, const
// std::string&, const std::vector<std::string>&)>> transform_vector_complicated;
bool is_init = false;

void init()
{
    key_translation_map["version"] = "";
    key_translation_map["from"] = "";
    key_translation_map["name"] = "";

    //list (partially) from https://github.com/theophile/SuperSlicer_to_Orca_scripts/blob/main/superslicer_to_orca.pl
    key_translation_map["extra_perimeters_on_overhangs"]    = "extra_perimeters_overhangs";
    key_translation_map["internal_solid_infill_pattern"]    = "solid_fill_pattern";
    key_translation_map["overhang_speed_classic"]           = "overhangs_speed";
    key_translation_map["preferred_orientation"]            = "init_z_rotate";
    key_translation_map["solid_infill_filament"]            = "solid_infill_extruder";
    key_translation_map["support_filament"]                 = "support_material_extruder";
    key_translation_map["support_interface_filament"]       = "support_material_interface_extruder";
    key_translation_map["sparse_infill_filament"]           = "infill_extruder";
    key_translation_map["wall_filament"]                    = "perimeter_extruder";
    key_translation_map["first_layer_filament"]             = "first_layer_extruder";
    key_translation_map["spiral_mode"]                      = "spiral_vase";
    
    // print
    key_translation_map["alternate_extra_wall"]             = "extra_perimeters_odd_layers";
    key_translation_map["is_infill_first"]                          = "infill_first";
    key_translation_map["enable_arc_fitting"]                       = "arc_fitting";
    key_translation_map["bottom_shell_layers"]                      = "bottom_solid_layers";
    key_translation_map["bottom_shell_thickness"]                   = "bottom_solid_min_thickness";
    key_translation_map["bottom_solid_infill_flow_ratio"]   = "first_layer_flow_ratio ";
    //key_translation_map["bridge_acceleration"]                      = "bridge_acceleration";
    //key_translation_map["bridge_angle"]                             = "bridge_angle";
    key_translation_map["bridge_density"]                           = "bridge_overlap_min";
    key_translation_map["bridge_no_support"]                        = "dont_support_bridges";
    //key_translation_map["internal_bridge_speed"]                    = "internal_bridge_speed";
    //key_translation_map["brim_ears"]                                = "brim_ears";
    //key_translation_map["brim_ears_detection_length"]               = "brim_ears_detection_length";
    //key_translation_map["brim_ears_max_angle"]                      = "brim_ears_max_angle";
    key_translation_map["brim_object_gap"]                          = "brim_separation";
    key_translation_map["brim_width"]                               = "brim_width";
    key_translation_map["skirt_speed"]                              = "brim_speed";
    //key_translation_map["compatible_printers_condition"]            = "compatible_printers_condition";
    //key_translation_map["compatible_printers"]                      = "compatible_printers";
    //key_translation_map["default_acceleration"]                     = "default_acceleration";
    key_translation_map["detect_overhang_wall"]                     = "overhangs"; // will go to handle_legacy
    key_translation_map["detect_thin_wall"]                         = "thin_walls";
    //key_translation_map["draft_shield"]                             = "draft_shield";
    key_translation_map["elefant_foot_compensation"]                = "elefant_foot_compensation";  // will go to handle_legacy
    key_translation_map["elefant_foot_compensation_layers"]         = "first_layer_size_compensation_layers";
    key_translation_map["enable_overhang_speed"]                    = "enable_dynamic_overhang_speeds"; //2.7
    //key_translation_map["extra_perimeters_on_overhangs"]            = "extra_perimeters_on_overhangs";
    key_translation_map["enable_prime_tower"]                       = "wipe_tower";
    //key_translation_map["wipe_speed"]                               = "wipe_speed";
    //key_translation_map["ensure_vertical_shell_thickness"]          = "ensure_vertical_shell_thickness";
    key_translation_map["filter_out_gap_fill"]                      = "gap_fill_min_length";
    //key_translation_map["gcode_comments"]                           = "gcode_comments";
    //key_translation_map["gcode_label_objects"]                      = "gcode_label_objects";
    //key_translation_map["infill_anchor_max"]                        = "infill_anchor_max";
    //key_translation_map["infill_anchor"]                            = "infill_anchor";
    key_translation_map["infill_direction"]                         = "fill_angle";
    key_translation_map["infill_wall_overlap"]                      = "infill_overlap";
    //key_translation_map["inherits"]                                 = "inherits";
    key_translation_map["line_width"]                               = "extrusion_width";
    key_translation_map["print_flow_ratio"]                         = "extrusion_multiplier";
    key_translation_map["initial_layer_acceleration"]               = "first_layer_acceleration";
    key_translation_map["initial_layer_line_width"]                 = "first_layer_extrusion_width";
    key_translation_map["initial_layer_print_height"]               = "first_layer_height";
    //key_translation_map["interface_shells"]                         = "interface_shells";
    key_translation_map["inner_wall_line_width"]                    = "perimeter_extrusion_width";
    //key_translation_map["seam_gap"]                                 = "seam_gap";
    key_translation_map["internal_solid_infill_acceleration"]       = "solid_infill_acceleration";
    key_translation_map["internal_solid_infill_line_width"]         = "solid_infill_extrusion_width";
    key_translation_map["ironing_flow"]                             = "ironing_flowrate";
    key_translation_map["ironing_spacing"]                          = "ironing_spacing";
    key_translation_map["ironing_speed"]                            = "ironing_speed";
    //key_translation_map["layer_height"]                             = "layer_height";
    key_translation_map["max_travel_detour_distance"]               = "avoid_crossing_perimeters_max_detour";
    //key_translation_map["min_bead_width"]                           = "min_bead_width";
    //key_translation_map["min_feature_size"]                         = "min_feature_size";
    key_translation_map["minimum_sparse_infill_area"]               = "solid_infill_below_area";
    key_translation_map["only_one_wall_first_layer"]                = "only_one_perimeter_first_layer";
    key_translation_map["only_one_wall_top"]                        = "only_one_perimeter_top";
    //key_translation_map["ooze_prevention"]                          = "ooze_prevention";
    key_translation_map["overhang_reverse"]                         = "overhangs_reverse";
    key_translation_map["overhang_reverse_threshold"]               = "overhangs_reverse_threshold";
    key_translation_map["inner_wall_acceleration"]                  = "perimeter_acceleration";
    key_translation_map["outer_wall_acceleration"]                  = "external_perimeter_acceleration";
    key_translation_map["outer_wall_line_width"]                    = "external_perimeter_extrusion_width";
    //key_translation_map["post_process"]                             = "post_process";
    key_translation_map["prime_tower_brim_width"]                   = "wipe_tower_brim_width";
    key_translation_map["prime_tower_width"]                        = "wipe_tower_width";
    //key_translation_map["raft_contact_distance"]                    = "raft_contact_distance";
    //key_translation_map["raft_expansion"]                           = "raft_expansion";
    //key_translation_map["raft_first_layer_density"]                 = "raft_first_layer_density";
    //key_translation_map["raft_first_layer_expansion"]               = "raft_first_layer_expansion";
    //key_translation_map["raft_layers"]                              = "raft_layers";
    key_translation_map["reduce_crossing_wall"]                     = "avoid_crossing_perimeters";
    key_translation_map["reduce_infill_retraction"]                 = "only_retract_when_crossing_perimeters";
    //key_translation_map["resolution"]                               = "resolution";
    //key_translation_map["seam_position"]                            = "seam_position";
    //key_translation_map["skirt_distance"]                           = "skirt_distance";
    //key_translation_map["skirt_height"]                             = "skirt_height";
    key_translation_map["skirt_loops"]                              = "skirts";
    //key_translation_map["slice_closing_radius"]                     = "slice_closing_radius";
    //key_translation_map["slicing_mode"]                             = "slicing_mode";
    key_translation_map["small_perimeter_threshold"]                = "small_perimeter_min_length";
    key_translation_map["sparse_infill_acceleration"]               = "infill_acceleration";
    key_translation_map["sparse_infill_density"]                    = "fill_density";
    key_translation_map["sparse_infill_line_width"]                 = "infill_extrusion_width";
    //key_translation_map["staggered_inner_seams"]                    = "staggered_inner_seams";
    //key_translation_map["standby_temperature_delta"]                = "standby_temperature_delta";
    //key_translation_map["hole_to_polyhole"]                         = "hole_to_polyhole";
    //key_translation_map["hole_to_polyhole_threshold"]               = "hole_to_polyhole_threshold";
    //key_translation_map["hole_to_polyhole_twisted"]                 = "hole_to_polyhole_twisted";
    key_translation_map["enable_support"]                           = "support_material";
    key_translation_map["support_angle"]                            = "support_material_angle";
    key_translation_map["enforce_support_layers"]                   = "support_material_enforce_layers";
    key_translation_map["support_base_pattern_spacing"]             = "support_material_spacing";
    key_translation_map["support_top_z_distance"]                   = "support_material_contact_distance";
    key_translation_map["support_bottom_z_distance"]                = "support_material_bottom_contact_distance";
    key_translation_map["support_interface_bottom_layers"]          = "support_material_bottom_interface_layers";
    key_translation_map["support_interface_loop_pattern"]           = "support_material_interface_contact_loops";
    key_translation_map["support_interface_spacing"]                = "support_material_interface_spacing";
    key_translation_map["support_interface_top_layers"]             = "support_material_interface_layers";
    key_translation_map["support_line_width"]                       = "support_material_extrusion_width";
    key_translation_map["support_on_build_plate_only"]              = "support_material_buildplate_only";
    key_translation_map["support_threshold_angle"]                  = "support_material_threshold";
    key_translation_map["thick_bridges"]                            = "thick_bridges"; //handled by from_prusa
    key_translation_map["top_shell_layers"]                         = "top_solid_layers";
    key_translation_map["top_shell_thickness"]                      = "top_solid_min_thickness";
    key_translation_map["top_surface_acceleration"]                 = "top_solid_infill_acceleration";
    key_translation_map["top_surface_line_width"]                   = "top_infill_extrusion_width";
    //key_translation_map["min_width_top_surface"]                    = "min_width_top_surface";
    //key_translation_map["travel_acceleration"]                      = "travel_acceleration";
    //key_translation_map["travel_speed_z"]                           = "travel_speed_z";
    //key_translation_map["travel_speed"]                             = "travel_speed";
    key_translation_map["tree_support_branch_angle"]                = "support_tree_angle"; //2.7
    key_translation_map["tree_support_angle_slow"]                  = "support_tree_angle_slow";
    key_translation_map["tree_support_branch_diameter"]             = "support_tree_branch_diameter";
    key_translation_map["tree_support_branch_diameter_angle"]       = "support_tree_branch_diameter_angle";
    key_translation_map["tree_support_branch_diameter_double_wall"] = "support_tree_branch_diameter_double_wall";
    key_translation_map["tree_support_tip_diameter"]                = "support_tree_tip_diameter";
    key_translation_map["tree_support_top_rate"]                    = "support_tree_top_rate";
    //key_translation_map["wall_distribution_count"]                  = "wall_distribution_count";
    key_translation_map["wall_generator"]                           = "perimeter_generator";
    key_translation_map["wall_loops"]                               = "perimeters";
    //key_translation_map["wall_transition_angle"]                    = "wall_transition_angle";
    //key_translation_map["wall_transition_filter_deviation"]         = "wall_transition_filter_deviation";
    //key_translation_map["wall_transition_length"]                   = "wall_transition_length";
    //key_translation_map["wipe_tower_no_sparse_layers"]              = "wipe_tower_no_sparse_layers";
    key_translation_map["xy_contour_compensation"]                  = "xy_size_compensation";
    //key_translation_map["z_offset"]                                 = "z_offset";
    key_translation_map["xy_hole_compensation"]                     = "xy_inner_size_compensation";
    key_translation_map["independent_support_layer_height"]         = "support_material_layer_height";
    key_translation_map["sparse_infill_pattern"]                    = "fill_pattern";
    key_translation_map["filename_format"]                          = "output_filename_format";
    key_translation_map["support_base_pattern"]                     = "support_material_pattern";
    key_translation_map["support_interface_pattern"]                = "support_material_top_interface_pattern";
    key_translation_map["top_surface_pattern"]                      = "top_fill_pattern";
    key_translation_map["support_object_xy_distance"]               = "support_material_xy_spacing";
    key_translation_map["fuzzy_skin_point_distance"]                = "fuzzy_skin_point_dist";
    //key_translation_map["fuzzy_skin_thickness"]                     = "fuzzy_skin_thickness";
    //key_translation_map["fuzzy_skin"]                               = "fuzzy_skin";
    key_translation_map["bottom_surface_pattern"]                   = "bottom_fill_pattern";
    key_translation_map["bridge_flow"]                              = "bridge_flow_ratio";
    key_translation_map["top_solid_infill_flow_ratio"]              = "fill_top_flow_ratio";
    key_translation_map["bottom_solid_infill_flow_ratio"]           = "initial_layer_flow_ratio";
    key_translation_map["infill_combination"]                       = "infill_every_layers";
    key_translation_map["print_sequence"]                           = "complete_objects";
    key_translation_map["brim_type"]                                = "brim_type"; //handled by from_prusa
    //key_translation_map["notes"]                                    = "notes";
    key_translation_map["support_style"]                   = "support_material_style";
    //key_translation_map["ironing"]                                  = "ironing";
    //key_translation_map["ironing_type"]                             = "ironing_type";
    //key_translation_map["ironing_angle"]                            = "ironing_angle";
    //key_translation_map["external_perimeters_first"]                = "external_perimeters_first";
    //key_translation_map["infill_first"]                             = "infill_first";

    //speeds
    key_translation_map["inner_wall_speed"]            = "perimeter_speed";
    key_translation_map["outer_wall_speed"]            = "external_perimeter_speed";
    //key_translation_map["small_perimeter_speed"]       = "small_perimeter_speed";
    key_translation_map["internal_solid_infill_speed"] = "solid_infill_speed";
    key_translation_map["sparse_infill_speed"]         = "infill_speed";
    key_translation_map["top_surface_speed"]           = "top_solid_infill_speed";
    key_translation_map["gap_infill_speed"]            = "gap_fill_speed";
    key_translation_map["support_speed"]               = "support_material_speed";
    key_translation_map["support_interface_speed"]     = "support_material_interface_speed";
    //key_translation_map["bridge_speed"]                = "bridge_speed";
    key_translation_map["initial_layer_speed"]         = "first_layer_speed";
    key_translation_map["initial_layer_infill_speed"]  = "first_layer_infill_speed";

//    'filament' = >
    key_translation_map["hot_plate_temp"]                   = "bed_temperature";
    key_translation_map["cool_plate_temp"]                   = "bed_temperature";
    key_translation_map["eng_plate_temp"]                   = "bed_temperature";
    key_translation_map["textured_plate_temp"]                   = "bed_temperature";
    key_translation_map["overhang_fan_speed"]                   = "bridge_fan_speed";
    //key_translation_map["chamber_temperature"]                  = "chamber_temperature";
    key_translation_map["close_fan_the_first_x_layers"]         = "disable_fan_first_layers";
    key_translation_map["filament_end_gcode"]                   = "end_filament_gcode";
    key_translation_map["overhang_fan_threshold"]               = "external_perimeter_fan_speed";
    key_translation_map["filament_flow_ratio"]                  = "extrusion_multiplier";
    key_translation_map["reduce_fan_stop_start_freq"]           = "fan_always_on";
    key_translation_map["fan_cooling_layer_time"]               = "fan_below_layer_time";
    //key_translation_map["fan_speedup_time"]                     = "fan_speedup_time";
    //key_translation_map["fan_speedup_overhangs"]                = "fan_speedup_overhangs";
    //key_translation_map["fan_kickstart"]                        = "fan_kickstart";
    key_translation_map["default_filament_colour"]              = "filament_colour";
    //key_translation_map["filament_cost"]                        = "filament_cost";
    //key_translation_map["filament_density"]                     = "filament_density";
    key_translation_map["filament_deretraction_speed"]          = "filament_deretract_speed";
    //key_translation_map["filament_diameter"]                    = "filament_diameter";
    //key_translation_map["filament_max_volumetric_speed"]        = "filament_max_volumetric_speed";
    //key_translation_map["filament_notes"]                       = "filament_notes";
    key_translation_map["filament_retraction_minimum_travel"]   = "filament_retract_before_travel";
    //key_translation_map["filament_retract_before_wipe"]         = "filament_retract_before_wipe";
    key_translation_map["filament_retract_when_changing_layer"] = "filament_retract_layer_change";
    key_translation_map["filament_retraction_length"]           = "filament_retract_length";
    key_translation_map["filament_z_hop"]                       = "filament_retract_lift";
    //key_translation_map["filament_retract_lift_above"]          = "filament_retract_lift_above";
    //key_translation_map["filament_retract_lift_below"]          = "filament_retract_lift_below";
    //key_translation_map["filament_retract_restart_extra"]       = "filament_retract_restart_extra";
    key_translation_map["filament_retraction_speed"]            = "filament_retract_speed";
    //key_translation_map["filament_shrink"]                      = "filament_shrink";
    //key_translation_map["filament_soluble"]                     = "filament_soluble";
    //key_translation_map["filament_type"]                        = "filament_type";
    //key_translation_map["filament_wipe"]                        = "filament_wipe";
    key_translation_map["hot_plate_temp_initial_layer"]         = "first_layer_bed_temperature";
    key_translation_map["cool_plate_temp_initial_layer"]        = "first_layer_bed_temperature";
    key_translation_map["eng_plate_temp_initial_layer"]         = "first_layer_bed_temperature";
    key_translation_map["textured_plate_temp_initial_layer"]    = "first_layer_bed_temperature";
    key_translation_map["nozzle_temperature_initial_layer"]     = "first_layer_temperature";
    //key_translation_map["full_fan_speed_layer"]                 = "full_fan_speed_layer";
    //key_translation_map["inherits"]                             = "inherits";
    key_translation_map["fan_max_speed"]                        = "max_fan_speed";
    key_translation_map["fan_min_speed"]                        = "default_fan_speed";
    key_translation_map["slow_down_min_speed"]                  = "min_print_speed";
    key_translation_map["slow_down_layer_time"]                 = "slowdown_below_layer_time";
    key_translation_map["filament_start_gcode"]                 = "start_filament_gcode";
    //key_translation_map["support_material_interface_fan_speed"] = "support_material_interface_fan_speed";
    key_translation_map["nozzle_temperature"]                   = "temperature";
    //key_translation_map["compatible_printers_condition"]        = "compatible_printers_condition";
    //key_translation_map["compatible_printers"]                  = "compatible_printers";
    //key_translation_map["compatible_prints_condition"]          = "compatible_prints_condition";
    //key_translation_map["compatible_prints"]                    = "compatible_prints";
    //key_translation_map["filament_vendor"]                      = "filament_vendor";
    //key_translation_map["filament_minimal_purge_on_wipe_tower"] = "filament_minimal_purge_on_wipe_tower";

//    'printer' = >
    //key_translation_map["bed_custom_model"]                    = "bed_custom_model";
    //key_translation_map["bed_custom_texture"]                  = "bed_custom_texture";
    key_translation_map["before_layer_change_gcode"]           = "before_layer_gcode";
    key_translation_map["change_filament_gcode"]               = "toolchange_gcode";
    //key_translation_map["default_filament_profile"]            = "default_filament_profile";
    //key_translation_map["default_print_profile"]               = "default_print_profile";
    key_translation_map["deretraction_speed"]                  = "deretract_speed";
    key_translation_map["emit_machine_limits_to_gcode"]       = "machine_limits_usage";
    //key_translation_map["gcode_flavor"]                        = "gcode_flavor";
    //key_translation_map["inherits"]                            = "inherits";
    key_translation_map["layer_change_gcode"]                  = "layer_gcode";
    key_translation_map["change_extrusion_role_gcode"]         = "feature_gcode";
    key_translation_map["machine_end_gcode"]                   = "end_gcode";
    key_translation_map["machine_max_acceleration_e"]          = "machine_max_acceleration_e";
    key_translation_map["machine_max_acceleration_extruding"]  = "machine_max_acceleration_extruding";
    key_translation_map["machine_max_acceleration_retracting"] = "machine_max_acceleration_retracting";
    key_translation_map["machine_max_acceleration_travel"]     = "machine_max_acceleration_travel";
    key_translation_map["machine_max_acceleration_x"]          = "machine_max_acceleration_x";
    key_translation_map["machine_max_acceleration_y"]          = "machine_max_acceleration_y";
    key_translation_map["machine_max_acceleration_z"]          = "machine_max_acceleration_z";
    key_translation_map["machine_max_speed_e"]                 = "machine_max_feedrate_e";
    key_translation_map["machine_max_speed_x"]                 = "machine_max_feedrate_x";
    key_translation_map["machine_max_speed_y"]                 = "machine_max_feedrate_y";
    key_translation_map["machine_max_speed_z"]                 = "machine_max_feedrate_z";
    key_translation_map["machine_max_jerk_e"]                  = "machine_max_jerk_e";
    key_translation_map["machine_max_jerk_x"]                  = "machine_max_jerk_x";
    key_translation_map["machine_max_jerk_y"]                  = "machine_max_jerk_y";
    key_translation_map["machine_max_jerk_z"]                  = "machine_max_jerk_z";
    key_translation_map["machine_min_extruding_rate"]          = "machine_min_extruding_rate";
    key_translation_map["machine_min_travel_rate"]             = "machine_min_travel_rate";
    key_translation_map["machine_pause_gcode"]                 = "pause_print_gcode";
    key_translation_map["machine_start_gcode"]                 = "start_gcode";
    key_translation_map["max_layer_height"]                    = "max_layer_height";
    key_translation_map["min_layer_height"]                    = "min_layer_height";
    key_translation_map["nozzle_diameter"]                     = "nozzle_diameter";
    key_translation_map["print_host"]                          = "print_host";
    key_translation_map["printer_notes"]                       = "printer_notes";
    key_translation_map["printable_area"]                      = "bed_shape";
    key_translation_map["printable_height"]                    = "max_print_height";
    key_translation_map["printer_technology"]                  = "printer_technology";
    key_translation_map["printer_variant"]                     = "printer_variant";
    key_translation_map["retract_before_wipe"]                 = "retract_before_wipe";
    key_translation_map["retract_length_toolchange"]           = "retract_length_toolchange";
    key_translation_map["retract_restart_extra_toolchange"]    = "retract_restart_extra_toolchange";
    key_translation_map["retract_restart_extra"]               = "retract_restart_extra";
    key_translation_map["retract_when_changing_layer"]         = "retract_layer_change";
    key_translation_map["retraction_length"]                   = "retract_length";
    key_translation_map["z_hop"]                               = "retract_lift";
    key_translation_map["retract_lift_enforce"]                = "retract_lift_top";
    key_translation_map["retraction_minimum_travel"]           = "retract_before_travel";
    key_translation_map["retraction_speed"]                    = "retract_speed";
    key_translation_map["silent_mode"]                         = "silent_mode";
    key_translation_map["single_extruder_multi_material"]      = "single_extruder_multi_material";
    key_translation_map["thumbnails"]                          = "thumbnails"; //TOCHECK
    key_translation_map["thumbnails_format"]                   = "thumbnails_format";
    key_translation_map["template_custom_gcode"]               = "template_custom_gcode";
    key_translation_map["use_firmware_retraction"]             = "use_firmware_retraction";
    key_translation_map["use_relative_e_distances"]            = "use_relative_e_distances";
    key_translation_map["wipe"]                                = "wipe";

//    'physical_printer' => {
    //    host_type                    => 1,
    //    print_host                   => 1,
    //    printer_technology           => 1,
    //    printhost_apikey             => 1,
    //    printhost_authorization_type => 1,
    //    printhost_cafile             => 1,
    //    printhost_password           => 1,
    //    printhost_port               => 1,
    //    printhost_ssl_ignore_revoke  => 1,
    //    printhost_user               => 1,
    //}

//#Printer parameters that may be comma - separated lists
//my %multivalue_params = (
//key_translation_map["single"]="max_layer_height";
//key_translation_map["single"]="min_layer_height";
//key_translation_map["single"]="deretract_speed";
//key_translation_map["single"]="default_filament_profile";
//key_translation_map["array"]="machine_max_acceleration_e";
//key_translation_map["array"]="machine_max_acceleration_extruding";
//key_translation_map["array"]="machine_max_acceleration_extruding";
//key_translation_map["array"]="machine_max_acceleration_retracting";
//key_translation_map["array"]="machine_max_acceleration_travel";
//key_translation_map["array"]="machine_max_acceleration_x";
//key_translation_map["array"]="machine_max_acceleration_y";
//key_translation_map["array"]="machine_max_acceleration_z";
//key_translation_map["array"]="machine_max_feedrate_e";
//key_translation_map["array"]="machine_max_feedrate_x";
//key_translation_map["array"]="machine_max_feedrate_y";
//key_translation_map["array"]="machine_max_feedrate_z";
//key_translation_map["array"]="machine_max_jerk_e";
//key_translation_map["array"]="machine_max_jerk_x";
//key_translation_map["array"]="machine_max_jerk_y";
//key_translation_map["array"]="machine_max_jerk_z";
//key_translation_map["array"]="machine_min_extruding_rate";
//key_translation_map["array"]="machine_min_travel_rate";
//key_translation_map["single"]="nozzle_diameter";
//key_translation_map["array"]="bed_shape";
//key_translation_map["single"]="retract_before_wipe";
//key_translation_map["single"]="retract_length_toolchange";
//key_translation_map["single"]="retract_restart_extra_toolchange";
//key_translation_map["single"]="retract_restart_extra";
//key_translation_map["single"]="retract_layer_change";
//key_translation_map["single"]="retract_length";
//key_translation_map["single"]="retract_lift";
//key_translation_map["single"]="retract_before_travel";
//key_translation_map["single"]="retract_speed";
//key_translation_map["array"]="thumbnails";
//key_translation_map["single"]="extruder_offset";
//key_translation_map["single"]="retract_lift_above";
//key_translation_map["single"]="retract_lift_below";
//key_translation_map["single"]="wipe";
    //pattern
    value_translation_map["fill_pattern"]["monotonicline"] = "monotoniclines"; //2.7
    value_translation_map["fill_pattern"]["zig-zag"] = "rectilinear";
    value_translation_map["fill_pattern"]["tri-hexagon"] = "stars";
    //value_translation_map["fill_pattern"]["rectilinear-grid"] = "???"; //can't convert let the config_substitutions emit the warning
    value_translation_map["top_fill_pattern"] = value_translation_map["fill_pattern"];
    value_translation_map["bottom_fill_pattern"] = value_translation_map["fill_pattern"];
    value_translation_map["solid_fill_pattern"] = value_translation_map["fill_pattern"];
    value_translation_map["brim_ears_pattern"] = value_translation_map["fill_pattern"];
    value_translation_map["bridge_fill_pattern"] = value_translation_map["fill_pattern"];
    value_translation_map["support_material_top_interface_pattern"] = value_translation_map["fill_pattern"];
    value_translation_map["support_material_bottom_interface_pattern"] = value_translation_map["fill_pattern"];
    //specific
    value_translation_map["fill_pattern"]["default"] = "gyroid";
    value_translation_map["top_fill_pattern"]["default"] = "monotonic";
    value_translation_map["bottom_fill_pattern"]["default"] = "monotonic";
    value_translation_map["solid_fill_pattern"]["default"] = "rectilinear";
    value_translation_map["brim_ears_pattern"]["default"] = "concentric";
    value_translation_map["bridge_fill_pattern"]["default"] = "rectilinear";
    value_translation_map["support_material_top_interface_pattern"]["default"] = "auto";
    value_translation_map["support_material_bottom_interface_pattern"]["default"] = "auto";
    //value_translation_map["support_material_interface_pattern"]["rectilinear_interlaced"] = "???"; //can't convert let the config_substitutions emit the warning
 
    //others
    value_translation_map["support_material_pattern"]["default"] = "rectilinear";
    //value_translation_map["support_material_pattern"]["lightning"] = ""; //can't convert, let the config_substitutions emit the warning
    //value_translation_map["support_material_pattern"]["hollow"] = ""; //can't convert, let the config_substitutions emit the warning
    value_translation_map["seam_position"]["back"] = "rear";
    value_translation_map["filament_type"]["TPU"] = "FLEX";
    value_translation_map["support_material_style"]["normal"] = "grid";
    value_translation_map["support_material_style"]["default"] = "grid";
    value_translation_map["support_material_style"]["tree"] = "snug"; // organic in 2.7
    value_translation_map["support_material_style"]["tree_slim"] = "snug"; // organic in 2.7
    value_translation_map["support_material_style"]["tree_strong"] = "snug"; // organic in 2.7
    value_translation_map["support_material_style"]["tree_hybrid"] = "snug"; // organic in 2.7
    value_translation_map["support_material_style"]["organic"] = "snug"; // organic in 2.7
    value_translation_map["retract_lift_top"]["Bottom Only"] = "Not on top";
    value_translation_map["retract_lift_top"]["Top Only"] = "Only on top";
    value_translation_map["thumbnails_format"]["BTT_TFT"] = "BIQU";
    value_translation_map["complete_objects"]["by layer"] = "0";
    value_translation_map["complete_objects"]["by object"] = "1";
    value_translation_map["machine_limits_usage"]["0"] = "time_estimate_only";
    value_translation_map["machine_limits_usage"]["1"] = "emit_to_gcode";



/// GCODE
    custom_gcode_replace.emplace_back("[bed_temperature_initial_layer_single]", "{first_layer_bed_temperature[initial_extruder]}");
    custom_gcode_replace.emplace_back("bed_temperature_initial_layer_single", "first_layer_bed_temperature[initial_extruder]");
    custom_gcode_replace.emplace_back("initial_extruder_id", "initial_extruder");
    custom_gcode_replace.emplace_back("bbl_bed_temperature_gcode", "false");
    custom_gcode_replace.emplace_back("bed_temperature_initial_layer", "first_layer_bed_temperature");
    custom_gcode_replace.emplace_back("bed_temperature_initial_layer_vector", "\"\"");
    custom_gcode_replace.emplace_back("[temperature_initial_layer]", "{first_layer_temperature[initial_extruder]}");
    custom_gcode_replace.emplace_back("temperature_initial_layer", "first_layer_temperature[initial_extruder]");
    //custom_gcode_replace.emplace_back("overall_chamber_temperature", "chamber_temperature"); //fixme: it's a max.

    //if plate_name, then add plate_name as custom setting
    key_custom_settings_translation_map["print_custom_variables"] = BBSettingType(bbstFFF_PRINT | bbstSLA_PRINT);
    key_custom_settings_translation_map["filament_custom_variables"] = BBSettingType(bbstFFF_FILAMENT | bbstSLA_MATERIAL);
    key_custom_settings_translation_map["printer_custom_variables"] = BBSettingType(bbstFFF_PRINTER | bbstSLA_PRINTER);
    key_custom_settings_translation_map["plate_name"] = BBSettingType(bbstFFF_PRINT | bbstSLA_PRINT);
}

void complicated_convert(t_config_option_key &opt_key, std::string &value, const std::map<std::string, std::string> &input, std::map<std::string, std::string> &output)
{
    
    if ("ironing_type" == opt_key && "no ironing" == value) {
        value = "top";
        output["ironing"] = "0";
        assert(input.find("ironing") == input.end() || input.find("ironing")->second == "0");
    }
    if ("brim_type" == opt_key && "brim_ears" == value) {
        opt_key = "brim_ears";
        value = "1";
    }
    if ("disable_m73" == opt_key) {
        output["remaining_times_type"] = "m73";
        opt_key = "remaining_times";
        if ("1" == value) {
            value = "0";
        } else {
            value = "1";
        }
    }
    //if ("enable_overhang_speed") {
    //
    //}
    if ("support_material_top_interface_pattern" == opt_key || "support_interface_pattern" == opt_key) {
        output["support_material_top_interface_pattern"] = value;
        output["support_material_bottom_interface_pattern"] = value;
    }
}

//settings from orca that I can't convert
// ironing_pattern

bool push_into_custom_variable(DynamicPrintConfig &print_config,
                               const std::string & opt_key,
                               const std::string & opt_value)
{
    if (auto it = key_custom_settings_translation_map.find(opt_key); it != key_custom_settings_translation_map.end()) {
        if ((it->second & bbstFFF_PRINT) != 0 || (it->second & bbstSLA_PRINT) != 0) {
            if (print_config.opt<ConfigOptionStrings>("print_custom_variables") == nullptr)
                print_config.set_deserialize("print_custom_variables", "");
            std::string &value = print_config.opt<ConfigOptionString>("print_custom_variables")->value;
            if (value.find(opt_key) == std::string::npos)
                value += opt_key + std::string("=") + opt_value + std::string("\n");
        }
        if ((it->second & bbstFFF_FILAMENT) != 0 || (it->second & bbstSLA_MATERIAL) != 0) {
            if (print_config.opt<ConfigOptionStrings>("filament_custom_variables") == nullptr)
                print_config.set_deserialize("filament_custom_variables", "");
            const std::string &val = print_config.opt<ConfigOptionStrings>("filament_custom_variables")->get_at(0);
            if (val.find(opt_key) == std::string::npos)
                print_config.opt<ConfigOptionStrings>("filament_custom_variables")
                    ->set_at(val + opt_key + std::string("=") + opt_value + std::string("\n"), 0);
        }
        if ((it->second & bbstFFF_PRINTER) != 0 || (it->second & bbstSLA_PRINTER) != 0) {
            if (print_config.opt<ConfigOptionStrings>("printer_custom_variables") == nullptr)
                print_config.set_deserialize("printer_custom_variables", "");
            std::string &value = print_config.opt<ConfigOptionString>("printer_custom_variables")->value;
            if (value.find(opt_key) == std::string::npos)
                value += opt_key + std::string("=") + opt_value + std::string("\n");
        }
        return true;
    }
    return false;
}

bool push_into_custom_variables(DynamicPrintConfig &            print_config,
                                const std::string &             opt_key,
                                const std::vector<std::string> &opt_value)
{
    if (auto it = key_custom_settings_translation_map.find(opt_key); it != key_custom_settings_translation_map.end()) {
        if ((it->second & bbstFFF_FILAMENT) != 0 || (it->second & bbstSLA_MATERIAL) != 0) {
            for (int i = 0; i < opt_value.size(); ++i) {
                if (print_config.opt<ConfigOptionStrings>("filament_custom_variables") == nullptr)
                    print_config.set_deserialize("filament_custom_variables", "");
                const std::string &val = print_config.opt<ConfigOptionStrings>("filament_custom_variables")->get_at(i);
                if (val.find(opt_key) == std::string::npos)
                    print_config.opt<ConfigOptionStrings>("filament_custom_variables")
                        ->set_at(val + opt_key + std::string("=") + opt_value[i] + std::string("\n"), i);
            }
            return true;
        }
    }
    return false;
}

//TODO: ensure it's inside '{' '[' script section, reliably
void custom_gcode_transform(DynamicPrintConfig &print_config)
{
    for (std::string opt_key : {"template_custom_gcode", "toolchange_gcode", "before_layer_gcode",
                                "between_objects_gcode", "end_gcode", "layer_gcode", "feature_gcode", "start_gcode",
                                "color_change_gcode", "pause_print_gcode", "toolchange_gcode"}) {
        auto opt = print_config.opt<ConfigOptionString>(opt_key);
        if (opt != nullptr) {
            std::string &custom_gcode = opt->value;
            // check & replace setting name
            for (auto &entry : key_translation_map) { boost::replace_all(custom_gcode, entry.first, entry.second); }
            // check & replace special things
            for (auto &entry : custom_gcode_replace) { boost::replace_all(custom_gcode, entry.first, entry.second); }
        }
    }
    for (std::string opt_key : {"end_filament_gcode", "start_filament_gcode"}) {
        auto opt = print_config.opt<ConfigOptionStrings>(opt_key);
        if (opt != nullptr) {
            std::vector<std::string> custom_gcode_values = opt->get_values();
            for (std::string &custom_gcode : custom_gcode_values) {
                // check & replace setting name
                for (auto &entry : key_translation_map) {
                    boost::replace_all(custom_gcode, entry.first, entry.second);
                }
                // check & replace special things
                for (auto &entry : custom_gcode_replace) {
                    boost::replace_all(custom_gcode, entry.first, entry.second);
                }
            }
            opt->set(custom_gcode_values);
        }
    }
}

} // BBconfiguration

bool read_json_file_bambu(const std_path &temp_file,
                             DynamicPrintConfig &         config,
                             ConfigSubstitutionContext &  config_substitutions,
                             bool                         with_phony)
{
    using namespace BBConfiguration;
    if (!is_init) // not thread-safe
        init();

    std::map<std::string, std::string>              key_values;
    std::map<std::string, std::vector<std::string>> key_vector_values;
    // read entries into map

    nlohmann::json         j;
    std::list<std::string> different_settings_append;
    std::string            new_support_style;
    std::string            is_infill_first;
    std::string            get_wall_sequence;
    bool                   is_project_settings = false;

    CNumericLocalesSetter locales_setter;

    try {
        std_ifstream ifs(GET_STD_PATH_FOR_IFSTREAM(temp_file));
        ifs >> j;
        ifs.close();

        const ConfigDef *config_def = config.def();
        if (config_def == nullptr) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": no config defs!";
            return false;
        }
        // parse the json elements
        for (auto it = j.begin(); it != j.end(); it++) {
            if (boost::iequals(it.key(), BBL_JSON_KEY_VERSION)) {
                key_values.emplace(BBL_JSON_KEY_VERSION, it.value());
            } else if (boost::iequals(it.key(), BBL_JSON_KEY_IS_CUSTOM)) {
                key_values.emplace(BBL_JSON_KEY_IS_CUSTOM, it.value());
            } else if (boost::iequals(it.key(), BBL_JSON_KEY_NAME)) {
                key_values.emplace(BBL_JSON_KEY_NAME, it.value());
                if (it.value() == "project_settings")
                    is_project_settings = true;
            } else if (boost::iequals(it.key(), BBL_JSON_KEY_URL)) {
                key_values.emplace(BBL_JSON_KEY_URL, it.value());
            } else if (boost::iequals(it.key(), BBL_JSON_KEY_TYPE)) {
                key_values.emplace(BBL_JSON_KEY_TYPE, it.value());
            } else if (boost::iequals(it.key(), BBL_JSON_KEY_SETTING_ID)) {
                key_values.emplace(BBL_JSON_KEY_SETTING_ID, it.value());
            } else if (boost::iequals(it.key(), BBL_JSON_KEY_FILAMENT_ID)) {
                key_values.emplace(BBL_JSON_KEY_FILAMENT_ID, it.value());
            } else if (boost::iequals(it.key(), BBL_JSON_KEY_FROM)) {
                key_values.emplace(BBL_JSON_KEY_FROM, it.value());
            } else if (boost::iequals(it.key(), BBL_JSON_KEY_INSTANTIATION)) {
                key_values.emplace(BBL_JSON_KEY_INSTANTIATION, it.value());
            } else if (/*!load_inherits_to_config &&*/ boost::iequals(it.key(), BBL_JSON_KEY_INHERITS)) {
                key_values.emplace(BBL_JSON_KEY_INHERITS, it.value());
            } else if (it.value().is_string()) {
                key_values.emplace(it.key(), it.value());
            } else if (it.value().is_array()) {
                // std::string value_str;
                // bool first = true, use_comma = true;
                for (auto iter = it.value().begin(); iter != it.value().end(); iter++) {
                    if (iter.value().is_string()) {
                        key_vector_values[it.key()].push_back(iter.value());
                    } else {
                        // should not happen
                        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": parse " << temp_file
                                                 << " error, invalid json array for " << it.key();
                        break;
                    }
                }
            } else {
                // should not happen
                BOOST_LOG_TRIVIAL(error)
                    << __FUNCTION__ << ": parse " << temp_file << " error, invalid json type for " << it.key();
            }
        }
    } catch (const std_ifstream::failure &err) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": parse " << temp_file
                                 << " got a ifstream error, reason = " << err.what();
    } catch (nlohmann::detail::parse_error &err) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": parse " << temp_file
                                 << " got a nlohmann::detail::parse_error, reason = " << err.what();
    } catch (std::exception &err) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": parse " << temp_file
                                 << " got a generic exception, reason = " << err.what();
    }

    // transform entries into susi config
    std::map<std::string, std::string>              good_key_values;
    std::map<std::string, std::vector<std::string>> good_key_vector_values;
    for (auto &entry : key_values) {
        t_config_option_key opt_key = entry.first;
        std::string         value   = entry.second;

        if (push_into_custom_variable(config, opt_key, value))
            continue;

        if (auto it = key_translation_map.find(opt_key); it != key_translation_map.end())
            opt_key = it->second;
        PrintConfigDef::handle_legacy(opt_key, value, false);

        complicated_convert(opt_key, value, key_values, good_key_values);

        if (auto it_key = value_translation_map.find(opt_key); it_key != value_translation_map.end())
            if (auto it_val = it_key->second.find(value); it_val != it_key->second.end())
                value = it_val->second;

        if (!opt_key.empty())
            good_key_values[opt_key] = value;
        //else
        //    config_substitutions.substitutions.push_back(ConfigSubstitution{ nullptr, entry.first+std::string(" : ")+value, nullptr});
    }

    for (auto &entry : key_vector_values) {
        t_config_option_key      key    = entry.first;
        std::vector<std::string> values = entry.second;
        if (push_into_custom_variables(config, key, values))
            continue;
        if (auto it = key_translation_map.find(key); it != key_translation_map.end())
            key = it->second;
        std::string check_val = values[0];
        PrintConfigDef::handle_legacy(key, values[0], false);
        assert(check_val == values[0]); // can't change a vec value, sadly.
        if (!key.empty())
            good_key_vector_values[key] = values;
        //else
        //    config_substitutions.substitutions.push_back(ConfigSubstitution{ nullptr, entry.first+std::string(" : ")+(values.empty()?"":values.front()), nullptr});
    }

    // check how to serialize the array (string use ';', others ',')
    const ConfigDef *config_def = config.def();
    for (auto &entry : good_key_vector_values) {
        assert(!entry.first.empty());
        t_config_option_key opt_key = entry.first;
        std::string         value_str;
        bool                valid = true, first = true, use_comma = true;
        // bool test2 = (it.key() == std::string("filament_end_gcode"));
        const ConfigOptionDef *optdef = config_def->get(entry.first);
        if (optdef == nullptr) {
            // If we didn't find an option, look for any other option having this as an alias.
            for (const auto &opt : config_def->options) {
                for (const t_config_option_key &opt_key2 : opt.second.aliases) {
                    if (opt_key2 == opt_key) {
                        opt_key = opt.first;
                        optdef  = &opt.second;
                        break;
                    }
                }
                if (optdef != nullptr)
                    break;
            }
        }

        if (optdef && optdef->type == coStrings) {
            use_comma = false;
        }
        for (const std::string &val : entry.second) {
            if (!first) {
                if (use_comma)
                    value_str += ",";
                else
                    value_str += ";";
            } else
                first = false;

            if (use_comma)
                value_str += val;
            else {
                value_str += "\"";
                value_str += escape_string_cstyle(val);
                value_str += "\"";
            }
        }
        if (valid)
            good_key_values[opt_key] = value_str;
        else if(optdef)
            config_substitutions.add(ConfigSubstitution( optdef, value_str, ConfigOptionUniquePtr(optdef->default_value->clone()) ));
        else
            config_substitutions.add(ConfigSubstitution(entry.first, value_str));

    }

    // push these into config

    for (auto &entry : good_key_values) {
        if(config_def->has(entry.first))
            config.set_deserialize(entry.first, entry.second, config_substitutions);
        else
            config_substitutions.add(ConfigSubstitution(entry.first, entry.second));
    }

    // final transform
    config.convert_from_prusa(with_phony);

    custom_gcode_transform(config);

    return true;
}


std::map<std::string, std::string> read_ini_file_bambu(const std_path &temp_file)
{
    boost::property_tree::ptree tree;
    std_ifstream ifs(GET_STD_PATH_FOR_IFSTREAM(temp_file));
    boost::property_tree::read_ini(ifs, tree);
    //return this->load(tree, compatibility_rule);
        
        std::map<std::string, std::string> key_values;
    for (const boost::property_tree::ptree::value_type &v : tree) {
        key_values[v.first] = v.second.get_value<std::string>();
    }
    return key_values;
}

bool convert_settings_from_bambu(std::map<std::string, std::string> bambu_settings_serialized,
                                 DynamicPrintConfig &               print_config,
                                 ConfigSubstitutionContext &        config_substitutions,
                                 bool                               with_phony)
{
    using namespace BBConfiguration;
    if (!is_init) // not thread-safe
        init();
    
    // transform entries into susi config
    std::map<std::string, std::string>              good_key_values;
    for (auto &entry : bambu_settings_serialized) {
        t_config_option_key opt_key = entry.first;
        std::string         value   = entry.second;

        if (push_into_custom_variable(print_config, opt_key, value))
            continue;

        if (auto it = key_translation_map.find(opt_key); it != key_translation_map.end())
            opt_key = it->second;
        PrintConfigDef::handle_legacy(opt_key, value, false);
        
        complicated_convert(opt_key, value, bambu_settings_serialized, good_key_values);

        if (auto it_key = value_translation_map.find(opt_key); it_key != value_translation_map.end())
            if (auto it_val = it_key->second.find(value); it_val != it_key->second.end())
                value = it_val->second;

        if (!opt_key.empty())
            good_key_values[opt_key] = value;
    }
    

    // push these into config
    const ConfigDef *config_def = print_config.def();
    for (auto &entry : good_key_values) {
        if(config_def->has(entry.first))
            print_config.set_deserialize(entry.first, entry.second, config_substitutions);
        else
            config_substitutions.add(ConfigSubstitution(entry.first, entry.second));
    }
    
    // final transform
    print_config.convert_from_prusa(with_phony);
    custom_gcode_transform(print_config);

    return true;
}

bool convert_settings_from_bambu(std::map<std::string, std::string> bambu_settings_serialized,
                                 const DynamicPrintConfig &         print_config,
                                 ModelConfigObject &                object_config,
                                 ConfigSubstitutionContext &        config_substitutions,
                                 bool                               with_phony)
{
    using namespace BBConfiguration;
    if (!is_init) // not thread-safe
        init();
    
    // transform entries into susi config
    std::map<std::string, std::string>              good_key_values;
    for (auto &entry : bambu_settings_serialized) {
        t_config_option_key opt_key = entry.first;
        std::string         value   = entry.second;
        if (auto it = key_translation_map.find(opt_key); it != key_translation_map.end())
            opt_key = it->second;
        PrintConfigDef::handle_legacy(opt_key, value, false);
        
        complicated_convert(opt_key, value, bambu_settings_serialized, good_key_values);

        if (auto it_key = value_translation_map.find(opt_key); it_key != value_translation_map.end())
            if (auto it_val = it_key->second.find(value); it_val != it_key->second.end())
                value = it_val->second;

        if (!opt_key.empty())
            good_key_values[opt_key] = value;
    }
    

    // push these into config
    const ConfigDef *config_def = object_config.get().def();
    for (auto &entry : good_key_values) {
        if(config_def->has(entry.first))
            object_config.set_deserialize(entry.first, entry.second, config_substitutions);
        else
            config_substitutions.add(ConfigSubstitution(entry.first, entry.second));
    }
    
    // final transform
    object_config.convert_from_prusa(print_config, with_phony);
    return true;
}

std_path get_temp_file(Model &model)
{
#ifdef __APPLE__
    std_path temp_path  = boost::filesystem::temp_directory_path();
#else
    std_path temp_path  = std::filesystem::temp_directory_path();
#endif
    std::string           model_name = "";
    for (const ModelObject *model_object : model.objects) {
        model_name = model_object->get_export_filename();
        if (!model_name.empty()) {
            break;
        }
    }
    return temp_path / (model_name + std::string("_temp_conf.config"));
}

std_path extract_file(Model &model, mz_zip_archive &archive, const mz_zip_archive_file_stat &stat)
{
    std_path temp_path  = get_temp_file(model);
    mz_bool res = mz_zip_reader_extract_to_file(&archive, stat.m_file_index, temp_path.string().c_str(), 0);
    if (res == 0) {
        //add_error("Error while extract project config file to temp file");
        return "";
    }
    return temp_path;
}

} // namespace Slic3r

