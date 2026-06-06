#include <catch2/catch.hpp>

#include <initializer_list>
#include <vector>

#include "plugin_test_helpers.hpp"
#include "test_data.hpp"

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/host/Plugin.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PrintObject.hpp"
#include "libslic3r/Steps/StepLayerHeightGeneration.hpp"
#include "libslic3r/Steps/StepPipeline.hpp"
#include "libslic3r/Steps/StepSlicing.hpp"
#include "libslic3r/Steps/StepSkirtBrim.hpp"

/*
Default brim generator tests
============================

These tests exercise the new STEP_SKIRT_BRIM boundary, not the whole print
pipeline. They slice a simple cube far enough to create first-layer slices, then
run only the built-in brim plugin. This keeps the test focused on three
contracts:

- STEP_SKIRT_BRIM is a non-exclusive chain, while the brim sub-feature is an
  exclusive group selected by plugin id.
- The default brim plugin writes global or per-object brim through the step
  callbacks.
- The host rebuilds the first-layer convex hull from plugin brim output.
*/

namespace {

using namespace Slic3r;

const char *const STANDARD_LAYER_HEIGHT_GENERATOR = "standard_layer_height_generator";
const char *const SLICE_VOLUME = "slice_volume";
const char *const DEFAULT_BRIM_GENERATOR = "skirt_brim.brim.default";

class ScopedActivePlugins
{
public:
    explicit ScopedActivePlugins(std::initializer_list<const char *> plugin_ids) :
        m_orchestrator(Orchestrator::instance())
    {
        m_previous_active_plugins.reserve(m_orchestrator.active_plugins().size());
        for (Plugin *plugin : m_orchestrator.active_plugins())
            m_previous_active_plugins.push_back(plugin);

        m_orchestrator.clear_active_plugins();
        for (const char *plugin_id : plugin_ids) {
            INFO("Activating test plugin " << plugin_id);
            REQUIRE(m_orchestrator.set_plugin_active(plugin_id, true));
        }
    }

    ~ScopedActivePlugins()
    {
        m_orchestrator.clear_active_plugins();
        for (Plugin *plugin : m_previous_active_plugins)
            m_orchestrator.set_plugin_active(plugin, true);
    }

private:
    Orchestrator &m_orchestrator;
    std::vector<Plugin *> m_previous_active_plugins;
};

DynamicPrintConfig brim_test_config(const bool brim_enabled, const bool per_object)
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height", "0.2" },
        { "first_layer_height", "0.2" },
        { "brim_width", brim_enabled ? "3" : "0" },
        { "brim_width_interior", "0" },
        { "brim_per_object", per_object ? "1" : "0" },
        { "brim_ears", "0" },
        { "skirts", "0" },
        { "skirt_brim", "0" },
        { "support_material", "0" },
        { "raft_layers", "0" },
        { "perimeters", "2" },
        { "fill_density", "0" },
        { "step_layer_height_plugin", STANDARD_LAYER_HEIGHT_GENERATOR }
    });
    return config;
}

struct PreparedBrimPrint
{
    Model model;
    Print print;
};

void run_until_skirt_brim(PreparedBrimPrint &prepared, const DynamicPrintConfig &config)
{
    Slic3r::Test::init_print({ Slic3r::Test::TestMesh::cube_20x20x20 }, prepared.print, prepared.model, config);
    ScopedActivePlugins active({ STANDARD_LAYER_HEIGHT_GENERATOR, SLICE_VOLUME, DEFAULT_BRIM_GENERATOR });

    Orchestrator &orchestrator = Orchestrator::instance();
    Steps::StepLayerHeightGeneration::run_step(orchestrator, prepared.print);
    Steps::StepSlicing::run_step(orchestrator, prepared.print);
    Steps::StepSkirtBrim::run_step(orchestrator, prepared.print);
}

double first_layer_slice_area_mm2(const Print &print)
{
    REQUIRE(print.objects().size() == 1);
    const PrintObject &object = print.objects().front();
    REQUIRE(object.layer_count() > 0);
    return unscaled(unscaled(std::abs(area(object.layers().front().lslices()))));
}

double hull_area_mm2(const Print &print)
{
    return unscaled(unscaled(std::abs(area(print.first_layer_convex_hull()))));
}

} // namespace

TEST_CASE("STEP_SKIRT_BRIM exposes a brim exclusive group but is not exclusive itself",
          "[plugins][skirt-brim]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Orchestrator &orchestrator = Orchestrator::instance();
    Plugin *plugin = orchestrator.get_plugin(DEFAULT_BRIM_GENERATOR);
    REQUIRE(plugin != nullptr);
    CHECK(plugin->get_step() == STEP_SKIRT_BRIM);
    CHECK(plugin->get_exclusive_group() == "skirt_brim.brim");

    const std::map<slicing_step_t, Steps::StepExclusiveGroup> &exclusive_steps = Steps::get_exclusive_steps();
    CHECK(exclusive_steps.find(STEP_SKIRT_BRIM) == exclusive_steps.end());
}

TEST_CASE("Default brim generator is active by default for plugin tests", "[plugins][skirt-brim]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    std::vector<Plugin *> plugins = Orchestrator::instance().get_active_plugins_for_step(STEP_SKIRT_BRIM);
    bool found_default_brim = false;
    for (Plugin *plugin : plugins)
        found_default_brim = found_default_brim || (plugin != nullptr && plugin->get_id() == DEFAULT_BRIM_GENERATOR);
    CHECK(found_default_brim);
}

TEST_CASE("Default brim generator leaves output empty when brim is disabled", "[plugins][skirt-brim]")
{
    PreparedBrimPrint prepared;
    run_until_skirt_brim(prepared, brim_test_config(false, false));

    CHECK(prepared.print.brim().empty());
    REQUIRE(prepared.print.objects().size() == 1);
    CHECK(prepared.print.objects().front().brim().empty());
}

TEST_CASE("Default brim generator creates global brim and expands the first-layer hull",
          "[plugins][skirt-brim]")
{
    PreparedBrimPrint prepared;
    run_until_skirt_brim(prepared, brim_test_config(true, false));

    CHECK_FALSE(prepared.print.brim().empty());
    REQUIRE(prepared.print.objects().size() == 1);
    CHECK(prepared.print.objects().front().brim().empty());

    /*
    The hull is rebuilt by the host after the plugin chain. If the callback
    output were not considered, the hull would stay near the object slice area.
    */
    CHECK(hull_area_mm2(prepared.print) > first_layer_slice_area_mm2(prepared.print));
}

TEST_CASE("Default brim generator can publish object-owned brim", "[plugins][skirt-brim]")
{
    PreparedBrimPrint prepared;
    run_until_skirt_brim(prepared, brim_test_config(true, true));

    CHECK(prepared.print.brim().empty());
    REQUIRE(prepared.print.objects().size() == 1);
    CHECK_FALSE(prepared.print.objects().front().brim().empty());
    CHECK(hull_area_mm2(prepared.print) > first_layer_slice_area_mm2(prepared.print));
}
