///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_plugin_types_h_
#define slic3r_plugin_types_h_

#include <stdint.h>

#include "slic3r_config_types.h"
#include "slic3r_gcode_firmware.h"
#include "slic3r_gcode_script.h"
#include "slic3r_plugin_run_context.h"
#include "slic3r_printing_plan.h"
#include "slic3r_slicing_step.h"
#include "slic3r_utils.h"

#define SLIC3R_PLUGIN_ABI_VERSION 51u

#ifdef __cplusplus
extern "C" {
#endif

/* ========================= HANDLES ========================= */

typedef struct orchestrator_handle orchestrator_handle;

/* ========================= PLUGIN CALLBACKS ========================= */

typedef void (*plugin_initialize_fn)(void *plugin_ctx, storage_handle *storage);

/* This plugin_run_context is destroyed after each of these function return, so don't keep it, copy the data you want instead. */
typedef void (*plugin_setup_fn)(void *plugin_ctx, const plugin_run_context *run_ctx, uint32_t run_count);
typedef void (*plugin_setup_run_fn)(void *plugin_ctx, const plugin_run_context *run_ctx);
typedef void (*plugin_run_fn)(void *plugin_ctx, const plugin_run_context *run_ctx);

/*
One configuration option read or defined by a plugin.

type is mandatory and must not be RAW_CO_NONE. It lets the host verify that the
plugin and the option owner agree on the value representation.

container_type and option_preset_type are optional filters. Use *_NONE when the
plugin only cares about the value type and key, for example when a setting may
legitimately live in several preset buckets.
*/
typedef struct raw_used_config_key {
    const char *key;
    raw_config_option_type type;
    raw_container_type container_type;
    raw_option_preset_type option_preset_type;
} raw_used_config_key;

/*
Return the configuration options read or defined by this plugin.

This uses the usual C double-call pattern:
- call with keys == NULL to get the number of entries to allocate;
- call again with an array of that size to receive borrowed key pointers and
  value expectations owned by the plugin.

The host uses this list to validate plugin contracts and to enable/disable GUI
fields when several plugins are available for an exclusive step and a project
selects one of them. Every key returned by defined_config_keys must have a
matching typed entry in this list, even when the plugin only defines that option
for its GUI or for another plugin.
*/
typedef int32_t (*plugin_used_config_keys_fn)(void *plugin_ctx, raw_used_config_key *keys);

/*
Return the configuration option keys defined by this plugin.

This is a lightweight declaration used for early validation before a plugin is
enabled from the GUI. The authoritative check still happens when the plugin
calls orchestrator_create_option_def(), because only that call contains the full
definition to compare.
Every key returned here must also appear in plugin_used_config_keys_fn. The host
uses that typed declaration to describe settings before an inactive plugin has
called initialize().

Use the same double-call convention as plugin_used_config_keys_fn.
*/
typedef int32_t (*plugin_defined_config_keys_fn)(void *plugin_ctx, const char **keys);

/* One independently versioned API contract required by a plugin. */
typedef struct plugin_api_requirement {
    uint32_t api_id;
    uint16_t major;
    uint16_t minor;
} plugin_api_requirement;

/* Return API requirements using the standard C double-call convention. */
typedef int32_t (*plugin_api_requirements_fn)(
    void *plugin_ctx,
    plugin_api_requirement *requirements);

/*
Each identifier represents one independently versioned API contract. Existing
identifiers are append-only: their numeric values must never be renumbered.
*/
typedef enum slic3r_plugin_api_id {
    SLIC3R_PLUGIN_API_PLUGIN = 0,
    SLIC3R_PLUGIN_API_RUNTIME,
    SLIC3R_PLUGIN_API_GEOMETRY,
    SLIC3R_PLUGIN_API_CONFIG,
    SLIC3R_PLUGIN_API_DATA_TREE,
    SLIC3R_PLUGIN_API_SURFACE,
    SLIC3R_PLUGIN_API_EXTRUSION,
    SLIC3R_PLUGIN_API_PERIMETER,
    SLIC3R_PLUGIN_API_INFILL,
    SLIC3R_PLUGIN_API_SUPPORT,
    SLIC3R_PLUGIN_API_ORDERING,
    SLIC3R_PLUGIN_API_GCODE,
    SLIC3R_PLUGIN_API_COUNT
} slic3r_plugin_api_id;

/* The major version identifies an incompatible contract */
#define SLIC3R_PLUGIN_API_PLUGIN_MAJOR    1u
#define SLIC3R_PLUGIN_API_PLUGIN_MINOR    0u
//#define SLIC3R_PLUGIN_API_RUNTIME_MAJOR   0u
//#define SLIC3R_PLUGIN_API_RUNTIME_MINOR   0u
//#define SLIC3R_PLUGIN_API_GEOMETRY_MAJOR  0u
//#define SLIC3R_PLUGIN_API_GEOMETRY_MINOR  0u
//#define SLIC3R_PLUGIN_API_CONFIG_MAJOR    0u
//#define SLIC3R_PLUGIN_API_CONFIG_MINOR    0u
//#define SLIC3R_PLUGIN_API_DATA_TREE_MAJOR 0u
//#define SLIC3R_PLUGIN_API_DATA_TREE_MINOR 0u
//#define SLIC3R_PLUGIN_API_SURFACE_MAJOR   0u
//#define SLIC3R_PLUGIN_API_SURFACE_MINOR   0u
//#define SLIC3R_PLUGIN_API_EXTRUSION_MAJOR 1u
//#define SLIC3R_PLUGIN_API_EXTRUSION_MINOR 0u
//#define SLIC3R_PLUGIN_API_PERIMETER_MAJOR 1u
//#define SLIC3R_PLUGIN_API_PERIMETER_MINOR 0u
//#define SLIC3R_PLUGIN_API_INFILL_MAJOR    1u
//#define SLIC3R_PLUGIN_API_INFILL_MINOR    0u
//#define SLIC3R_PLUGIN_API_SUPPORT_MAJOR   1u
//#define SLIC3R_PLUGIN_API_SUPPORT_MINOR   0u
//#define SLIC3R_PLUGIN_API_ORDERING_MAJOR  1u
//#define SLIC3R_PLUGIN_API_ORDERING_MINOR  0u
//#define SLIC3R_PLUGIN_API_GCODE_MAJOR     1u
//#define SLIC3R_PLUGIN_API_GCODE_MINOR     0u

typedef struct slic3r_plugin_api_version {
    uint16_t major;
    uint16_t minor;
} slic3r_plugin_api_version;

/* Return the API version currently used. */
static inline int32_t slic3r_plugin_api_current_version(slic3r_plugin_api_version *requirements) {
    if (requirements == NULL)
        return SLIC3R_PLUGIN_API_COUNT;

    requirements[SLIC3R_PLUGIN_API_PLUGIN].major = uint16_t(SLIC3R_PLUGIN_API_PLUGIN_MAJOR);
    requirements[SLIC3R_PLUGIN_API_PLUGIN].minor = uint16_t(SLIC3R_PLUGIN_API_PLUGIN_MINOR);

#ifdef SLIC3R_PLUGIN_API_PLUGIN_MAJOR
    requirements[SLIC3R_PLUGIN_API_PLUGIN].major = uint16_t(SLIC3R_PLUGIN_API_PLUGIN_MAJOR);
    requirements[SLIC3R_PLUGIN_API_PLUGIN].minor = uint16_t(SLIC3R_PLUGIN_API_PLUGIN_MINOR);
#else
    requirements[SLIC3R_PLUGIN_API_PLUGIN].major = uint16_t(0);
    requirements[SLIC3R_PLUGIN_API_PLUGIN].minor = uint16_t(0);
#endif

#ifdef SLIC3R_PLUGIN_API_RUNTIME_MAJOR
    requirements[SLIC3R_PLUGIN_API_RUNTIME].major = uint16_t(SLIC3R_PLUGIN_API_RUNTIME_MAJOR);
    requirements[SLIC3R_PLUGIN_API_RUNTIME].minor = uint16_t(SLIC3R_PLUGIN_API_RUNTIME_MINOR);
#else
    requirements[SLIC3R_PLUGIN_API_RUNTIME].major = uint16_t(0);
    requirements[SLIC3R_PLUGIN_API_RUNTIME].minor = uint16_t(0);
#endif

#ifdef SLIC3R_PLUGIN_API_GEOMETRY_MAJOR
    requirements[SLIC3R_PLUGIN_API_GEOMETRY].major = uint16_t(SLIC3R_PLUGIN_API_GEOMETRY_MAJOR);
    requirements[SLIC3R_PLUGIN_API_GEOMETRY].minor = uint16_t(SLIC3R_PLUGIN_API_GEOMETRY_MINOR);
#else
    requirements[SLIC3R_PLUGIN_API_GEOMETRY].major = uint16_t(0);
    requirements[SLIC3R_PLUGIN_API_GEOMETRY].minor = uint16_t(0);
#endif

#ifdef SLIC3R_PLUGIN_API_CONFIG_MAJOR
    requirements[SLIC3R_PLUGIN_API_CONFIG].major = uint16_t(SLIC3R_PLUGIN_API_CONFIG_MAJOR);
    requirements[SLIC3R_PLUGIN_API_CONFIG].minor = uint16_t(SLIC3R_PLUGIN_API_CONFIG_MINOR);
#else
    requirements[SLIC3R_PLUGIN_API_CONFIG].major = uint16_t(0);
    requirements[SLIC3R_PLUGIN_API_CONFIG].minor = uint16_t(0);
#endif

#ifdef SLIC3R_PLUGIN_API_DATA_TREE_MAJOR
    requirements[SLIC3R_PLUGIN_API_DATA_TREE].major = uint16_t(SLIC3R_PLUGIN_API_DATA_TREE_MAJOR);
    requirements[SLIC3R_PLUGIN_API_DATA_TREE].minor = uint16_t(SLIC3R_PLUGIN_API_DATA_TREE_MINOR);
#else
    requirements[SLIC3R_PLUGIN_API_DATA_TREE].major = uint16_t(0);
    requirements[SLIC3R_PLUGIN_API_DATA_TREE].minor = uint16_t(0);
#endif

#ifdef SLIC3R_PLUGIN_API_SURFACE_MAJOR
    requirements[SLIC3R_PLUGIN_API_SURFACE].major = uint16_t(SLIC3R_PLUGIN_API_SURFACE_MAJOR);
    requirements[SLIC3R_PLUGIN_API_SURFACE].minor = uint16_t(SLIC3R_PLUGIN_API_SURFACE_MINOR);
#else
    requirements[SLIC3R_PLUGIN_API_SURFACE].major = uint16_t(0);
    requirements[SLIC3R_PLUGIN_API_SURFACE].minor = uint16_t(0);
#endif

#ifdef SLIC3R_PLUGIN_API_EXTRUSION_MAJOR
    requirements[SLIC3R_PLUGIN_API_EXTRUSION].major = uint16_t(SLIC3R_PLUGIN_API_EXTRUSION_MAJOR);
    requirements[SLIC3R_PLUGIN_API_EXTRUSION].minor = uint16_t(SLIC3R_PLUGIN_API_EXTRUSION_MINOR);
#else
    requirements[SLIC3R_PLUGIN_API_EXTRUSION].major = uint16_t(0);
    requirements[SLIC3R_PLUGIN_API_EXTRUSION].minor = uint16_t(0);
#endif

#ifdef SLIC3R_PLUGIN_API_PERIMETER_MAJOR
    requirements[SLIC3R_PLUGIN_API_PERIMETER].major = uint16_t(SLIC3R_PLUGIN_API_PERIMETER_MAJOR);
    requirements[SLIC3R_PLUGIN_API_PERIMETER].minor = uint16_t(SLIC3R_PLUGIN_API_PERIMETER_MINOR);
#else
    requirements[SLIC3R_PLUGIN_API_PERIMETER].major = uint16_t(0);
    requirements[SLIC3R_PLUGIN_API_PERIMETER].minor = uint16_t(0);
#endif

#ifdef SLIC3R_PLUGIN_API_INFILL_MAJOR
    requirements[SLIC3R_PLUGIN_API_INFILL].major = uint16_t(SLIC3R_PLUGIN_API_INFILL_MAJOR);
    requirements[SLIC3R_PLUGIN_API_INFILL].minor = uint16_t(SLIC3R_PLUGIN_API_INFILL_MINOR);
#else
    requirements[SLIC3R_PLUGIN_API_INFILL].major = uint16_t(0);
    requirements[SLIC3R_PLUGIN_API_INFILL].minor = uint16_t(0);
#endif

#ifdef SLIC3R_PLUGIN_API_SUPPORT_MAJOR
    requirements[SLIC3R_PLUGIN_API_SUPPORT].major = uint16_t(SLIC3R_PLUGIN_API_SUPPORT_MAJOR);
    requirements[SLIC3R_PLUGIN_API_SUPPORT].minor = uint16_t(SLIC3R_PLUGIN_API_SUPPORT_MINOR);
#else
    requirements[SLIC3R_PLUGIN_API_SUPPORT].major = uint16_t(0);
    requirements[SLIC3R_PLUGIN_API_SUPPORT].minor = uint16_t(0);
#endif

#ifdef SLIC3R_PLUGIN_API_ORDERING_MAJOR
    requirements[SLIC3R_PLUGIN_API_ORDERING].major = uint16_t(SLIC3R_PLUGIN_API_ORDERING_MAJOR);
    requirements[SLIC3R_PLUGIN_API_ORDERING].minor = uint16_t(SLIC3R_PLUGIN_API_ORDERING_MINOR);
#else
    requirements[SLIC3R_PLUGIN_API_ORDERING].major = uint16_t(0);
    requirements[SLIC3R_PLUGIN_API_ORDERING].minor = uint16_t(0);
#endif

#ifdef SLIC3R_PLUGIN_API_GCODE_MAJOR
    requirements[SLIC3R_PLUGIN_API_GCODE].major = uint16_t(SLIC3R_PLUGIN_API_GCODE_MAJOR);
    requirements[SLIC3R_PLUGIN_API_GCODE].minor = uint16_t(SLIC3R_PLUGIN_API_GCODE_MINOR);
#else
    requirements[SLIC3R_PLUGIN_API_GCODE].major = uint16_t(0);
    requirements[SLIC3R_PLUGIN_API_GCODE].minor = uint16_t(0);
#endif

    return SLIC3R_PLUGIN_API_COUNT;
}


/* ========================= PLUGIN VTABLE ========================= */

typedef struct plugin_vtable {

    /*
    ABI version used to build this vtable.

    Keep this as the first field: the host can reject stale plugin instances
    before calling any function pointer whose slot may have moved.
    */
    uint32_t abi_version;

    /*
    Stable machine-readable id.

    This value is used in config files, dependencies and plugin activation
    lists. It must not be translated and should not change between releases
    unless the plugin is intentionally replaced by a different plugin.
    */
    const char* (*get_id)(void *plugin_ctx);

    /*
    Short user-facing name.

    This is displayed in combo boxes and plugin lists. It may contain spaces and
    should be clear to non-developers. It is only a label: the host still stores
    get_id() as the serialized value.
    */
    const char* (*get_name)(void *plugin_ctx);

    /*
    Optional longer user-facing description.

    Return an empty string if there is no useful description yet. The pointer is
    borrowed from the plugin and only read during registration.
    */
    const char* (*get_description)(void *plugin_ctx);

    /*
    Machine-readable exclusive group id.

    Plugins in the same exclusive group are alternatives: the project stores a
    selector setting and the host runs only the selected active plugin from the
    group. Return the plugin id when the plugin has no known alternatives yet;
    the host also treats NULL or an empty string as the plugin id for backward
    compatibility with older plugins. This gives every plugin a stable
    singleton group that future alternatives may join without modifying the
    original plugin.

    Some host-defined "unique" steps force all active plugins for that step into
    one exclusive group even if this callback returns empty. This lets old-style
    step replacement plugins use the same selection machinery.
    */
    const char* (*get_exclusive_group)(void *plugin_ctx);

    /*
    User-facing label for the exclusive group selector.

    If several active plugins declare the same group, the host uses the first
    non-empty label it sees in plugin execution order. Return an empty string to
    let the host use its default label.
    */
    const char* (*get_exclusive_group_label)(void *plugin_ctx);

    /*
    User-facing tooltip/description for the exclusive group selector.

    As with get_exclusive_group_label(), the first non-empty tooltip found for a
    group wins. Return an empty string to use the host default.
    */
    const char* (*get_exclusive_group_tooltip)(void *plugin_ctx);

    slicing_step_t (*get_step)(void *plugin_ctx);

    const_strings_t (*get_dependencies)(void *plugin_ctx);

    int32_t (*get_priority)(void *plugin_ctx);

    plugin_used_config_keys_fn used_config_keys;

    plugin_defined_config_keys_fn defined_config_keys;

    /**
     * Called once at startup, to be able to setup settings, via orchestrator_create_option_def
    */
    plugin_initialize_fn initialize;

    /*
    Called once before any setup_run()/run() call for this step/plugin pair.
    run_count is the number of run contexts that will be prepared for this
    plugin. Use it to reset plugin-side shared state such as progress helpers.
    */
    plugin_setup_fn setup;

    /*
    Called once for each run context before any run() starts. The host may call
    setup_run() in parallel, but it guarantees that all setup_run() calls finish
    before the first run() starts. Use it to estimate per-run work.
    */
    plugin_setup_run_fn setup_run;

    /*
    * Called once per object to do the step this plugin is made for.
    */
    plugin_run_fn run;

    /* Optional per-plugin API requirements. */
    plugin_api_requirements_fn api_requirements;

} plugin_vtable;

/* ========================= PLUGIN INSTANCE ========================= */

typedef struct plugin_instance {
    void *ctx;
    const plugin_vtable *vt;
} plugin_instance;

#ifdef __cplusplus
}
#endif

#endif // slic3r_plugin_types_h_
