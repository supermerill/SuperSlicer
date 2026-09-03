///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_config_def_h_
#define slic3r_config_def_h_

#define SLIC3R_PLUGIN_API_CONFIG_DEF_MAJOR 1u
#define SLIC3R_PLUGIN_API_CONFIG_DEF_MINOR 0u

#include "slic3r_config_types.h"
#include "slic3r_plugin_types.h"
#include "slic3r_utils.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================== ENUMS ========================== */

/* GUI behavior / rendering type */
typedef enum raw_gui_type {
    RAW_GUI_TYPE_UNDEFINED = 0,

    /* Open enums (value may be outside predefined list) */
    RAW_GUI_TYPE_I_ENUM_OPEN,
    RAW_GUI_TYPE_F_ENUM_OPEN,
    RAW_GUI_TYPE_SELECT_OPEN,

    /* Color picker (string value) */
    RAW_GUI_TYPE_COLOR,

    /* Currently unused */
    RAW_GUI_TYPE_SLIDER,

    /* Static text */
    RAW_GUI_TYPE_LEGEND,

    /* Closed enum (must match provided values) */
    RAW_GUI_TYPE_SELECT_CLOSE
} raw_gui_type;

/* Category of a configuration field (GUI grouping) */
typedef enum raw_option_category {
    RAW_OPTION_CATEGORY_NONE = 0,

    RAW_OPTION_CATEGORY_PERIMETER,
    RAW_OPTION_CATEGORY_SLICING,
    RAW_OPTION_CATEGORY_INFILL,
    RAW_OPTION_CATEGORY_IRONING,
    RAW_OPTION_CATEGORY_FUZZY_SKIN,
    RAW_OPTION_CATEGORY_SKIRT_BRIM,
    RAW_OPTION_CATEGORY_SUPPORT,
    RAW_OPTION_CATEGORY_SPEED,
    RAW_OPTION_CATEGORY_WIDTH,
    RAW_OPTION_CATEGORY_EXTRUDERS,
    RAW_OPTION_CATEGORY_OUTPUT,
    RAW_OPTION_CATEGORY_NOTES,
    RAW_OPTION_CATEGORY_DEPENDENCIES,

    RAW_OPTION_CATEGORY_FILAMENT,
    RAW_OPTION_CATEGORY_COOLING,
    RAW_OPTION_CATEGORY_ADVANCED,
    RAW_OPTION_CATEGORY_FILOVERRIDE,
    RAW_OPTION_CATEGORY_CUSTOMGCODE,

    RAW_OPTION_CATEGORY_GENERAL,
    RAW_OPTION_CATEGORY_LIMITS,
    RAW_OPTION_CATEGORY_MMSETUP,
    RAW_OPTION_CATEGORY_FIRMWARE,

    RAW_OPTION_CATEGORY_PAD,
    RAW_OPTION_CATEGORY_PAD_SUPP,
    RAW_OPTION_CATEGORY_WIPE,

    RAW_OPTION_CATEGORY_HOLLOWING,

    RAW_OPTION_CATEGORY_MILLING_EXTRUDERS,
    RAW_OPTION_CATEGORY_MILLING

} raw_option_category;

/* Logical level of the option (UI / filtering purpose) */
typedef enum raw_option_level {
    RAW_OPTION_LEVEL_BASIC = 0,
    RAW_OPTION_LEVEL_ADVANCED,
    RAW_OPTION_LEVEL_EXPERT
} raw_option_level;

/* Which printer technology this config applies to */
typedef enum raw_printer_technology {
    RAW_PT_NONE = 0,
    RAW_PT_FFF,
    RAW_PT_SLA
} raw_printer_technology;

/* Bitmask flags for modes (mirrors ConfigOptionMode) */
typedef uint64_t raw_config_option_mode;
#define RAW_CONFIG_OPTION_MODE_NONE 0
#define RAW_CONFIG_OPTION_MODE_SIMPLE 1
#define RAW_CONFIG_OPTION_MODE_ADVANCED (1 << 1)
#define RAW_CONFIG_OPTION_MODE_EXPERT (1 << 2)
#define RAW_CONFIG_OPTION_MODE_ADV_EXP RAW_CONFIG_OPTION_MODE_ADVANCED | RAW_CONFIG_OPTION_MODE_EXPERT
#define RAW_CONFIG_OPTION_MODE_SIM_ADV_EXP RAW_CONFIG_OPTION_MODE_SIMPLE | RAW_CONFIG_OPTION_MODE_ADVANCED | RAW_CONFIG_OPTION_MODE_EXPERT
#define RAW_CONFIG_OPTION_MODE_PRUSA (1 << 3)
#define RAW_CONFIG_OPTION_MODE_SUSI (1 << 4)
#define RAW_CONFIG_OPTION_MODE_HIDDEN (1 << 5)

/* Generic extensibility flags */
typedef uint64_t RawConfigOptionFlags;

/* ========================== ARRAY HELPERS ========================== */

/* Pair of (value, label) for enum entries */
typedef struct key_value_string_pair_t {
    const char *value; /* stored value */
    const char *label; /* display label */
} key_value_string_pair_t;

/* Array of enum pairs */
typedef struct key_value_string_pair_array_t {
    const key_value_string_pair_t *items;
    uint32_t count;
} key_value_string_pair_array_t;

/* ========================== ENUM DEFINITION ========================== */

/*
Definition of values / labels for a combo box.
A value is a lowercase string with only alphanumerical and '_' characters. It is used to serialize the value.
A Label is the string that is shown in the interface.
*/
typedef struct option_enum_def_t {
    key_value_string_pair_array_t value_label_pairs; /* value+label pairs */
} option_enum_def_t;


/* ========================== MAIN STRUCT ========================== */

/*
Definition of a configuration value for:
- GUI presentation
- editing
- value mapping
- config file handling

This is the C-compatible version of ConfigOptionDef.
*/
typedef struct raw_config_option_def {

    /* Identifier of this option (unique key) */
    const char *opt_key;

    /* What type? bool, int, string etc. */
    raw_config_option_type type;

    /* Category of the option (GUI grouping) */
    raw_option_category category;

    /* Logical level (basic/advanced/expert) */
    raw_option_level level;

    /* Where this option is accessible */
    raw_container_type container_type;

    /* Type of preset it resides in */
    raw_option_preset_type option_preset_type;

    /* GUI type (affects rendering & interaction) */
    raw_gui_type gui_type;

    /* Which printer technology this applies to */
    raw_printer_technology printer_technology;

    /*
    Earliest slicing step invalidated when this option changes.
    The default STEP_NONE means "not filled" for plugin-created options: while
    a plugin is initialized, the host replaces it with the plugin's own step.
    Use STEP_ANY to force conservative full invalidation. Outside plugin option
    registration, STEP_NONE still means no slicing invalidation.
    */
    slicing_step_t invalidates_step;

    /* If optional and enabled, may not be serialized */
    int32_t is_optional;

    /* GUI flags (e.g. "serialized", "show_value") */
    const char *gui_flags;

    /*
    Label of the GUI input field.
    Short label used in grouped views.
    */
    const char *label;

    /*
    Full label used when shown standalone or in overrides.
    If NULL, label is used instead.
    */
    const char *full_label;

    /* Tooltip text shown in GUI */
    const char *tooltip;

    /*
    Gettext domain used for every user-facing text in this definition,
    including enum labels. NULL or empty selects the registering plugin's
    default domain. The domain must either be "Slic3r" or have been
    registered with orchestrator_register_translation_catalog().
    */
    const char *translation_domain;

    /* Text displayed next to input (e.g. unit) */
    const char *sidetext;

    /* CLI representation (command-line argument format) */
    const char *cli;

    /*
    Reference option key for percentage-based values.
    Example: speed relative to another speed.
    */
    const char *ratio_over;

    /* True for multiline text inputs */
    int32_t multiline;

    /* If true, GUI input spans full width */
    int32_t full_width;

    /* If true, display as code (monospace) */
    int32_t is_code;

    /*
    For array settings:
    if true, size matches number of extruders
    */
    int32_t is_vector_extruder;

    /* Not editable (read-only field) */
    int32_t readonly;

    /*
    Can be "phony":
    if missing at load, GUI adapts accordingly
    */
    int32_t can_phony;
    
    /* If true, option can be disabled (separate flag, not nullable) */
    /* Can be enabled/disabled via checkbox */
    int32_t can_be_disabled;

    /* Height of multiline input */
    int32_t height;

    /* Width of input field */
    int32_t width;

    /* Width of label */
    int32_t label_width;

    /* If true, label aligned left instead of right */
    int32_t aligned_label_left;

    /* Width of sidetext */
    int32_t sidetext_width;

    /*
    Numeric limits:
    If not set, defaults to (-inf, +inf)
    */
    double min_value;
    double max_value;
    int32_t has_min;
    int32_t has_max;

    /*
    Max literal check:
    Used to detect missing % or abnormal values.
    */
    double max_literal_value;
    int32_t max_literal_is_percent;
    int32_t has_max_literal;

    /* Display precision (digits after decimal point) */
    int32_t precision;

    /* Mode flags (bitmask) */
    raw_config_option_mode mode;

    /* Generic flags for future extension */
    RawConfigOptionFlags flags;

    /*
    Legacy names for this option.
    Used when parsing old config files.
    */
    const_strings_t aliases;

    /*
    Shortcut mapping:
    one option maps to multiple internal ones
    */
    const_strings_t shortcut;

    /*
    Dependencies:
    list of "opt_key#idx" that affect this value
    */
    const_strings_t depends_on;

    /* Default value (serialized in a string) */
    const char* default_serialized_value;

    /* Enum definition (for select/enum types) */
    option_enum_def_t enum_def;

    /* Reserved for future expansion (must be zero) */
    uint64_t reserved_u64[8];
    const void *reserved_ptr[8];

} raw_config_option_def;

/* ========================= CONFIG CREATION ========================= */
typedef enum option_def_error_code {
    OPTION_DEF_ERROR_OK = 0,
    OPTION_DEF_ERROR_ALREADY_EXISTS,
    OPTION_DEF_ERROR_INVALID_ARGUMENT,
    OPTION_DEF_ERROR_INTERNAL
} option_def_error_code;

/*
Initialize a config option definition with safe defaults.
invalidates_step defaults to STEP_NONE. During plugin initialization the host
interprets that default as "use the plugin's own slicing step". Set STEP_ANY
explicitly if an option must invalidate the whole slicing state.
*/
static inline raw_config_option_def raw_config_option_def_init()
{
    raw_config_option_def def = {0};
    def.height = -1;
    def.width = -1;
    def.label_width = -1;
    def.sidetext_width = -1;
    def.invalidates_step = STEP_NONE;
    return def;
}

/*
Create a new config option definition.
The options will be available in the gui and you will have access to them 
*/
SLIC3R_HOST_API option_def_error_code orchestrator_create_option_def(
    orchestrator_handle *orch,
    const raw_config_option_def *def
);

/* ========================= CONFIG DEFINITION REQUEST ========================= */
typedef struct config_option_handle config_option_handle;
typedef struct print_config_def_handler print_config_def_handler;

/*
Ask for the global definition handler
*/
SLIC3R_HOST_API const print_config_def_handler *orchestrator_get_print_config_def(orchestrator_handle *);

/*
Ask the global definition handler to copy the definition data from an option into out.
*/
SLIC3R_HOST_API void printconfigdef_get_config_definition(const orchestrator_handle*, const char* id, raw_config_option_def *out);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // slic3r_config_def_h_
