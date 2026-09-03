///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_config_option_type_h_
#define slic3r_config_option_type_h_

#define SLIC3R_PLUGIN_API_CONFIG_OPTION_TYPE_MAJOR 1u
#define SLIC3R_PLUGIN_API_CONFIG_OPTION_TYPE_MINOR 0u

/*
Configuration option value kinds shared by the host and the plugin ABI.

The enumerator names are prefixed because C enum values live in the global
namespace. Keep this header small: ConfigOption.hpp includes it to reuse the exact
same numeric values without pulling the full plugin API function declarations.
*/
typedef enum config_option_type
{
    /* Bit added to a scalar option kind to form the matching vector kind. */
    SLIC3R_CONFIG_OPTION_VECTOR_TYPE = 0x4000,
    /* No option / invalid option type. */
    SLIC3R_CONFIG_OPTION_NONE = 0,
    /* Single floating-point value. */
    SLIC3R_CONFIG_OPTION_FLOAT = 1,
    /* Vector of floating-point values. */
    SLIC3R_CONFIG_OPTION_FLOATS = SLIC3R_CONFIG_OPTION_FLOAT + SLIC3R_CONFIG_OPTION_VECTOR_TYPE,
    /* Single integer value. */
    SLIC3R_CONFIG_OPTION_INT = 2,
    /* Vector of integer values. */
    SLIC3R_CONFIG_OPTION_INTS = SLIC3R_CONFIG_OPTION_INT + SLIC3R_CONFIG_OPTION_VECTOR_TYPE,
    /* Single string value. */
    SLIC3R_CONFIG_OPTION_STRING = 3,
    /* Vector of string values. */
    SLIC3R_CONFIG_OPTION_STRINGS = SLIC3R_CONFIG_OPTION_STRING + SLIC3R_CONFIG_OPTION_VECTOR_TYPE,
    /* Percent value. Currently only used for infill and flow ratio. */
    SLIC3R_CONFIG_OPTION_PERCENT = 4,
    /* Vector of percent values. Currently used for retract before wipe. */
    SLIC3R_CONFIG_OPTION_PERCENTS = SLIC3R_CONFIG_OPTION_PERCENT + SLIC3R_CONFIG_OPTION_VECTOR_TYPE,
    /* Fraction or absolute value. */
    SLIC3R_CONFIG_OPTION_FLOAT_OR_PERCENT = 5,
    /* Vector of fraction-or-absolute values. */
    SLIC3R_CONFIG_OPTION_FLOATS_OR_PERCENTS = SLIC3R_CONFIG_OPTION_FLOAT_OR_PERCENT + SLIC3R_CONFIG_OPTION_VECTOR_TYPE,
    /* Single 2D point value (Point2f). Currently not used. */
    SLIC3R_CONFIG_OPTION_POINT = 6,
    /* Vector of 2D point values (Point2f). Used for print bed shape and extruder offsets. */
    SLIC3R_CONFIG_OPTION_POINTS = SLIC3R_CONFIG_OPTION_POINT + SLIC3R_CONFIG_OPTION_VECTOR_TYPE,
    /* Single 3D point value. */
    SLIC3R_CONFIG_OPTION_POINT3 = 7,
    /* Single boolean value. */
    SLIC3R_CONFIG_OPTION_BOOL = 8,
    /* Vector of boolean values. */
    SLIC3R_CONFIG_OPTION_BOOLS = SLIC3R_CONFIG_OPTION_BOOL + SLIC3R_CONFIG_OPTION_VECTOR_TYPE,
    /* Generic enum value. */
    SLIC3R_CONFIG_OPTION_ENUM = 9,
    /* Graph of double -> double values. */
    SLIC3R_CONFIG_OPTION_GRAPH = 10,
    /* Vector of double -> double graphs. */
    SLIC3R_CONFIG_OPTION_GRAPHS = SLIC3R_CONFIG_OPTION_GRAPH + SLIC3R_CONFIG_OPTION_VECTOR_TYPE,
} config_option_type;

#endif // slic3r_config_option_type_h_
