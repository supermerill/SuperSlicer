///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "PluginLoader.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

#include <chrono>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/plugin/c/slic3r_plugin.h"
#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/FFFPrintConfig.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Plugins/GCode/PrintingPlanFileWriter.hpp"
#include "libslic3r/Plugins/Infill/DefaultInfillGenerator.hpp"
#include "libslic3r/Plugins/Infill/LegacyInfillPatterns.hpp"
#include "libslic3r/Plugins/Infill/PostInfillGapFill.hpp"
#include "libslic3r/Plugins/MaxOverhangThreshold.hpp"
#include "libslic3r/Plugins/Ordering/DefaultOrdering.hpp"
#include "libslic3r/Plugins/Perimeter/ArachnePerimeterGenerator.hpp"
#include "libslic3r/Plugins/Perimeter/ClassicPerimeterGenerator.hpp"
#include "libslic3r/Plugins/Perimeter/ExtraPerimeterBelowArea.hpp"
#include "libslic3r/Plugins/Perimeter/ExtraPerimeterCount.hpp"
#include "libslic3r/Plugins/Perimeter/ExtraPerimeterOddLayer.hpp"
#include "libslic3r/Plugins/Perimeter/DetectOverhang.hpp"
#include "libslic3r/Plugins/Perimeter/ExtraPerimetersOnOverhangs.hpp"
#include "libslic3r/Plugins/Perimeter/FuzzySkin.hpp"
#include "libslic3r/Plugins/Perimeter/MarkFirstLoop.hpp"
#include "libslic3r/Plugins/Perimeter/OnlyOnePerimeterFirstLayer.hpp"
#include "libslic3r/Plugins/Perimeter/OnlyOnePerimeterOnTop.hpp"
#include "libslic3r/Plugins/Perimeter/RemoveGapFillOnOverhangs.hpp"
#include "libslic3r/Plugins/Perimeter/SeparateHoleContour.hpp"
#include "libslic3r/Plugins/Perimeter/SimplePerimeterGenerator.hpp"
#include "libslic3r/Plugins/SliceVolume.hpp"
#include "libslic3r/Plugins/StandardLayerHeightGenerator.hpp"
#include "libslic3r/Plugins/Surface/CleanInfillSurfaces.hpp"
#include "libslic3r/Plugins/Surface/InitialTypedSurfaceBuilder.hpp"
#include "libslic3r/Plugins/Surface/InfillRegionCompatibilitySplitter.hpp"
#include "libslic3r/Plugins/Surface/SolidShells.hpp"
#include "libslic3r/Plugins/Surface/TopSurfaceExpansion.hpp"
#include "libslic3r/Plugins/Support/SupportDemandBridgeRemoval.hpp"
#include "libslic3r/Plugins/VaseMultiIslandConnector.hpp"
#include "libslic3r/Steps/StepPipeline.hpp"
#include "libslic3r/Utils.hpp"

namespace Slic3r {
namespace {

using RegisterPluginFn = void (*)(orchestrator_handle *);
using PluginAbiVersionFn = uint32_t (*)();
using PluginLoadClock = std::chrono::steady_clock;

const char *const PLUGIN_ACTIVATION_DIR = "plugin";
const char *const ACTIVATED_PLUGINS_FILENAME = "activated.ini";
const char *const DEFAULT_ACTIVATED_PLUGINS_DIR = "plugins";
const char *const DEFAULT_ACTIVATED_PLUGINS_FILENAME = "default_activated.ini";

std::chrono::milliseconds elapsed_ms(const PluginLoadClock::time_point &start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(PluginLoadClock::now() - start);
}

bool ini_value_is_enabled(const std::string &value)
{
    return boost::algorithm::iequals(value, "1") ||
           boost::algorithm::iequals(value, "true") ||
           boost::algorithm::iequals(value, "yes") ||
           boost::algorithm::iequals(value, "on") ||
           boost::algorithm::iequals(value, "enabled");
}

bool read_plugin_activation_ini(const boost::filesystem::path &config_path,
                                std::map<std::string, bool> &plugin_states)
{
    boost::nowide::ifstream stream(config_path.string());
    if (!stream) {
        BOOST_LOG_TRIVIAL(warning) << "Cannot read active plugin configuration '" << config_path.string() << "'.";
        return false;
    }

    boost::property_tree::ptree tree;
    try {
        boost::property_tree::read_ini(stream, tree);
    } catch (const boost::property_tree::ini_parser_error &error) {
        BOOST_LOG_TRIVIAL(warning) << "Cannot parse active plugin configuration '" << config_path.string()
                                   << "': " << error.what();
        return false;
    }

    const boost::property_tree::ptree &const_tree = tree;
    const boost::optional<const boost::property_tree::ptree&> activated = const_tree.get_child_optional("activated");
    if (!activated) {
        BOOST_LOG_TRIVIAL(warning) << "Active plugin configuration '" << config_path.string()
                                   << "' has no [activated] section.";
        return false;
    }

    for (const boost::property_tree::ptree::value_type &entry : *activated) {
        const std::string plugin_id = boost::algorithm::trim_copy(entry.first);
        if (!plugin_id.empty())
            plugin_states[plugin_id] = ini_value_is_enabled(entry.second.get_value<std::string>());
    }

    return true;
}

std::vector<std::string> enabled_plugin_ids(const std::map<std::string, bool> &plugin_states)
{
    std::vector<std::string> plugin_ids;
    for (const auto &[plugin_id, is_enabled] : plugin_states)
        if (is_enabled)
            plugin_ids.push_back(plugin_id);
    return plugin_ids;
}

boost::filesystem::path default_active_plugin_config_path()
{
    return boost::filesystem::path(Slic3r::resources_dir()) / DEFAULT_ACTIVATED_PLUGINS_DIR / DEFAULT_ACTIVATED_PLUGINS_FILENAME;
}

boost::filesystem::path active_plugin_config_path(const boost::filesystem::path &config_dir)
{
    return config_dir / PLUGIN_ACTIVATION_DIR / ACTIVATED_PLUGINS_FILENAME;
}

boost::filesystem::path ensure_active_plugin_config(const boost::filesystem::path &config_dir,
                                                    bool &from_user_config)
{
    const boost::filesystem::path default_config_path = default_active_plugin_config_path();
    from_user_config = false;

    if (config_dir.empty()) {
        BOOST_LOG_TRIVIAL(trace) << "data_dir is not available before plugin activation. Using default active plugin "
                                    "configuration from resources.";
        return default_config_path;
    }

    const boost::filesystem::path config_path = active_plugin_config_path(config_dir);
    if (boost::filesystem::exists(config_path)) {
        from_user_config = true;
        return config_path;
    }

    try {
        boost::filesystem::create_directories(config_path.parent_path());
        boost::filesystem::copy_file(default_config_path, config_path);
        from_user_config = true;
        return config_path;
    } catch (const boost::filesystem::filesystem_error &error) {
        BOOST_LOG_TRIVIAL(warning) << "Cannot create active plugin configuration '" << config_path.string()
                                   << "' from '" << default_config_path.string() << "': " << error.what()
                                   << ". Falling back to resources.";
        return default_config_path;
    }
}

std::vector<std::string> read_active_plugin_ids(const boost::filesystem::path &config_dir,
                                                bool &from_user_config)
{
    const boost::filesystem::path config_path = ensure_active_plugin_config(config_dir, from_user_config);
    std::map<std::string, bool> plugin_states;
    if (!read_plugin_activation_ini(config_path, plugin_states))
        return {};

    if (from_user_config) {
        // Existing user profiles may have been created before newer built-in
        // plugins existed. Merge newly-added default-active plugins into the
        // effective activation set, while keeping an explicit "plugin = 0" in
        // the user file as a real opt-out.
        std::map<std::string, bool> default_plugin_states;
        if (read_plugin_activation_ini(default_active_plugin_config_path(), default_plugin_states))
            for (const auto &[plugin_id, is_enabled] : default_plugin_states)
                if (is_enabled && plugin_states.find(plugin_id) == plugin_states.end())
                    plugin_states.emplace(plugin_id, true);
    }

    return enabled_plugin_ids(plugin_states);
}

void activate_plugins_from_ids(Orchestrator &orchestrator,
                               const std::vector<std::string> &plugin_ids,
                               bool from_user_config)
{
    orchestrator.clear_active_plugins();

    for (const std::string &plugin_id : plugin_ids) {
        if (orchestrator.set_plugin_active(plugin_id, true)) {
            BOOST_LOG_TRIVIAL(info) << "Activated plugin '" << plugin_id << "'.";
            continue;
        }

        if (from_user_config)
            BOOST_LOG_TRIVIAL(warning) << "Active plugin '" << plugin_id << "' is listed in "
                                       << ACTIVATED_PLUGINS_FILENAME << " but is not loaded.";
        else
            BOOST_LOG_TRIVIAL(trace) << "Default active plugin '" << plugin_id << "' is not loaded.";
    }
}

bool is_plugin_library_path(const boost::filesystem::path &path)
{
#ifdef _WIN32
    return boost::algorithm::iequals(path.extension().string(), ".dll");
#elif defined(__APPLE__)
    return path.extension() == ".dylib";
#else
    return path.extension() == ".so";
#endif
}

void load_plugin_library(const boost::filesystem::path &plugin_path, orchestrator_handle *orchestrator)
{
    const PluginLoadClock::time_point start = PluginLoadClock::now();
#ifdef _WIN32
    static std::vector<HMODULE> loaded_modules;
    HMODULE module = LoadLibraryW(plugin_path.wstring().c_str());
    if (module == NULL) {
        BOOST_LOG_TRIVIAL(warning) << "Cannot load plugin DLL '" << plugin_path.string()
                                   << "': error " << GetLastError();
        return;
    }

    FARPROC abi_farproc = GetProcAddress(module, "slic3r_plugin_abi_version");
    if (abi_farproc == NULL) {
        BOOST_LOG_TRIVIAL(warning) << "Plugin DLL '" << plugin_path.string()
                                   << "' does not export slic3r_plugin_abi_version(); skipping stale or incompatible plugin.";
        FreeLibrary(module);
        return;
    }

    PluginAbiVersionFn abi_version_fn = reinterpret_cast<PluginAbiVersionFn>(abi_farproc);
    const uint32_t abi_version = abi_version_fn();
    if (abi_version != SLIC3R_PLUGIN_ABI_VERSION) {
        BOOST_LOG_TRIVIAL(warning) << "Plugin DLL '" << plugin_path.string() << "' ABI mismatch: plugin ABI "
                                   << abi_version << ", host ABI " << SLIC3R_PLUGIN_ABI_VERSION
                                   << "; skipping incompatible plugin.";
        FreeLibrary(module);
        return;
    }

    FARPROC farproc = GetProcAddress(module, "register_plugin");
    if (farproc == NULL) {
        BOOST_LOG_TRIVIAL(warning) << "Plugin DLL '" << plugin_path.string()
                                   << "' does not export register_plugin().";
        FreeLibrary(module);
        return;
    }

    RegisterPluginFn register_plugin_fn = reinterpret_cast<RegisterPluginFn>(farproc);
    register_plugin_fn(orchestrator);
    loaded_modules.push_back(module);
    BOOST_LOG_TRIVIAL(debug) << "Loaded plugin DLL '" << plugin_path.string() << "' in "
                             << elapsed_ms(start).count() << " ms.";
#else
    static std::vector<void *> loaded_modules;
    void *module = dlopen(plugin_path.string().c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (module == nullptr) {
        BOOST_LOG_TRIVIAL(warning) << "Cannot load plugin library '" << plugin_path.string()
                                   << "': " << dlerror();
        return;
    }

    void *abi_symbol = dlsym(module, "slic3r_plugin_abi_version");
    if (abi_symbol == nullptr) {
        BOOST_LOG_TRIVIAL(warning) << "Plugin library '" << plugin_path.string()
                                   << "' does not export slic3r_plugin_abi_version(); skipping stale or incompatible plugin.";
        dlclose(module);
        return;
    }

    PluginAbiVersionFn abi_version_fn = reinterpret_cast<PluginAbiVersionFn>(abi_symbol);
    const uint32_t abi_version = abi_version_fn();
    if (abi_version != SLIC3R_PLUGIN_ABI_VERSION) {
        BOOST_LOG_TRIVIAL(warning) << "Plugin library '" << plugin_path.string() << "' ABI mismatch: plugin ABI "
                                   << abi_version << ", host ABI " << SLIC3R_PLUGIN_ABI_VERSION
                                   << "; skipping incompatible plugin.";
        dlclose(module);
        return;
    }

    void *symbol = dlsym(module, "register_plugin");
    if (symbol == nullptr) {
        BOOST_LOG_TRIVIAL(warning) << "Plugin library '" << plugin_path.string()
                                   << "' does not export register_plugin(): " << dlerror();
        dlclose(module);
        return;
    }

    RegisterPluginFn register_plugin_fn = reinterpret_cast<RegisterPluginFn>(symbol);
    register_plugin_fn(orchestrator);
    loaded_modules.push_back(module);
    BOOST_LOG_TRIVIAL(debug) << "Loaded plugin library '" << plugin_path.string() << "' in "
                             << elapsed_ms(start).count() << " ms.";
#endif
}

void load_plugins_from_repository(const boost::filesystem::path &repository, orchestrator_handle *orchestrator)
{
    if (!boost::filesystem::exists(repository)) {
        BOOST_LOG_TRIVIAL(trace) << "Plugin repository '" << repository.string() << "' does not exist.";
        return;
    }
    if (!boost::filesystem::is_directory(repository)) {
        BOOST_LOG_TRIVIAL(warning) << "Plugin repository path '" << repository.string() << "' is not a directory.";
        return;
    }

    for (boost::filesystem::directory_iterator it(repository), end; it != end; ++it) {
        const boost::filesystem::path plugin_path = it->path();
        if (boost::filesystem::is_regular_file(plugin_path) && is_plugin_library_path(plugin_path)) {
            BOOST_LOG_TRIVIAL(info) << "Loading plugin '" << plugin_path.string() << "'.";
            load_plugin_library(plugin_path, orchestrator);
        }
    }
}

void register_builtin_plugin(orchestrator_handle *orchestrator,
                             const char *plugin_name,
                             RegisterPluginFn register_plugin_fn)
{
    const PluginLoadClock::time_point start = PluginLoadClock::now();
    register_plugin_fn(orchestrator);
    BOOST_LOG_TRIVIAL(debug) << "Loaded built-in plugin '" << plugin_name << "' in "
                             << elapsed_ms(start).count() << " ms.";
}

void register_builtin_plugins(orchestrator_handle *orchestrator)
{
    register_builtin_plugin(orchestrator, "standard_layer_height_generator",
        slic3r_api::StandardLayerHeightGeneratorPlugin::register_standard_layer_height_generator_plugin);
    register_builtin_plugin(orchestrator, "slice_volume",
        slic3r_api::SliceVolumePlugin::register_slice_volume_plugin);
    register_builtin_plugin(orchestrator, "max_overhang_threshold",
        slic3r_api::MaxOverhangThresholdPlugin::register_max_overhang_threshold_plugin);
    register_builtin_plugin(orchestrator, "vase.multi_island_connector",
        slic3r_api::VaseMultiIslandConnectorPlugin::register_vase_multi_island_connector_plugin);
    register_builtin_plugin(orchestrator, "gcode.printing_plan_file_writer",
        slic3r_api::GCodeGeneration::PrintingPlanFileWriterPlugin::register_printing_plan_file_writer_plugin);
    register_builtin_plugin(orchestrator, "infill.generator.default",
        slic3r_api::Infill::DefaultInfillGeneratorPlugin::register_default_infill_generator_plugin);
    register_builtin_plugin(orchestrator, "legacy_infill_patterns",
        slic3r_api::Infill::LegacyInfillPatternsPlugin::register_legacy_infill_pattern_plugins);
    register_builtin_plugin(orchestrator, "infill.post_process.gap_fill",
        slic3r_api::Infill::PostInfillGapFillPlugin::register_post_infill_gap_fill_plugin);
    register_builtin_plugin(orchestrator, "ordering.default",
        slic3r_api::Ordering::DefaultOrderingPlugin::register_default_ordering_plugins);
    register_builtin_plugin(orchestrator, "perimeter.module.extra_perimeter_count",
        slic3r_api::Perimeter::ExtraPerimeterCountPlugin::register_extra_perimeter_count_plugin);
    register_builtin_plugin(orchestrator, "perimeter.module.extra_perimeter_below_area",
        slic3r_api::Perimeter::ExtraPerimeterBelowAreaPlugin::register_extra_perimeter_below_area_plugin);
    register_builtin_plugin(orchestrator, "perimeter.module.extra_perimeter_odd_layer",
        slic3r_api::Perimeter::ExtraPerimeterOddLayerPlugin::register_extra_perimeter_odd_layer_plugin);
    register_builtin_plugin(orchestrator, "perimeter.module.only_one_perimeter_first_layer",
        slic3r_api::Perimeter::OnlyOnePerimeterFirstLayerPlugin::register_only_one_perimeter_first_layer_plugin);
    register_builtin_plugin(orchestrator, "perimeter.module.only_one_perimeter_on_top",
        slic3r_api::Perimeter::OnlyOnePerimeterOnTopPlugin::register_only_one_perimeter_on_top_plugin);
    register_builtin_plugin(orchestrator, "perimeter.module.separate_hole_contour",
        slic3r_api::Perimeter::SeparateHoleContourPlugin::register_separate_hole_contour_plugin);
    register_builtin_plugin(orchestrator, "perimeter.module.remove_gap_fill_on_overhangs",
        slic3r_api::Perimeter::RemoveGapFillOnOverhangsPlugin::register_remove_gap_fill_on_overhangs_plugin);
    register_builtin_plugin(orchestrator, "perimeter.module.mark_first_loop",
        slic3r_api::Perimeter::MarkFirstLoopPlugin::register_mark_first_loop_plugin);
    register_builtin_plugin(orchestrator, "perimeter.post_process.extra_perimeters_on_overhangs",
        slic3r_api::Perimeter::ExtraPerimetersOnOverhangsPlugin::register_extra_perimeters_on_overhangs_plugin);
    register_builtin_plugin(orchestrator, "perimeter.post_process.detect_overhang",
        slic3r_api::Perimeter::DetectOverhangPlugin::register_detect_overhang_plugin);
    register_builtin_plugin(orchestrator, "perimeter.post_process.fuzzy_skin",
        slic3r_api::Perimeter::FuzzySkinPlugin::register_fuzzy_skin_plugin);
    register_builtin_plugin(orchestrator, "perimeter.generator.arachne",
        slic3r_api::Perimeter::ArachnePerimeterGeneratorPlugin::register_arachne_perimeter_generator_plugin);
    register_builtin_plugin(orchestrator, "perimeter.generator.classic",
        slic3r_api::Perimeter::ClassicPerimeterGeneratorPlugin::register_classic_perimeter_generator_plugin);
    register_builtin_plugin(orchestrator, "perimeter.generator.simple",
        slic3r_api::Perimeter::SimplePerimeterGeneratorPlugin::register_simple_perimeter_generator_plugin);
    register_builtin_plugin(orchestrator, "surface.initial_typed_surface_builder",
        slic3r_api::SurfaceGeneration::InitialTypedSurfaceBuilderPlugin::register_initial_typed_surface_builder_plugin);
    register_builtin_plugin(orchestrator, "surface.solid_shells",
        slic3r_api::SurfaceGeneration::SolidShellsPlugin::register_solid_shells_plugin);
    register_builtin_plugin(orchestrator, "surface.top_surface_expansion",
        slic3r_api::SurfaceGeneration::TopSurfaceExpansionPlugin::register_top_surface_expansion_plugin);
    register_builtin_plugin(orchestrator, "surface.clean_infill_surfaces",
        slic3r_api::SurfaceGeneration::CleanInfillSurfacesPlugin::register_clean_infill_surfaces_plugin);
    register_builtin_plugin(orchestrator, "surface.infill_region_compatibility_splitter",
        slic3r_api::SurfaceGeneration::InfillRegionCompatibilitySplitterPlugin::
            register_infill_region_compatibility_splitter_plugin);
    register_builtin_plugin(orchestrator, "support_demand_bridge_removal",
        slic3r_api::Support::SupportDemandBridgeRemovalPlugin::register_support_demand_bridge_removal_plugin);
}

std::vector<std::pair<std::string, std::string>> active_infill_pattern_choices(Orchestrator &orchestrator)
{
    std::vector<std::pair<std::string, std::string>> choices;
    std::set<std::string> seen_ids;

    for (const Plugin *plugin : orchestrator.get_active_plugins_for_step(INFILL_PATTERN)) {
        Slic3r::InfillPattern typed_value = Slic3r::ipCount;
        if (!Slic3r::ConfigOptionEnum<Slic3r::InfillPattern>::from_string(plugin->get_id(), typed_value)) {
            BOOST_LOG_TRIVIAL(warning)
                << "Active infill pattern plugin '" << plugin->get_id()
                << "' cannot be exposed yet because fill pattern options still use the static InfillPattern enum.";
            continue;
        }

        if (seen_ids.insert(plugin->get_id()).second)
            choices.emplace_back(plugin->get_id(), plugin->get_name());
    }

    return choices;
}

void apply_infill_pattern_choices_to_option(ConfigOptionDef &def,
                                            const std::vector<std::pair<std::string, std::string>> &choices,
                                            const std::string &default_id)
{
    // The GUI list is dynamic, but the stored option is still
    // ConfigOptionEnum<InfillPattern>. set_enum<InfillPattern>() therefore
    // keeps the existing string-to-enum map while restricting the visible
    // choices to active INFILL_PATTERN plugins.
    def.set_enum<InfillPattern>(choices);

    InfillPattern default_pattern = ipRectilinear;
    if (!ConfigOptionEnum<InfillPattern>::from_string(default_id, default_pattern))
        ConfigOptionEnum<InfillPattern>::from_string(choices.front().first, default_pattern);
    def.set_default_value(new ConfigOptionEnum<InfillPattern>(default_pattern));
}

void register_infill_pattern_config_choices(Orchestrator &orchestrator)
{
    const std::vector<std::pair<std::string, std::string>> choices =
        active_infill_pattern_choices(orchestrator);
    if (choices.empty())
        return;

    ConfigDef &definition = PrintConfigDef::instance_mutable();
    const char *const option_keys[] = {
        "fill_pattern",
        "top_fill_pattern",
        "bottom_fill_pattern",
        "solid_fill_pattern",
        "bridge_fill_pattern"
    };

    for (const char *option_key : option_keys) {
        t_optiondef_map::iterator option = definition.options.find(option_key);
        if (option != definition.options.end())
            apply_infill_pattern_choices_to_option(option->second, choices, "rectilinear");
    }
}

void add_exclusive_step_used_setting_rules(Orchestrator &orchestrator,
                                           const Steps::StepExclusiveGroup &group,
                                           const std::vector<Plugin *> &plugins)
{
    // An exclusive group exposes one enum selector whose values are plugin ids.
    // Settings declared as "used" by a plugin should only be editable when
    // that plugin is the selected implementation. These generated rules keep
    // old plugin-specific options visible in the preset but avoid presenting
    // inactive implementation details as active controls.
    for (size_t plugin_idx = 0; plugin_idx < plugins.size(); ++plugin_idx) {
        const Plugin *plugin = plugins[plugin_idx];
        for (const Plugin::UsedConfigKey &setting : plugin->get_used_config_keys()) {
            raw_gui_rule rule = raw_gui_rule_init();
            rule.action = RAW_GUI_RULE_ACTION_ENABLE_ANY;
            rule.condition = RAW_GUI_RULE_CONDITION_INT_EQUALS;
            rule.target_key = setting.key.c_str();
            rule.condition_key = group.option_def.opt_key;
            rule.condition_int_value = int32_t(plugin_idx);
            orchestrator.add_gui_rule(&rule);
        }
    }
}

void register_exclusive_step_group_options_impl(Orchestrator &orchestrator)
{
    for (Steps::StepExclusivePluginGroup plugin_group : Steps::active_exclusive_plugin_groups(orchestrator)) {
        // The selector must use the stable plugin id as the stored enum value,
        // while showing the user-facing plugin name in the GUI. This lets a
        // preset survive a label change without changing its serialized value.
        std::vector<std::pair<std::string, std::string>> plugin_ids_and_labels;
        plugin_ids_and_labels.reserve(plugin_group.plugins.size());
        for (const Plugin *plugin : plugin_group.plugins)
            plugin_ids_and_labels.emplace_back(plugin->get_id(), plugin->get_name());

        Steps::StepExclusiveGroup &group = plugin_group.group;
        group.set_enum_plugins(plugin_ids_and_labels);
        orchestrator.create_new_print_config(&group.option_def);
        add_exclusive_step_used_setting_rules(orchestrator, group, plugin_group.plugins);
        for (const raw_gui_rule &rule : group.gui_activation_rules)
            orchestrator.add_gui_rule(&rule);
    }
}

bool has_ui_fragment(Orchestrator &orchestrator,
                     const std::string &target_file,
                     const std::string &fragment_id)
{
    const std::vector<Orchestrator::PluginUiFragment> fragments =
        orchestrator.ui_fragments_for_file(target_file);
    for (const Orchestrator::PluginUiFragment &fragment : fragments)
        if (fragment.fragment_id == fragment_id)
            return true;
    return false;
}

void register_exclusive_step_group_ui_fragments_impl(Orchestrator &orchestrator)
{
    for (Steps::StepExclusivePluginGroup plugin_group : Steps::active_exclusive_plugin_groups(orchestrator)) {
        // The fragment id is the group id, not the generated option key. This
        // lets several plugins in the same group provide the same placement
        // fragment while keeping de-duplication stable and independent from the
        // generated setting name.
        const Steps::StepExclusiveGroup &group = plugin_group.group;
        // A feature plugin may place the selector next to its own controls.
        // The generic Notes-page selector is only a fallback for groups that
        // did not already publish an explicit placement fragment.
        if (has_ui_fragment(orchestrator, "print.ui", group.group_id))
            continue;
        orchestrator.add_ui_fragment("print.ui", group.group_id.c_str(), group.ui_fragment.c_str(), 0);
    }
}

} // namespace

void register_exclusive_step_group_options(Orchestrator &orchestrator)
{
    register_exclusive_step_group_options_impl(orchestrator);
}

void register_exclusive_step_group_ui_fragments(Orchestrator &orchestrator)
{
    register_exclusive_step_group_ui_fragments_impl(orchestrator);
}

void register_exclusive_step_groups(Orchestrator &orchestrator)
{
    register_exclusive_step_group_options_impl(orchestrator);
    register_exclusive_step_group_ui_fragments_impl(orchestrator);
}

void load_plugins()
{
    const PluginLoadClock::time_point start = PluginLoadClock::now();
    Orchestrator &orchestrator = Orchestrator::instance();
    orchestrator_handle *orchestrator_handle_ptr = reinterpret_cast<orchestrator_handle *>(&orchestrator);

    register_builtin_plugins(orchestrator_handle_ptr);
    load_plugins_from_repository(Slic3r::install_path() / "plugins", orchestrator_handle_ptr);

    // Loading and activation are intentionally separate. A disabled plugin is
    // still registered so the configuration dialog can show it, but it cannot
    // publish settings or run until its id appears in activated.ini.
    bool active_plugins_loaded_from_user_config = false;
    const boost::filesystem::path config_dir = has_data_dir() ? boost::filesystem::path(data_dir()) :
                                                                boost::filesystem::path();
    const std::vector<std::string> active_plugin_ids =
        read_active_plugin_ids(config_dir, active_plugins_loaded_from_user_config);
    BOOST_LOG_TRIVIAL(info) << "Loaded " << active_plugin_ids.size() << " active plugin id(s) from "
                            << (active_plugins_loaded_from_user_config ? active_plugin_config_path(config_dir).string() :
                                default_active_plugin_config_path().string()) << ".";
    activate_plugins_from_ids(orchestrator, active_plugin_ids, active_plugins_loaded_from_user_config);
    register_infill_pattern_config_choices(orchestrator);
    register_exclusive_step_group_options_impl(orchestrator);

    orchestrator.initialize_plugins();
    register_exclusive_step_group_ui_fragments_impl(orchestrator);
    //note: --loglevel 4 is read too late for this log, use $env:SLIC3R_LOGLEVEL = "4" (or SLIC3R_LOGLEVEL=4 in visual studio environement line)
    BOOST_LOG_TRIVIAL(debug) << "Loaded the entire plugin library in "
                             << elapsed_ms(start).count() << " ms.";
}

} // namespace Slic3r
