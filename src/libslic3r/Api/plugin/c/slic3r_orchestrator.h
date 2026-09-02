///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_orchestrator_h_
#define slic3r_orchestrator_h_

/*
Property registration guide:
[Using Plugin Properties](../../../../../doc/plugins/properties.md)
*/

#include <stdint.h>

#include "slic3r_plugin_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================= REGISTRATION ========================= */

typedef struct config_handle config_handle;
typedef struct print_handle print_handle;

/*
Borrowed reference to one plugin registered in an orchestrator.

The handle remains valid until the orchestrator is destroyed. It does not keep
the plugin active and must only be passed back to the orchestrator that
returned it.
*/
typedef struct orchestrator_plugin_handle orchestrator_plugin_handle;

/*
One step-specific payload used by setup_run() and run(). The orchestrator does
not inspect data; its concrete type is the private contract shared by the
caller and the selected service plugin.
*/
typedef struct raw_plugin_run_payload {
    void *data;
} raw_plugin_run_payload;

/* Result of one host-managed plugin execution. */
typedef enum raw_plugin_execution_status {
    RAW_PLUGIN_EXECUTION_SUCCESS = 1,
    RAW_PLUGIN_EXECUTION_CANCELLED = 0,
    RAW_PLUGIN_EXECUTION_INVALID_ARGUMENT = -1,
    RAW_PLUGIN_EXECUTION_INACTIVE = -2,
    RAW_PLUGIN_EXECUTION_PLUGIN_ERROR = -3
} raw_plugin_execution_status;

/*
Register a plugin instance.
*/
SLIC3R_HOST_API void orchestrator_register_plugin(
    orchestrator_handle *orch,
    plugin_instance plugin
);

/*
Register a plugin instance and explicitly associate it with its installed
package directory. This variant is intended for loaders which discover a
plugin after the native package-loading scope has ended, notably the Python
loader. Relative resources, translations and diagnostics for the registered
plugin are resolved from package_root.

package_root must name the root containing description.ini and version.ini.
Normal native register_plugin() entry points should continue to call
orchestrator_register_plugin(), because PluginLoader already supplies their
package scope.
*/
SLIC3R_HOST_API void orchestrator_register_plugin_from_package(
    orchestrator_handle *orch,
    plugin_instance plugin,
    const char *package_root
);

/*
Register a service step which is not part of the host's compiled pipeline.

namespaced_name is the stable identity shared by providers and consumers, for
example "com.example.seam_placer". invalidates_step is the normal pipeline
step whose result becomes stale when the selected provider changes.

Registering the same name and invalidation again returns the same runtime id.
Registering the same name with another invalidation, exhausting the custom id
range, or passing invalid arguments returns STEP_NONE.

The returned id belongs to this orchestrator and must be registered again for
another orchestrator. Register it before registering plugin instances whose
get_step() returns that id.
*/
SLIC3R_HOST_API slicing_step_t orchestrator_register_step(
    orchestrator_handle *orchestrator,
    const char *namespaced_name,
    slicing_step_t invalidates_step
);

/*
Return the stable name of a dynamically registered step.

The returned string is borrowed from the orchestrator and remains valid until
its destruction. Built-in and unregistered numeric steps return NULL.
*/
SLIC3R_HOST_API const char *orchestrator_step_name(
    const orchestrator_handle *orchestrator,
    slicing_step_t step
);

/*
Find a plugin by its stable id, whether or not it is active.

Returns NULL for an invalid argument or an unknown id.
*/
SLIC3R_HOST_API const orchestrator_plugin_handle *orchestrator_find_plugin(
    orchestrator_handle *orchestrator,
    const char *plugin_id
);

/*
Select the active provider from one exclusive group on a step.

When several providers are active, config selects the provider through the
group's generated enum option. A missing config or selector uses the first
provider in normal priority order. Returns NULL when the group has no active
provider or any argument is invalid.
*/
SLIC3R_HOST_API const orchestrator_plugin_handle *orchestrator_select_plugin(
    orchestrator_handle *orchestrator,
    const config_handle *config,
    slicing_step_t step,
    const char *exclusive_group
);

/* Return borrowed identity metadata for a plugin handle. */
SLIC3R_HOST_API const char *orchestrator_plugin_id(
    const orchestrator_plugin_handle *plugin
);
SLIC3R_HOST_API slicing_step_t orchestrator_plugin_step(
    const orchestrator_plugin_handle *plugin
);

/*
Execute one active plugin through the complete host lifecycle.

setup() runs once with data == NULL. The host then calls setup_run() once for
each payload, waits for every setup_run() call, and calls run() once for each
payload. Calls inside either per-payload phase may run in parallel. The same
data pointer is supplied to both calls for an index, and object_idx/object_count
identify that index and the total count.

print and individual data pointers may be NULL. payloads may be NULL only when
run_count is zero. Concurrent or recursive executions of the same plugin
instance are not supported. Plugin code should be invoked from a sequential
orchestration phase and use the parallelism supplied by this function.
*/
SLIC3R_HOST_API raw_plugin_execution_status orchestrator_execute_plugin(
    orchestrator_handle *orchestrator,
    const orchestrator_plugin_handle *plugin,
    const print_handle *print,
    const raw_plugin_run_payload *payloads,
    uint32_t run_count
);

/*
Report a package-loading failure discovered by a secondary loader.

The main native loader records its failures directly. A runtime loader such as
the Python bridge runs in another shared library and uses these functions to
publish the same package-level diagnostics. package_root identifies the live
package directory containing description.ini and version.ini.
*/
typedef enum raw_plugin_package_load_error_code {
    RAW_PLUGIN_PACKAGE_LOAD_ERROR_PACKAGE_MISSING = 0,
    RAW_PLUGIN_PACKAGE_LOAD_ERROR_INVALID_PACKAGE = 1,
    RAW_PLUGIN_PACKAGE_LOAD_ERROR_LIBRARY_OPEN_FAILED = 2,
    RAW_PLUGIN_PACKAGE_LOAD_ERROR_MISSING_ABI_EXPORT = 3,
    RAW_PLUGIN_PACKAGE_LOAD_ERROR_ABI_MISMATCH = 4,
    RAW_PLUGIN_PACKAGE_LOAD_ERROR_MISSING_REGISTRATION_EXPORT = 5,
    RAW_PLUGIN_PACKAGE_LOAD_ERROR_REGISTRATION_FAILED = 6,
    RAW_PLUGIN_PACKAGE_LOAD_ERROR_NO_PLUGINS_REGISTERED = 7,
    RAW_PLUGIN_PACKAGE_LOAD_ERROR_PYTHON_RUNTIME_UNAVAILABLE = 8,
    RAW_PLUGIN_PACKAGE_LOAD_ERROR_PYTHON_READ_FAILED = 9,
    RAW_PLUGIN_PACKAGE_LOAD_ERROR_PYTHON_COMPILE_FAILED = 10,
    RAW_PLUGIN_PACKAGE_LOAD_ERROR_PYTHON_IMPORT_FAILED = 11,
    RAW_PLUGIN_PACKAGE_LOAD_ERROR_PYTHON_REGISTRATION_FAILED = 12,
    RAW_PLUGIN_PACKAGE_LOAD_ERROR_CONFIGURED_PLUGIN_MISSING = 13
} raw_plugin_package_load_error_code;

SLIC3R_HOST_API void orchestrator_begin_plugin_package_load(
    orchestrator_handle *orch,
    const char *package_root
);
SLIC3R_HOST_API void orchestrator_report_plugin_package_load_error(
    orchestrator_handle *orch,
    const char *package_root,
    raw_plugin_package_load_error_code code,
    const char *detail
);
SLIC3R_HOST_API void orchestrator_finish_plugin_package_load(
    orchestrator_handle *orch,
    const char *package_root
);

/*
Register one gettext catalog domain provided by the plugin package currently
being loaded. locale_directory is relative to that package root and contains
one subdirectory per language, for example:

    locale/fr/com.example.plugin.mo

The host loads the catalog after it has selected the application language.
Call this from register_plugin(), before the loader returns control to the
host. A plugin may register several distinct domains. Returns 1 when added,
0 for an identical duplicate, and a negative value for invalid input.
*/
SLIC3R_HOST_API int32_t orchestrator_register_translation_catalog(
    orchestrator_handle *orch,
    const char *domain,
    const char *locale_directory
);

SLIC3R_HOST_API bridge_detector_instance orchestrator_create_bridge_detector(
    orchestrator_handle *orch,
    const bridge_detector_create_input *input
);

/*
Register a generic property payload and receive its runtime numeric id.

The namespaced_name is the stable identity of the payload layout, for example
"com.example.plugin.surface_priority". The returned id is the compact key
stored by PluginPropertyContainer and ExtrusionPropertyContainer. Custom ids
belong to one orchestrator; register the same name again for each slicing
orchestrator instead of serializing the numeric value.

If the name was already registered with the same size and alignment, the
existing id is returned. If the same name is registered with a different layout,
SLIC3R_PROPERTY_TYPE_INVALID is returned.
*/
SLIC3R_HOST_API slic3r_property_type orchestrator_register_property(
    orchestrator_handle *orch,
    const char *namespaced_name,
    uint32_t byte_count,
    uint32_t alignment
);

/* Return metadata for a built-in or registered property type. */
SLIC3R_HOST_API uint32_t orchestrator_property_byte_count(
    const orchestrator_handle *orch,
    slic3r_property_type type
);
SLIC3R_HOST_API uint32_t orchestrator_property_alignment(
    const orchestrator_handle *orch,
    slic3r_property_type type
);
SLIC3R_HOST_API const char *orchestrator_property_name(
    const orchestrator_handle *orch,
    slic3r_property_type type
);

/*
Register a simple seam-like FacetsAnnotation kind.

This is intentionally a small declaration API, not a custom GUI API. The host
creates the toolbar button, brush controls, left/right mouse behavior and model
storage. The plugin only provides stable identity and labels:

- key is the persistent identity, for example "com.example.plugin:paint_name".
  It is saved with the model; do not use a translated label as a key.
- label is the painter window/tool name.
- enforce_label is the left-click action label.
- block_label is the right-click action label.
- icon_svg is a complete SVG document stored as UTF-8 text. The host registers
  it in the GUI bitmap cache under key, then uses it to build the toolbar icon.

Call this during plugin registration/initialization. The GUI builds its toolbar
from the orchestrator registry, so annotation tools registered after the toolbar
exists may not be visible until the GUI is recreated.
*/
typedef struct raw_generic_facets_annotation_def {
    const char *key;
    const char *label;
    const char *enforce_label;
    const char *block_label;
    /*
    NULL or empty selects the registering plugin's default gettext domain.
    A non-empty domain must be "Slic3r" or registered by the current package.
    */
    const char *translation_domain;
    const char *icon_svg;
} raw_generic_facets_annotation_def;

/*
Returns 1 when the definition is accepted, or a negative value on invalid
arguments or internal failure.
*/
SLIC3R_HOST_API int32_t orchestrator_register_generic_facets_annotation(
    orchestrator_handle *orch,
    const raw_generic_facets_annotation_def *def
);

/*
Register a UI layout fragment for one target layout file.

target_file is the base UI file the fragment applies to, for example
"print.ui", "filament.ui", or "printer_fff.ui".

fragment_id identifies this contribution inside target_file. If the same
target_file + fragment_id pair is registered twice, the second registration is
ignored. Identical duplicate content is expected when alternative plugins in
the same exclusive group expose the same settings. If the duplicate content is
different, the host keeps the first fragment and logs a warning because those
plugins no longer agree on the UI hidden behind that shared id.

ui_fragment is a small .ui document using the normal UI layout syntax. During
GUI construction, the host parses the base UI file and this fragment, then
merges pages, groups and lines by name. Missing pages/groups/lines are created.
Use insert$before$NAME or insert$after$NAME on page/group/line commands to
place a missing node next to an existing node of the same kind. A kind may be
specified explicitly, for example insert$aftergroup$Filtering.

priority orders fragments for the same target_file. Lower priority is applied
first. Fragments with the same priority keep registration order.

Returns 1 when the fragment was added, 0 when it was already present, and a
negative value on invalid arguments or internal failure.
*/
SLIC3R_HOST_API int32_t orchestrator_add_ui_fragment(
    orchestrator_handle *orch,
    const char *target_file,
    const char *fragment_id,
    const char *ui_fragment,
    int32_t priority
);

typedef enum raw_gui_rule_condition {
    RAW_GUI_RULE_CONDITION_NONE = 0,
    RAW_GUI_RULE_CONDITION_BOOL_TRUE,
    RAW_GUI_RULE_CONDITION_BOOL_FALSE,
    RAW_GUI_RULE_CONDITION_OPTION_ENABLED,
    RAW_GUI_RULE_CONDITION_OPTION_DISABLED,
    RAW_GUI_RULE_CONDITION_VALUE_NON_ZERO,
    RAW_GUI_RULE_CONDITION_INT_EQUALS,
    RAW_GUI_RULE_CONDITION_INT_NOT_EQUALS
} raw_gui_rule_condition;

typedef enum raw_gui_rule_action {
    RAW_GUI_RULE_ACTION_NONE = 0,
    RAW_GUI_RULE_ACTION_ENABLE,
    RAW_GUI_RULE_ACTION_ENABLE_ANY
} raw_gui_rule_action;

/*
Index values shared by target_index and condition_index.

RAW_GUI_RULE_INDEX_ALL means "no specific item":
- for target_index, apply the rule to the whole GUI field;
- for condition_index, automatically choose the safest condition read:
  use the current extruder item when such an item exists, otherwise enable if
  any vector item satisfies the condition.

RAW_GUI_RULE_INDEX_CURRENT is mainly useful for target_index. It means "apply
this rule to the vector item currently processed by the GUI refresh loop".
Use it only for options whose value is sized like the extruder count.
*/
#define RAW_GUI_RULE_INDEX_ALL     (-1)
#define RAW_GUI_RULE_INDEX_CURRENT (-2)

typedef struct raw_gui_rule {
    raw_gui_rule_action action;
    raw_gui_rule_condition condition;
    const char *target_key;
    const char *condition_key;
    int32_t target_index;
    int32_t condition_index;
    int32_t condition_int_value;
} raw_gui_rule;

static inline raw_gui_rule raw_gui_rule_init()
{
    raw_gui_rule rule = {0};
    rule.target_index = RAW_GUI_RULE_INDEX_ALL;
    rule.condition_index = RAW_GUI_RULE_INDEX_ALL;
    return rule;
}

/*
Register a GUI state rule.

Rules are evaluated by ConfigManipulation when a tab refreshes its enabled
state.

RAW_GUI_RULE_ACTION_ENABLE enables target_key when every ENABLE rule registered
for the same target is true.

RAW_GUI_RULE_ACTION_ENABLE_ANY enables target_key when at least one ENABLE_ANY
rule registered for the same target is true. It is useful for legacy OR
conditions such as "support is enabled when support_material is true or
raft_layers is non-zero".

If both actions are used for the same target, the target is enabled only when
all ENABLE rules are true and at least one ENABLE_ANY rule is true.

condition_index and target_index are used for vector/extruder options.
Use RAW_GUI_RULE_INDEX_ALL for scalar options or when the whole field should be
affected. Use RAW_GUI_RULE_INDEX_CURRENT for a target that must be applied item
by item in the current extruder loop.

condition_int_value is used by RAW_GUI_RULE_CONDITION_INT_EQUALS and
RAW_GUI_RULE_CONDITION_INT_NOT_EQUALS. This is the intended way to express enum
conditions, because enum config options are read as integer values.

Returns 1 when the rule was added, 0 when an identical rule was already
registered, and a negative value on invalid arguments or internal failure.
*/
SLIC3R_HOST_API int32_t orchestrator_add_gui_rule(
    orchestrator_handle *orch,
    const raw_gui_rule *rule
);

/*
Return the config keys used by the plugin or plugins currently active for a
step.

This is mainly for plugins that need to mirror another plugin's compatibility
rules. For example, a surface-generation plugin may need to split regions by
the settings used by the selected STEP_PERIMETER plugin when concentric infill
delegates line generation to the perimeter generator.

For exclusive steps, config is the project/full config that contains the
exclusive-step selector option, such as "step_perimeter_plugin"; the function
returns the selected plugin's keys. For non-exclusive steps, no selection exists,
so the function returns the de-duplicated union of every active plugin's keys.
If a selector option does not exist because only one plugin is active, the host
falls back to that active plugin.

The function uses the usual double-call convention:
- call with keys == NULL to get the number of entries;
- call again with an array of that size to receive borrowed key pointers.
*/
SLIC3R_HOST_API int32_t orchestrator_selected_plugin_used_config_keys(
    orchestrator_handle *orch,
    const config_handle *config,
    slicing_step_t step,
    raw_used_config_key *keys
);

/*
Default host callbacks used to populate plugin_run_context.
Plugins normally call these through the function pointers stored in the run
context instead of calling them directly.

report_warning and report_error copy the message into the host diagnostic queue.
The GUI later drains that queue and displays the text as Plater notifications.
report_error also requests cancellation for the current plugin-driven slicing
run, so the plugin should reserve it for errors that make the result unsafe.
*/
SLIC3R_HOST_API int orchestrator_plugin_is_cancelled(plugin_host_context *host_context);
SLIC3R_HOST_API void orchestrator_plugin_report_warning(plugin_host_context *host_context, const char *message);
SLIC3R_HOST_API void orchestrator_plugin_report_error(plugin_host_context *host_context, const char *message);
SLIC3R_HOST_API void orchestrator_plugin_report_progress(plugin_host_context *host_context, double progress, const char *message);

#ifdef __cplusplus
}
#endif

#endif // slic3r_orchestrator_h_
