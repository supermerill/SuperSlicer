#include <catch2/catch.hpp>

#include "perimeter_test_helpers.hpp"
#include "plugin_test_helpers.hpp"

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/host/Plugin.hpp"

namespace {

using namespace Slic3r;
using namespace Slic3r::Test::PerimeterPluginTests;

} // namespace

TEST_CASE("ClassicPerimeterGenerator is registered and selectable skeleton", "[plugins][perimeter][classic-generator]")
{
    // This test protects the temporary STEP_PERIMETER skeleton, not the final
    // classic geometry. It should appear as a selectable perimeter generator
    // and run through the host perimeter tree without crashing while the real
    // process_classic() blocks are migrated later.
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Slic3r::Plugin *plugin = Slic3r::Orchestrator::instance().get_plugin(CLASSIC_PERIMETER_GENERATOR);
    REQUIRE(plugin != nullptr);
    CHECK(plugin->get_id() == CLASSIC_PERIMETER_GENERATOR);
    CHECK(plugin->get_name() == "Classic perimeter generator");
    CHECK(plugin->get_step() == STEP_PERIMETER);
    CHECK(plugin->get_priority() == 5);

    const DynamicPrintConfig config = perimeter_config({{"perimeters", "1"}});
    const ExPolygon surface = rectangle_expolygon(-10., -10., 10., 10.);
    const PerimeterRunCapture generated =
        run_perimeter_case(config, {CLASSIC_PERIMETER_GENERATOR}, surface, 0);

    REQUIRE(external_perimeter_count(generated) > 0);
    REQUIRE_FALSE(generated.fill_surfaces.empty());
    REQUIRE_FALSE(generated.fill_no_overlap_surfaces.empty());

    // The classic skeleton is no longer expected to match the deliberately
    // simple child-area formula. It already follows the first pieces of the
    // classic "next onion" logic, so this test only protects the shared
    // perimeter-step contract: remaining no-overlap areas and fill/anchor areas
    // must form valid leaf domains for the later infill steps.
    require_leaf_fill_area_consistency(generated);
}
