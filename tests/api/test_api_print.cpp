
//#define CATCH_CONFIG_DISABLE
#include <catch2/catch.hpp>

#include "test_data.hpp"
#include <libslic3r/libslic3r.h>
#include <libslic3r/Layer.hpp>
#include <libslic3r/LayerRegion.hpp>
#include <libslic3r/PrintObject.hpp>
#include <libslic3r/SVG.hpp>
#include <libslic3r/Format/3mf.hpp>
#include <libslic3r/Api/host/Orchestrator.hpp>
#include <libslic3r/Api/plugin/c/slic3r_orchestrator.h>
#include <libslic3r/Api/plugin/c/slic3r_plugin.h>
#include <libslic3r/Plugins/GuiRulesExample.hpp>
#include <libslic3r/Plugins/MaxOverhangThreshold.hpp>
#include <libslic3r/Plugins/PluginLoader.hpp>
#include <libslic3r/Plugins/SkirtBrim/DefaultBrimGenerator.hpp>
#include <libslic3r/Plugins/SliceVolume.hpp>
#include <libslic3r/Plugins/StandardLayerHeightGenerator.hpp>
#include <libslic3r/Plugins/Support/SupportDemandBridgeRemoval.hpp>
#include <libslic3r/FFFPrintConfig.hpp>
#include <libslic3r/PrintConfig.hpp>
#include <libslic3r/SLA/SLAPrintConfig.hpp>
#include <libslic3r/Steps/StepPipeline.hpp>
//#include <libslic3r/config.hpp>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;
using namespace std::literals;

namespace {

void ensure_api_test_runtime_initialized()
{
    static bool initialized = []() {
        PrintConfigDef::instance_mutable().init_common_params();
        init_fff_params(PrintConfigDef::instance_mutable());
        init_sla_params(PrintConfigDef::instance_mutable());

        orchestrator_handle *orchestrator = reinterpret_cast<orchestrator_handle *>(&Orchestrator::instance());
        slic3r_api::StandardLayerHeightGeneratorPlugin::register_standard_layer_height_generator_plugin(orchestrator);
        slic3r_api::SliceVolumePlugin::register_slice_volume_plugin(orchestrator);
        register_plugin(orchestrator);
        slic3r_api::GuiRulesExamplePlugin::register_gui_rules_example_plugin(orchestrator);
        slic3r_api::MaxOverhangThresholdPlugin::register_max_overhang_threshold_plugin(orchestrator);
        slic3r_api::Support::SupportDemandBridgeRemovalPlugin::register_support_demand_bridge_removal_plugin(orchestrator);
        slic3r_api::SkirtBrim::DefaultBrimGeneratorPlugin::register_default_brim_generator_plugin(orchestrator);

        REQUIRE(Orchestrator::instance().set_plugin_active("bridge_detector.default", true));
        REQUIRE(Orchestrator::instance().set_plugin_active("standard_layer_height_generator", true));
        REQUIRE(Orchestrator::instance().set_plugin_active("slice_volume", true));
        REQUIRE(Orchestrator::instance().set_plugin_active("polyholes", true));
        REQUIRE(Orchestrator::instance().set_plugin_active("gui_rules_example", true));
        REQUIRE(Orchestrator::instance().set_plugin_active("max_overhang_threshold", true));
        REQUIRE(Orchestrator::instance().set_plugin_active("support.demand.bridge_removal", true));
        REQUIRE(Orchestrator::instance().set_plugin_active("skirt_brim.brim.default", true));

        register_exclusive_step_group_options(Orchestrator::instance());
        Orchestrator::instance().initialize_plugins();
        register_exclusive_step_group_ui_fragments(Orchestrator::instance());
        initialize_fff_print_config_cache();
        initialize_sla_print_config_cache();
        PrintConfigDef::instance_mutable().finalize();
        return true;
    }();
    (void)initialized;
}


std::string read_text_file(const std::string &path)
{
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.good());
    std::ostringstream content;
    content << file.rdbuf();
    return content.str();
}

bool region_island_contains_region(const LayerRegionIsland &region_island, const LayerRegion *region)
{
    return region_island.regions().find(region) != region_island.regions().end();
}

size_t perimeter_region_island_count(const Layer &layer, const LayerRegion &region)
{
    size_t count = 0;
    for (const LayerSliceIsland &island : layer.islands())
        for (const LayerRegionIsland &region_island : island.regions_islands())
            if (region_island_contains_region(region_island, &region) &&
                region_island.has_extrusion(LayerRegionIsland::PERIMETERS))
                ++count;
    return count;
}

size_t perimeter_item_count(const Layer &layer, const LayerRegion &region)
{
    size_t count = 0;
    for (const LayerSliceIsland &island : layer.islands())
        for (const LayerRegionIsland &region_island : island.regions_islands())
            if (region_island_contains_region(region_island, &region) &&
                region_island.has_extrusion(LayerRegionIsland::PERIMETERS))
                count += region_island.extrusion(LayerRegionIsland::PERIMETERS).items_count();
    return count;
}

} // namespace

TEST_CASE("Plugin UI fragment rebuilds the original print layout", "[Api][UiLayout]")
{
    ensure_api_test_runtime_initialized();
    Orchestrator &orchestrator = Orchestrator::instance();
    orchestrator_handle *orch_handle = reinterpret_cast<orchestrator_handle *>(&orchestrator);
    const int32_t overhang_added = orchestrator_add_ui_fragment(orch_handle,
                                                               "print.ui",
                                                               "max_overhang_threshold",
                                                               slic3r_api::MaxOverhangThresholdPlugin::MaxOverhangThreshold::print_ui_fragment(),
                                                               0);
    REQUIRE((overhang_added == 0 || overhang_added == 1));
    const int32_t bridge_removal_added = orchestrator_add_ui_fragment(
        orch_handle,
        "print.ui",
        "support.demand.bridge_removal",
        slic3r_api::Support::SupportDemandBridgeRemovalPlugin::SupportDemandBridgeRemoval::print_ui_fragment(),
        0);
    REQUIRE((bridge_removal_added == 0 || bridge_removal_added == 1));
    const int32_t gui_rules_example_added = orchestrator_add_ui_fragment(orch_handle,
                                                                        "print.ui",
                                                                        "gui_rules_example",
                                                                        slic3r_api::GuiRulesExamplePlugin::GuiRulesExample::print_ui_fragment(),
                                                                        0);
    REQUIRE((gui_rules_example_added == 0 || gui_rules_example_added == 1));

    const std::string base = read_text_file(std::string(TEST_DATA_DIR) + "/../../resources/ui_layout/default/print.ui");
    const std::string expected = read_text_file(std::string(TEST_DATA_DIR) + "/ui_layout/print_with_builtin_overhang_threshold.ui");

    const std::string merged = orchestrator.merged_ui_layout("print.ui", base);
    const size_t common_size = std::min(merged.size(), expected.size());
    const size_t diff_pos = std::mismatch(merged.begin(), merged.begin() + common_size, expected.begin()).first -
                            merged.begin();
    CAPTURE(diff_pos);
    CAPTURE(merged.substr(diff_pos, 160));
    CAPTURE(expected.substr(diff_pos, 160));
    REQUIRE(merged == expected);
}

TEST_CASE("Implemented plugin UI fragments are skipped during layout merge", "[Api][UiLayout]")
{
    ensure_api_test_runtime_initialized();
    Orchestrator &orchestrator = Orchestrator::instance();
    const char *target_file = "test_implemented_fragment.ui";
    const char *fragment_id = "test.implemented.fragment";
    const bool added = orchestrator.add_ui_fragment(
        target_file,
        fragment_id,
        "page:Notes\n"
        "group:insert$aftergroup$Existing group:Inserted group\n"
        "line:inserted_line\n"
        "setting:inserted_setting\n"
        "end_line\n",
        0);
    REQUIRE((added || orchestrator.merged_ui_layout(target_file, "page:Notes\ngroup:Existing group\n").find("inserted_setting") != std::string::npos));

    const std::string base =
        "page:Notes\n"
        "group:Existing group\n"
        "line:existing_line\n"
        "setting:existing_setting\n"
        "end_line\n";

    const std::string merged_with_fragment = orchestrator.merged_ui_layout(target_file, base);
    REQUIRE(merged_with_fragment.find("inserted_setting") != std::string::npos);

    const std::unordered_set<std::string> implemented_fragment_ids = { fragment_id };
    const std::string merged_with_fragment_skipped =
        orchestrator.merged_ui_layout(target_file, base, implemented_fragment_ids);
    REQUIRE(merged_with_fragment_skipped == base);
}

TEST_CASE("Duplicate plugin UI fragment ids keep the first registered content", "[Api][UiLayout]")
{
    ensure_api_test_runtime_initialized();
    Orchestrator &orchestrator = Orchestrator::instance();
    const char *target_file = "test_duplicate_fragment_warning.ui";
    const char *fragment_id = "test.duplicate.fragment";
    const char *first_content =
        "page:Notes\n"
        "group:Inserted group\n"
        "line:first_line\n"
        "setting:first_setting\n"
        "end_line\n";
    const char *different_content =
        "page:Notes\n"
        "group:Inserted group\n"
        "line:second_line\n"
        "setting:second_setting\n"
        "end_line\n";

    REQUIRE(orchestrator.add_ui_fragment(target_file, fragment_id, first_content, 0));
    CHECK_FALSE(orchestrator.add_ui_fragment(target_file, fragment_id, first_content, 0));

    // A different second fragment with the same id is expected to log a warning.
    // The safety rule is that the already accepted fragment still owns the id,
    // so the visible UI cannot depend on plugin initialization order.
    CHECK_FALSE(orchestrator.add_ui_fragment(target_file, fragment_id, different_content, 0));

    const std::vector<Orchestrator::PluginUiFragment> fragments = orchestrator.ui_fragments_for_file(target_file);
    REQUIRE(fragments.size() == 1);
    CHECK(fragments.front().content == first_content);

    const std::string merged = orchestrator.merged_ui_layout(target_file, "page:Notes\n");
    CHECK(merged.find("first_setting") != std::string::npos);
    CHECK(merged.find("second_setting") == std::string::npos);
}

TEST_CASE("Plugin warning and error callbacks queue GUI notifications", "[Api][Plugins]")
{
    ensure_api_test_runtime_initialized();
    Orchestrator &orchestrator = Orchestrator::instance();
    const std::vector<Orchestrator::PluginMessage> stale_messages = orchestrator.consume_plugin_messages();
    (void)stale_messages;

    Plugin *plugin = orchestrator.get_plugin("polyholes");
    REQUIRE(plugin != nullptr);

    // The C callbacks are used by both native and foreign-language plugins.
    // They must copy the message into the host queue because the GUI consumes
    // it later, after the plugin callback has already returned.
    plugin_host_context host_context =
        orchestrator.prepare_plugin_host_context(STEP_POST_SLICING, plugin, nullptr);
    orchestrator.reset_plugin_cancel();

    orchestrator_plugin_report_warning(&host_context, "queued warning");
    orchestrator_plugin_report_error(&host_context, "queued error");

    CHECK(orchestrator.is_plugin_cancelled());

    const std::vector<Orchestrator::PluginMessage> messages = orchestrator.consume_plugin_messages();
    REQUIRE(messages.size() == 2);
    CHECK(messages[0].level == Orchestrator::PluginMessageLevel::Warning);
    CHECK(messages[0].plugin_id == "polyholes");
    CHECK(messages[0].step == STEP_POST_SLICING);
    CHECK(messages[0].message == "queued warning");
    CHECK(messages[1].level == Orchestrator::PluginMessageLevel::Error);
    CHECK(messages[1].plugin_id == "polyholes");
    CHECK(messages[1].step == STEP_POST_SLICING);
    CHECK(messages[1].message == "queued error");
    CHECK(orchestrator.consume_plugin_messages().empty());

    orchestrator.reset_plugin_cancel();
}

SCENARIO("PrintObject: Perimeter generation") {
    GIVEN("20mm cube and default config & 0.3 layer height") {
        ensure_api_test_runtime_initialized();
        DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
        TestMesh mesh = TestMesh::cube_20x20x20;
        Model model{};
        config.set_key_value("fill_density", new ConfigOptionPercent(0));
        config.set_deserialize("nozzle_diameter", "0.4");
        config.set_deserialize("layer_height", "0.3");

        WHEN("make_perimeters() is called") {
            Print print{};
            Slic3r::Test::init_print({mesh}, print, model, config);
#ifdef _DEBUG
            Slic3r::Steps::StepPipeline::debug_run(Orchestrator::instance(), print, STEP_PRE_PERIMETER);
#endif
            print.process();
            PrintObject &object = print.object(0);
            //THEN("67 layers exist in the model") 
            { REQUIRE(object.layer_count() == 67); }
            //THEN("Every layer in region 0 has 1 island of perimeters")
            {
                for (Layer &layer : object.layers()) {
                    REQUIRE(perimeter_region_island_count(layer, layer.region(0)) == 1);
                }
            }
            //THEN("Every layer (but top) in region 0 has 3 paths in its perimeters list.")
            {
                for (size_t layer_idx = 0; layer_idx + 1 < object.layer_count(); ++layer_idx) {
                    Layer &layer = object.layer(layer_idx);
                    REQUIRE(perimeter_item_count(layer, layer.region(0)) == 3);
                }
            }
            //THEN("Top layer in region 0 has 1 path in its perimeters list (only 1 perimeter on top).")
            {
                Layer &top_layer = object.layer(object.layer_count() - 1);
                REQUIRE(perimeter_item_count(top_layer, top_layer.region(0)) == 1);
            }
            REQUIRE_FALSE(object.has_raft());
        }
    }
}
