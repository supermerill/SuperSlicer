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
#include "libslic3r/LayerRegion.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PluginProperty.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PrintObject.hpp"
#include "libslic3r/Steps/StepLayerHeightGeneration.hpp"
#include "libslic3r/Steps/StepPipeline.hpp"
#include "libslic3r/Steps/StepSlicing.hpp"
#include "libslic3r/Steps/StepSkirtBrim.hpp"

/*
Default skirt/brim generator tests
==================================

These tests exercise the new STEP_SKIRT_BRIM boundary, not the whole print
pipeline. They slice a simple cube far enough to create first-layer slices, then
run the built-in adhesion plugins. This keeps the test focused on three
contracts:

- STEP_SKIRT_BRIM is a non-exclusive chain, while brim and skirt sub-features
  are separate exclusive groups selected by plugin id.
- The default brim/skirt plugins write global, per-object, and first-layer-only
  extrusion through tagged auxiliary layers.
- The host rebuilds the first-layer convex hull from auxiliary adhesion output.
*/

namespace {

using namespace Slic3r;

const char *const STANDARD_LAYER_HEIGHT_GENERATOR = "standard_layer_height_generator";
const char *const SLICE_VOLUME = "slice_volume";
const char *const DEFAULT_BRIM_GENERATOR = "skirt_brim.brim.default";
const char *const DEFAULT_SKIRT_GENERATOR = "skirt_brim.skirt.default";
const char *const DEFAULT_BRIM_SKIRT_TRIM = "skirt_brim.brim_skirt_trim.default";

class ScopedActivePlugins
{
public:
    explicit ScopedActivePlugins(std::initializer_list<const char *> plugin_ids) :
        ScopedActivePlugins(std::vector<const char *>(plugin_ids))
    {}

    explicit ScopedActivePlugins(const std::vector<const char *> &plugin_ids) :
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

DynamicPrintConfig skirt_test_config(const int skirts, const int skirt_brim, const bool per_object)
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height", "0.2" },
        { "first_layer_height", "0.2" },
        { "brim_width", "0" },
        { "brim_width_interior", "0" },
        { "brim_per_object", "0" },
        { "brim_ears", "0" },
        { "skirts", std::to_string(skirts) },
        { "skirt_height", "1" },
        { "skirt_distance", "3" },
        { "skirt_distance_from_brim", "0" },
        { "skirt_brim", std::to_string(skirt_brim) },
        { "min_skirt_length", "0" },
        { "draft_shield", "disabled" },
        { "complete_objects", per_object ? "1" : "0" },
        { "complete_objects_one_skirt", "0" },
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

void run_until_skirt_brim(PreparedBrimPrint &prepared,
                          const DynamicPrintConfig &config,
                          const bool include_trim = true)
{
    Slic3r::Test::init_print({ Slic3r::Test::TestMesh::cube_20x20x20 }, prepared.print, prepared.model, config);
    std::vector<const char *> active_plugin_ids = {
        STANDARD_LAYER_HEIGHT_GENERATOR,
        SLICE_VOLUME,
        DEFAULT_BRIM_GENERATOR,
        DEFAULT_SKIRT_GENERATOR
    };
    if (include_trim)
        active_plugin_ids.push_back(DEFAULT_BRIM_SKIRT_TRIM);
    ScopedActivePlugins active(active_plugin_ids);

    Orchestrator &orchestrator = Orchestrator::instance();
    Steps::StepLayerHeightGeneration::run_step(orchestrator, prepared.print);
    Steps::StepSlicing::run_step(orchestrator, prepared.print);
    (void) orchestrator.consume_plugin_messages();
    Steps::StepSkirtBrim::run_step(orchestrator, prepared.print);
    for (const Orchestrator::PluginMessage &message : orchestrator.consume_plugin_messages()) {
        INFO("Plugin " << message.plugin_id << " reported: " << message.message);
        CHECK(message.level != Orchestrator::PluginMessageLevel::Error);
    }
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

double extrusion_tree_length_mm(const ExtrusionEntity &entity)
{
    ArcPolylines polylines;
    entity.collect_polylines(polylines);

    double length = 0.0;
    for (const ArcPolyline &polyline : polylines)
        length += unscaled(polyline.length());
    return length;
}

std::vector<const Layer *> object_brim_auxiliary_layers(const PrintObject &object)
{
    std::vector<const Layer *> out;
    for (const Layer &layer : object.auxiliary_layers())
        if (LayerAdhesionProperty::layer_is_brim(layer))
            out.push_back(&layer);
    return out;
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

    Plugin *skirt_plugin = orchestrator.get_plugin(DEFAULT_SKIRT_GENERATOR);
    REQUIRE(skirt_plugin != nullptr);
    CHECK(skirt_plugin->get_step() == STEP_SKIRT_BRIM);
    CHECK(skirt_plugin->get_exclusive_group() == "skirt_brim.skirt");

    Plugin *trim_plugin = orchestrator.get_plugin(DEFAULT_BRIM_SKIRT_TRIM);
    REQUIRE(trim_plugin != nullptr);
    CHECK(trim_plugin->get_step() == STEP_SKIRT_BRIM);
    CHECK(trim_plugin->get_exclusive_group() == "skirt_brim.brim_skirt_trim");

    bool found_trim_setting_fragment = false;
    const std::vector<Orchestrator::PluginUiFragment> print_fragments =
        orchestrator.ui_fragments_for_file("print.ui");
    for (const Orchestrator::PluginUiFragment &fragment : print_fragments)
        if (fragment.fragment_id == "brim_skirt_trim") {
            found_trim_setting_fragment = true;
            CHECK(fragment.content.find("group:Brim") != std::string::npos);
            CHECK(fragment.content.find("insert$aftersetting$brim_per_object:brim_skirt_trim") !=
                  std::string::npos);
        }
    CHECK(found_trim_setting_fragment);

    const std::map<slicing_step_t, Steps::StepExclusiveGroup> &exclusive_steps = Steps::get_exclusive_steps();
    CHECK(exclusive_steps.find(STEP_SKIRT_BRIM) == exclusive_steps.end());
}

TEST_CASE("Default skirt/brim plugins are active by default for plugin tests", "[plugins][skirt-brim]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    std::vector<Plugin *> plugins = Orchestrator::instance().get_active_plugins_for_step(STEP_SKIRT_BRIM);
    bool found_default_brim = false;
    bool found_default_skirt = false;
    bool found_default_trim = false;
    for (Plugin *plugin : plugins)
        found_default_brim = found_default_brim || (plugin != nullptr && plugin->get_id() == DEFAULT_BRIM_GENERATOR);
    for (Plugin *plugin : plugins)
        found_default_skirt = found_default_skirt || (plugin != nullptr && plugin->get_id() == DEFAULT_SKIRT_GENERATOR);
    for (Plugin *plugin : plugins)
        found_default_trim = found_default_trim || (plugin != nullptr && plugin->get_id() == DEFAULT_BRIM_SKIRT_TRIM);
    CHECK(found_default_brim);
    CHECK(found_default_skirt);
    CHECK(found_default_trim);
}

TEST_CASE("Default brim generator leaves output empty when brim is disabled", "[plugins][skirt-brim]")
{
    PreparedBrimPrint prepared;
    run_until_skirt_brim(prepared, brim_test_config(false, false));

    CHECK(prepared.print.brim().empty());
    REQUIRE(prepared.print.objects().size() == 1);
    CHECK(prepared.print.objects().front().brim().empty());
    CHECK(object_brim_auxiliary_layers(prepared.print.objects().front()).empty());
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
    PrintObject &object = prepared.print.objects().front();
    CHECK_FALSE(object.brim().empty());
    CHECK(hull_area_mm2(prepared.print) > first_layer_slice_area_mm2(prepared.print));

    const std::vector<const Layer *> brim_layers = object_brim_auxiliary_layers(object);
    REQUIRE(brim_layers.size() == 1);
    const Layer &brim_layer = *brim_layers.front();
    const LayerAdhesionProperty *adhesion = brim_layer.get_property<LayerAdhesionProperty>();
    REQUIRE(adhesion != nullptr);
    CHECK(adhesion->is_brim());
    CHECK(brim_layer.get_property<LayerSupportProperty>() == nullptr);
    CHECK_FALSE(brim_layer.lslices().empty());
    REQUIRE_FALSE(brim_layer.islands().empty());
    REQUIRE_FALSE(brim_layer.island(0).regions_islands().empty());
    const LayerRegionIsland &region_island = brim_layer.island(0).regions_island(0);
    REQUIRE(region_island.has_extrusion(LayerRegionIsland::PERIMETERS));
    CHECK_FALSE(region_island.extrusion(LayerRegionIsland::PERIMETERS).empty());

    Steps::StepSkirtBrim::clean_and_prepare(prepared.print);
    CHECK(object.brim().empty());
    CHECK(object_brim_auxiliary_layers(object).empty());
}

TEST_CASE("Default skirt generator creates global skirt", "[plugins][skirt-brim]")
{
    PreparedBrimPrint prepared;
    run_until_skirt_brim(prepared, skirt_test_config(1, 0, false));

    CHECK(prepared.print.brim().empty());
    CHECK_FALSE(prepared.print.skirt().empty());
    REQUIRE(prepared.print.objects().size() == 1);
    CHECK(prepared.print.objects().front().skirt().empty());
    CHECK(hull_area_mm2(prepared.print) > first_layer_slice_area_mm2(prepared.print));
}

TEST_CASE("Default skirt generator can publish object-owned skirt", "[plugins][skirt-brim]")
{
    PreparedBrimPrint prepared;
    run_until_skirt_brim(prepared, skirt_test_config(1, 0, true));

    CHECK(prepared.print.skirt().empty());
    REQUIRE(prepared.print.objects().size() == 1);
    CHECK_FALSE(prepared.print.objects().front().skirt().empty());
    CHECK(hull_area_mm2(prepared.print) > first_layer_slice_area_mm2(prepared.print));
}

TEST_CASE("Default skirt generator publishes skirt-brim as first-layer-only skirt", "[plugins][skirt-brim]")
{
    PreparedBrimPrint prepared;
    run_until_skirt_brim(prepared, skirt_test_config(0, 2, false));

    CHECK(prepared.print.skirt().empty());
    REQUIRE(prepared.print.skirt_first_layer().has_value());
    CHECK_FALSE(prepared.print.skirt_first_layer()->empty());
    CHECK(hull_area_mm2(prepared.print) > first_layer_slice_area_mm2(prepared.print));
}

TEST_CASE("Default brim/skirt trim setting controls draft-shield brim trimming", "[plugins][skirt-brim]")
{
    DynamicPrintConfig config = skirt_test_config(1, 0, false);
    config.set_deserialize_strict({
        { "brim_width", "5" },
        { "draft_shield", "limited" },
        { "skirt_distance", "1" },
        { "brim_skirt_trim", "0" }
    });

    PreparedBrimPrint untrimmed;
    run_until_skirt_brim(untrimmed, config, true);
    REQUIRE_FALSE(untrimmed.print.brim().empty());
    REQUIRE_FALSE(untrimmed.print.skirt().empty());

    config.set_deserialize_strict({
        { "brim_skirt_trim", "1" }
    });

    PreparedBrimPrint trimmed;
    run_until_skirt_brim(trimmed, config, true);
    REQUIRE_FALSE(trimmed.print.brim().empty());
    REQUIRE_FALSE(trimmed.print.skirt().empty());

    /*
    The legacy trim exists for this exact configuration: a draft shield is
    allowed to stand inside the configured brim width. The trim plugin keeps
    printable brim pieces, but removes the part occupied by the final skirt
    band, so total brim centerline length must decrease.
    */
    CHECK(extrusion_tree_length_mm(trimmed.print.brim()) <
          extrusion_tree_length_mm(untrimmed.print.brim()));
}
