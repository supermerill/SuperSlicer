///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
// SLA-specific static print configuration classes.
//
// This header keeps the resin-printing configuration hierarchy separate from
// both the common PrintConfig.hpp base layer and the larger FFF configuration
// hierarchy.

#ifndef slic3r_SLAPrintConfig_hpp_
#define slic3r_SLAPrintConfig_hpp_

#include "libslic3r/Config/PrintConfig.hpp"
#include "libslic3r/SLA/SupportTreeStrategies.hpp"

namespace Slic3r {

void initialize_sla_print_config_cache();

enum SLADisplayOrientation {
    sladoLandscape,
    sladoPortrait
};

using SLASupportTreeType = sla::SupportTreeType;
using SLAPillarConnectionMode = sla::PillarConnectionMode;

// from prusa, not used in superslicer (as we can choose the width of inner & outer separatly.
enum SLAMaterialSpeed { slamsSlow, slamsFast, slamsHighViscosity };

#define CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(NAME) \
    template<> const t_config_enum_names& ConfigOptionEnum<NAME>::get_enum_names(); \
    template<> const t_config_enum_values& ConfigOptionEnum<NAME>::get_enum_values();

CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(SLADisplayOrientation)
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(SLAPillarConnectionMode)
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(SLAMaterialSpeed)
CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(SLASupportTreeType)

#undef CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS

PRINT_CONFIG_CLASS_DEFINE_WITH_SCOPE(
    SLAPrintConfig,
    StaticPrintConfig::DynamicOptionScope::SLAPrint,
    ((ConfigOptionString, output_filename_format))
    ((ConfigOptionString, print_custom_variables))
)

PRINT_CONFIG_CLASS_DEFINE_WITH_SCOPE(
    SLAPrintObjectConfig,
    StaticPrintConfig::DynamicOptionScope::SLAObject,

    ((ConfigOptionFloat, layer_height))

    ((ConfigOptionFloat, model_precision))

    //Number of the layers needed for the exposure time fade [3;20]
    ((ConfigOptionInt, faded_layers))/*= 10*/

    ((ConfigOptionFloat, slice_closing_radius))
    ((ConfigOptionEnum<SlicingMode>, slicing_mode))

    // Enabling or disabling support creation
    ((ConfigOptionBool,  supports_enable))

    ((ConfigOptionEnum<sla::SupportTreeType>, support_tree_type))

    // Diameter in mm of the pointing side of the head.
    ((ConfigOptionFloat, support_head_front_diameter))/*= 0.2*/

    // How much the pinhead has to penetrate the model surface
    ((ConfigOptionFloat, support_head_penetration))/*= 0.2*/

    // Width in mm from the back sphere center to the front sphere center.
    ((ConfigOptionFloat, support_head_width))/*= 1.0*/

    // Radius in mm of the support pillars.
    ((ConfigOptionFloat, support_pillar_diameter))/*= 0.8*/

    // The percentage of smaller pillars compared to the normal pillar diameter
    // which are used in problematic areas where a normal pilla cannot fit.
    ((ConfigOptionPercent, support_small_pillar_diameter_percent))

    // How much bridge (supporting another pinhead) can be placed on a pillar.
    ((ConfigOptionInt,   support_max_bridges_on_pillar))

    // How the pillars are bridged together
    ((ConfigOptionEnum<SLAPillarConnectionMode>, support_pillar_connection_mode))

    // Generate only ground facing supports
    ((ConfigOptionBool, support_buildplate_only))

    ((ConfigOptionFloat, support_max_weight_on_model))

    // Generate only ground facing supports
    ((ConfigOptionBool, support_enforcers_only))

    // TODO: unimplemented at the moment. This coefficient will have an impact
    // when bridges and pillars are merged. The resulting pillar should be a bit
    // thicker than the ones merging into it. How much thicker? I don't know
    // but it will be derived from this value.
    ((ConfigOptionFloat, support_pillar_widening_factor))

    // Radius in mm of the pillar base.
    ((ConfigOptionFloat, support_base_diameter))/*= 2.0*/

    // The height of the pillar base cone in mm.
    ((ConfigOptionFloat, support_base_height))/*= 1.0*/

    // The minimum distance of the pillar base from the model in mm.
    ((ConfigOptionFloat, support_base_safety_distance)) /*= 1.0*/

    // The default angle for connecting support sticks and junctions.
    ((ConfigOptionFloat, support_critical_angle))/*= 45*/

    // The max length of a bridge in mm
    ((ConfigOptionFloat, support_max_bridge_length))/*= 15.0*/

    // The max distance of two pillars to get cross linked.
    ((ConfigOptionFloat, support_max_pillar_link_distance))

    // The elevation in Z direction upwards. This is the space between the pad
    // and the model object's bounding box bottom. Units in mm.
    ((ConfigOptionFloat, support_object_elevation))/*= 5.0*/


    // Branching tree

    // Diameter in mm of the pointing side of the head.
    ((ConfigOptionFloat, branchingsupport_head_front_diameter))/*= 0.2*/

    // How much the pinhead has to penetrate the model surface
    ((ConfigOptionFloat, branchingsupport_head_penetration))/*= 0.2*/

    // Width in mm from the back sphere center to the front sphere center.
    ((ConfigOptionFloat, branchingsupport_head_width))/*= 1.0*/

    // Radius in mm of the support pillars.
    ((ConfigOptionFloat, branchingsupport_pillar_diameter))/*= 0.8*/

    // The percentage of smaller pillars compared to the normal pillar diameter
    // which are used in problematic areas where a normal pilla cannot fit.
    ((ConfigOptionPercent, branchingsupport_small_pillar_diameter_percent))

    // How much bridge (supporting another pinhead) can be placed on a pillar.
    ((ConfigOptionInt,   branchingsupport_max_bridges_on_pillar))

    // How the pillars are bridged together
    ((ConfigOptionEnum<SLAPillarConnectionMode>, branchingsupport_pillar_connection_mode))

    // Generate only ground facing supports
    ((ConfigOptionBool, branchingsupport_buildplate_only))

    ((ConfigOptionFloat, branchingsupport_max_weight_on_model))

    ((ConfigOptionFloat, branchingsupport_pillar_widening_factor))

    // Radius in mm of the pillar base.
    ((ConfigOptionFloat, branchingsupport_base_diameter))/*= 2.0*/

    // The height of the pillar base cone in mm.
    ((ConfigOptionFloat, branchingsupport_base_height))/*= 1.0*/

    // The minimum distance of the pillar base from the model in mm.
    ((ConfigOptionFloat, branchingsupport_base_safety_distance)) /*= 1.0*/

    // The default angle for connecting support sticks and junctions.
    ((ConfigOptionFloat, branchingsupport_critical_angle))/*= 45*/

    // The max length of a bridge in mm
    ((ConfigOptionFloat, branchingsupport_max_bridge_length))/*= 15.0*/

    // The max distance of two pillars to get cross linked.
    ((ConfigOptionFloat, branchingsupport_max_pillar_link_distance))

    // The elevation in Z direction upwards. This is the space between the pad
    // and the model object's bounding box bottom. Units in mm.
    ((ConfigOptionFloat, branchingsupport_object_elevation))/*= 5.0*/



    /////// Following options influence automatic support points placement:
    ((ConfigOptionInt, support_points_density_relative))
    ((ConfigOptionFloat, support_points_minimal_distance))

    // Now for the base pool (pad) /////////////////////////////////////////////

    // Enabling or disabling support creation
    ((ConfigOptionBool,  pad_enable))

    // The thickness of the pad walls
    ((ConfigOptionFloat, pad_wall_thickness))/*= 2*/

    // The height of the pad from the bottom to the top not considering the pit
    ((ConfigOptionFloat, pad_wall_height))/*= 5*/

    // How far should the pad extend around the contained geometry
    ((ConfigOptionFloat, pad_brim_size))

    // The greatest distance where two individual pads are merged into one. The
    // distance is measured roughly from the centroids of the pads.
    ((ConfigOptionFloat, pad_max_merge_distance))/*= 50*/

    // The smoothing radius of the pad edges
    // ((ConfigOptionFloat, pad_edge_radius))/*= 1*/;

    // The slope of the pad wall...
    ((ConfigOptionFloat, pad_wall_slope))

    // /////////////////////////////////////////////////////////////////////////
    // Zero elevation mode parameters:
    //    - The object pad will be derived from the model geometry.
    //    - There will be a gap between the object pad and the generated pad
    //      according to the support_base_safety_distance parameter.
    //    - The two pads will be connected with tiny connector sticks
    // /////////////////////////////////////////////////////////////////////////

    // Disable the elevation (ignore its value) and use the zero elevation mode
    ((ConfigOptionBool,  pad_around_object))

    ((ConfigOptionBool, pad_around_object_everywhere))

    // This is the gap between the object bottom and the generated pad
    ((ConfigOptionFloat, pad_object_gap))

    // How far to place the connector sticks on the object pad perimeter
    ((ConfigOptionFloat, pad_object_connector_stride))

    // The width of the connectors sticks
    ((ConfigOptionFloat, pad_object_connector_width))

    // How much should the tiny connectors penetrate into the model body
    ((ConfigOptionFloat, pad_object_connector_penetration))

    // /////////////////////////////////////////////////////////////////////////
    // Model hollowing parameters:
    //   - Models can be hollowed out as part of the SLA print process
    //   - Thickness of the hollowed model walls can be adjusted
    //   -
    //   - Additional holes will be drilled into the hollow model to allow for
    //   - resin removal.
    // /////////////////////////////////////////////////////////////////////////

    ((ConfigOptionBool, hollowing_enable))

    // The minimum thickness of the model walls to maintain. Note that the
    // resulting walls may be thicker due to smoothing out fine cavities where
    // resin could stuck.
    ((ConfigOptionFloat, hollowing_min_thickness))

    // Indirectly controls the voxel size (resolution) used by openvdb
    ((ConfigOptionFloat, hollowing_quality))

    // Indirectly controls the minimum size of created cavities.
    ((ConfigOptionFloat, hollowing_closing_distance))
)


PRINT_CONFIG_CLASS_DEFINE_WITH_SCOPE(
    SLAMaterialConfig,
    StaticPrintConfig::DynamicOptionScope::SLAMaterial,

    ((ConfigOptionFloat,                       initial_layer_height))
    ((ConfigOptionFloat,                       bottle_cost))
    ((ConfigOptionFloat,                       bottle_volume))
    ((ConfigOptionFloat,                       bottle_weight))
    ((ConfigOptionStrings,                     filament_custom_variables))
    ((ConfigOptionFloat,                       material_density))
    ((ConfigOptionFloat,                       exposure_time))
    ((ConfigOptionFloat,                       initial_exposure_time))
    ((ConfigOptionFloats,                      material_correction))
    ((ConfigOptionFloat,                       material_correction_x))
    ((ConfigOptionFloat,                       material_correction_y))
    ((ConfigOptionFloat,                       material_correction_z))
    ((ConfigOptionEnum<SLAMaterialSpeed>,      material_print_speed))
    ((ConfigOptionFloat,                       material_ow_support_pillar_diameter))
    ((ConfigOptionFloat,                       material_ow_branchingsupport_pillar_diameter))
    ((ConfigOptionFloat,                       material_ow_support_head_front_diameter))
    ((ConfigOptionFloat,                       material_ow_branchingsupport_head_front_diameter))
    ((ConfigOptionFloat,                       material_ow_support_head_penetration))
    ((ConfigOptionFloat,                       material_ow_branchingsupport_head_penetration))
    ((ConfigOptionFloat,                       material_ow_support_head_width))
    ((ConfigOptionFloat,                       material_ow_branchingsupport_head_width))
    ((ConfigOptionInt,                         material_ow_support_points_density_relative))

    ((ConfigOptionFloat,                       material_ow_first_layer_size_compensation)) /* material_ow_elefant_foot_compensation */
    ((ConfigOptionFloat,                       material_ow_relative_correction_x))
    ((ConfigOptionFloat,                       material_ow_relative_correction_y))
    ((ConfigOptionFloat,                       material_ow_relative_correction_z))
)

PRINT_CONFIG_CLASS_DEFINE_WITH_SCOPE(
    SLAPrinterConfig,
    StaticPrintConfig::DynamicOptionScope::SLAPrinter,

    ((ConfigOptionEnum<PrinterTechnology>,      printer_technology))
    ((ConfigOptionEnum<OutputFormat>,           output_format))
    ((ConfigOptionPoints,                       bed_shape))
    ((ConfigOptionFloat,                        max_print_height))
    ((ConfigOptionFloat,                        display_width))
    ((ConfigOptionFloat,                        display_height))
    ((ConfigOptionInt,                          display_pixels_x))
    ((ConfigOptionInt,                          display_pixels_y))
    ((ConfigOptionEnum<SLADisplayOrientation>,  display_orientation))
    ((ConfigOptionBool,                         display_mirror_x))
    ((ConfigOptionBool,                         display_mirror_y))
    ((ConfigOptionFloats,                       relative_correction))
    ((ConfigOptionFloat,                        relative_correction_x))
    ((ConfigOptionFloat,                        relative_correction_y))
    ((ConfigOptionFloat,                        relative_correction_z))
    ((ConfigOptionFloat,                        absolute_correction))
    ((ConfigOptionFloat,                        first_layer_size_compensation))
    ((ConfigOptionFloat,                        elephant_foot_min_width))
    ((ConfigOptionFloat,                        gamma_correction))
    ((ConfigOptionFloat,                        fast_tilt_time))
    ((ConfigOptionFloat,                        slow_tilt_time))
    ((ConfigOptionFloat,                        high_viscosity_tilt_time))
    ((ConfigOptionFloat,                        area_fill))
    ((ConfigOptionFloat,                        min_exposure_time))
    ((ConfigOptionFloat,                        max_exposure_time))
    ((ConfigOptionFloat,                        min_initial_exposure_time))
    ((ConfigOptionFloat,                        max_initial_exposure_time))
    ((ConfigOptionString,                       printer_custom_variables))
    ((ConfigOptionFloat,                        sla_output_precision))
    ((ConfigOptionPoints,                       thumbnails))
    ((ConfigOptionString,                       thumbnails_color))
    ((ConfigOptionBool,                         thumbnails_custom_color))
    ((ConfigOptionBool,                         thumbnails_tag_format))
    ((ConfigOptionBool,                         thumbnails_with_bed))
    ((ConfigOptionBool,                         thumbnails_with_support))
    ((ConfigOptionFloat,                        z_rotate))
)

PRINT_CONFIG_CLASS_DERIVED_DEFINE0_WITH_SCOPE(
    SLAFullPrintConfig,
    (SLAPrinterConfig, SLAPrintConfig, SLAPrintObjectConfig, SLAMaterialConfig),
    StaticPrintConfig::DynamicOptionScope::SLAAggregate
)

Points get_bed_shape(const SLAPrinterConfig &cfg);
std::string get_sla_suptree_prefix(const DynamicPrintConfig &config);

void init_sla_params(PrintConfigDef &definition);

} // namespace Slic3r

#endif // slic3r_SLAPrintConfig_hpp_
