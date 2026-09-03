///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_config_types_h_
#define slic3r_config_types_h_

#define SLIC3R_PLUGIN_API_CONFIG_TYPES_MAJOR 1u
#define SLIC3R_PLUGIN_API_CONFIG_TYPES_MINOR 0u

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================== CONFIG ENUMS ========================== */

/* What type? bool, int, string etc. */
typedef enum raw_config_option_type {
    RAW_CO_NONE = 0,
    RAW_CO_BOOL,
    RAW_CO_INT,
    RAW_CO_FLOAT,
    RAW_CO_PERCENT,
    RAW_CO_FLOAT_OR_PERCENT,
    RAW_CO_STRING,
    RAW_CO_POINT,
    RAW_CO_ENUM,
    RAW_CO_GRAPH,
    RAW_CO_VECTOR_BOOL,
    RAW_CO_VECTOR_INT,
    RAW_CO_VECTOR_FLOAT,
    RAW_CO_VECTOR_PERCENT,
    RAW_CO_VECTOR_FLOAT_OR_PERCENT,
    RAW_CO_VECTOR_STRING,
    RAW_CO_VECTOR_POINT,
    RAW_CO_VECTOR_ENUM,
    RAW_CO_VECTOR_GRAPH,
} raw_config_option_type;

/* Where the option instance is stored */
typedef enum raw_container_type {
    RAW_CONTAINER_TYPE_NONE = 0,
    RAW_CONTAINER_TYPE_PROJECT,
    RAW_CONTAINER_TYPE_PLATER,
    RAW_CONTAINER_TYPE_OBJECT,
    RAW_CONTAINER_TYPE_LAYER,
    RAW_CONTAINER_TYPE_REGION
} raw_container_type;

/* Where the option instance is available */
typedef enum raw_option_preset_type : uint32_t{
    RAW_PRESET_TYPE_NONE = 0,
    RAW_PRESET_TYPE_FFF_PRINT,
    RAW_PRESET_TYPE_FFF_FILAMENT,
    RAW_PRESET_TYPE_FFF_FILAMENT_OVERRIDE,
    RAW_PRESET_TYPE_FFF_TOOL_EXTRUDER,
    RAW_PRESET_TYPE_FFF_TOOL_EXTRUDER_RETRACTION,
    RAW_PRESET_TYPE_FFF_TOOL_MILLING,
    RAW_PRESET_TYPE_FFF_PRINTER,
    RAW_PRESET_TYPE_FFF_PRINTER_MACHINE_LIMITS,
    RAW_PRESET_TYPE_SLA_PRINT,
    RAW_PRESET_TYPE_SLA_MATERIAL,
    RAW_PRESET_TYPE_SLA_MATERIAL_OVERRIDE,
    RAW_PRESET_TYPE_SLA_PRINTER,
    RAW_PRESET_TYPE_COUNT
} raw_option_preset_type;

#ifdef __cplusplus
} // extern "C"
#endif

#endif // slic3r_config_types_h_
