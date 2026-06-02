///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "StepPipeline.hpp"

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/internal/LayerAccess.hpp"
#include "libslic3r/Api/internal/LayerRegionAccess.hpp"
#include "libslic3r/Api/internal/PrintObjectAccess.hpp"
#include "libslic3r/ConfigDef.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintObject.hpp"
#include "libslic3r/Slicing.hpp"
#include "libslic3r/Steps/StepDetectSupportSpots.hpp"
#include "libslic3r/Steps/StepExtrusionEdition.hpp"
#include "libslic3r/Steps/StepExtrusionOrdering.hpp"
#include "libslic3r/Steps/StepExtrusionSimplification.hpp"
#include "libslic3r/Steps/StepGenerateInfill.hpp"
#include "libslic3r/Steps/StepGeneratePerimeter.hpp"
#include "libslic3r/Steps/StepGenerateSupport.hpp"
#include "libslic3r/Steps/StepGenerateWipeTower.hpp"
#include "libslic3r/Steps/StepGroupInfillRegions.hpp"
#include "libslic3r/Steps/StepLayerExtrusionEdition.hpp"
#include "libslic3r/Steps/StepLayerHeightGeneration.hpp"
#include "libslic3r/Steps/StepLayerStiching.hpp"
#include "libslic3r/Steps/StepPostInfillGeneration.hpp"
#include "libslic3r/Steps/StepPostPerimeterGeneration.hpp"
#include "libslic3r/Steps/StepPostSlicing.hpp"
#include "libslic3r/Steps/StepPrepareForPeriemters.hpp"
#include "libslic3r/Steps/StepPrepareGcode.hpp"
#include "libslic3r/Steps/StepPrepareInfill.hpp"
#include "libslic3r/Steps/StepSlicing.hpp"
#include "libslic3r/Steps/StepRunner.hpp"
#include "libslic3r/Steps/StepSkirtBrim.hpp"
#include "libslic3r/Steps/StepSupportDemand.hpp"
#include "libslic3r/Steps/StepSurfaceGeneration.hpp"
#include "libslic3r/SurfaceCollection.hpp"
#include "libslic3r/Thread.hpp"

#ifdef _DEBUG
#include "libslic3r/Steps/DebugPrintProcessComparator.hpp"

#include <iostream>
#include <stdexcept>
#endif

#include <algorithm>
#include <cassert>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace Slic3r {
LayerUPtrs new_layers(PrintObject *print_object, const std::vector<double> &object_layers);
}

namespace Slic3r::Steps {

const std::vector<slicing_step_t> &execution_order()
{
    static const std::vector<slicing_step_t> steps {
        STEP_LAYER_HEIGHT,
        STEP_SLICING,
        STEP_POST_SLICING,
        STEP_PRE_PERIMETER,
        STEP_PERIMETER,
        STEP_POST_PERIMETER,
        STEP_SURFACE_GENERATION,
        STEP_PRE_INFILL,
        STEP_INFILL_GROUP,
        STEP_INFILL,
        STEP_POST_INFILL,
        STEP_SKIRT_BRIM,
        STEP_SUPPORT_DEMAND,
        STEP_SUPPORT,
        STEP_PRE_GCODE,
        STEP_ORDERING,
        STEP_WIPETOWER,
        STEP_SUPPORT_SPOT,
        STEP_LAYER_EXTRUSION_EDIT,
        STEP_LAYER_STICHING,
        STEP_EXTRUSION_EDIT,
        STEP_EXTRUSION_SIMPLIFICATION,
        STEP_GCODE,
    };
    return steps;
}

const std::map<slicing_step_t, std::vector<slicing_step_t>> &step_dependents()
{
    static const std::map<slicing_step_t, std::vector<slicing_step_t>> dependents {
        {STEP_LAYER_HEIGHT, {STEP_SLICING}},
        {STEP_SLICING, {STEP_POST_SLICING}},
        {STEP_POST_SLICING, {STEP_PRE_PERIMETER, STEP_SKIRT_BRIM}},
        {STEP_PRE_PERIMETER, {STEP_PERIMETER}},
        {STEP_PERIMETER, {STEP_POST_PERIMETER}},
        {STEP_POST_PERIMETER, {STEP_SURFACE_GENERATION}},
        {STEP_SURFACE_GENERATION, {STEP_PRE_INFILL}},
        {STEP_PRE_INFILL, {STEP_INFILL_GROUP}},
        {STEP_INFILL_GROUP, {STEP_INFILL}},
        {STEP_INFILL, {STEP_POST_INFILL}},
        {STEP_POST_INFILL, {STEP_PRE_GCODE}},
        {STEP_SKIRT_BRIM, {STEP_SUPPORT_DEMAND, STEP_PRE_GCODE}},
        {STEP_SUPPORT_DEMAND, {STEP_SUPPORT}},
        {STEP_SUPPORT, {STEP_PRE_GCODE}},
        {STEP_PRE_GCODE, {STEP_ORDERING}},
        {STEP_ORDERING, {STEP_WIPETOWER}},
        {STEP_WIPETOWER, {STEP_SUPPORT_SPOT}},
        {STEP_SUPPORT_SPOT, {STEP_LAYER_EXTRUSION_EDIT}},
        {STEP_LAYER_EXTRUSION_EDIT, {STEP_LAYER_STICHING}},
        {STEP_LAYER_STICHING, {STEP_EXTRUSION_EDIT}},
        {STEP_EXTRUSION_EDIT, {STEP_EXTRUSION_SIMPLIFICATION}},
        {STEP_EXTRUSION_SIMPLIFICATION, {STEP_GCODE}},
    };
    return dependents;
}

std::vector<slicing_step_t> dependent_steps_closure(slicing_step_t step)
{
    const std::map<slicing_step_t, std::vector<slicing_step_t>> &dependents = step_dependents();
    std::set<slicing_step_t> visited;
    std::vector<slicing_step_t> stack;

    if (auto it = dependents.find(step); it != dependents.end())
        stack.insert(stack.end(), it->second.begin(), it->second.end());

    while (!stack.empty()) {
        const slicing_step_t current = stack.back();
        stack.pop_back();
        if (!visited.insert(current).second)
            continue;

        if (auto it = dependents.find(current); it != dependents.end())
            stack.insert(stack.end(), it->second.begin(), it->second.end());
    }

    std::vector<slicing_step_t> out;
    out.reserve(visited.size());
    for (slicing_step_t ordered_step : execution_order())
        if (visited.find(ordered_step) != visited.end())
            out.push_back(ordered_step);
    return out;
}

#ifdef _DEBUG
bool validate_execution_order_against_dependencies()
{
    std::map<slicing_step_t, size_t> order_index;
    const std::vector<slicing_step_t> &order = execution_order();
    for (size_t idx = 0; idx < order.size(); ++idx)
        order_index.emplace(order[idx], idx);

    for (const auto &[producer, consumers] : step_dependents()) {
        const auto producer_it = order_index.find(producer);
        assert(producer_it != order_index.end());
        if (producer_it == order_index.end())
            return false;

        for (slicing_step_t consumer : consumers) {
            const auto consumer_it = order_index.find(consumer);
            assert(consumer_it != order_index.end());
            if (consumer_it == order_index.end())
                return false;
            assert(producer_it->second < consumer_it->second);
            if (producer_it->second >= consumer_it->second)
                return false;
        }
    }
    return true;
}
#endif

namespace {

std::string exclusive_group_ui_fragment(const std::string &key,
                                        const std::string &line_label)
{
    // This is the neutral fallback for exclusive groups that do not provide
    // their own placement fragment. Feature plugins may register a fragment
    // with the same group id to put the selector next to their settings.
    return std::string("page:Notes\n") +
           "group:Exclusive step plugins\n" +
           "line:" + line_label + "\n" +
           "setting:" + key + "\n" +
           "end_line\n";
}

std::string sanitized_config_key_part(const std::string &text)
{
    std::string out;
    out.reserve(text.size());
    bool previous_was_separator = false;

    for (const char raw_ch : text) {
        const unsigned char ch = static_cast<unsigned char>(raw_ch);
        if (std::isalnum(ch) != 0) {
            out.push_back(static_cast<char>(std::tolower(ch)));
            previous_was_separator = false;
        } else if (!previous_was_separator && !out.empty()) {
            out.push_back('_');
            previous_was_separator = true;
        }
    }

    while (!out.empty() && out.back() == '_')
        out.pop_back();
    return out.empty() ? "plugin_group" : out;
}

std::string option_key_for_plugin_group(slicing_step_t step, const std::string &group_id)
{
    return "exclusive_group_" + std::to_string(int(step)) + "_" +
           sanitized_config_key_part(group_id) + "_plugin";
}

raw_option_category option_category_for_step(slicing_step_t step)
{
    switch (step) {
    case STEP_PERIMETER:
    case STEP_POST_PERIMETER:
    case STEP_PRE_PERIMETER:
        return RAW_OPTION_CATEGORY_PERIMETER;
    case STEP_INFILL:
    case STEP_INFILL_GROUP:
    case STEP_PRE_INFILL:
    case STEP_POST_INFILL:
        return RAW_OPTION_CATEGORY_INFILL;
    case STEP_SUPPORT:
    case STEP_SUPPORT_DEMAND:
    case STEP_SUPPORT_SPOT:
        return RAW_OPTION_CATEGORY_SUPPORT;
    case STEP_GCODE:
    case STEP_PRE_GCODE:
    case STEP_ORDERING:
    case STEP_WIPETOWER:
    case STEP_LAYER_STICHING:
        return RAW_OPTION_CATEGORY_OUTPUT;
    default:
        return RAW_OPTION_CATEGORY_SLICING;
    }
}

StepExclusiveGroup make_exclusive_group(slicing_step_t step,
                                        const std::string &group_id,
                                        const std::string &key,
                                        const std::string &label,
                                        raw_option_category category,
                                        const std::string &line_label,
                                        const std::string &tooltip)
{
    StepExclusiveGroup group = {};
    group.group_id = group_id;
    group.option_key_storage = key;
    group.label_storage = label;
    group.tooltip_storage = tooltip;
    group.step = step;

    raw_config_option_def def = raw_config_option_def_init();
    def.type = RAW_CO_ENUM;
    def.gui_type = RAW_GUI_TYPE_SELECT_CLOSE;
    def.container_type = RAW_CONTAINER_TYPE_PROJECT;
    def.option_preset_type = RAW_PRESET_TYPE_FFF_PRINT;
    def.printer_technology = RAW_PT_FFF;
    def.category = category;
    def.invalidates_step = step;
    def.mode = RAW_CONFIG_OPTION_MODE_ADV_EXP | RAW_CONFIG_OPTION_MODE_SUSI;
    group.option_def = def;

    group.ui_fragment = exclusive_group_ui_fragment(group.option_key_storage, line_label);
    group.refresh_storage_pointers();
    return group;
}

StepExclusiveGroup make_exclusive_step_group(slicing_step_t step,
                                             const char *key,
                                             const char *label,
                                             raw_option_category category,
                                             const char *line_label)
{
    return make_exclusive_group(step,
                                key,
                                key,
                                label,
                                category,
                                line_label,
                                "Choose which active plugin owns this exclusive slicing step.");
}

StepExclusiveGroup make_plugin_exclusive_group(slicing_step_t step,
                                               const std::string &group_id,
                                               const Plugin &first_plugin)
{
    const std::string key = option_key_for_plugin_group(step, group_id);
    const std::string label = first_plugin.get_exclusive_group_label().empty() ?
        group_id :
        first_plugin.get_exclusive_group_label();
    const std::string tooltip = first_plugin.get_exclusive_group_tooltip().empty() ?
        "Choose which active plugin owns this exclusive plugin group." :
        first_plugin.get_exclusive_group_tooltip();

    return make_exclusive_group(step,
                                group_id,
                                key,
                                label,
                                option_category_for_step(step),
                                label,
                                tooltip);
}

void apply_plugin_group_text(StepExclusiveGroup &group, const std::vector<Plugin *> &plugins)
{
    // The selector belongs to the group, not to one plugin. When plugin authors
    // provide group text, the first active plugin in execution order owns the
    // wording so every project sees one stable label for that group.
    for (const Plugin *plugin : plugins) {
        if (plugin == nullptr)
            continue;
        if (plugin->get_exclusive_group_label().empty() && plugin->get_exclusive_group_tooltip().empty())
            continue;

        if (!plugin->get_exclusive_group_label().empty()) {
            group.label_storage = plugin->get_exclusive_group_label();
            group.ui_fragment = exclusive_group_ui_fragment(group.option_key_storage, group.label_storage);
        }
        if (!plugin->get_exclusive_group_tooltip().empty())
            group.tooltip_storage = plugin->get_exclusive_group_tooltip();
        group.refresh_storage_pointers();
        return;
    }
}

inline std::map<slicing_step_t, int> slicingstep_2_percent = {
    {STEP_LAYER_HEIGHT, 0},
    {STEP_SLICING, 5},
    {STEP_POST_SLICING, 10},
    {STEP_SURFACE_GENERATION, 15},
    {STEP_PRE_PERIMETER, 20},
    {STEP_PERIMETER, 25},
    {STEP_POST_PERIMETER, 30},
    {STEP_PRE_INFILL, 40},
    {STEP_INFILL_GROUP, 45},
    {STEP_INFILL, 50},
    {STEP_POST_INFILL, 55},
    {STEP_SKIRT_BRIM, 58},
    {STEP_SUPPORT_DEMAND, 60},
    {STEP_SUPPORT, 65},
    {STEP_PRE_GCODE, 70},
    {STEP_ORDERING, 75},
    {STEP_WIPETOWER, 80},
    {STEP_SUPPORT_SPOT, 81},
    {STEP_LAYER_EXTRUSION_EDIT, 82},
    {STEP_LAYER_STICHING, 85},
    {STEP_EXTRUSION_EDIT, 90},
    {STEP_EXTRUSION_SIMPLIFICATION, 95},
    {STEP_GCODE, 100},
};

} // namespace

void StepExclusiveGroup::refresh_storage_pointers()
{
    if (!option_key_storage.empty())
        option_def.opt_key = option_key_storage.c_str();
    if (!label_storage.empty()) {
        option_def.label = label_storage.c_str();
        option_def.full_label = label_storage.c_str();
    }
    if (!tooltip_storage.empty())
        option_def.tooltip = tooltip_storage.c_str();

    enum_pairs.clear();
    enum_pairs.reserve(enum_values.size());
    for (size_t i = 0; i < enum_values.size(); ++i) {
        key_value_string_pair_t pair = {};
        pair.value = enum_values[i].c_str();
        pair.label = i < enum_labels.size() ? enum_labels[i].c_str() : enum_values[i].c_str();
        enum_pairs.push_back(pair);
    }

    option_def.enum_def.value_label_pairs.items = enum_pairs.empty() ? nullptr : enum_pairs.data();
    option_def.enum_def.value_label_pairs.count = uint32_t(enum_pairs.size());
    option_def.default_serialized_value = enum_values.empty() ? "" : enum_values.front().c_str();
}

void StepExclusiveGroup::set_enum_plugins(const std::vector<std::pair<std::string, std::string>> &plugin_ids_and_labels)
{
    enum_values.clear();
    enum_labels.clear();
    enum_values.reserve(plugin_ids_and_labels.size());
    enum_labels.reserve(plugin_ids_and_labels.size());
    for (const std::pair<std::string, std::string> &plugin : plugin_ids_and_labels) {
        enum_values.emplace_back(plugin.first);
        enum_labels.emplace_back(plugin.second.empty() ? plugin.first : plugin.second);
    }
    refresh_storage_pointers();
}

const std::map<slicing_step_t, StepExclusiveGroup> &get_exclusive_steps()
{
    static const std::map<slicing_step_t, StepExclusiveGroup> s_groups = {
        {STEP_LAYER_HEIGHT,       make_exclusive_step_group(STEP_LAYER_HEIGHT,       "step_layer_height_plugin",       "Layer height plugin",       RAW_OPTION_CATEGORY_SLICING,   "Layer height step plugin")},
        {STEP_SLICING,            make_exclusive_step_group(STEP_SLICING,            "step_slicing_plugin",            "Slicing plugin",            RAW_OPTION_CATEGORY_SLICING,   "Slicing step plugin")},
        {STEP_PERIMETER,          make_exclusive_step_group(STEP_PERIMETER,          "step_perimeter_plugin",          "Perimeter plugin",          RAW_OPTION_CATEGORY_PERIMETER, "Perimeter step plugin")},
        {STEP_SKIRT_BRIM,         make_exclusive_step_group(STEP_SKIRT_BRIM,         "step_skirt_brim_plugin",         "Skirt and brim plugin",     RAW_OPTION_CATEGORY_OUTPUT,    "Skirt and brim step plugin")},
        {STEP_INFILL_GROUP,       make_exclusive_step_group(STEP_INFILL_GROUP,       "step_infill_group_plugin",       "Infill grouping plugin",    RAW_OPTION_CATEGORY_INFILL,    "Infill grouping step plugin")},
        {STEP_INFILL,             make_exclusive_step_group(STEP_INFILL,             "step_infill_plugin",             "Infill plugin",             RAW_OPTION_CATEGORY_INFILL,    "Infill step plugin")},
        {STEP_SUPPORT,            make_exclusive_step_group(STEP_SUPPORT,            "step_support_plugin",            "Support plugin",            RAW_OPTION_CATEGORY_SUPPORT,   "Support step plugin")},
        {STEP_ORDERING,           make_exclusive_step_group(STEP_ORDERING,           "step_ordering_plugin",           "Ordering plugin",           RAW_OPTION_CATEGORY_OUTPUT,    "Ordering step plugin")},
        {STEP_WIPETOWER,          make_exclusive_step_group(STEP_WIPETOWER,          "step_wipetower_plugin",          "Wipe tower plugin",         RAW_OPTION_CATEGORY_OUTPUT,    "Wipe tower step plugin")},
        {STEP_LAYER_STICHING,     make_exclusive_step_group(STEP_LAYER_STICHING,     "step_layer_stiching_plugin",     "Layer stitching plugin",    RAW_OPTION_CATEGORY_OUTPUT,    "Layer stitching step plugin")},
        {STEP_GCODE,              make_exclusive_step_group(STEP_GCODE,              "step_gcode_plugin",              "G-code plugin",             RAW_OPTION_CATEGORY_OUTPUT,    "G-code step plugin")}
    };
    return s_groups;
}

std::vector<StepExclusivePluginGroup> active_exclusive_plugin_groups(Orchestrator &orchestrator)
{
    std::vector<StepExclusivePluginGroup> out;
    const std::map<slicing_step_t, StepExclusiveGroup> &exclusive_steps = get_exclusive_steps();

    // Host-defined unique steps always have exactly one exclusive group. This
    // keeps old "replace this whole step" plugins and plugin-declared groups in
    // the same selection model.
    for (const std::pair<const slicing_step_t, StepExclusiveGroup> &entry : exclusive_steps) {
        std::vector<Plugin *> active_plugins = orchestrator.get_active_plugins_for_step(entry.first);
        if (active_plugins.size() <= 1)
            continue;

        StepExclusivePluginGroup plugin_group;
        plugin_group.group = entry.second;
        plugin_group.group.refresh_storage_pointers();
        apply_plugin_group_text(plugin_group.group, active_plugins);
        plugin_group.plugins = std::move(active_plugins);
        out.push_back(std::move(plugin_group));
    }

    std::map<slicing_step_t, std::vector<Plugin *>> active_plugins_by_step;
    for (Plugin *plugin : orchestrator.registered_plugins()) {
        if (plugin != nullptr && orchestrator.is_plugin_active(plugin))
            active_plugins_by_step[plugin->get_step()].push_back(plugin);
    }

    for (std::pair<const slicing_step_t, std::vector<Plugin *>> &step_plugins : active_plugins_by_step) {
        if (exclusive_steps.find(step_plugins.first) != exclusive_steps.end())
            continue;

        std::stable_sort(step_plugins.second.begin(), step_plugins.second.end(), [](const Plugin *lhs, const Plugin *rhs) {
            return lhs->get_priority() < rhs->get_priority();
        });

        std::map<std::string, std::vector<Plugin *>> plugins_by_group_id;
        for (Plugin *plugin : step_plugins.second)
            if (plugin != nullptr && !plugin->get_exclusive_group().empty())
                plugins_by_group_id[plugin->get_exclusive_group()].push_back(plugin);

        for (const std::pair<const std::string, std::vector<Plugin *>> &entry : plugins_by_group_id) {
            if (entry.second.size() <= 1)
                continue;

            StepExclusivePluginGroup plugin_group;
            plugin_group.group = make_plugin_exclusive_group(step_plugins.first, entry.first, *entry.second.front());
            apply_plugin_group_text(plugin_group.group, entry.second);
            plugin_group.plugins = entry.second;
            out.push_back(std::move(plugin_group));
        }
    }

    return out;
}

Plugin *selected_plugin_from_group(const StepExclusiveGroup &group,
                                   const std::vector<Plugin *> &plugins,
                                   const ConfigBase *config)
{
    if (plugins.empty())
        return nullptr;
    if (plugins.size() == 1 || config == nullptr)
        return plugins.front();

    const ConfigOption *option = config->option(group.option_def.opt_key);
    if (option == nullptr)
        return plugins.front();

    const int32_t selected_idx = option->get_int();
    if (selected_idx < 0 || size_t(selected_idx) >= plugins.size())
        return plugins.front();

    return plugins[size_t(selected_idx)];
}

std::vector<Plugin *> selected_or_active_plugins_for_step(Orchestrator &orchestrator,
                                                          slicing_step_t step,
                                                          const ConfigBase *config)
{
    std::vector<Plugin *> active_plugins = orchestrator.get_active_plugins_for_step(step);
    if (active_plugins.size() <= 1)
        return active_plugins;

    const std::map<slicing_step_t, StepExclusiveGroup> &exclusive_steps = get_exclusive_steps();
    const std::map<slicing_step_t, StepExclusiveGroup>::const_iterator exclusive_it = exclusive_steps.find(step);
    if (exclusive_it != exclusive_steps.end()) {
        StepExclusiveGroup group = exclusive_it->second;
        group.refresh_storage_pointers();
        Plugin *selected = selected_plugin_from_group(group, active_plugins, config);
        return selected != nullptr ? std::vector<Plugin *>{ selected } : std::vector<Plugin *>{};
    }

    std::map<std::string, std::vector<Plugin *>> plugins_by_group_id;
    for (Plugin *plugin : active_plugins)
        if (plugin != nullptr && !plugin->get_exclusive_group().empty())
            plugins_by_group_id[plugin->get_exclusive_group()].push_back(plugin);

    std::map<std::string, bool> group_already_emitted;
    std::vector<Plugin *> selected_plugins;
    selected_plugins.reserve(active_plugins.size());
    for (Plugin *plugin : active_plugins) {
        if (plugin == nullptr)
            continue;

        const std::string &group_id = plugin->get_exclusive_group();
        if (group_already_emitted[group_id])
            continue;
        group_already_emitted[group_id] = true;

        const std::vector<Plugin *> &group_plugins = plugins_by_group_id[group_id];
        StepExclusiveGroup group = make_plugin_exclusive_group(step, group_id, *group_plugins.front());
        group.refresh_storage_pointers();
        Plugin *selected = selected_plugin_from_group(group, group_plugins, config);
        if (selected != nullptr)
            selected_plugins.push_back(selected);
    }

    return selected_plugins;
}

Plugin *selected_or_active_plugin_for_step(Orchestrator &orchestrator,
                                           slicing_step_t step,
                                           const ConfigBase *config)
{
    const std::vector<Plugin *> plugins = selected_or_active_plugins_for_step(orchestrator, step, config);
    assert(plugins.size() <= 1);
    return plugins.empty() ? nullptr : plugins.front();
}

namespace {

void begin_step(Print &print, slicing_step_t step, const std::string &message, const std::string &path)
{
    // Status arguments are consumed through boost::format by the GUI. Passing
    // even one unused argument to a message without a placeholder throws, so the
    // optional path is forwarded only when there is real text to display.
    if (path.empty())
        print.set_status(slicingstep_2_percent[step], message);
    else
        print.set_status(slicingstep_2_percent[step], message, {path});
    print.secondary_status_counter_reset();
}

bool stop_after(slicing_step_t current, slicing_step_t until)
{
    return current == until;
}

template<class RunFn>
void run_step_if_requested(Print &print, slicing_step_t step, RunFn &&run_fn)
{
    if (!print.should_execute_step(step))
        return;

    run_fn();
    print.mark_step_executed(step);
}

void mark_legacy_step_done(Print &print, slicing_step_t step)
{
    // The new plugin pipeline writes the same data that the legacy GUI expects,
    // but the GUI still asks the historical Print/PrintObject state machine
    // whether slices, perimeters or infill are available. Mark only those
    // compatibility milestones that really correspond to finished data.
    Print::StatusMonitor status(print);
    if (status.set_started(step))
        status.set_done(step);
}

void run_layer_height_generation(Orchestrator &orchestrator, Print &print, const std::string &path)
{
    run_step_if_requested(print, STEP_LAYER_HEIGHT, [&] {
        begin_step(print, STEP_LAYER_HEIGHT, L("Creating the layer height"), path);
        StepLayerHeightGeneration::clean_and_prepare(print);
        StepLayerHeightGeneration::run_step(orchestrator, print);
    });
}

void run_slicing(Orchestrator &orchestrator, Print &print, const std::string &path)
{
    run_step_if_requested(print, STEP_SLICING, [&] {
        begin_step(print, STEP_SLICING, L("StepSlicing"), path);
        StepSlicing::run_step(orchestrator, print);
        mark_legacy_step_done(print, posSlice);
    });
}

void run_post_slicing(Orchestrator &orchestrator, Print &print, const std::string &path)
{
    run_step_if_requested(print, STEP_POST_SLICING, [&] {
        begin_step(print, STEP_POST_SLICING, L("Post-processing slices"), path);
        StepPostSlicing::clean_and_prepare(print);
        StepPostSlicing::run_step(orchestrator, print);
    });
}

void run_remaining_steps(Orchestrator &orchestrator, Print &print, const std::string &path, slicing_step_t until = STEP_GCODE)
{
    // The pipeline currently builds the complete print tree used by G-code export,
    // but the actual G-code generation still runs through the legacy
    // Print::export_gcode() entry point. Keeping that boundary explicit makes the
    // migration easier to reason about while GUI and CLI callers still export
    // G-code in the usual place.
    StepSupportDemand::State support_demand;

    run_step_if_requested(print, STEP_PRE_PERIMETER, [&] {
        begin_step(print, STEP_PRE_PERIMETER, L("Preparing perimeters"), path);
        StepPrepareForPeriemters::clean_and_prepare(print);
        StepPrepareForPeriemters::run_step(orchestrator, print);
    });
    if (stop_after(STEP_PRE_PERIMETER, until)) return;
    
    run_step_if_requested(print, STEP_PERIMETER, [&] {
        begin_step(print, STEP_PERIMETER, L("Generating perimeters"), path);
        StepGeneratePerimeter::clean_and_prepare(print);
        StepGeneratePerimeter::run_step(orchestrator, print);
        mark_legacy_step_done(print, posPerimeters);
    });
    if (stop_after(STEP_PERIMETER, until)) return;
    
    run_step_if_requested(print, STEP_POST_PERIMETER, [&] {
        begin_step(print, STEP_POST_PERIMETER, L("Post-processing perimeters"), path);
        StepPostPerimeterGeneration::clean_and_prepare(print);
        StepPostPerimeterGeneration::run_step(orchestrator, print);
    });
    if (stop_after(STEP_POST_PERIMETER, until)) return;

    run_step_if_requested(print, STEP_SURFACE_GENERATION, [&] {
        begin_step(print, STEP_SURFACE_GENERATION, L("Generating surfaces"), path);
        StepSurfaceGeneration::clean_and_prepare(print);
#ifdef _DEBUG
        Detail::validate_or_report(StepSurfaceGeneration::validate_pre, print, "Surface-generation pre-step validation");
#endif
        StepSurfaceGeneration::run_step(orchestrator, print);
#ifdef _DEBUG
        Detail::validate_or_report(StepSurfaceGeneration::validate_post, print, "Surface-generation post-step validation");
#endif
    });
    if (stop_after(STEP_SURFACE_GENERATION, until)) return;

    run_step_if_requested(print, STEP_PRE_INFILL, [&] {
        begin_step(print, STEP_PRE_INFILL, L("Preparing infill"), path);
        StepPrepareInfill::clean_and_prepare(print);
        StepPrepareInfill::run_step(orchestrator, print);
        mark_legacy_step_done(print, posPrepareInfill);
    });
    if (stop_after(STEP_PRE_INFILL, until)) return;

    run_step_if_requested(print, STEP_INFILL_GROUP, [&] {
        begin_step(print, STEP_INFILL_GROUP, L("Grouping infill regions"), path);
        StepGroupInfillRegions::clean_and_prepare(print);
        StepGroupInfillRegions::run_step(orchestrator, print);
    });
    if (stop_after(STEP_INFILL_GROUP, until)) return;

    run_step_if_requested(print, STEP_INFILL, [&] {
        begin_step(print, STEP_INFILL, L("Generating infill"), path);
        StepGenerateInfill::clean_and_prepare(print);
        StepGenerateInfill::run_step(orchestrator, print);
        mark_legacy_step_done(print, posInfill);
    });
    if (stop_after(STEP_INFILL, until)) return;

    run_step_if_requested(print, STEP_POST_INFILL, [&] {
        begin_step(print, STEP_POST_INFILL, L("Post-processing infill"), path);
        StepPostInfillGeneration::clean_and_prepare(print);
        StepPostInfillGeneration::run_step(orchestrator, print);
        mark_legacy_step_done(print, posIroning);
    });
    if (stop_after(STEP_POST_INFILL, until)) return;

    run_step_if_requested(print, STEP_SKIRT_BRIM, [&] {
        begin_step(print, STEP_SKIRT_BRIM, L("Generating skirt and brim"), path);
        StepSkirtBrim::clean_and_prepare(print);
        StepSkirtBrim::run_step(orchestrator, print);
    });
    if (stop_after(STEP_SKIRT_BRIM, until)) return;

    run_step_if_requested(print, STEP_SUPPORT_DEMAND, [&] {
        begin_step(print, STEP_SUPPORT_DEMAND, L("Detecting support demand"), path);
        StepSupportDemand::clean_and_prepare(print);
        StepSupportDemand::run_step(orchestrator, print, support_demand);
    });
    if (stop_after(STEP_SUPPORT_DEMAND, until)) return;

    run_step_if_requested(print, STEP_SUPPORT, [&] {
        begin_step(print, STEP_SUPPORT, L("Generating support material"), path);
        StepGenerateSupport::clean_and_prepare(print);
        StepGenerateSupport::run_step(orchestrator, print, support_demand);
        mark_legacy_step_done(print, posSupportMaterial);
    });
    if (stop_after(STEP_SUPPORT, until)) return;

    run_step_if_requested(print, STEP_PRE_GCODE, [&] {
        begin_step(print, STEP_PRE_GCODE, L("Preparing G-code"), path);
        StepPrepareGcode::clean_and_prepare(print);
        StepPrepareGcode::run_step(orchestrator, print);
    });
    if (stop_after(STEP_PRE_GCODE, until)) return;

    run_step_if_requested(print, STEP_ORDERING, [&] {
        begin_step(print, STEP_ORDERING, L("Ordering extrusions"), path);
        StepExtrusionOrdering::clean_and_prepare(print);
        StepExtrusionOrdering::run_step(orchestrator, print);
    });
    if (stop_after(STEP_ORDERING, until)) return;

    run_step_if_requested(print, STEP_WIPETOWER, [&] {
        begin_step(print, STEP_WIPETOWER, L("Generating wipe tower"), path);
        StepGenerateWipeTower::clean_and_prepare(print);
        StepGenerateWipeTower::run_step(orchestrator, print);
    });
    if (stop_after(STEP_WIPETOWER, until)) return;

    run_step_if_requested(print, STEP_SUPPORT_SPOT, [&] {
        begin_step(print, STEP_SUPPORT_SPOT, L("Detecting support spots"), path);
        StepDetectSupportSpots::clean_and_prepare(print);
        StepDetectSupportSpots::run_step(orchestrator, print);
        mark_legacy_step_done(print, posSupportSpotsSearch);
    });
    if (stop_after(STEP_SUPPORT_SPOT, until)) return;

    run_step_if_requested(print, STEP_LAYER_EXTRUSION_EDIT, [&] {
        begin_step(print, STEP_LAYER_EXTRUSION_EDIT, L("Editing layers extrusions"), path);
        StepLayerExtrusionEdition::clean_and_prepare(print);
        StepLayerExtrusionEdition::run_step(orchestrator, print);
        mark_legacy_step_done(print, posEstimateCurledExtrusions);
    });
    if (stop_after(STEP_LAYER_EXTRUSION_EDIT, until)) return;

    run_step_if_requested(print, STEP_LAYER_STICHING, [&] {
        begin_step(print, STEP_LAYER_STICHING, L("Stitching layers"), path);
        StepLayerStiching::clean_and_prepare(print);
        StepLayerStiching::run_step(orchestrator, print);
    });
    if (stop_after(STEP_LAYER_STICHING, until)) return;

    run_step_if_requested(print, STEP_EXTRUSION_EDIT, [&] {
        begin_step(print, STEP_EXTRUSION_EDIT, L("Editing extrusions"), path);
        StepExtrusionEdition::clean_and_prepare(print);
        StepExtrusionEdition::run_step(orchestrator, print);
        mark_legacy_step_done(print, posCalculateOverhangingPerimeters);
    });
    if (stop_after(STEP_EXTRUSION_EDIT, until)) return;

    run_step_if_requested(print, STEP_EXTRUSION_SIMPLIFICATION, [&] {
        begin_step(print, STEP_EXTRUSION_SIMPLIFICATION, L("Simplifying extrusions"), path);
        StepExtrusionSimplification::clean_and_prepare(print);
        StepExtrusionSimplification::run_step(orchestrator, print);
        mark_legacy_step_done(print, posSimplifyPath);
    });
    if (stop_after(STEP_EXTRUSION_SIMPLIFICATION, until)) return;
}

#ifdef _DEBUG

void assert_same(DebugPrintProcessComparator &comparator, const char *label)
{
    std::string error;
    if (comparator.compare_tree_after(label, error))
        return;

    std::cerr << error << std::endl;
    assert(false && "Debug step pipeline comparison failed");
    throw std::runtime_error(error);
}

std::vector<coord_t> layer_profile_from_object_layers(const std::vector<double> &object_layers)
{
    std::vector<coord_t> out;
    out.reserve(object_layers.size());
    for (size_t idx = 1; idx < object_layers.size(); idx += 2) {
        out.push_back(scale_i(object_layers[idx]));
        out.push_back(scale_i(object_layers[idx] - object_layers[idx - 1]));
    }
    return out;
}

std::vector<double> object_layers_from_layer_profile(const std::vector<coord_t> &layer_profile)
{
    std::vector<double> out;
    out.reserve(layer_profile.size());
    for (size_t idx = 0; idx + 1 < layer_profile.size(); idx += 2) {
        const double z = unscaled(layer_profile[idx]);
        const double height = unscaled(layer_profile[idx + 1]);
        out.push_back(z - height);
        out.push_back(z);
    }
    return out;
}

void recompute_layer_slices_from_regions(PrintObject &object)
{
    for (Layer &layer : object.layers())
        ApiInternal::LayerAccess::recompute_slices_from_layer_regions(layer);
}

void add_debug_surfaces_from_raw_slices(PrintObject &object)
{
    for (Layer &layer : object.layers()) {
        for (LayerRegion &region : layer.regions()) {
            SurfaceCollection &surfaces = ApiInternal::LayerRegionAccess::surfaces_mutable(region);
            surfaces.clear();
            // Native slice_volumes() creates temporary internal/sparse surfaces
            // from raw slices. Recreate them only for debug comparisons while
            // the plugin slicing step intentionally owns just the raw slices.
            surfaces.append(region.get_raw_slices(), stPosInternal | stDensSparse);
        }
    }
}

void add_debug_surfaces_from_raw_slices(Print &print)
{
    parallel_for(size_t(0), print.objects().size(), [&print](const size_t idx) {
        add_debug_surfaces_from_raw_slices(print.object(idx));
    });
}

void clear_debug_surfaces(PrintObject &object)
{
    for (Layer &layer : object.layers())
        for (LayerRegion &region : layer.regions())
            ApiInternal::LayerRegionAccess::surfaces_mutable(region).clear();
}

void clear_debug_surfaces(Print &print)
{
    parallel_for(size_t(0), print.objects().size(), [&print](const size_t idx) {
        clear_debug_surfaces(print.object(idx));
    });
}

#endif

} // namespace

void StepPipeline::run(Orchestrator &orchestrator, Print &print)
{
#ifdef _DEBUG
    assert(validate_execution_order_against_dependencies());
#endif

    const std::string path;
    orchestrator.reset_plugin_cancel();

    run_layer_height_generation(orchestrator, print, path);
    run_slicing(orchestrator, print, path);
    run_post_slicing(orchestrator, print, path);
    run_remaining_steps(orchestrator, print, path);
}

#ifdef _DEBUG
void StepPipeline::run_native_layer_height_generation(Print &print)
{
    parallel_for(size_t(0), print.objects().size(), [&print](const size_t idx) {
        StepPipeline::run_native_layer_height_generation_object(print.object(idx));
    });
}

void StepPipeline::run_native_slicing(Print &print)
{
    parallel_for(size_t(0), print.objects().size(), [&print](const size_t idx) {
        StepPipeline::run_native_slicing_object(print.object(idx));
    });
}

void StepPipeline::run_native_post_slicing(Print &print)
{
    parallel_for(size_t(0), print.objects().size(), [&print](const size_t idx) {
        StepPipeline::run_native_post_slicing_object(print.object(idx));
    });
}

void StepPipeline::run_native_layer_height_generation_object(PrintObject &object)
{
    object.update_slicing_parameters();

    std::vector<coordf_t> layer_height_profile;
    PrintObject::update_layer_height_profile(*object.model_object(), *object.m_slicing_params, layer_height_profile);

    ApiInternal::PrintObjectAccess::set_layer_profile(
        object,
        layer_profile_from_object_layers(generate_object_layers(object.slicing_parameters(), layer_height_profile)));
}

void StepPipeline::run_native_slicing_object(PrintObject &object)
{
    LayerUPtrs object_layers = new_layers(&object, object_layers_from_layer_profile(object.layer_profile()));
    for (std::unique_ptr<Layer> &layer : object_layers)
        ApiInternal::LayerAccess::init_regions_from_object(*layer);

    ApiInternal::PrintObjectAccess::replace_layers_by_moving_contents(object, std::move(object_layers));
    object.slice_volumes();
    recompute_layer_slices_from_regions(object);
}

void StepPipeline::run_native_post_slicing_object(PrintObject &object)
{
    object._transform_hole_to_polyholes();
    object._max_overhang_threshold();
    recompute_layer_slices_from_regions(object);
}

void StepPipeline::debug_run(Orchestrator &orchestrator, const Print &source, slicing_step_t until)
{
    const std::string path;
    DebugPrintProcessComparator comparator(source);
    orchestrator.reset_plugin_cancel();

    assert_same(comparator, "START");

    run_native_layer_height_generation(comparator.reference_print());
    run_layer_height_generation(orchestrator, comparator.candidate_print(), path);
    assert_same(comparator, "STEP_LAYER_HEIGHT");
    if (stop_after(STEP_LAYER_HEIGHT, until)) return;

    run_native_slicing(comparator.reference_print());
    run_slicing(orchestrator, comparator.candidate_print(), path);
    // temporairement generer des surface dans les regionlayer du plugin pour correspondre à ce que fait le natif
    add_debug_surfaces_from_raw_slices(comparator.candidate_print());
    assert_same(comparator, "STEP_SLICING");
    // on retire les surfaces
    clear_debug_surfaces(comparator.candidate_print());
    if (stop_after(STEP_SLICING, until)) return;

    run_native_post_slicing(comparator.reference_print());
    run_post_slicing(orchestrator, comparator.candidate_print(), path);
    // temporairement generer des surface dans les regionlayer du plugin pour correspondre à ce que fait le natif
    add_debug_surfaces_from_raw_slices(comparator.candidate_print());
    assert_same(comparator, "STEP_POST_SLICING");
    // on retire les surfaces
    clear_debug_surfaces(comparator.candidate_print());
    if (stop_after(STEP_POST_SLICING, until)) return;

    // The remaining step classes are still being filled one by one. Keep the
    // candidate path executable. Add the native side above as each step gets a
    // stable split point.
    run_remaining_steps(orchestrator, comparator.candidate_print(), path, until);
}
#endif


} // namespace Slic3r::Steps
