#include <catch2/catch.hpp>

#include "plugin_test_helpers.hpp"
#include "test_data.hpp"

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/host/Plugin.hpp"
#include "libslic3r/Api/internal/LayerAccess.hpp"
#include "libslic3r/Api/internal/LayerRegionAccess.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/LayerRegion.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Config/PrintConfig.hpp"
#include "libslic3r/PrintObject.hpp"
#include "libslic3r/PrintRegion.hpp"
#include "libslic3r/Steps/StepGeneratePerimeter.hpp"
#include "libslic3r/Steps/StepLayerHeightGeneration.hpp"
#include "libslic3r/Steps/StepPostSlicing.hpp"
#include "libslic3r/Steps/StepSlicing.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

namespace {

using namespace Slic3r;

const char *const STANDARD_LAYER_HEIGHT_GENERATOR = "standard_layer_height_generator";
const char *const SLICE_VOLUME = "slice_volume";
const char *const SIMPLE_PERIMETER_GENERATOR = "perimeter.generator.simple";
const char *const VASE_MULTI_ISLAND_CONNECTOR = "vase.multi_island_connector";

struct Box
{
    double min_x = 0.;
    double min_y = 0.;
    double min_z = 0.;
    double max_x = 0.;
    double max_y = 0.;
    double max_z = 0.;
};

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

TriangleMesh make_boxes(const std::vector<Box> &boxes)
{
    std::vector<Vec3f> vertices;
    std::vector<Vec3i32> faces;
    vertices.reserve(boxes.size() * 8);
    faces.reserve(boxes.size() * 12);

    for (const Box &box : boxes) {
        const int base = int(vertices.size());
        vertices.push_back({float(box.min_x), float(box.min_y), float(box.min_z)});
        vertices.push_back({float(box.max_x), float(box.min_y), float(box.min_z)});
        vertices.push_back({float(box.max_x), float(box.max_y), float(box.min_z)});
        vertices.push_back({float(box.min_x), float(box.max_y), float(box.min_z)});
        vertices.push_back({float(box.min_x), float(box.min_y), float(box.max_z)});
        vertices.push_back({float(box.max_x), float(box.min_y), float(box.max_z)});
        vertices.push_back({float(box.max_x), float(box.max_y), float(box.max_z)});
        vertices.push_back({float(box.min_x), float(box.max_y), float(box.max_z)});
        const std::vector<Vec3i32> box_faces = {
            {base + 0, base + 2, base + 1}, {base + 0, base + 3, base + 2},
            {base + 4, base + 5, base + 6}, {base + 4, base + 6, base + 7},
            {base + 0, base + 1, base + 5}, {base + 0, base + 5, base + 4},
            {base + 1, base + 2, base + 6}, {base + 1, base + 6, base + 5},
            {base + 2, base + 3, base + 7}, {base + 2, base + 7, base + 6},
            {base + 3, base + 0, base + 4}, {base + 3, base + 4, base + 7}
        };
        faces.insert(faces.end(), box_faces.begin(), box_faces.end());
    }

    return TriangleMesh(std::move(vertices), std::move(faces));
}

DynamicPrintConfig vase_config()
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"layer_height", "1"},
        {"first_layer_height", "1"},
        {"spiral_vase", "0"},
        {"perimeters", "1"},
        {"fill_density", "0"},
        {"external_perimeter_extrusion_width", "0.5"},
        {"perimeter_extrusion_width", "0.5"},
        {"extrusion_width", "0.5"},
        {"first_layer_extrusion_width", "0.5"},
        {"nozzle_diameter", "0.4"}
    });
    return config;
}

struct PreparedVasePrint
{
    Model model;
    Print print;
    std::vector<std::unique_ptr<PrintRegion>> extra_regions;
};

void run_until_post_slicing(PreparedVasePrint &prepared,
                            const TriangleMesh &mesh,
                            const DynamicPrintConfig &config,
                            const bool spiral_vase)
{
    Slic3r::Test::init_print({mesh}, prepared.print, prepared.model, config);
    // Some legacy initialization paths still special-case spiral vase before
    // post-slicing gets a chance to repair the geometry. Tests initialize the
    // model normally, then enable the print-level flag immediately before the
    // plugin pipeline observes it.
    const_cast<PrintConfig &>(prepared.print.config()).spiral_vase.value = spiral_vase;
    ScopedActivePlugins active({STANDARD_LAYER_HEIGHT_GENERATOR, SLICE_VOLUME, VASE_MULTI_ISLAND_CONNECTOR});

    Orchestrator &orchestrator = Orchestrator::instance();
    Steps::StepLayerHeightGeneration::run_step(orchestrator, prepared.print);
    Steps::StepSlicing::run_step(orchestrator, prepared.print);
    const_cast<PrintConfig &>(prepared.print.config()).spiral_vase.value = spiral_vase;
    Steps::StepPostSlicing::run_step(orchestrator, prepared.print);
}

void run_until_perimeter(PreparedVasePrint &prepared,
                         const TriangleMesh &mesh,
                         const DynamicPrintConfig &config,
                         const bool spiral_vase)
{
    Slic3r::Test::init_print({mesh}, prepared.print, prepared.model, config);
    ScopedActivePlugins active({
        STANDARD_LAYER_HEIGHT_GENERATOR,
        SLICE_VOLUME,
        VASE_MULTI_ISLAND_CONNECTOR,
        SIMPLE_PERIMETER_GENERATOR
    });

    Orchestrator &orchestrator = Orchestrator::instance();
    Steps::StepLayerHeightGeneration::run_step(orchestrator, prepared.print);
    Steps::StepSlicing::run_step(orchestrator, prepared.print);
    const_cast<PrintConfig &>(prepared.print.config()).spiral_vase.value = spiral_vase;
    Steps::StepPostSlicing::run_step(orchestrator, prepared.print);
    Steps::StepGeneratePerimeter::run_step(orchestrator, prepared.print);
}

double layer_area_mm2(const Layer &layer)
{
    return unscaled(unscaled(std::abs(area(layer.lslices()))));
}

double island_area_mm2(const LayerSliceIsland &island)
{
    return unscaled(unscaled(std::abs(island.get_slice().area())));
}

double scaled_area_sum(const ExPolygons &areas)
{
    double out = 0.;
    for (const ExPolygon &area : areas)
        out += std::abs(area.area());
    return out;
}

double scaled_area_tolerance()
{
    const double side = double(scale_i(0.005));
    return side * side;
}

double scaled_layer_reconstruction_tolerance()
{
    const double side = double(scale_i(0.1));
    return side * side;
}

void require_region_slices_partition_layer(const Layer &layer)
{
    ExPolygons region_union;
    for (uint32_t region_idx = 0; region_idx < layer.region_count(); ++region_idx) {
        const ExPolygons &region_slices = layer.region(region_idx).get_raw_slices();
        for (uint32_t other_idx = region_idx + 1; other_idx < layer.region_count(); ++other_idx) {
            INFO("region overlap " << region_idx << " / " << other_idx);
            CHECK(scaled_area_sum(intersection_ex(region_slices, layer.region(other_idx).get_raw_slices())) <=
                  scaled_area_tolerance());
        }
        append(region_union, region_slices);
    }

    // Layer slices are rebuilt from raw LayerRegion slices. Their union must
    // match the layer geometry, or later island/region attachment would observe
    // a different printable domain than the one edited by the plugin.
    // The layer reconstruction applies safety cleanup, which may shave tiny
    // slivers around the bridge/region junctions. This tolerance is larger than
    // the raw-region overlap tolerance, but still far below the area of a real
    // bridge or island component.
    const ExPolygons expected_layer_slices = union_safety_offset_ex(region_union);
    CHECK(scaled_area_sum(diff_ex(expected_layer_slices, layer.lslices())) <=
          scaled_layer_reconstruction_tolerance());
    CHECK(scaled_area_sum(diff_ex(layer.lslices(), expected_layer_slices)) <=
          scaled_layer_reconstruction_tolerance());
}

} // namespace

TEST_CASE("Vase multi-island connector leaves normal slicing untouched", "[plugins][post-slicing][vase]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    // The plugin is allowed to stay active in normal print profiles. With
    // spiral_vase=false it must not connect or discard islands, otherwise normal
    // multi-part layers would be silently changed before perimeter generation.
    const TriangleMesh two_islands = make_boxes({
        {-5., -2., 0., -1., 2., 3.},
        {-0.4, -2., 0., 4.6, 2., 3.}
    });

    PreparedVasePrint prepared;
    run_until_post_slicing(prepared, two_islands, vase_config(), false);
    const Layer &first_layer = prepared.print.object(0).layer(0);
    REQUIRE(first_layer.islands().size() == 2);
}

TEST_CASE("Vase multi-island connector keeps single islands unchanged", "[plugins][post-slicing][vase]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    // A single island already satisfies the vase-mode continuity contract. The
    // plugin should simply pass it through and keep its area stable.
    const TriangleMesh one_island = make_boxes({{-5., -5., 0., 5., 5., 3.}});

    PreparedVasePrint prepared;
    run_until_post_slicing(prepared, one_island, vase_config(), true);
    const Layer &first_layer = prepared.print.object(0).layer(0);
    REQUIRE(first_layer.islands().size() == 1);
    CHECK(layer_area_mm2(first_layer) == Approx(100.).margin(0.1));
}

TEST_CASE("Vase multi-island connector bridges close islands", "[plugins][post-slicing][vase]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    // The two boxes have a 0.6 mm gap. With a 0.5 mm external perimeter width,
    // the proximity threshold is 1.0 mm, so one round-ended bridge capsule is
    // expected. The final layer should contain one island with more area than
    // the two original rectangles alone.
    const TriangleMesh close_islands = make_boxes({
        {-5., -2., 0., -1., 2., 3.},
        {-0.4, -2., 0., 4.6, 2., 3.}
    });

    PreparedVasePrint prepared;
    run_until_post_slicing(prepared, close_islands, vase_config(), true);
    const Layer &first_layer = prepared.print.object(0).layer(0);
    REQUIRE(first_layer.islands().size() == 1);
    CHECK(layer_area_mm2(first_layer) > 36.);
    CHECK(layer_area_mm2(first_layer) < 42.);
}

TEST_CASE("Vase multi-island connector keeps largest disconnected component without continuity", "[plugins][post-slicing][vase]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    // The islands are separated by more than the bridge threshold on the first
    // layer, and there is no previous layer to continue from. Vase mode cannot
    // print both disconnected groups, so the plugin keeps the largest one.
    const TriangleMesh far_islands = make_boxes({
        {-5., -2., 0., -1., 2., 3.},
        { 2., -4., 0., 8., 4., 3.}
    });

    PreparedVasePrint prepared;
    run_until_post_slicing(prepared, far_islands, vase_config(), true);
    const Layer &first_layer = prepared.print.object(0).layer(0);
    REQUIRE(first_layer.islands().size() == 1);
    CHECK(island_area_mm2(first_layer.islands().front()) == Approx(48.).margin(0.1));
}

TEST_CASE("Vase multi-island connector prefers previous-layer continuity", "[plugins][post-slicing][vase]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    // The small left island exists from the first layer. The larger right island
    // appears only above Z=1 and is too far away to bridge. Area alone would
    // choose the right island, so this proves vertical continuity is preferred.
    const TriangleMesh branching_model = make_boxes({
        {-5., -2., 0., -1., 2., 3.},
        { 2., -4., 1., 8., 4., 3.}
    });

    PreparedVasePrint prepared;
    run_until_post_slicing(prepared, branching_model, vase_config(), true);
    const PrintObject &object = prepared.print.object(0);
    REQUIRE(object.layer_count() >= 2);
    const Layer &second_layer = object.layer(1);
    REQUIRE(second_layer.islands().size() == 1);
    CHECK(island_area_mm2(second_layer.islands().front()) == Approx(16.).margin(0.1));
}

TEST_CASE("Vase multi-island connector uses minimal bridges for a close chain", "[plugins][post-slicing][vase]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    // Three islands form a chain where A-B and B-C are close. Kruskal should add
    // two bridges and avoid redundant cross-links. The observable contract is a
    // single connected island whose area is larger than the three boxes, but
    // still close to two capsule bridges rather than every island pair.
    const TriangleMesh chain = make_boxes({
        {-6.0, -1.5, 0., -3.0, 1.5, 3.},
        {-2.4, -1.5, 0.,  0.6, 1.5, 3.},
        { 1.2, -1.5, 0.,  4.2, 1.5, 3.}
    });

    PreparedVasePrint prepared;
    run_until_post_slicing(prepared, chain, vase_config(), true);
    const Layer &first_layer = prepared.print.object(0).layer(0);
    REQUIRE(first_layer.islands().size() == 1);
    CHECK(layer_area_mm2(first_layer) > 27.);
    CHECK(layer_area_mm2(first_layer) < 34.);
}

TEST_CASE("Vase multi-island connector keeps region slices disjoint across bridges", "[plugins][post-slicing][vase]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    // This case starts with two close islands owned by two different regions.
    // The bridge capsule is assigned to one region only. The other region must
    // be clipped away from that capsule so raw regions remain a clean partition
    // of the reconstructed layer.
    const TriangleMesh close_islands = make_boxes({
        {-5., -2., 0., -1., 2., 3.},
        {-0.4, -2., 0., 4.6, 2., 3.}
    });

    PreparedVasePrint prepared;
    const DynamicPrintConfig config = vase_config();
    Slic3r::Test::init_print({close_islands}, prepared.print, prepared.model, config);
    ScopedActivePlugins active({STANDARD_LAYER_HEIGHT_GENERATOR, SLICE_VOLUME, VASE_MULTI_ISLAND_CONNECTOR});

    Orchestrator &orchestrator = Orchestrator::instance();
    Steps::StepLayerHeightGeneration::run_step(orchestrator, prepared.print);
    Steps::StepSlicing::run_step(orchestrator, prepared.print);
    const_cast<PrintConfig &>(prepared.print.config()).spiral_vase.value = true;

    Layer &first_layer = prepared.print.object(0).layer(0);
    REQUIRE(first_layer.islands().size() == 2);
    REQUIRE(first_layer.region_count() == 1);
    const ExPolygon first_island = first_layer.island(0).get_slice();
    const ExPolygon second_island = first_layer.island(1).get_slice();

    prepared.extra_regions.push_back(std::make_unique<PrintRegion>(first_layer.region(0).region().config()));
    ApiInternal::LayerAccess::add_region(first_layer, *prepared.extra_regions.back());
    ApiInternal::LayerRegionAccess::slices_mutable(first_layer.region(0)) = ExPolygons{first_island};
    ApiInternal::LayerRegionAccess::slices_mutable(first_layer.region(1)) = ExPolygons{second_island};
    ApiInternal::LayerAccess::recompute_slices_from_layer_regions(first_layer);
    REQUIRE(first_layer.region_count() == 2);
    REQUIRE(first_layer.islands().size() == 2);

    Steps::StepPostSlicing::run_step(orchestrator, prepared.print);

    const Layer &post_layer = prepared.print.object(0).layer(0);
    REQUIRE(post_layer.islands().size() == 1);
    REQUIRE(post_layer.region_count() == 2);
    CHECK_FALSE(post_layer.region(0).get_raw_slices().empty());
    CHECK_FALSE(post_layer.region(1).get_raw_slices().empty());
    require_region_slices_partition_layer(post_layer);
}

TEST_CASE("Vase multi-island connector feeds perimeter generation one island", "[plugins][post-slicing][vase]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    // This is a lightweight integration check. The post-slicing plugin connects
    // the islands first, then the simple perimeter generator sees one
    // LayerSliceIsland and can publish one perimeter tree for that connected
    // slice instead of two unrelated vase candidates.
    const TriangleMesh close_islands = make_boxes({
        {-5., -2., 0., -1., 2., 3.},
        {-0.4, -2., 0., 4.6, 2., 3.}
    });

    PreparedVasePrint prepared;
    run_until_perimeter(prepared, close_islands, vase_config(), true);
    const Layer &first_layer = prepared.print.object(0).layer(0);
    REQUIRE(first_layer.islands().size() == 1);
    REQUIRE(first_layer.islands().front().regions_islands().size() == 1);
    CHECK(first_layer.islands().front().regions_islands().front().has_extrusions());
}
