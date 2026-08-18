///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// PluginLoader prepares requested package changes, loads native installed
// packages, registers built-in plugins, and finally applies activation choices.
// Native packages are loaded here under a package registration scope. Pure
// Python sibling packages are intentionally left to PythonPluginLoader, which
// constructs the same scope when it registers their plugin instances.

#include "PluginLoader.hpp"
#include "PluginRepository.hpp"

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
#include <iterator>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/fstream.hpp>

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/plugin/c/slic3r_plugin.h"
#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/FFFPrintConfig.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Plugins/GCode/LegacyGCodeGenerator.hpp"
#include "libslic3r/Plugins/GCode/PrintingPlanFileWriter.hpp"
#include "libslic3r/Plugins/Infill/DefaultInfillGenerator.hpp"
#include "libslic3r/Plugins/Infill/LegacyInfillPatterns.hpp"
#include "libslic3r/Plugins/Infill/PostInfillGapFill.hpp"
#include "libslic3r/Plugins/LayerExtrusionEdit/DefaultAcceleration.hpp"
#include "libslic3r/Plugins/LayerExtrusionEdit/DefaultSpeed.hpp"
#include "libslic3r/Plugins/MaxOverhangThreshold.hpp"
#include "libslic3r/Plugins/Ordering/DefaultOrdering.hpp"
#include "libslic3r/Plugins/SkirtBrim/DefaultBrimSkirtTrim.hpp"
#include "libslic3r/Plugins/SkirtBrim/DefaultBrimGenerator.hpp"
#include "libslic3r/Plugins/SkirtBrim/DefaultSkirtGenerator.hpp"
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

// Startup runs before the GUI exists. Preserve a recoverable activation error
// until GUI_App can present it after creating its main frame.
std::optional<PluginActivationStartupError> g_plugin_activation_startup_error;

// Read the package descriptor before loading executable code. The loader uses
// its internal flag to accept runtime-support packages that intentionally
// register no plugin instance of their own.
bool read_plugin_package_description(const boost::filesystem::path &manifest,
                                     RepositoryDescription &description,
                                     std::string &error_message);

// Store one issue in the process report and mirror the same detail to logs.
void report_plugin_package_issue(Orchestrator &orchestrator,
                                 const std::string &package_id,
                                 PluginPackageLoadIssue issue);

// A pure Python package is loaded by the Python runtime package rather than by
// the native dynamic-library branch.
bool is_python_plugin_package(const boost::filesystem::path &package_root);

#ifdef _WIN32
// Convert a Windows loader code to the text shown by the operating system.
std::string windows_error_message(DWORD error_code);
#endif

std::chrono::milliseconds elapsed_ms(const PluginLoadClock::time_point &start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(PluginLoadClock::now() - start);
}

bool read_plugin_package_description(const boost::filesystem::path &manifest,
                                     RepositoryDescription &description,
                                     std::string &error_message)
{
    boost::nowide::ifstream stream(manifest.string(), std::ios::in | std::ios::binary);
    if (!stream) {
        error_message = "Cannot read package description '" + manifest.string() + "'.";
        return false;
    }
    const std::string contents((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return parse_repository_description(contents, RepositoryPackageType::Plugin, description, error_message);
}

void report_plugin_package_issue(Orchestrator &orchestrator,
                                 const std::string &package_id,
                                 PluginPackageLoadIssue issue)
{
    BOOST_LOG_TRIVIAL(warning) << "Plugin package '" << package_id << "' failed to load: " << issue.detail;
    orchestrator.report_plugin_package_load_issue(package_id, std::move(issue));
}

bool is_python_plugin_package(const boost::filesystem::path &package_root)
{
    const std::string package_id = package_root.filename().string();
    if (boost::filesystem::is_regular_file(package_root / "plugin.py") ||
        boost::filesystem::is_regular_file(package_root / (package_id + ".py")))
        return true;

    size_t python_file_count = 0;
    for (boost::filesystem::directory_iterator it(package_root), end; it != end; ++it)
        if (boost::filesystem::is_regular_file(it->path()) && it->path().extension() == ".py")
            ++python_file_count;
    return python_file_count == 1;
}

#ifdef _WIN32
std::string windows_error_message(DWORD error_code)
{
    wchar_t *message = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = FormatMessageW(flags, nullptr, error_code, 0,
                                        reinterpret_cast<wchar_t *>(&message), 0, nullptr);
    if (length == 0 || message == nullptr)
        return "Windows error " + std::to_string(error_code) + ".";

    std::wstring text(message, length);
    LocalFree(message);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' '))
        text.pop_back();
    return boost::nowide::narrow(text);
}
#endif

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
                                       << "activated.ini but is not loaded.";
        else
            BOOST_LOG_TRIVIAL(trace) << "Default active plugin '" << plugin_id << "' is not loaded.";
    }
}

const char *plugin_package_library_filename()
{
#ifdef _WIN32
    return "plugin.dll";
#elif defined(__APPLE__)
    return "plugin.dylib";
#else
    return "plugin.so";
#endif
}

void load_plugin_library(const boost::filesystem::path &plugin_path,
                         const boost::filesystem::path &package_root,
                         const std::string &package_id,
                         bool internal_package,
                         orchestrator_handle *orchestrator)
{
    const PluginLoadClock::time_point start = PluginLoadClock::now();
    Orchestrator &host = *reinterpret_cast<Orchestrator *>(orchestrator);
    host.begin_plugin_package_load(package_id, package_root.string(), internal_package);
#ifdef _WIN32
    static std::vector<HMODULE> loaded_modules;
    HMODULE module = LoadLibraryW(plugin_path.wstring().c_str());
    if (module == NULL) {
        const DWORD error_code = GetLastError();
        PluginPackageLoadIssue issue;
        issue.code = error_code == ERROR_MOD_NOT_FOUND || error_code == ERROR_DLL_NOT_FOUND ?
                         PluginPackageLoadErrorCode::DependencyMissing :
                         PluginPackageLoadErrorCode::LibraryOpenFailed;
        issue.system_error = uint32_t(error_code);
        issue.detail = "Cannot load '" + plugin_path.string() + "': " + windows_error_message(error_code);
        report_plugin_package_issue(host, package_id, std::move(issue));
        host.finish_plugin_package_load(package_id);
        return;
    }

    FARPROC abi_farproc = GetProcAddress(module, "slic3r_plugin_abi_version");
    if (abi_farproc == NULL) {
        PluginPackageLoadIssue issue;
        issue.code = PluginPackageLoadErrorCode::MissingAbiExport;
        issue.detail = "The library does not export slic3r_plugin_abi_version().";
        report_plugin_package_issue(host, package_id, std::move(issue));
        FreeLibrary(module);
        host.finish_plugin_package_load(package_id);
        return;
    }

    PluginAbiVersionFn abi_version_fn = reinterpret_cast<PluginAbiVersionFn>(abi_farproc);
    const uint32_t abi_version = abi_version_fn();
    if (abi_version != SLIC3R_PLUGIN_ABI_VERSION) {
        PluginPackageLoadIssue issue;
        issue.code = PluginPackageLoadErrorCode::AbiMismatch;
        issue.plugin_abi = abi_version;
        issue.host_abi = SLIC3R_PLUGIN_ABI_VERSION;
        issue.detail = "Plugin API version " + std::to_string(abi_version) +
                       " does not match host API version " + std::to_string(SLIC3R_PLUGIN_ABI_VERSION) + ".";
        report_plugin_package_issue(host, package_id, std::move(issue));
        FreeLibrary(module);
        host.finish_plugin_package_load(package_id);
        return;
    }

    FARPROC farproc = GetProcAddress(module, "register_plugin");
    if (farproc == NULL) {
        PluginPackageLoadIssue issue;
        issue.code = PluginPackageLoadErrorCode::MissingRegistrationExport;
        issue.detail = "The library does not export register_plugin().";
        report_plugin_package_issue(host, package_id, std::move(issue));
        FreeLibrary(module);
        host.finish_plugin_package_load(package_id);
        return;
    }

    RegisterPluginFn register_plugin_fn = reinterpret_cast<RegisterPluginFn>(farproc);
    try {
        Orchestrator::PluginRegistrationScope registration_scope(
            host.plugin_registration_scope(package_root.string(), true));
        register_plugin_fn(orchestrator);
    } catch (const std::exception &error) {
        PluginPackageLoadIssue issue;
        issue.code = PluginPackageLoadErrorCode::RegistrationFailed;
        issue.detail = error.what();
        report_plugin_package_issue(host, package_id, std::move(issue));
    } catch (...) {
        PluginPackageLoadIssue issue;
        issue.code = PluginPackageLoadErrorCode::RegistrationFailed;
        issue.detail = "register_plugin() threw an unknown exception.";
        report_plugin_package_issue(host, package_id, std::move(issue));
    }
    host.finish_plugin_package_load(package_id);
    const PluginPackageLoadReport *report = host.plugin_package_load_report(package_id);
    if (!internal_package && report != nullptr && report->registered_plugin_ids.empty())
        FreeLibrary(module);
    else
        loaded_modules.push_back(module);
    BOOST_LOG_TRIVIAL(debug) << "Loaded plugin DLL '" << plugin_path.string() << "' in "
                             << elapsed_ms(start).count() << " ms.";
#else
    static std::vector<void *> loaded_modules;
    void *module = dlopen(plugin_path.string().c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (module == nullptr) {
        const char *loader_error = dlerror();
        const std::string detail = loader_error == nullptr ? "Unknown dynamic loader error." : loader_error;
        PluginPackageLoadIssue issue;
        issue.code = detail.find("not found") != std::string::npos ||
                     detail.find("No such file") != std::string::npos ?
                         PluginPackageLoadErrorCode::DependencyMissing :
                         PluginPackageLoadErrorCode::LibraryOpenFailed;
        issue.detail = "Cannot load '" + plugin_path.string() + "': " + detail;
        report_plugin_package_issue(host, package_id, std::move(issue));
        host.finish_plugin_package_load(package_id);
        return;
    }

    void *abi_symbol = dlsym(module, "slic3r_plugin_abi_version");
    if (abi_symbol == nullptr) {
        PluginPackageLoadIssue issue;
        issue.code = PluginPackageLoadErrorCode::MissingAbiExport;
        issue.detail = "The library does not export slic3r_plugin_abi_version().";
        report_plugin_package_issue(host, package_id, std::move(issue));
        dlclose(module);
        host.finish_plugin_package_load(package_id);
        return;
    }

    PluginAbiVersionFn abi_version_fn = reinterpret_cast<PluginAbiVersionFn>(abi_symbol);
    const uint32_t abi_version = abi_version_fn();
    if (abi_version != SLIC3R_PLUGIN_ABI_VERSION) {
        PluginPackageLoadIssue issue;
        issue.code = PluginPackageLoadErrorCode::AbiMismatch;
        issue.plugin_abi = abi_version;
        issue.host_abi = SLIC3R_PLUGIN_ABI_VERSION;
        issue.detail = "Plugin API version " + std::to_string(abi_version) +
                       " does not match host API version " + std::to_string(SLIC3R_PLUGIN_ABI_VERSION) + ".";
        report_plugin_package_issue(host, package_id, std::move(issue));
        dlclose(module);
        host.finish_plugin_package_load(package_id);
        return;
    }

    void *symbol = dlsym(module, "register_plugin");
    if (symbol == nullptr) {
        const char *symbol_error = dlerror();
        PluginPackageLoadIssue issue;
        issue.code = PluginPackageLoadErrorCode::MissingRegistrationExport;
        issue.detail = "The library does not export register_plugin()";
        if (symbol_error != nullptr)
            issue.detail += ": " + std::string(symbol_error);
        report_plugin_package_issue(host, package_id, std::move(issue));
        dlclose(module);
        host.finish_plugin_package_load(package_id);
        return;
    }

    RegisterPluginFn register_plugin_fn = reinterpret_cast<RegisterPluginFn>(symbol);
    try {
        Orchestrator::PluginRegistrationScope registration_scope(
            host.plugin_registration_scope(package_root.string(), true));
        register_plugin_fn(orchestrator);
    } catch (const std::exception &error) {
        PluginPackageLoadIssue issue;
        issue.code = PluginPackageLoadErrorCode::RegistrationFailed;
        issue.detail = error.what();
        report_plugin_package_issue(host, package_id, std::move(issue));
    } catch (...) {
        PluginPackageLoadIssue issue;
        issue.code = PluginPackageLoadErrorCode::RegistrationFailed;
        issue.detail = "register_plugin() threw an unknown exception.";
        report_plugin_package_issue(host, package_id, std::move(issue));
    }
    host.finish_plugin_package_load(package_id);
    const PluginPackageLoadReport *report = host.plugin_package_load_report(package_id);
    if (!internal_package && report != nullptr && report->registered_plugin_ids.empty())
        dlclose(module);
    else
        loaded_modules.push_back(module);
    BOOST_LOG_TRIVIAL(debug) << "Loaded plugin library '" << plugin_path.string() << "' in "
                             << elapsed_ms(start).count() << " ms.";
#endif
}

void load_plugins_from_repository_impl(const boost::filesystem::path &repository, orchestrator_handle *orchestrator)
{
    if (!boost::filesystem::exists(repository)) {
        BOOST_LOG_TRIVIAL(trace) << "Plugin repository '" << repository.string() << "' does not exist.";
        return;
    }
    if (!boost::filesystem::is_directory(repository)) {
        BOOST_LOG_TRIVIAL(warning) << "Plugin repository path '" << repository.string() << "' is not a directory.";
        return;
    }

    Orchestrator &host = *reinterpret_cast<Orchestrator *>(orchestrator);
    for (boost::filesystem::directory_iterator it(repository), end; it != end; ++it) {
        const boost::filesystem::path plugin_path = it->path();
        if (boost::filesystem::is_directory(plugin_path)) {
            const std::string package_name = plugin_path.filename().string();
            // Interrupted install transactions keep their staging and backup
            // folders beside the real package. Their leading dot makes them
            // ineligible for loading on the next launch.
            if (package_name.empty() || package_name.front() == '.')
                continue;

            const boost::filesystem::path package_library = plugin_path / plugin_package_library_filename();
            const boost::filesystem::path package_manifest = plugin_path / "description.ini";
            const boost::filesystem::path package_version = plugin_path / "version.ini";
            if (!boost::filesystem::is_regular_file(package_manifest) ||
                !boost::filesystem::is_regular_file(package_version)) {
                host.begin_plugin_package_load(package_name, plugin_path.string());
                PluginPackageLoadIssue issue;
                issue.code = PluginPackageLoadErrorCode::InvalidPackage;
                issue.detail = "The installed package must contain description.ini and version.ini.";
                report_plugin_package_issue(host, package_name, std::move(issue));
                host.finish_plugin_package_load(package_name);
                continue;
            }

            RepositoryDescription description;
            std::string description_error;
            if (!read_plugin_package_description(package_manifest, description, description_error) ||
                description.id != package_name) {
                host.begin_plugin_package_load(package_name, plugin_path.string());
                PluginPackageLoadIssue issue;
                issue.code = PluginPackageLoadErrorCode::InvalidPackage;
                issue.detail = description_error.empty() ?
                    "description.ini does not match the installed package directory." : description_error;
                report_plugin_package_issue(host, package_name, std::move(issue));
                host.finish_plugin_package_load(package_name);
                continue;
            }

            if (boost::filesystem::is_regular_file(package_library)) {
                BOOST_LOG_TRIVIAL(info) << "Loading plugin package '" << plugin_path.string() << "'.";
                load_plugin_library(package_library, plugin_path, package_name,
                                    description.is_internal, orchestrator);
            } else if (!is_python_plugin_package(plugin_path)) {
                host.begin_plugin_package_load(package_name, plugin_path.string());
                PluginPackageLoadIssue issue;
                issue.code = PluginPackageLoadErrorCode::InvalidPackage;
                issue.detail = "The installed package contains neither a native plugin library nor a Python entry point.";
                report_plugin_package_issue(host, package_name, std::move(issue));
                host.finish_plugin_package_load(package_name);
            }
        }
    }

    // The Python runtime reports each package it attempted. Anything still
    // absent after native loading could not reach that runtime at all.
    const PluginPackageLoadReport *python_runtime = host.plugin_package_load_report("python");
    for (boost::filesystem::directory_iterator it(repository), end; it != end; ++it) {
        const boost::filesystem::path package_root = it->path();
        if (!boost::filesystem::is_directory(package_root) ||
            boost::filesystem::is_regular_file(package_root / plugin_package_library_filename()) ||
            !is_python_plugin_package(package_root))
            continue;
        const std::string package_id = package_root.filename().string();
        if (host.plugin_package_load_report(package_id) != nullptr)
            continue;
        host.begin_plugin_package_load(package_id, package_root.string());
        PluginPackageLoadIssue issue;
        if (python_runtime == nullptr || python_runtime->state == PluginPackageLoadState::Failed) {
            issue.code = PluginPackageLoadErrorCode::PythonRuntimeUnavailable;
            issue.detail = "The Python plugin runtime package is not installed or did not load.";
        } else {
            issue.code = PluginPackageLoadErrorCode::PythonRegistrationFailed;
            issue.detail = "The Python runtime did not load this package.";
        }
        report_plugin_package_issue(host, package_id, std::move(issue));
        host.finish_plugin_package_load(package_id);
    }
}

void register_builtin_plugin(orchestrator_handle *orchestrator,
                             const char *plugin_name,
                             RegisterPluginFn register_plugin_fn)
{
    const PluginLoadClock::time_point start = PluginLoadClock::now();
    Orchestrator::PluginRegistrationScope registration_scope(
        reinterpret_cast<Orchestrator *>(orchestrator)->plugin_registration_scope(std::string(), false));
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
    register_builtin_plugin(orchestrator, "gcode.legacy",
        slic3r_api::GCodeGeneration::LegacyGCodeGeneratorPlugin::register_legacy_gcode_generator_plugin);
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
    register_builtin_plugin(orchestrator, "layer_extrusion_edit.speed.default",
        slic3r_api::LayerExtrusionEdit::DefaultSpeedPlugin::register_default_speed_plugin);
    register_builtin_plugin(orchestrator, "layer_extrusion_edit.acceleration.default",
        slic3r_api::LayerExtrusionEdit::DefaultAccelerationPlugin::register_default_acceleration_plugin);
    register_builtin_plugin(orchestrator, "skirt_brim.brim.default",
        slic3r_api::SkirtBrim::DefaultBrimGeneratorPlugin::register_default_brim_generator_plugin);
    register_builtin_plugin(orchestrator, "skirt_brim.skirt.default",
        slic3r_api::SkirtBrim::DefaultSkirtGeneratorPlugin::register_default_skirt_generator_plugin);
    register_builtin_plugin(orchestrator, "skirt_brim.brim_skirt_trim.default",
        slic3r_api::SkirtBrim::DefaultBrimSkirtTrimPlugin::register_default_brim_skirt_trim_plugin);
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

bool has_ui_fragment_in_layout(Orchestrator &orchestrator, const std::string &fragment_id)
{
    /*
    Exclusive group selectors have one logical fragment id. A plugin may place
    that selector in a file that better matches the setting category, for
    example printer_fff.ui for firmware/G-code choices. The generic print.ui
    fallback must then stay silent, otherwise the same setting appears twice in
    the GUI.
    */
    static const char *const k_layout_files[] = {
        "print.ui",
        "printer_fff.ui",
        "filament.ui",
        "printer_sla.ui"
    };

    for (const char *target_file : k_layout_files)
        if (has_ui_fragment(orchestrator, target_file, fragment_id))
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
        if (has_ui_fragment_in_layout(orchestrator, group.group_id))
            continue;
        orchestrator.add_ui_fragment("print.ui", group.group_id.c_str(), group.ui_fragment.c_str(), 0);
    }
}

} // namespace

bool resolve_plugin_startup_activation_config(const boost::filesystem::path &data_directory,
                                              PluginActivationConfig &config,
                                              PluginActivationConfigSource &source,
                                              std::string &error_message)
{
    g_plugin_activation_startup_error.reset();
    config = {};
    source = PluginActivationConfigSource::DefaultsFallback;

    // Tools without a writable data directory intentionally consume the
    // resource configuration directly and have no user file to repair.
    if (data_directory.empty()) {
        bool ignored_from_user_config = false;
        return ensure_plugin_activation_config(data_directory, config,
                                               ignored_from_user_config, error_message);
    }

    std::string user_error;
    const boost::filesystem::path user_config_path = plugin_activation_config_path(data_directory);
    try {
        if (!boost::filesystem::exists(user_config_path)) {
            bool ignored_from_user_config = false;
            if (ensure_plugin_activation_config(data_directory, config,
                                                ignored_from_user_config, user_error)) {
                source = PluginActivationConfigSource::UserValid;
                error_message.clear();
                return true;
            }
        } else {
            PluginActivationConfigReadResult read_result =
                read_plugin_activation_config_tolerant(user_config_path);
            if (read_result.status == PluginActivationConfigStatus::Valid) {
                config = std::move(read_result.config);
                source = PluginActivationConfigSource::UserValid;
                error_message.clear();
                return true;
            }
            if (read_result.status == PluginActivationConfigStatus::PartiallyValid) {
                config = std::move(read_result.config);
                source = PluginActivationConfigSource::UserSanitized;

                PluginActivationStartupError startup_error;
                startup_error.config_path = user_config_path.string();
                startup_error.detail = std::move(read_result.error_message);
                startup_error.source = source;
                startup_error.issues = std::move(read_result.issues);
                startup_error.removed_packages = std::move(read_result.rejected_packages);
                startup_error.sanitized_config = config;
                g_plugin_activation_startup_error = std::move(startup_error);
                error_message.clear();
                return true;
            }
            user_error = std::move(read_result.error_message);
        }
    } catch (const boost::filesystem::filesystem_error &error) {
        user_error = "Cannot inspect plugin configuration '" + user_config_path.string() + "': " + error.what();
    }

    PluginActivationConfig default_config;
    bool ignored_from_user_config = false;
    std::string default_error;
    if (!ensure_plugin_activation_config(boost::filesystem::path(), default_config,
                                         ignored_from_user_config, default_error)) {
        config = {};
        source = PluginActivationConfigSource::DefaultsFallback;
        error_message = user_error + " Default plugin activations are also unavailable: " + default_error;
        return false;
    }

    // Copy only [activated]. Omitting the default package sections makes it
    // impossible for this fallback value to become a desired installation
    // state, even if a future caller forgets to inspect the source flag.
    config = {};
    config.activated = std::move(default_config.activated);
    source = PluginActivationConfigSource::DefaultsFallback;
    PluginActivationStartupError startup_error;
    startup_error.config_path = user_config_path.string();
    startup_error.detail = std::move(user_error);
    startup_error.source = source;
    startup_error.default_activations_used = true;
    startup_error.package_changes_skipped = true;
    g_plugin_activation_startup_error = std::move(startup_error);
    error_message.clear();
    return true;
}

std::optional<PluginActivationStartupError> take_plugin_activation_startup_error()
{
    std::optional<PluginActivationStartupError> error = std::move(g_plugin_activation_startup_error);
    g_plugin_activation_startup_error.reset();
    return error;
}

void register_exclusive_step_group_options(Orchestrator &orchestrator)
{
    register_exclusive_step_group_options_impl(orchestrator);
}

void load_plugin_packages_from_repository(const boost::filesystem::path &repository,
                                          Orchestrator &orchestrator)
{
    load_plugins_from_repository_impl(repository,
        reinterpret_cast<orchestrator_handle *>(&orchestrator));
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
    orchestrator.clear_plugin_package_load_reports();
    orchestrator_handle *orchestrator_handle_ptr = reinterpret_cast<orchestrator_handle *>(&orchestrator);

    // Package installation happens before any external DLL is loaded. This
    // keeps a running process from replacing a library that Windows or the
    // dynamic linker may still hold open.
    PluginActivationConfigSource activation_config_source = PluginActivationConfigSource::DefaultsFallback;
    const boost::filesystem::path config_dir = has_data_dir() ? boost::filesystem::path(data_dir()) :
                                                                boost::filesystem::path();
    PluginActivationConfig plugin_config;
    std::string plugin_config_error;
    if (!resolve_plugin_startup_activation_config(config_dir, plugin_config,
                                                  activation_config_source,
                                                  plugin_config_error))
        throw std::runtime_error(plugin_config_error);

    const bool user_configuration_available =
        activation_config_source != PluginActivationConfigSource::DefaultsFallback;
    const bool automatic_configuration_writes_allowed =
        activation_config_source == PluginActivationConfigSource::UserValid;

    if (const std::optional<PluginActivationStartupError> startup_error =
            g_plugin_activation_startup_error; startup_error.has_value()) {
        if (startup_error->source == PluginActivationConfigSource::UserSanitized) {
            BOOST_LOG_TRIVIAL(error) << startup_error->detail << " Valid plugin configuration entries are used "
                                     << "for this session. Rejected packages are removed from the desired state; "
                                     << "the user file remains unchanged.";
        } else {
            BOOST_LOG_TRIVIAL(error) << startup_error->detail << " Using default plugin activations for this "
                                     << "session. The user configuration remains unchanged and package changes "
                                     << "were not applied.";
        }
    }

    if (!config_dir.empty() && user_configuration_available) {
        // A profile created before a later built-in plugin existed should pick
        // up the new default unless it explicitly keeps that id disabled.
        PluginActivationConfig default_plugin_config;
        bool ignored_from_user_config = false;

        // Split the former combined editor before merging defaults. Copying its
        // false state now prevents the default merge from re-enabling either
        // independent successor in a profile that disabled the old plugin.
        bool activation_config_changed = migrate_plugin_activation_id(
            plugin_config,
            "layer_extrusion_edit.speed_acceleration.default",
            {"layer_extrusion_edit.speed.default", "layer_extrusion_edit.acceleration.default"});
        if (ensure_plugin_activation_config(boost::filesystem::path(), default_plugin_config,
                                            ignored_from_user_config, plugin_config_error)) {
            for (const auto &[plugin_id, enabled] : default_plugin_config.activated)
                if (enabled && plugin_config.activated.find(plugin_id) == plugin_config.activated.end()) {
                    plugin_config.activated.emplace(plugin_id, true);
                    activation_config_changed = true;
                }
            for (const auto &[plugin_id, package_id] : default_plugin_config.plugin_packages)
                if (plugin_config.plugin_packages.emplace(plugin_id, package_id).second)
                    activation_config_changed = true;
        } else {
            BOOST_LOG_TRIVIAL(warning) << plugin_config_error;
        }
        const bool packages_prepared = prepare_plugin_bundle_cache(
            boost::filesystem::path(resources_dir()), config_dir, plugin_config, plugin_config_error);
        const bool packages_applied = packages_prepared && reconcile_installed_plugin_packages(
            config_dir, plugin_config, plugin_config_error);
        if (!packages_applied)
            BOOST_LOG_TRIVIAL(warning) << plugin_config_error;
        if (activation_config_changed && automatic_configuration_writes_allowed &&
            !write_plugin_activation_config(plugin_activation_config_path(config_dir),
                                            plugin_config, plugin_config_error))
            BOOST_LOG_TRIVIAL(warning) << plugin_config_error;
    }

    register_builtin_plugins(orchestrator_handle_ptr);
    if (!config_dir.empty())
        load_plugin_packages_from_repository(config_dir / "plugins", orchestrator);

    // A successfully registered external plugin is the authoritative source
    // for its provider association. Backfill old profiles when that relation
    // was not yet persisted, without replacing an explicit existing mapping.
    bool learned_package_association = false;
    if (user_configuration_available) {
        for (const Plugin *plugin : orchestrator.registered_plugins()) {
            if (plugin->get_package_root().empty() ||
                plugin_config.activated.find(plugin->get_id()) == plugin_config.activated.end() ||
                plugin_config.plugin_packages.find(plugin->get_id()) != plugin_config.plugin_packages.end())
                continue;
            const std::string package_id = boost::filesystem::path(plugin->get_package_root()).filename().string();
            if (!package_id.empty()) {
                plugin_config.plugin_packages[plugin->get_id()] = package_id;
                learned_package_association = true;
            }
        }
    }
    if (learned_package_association && automatic_configuration_writes_allowed && !config_dir.empty() &&
        !write_plugin_activation_config(plugin_activation_config_path(config_dir),
                                        plugin_config, plugin_config_error))
        BOOST_LOG_TRIVIAL(warning) << plugin_config_error;

    // Publish the final in-memory value only through the deferred diagnostic.
    // The original partial INI remains byte-identical until the user chooses
    // whether this sanitized value should become durable.
    if (activation_config_source == PluginActivationConfigSource::UserSanitized &&
        g_plugin_activation_startup_error.has_value())
        g_plugin_activation_startup_error->sanitized_config = plugin_config;

    // Loading and activation are intentionally separate. A disabled plugin is
    // still registered so the configuration dialog can show it, but it cannot
    // publish settings or run until its id appears in activated.ini.
    std::vector<std::string> active_plugin_ids;
    for (const auto &[plugin_id, is_enabled] : plugin_config.activated)
        if (is_enabled)
            active_plugin_ids.push_back(plugin_id);
    BOOST_LOG_TRIVIAL(info) << "Loaded " << active_plugin_ids.size() << " active plugin id(s) from "
                            << (user_configuration_available ? plugin_activation_config_path(config_dir).string() :
                                (boost::filesystem::path(resources_dir()) / "plugins/default_activated.ini").string()) << ".";
    activate_plugins_from_ids(orchestrator, active_plugin_ids, user_configuration_available);
    register_infill_pattern_config_choices(orchestrator);
    register_exclusive_step_group_options_impl(orchestrator);

    orchestrator.initialize_plugins();
    register_exclusive_step_group_ui_fragments_impl(orchestrator);
    //note: --loglevel 4 is read too late for this log, use $env:SLIC3R_LOGLEVEL = "4" (or SLIC3R_LOGLEVEL=4 in visual studio environement line)
    BOOST_LOG_TRIVIAL(debug) << "Loaded the entire plugin library in "
                             << elapsed_ms(start).count() << " ms.";
}

} // namespace Slic3r
