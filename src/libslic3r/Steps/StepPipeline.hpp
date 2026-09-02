///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef steps_steppipeline_hpp_
#define steps_steppipeline_hpp_

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_config_def.h"
#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/c/slic3r_plugin_types.h"

namespace Slic3r {

class ConfigBase;
class Orchestrator;
class Plugin;
class Print;
class PrintObject;

namespace Steps {

struct StepExclusiveGroup
{
    std::string group_id;
    std::string option_key_storage;
    std::string label_storage;
    std::string tooltip_storage;
    slicing_step_t step;
    raw_config_option_def option_def;
    std::string ui_fragment;
    std::vector<raw_gui_rule> gui_activation_rules;
    // The option enum stores plugin ids as serialized values. Its integer
    // value is the index in enum_values, which is also the index used by
    // generated RAW_GUI_RULE_CONDITION_INT_EQUALS rules.
    std::vector<std::string> enum_values;
    std::vector<std::string> enum_labels;
    std::vector<key_value_string_pair_t> enum_pairs;

    void refresh_storage_pointers();
    void set_enum_plugins(const std::vector<std::pair<std::string, std::string>> &plugin_ids_and_labels);
};

struct StepExclusivePluginGroup
{
    StepExclusiveGroup group;
    std::vector<Plugin *> plugins;
};

const std::map<slicing_step_t, StepExclusiveGroup> &get_exclusive_steps();
std::vector<StepExclusivePluginGroup> active_exclusive_plugin_groups(Orchestrator &orchestrator);
std::vector<Plugin *> selected_or_active_plugins_for_step(Orchestrator &orchestrator,
                                                          slicing_step_t step,
                                                          const ConfigBase *config);
Plugin *selected_or_active_plugin_for_step(Orchestrator &orchestrator,
                                           slicing_step_t step,
                                           const ConfigBase *config);

// Select one active provider from a specific plugin-defined exclusive group.
// This is the service-plugin counterpart of selecting a complete built-in
// step: unrelated groups registered on the same numeric step are ignored.
Plugin *selected_active_plugin_from_group(Orchestrator &orchestrator,
                                          slicing_step_t step,
                                          const std::string &exclusive_group,
                                          const ConfigBase *config);

// Written execution order for the migrated pipeline. The order is intentionally
// not inferred from the dependency graph: a missing step in this list is a
// pipeline definition error, not something the graph should silently repair.
const std::vector<slicing_step_t> &execution_order();

// Direct dependency graph used by Print's new invalidation plan. If A maps to
// B, invalidating A also asks B to run on the next process().
const std::map<slicing_step_t, std::vector<slicing_step_t>> &step_dependents();

// Stable transitive closure of step_dependents(). The returned vector follows
// execution_order(), so callers may insert it into sets or display it without
// depending on std::map iteration details.
std::vector<slicing_step_t> dependent_steps_closure(slicing_step_t step);

#ifdef _DEBUG
// Debug guard for the central graph definition. Each edge A -> B must respect
// execution_order(); otherwise the pipeline would need to run B before A has
// produced its new data.
bool validate_execution_order_against_dependencies();
#endif

// Central entry point for the step-based slicing pipeline.
//
// Orchestrator owns plugin registration and host callbacks; StepPipeline owns
// the ordered execution of slicing steps. Keeping the ordering here avoids
// growing Orchestrator with step-specific code.
class StepPipeline
{
public:
    // Prepare the sliced print tree up to the point where G-code ordering can
    // start. Export callers run this first through Print::process(), then call
    // run_gcode() later with the concrete output path.
    static void run_slice(Orchestrator &orchestrator, Print &print);

    // Finish the export-side pipeline. This starts at STEP_ORDERING because the
    // G-code writer consumes the ordered PrintingPlan, then runs every
    // post-ordering step and finally STEP_GCODE with the destination path.
    static void run_gcode(Orchestrator &orchestrator, Print &print, const std::string &path);

    // Compatibility wrapper for code that still wants the whole migrated
    // pipeline in one call. Normal GUI/CLI flow uses run_slice() and run_gcode()
    // separately so the final output path is available only during export.
    static void run(Orchestrator &orchestrator, Print &print, const std::string &path = std::string());

#ifdef _DEBUG
    // Run the native/original process and the step/plugin process on two cloned
    // Print trees, comparing them after each migrated step group. This method is
    // for migration tests and intentionally does not mutate the input print.
    static void debug_run(Orchestrator &orchestrator, const Print &source, slicing_step_t until);

private:
    // These native split points intentionally access PrintObject internals.
    // PrintObject/Layer grant friendship to StepPipeline, but not to anonymous
    // namespace helper functions, so keep them as real class members.
    static void run_native_layer_height_generation(Print &print);
    static void run_native_slicing(Print &print);
    static void run_native_post_slicing(Print &print);
    static void run_native_layer_height_generation_object(PrintObject &object);
    static void run_native_slicing_object(PrintObject &object);
    static void run_native_post_slicing_object(PrintObject &object);
#endif
};

} // namespace Steps
} // namespace Slic3r

#endif // steps_steppipeline_hpp_
