///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "Orchestrator.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <utility>

#include "libslic3r/Api/plugin/c/slic3r_config_def.h"
#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/c/slic3r_plugin_types.h"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Plugins/BridgeDetector.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Steps/StepPipeline.hpp"
#include "libslic3r/SurfaceCollection.hpp"
#include "libslic3r/UiLayoutMerger.hpp"

#include "ApiHostUtils.hpp"
#include "ClipperShapes.hpp"
#include "Plugin.hpp"

namespace Slic3r {

Orchestrator *orchestrator_from_handle(orchestrator_handle *me) {
    return me == nullptr ? &Orchestrator::instance() : reinterpret_cast<Orchestrator *>(me);
}

const Orchestrator *orchestrator_from_handle(const orchestrator_handle *me) {
    return me == nullptr ? &Orchestrator::instance() : reinterpret_cast<const Orchestrator *>(me);
}

static ConfigOptionContainerType config_option_container_type(raw_container_type type)
{
    switch (type) {
    case RAW_CONTAINER_TYPE_PROJECT: return ConfigOptionContainerType::Project;
    case RAW_CONTAINER_TYPE_PLATER:  return ConfigOptionContainerType::Plater;
    case RAW_CONTAINER_TYPE_OBJECT:  return ConfigOptionContainerType::Object;
    case RAW_CONTAINER_TYPE_LAYER:   return ConfigOptionContainerType::Layer;
    case RAW_CONTAINER_TYPE_REGION:  return ConfigOptionContainerType::Region;
    case RAW_CONTAINER_TYPE_NONE:
    default:                         return ConfigOptionContainerType::None;
    }
}

static ConfigOptionType config_option_type(raw_config_option_type type)
{
    switch (type) {
    case RAW_CO_NONE:                    return coNone;
    case RAW_CO_BOOL:                    return coBool;
    case RAW_CO_INT:                     return coInt;
    case RAW_CO_FLOAT:                   return coFloat;
    case RAW_CO_FLOAT_OR_PERCENT:        return coFloatOrPercent;
    case RAW_CO_PERCENT:                 return coPercent;
    case RAW_CO_STRING:                  return coString;
    case RAW_CO_POINT:                   return coPoint;
    case RAW_CO_ENUM:                    return coEnum;
    case RAW_CO_GRAPH:                   return coGraph;
    case RAW_CO_VECTOR_BOOL:             return coBools;
    case RAW_CO_VECTOR_INT:              return coInts;
    case RAW_CO_VECTOR_FLOAT:            return coFloats;
    case RAW_CO_VECTOR_FLOAT_OR_PERCENT: return coFloatsOrPercents;
    case RAW_CO_VECTOR_STRING:           return coStrings;
    case RAW_CO_VECTOR_POINT:            return coPoints;
    case RAW_CO_VECTOR_GRAPH:            return coGraphs;
    case RAW_CO_VECTOR_ENUM:
    default:                             return coNone;
    }
}

static bool string_vector_equal(const std::vector<std::string> &lhs, const std::vector<std::string> &rhs)
{
    return lhs == rhs;
}

static bool optional_double_equal(const double lhs, const double rhs)
{
    return std::abs(lhs - rhs) < 1e-12;
}

static std::string config_option_def_default_serialized(const ConfigOptionDef &def)
{
    return def.default_value ? def.default_value->serialize() : std::string();
}

static bool enum_def_equal(const ConfigOptionDef &lhs, const ConfigOptionDef &rhs)
{
    if (lhs.enum_def.get() == nullptr || rhs.enum_def.get() == nullptr)
        return lhs.enum_def.get() == nullptr && rhs.enum_def.get() == nullptr;

    return lhs.enum_def->values() == rhs.enum_def->values() &&
           lhs.enum_def->labels() == rhs.enum_def->labels();
}

static bool config_option_def_compatible(const ConfigOptionDef &existing,
                                         const ConfigOptionDef &candidate,
                                         std::string *reason)
{
    const auto fail = [reason](const char *field) {
        if (reason != nullptr)
            *reason = field;
        return false;
    };

    if (existing.type != candidate.type) return fail("type");
    if (existing.category != candidate.category) return fail("category");
    if (existing.gui_type != candidate.gui_type) return fail("gui_type");
    if (existing.printer_technology != candidate.printer_technology) return fail("printer_technology");
    if (existing.container_type != candidate.container_type) return fail("container_type");
    if (existing.option_preset_type != candidate.option_preset_type) return fail("option_preset_type");
    if (existing.invalidates_step != candidate.invalidates_step) return fail("invalidates_step");
    if (existing.is_optional != candidate.is_optional) return fail("is_optional");
    if (existing.can_be_disabled != candidate.can_be_disabled) return fail("can_be_disabled");
    if (existing.multiline != candidate.multiline) return fail("multiline");
    if (existing.full_width != candidate.full_width) return fail("full_width");
    if (existing.is_code != candidate.is_code) return fail("is_code");
    if (existing.is_vector_extruder != candidate.is_vector_extruder) return fail("is_vector_extruder");
    if (existing.readonly != candidate.readonly) return fail("readonly");
    if (existing.can_phony != candidate.can_phony) return fail("can_phony");
    if (existing.aligned_label_left != candidate.aligned_label_left) return fail("aligned_label_left");
    if (existing.gui_flags != candidate.gui_flags) return fail("gui_flags");
    if (existing.label != candidate.label) return fail("label");
    if (existing.full_label != candidate.full_label) return fail("full_label");
    if (existing.tooltip != candidate.tooltip) return fail("tooltip");
    if (existing.sidetext != candidate.sidetext) return fail("sidetext");
    if (existing.cli != candidate.cli) return fail("cli");
    if (existing.ratio_over != candidate.ratio_over) return fail("ratio_over");
    if (existing.height != candidate.height) return fail("height");
    if (existing.width != candidate.width) return fail("width");
    if (existing.label_width != candidate.label_width) return fail("label_width");
    if (existing.sidetext_width != candidate.sidetext_width) return fail("sidetext_width");
    if (!optional_double_equal(existing.min, candidate.min)) return fail("min");
    if (!optional_double_equal(existing.max, candidate.max)) return fail("max");
    if (!optional_double_equal(existing.max_literal.value, candidate.max_literal.value) ||
        existing.max_literal.percent != candidate.max_literal.percent) return fail("max_literal");
    if (existing.precision != candidate.precision) return fail("precision");
    if (existing.mode != candidate.mode) return fail("mode");
    if (!string_vector_equal(existing.aliases, candidate.aliases)) return fail("aliases");
    if (!string_vector_equal(existing.shortcut, candidate.shortcut)) return fail("shortcut");
    if (!string_vector_equal(existing.depends_on, candidate.depends_on)) return fail("depends_on");
    if (!enum_def_equal(existing, candidate)) return fail("enum_def");
    if (config_option_def_default_serialized(existing) != config_option_def_default_serialized(candidate))
        return fail("default_value");

    return true;
}

static bool same_option_ownership_scope(const Orchestrator::ConfigOptionOwner &existing,
                                        const Plugin &candidate)
{
    // Dynamic config keys are global once they enter PrintConfigDef. Without a
    // small ownership rule, two unrelated plugins could accidentally share a
    // key and then disagree about its type, default value, GUI placement or
    // invalidation behavior.
    //
    // The one intentional exception is an exclusive group: those plugins are
    // alternatives for the same job, so sharing a compatible key is how they
    // expose a stable setting regardless of which implementation is selected.
    if (existing.plugin_id == candidate.get_id())
        return true;

    return !existing.exclusive_group.empty() &&
           existing.exclusive_group == candidate.get_exclusive_group();
}

static void populate_config_option_def_from_raw(ConfigOptionDef &out, const raw_config_option_def *def)
{
    out.opt_key = def->opt_key;
    out.type = config_option_type(def->type);
    out.category = static_cast<OptionCategory>(def->category);
    out.gui_type = static_cast<ConfigOptionDef::GUIType>(def->gui_type);
    out.printer_technology = static_cast<PrinterTechnology>(def->printer_technology);
    out.container_type = config_option_container_type(def->container_type);
    out.option_preset_type = static_cast<uint32_t>(def->option_preset_type);
    out.invalidates_step = def->invalidates_step;

    out.is_optional = def->is_optional != 0;
    out.multiline = def->multiline != 0;
    out.full_width = def->full_width != 0;
    out.is_code = def->is_code != 0;
    out.is_vector_extruder = def->is_vector_extruder != 0;
    out.readonly = def->readonly != 0;
    out.can_phony = def->can_phony != 0;
    out.can_be_disabled = def->can_be_disabled != 0;
    out.aligned_label_left = def->aligned_label_left != 0;
    out.is_script = false;

    if (def->gui_flags)
        out.gui_flags = def->gui_flags;
    if (def->label)
        out.label = def->label;
    if (def->full_label)
        out.full_label = def->full_label;
    if (def->tooltip)
        out.tooltip = def->tooltip;
    if (def->sidetext)
        out.sidetext = def->sidetext;
    if (def->cli)
        out.cli = def->cli;
    if (def->ratio_over)
        out.ratio_over = def->ratio_over;

    out.height = def->height;
    out.width = def->width;
    out.label_width = def->label_width;
    out.sidetext_width = def->sidetext_width;

    if (def->has_min)
        out.min = def->min_value;
    if (def->has_max)
        out.max = def->max_value;

    if (def->has_max_literal)
        out.max_literal = FloatOrPercent{def->max_literal_value, def->max_literal_is_percent != 0};

    out.precision = def->precision;
    out.mode = static_cast<ConfigOptionMode>(def->mode);

    for (size_t i = 0; i < def->aliases.size; ++i) {
        const char *s = def->aliases.items[i];
        if (s)
            out.aliases.emplace_back(s);
    }

    for (size_t i = 0; i < def->shortcut.size; ++i) {
        const char *s = def->shortcut.items[i];
        if (s)
            out.shortcut.emplace_back(s);
    }

    for (size_t i = 0; i < def->depends_on.size; ++i) {
        const char *s = def->depends_on.items[i];
        if (s)
            out.depends_on.emplace_back(s);
    }

    const bool has_pair_enum = def->enum_def.value_label_pairs.items != nullptr &&
        def->enum_def.value_label_pairs.count > 0;

    if (has_pair_enum) {
        std::vector<std::string> values;
        std::vector<std::pair<std::string, std::string>> values_labels;

        values.reserve(def->enum_def.value_label_pairs.count);
        values_labels.reserve(def->enum_def.value_label_pairs.count);
        for (size_t i = 0; i < def->enum_def.value_label_pairs.count; ++i) {
            const key_value_string_pair_t &p = def->enum_def.value_label_pairs.items[i];
            values.emplace_back(p.value ? p.value : "");
            values_labels.emplace_back(values.back(), p.label ? p.label : "");
        }

        if (!values.empty()) {
            // Plugin-provided enums may omit gui_type. In that case, keep the
            // old GUI behavior by exposing them as a closed combo box.
            const ConfigOptionDef::GUIType enum_gui_type = out.gui_type == ConfigOptionDef::GUIType::undefined ?
                ConfigOptionDef::GUIType::select_close :
                out.gui_type;
            if (enum_gui_type == ConfigOptionDef::GUIType::select_close) {
                // Closed scripted enums need the string -> int map before the
                // default value is deserialized below. This is especially
                // important for exclusive-step plugin selectors: GUI rules read
                // the enum as its integer index.
                out.set_enum_as_closed_for_scripted_enum(values_labels);
                out.gui_type = ConfigOptionDef::GUIType::select_close;
            } else if (!values_labels.empty()) {
                // Open enums can keep their string values directly; labels are
                // used only for display when provided.
                out.set_enum_values(enum_gui_type, values_labels);
            } else {
                out.set_enum_values(enum_gui_type, values);
            }
        }
    }

    ConfigOption *temp_default_option = out.create_empty_option();
    if (def->default_serialized_value != nullptr)
        temp_default_option->deserialize(def->default_serialized_value);
    out.set_default_value(temp_default_option);
}

static raw_config_option_def config_option_def_with_resolved_invalidation(const raw_config_option_def &def,
                                                                          const Plugin *plugin)
{
    raw_config_option_def resolved = def;

    // raw_config_option_def_init() leaves invalidates_step at STEP_NONE. During
    // plugin initialization this means "use the plugin's own pipeline step" so
    // plugin authors only need to override options that invalidate an earlier
    // step or intentionally use STEP_ANY for full invalidation.
    if (plugin != nullptr && resolved.invalidates_step == STEP_NONE)
        resolved.invalidates_step = plugin->get_step();

    return resolved;
}

static bool validate_used_config_key_definition(const Plugin &,
                                                const Plugin::UsedConfigKey &used_key,
                                                std::string &error_message)
{
    // used_config_keys() is the plugin's read-side contract. The plugin may
    // read a built-in option or an option created by a dependency, but it must
    // describe the value representation it expects. This catches stale plugins
    // before they run with a ConfigOption subclass they do not understand.
    if (used_key.key.empty()) {
        error_message = "declares an empty used config key.";
        return false;
    }

    const ConfigOptionType expected_type = config_option_type(used_key.type);
    if (expected_type == coNone) {
        error_message = "declares used option '" + used_key.key +
                        "' without a supported value type.";
        return false;
    }

    const ConfigOptionDef *def = PrintConfigDef::instance().get(used_key.key);
    if (def == nullptr) {
        error_message = "uses option '" + used_key.key +
                        "', but no active plugin or built-in config defines it.";
        return false;
    }

    if (def->type != expected_type) {
        error_message = "expects option '" + used_key.key +
                        "' to have a different value type.";
        return false;
    }

    if (used_key.container_type != RAW_CONTAINER_TYPE_NONE &&
        def->container_type != config_option_container_type(used_key.container_type)) {
        error_message = "expects option '" + used_key.key +
                        "' to live in a different config container.";
        return false;
    }

    if (used_key.option_preset_type != RAW_PRESET_TYPE_NONE &&
        def->option_preset_type != uint32_t(used_key.option_preset_type)) {
        error_message = "expects option '" + used_key.key +
                        "' to belong to a different preset type.";
        return false;
    }

    return true;
}

static std::chrono::milliseconds elapsed_ms(const std::chrono::steady_clock::time_point &start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
}

} // namespace Slic3r

extern "C" {

option_def_error_code orchestrator_create_option_def(orchestrator_handle *me, const raw_config_option_def *def) {
    if (def == nullptr)
        return OPTION_DEF_ERROR_INVALID_ARGUMENT;

    // removed for now
    // if (def->struct_size < offsetof(raw_config_option_def, reserved_u64))
    //    return OPTION_DEF_ERROR_INVALID_ARGUMENT;

    if (def->opt_key == nullptr || def->default_serialized_value == nullptr ||
        def->container_type == RAW_CONTAINER_TYPE_NONE || def->option_preset_type == RAW_PRESET_TYPE_NONE ||
        def->printer_technology == RAW_PT_NONE)
        return OPTION_DEF_ERROR_INVALID_ARGUMENT;

    try {
        return Slic3r::orchestrator_from_handle(me)->create_new_print_config(def);
    } catch (...) { return OPTION_DEF_ERROR_INTERNAL; }
}

int32_t orchestrator_selected_plugin_used_config_keys(orchestrator_handle *me,
                                                      const config_handle *config,
                                                      slicing_step_t step,
                                                      raw_used_config_key *keys)
{
    Slic3r::Orchestrator *orchestrator = Slic3r::orchestrator_from_handle(me);
    const Slic3r::ConfigBase *config_base = Slic3r::ApiHost::to_config(config);
    const std::vector<Slic3r::Plugin *> plugins =
        Slic3r::Steps::selected_or_active_plugins_for_step(*orchestrator, step, config_base);

    // Exclusive steps yield one selected plugin; non-exclusive steps yield all
    // active plugins. Build one stable union so callers do not have to care
    // which execution model the queried step uses.
    std::vector<const Slic3r::Plugin::UsedConfigKey *> used_keys;
    for (const Slic3r::Plugin *plugin : plugins) {
        if (plugin == nullptr)
            continue;

        const std::vector<Slic3r::Plugin::UsedConfigKey> &plugin_keys = plugin->get_used_config_keys();
        for (const Slic3r::Plugin::UsedConfigKey &candidate : plugin_keys) {
            const std::vector<const Slic3r::Plugin::UsedConfigKey *>::const_iterator found =
                std::find_if(used_keys.begin(),
                             used_keys.end(),
                             [&candidate](const Slic3r::Plugin::UsedConfigKey *existing) {
                                 return existing != nullptr &&
                                        existing->key == candidate.key &&
                                        existing->type == candidate.type &&
                                        existing->container_type == candidate.container_type &&
                                        existing->option_preset_type == candidate.option_preset_type;
                             });
            if (found == used_keys.end())
                used_keys.push_back(&candidate);
        }
    }

    if (keys != nullptr) {
        for (size_t idx = 0; idx < used_keys.size(); ++idx) {
            keys[idx].key = used_keys[idx]->key.c_str();
            keys[idx].type = used_keys[idx]->type;
            keys[idx].container_type = used_keys[idx]->container_type;
            keys[idx].option_preset_type = used_keys[idx]->option_preset_type;
        }
    }
    return int32_t(used_keys.size());
}
}

namespace Slic3r {

Orchestrator::Orchestrator()
{
    // Built-in seam painting is registered in the same table as plugin generic
    // facet annotations. The GUI still creates a fixed toolbar button for seam
    // today, but using the registry keeps the storage key and future plugin
    // path identical from the model's point of view.
    m_generic_facets_annotations.emplace_back(builtin_seam_facets_annotation_definition());
}

Orchestrator &Orchestrator::instance() {
    static Orchestrator s_instance;
    static bool s_default_bridge_detector_registered = []() {
        slic3r_api::BridgeDetectorPlugin::register_bridge_detector_plugin(
            reinterpret_cast<orchestrator_handle *>(&s_instance));
        return true;
    }();
    (void) s_default_bridge_detector_registered;
    return s_instance;
}

bool Orchestrator::register_plugin(plugin_instance plugin) {
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    std::unique_ptr<Plugin> new_plugin;
    try {
        new_plugin.reset(new Plugin(plugin));
    } catch (const std::exception &error) {
        BOOST_LOG_TRIVIAL(error) << "Cannot register plugin: " << error.what() << std::endl;
        return false;
    }

    const std::string new_id = new_plugin->get_id();
    const Plugin *check_exists = get_plugin(new_id);
    if (check_exists) {
        BOOST_LOG_TRIVIAL(error) << "Plugin with id " << new_id << " already exists, cannot register plugin"
                                 << std::endl;
        return false;
    }
    m_registered_plugins.emplace_back(std::move(new_plugin));
    BOOST_LOG_TRIVIAL(debug) << "Registered plugin '" << new_id << "' in " << elapsed_ms(start).count() << " ms.";
    return true;
}

std::vector<Plugin *> Orchestrator::registered_plugins() const
{
    std::vector<Plugin *> plugins;
    plugins.reserve(m_registered_plugins.size());
    for (const std::unique_ptr<Plugin> &plugin : m_registered_plugins)
        plugins.push_back(plugin.get());
    return plugins;
}

bool Orchestrator::add_ui_fragment(const char *target_file,
                                   const char *fragment_id,
                                   const char *content,
                                   int32_t priority)
{
    if (target_file == nullptr || target_file[0] == '\0' ||
        fragment_id == nullptr || fragment_id[0] == '\0' ||
        content == nullptr || content[0] == '\0')
        return false;

    for (const PluginUiFragment &fragment : m_ui_fragments)
        if (fragment.target_file == target_file && fragment.fragment_id == fragment_id) {
            // The fragment id names the logical UI slot, not the plugin that
            // registered it. This lets a built-in layout or one plugin say "the
            // controls for this feature are already present" and suppress later
            // copies. If the second copy is different, the application can still
            // run with the first fragment, but developers need a warning: two
            // providers now disagree on the controls hidden behind one id.
            if (fragment.content != content) {
                std::ostringstream message;
                message << "UI fragment '" << fragment_id << "' for '" << target_file
                        << "' was already registered with different content";
                if (!fragment.plugin_id.empty())
                    message << " by plugin '" << fragment.plugin_id << "'";
                if (m_initializing_plugin != nullptr)
                    message << "; plugin '" << m_initializing_plugin->get_id()
                            << "' attempted to register another version";
                if (m_initializing_plugin != nullptr && !m_initializing_plugin->get_exclusive_group().empty())
                    message << " from exclusive group '" << m_initializing_plugin->get_exclusive_group() << "'";
                if (!fragment.exclusive_group.empty())
                    message << " while the kept fragment belongs to exclusive group '" << fragment.exclusive_group << "'";
                message << ". The first fragment is kept.";
                const std::string message_text = message.str();
                BOOST_LOG_TRIVIAL(warning) << message_text;
                if (m_initializing_plugin != nullptr)
                    this->add_plugin_message(PluginMessageLevel::Warning,
                                             m_initializing_plugin,
                                             STEP_NONE,
                                             message_text.c_str());
            }
            return false;
        }

    PluginUiFragment fragment;
    fragment.target_file = target_file;
    fragment.fragment_id = fragment_id;
    fragment.content = content;
    if (m_initializing_plugin != nullptr) {
        fragment.plugin_id = m_initializing_plugin->get_id();
        fragment.exclusive_group = m_initializing_plugin->get_exclusive_group();
    }
    fragment.priority = priority;
    fragment.order = m_next_ui_fragment_order++;
    m_ui_fragments.emplace_back(std::move(fragment));
    return true;
}

std::vector<Orchestrator::PluginUiFragment>
Orchestrator::ui_fragments_for_file(const std::string &target_file) const
{
    std::vector<PluginUiFragment> out;
    for (const PluginUiFragment &fragment : m_ui_fragments)
        if (fragment.target_file == target_file)
            out.emplace_back(fragment);

    std::sort(out.begin(), out.end(), [](const PluginUiFragment &lhs, const PluginUiFragment &rhs) {
        if (lhs.priority != rhs.priority)
            return lhs.priority < rhs.priority;
        if (lhs.order != rhs.order)
            return lhs.order < rhs.order;
        return lhs.fragment_id < rhs.fragment_id;
    });
    return out;
}

std::string Orchestrator::merged_ui_layout(const std::string &target_file, const std::string &base_content) const
{
    static const std::unordered_set<std::string> empty_implemented_fragment_ids;
    return this->merged_ui_layout(target_file, base_content, empty_implemented_fragment_ids);
}

std::string Orchestrator::merged_ui_layout(const std::string &target_file,
                                           const std::string &base_content,
                                           const std::unordered_set<std::string> &implemented_fragment_ids) const
{
    const std::vector<PluginUiFragment> fragments = this->ui_fragments_for_file(target_file);
    if (fragments.empty())
        return base_content;

    UiLayoutMerger merger(target_file);
    merger.set_base(base_content);
    for (const PluginUiFragment &fragment : fragments) {
        if (implemented_fragment_ids.find(fragment.fragment_id) != implemented_fragment_ids.end())
            continue;
        merger.add_fragment(fragment.fragment_id, fragment.content, fragment.priority, fragment.order);
    }
    return merger.merged();
}

bool Orchestrator::add_gui_rule(const raw_gui_rule *rule)
{
    if (rule == nullptr ||
        rule->action == RAW_GUI_RULE_ACTION_NONE ||
        rule->condition == RAW_GUI_RULE_CONDITION_NONE ||
        rule->target_key == nullptr || rule->target_key[0] == '\0' ||
        rule->condition_key == nullptr || rule->condition_key[0] == '\0')
        return false;

    PluginGuiRule gui_rule;
    gui_rule.action = rule->action;
    gui_rule.condition = rule->condition;
    gui_rule.target_key = rule->target_key;
    gui_rule.condition_key = rule->condition_key;
    gui_rule.target_index = rule->target_index;
    gui_rule.condition_index = rule->condition_index;
    gui_rule.condition_int_value = rule->condition_int_value;

    for (const PluginGuiRule &existing : m_gui_rules)
        if (existing.action == gui_rule.action &&
            existing.condition == gui_rule.condition &&
            existing.target_key == gui_rule.target_key &&
            existing.condition_key == gui_rule.condition_key &&
            existing.target_index == gui_rule.target_index &&
            existing.condition_index == gui_rule.condition_index &&
            existing.condition_int_value == gui_rule.condition_int_value)
            return false;

    m_gui_rules.emplace_back(std::move(gui_rule));
    return true;
}

slic3r_property_type Orchestrator::register_property(const char *namespaced_name,
                                                     uint32_t byte_count,
                                                     uint32_t alignment)
{
    if (namespaced_name == nullptr || namespaced_name[0] == '\0' || byte_count == 0 || alignment == 0)
        return SLIC3R_PROPERTY_TYPE_INVALID;

    /*
    The orchestrator owns the string-to-id mapping for custom properties. The
    numeric id is only a compact runtime handle; callers must register by name
    again for each orchestrator so parallel plugin configurations do not share
    mutable global state.
    */
    if (const PropertyInfo *existing = this->property_info(namespaced_name)) {
        return existing->byte_count == byte_count && existing->alignment == alignment ?
            existing->type :
            SLIC3R_PROPERTY_TYPE_INVALID;
    }

    PropertyInfo info;
    info.type = m_next_custom_property_type++;
    if (info.type == SLIC3R_PROPERTY_TYPE_INVALID)
        info.type = m_next_custom_property_type++;
    info.name = namespaced_name;
    info.byte_count = byte_count;
    info.alignment = alignment;
    m_custom_property_infos.emplace_back(std::move(info));
    return m_custom_property_infos.back().type;
}

extrusion_property_type Orchestrator::register_custom_extrusion_property(const char *namespaced_name,
                                                                         uint32_t byte_count,
                                                                         uint32_t alignment)
{
    return static_cast<extrusion_property_type>(this->register_property(namespaced_name, byte_count, alignment));
}

bool Orchestrator::register_generic_facets_annotation(GenericFacetsAnnotationDefinition def)
{
    if (def.key.empty() || def.label.empty() || def.enforce_label.empty() || def.block_label.empty())
        return false;

    if (def.icon_filename.empty() && def.icon_svg.empty())
        return false;

    // Duplicate keys are allowed only when they describe the same painting, so
    // a plugin can call registration more than once without changing the GUI or
    // model storage associated with that stable key.
    for (const GenericFacetsAnnotationDefinition &existing : m_generic_facets_annotations) {
        if (existing.key != def.key)
            continue;

        if (existing.label == def.label &&
            existing.enforce_label == def.enforce_label &&
            existing.block_label == def.block_label &&
            existing.icon_filename == def.icon_filename &&
            existing.icon_svg == def.icon_svg)
            return true;

        if (m_initializing_plugin != nullptr) {
            m_initializing_plugin_failed = true;
            m_initializing_plugin_failure = "Generic facet annotation '" + def.key +
                "' was already registered with different labels.";
        }
        return false;
    }

    m_generic_facets_annotations.emplace_back(std::move(def));
    return true;
}

const Orchestrator::CustomExtrusionPropertyInfo*
Orchestrator::custom_extrusion_property_info(extrusion_property_type type) const
{
    return this->property_info(type);
}

const Orchestrator::PropertyInfo*
Orchestrator::property_info(slic3r_property_type type) const
{
    for (const PropertyInfo &info : m_custom_property_infos)
        if (info.type == type)
            return &info;
    return nullptr;
}

const Orchestrator::CustomExtrusionPropertyInfo*
Orchestrator::custom_extrusion_property_info(const char *namespaced_name) const
{
    return this->property_info(namespaced_name);
}

const Orchestrator::PropertyInfo*
Orchestrator::property_info(const char *namespaced_name) const
{
    if (namespaced_name == nullptr)
        return nullptr;

    for (const PropertyInfo &info : m_custom_property_infos)
        if (info.name == namespaced_name)
            return &info;
    return nullptr;
}

plugin_host_context Orchestrator::prepare_plugin_host_context(slicing_step_t step,
                                                              Plugin *plugin,
                                                              Print *print)
{
    plugin_host_context context = {};
    context.orchestrator = this;
    context.print = print;
    context.plugin = plugin;
    context.step = step;
    return context;
}

plugin_run_context Orchestrator::prepare_plugin_run_context(slicing_step_t step,
                                                            Plugin *plugin,
                                                            plugin_host_context *host_context) {
    plugin_run_context context = {};
    context.step = step;
    context.plugin_storage = reinterpret_cast<storage_handle *>(&this->plugin_storage()[plugin]);
    context.host_context = host_context;
    context.is_cancelled = orchestrator_plugin_is_cancelled;
    context.report_warning = orchestrator_plugin_report_warning;
    context.report_error = orchestrator_plugin_report_error;
    context.report_progress = orchestrator_plugin_report_progress;
    return context;
}

void Orchestrator::add_plugin_message(PluginMessageLevel level,
                                      const Plugin *plugin,
                                      slicing_step_t step,
                                      const char *message)
{
    // Plugins can report messages while slicing runs on a worker thread, while
    // the Plater consumes them later from the UI thread. The queue keeps only
    // copied strings, so plugin-owned buffers may disappear immediately after
    // the callback returns.
    PluginMessage plugin_message;
    plugin_message.level = level;
    plugin_message.plugin_id = plugin != nullptr ? plugin->get_id() : "<unknown>";
    plugin_message.step = step;
    plugin_message.message = message != nullptr && message[0] != '\0' ?
        message :
        "(empty plugin message)";

    std::lock_guard<std::mutex> lock(m_plugin_messages_mutex);
    m_plugin_messages.emplace_back(std::move(plugin_message));
}

std::vector<Orchestrator::PluginMessage> Orchestrator::consume_plugin_messages()
{
    // The GUI drains the queue instead of peeking at it so each plugin message
    // is displayed exactly once, even if several status updates arrive for the
    // same slicing step.
    std::lock_guard<std::mutex> lock(m_plugin_messages_mutex);
    std::vector<PluginMessage> messages;
    messages.swap(m_plugin_messages);
    return messages;
}

bool Orchestrator::is_plugin_cancelled() const { return m_plugin_cancel_requested.load(std::memory_order_relaxed); }

void Orchestrator::request_plugin_cancel() { m_plugin_cancel_requested.store(true, std::memory_order_relaxed); }

void Orchestrator::reset_plugin_cancel() { m_plugin_cancel_requested.store(false, std::memory_order_relaxed); }

bool Orchestrator::validate_plugin_activation(const std::vector<std::string> &plugin_ids,
                                              std::string &error_message) const
{
    // The plugin dialog needs to reject a bad activation set before it writes
    // activated.ini and asks the user to restart. At that moment the plugins
    // are loaded but not initialized, so their real raw_config_option_def
    // objects may not exist yet. The only contract available cheaply is the
    // declared list of keys returned by defined_config_keys().
    //
    // Therefore this is a purposefully narrow ownership preflight:
    // - it verifies that every requested plugin is loaded;
    // - it rejects a key already owned by an unrelated plugin;
    // - it rejects a key already present in the built-in/global config unless
    //   it was introduced by the same compatible exclusive group.
    //
    // It does not decide whether the definitions are byte-for-byte compatible.
    // That stricter validation still happens in create_new_print_config() while
    // the active plugin initializes and publishes the actual option definition.
    std::set<std::string> selected_ids(plugin_ids.begin(), plugin_ids.end());
    std::map<std::string, ConfigOptionOwner> future_owners = m_config_option_owners;

    for (const std::string &plugin_id : selected_ids) {
        const Plugin *plugin = this->get_plugin(plugin_id);
        if (plugin == nullptr) {
            error_message = "Plugin '" + plugin_id + "' is not loaded.";
            return false;
        }

        for (const std::string &key : plugin->get_defined_config_keys()) {
            const std::map<std::string, ConfigOptionOwner>::const_iterator existing_owner = future_owners.find(key);
            if (existing_owner != future_owners.end()) {
                if (same_option_ownership_scope(existing_owner->second, *plugin))
                    continue;

                error_message = "Plugin '" + plugin->get_id() + "' defines option '" + key +
                    "', but that option is already owned by plugin '" + existing_owner->second.plugin_id + "'.";
                return false;
            }

            if (PrintConfigDef::instance().get(key) != nullptr) {
                error_message = "Plugin '" + plugin->get_id() + "' defines option '" + key +
                    "', but that option already exists outside its exclusive group.";
                return false;
            }

            future_owners.emplace(key, ConfigOptionOwner{plugin->get_id(), plugin->get_exclusive_group()});
        }
    }

    return true;
}

option_def_error_code Orchestrator::create_new_print_config(const raw_config_option_def *def) {
    const raw_config_option_def resolved_def =
        config_option_def_with_resolved_invalidation(*def, m_initializing_plugin);
    def = &resolved_def;

    //PrintOptionPresetType preset_type = static_cast<PrintOptionPresetType>(def->option_preset_type);
    //PrintOptionContainer container = static_cast<PrintOptionContainer>(def->container_type);
    const ConfigOptionType type = config_option_type(def->type);
    assert(type != coNone);

    // This function is the definitive validation point for plugin settings.
    // Unlike validate_plugin_activation(), it sees the whole raw_config_option_def
    // and can compare every user-visible and serialization-relevant field.
    //
    // Several alternative plugins may publish the same setting, but only when
    // they are explicit alternatives in the same exclusive group. A compatible
    // duplicate from another group is still an error: a plugin may depend on
    // an option owned by another plugin, but it must not re-declare that
    // option. The dependency should provide the option definition; the
    // dependent plugin should only read the already-owned key.
    if (const ConfigOptionDef *existing = PrintConfigDef::instance().get(def->opt_key)) {
        ConfigOptionDef candidate;
        populate_config_option_def_from_raw(candidate, def);

        std::string reason;
        if (config_option_def_compatible(*existing, candidate, &reason)) {
            if (m_initializing_plugin == nullptr)
                return OPTION_DEF_ERROR_OK;

            const std::map<std::string, ConfigOptionOwner>::const_iterator owner_it =
                m_config_option_owners.find(def->opt_key);
            if (owner_it != m_config_option_owners.end() &&
                same_option_ownership_scope(owner_it->second, *m_initializing_plugin))
                return OPTION_DEF_ERROR_OK;

            std::ostringstream message;
            message << "Plugin config option '" << def->opt_key
                    << "' is already defined outside exclusive group '"
                    << m_initializing_plugin->get_exclusive_group() << "'.";
            if (owner_it != m_config_option_owners.end())
                message << " Existing owner is plugin '" << owner_it->second.plugin_id << "'.";
            else
                message << " Existing definition is not owned by a plugin in this group.";
            message << " Plugin '" << m_initializing_plugin->get_id() << "' will be disabled.";
            const std::string message_text = message.str();
            BOOST_LOG_TRIVIAL(error) << message_text;
            this->add_plugin_message(PluginMessageLevel::Error,
                                     m_initializing_plugin,
                                     STEP_NONE,
                                     message_text.c_str());
            m_initializing_plugin_failed = true;
            m_initializing_plugin_failure = message_text;
            return OPTION_DEF_ERROR_ALREADY_EXISTS;
        }

        std::ostringstream message;
        message << "Plugin config option '" << def->opt_key
                << "' is already defined with incompatible " << reason << ".";
        if (m_initializing_plugin != nullptr)
            message << " Plugin '" << m_initializing_plugin->get_id() << "' will be disabled.";
        const std::string message_text = message.str();
        BOOST_LOG_TRIVIAL(error) << message_text;
        if (m_initializing_plugin != nullptr) {
            this->add_plugin_message(PluginMessageLevel::Error,
                                     m_initializing_plugin,
                                     STEP_NONE,
                                     message_text.c_str());
            m_initializing_plugin_failed = true;
            m_initializing_plugin_failure = message_text;
        }
        return OPTION_DEF_ERROR_ALREADY_EXISTS;
    }

    ConfigOptionDef &out = *PrintConfigDef::instance_mutable().add(def->opt_key, type);
    populate_config_option_def_from_raw(out, def);

    // publish it?
    PrintConfigDef::instance_mutable().option_keys(def->option_preset_type).insert(out.opt_key);
    if(def->option_preset_type == RAW_PRESET_TYPE_FFF_FILAMENT_OVERRIDE) {
        assert(false); // please do'nt yet, not made for that
    }
    if(def->option_preset_type == RAW_PRESET_TYPE_SLA_MATERIAL_OVERRIDE) {
        assert(false); // please do'nt yet, not made for that
    }
    if(def->option_preset_type == RAW_PRESET_TYPE_FFF_TOOL_EXTRUDER
        || def->option_preset_type == RAW_PRESET_TYPE_FFF_PRINTER_MACHINE_LIMITS
        || def->option_preset_type == RAW_PRESET_TYPE_FFF_TOOL_MILLING) {
        PrintConfigDef::instance_mutable().option_keys(RAW_PRESET_TYPE_FFF_PRINTER).insert(out.opt_key);
    }
    if(def->option_preset_type == RAW_PRESET_TYPE_FFF_TOOL_EXTRUDER_RETRACTION) {
        PrintConfigDef::instance_mutable().option_keys(RAW_PRESET_TYPE_FFF_TOOL_EXTRUDER).insert(out.opt_key);
        PrintConfigDef::instance_mutable().option_keys(RAW_PRESET_TYPE_FFF_PRINTER).insert(out.opt_key);
    }
    add_to_prusa_export_to_remove_keys(out.opt_key);
    if (m_initializing_plugin != nullptr)
        m_config_option_owners[out.opt_key] = ConfigOptionOwner{
            m_initializing_plugin->get_id(),
            m_initializing_plugin->get_exclusive_group()
        };
    return OPTION_DEF_ERROR_OK;
}

std::vector<Plugin *> Orchestrator::get_all_plugins_for_step(slicing_step_t step) const {
    std::vector<Plugin *> list;
    for (const std::unique_ptr<Plugin> &plugin : m_registered_plugins) {
        if (plugin->get_step() == step)
            list.push_back(plugin.get());
    }
    std::stable_sort(list.begin(), list.end(), [](const Plugin *lhs, const Plugin *rhs) {
        return lhs->get_priority() < rhs->get_priority();
    });
    return list;
}

std::vector<Plugin *> Orchestrator::get_active_plugins_for_step(slicing_step_t step) const {
    std::vector<Plugin *> list;
    for (Plugin *plugin : this->get_all_plugins_for_step(step))
        if (this->is_plugin_active(plugin))
            list.push_back(plugin);
    return list;
}

std::vector<Plugin *> Orchestrator::get_current_plugins_for_step(slicing_step_t step) const {
    auto it = m_plugins_by_step.find(step);
    if (it == m_plugins_by_step.end())
        return {};

    std::vector<Plugin *> plugins;
    plugins.reserve(it->second.size());
    for (Plugin *plugin : it->second)
        if (this->is_plugin_active(plugin))
            plugins.push_back(plugin);
    return plugins;
}

const Plugin *Orchestrator::get_plugin(const std::string &plugin_id) const {
    const Plugin *plugin = nullptr;
    for (const std::unique_ptr<Plugin> &plugin_test : m_registered_plugins) {
        if (plugin_test->get_id() == plugin_id) {
            plugin = plugin_test.get();
            break;
        }
    }
    return plugin;
}

Plugin *Orchestrator::get_plugin(const std::string &plugin_id) {
    Plugin *plugin = nullptr;
    for (const std::unique_ptr<Plugin> &plugin_test : m_registered_plugins) {
        if (plugin_test->get_id() == plugin_id) {
            plugin = plugin_test.get();
            break;
        }
    }
    return plugin;
}

bool Orchestrator::is_plugin_active(const Plugin *plugin) const
{
    return plugin != nullptr && m_active_plugins.find(const_cast<Plugin *>(plugin)) != m_active_plugins.end();
}

bool Orchestrator::is_plugin_active(const std::string &plugin_id) const
{
    return this->is_plugin_active(this->get_plugin(plugin_id));
}

void Orchestrator::clear_active_plugins()
{
    m_active_plugins.clear();
}

bool Orchestrator::set_plugin_active(Plugin *plugin, bool active)
{
    if (plugin == nullptr)
        return false;

    if (active)
        m_active_plugins.insert(plugin);
    else
        m_active_plugins.erase(plugin);
    return true;
}

bool Orchestrator::set_plugin_active(const std::string &plugin_id, bool active)
{
    return this->set_plugin_active(this->get_plugin(plugin_id), active);
}

void Orchestrator::add_plugin_to_step(Plugin *plugin, slicing_step_t step) {
    if (!plugin) {
        return;
    }
    auto it_unique = m_plugins_by_step.find(step);
    if (it_unique != m_plugins_by_step.end()) {
        // replace
        it_unique->second = {plugin};
    } else {
        // create new
        std::vector<Plugin *> &plugins = m_plugins_by_step[step];
        // check not already inside, if so remove it.
        plugins.erase(std::remove(plugins.begin(), plugins.end(), plugin), plugins.end());
        // search best place
        // search from end until a dep is found
        // we have to put it between plugins[i-1] and plugins[i]
        size_t i = plugins.size();
        const std::vector<std::string> &deps = plugin->get_dependencies();
        for (; i > 0; i--) {
            const std::string candidate_id = plugins[i - 1]->get_id();
            for (const std::string &dep : deps) {
                if (candidate_id == dep) {
                    goto found_dep;
                }
            }
        }
    found_dep:
        // now move forward until the  priority is higher than our plugin's
        for (; i < plugins.size(); i++) {
            if (plugins[i]->get_priority() > plugin->get_priority()) {
                break;
            }
        }
        plugins.insert(plugins.begin() + i, plugin);
    }
}

void Orchestrator::slice(Print &print) {
    Steps::StepPipeline::run_slice(*this, print);
}

std::string Orchestrator::export_gcode(Print &print,
                                       const std::string &path_template,
                                       GCodeProcessorResult *result,
                                       ThumbnailsGeneratorCallback thumbnail_cb)
{
    (void)thumbnail_cb;

    // output everything to a G-code file
    // The following call may die if the output_filename_format template substitution fails.
    const std::string path = print.output_filepath(path_template);
    if (!path.empty() && result == nullptr) {
        // Only show the path if preview_data is not set -> running from command line.
        print.set_status(printstep_percent(psGCodeExport), L("Exporting G-code to %s"), {path});
    } else {
        print.set_status(printstep_percent(psGCodeExport), L("Generating G-code"));
    }

    // Export can be requested multiple times after one slice. Force the
    // export-side sub-pipeline to rebuild the PrintingPlan and rewrite the
    // destination file, while leaving the expensive slicing data intact.
    print.mark_step_and_dependents_for_execution(STEP_ORDERING);
    Steps::StepPipeline::run_gcode(*this, print, path);

    if (result != nullptr) {
        result->reset();
        result->filename = path;
        if (print.conflict_result())
            result->conflict_result = *print.conflict_result();
    }

    print.set_status(100, "", PrintBase::SlicingStatus::DEFAULT | PrintBase::SlicingStatus::SECONDARY_STATE);
    print.set_status(100, L("Gcode done"), PrintBase::SlicingStatus::FlagBits::GCODE_ENDED);
    return path;
}

void Orchestrator::initialize_plugins() {
    for (const std::unique_ptr<Plugin> &plugin_ptr : m_registered_plugins) {
        if (this->is_plugin_active(plugin_ptr.get())) {
            const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
            // Plugin initialization is allowed to register dynamic options,
            // GUI fragments and GUI rules. Keep the current plugin in the
            // orchestrator so those registration paths can attach diagnostics
            // to the responsible plugin and, for option definitions, disable
            // only that plugin if validation fails.
            m_initializing_plugin = plugin_ptr.get();
            m_initializing_plugin_failed = false;
            m_initializing_plugin_failure.clear();
            plugin_ptr->initialize(reinterpret_cast<storage_handle *>(&m_plugin_storage[plugin_ptr.get()]));
            if (m_initializing_plugin_failed) {
                this->set_plugin_active(plugin_ptr.get(), false);
                BOOST_LOG_TRIVIAL(error) << "Disabled plugin '" << plugin_ptr->get_id()
                                         << "' after initialization failure: "
                                         << m_initializing_plugin_failure;
            } else {
                BOOST_LOG_TRIVIAL(debug) << "Initialized plugin '" << plugin_ptr->get_id() << "' in "
                                         << elapsed_ms(start).count() << " ms.";
            }
            m_initializing_plugin = nullptr;
            m_initializing_plugin_failed = false;
            m_initializing_plugin_failure.clear();
        }
    }

    for (const std::unique_ptr<Plugin> &plugin_ptr : m_registered_plugins) {
        if (!this->is_plugin_active(plugin_ptr.get()))
            continue;

        // Validate read-side config contracts after every active plugin had a
        // chance to publish its option definitions. This allows a plugin to
        // depend on settings created by another active plugin without owning or
        // re-declaring those settings itself.
        for (const Plugin::UsedConfigKey &used_key : plugin_ptr->get_used_config_keys()) {
            std::string failure;
            if (!validate_used_config_key_definition(*plugin_ptr, used_key, failure)) {
                this->set_plugin_active(plugin_ptr.get(), false);
                const std::string message = failure + " Plugin will be disabled.";
                BOOST_LOG_TRIVIAL(error) << "Plugin '" << plugin_ptr->get_id() << "': " << message;
                // This validation runs after plugin initialization, so plugin
                // code cannot report the failure itself. Queue the diagnostic
                // here to make the automatic deactivation visible in the GUI.
                this->add_plugin_message(PluginMessageLevel::Error,
                                         plugin_ptr.get(),
                                         plugin_ptr->get_step(),
                                         message.c_str());
                break;
            }
        }
    }
}

PluginStorage::PluginStorage() = default;
PluginStorage::~PluginStorage() = default;

void PluginStorage::clear() {
    polylines.clear();
    polygons.clear();
    expolygons.clear();
    polyline_collections.clear();
    polygon_collections.clear();
    expolygon_collections.clear();
    surface_collections.clear();
    extrusions.clear();
    clipper_shapes.clear();
    generic_storage.clear();
}

bool PluginStorage::contains(void *ptr) const {
    return ptr != nullptr && generic_storage.find(ptr) != generic_storage.end();
}

size_t PluginStorage::size() const { return generic_storage.size(); }

bool PluginStorage::free(void *ptr) {
    if (!contains(ptr)) {
        assert(false);
        return false;
    }

    for (auto it = polygons.begin(); it != polygons.end(); ++it) {
        if (ptr == static_cast<void *>(it->get())) {
            polygons.erase(it);
            generic_storage.erase(ptr);
            return true;
        }
    }

    for (auto it = polylines.begin(); it != polylines.end(); ++it) {
        if (ptr == static_cast<void *>(it->get())) {
            polylines.erase(it);
            generic_storage.erase(ptr);
            return true;
        }
    }

    for (auto it = expolygons.begin(); it != expolygons.end(); ++it) {
        if (ptr == static_cast<void *>(it->get())) {
            expolygons.erase(it);
            generic_storage.erase(ptr);
            return true;
        }
    }

    for (auto it = polyline_collections.begin(); it != polyline_collections.end(); ++it) {
        if (ptr == static_cast<void *>(it->get())) {
            polyline_collections.erase(it);
            generic_storage.erase(ptr);
            return true;
        }
    }

    for (auto it = polygon_collections.begin(); it != polygon_collections.end(); ++it) {
        if (ptr == static_cast<void *>(it->get())) {
            polygon_collections.erase(it);
            generic_storage.erase(ptr);
            return true;
        }
    }

    for (auto it = expolygon_collections.begin(); it != expolygon_collections.end(); ++it) {
        if (ptr == static_cast<void *>(it->get())) {
            expolygon_collections.erase(it);
            generic_storage.erase(ptr);
            return true;
        }
    }

    for (auto it = surface_collections.begin(); it != surface_collections.end(); ++it) {
        if (ptr == static_cast<void *>(it->get())) {
            surface_collections.erase(it);
            generic_storage.erase(ptr);
            return true;
        }
    }

    for (auto it = extrusions.begin(); it != extrusions.end(); ++it) {
        if (ptr == static_cast<void *>(it->get())) {
            extrusions.erase(it);
            generic_storage.erase(ptr);
            return true;
        }
    }

    for (auto it = clipper_shapes.begin(); it != clipper_shapes.end(); ++it) {
        if (ptr == static_cast<void *>(it->get())) {
            clipper_shapes.erase(it);
            generic_storage.erase(ptr);
            return true;
        }
    }

    generic_storage.erase(ptr);
    return false;
}

} // namespace Slic3r
