#include "plugin_test_helpers.hpp"

#include <cassert>

#include <boost/log/trivial.hpp>

#ifdef SLIC3R_TEST_PYTHON_PLUGINS
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
#include <limits.h>
#include <unistd.h>
#endif

#include <string>
#include <vector>

#include <boost/filesystem.hpp>
#endif

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/FFFPrintConfig.hpp"
#include "libslic3r/Plugins/PluginLoader.hpp"
#include "libslic3r/Plugins/Perimeter/ArachnePerimeterGenerator.hpp"
#include "libslic3r/Plugins/Perimeter/ExtraPerimeterBelowArea.hpp"
#include "libslic3r/Plugins/Perimeter/ExtraPerimeterCount.hpp"
#include "libslic3r/Plugins/Perimeter/ExtraPerimeterOddLayer.hpp"
#include "libslic3r/Plugins/Perimeter/ExtraPerimeterOverhangWave.hpp"
#include "libslic3r/Plugins/Perimeter/ExtraPerimetersOnOverhangs.hpp"
#include "libslic3r/Plugins/Perimeter/FuzzySkin.hpp"
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
#include "libslic3r/Plugins/Support/SupportDemandModifiers.hpp"
#include "libslic3r/Plugins/Support/SupportDemandOverhangs.hpp"
#include "libslic3r/Plugins/Support/SupportDemandPainting.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/SLA/SLAPrintConfig.hpp"
#include "plugins_cpp/FlatAreaLayerHeight/FlatAreaLayerHeight.hpp"
#include "plugins_cpp/Polyholes/Polyholes.hpp"

namespace {

#ifdef SLIC3R_TEST_PYTHON_PLUGINS

bool g_python_plugins_loaded = false;

boost::filesystem::path current_executable_dir()
{
#ifdef _WIN32
    std::vector<wchar_t> buffer(MAX_PATH);
    DWORD length = 0;
    for (;;) {
        length = GetModuleFileNameW(NULL, buffer.data(), DWORD(buffer.size()));
        if (length == 0)
            return {};
        if (length < buffer.size() - 1)
            break;
        buffer.resize(buffer.size() * 2);
    }
    return boost::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
#else
    std::vector<char> buffer(PATH_MAX);
    for (;;) {
        const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0)
            return boost::filesystem::current_path();
        if (size_t(length) < buffer.size())
            return boost::filesystem::path(std::string(buffer.data(), size_t(length))).parent_path();
        buffer.resize(buffer.size() * 2);
    }
#endif
}

bool load_python_plugins_for_tests(orchestrator_handle *orchestrator)
{
#ifdef _WIN32
    const boost::filesystem::path loader_path = current_executable_dir() / "plugins" / "python_plugin_loader.dll";
    if (!boost::filesystem::exists(loader_path)) {
        BOOST_LOG_TRIVIAL(warning) << "Python plugin loader not found for plugin tests: " << loader_path.string();
        return false;
    }

    static std::vector<HMODULE> loaded_modules;
    HMODULE module = LoadLibraryW(loader_path.wstring().c_str());
    if (module == NULL) {
        BOOST_LOG_TRIVIAL(error) << "Cannot load Python plugin loader '" << loader_path.string()
                                 << "': error " << GetLastError();
        return false;
    }
    loaded_modules.push_back(module);

    FARPROC proc = GetProcAddress(module, "register_plugin");
    if (proc == NULL) {
        BOOST_LOG_TRIVIAL(error) << "Python plugin loader '" << loader_path.string()
                                 << "' does not export register_plugin.";
        return false;
    }
#else
    const boost::filesystem::path loader_path = current_executable_dir() / "plugins" / "libpython_plugin_loader.so";
    if (!boost::filesystem::exists(loader_path)) {
        BOOST_LOG_TRIVIAL(warning) << "Python plugin loader not found for plugin tests: " << loader_path.string();
        return false;
    }

    static std::vector<void *> loaded_modules;
    void *module = dlopen(loader_path.string().c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (module == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "Cannot load Python plugin loader '" << loader_path.string()
                                 << "': " << dlerror();
        return false;
    }
    loaded_modules.push_back(module);

    void *proc = dlsym(module, "register_plugin");
    if (proc == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "Python plugin loader '" << loader_path.string()
                                 << "' does not export register_plugin: " << dlerror();
        return false;
    }
#endif

    using RegisterPluginFn = void (*)(orchestrator_handle *);
    reinterpret_cast<RegisterPluginFn>(proc)(orchestrator);
    return true;
}

#endif // SLIC3R_TEST_PYTHON_PLUGINS

void activate_plugin_or_fail(Slic3r::Orchestrator &orchestrator, const char *plugin_id)
{
    if (!orchestrator.set_plugin_active(plugin_id, true)) {
        BOOST_LOG_TRIVIAL(error) << "Test plugin '" << plugin_id << "' was not registered.";
        assert(false);
    }
}

} // namespace

namespace Slic3r::Test::Plugins {

void ensure_plugin_test_runtime_initialized()
{
    static bool initialized = []() {
        PrintConfigDef::instance_mutable().init_common_params();
        init_fff_params(PrintConfigDef::instance_mutable());
        init_sla_params(PrintConfigDef::instance_mutable());

        Orchestrator &orchestrator = Orchestrator::instance();
        orchestrator_handle *orchestrator_handle_value =
            reinterpret_cast<orchestrator_handle *>(&orchestrator);
        slic3r_api::StandardLayerHeightGeneratorPlugin::register_standard_layer_height_generator_plugin(
            orchestrator_handle_value);
        slic3r_api::SliceVolumePlugin::register_slice_volume_plugin(orchestrator_handle_value);
        slic3r_api::FlatAreaLayerHeightPlugin::register_flat_area_layer_height_plugin(orchestrator_handle_value);
        slic3r_api::PolyholesPlugin::register_polyholes_plugin(orchestrator_handle_value);
        slic3r_api::Support::SupportDemandOverhangsPlugin::register_support_demand_overhangs_plugin(
            orchestrator_handle_value);
        slic3r_api::Support::SupportDemandPaintingPlugin::register_support_demand_painting_plugin(
            orchestrator_handle_value);
        slic3r_api::Support::SupportDemandModifiersPlugin::register_support_demand_modifiers_plugin(
            orchestrator_handle_value);
        slic3r_api::Support::SupportDemandBridgeRemovalPlugin::register_support_demand_bridge_removal_plugin(
            orchestrator_handle_value);
        slic3r_api::Perimeter::ArachnePerimeterGeneratorPlugin::register_arachne_perimeter_generator_plugin(
            orchestrator_handle_value);
        slic3r_api::Perimeter::SimplePerimeterGeneratorPlugin::register_simple_perimeter_generator_plugin(
            orchestrator_handle_value);
        slic3r_api::Perimeter::ExtraPerimeterCountPlugin::register_extra_perimeter_count_plugin(
            orchestrator_handle_value);
        slic3r_api::Perimeter::ExtraPerimeterBelowAreaPlugin::register_extra_perimeter_below_area_plugin(
            orchestrator_handle_value);
        slic3r_api::Perimeter::ExtraPerimeterOddLayerPlugin::register_extra_perimeter_odd_layer_plugin(
            orchestrator_handle_value);
        slic3r_api::Perimeter::OnlyOnePerimeterFirstLayerPlugin::register_only_one_perimeter_first_layer_plugin(
            orchestrator_handle_value);
        slic3r_api::Perimeter::OnlyOnePerimeterOnTopPlugin::register_only_one_perimeter_on_top_plugin(
            orchestrator_handle_value);
        slic3r_api::Perimeter::SeparateHoleContourPlugin::register_separate_hole_contour_plugin(
            orchestrator_handle_value);
        slic3r_api::Perimeter::RemoveGapFillOnOverhangsPlugin::register_remove_gap_fill_on_overhangs_plugin(
            orchestrator_handle_value);
        slic3r_api::Perimeter::ExtraPerimetersOnOverhangsPlugin::register_extra_perimeters_on_overhangs_plugin(
            orchestrator_handle_value);
        slic3r_api::Perimeter::ExtraPerimeterOverhangWavePlugin::register_extra_perimeter_overhang_wave_plugin(
            orchestrator_handle_value);
        slic3r_api::Perimeter::FuzzySkinPlugin::register_fuzzy_skin_plugin(orchestrator_handle_value);
        slic3r_api::SurfaceGeneration::InitialTypedSurfaceBuilderPlugin::register_initial_typed_surface_builder_plugin(
            orchestrator_handle_value);
        slic3r_api::SurfaceGeneration::SolidShellsPlugin::register_solid_shells_plugin(
            orchestrator_handle_value);
        slic3r_api::SurfaceGeneration::TopSurfaceExpansionPlugin::register_top_surface_expansion_plugin(
            orchestrator_handle_value);
        slic3r_api::SurfaceGeneration::CleanInfillSurfacesPlugin::register_clean_infill_surfaces_plugin(
            orchestrator_handle_value);
        slic3r_api::SurfaceGeneration::InfillRegionCompatibilitySplitterPlugin::
            register_infill_region_compatibility_splitter_plugin(orchestrator_handle_value);
#ifdef SLIC3R_TEST_PYTHON_PLUGINS
        g_python_plugins_loaded = load_python_plugins_for_tests(orchestrator_handle_value) &&
                                  orchestrator.get_plugin("python.polyholes") != nullptr &&
                                  orchestrator.get_plugin("python.polyholes.high_level") != nullptr &&
                                  orchestrator.get_plugin("python.perimeter.post_process.hairy_object") != nullptr &&
                                  orchestrator.get_plugin("python.perimeter.generator.simple") != nullptr;
#endif

        activate_plugin_or_fail(orchestrator, "bridge_detector.default");
        activate_plugin_or_fail(orchestrator, "standard_layer_height_generator");
        activate_plugin_or_fail(orchestrator, "slice_volume");
        activate_plugin_or_fail(orchestrator, "flat_area_layer_height");
        activate_plugin_or_fail(orchestrator, "polyholes");
        activate_plugin_or_fail(orchestrator, "support.demand.overhangs");
        activate_plugin_or_fail(orchestrator, "support.demand.painting");
        activate_plugin_or_fail(orchestrator, "support.demand.modifiers");
        activate_plugin_or_fail(orchestrator, "support.demand.bridge_removal");
        activate_plugin_or_fail(orchestrator, "perimeter.generator.arachne");
        activate_plugin_or_fail(orchestrator, "perimeter.generator.simple");
        activate_plugin_or_fail(orchestrator, "perimeter.module.extra_perimeter_count");
        activate_plugin_or_fail(orchestrator, "perimeter.module.extra_perimeter_below_area");
        activate_plugin_or_fail(orchestrator, "perimeter.module.extra_perimeter_odd_layer");
        activate_plugin_or_fail(orchestrator, "perimeter.module.only_one_perimeter_first_layer");
        activate_plugin_or_fail(orchestrator, "perimeter.module.only_one_perimeter_on_top");
        activate_plugin_or_fail(orchestrator, "perimeter.module.separate_hole_contour");
        activate_plugin_or_fail(orchestrator, "perimeter.module.remove_gap_fill_on_overhangs");
        activate_plugin_or_fail(orchestrator, "perimeter.post_process.extra_perimeters_on_overhangs");
        activate_plugin_or_fail(orchestrator, "perimeter.post_process.extra_perimeter_overhang_wave");
        activate_plugin_or_fail(orchestrator, "perimeter.post_process.fuzzy_skin");
        activate_plugin_or_fail(orchestrator, "surface.initial_typed_surface_builder");
        activate_plugin_or_fail(orchestrator, "surface.solid_shells");
        activate_plugin_or_fail(orchestrator, "surface.top_surface_expansion");
        activate_plugin_or_fail(orchestrator, "surface.clean_infill_surfaces");
        activate_plugin_or_fail(orchestrator, "surface.infill_region_compatibility_splitter");
#ifdef SLIC3R_TEST_PYTHON_PLUGINS
        if (g_python_plugins_loaded) {
            activate_plugin_or_fail(orchestrator, "python.polyholes");
            activate_plugin_or_fail(orchestrator, "python.polyholes.high_level");
            activate_plugin_or_fail(orchestrator, "python.perimeter.post_process.hairy_object");
            activate_plugin_or_fail(orchestrator, "python.perimeter.generator.simple");
        }
#endif

        register_exclusive_step_group_options(orchestrator);
        orchestrator.initialize_plugins();
        register_exclusive_step_group_ui_fragments(orchestrator);
        initialize_fff_print_config_cache();
        initialize_sla_print_config_cache();
        PrintConfigDef::instance_mutable().finalize();
        return true;
    }();
    (void)initialized;
}

bool python_plugin_test_runtime_available()
{
    ensure_plugin_test_runtime_initialized();
#ifdef SLIC3R_TEST_PYTHON_PLUGINS
    return g_python_plugins_loaded;
#else
    return false;
#endif
}

} // namespace Slic3r::Test::Plugins
