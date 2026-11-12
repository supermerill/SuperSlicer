#include "BBConfig.hpp"

#include "../Model.hpp"
#include "../PrintConfig.hpp"
#include "../Config.hpp"

#include "../../nlohmann/json.hpp"

#include <array>
#include <map>
#include <regex>
#include <string>
#include <string_view>

#include <boost/algorithm/string.hpp>
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
    bbstFFF_PRINT = 1 << 0,
    bbstFFF_FILAMENT = 1 << 1,
    bbstFFF_PRINTER = 1 << 2,
    bbstSLA_PRINT = 1 << 3,
    bbstSLA_MATERIAL = 1 << 4,
    bbstSLA_PRINTER = 1 << 5,

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
    key_translation_map["bottom_shell_layers"]                      = "bottom_solid_layers";
    key_translation_map["bottom_shell_thickness"]                   = "bottom_solid_min_thickness";
    key_translation_map["bottom_solid_infill_flow_ratio"]   = "first_layer_flow_ratio";
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
    //key_translation_map["print_flow_ratio"]                         = "print_extrusion_multiplier"; // need to *100
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
    //key_translation_map["make_overhang_printable"]                  = "";
    //key_translation_map["make_overhang_printable_angle"]            = "";
    //key_translation_map["make_overhang_printable_hole_size"]        = "";
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
    //key_translation_map["internal_bridge_flow"]                   = "bridge_flow_ratio"; //TO ADD
    key_translation_map["bridge_flow"]                              = "bridge_flow_ratio";
    key_translation_map["top_solid_infill_flow_ratio"]              = "fill_top_flow_ratio";
    //key_translation_map["infill_combination"]                       = "infill_every_layers"; // bool -> int
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

    // these bed temp : get the max ? or the only not-default ?
    key_translation_map["hot_plate_temp"]                   = "bed_temperature"; //default to 45
    //key_translation_map["cool_plate_temp"]                   = "bed_temperature"; //default to 35
    //key_translation_map["eng_plate_temp"]                   = "bed_temperature"; //default to 45
    //key_translation_map["textured_plate_temp"]                   = "bed_temperature"; //default to 45

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
    
    // these bed temp : get the max ? or the only not-default ?
    key_translation_map["hot_plate_temp_initial_layer"]         = "first_layer_bed_temperature"; //default to 45
    //key_translation_map["cool_plate_temp_initial_layer"]        = "first_layer_bed_temperature"; //default to 35
    //key_translation_map["eng_plate_temp_initial_layer"]         = "first_layer_bed_temperature"; //default to 45
    //key_translation_map["textured_plate_temp_initial_layer"]    = "first_layer_bed_temperature"; //default to 45

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
    custom_gcode_replace.emplace_back("bed_temperature_initial_layer_single", "first_layer_bed_temperature");
    custom_gcode_replace.emplace_back("initial_extruder_id", "initial_extruder");
    custom_gcode_replace.emplace_back("bbl_bed_temperature_gcode", "false");
    custom_gcode_replace.emplace_back("bed_temperature_initial_layer", "first_layer_bed_temperature");
    custom_gcode_replace.emplace_back("bed_temperature_initial_layer_vector", "\"\"");
    custom_gcode_replace.emplace_back("[temperature_initial_layer]", "{first_layer_temperature[initial_extruder]}");
    custom_gcode_replace.emplace_back("temperature_initial_layer", "first_layer_temperature");
    //custom_gcode_replace.emplace_back("overall_chamber_temperature", "chamber_temperature"); //fixme: it's a max.

    //if plate_name, then add plate_name as custom setting
    key_custom_settings_translation_map["print_custom_variables"] = BBSettingType(bbstFFF_PRINT | bbstSLA_PRINT);
    key_custom_settings_translation_map["filament_custom_variables"] = BBSettingType(bbstFFF_FILAMENT | bbstSLA_MATERIAL);
    key_custom_settings_translation_map["printer_custom_variables"] = BBSettingType(bbstFFF_PRINTER | bbstSLA_PRINTER);
    key_custom_settings_translation_map["plate_name"] = BBSettingType(bbstFFF_PRINT | bbstSLA_PRINT);
}

void complicated_convert(t_config_option_key &opt_key,
                         std::string &value,
                         const std::map<std::string, std::string> &input,
                         std::map<std::string, std::string> &output) {
    // start PHONY
    // no spacing from orca/bamabu, only width
    if (opt_key.find("extrusion_width") != std::string::npos) {
        t_config_option_key spacing_opt_key = std::regex_replace(opt_key, std::regex("extrusion_width"), "extrusion_spacing");
        output[spacing_opt_key] = "";
    }
    // end PHONY

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
    if ("enable_arc_fitting" == opt_key) {
        opt_key = "arc_fitting";
        if ("1" == value) {
            value = "bambu";
        } else {
            assert(value == "0");
            value = "disabled";
        }
    }
    if ("support_material_top_interface_pattern" == opt_key || "support_interface_pattern" == opt_key) {
        output["support_material_top_interface_pattern"] = value;
        output["support_material_bottom_interface_pattern"] = value;
    }
    if ("initial_layer_line_width" == opt_key || "first_layer_extrusion_width" == opt_key) {
        output["skirt_extrusion_width"] = value;
    }
    if ("small_perimeter_threshold" == opt_key || "small_perimeter_min_length" == opt_key) {
        output["small_perimeter_max_length"] = value;
    }
    if ("reduce_crossing_wall" == opt_key || "avoid_crossing_perimeters" == opt_key) {
        output["avoid_crossing_not_first_layer"] = "0";
    }

    if ("resolution" == opt_key) {
        if (std::stof(value.c_str()) < 0.01f)
            value = "0.0";
        output["resolution_internal"] = "0.0125";
    }
    if ("bridge_density" == opt_key || "bridge_overlap_min" == opt_key) {
        output["bridge_overlap"] = std::to_string(int(std::stof(value.c_str()) * 1.3));
    }
    //check that the value is good.
    if ("infill_combination" == opt_key) {
        if ("0" == value) {
            value = "1";
        } else {
            assert("1" == value);
            value = "10";
        }
    }
    if ("print_flow_ratio" == opt_key) {
        opt_key = "print_extrusion_multiplier";
        value = std::to_string(int(std::stof(value.c_str()) * 100));
    }
    // since Mar 10, 2024 ( Optimize ensure vertical feature's UX #4402 )
    if ("ensure_vertical_shell_thickness" == opt_key && value != "0" && value != "1") {
        if ("none" == value) {
            value = "disabled";
        } else if("ensure_critical_only" == value) {
            value = "enabled_old";
            output["solid_over_perimeters"] = "2";
        } else if("ensure_moderate" == value) {
            value = "enabled_old";
            output["solid_over_perimeters"] = "4";
        } else if("ensure_all" == value) {
            value = "enabled_old";
        }
    }
}

void very_complicated_convert(ConfigSubstitutionContext &unconverted_config, ConfigBase &conf_to_read_and_update) {
    std::optional<ConfigSubstitution> substitution = unconverted_config.find("make_overhang_printable");
    if (substitution) {
        if (substitution->old_value == "1") {
            double make_overhang_printable_angle = 0;
            std::optional<ConfigSubstitution> angle_subst = unconverted_config.find("make_overhang_printable_angle");
            if (!angle_subst) {
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__
                                         << ": make_overhang_printable setting, but no "
                                            "make_overhang_printable_angle in the orca config!";
            }
            make_overhang_printable_angle = std::stod(angle_subst->old_value.c_str());
            assert(make_overhang_printable_angle <= 90 && make_overhang_printable_angle >= 0);
            make_overhang_printable_angle = std::min(make_overhang_printable_angle, 90.);
            make_overhang_printable_angle = std::max(make_overhang_printable_angle, 0.);

            // now we have to compute a % from the angle: 1lh is 45°, 0% is 0°, 9999 for 90°
            // get layer height
            double layer_height = conf_to_read_and_update.get_computed_value("layer_height");

            double overhangs_max_slope = std::tan(make_overhang_printable_angle) * layer_height;
            conf_to_read_and_update.set_deserialize("overhangs_max_slope",
                                                    Slic3r::to_string_nozero(overhangs_max_slope, 3));
            conf_to_read_and_update.set_deserialize("overhangs_bridge_threshold", "!0");
            conf_to_read_and_update.set_deserialize("overhangs_bridge_upper_layers", "!0");
            unconverted_config.erase("make_overhang_printable");
            unconverted_config.erase("make_overhang_printable_angle");
        } else {
            unconverted_config.erase("make_overhang_printable");
            unconverted_config.erase("make_overhang_printable_angle");
            conf_to_read_and_update.set_deserialize("overhangs_max_slope", "0");
            conf_to_read_and_update.set_deserialize("overhangs_bridge_upper_layers", "0");
        }
    }
}

//settings from orca that I can't convert
// ironing_pattern

// Custom variables are line based. Match the complete name before '=' so that
// importing "foo" does not accidentally collide with an existing "foo_extra".
bool has_custom_variable(const std::string &variables, const std::string &name)
{
    std::vector<std::string> lines;
    boost::split(lines, variables, boost::is_any_of("\n"));
    for (const std::string &line : lines) {
        const size_t equal_pos = line.find('=');
        if (equal_pos == std::string::npos)
            continue;
        std::string existing_name = line.substr(0, equal_pos);
        boost::trim(existing_name);
        if (existing_name == name)
            return true;
    }
    return false;
}

void append_custom_variable(std::string &variables, const std::string &name, std::string value)
{
    if (has_custom_variable(variables, name))
        return;

    // A custom variable occupies one physical line. PlaceholderParser restores
    // escaped newlines after it has identified the name/value pair.
    boost::replace_all(value, "\r", "\\r");
    boost::replace_all(value, "\n", "\\n");
    if (!variables.empty() && variables.back() != '\n')
        variables.push_back('\n');
    variables += name + "=" + value + "\n";
}

void merge_custom_variable_block(std::string &destination, const std::string &source)
{
    std::vector<std::string> lines;
    boost::split(lines, source, boost::is_any_of("\n"));
    for (const std::string &line : lines) {
        const size_t equal_pos = line.find('=');
        if (equal_pos == std::string::npos)
            continue;
        std::string name = line.substr(0, equal_pos);
        std::string value = line.substr(equal_pos + 1);
        boost::trim(name);
        if (!name.empty())
            append_custom_variable(destination, name, value);
    }
}

ConfigOptionString *ensure_scalar_custom_variables(DynamicPrintConfig &config, const char *option_key)
{
    ConfigOptionString *variables = config.opt<ConfigOptionString>(option_key);
    if (variables == nullptr) {
        config.set_key_value(option_key, new ConfigOptionString());
        variables = config.opt<ConfigOptionString>(option_key);
    }
    assert(variables != nullptr);
    return variables;
}

ConfigOptionStrings *ensure_filament_custom_variables(DynamicPrintConfig &config, size_t extruder_count)
{
    ConfigOptionStrings *variables = config.opt<ConfigOptionStrings>("filament_custom_variables");
    if (variables == nullptr) {
        config.set_key_value("filament_custom_variables",
                             new ConfigOptionStrings(std::max<size_t>(extruder_count, 1), std::string()));
        variables = config.opt<ConfigOptionStrings>("filament_custom_variables");
    } else if (variables->size() < extruder_count) {
        variables->resize(extruder_count);
    }
    assert(variables != nullptr);
    return variables;
}

bool is_bambu_metadata_key(const std::string &key)
{
    static constexpr std::array<std::string_view, 20> metadata_keys = {
        BBL_JSON_KEY_VERSION, BBL_JSON_KEY_IS_CUSTOM, BBL_JSON_KEY_URL, BBL_JSON_KEY_NAME,
        BBL_JSON_KEY_DESCRIPTION, BBL_JSON_KEY_FORCE_UPDATE, BBL_JSON_KEY_MACHINE_MODEL_LIST,
        BBL_JSON_KEY_PROCESS_LIST, BBL_JSON_KEY_SUB_PATH, BBL_JSON_KEY_FILAMENT_LIST,
        BBL_JSON_KEY_MACHINE_LIST, BBL_JSON_KEY_TYPE, BBL_JSON_KEY_FROM, BBL_JSON_KEY_SETTING_ID,
        BBL_JSON_KEY_BASE_ID, BBL_JSON_KEY_USER_ID, BBL_JSON_KEY_FILAMENT_ID,
        BBL_JSON_KEY_UPDATE_TIME, BBL_JSON_KEY_INHERITS, BBL_JSON_KEY_INSTANTIATION
    };
    const std::string lower_key = boost::algorithm::to_lower_copy(key);
    return std::any_of(metadata_keys.begin(), metadata_keys.end(), [&lower_key](std::string_view metadata_key) {
        return lower_key == metadata_key;
    });
}

bool is_explicit_custom_variable_option(const std::string &key)
{
    return key == "print_custom_variables" || key == "filament_custom_variables" ||
           key == "printer_custom_variables" || key == "plate_name";
}

bool belongs_to_print_custom_variables(const std::string &key)
{
    static constexpr std::array<std::string_view, 16> print_keywords = {
        "print", "filament", "wall", "perimeter", "infill", "support", "layer", "bridge", "brim",
        "skirt", "raft", "seam", "surface", "shell", "ironing", "overhang"
    };
    const std::string lower_key = boost::algorithm::to_lower_copy(key);
    return std::any_of(print_keywords.begin(), print_keywords.end(), [&lower_key](std::string_view keyword) {
        return lower_key.find(keyword) != std::string::npos;
    });
}

void store_unconverted_scalar(DynamicPrintConfig &config, const std::string &key, const std::string &value)
{
    const char *destination = belongs_to_print_custom_variables(key) ? "print_custom_variables" :
                                                                        "printer_custom_variables";
    append_custom_variable(ensure_scalar_custom_variables(config, destination)->value, key, value);
}

void add_bambu_generated_custom_variables(DynamicPrintConfig &config)
{
    // Snapmaker Orca computes this variable immediately before expanding the
    // machine start G-code. SuperSlicer cannot reproduce that calculation yet,
    // so default to strong cooling to keep imported start G-code executable:
    // 0 = heat preservation (KeepWarm), 1 = weak cooling, 2 = strong cooling.
    append_custom_variable(ensure_scalar_custom_variables(config, "printer_custom_variables")->value,
                           "chamber_cooling_mode", "2");
}

void store_unconverted_vector(DynamicPrintConfig &config,
                              const std::string &key,
                              const std::vector<std::string> &values,
                              size_t extruder_count)
{
    if (!values.empty() && values.size() == extruder_count) {
        ConfigOptionStrings *variables = ensure_filament_custom_variables(config, extruder_count);
        for (size_t idx = 0; idx < values.size(); ++idx) {
            std::string current = variables->get_at(idx);
            append_custom_variable(current, key, values[idx]);
            variables->set_at(std::move(current), idx);
        }
        return;
    }

    // Non-extruder arrays have no native custom-variable vector representation.
    // Keep their JSON form as a scalar string so no source data is discarded.
    store_unconverted_scalar(config, key, nlohmann::json(values).dump());
}

size_t bambu_extruder_count(const std::map<std::string, std::vector<std::string>> &vector_values,
                            const DynamicPrintConfig &config)
{
    if (auto nozzle_it = vector_values.find(BBL_JSON_KEY_NOZZLE_DIAMETER);
        nozzle_it != vector_values.end() && !nozzle_it->second.empty())
        return nozzle_it->second.size();
    if (const ConfigOptionFloats *nozzles = config.opt<ConfigOptionFloats>("nozzle_diameter");
        nozzles != nullptr && !nozzles->empty())
        return nozzles->size();
    return 1;
}

bool option_accepts_serialized_value(const ConfigDef &config_def,
                                     const std::string &key,
                                     const std::string &value)
{
    const ConfigOptionDef *option_def = config_def.get(key);
    if (option_def == nullptr || option_def->default_value.get() == nullptr)
        return false;
    if (option_def->can_phony && value.empty())
        return true;
    try {
        ConfigOptionUniquePtr candidate(option_def->default_value->clone());
        return candidate->deserialize(value);
    } catch (const std::exception &) {
        return false;
    }
}

bool deserialize_converted_value(DynamicPrintConfig &config,
                                 const std::string &key,
                                 const std::string &value,
                                 ConfigSubstitutionContext &substitutions)
{
    const ConfigDef *config_def = config.def();
    const ConfigOptionDef *option_def = config_def == nullptr ? nullptr : config_def->get(key);
    if (option_def == nullptr)
        return false;

    const bool value_is_valid = option_accepts_serialized_value(*config_def, key, value);
    const size_t substitution_count = substitutions.get().size();
    const bool deserialized = config.set_deserialize_nothrow(key, value, substitutions);
    if (value_is_valid && deserialized)
        return true;

    // With substitution disabled, set_deserialize_nothrow leaves the invalid
    // value rejected. Keep the normal usable default and still report what was
    // lost, because the original value is also retained as a custom variable.
    ConfigOption *destination = config.option(key, true);
    if (destination != nullptr && option_def->default_value.get() != nullptr)
        destination->set(*option_def->default_value);
    if (substitutions.get().size() == substitution_count)
        substitutions.emplace(option_def, std::string(value),
                              ConfigOptionUniquePtr(option_def->default_value->clone()));
    return false;
}

bool converted_entries_are_valid(const std::map<std::string, std::string> &entries,
                                 const DynamicPrintConfig &config)
{
    const ConfigDef *config_def = config.def();
    if (config_def == nullptr || entries.empty())
        return false;
    return std::all_of(entries.begin(), entries.end(), [config_def](const auto &entry) {
        return option_accepts_serialized_value(*config_def, entry.first, entry.second);
    });
}

bool bambu_scalar_is_convertible(const std::string &source_key,
                                 const std::string &source_value,
                                 const std::map<std::string, std::string> &all_source_values,
                                 const DynamicPrintConfig &config)
{
    if (is_bambu_metadata_key(source_key) || is_explicit_custom_variable_option(source_key))
        return true;

    std::string key = source_key;
    std::string value = source_value;
    if (auto translation = key_translation_map.find(key); translation != key_translation_map.end())
        key = translation->second;
    if (key.empty())
        return true;

    try {
        std::map<std::string, std::string> additional_values;
        complicated_convert(key, value, all_source_values, additional_values);
        if (auto value_map = value_translation_map.find(key); value_map != value_translation_map.end())
            if (auto translated_value = value_map->second.find(value); translated_value != value_map->second.end())
                value = translated_value->second;
        PrintConfigDef::handle_legacy_pair(key, value, false);
        if (key.empty())
            return true;

        const ConfigDef *config_def = config.def();
        if (config_def != nullptr && option_accepts_serialized_value(*config_def, key, value))
            return true;

        std::map<std::string, std::string> converted = PrintConfigDef::from_prusa(key, value, config);
        if (!key.empty())
            converted.emplace(key, value);
        return converted_entries_are_valid(converted, config);
    } catch (const std::exception &) {
        return false;
    }
}

std::string serialize_bambu_vector(const std::string &key,
                                   const std::vector<std::string> &source_values,
                                   const ConfigDef &config_def)
{
    const ConfigOptionDef *option_def = config_def.get(key);
    const bool strings = option_def != nullptr && option_def->type == coStrings;
    std::string serialized;
    for (const std::string &value : source_values) {
        if (!serialized.empty())
            serialized += strings ? ";" : ",";
        if (strings)
            serialized += "\"" + escape_string_cstyle(value) + "\"";
        else
            serialized += value;
    }
    return serialized;
}

bool bambu_vector_is_convertible(const std::string &source_key,
                                 const std::vector<std::string> &source_values,
                                 const DynamicPrintConfig &config)
{
    if (is_bambu_metadata_key(source_key) || is_explicit_custom_variable_option(source_key))
        return true;

    std::string key = source_key;
    if (auto translation = key_translation_map.find(key); translation != key_translation_map.end())
        key = translation->second;
    if (key.empty())
        return true;

    std::vector<std::string> values = source_values;
    for (std::string &value : values) {
        PrintConfigDef::handle_legacy_pair(key, value, false);
        if (key.empty())
            return true;
    }

    const ConfigDef *config_def = config.def();
    if (config_def == nullptr || !config_def->has(key))
        return false;
    return option_accepts_serialized_value(*config_def, key,
                                           serialize_bambu_vector(key, values, *config_def));
}

bool push_into_custom_variable(DynamicPrintConfig &print_config,
                               const std::string & opt_key,
                               const std::string & opt_value)
{
    if (auto it = key_custom_settings_translation_map.find(opt_key); it != key_custom_settings_translation_map.end()) {
        if ((it->second & bbstFFF_PRINT) != 0 || (it->second & bbstSLA_PRINT) != 0) {
            std::string &value = ensure_scalar_custom_variables(print_config, "print_custom_variables")->value;
            if (opt_key == "print_custom_variables")
                merge_custom_variable_block(value, opt_value);
            else
                append_custom_variable(value, opt_key, opt_value);
        }
        if ((it->second & bbstFFF_FILAMENT) != 0 || (it->second & bbstSLA_MATERIAL) != 0) {
            ConfigOptionStrings *variables = ensure_filament_custom_variables(print_config, 1);
            std::string value = variables->get_at(0);
            if (opt_key == "filament_custom_variables")
                merge_custom_variable_block(value, opt_value);
            else
                append_custom_variable(value, opt_key, opt_value);
            variables->set_at(std::move(value), 0);
        }
        if ((it->second & bbstFFF_PRINTER) != 0 || (it->second & bbstSLA_PRINTER) != 0) {
            std::string &value = ensure_scalar_custom_variables(print_config, "printer_custom_variables")->value;
            if (opt_key == "printer_custom_variables")
                merge_custom_variable_block(value, opt_value);
            else
                append_custom_variable(value, opt_key, opt_value);
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
            ConfigOptionStrings *variables = ensure_filament_custom_variables(print_config, opt_value.size());
            for (size_t i = 0; i < opt_value.size(); ++i) {
                std::string value = variables->get_at(i);
                if (opt_key == "filament_custom_variables")
                    merge_custom_variable_block(value, opt_value[i]);
                else
                    append_custom_variable(value, opt_key, opt_value[i]);
                variables->set_at(std::move(value), i);
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
    std::unordered_map<t_config_option_key, std::pair<t_config_option_key, std::string>> dict_opt;

    // key_translation_map
    for (const auto& [bb_key, value] : key_values) {
        if (push_into_custom_variable(config, bb_key, value))
            continue;

        std::string my_key = bb_key;
        if (auto it = key_translation_map.find(bb_key); it != key_translation_map.end())
            my_key = it->second;

        dict_opt[my_key] = {my_key, value};
    }

    // complicated_convert & value_translation_map
    for (auto &[saved_key, key_val] : dict_opt) {
        auto &[opt_key, value] = key_val;

        complicated_convert(opt_key, value, key_values, good_key_values);

        if (auto it_key = value_translation_map.find(opt_key); it_key != value_translation_map.end())
            if (auto it_val = it_key->second.find(value); it_val != it_key->second.end())
                value = it_val->second;

        if (!opt_key.empty())
            good_key_values[opt_key] = value;
        //else
        //    config_substitutions.substitutions.push_back(ConfigSubstitution{ nullptr, entry.first+std::string(" : ")+value, nullptr});
    }

    // handle_legacy_map
    dict_opt.clear();
    for (const auto &[my_key, value] : good_key_values) {
        dict_opt[my_key] = {my_key, value};
    }
    PrintConfigDef::handle_legacy_map(dict_opt, false);
    good_key_values.clear();
    for (auto &[saved_key, key_val] : dict_opt) {
        auto &[opt_key, value] = key_val;
        if (!opt_key.empty())
            good_key_values[opt_key] = value;
        //else
        //    config_substitutions.substitutions.push_back(ConfigSubstitution{ nullptr, entry.first+std::string(" : ")+value, nullptr});
    }

    //now same for vectors (but no complicated_convert)
    // key_translation_map, handle_legacy_pair
    std::unordered_map<t_config_option_key, std::pair<t_config_option_key, std::string>> dict_opt_vector;
    std::unordered_map<t_config_option_key, std::string> dict_old_value;
    for (auto &entry : key_vector_values) {
        t_config_option_key key = entry.first;
        std::vector<std::string> values = entry.second;
        if (push_into_custom_variables(config, key, values))
            continue;
        if (auto it = key_translation_map.find(key); it != key_translation_map.end())
            key = it->second;
        
        if (!key.empty()) {
            for (size_t idx = 0; idx < values.size(); ++idx) {
                PrintConfigDef::handle_legacy_pair(key, values[idx], false);
                if (key.empty()) {
                    break;
                }
            }
            if (!key.empty()) {
                good_key_vector_values[key] = values;
            }
        }
        //else
        //    config_substitutions.substitutions.push_back(ConfigSubstitution{ nullptr, entry.first+std::string(" : ")+value, nullptr});
    }
    // there will be a final handle_legacy_pair after concat anyway

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
    dict_opt.clear();
    for (const auto &[my_key, value] : good_key_values) {
        dict_opt[my_key] = {my_key, value};
    }
    PrintConfigDef::handle_legacy_map(dict_opt, false);
    for (auto &entry : dict_opt) {
        if (!entry.second.first.empty() && config_def->has(entry.second.first))
            deserialize_converted_value(config, entry.second.first, entry.second.second, config_substitutions);
    }

    // final transform
    //config.convert_from_prusa(with_phony);
    std::map<t_config_option_key, std::string> settings_to_change;
    std::unordered_map<t_config_option_key, std::pair<t_config_option_key, std::string>> dict_opt_final;
    for (auto &entry : good_key_values) {
        if (!config_def->has(entry.first)) {
            std::string opt_key = entry.first;
            std::string value = entry.second;
            std::map<std::string, std::string> result = PrintConfigDef::from_prusa(opt_key, value, config);
            settings_to_change.insert(result.begin(), result.end());
            if (!opt_key.empty()) {
                // check if good this time
                // PrintConfigDef::handle_legacy(opt_key, value, false);
                dict_opt_final[opt_key] = {opt_key, value};
            }
        }
    }
    PrintConfigDef::handle_legacy_map(dict_opt_final, false);
    for (const auto &[saved_key, saved_value] : good_key_values) {
        if (auto it = dict_opt_final.find(saved_key); it != dict_opt_final.end()) {
            auto &[opt_key, value] = it->second;
            if (!opt_key.empty()) {
                if (!config_def->has(opt_key)) {
                    if (config_substitutions.rule != ForwardCompatibilitySubstitutionRule::Disable) {
                        config_substitutions.add(ConfigSubstitution(saved_key, value));
                    }
                } else
                    deserialize_converted_value(config, opt_key, value, config_substitutions);
            }
        }
    }
    for (const auto &entry : settings_to_change)
        if (config_def->has(entry.first))
            deserialize_converted_value(config, entry.first, entry.second, config_substitutions);

    custom_gcode_transform(config);

    
    
    //apply very_complicated_convert
    very_complicated_convert(config_substitutions, config);

    // Preserve only source settings for which the complete conversion pipeline
    // cannot produce a valid SuperSlicer option. The original Bambu name is
    // intentional: phase 1 keeps existing custom G-code references usable.
    const size_t extruder_count = bambu_extruder_count(key_vector_values, config);
    for (const auto &[key, value] : key_values)
        if (!bambu_scalar_is_convertible(key, value, key_values, config))
            store_unconverted_scalar(config, key, value);
    for (const auto &[key, values] : key_vector_values)
        if (!bambu_vector_is_convertible(key, values, config))
            store_unconverted_vector(config, key, values, extruder_count);
    add_bambu_generated_custom_variables(config);

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
    std::unordered_map<t_config_option_key, std::pair<t_config_option_key, std::string>> dict_opt;
    std::map<std::string, std::string>              good_key_values;
    for (auto &entry : bambu_settings_serialized) {
        t_config_option_key opt_key = entry.first;
        std::string value = entry.second;

        if (push_into_custom_variable(print_config, opt_key, value))
            continue;

        if (auto it = key_translation_map.find(opt_key); it != key_translation_map.end())
            opt_key = it->second;

        dict_opt[opt_key] = {opt_key, value};
    }
    PrintConfigDef::handle_legacy_map(dict_opt, false);
    for (auto &[saved_key, key_val] : dict_opt) {
        auto &[opt_key, value] = key_val;
        
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
            deserialize_converted_value(print_config, entry.first, entry.second, config_substitutions);
        else
            config_substitutions.add(ConfigSubstitution(entry.first, entry.second));
    }
    

    // final transforms

    //apply very_complicated_convert
    very_complicated_convert(config_substitutions, print_config);

    // The INI-style overload has already lost vector type information, so an
    // unresolved serialized value is retained as a scalar custom variable.
    for (const auto &[key, value] : bambu_settings_serialized)
        if (!bambu_scalar_is_convertible(key, value, bambu_settings_serialized, print_config))
            store_unconverted_scalar(print_config, key, value);
    
    //print_config.convert_from_prusa(with_phony);
    custom_gcode_transform(print_config);
    add_bambu_generated_custom_variables(print_config);

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
    std::unordered_map<t_config_option_key, std::pair<t_config_option_key, std::string>> dict_opt;
    std::map<std::string, std::string>              good_key_values;
    for (auto &entry : bambu_settings_serialized) {
        t_config_option_key opt_key = entry.first;
        std::string         value   = entry.second;
        if (auto it = key_translation_map.find(opt_key); it != key_translation_map.end())
            opt_key = it->second;
        dict_opt[opt_key] = {opt_key, value};
    }
    PrintConfigDef::handle_legacy_map(dict_opt, false);
    for (auto &[saved_key, key_val] : dict_opt) {
        auto &[opt_key, value] = key_val;
        
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
    
    //apply very_complicated_convert
    DynamicPrintConfig copy = object_config.get();
    copy.parent = &print_config;
    very_complicated_convert(config_substitutions, copy);
    object_config.apply(copy);
    
    // final transform
    //object_config.convert_from_prusa(print_config, with_phony);
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

