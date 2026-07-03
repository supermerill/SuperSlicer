#include <catch2/catch.hpp>

#include "perimeter_test_helpers.hpp"
#include "plugin_test_helpers.hpp"

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/host/Plugin.hpp"
#include "libslic3r/Api/internal/LayerAccess.hpp"
#include "libslic3r/Api/internal/LayerRegionAccess.hpp"
#include "libslic3r/Api/plugin/c/slic3r_data_tree.h"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/LayerRegion.hpp"
#include "libslic3r/PrintObject.hpp"
#include "libslic3r/PrintRegion.hpp"
#include "libslic3r/Steps/StepGeneratePerimeter.hpp"
#include "libslic3r/Steps/StepPostSlicing.hpp"
#include "libslic3r/Steps/StepSurfaceGeneration.hpp"
#include "libslic3r/Surface.hpp"
#include "libslic3r/SurfaceCollection.hpp"

#include <cmath>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace {
using namespace Slic3r;
using namespace Slic3r::Test::PerimeterPluginTests;

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

double area_sum(const ExPolygons &areas)
{
    double out = 0.;
    for (const ExPolygon &area : areas)
        out += std::abs(area.area());
    return out;
}

double area_tolerance()
{
    const double side = double(scale_i(0.005));
    return side * side;
}

ExPolygons surface_expolygons(const SurfaceCollection &surfaces)
{
    ExPolygons out;
    out.reserve(surfaces.size());
    for (const Surface &surface : surfaces)
        if (!surface.empty())
            out.push_back(surface.expolygon);
    return out;
}

ExPolygons surface_expolygons_of_type(const SurfaceCollection &surfaces, const SurfaceType surface_type)
{
    ExPolygons out;
    out.reserve(surfaces.size());
    for (const Surface &surface : surfaces)
        if (!surface.empty() && surface.surface_type == surface_type)
            out.push_back(surface.expolygon);
    return out;
}

ExPolygons dumbbell_expolygon()
{
    ExPolygons parts;
    parts.push_back(rectangle_expolygon(-8., -4., -1., 4.));
    parts.push_back(rectangle_expolygon(-1., -0.15, 1., 0.15));
    parts.push_back(rectangle_expolygon(1., -4., 8., 4.));

    ExPolygons merged = union_ex(parts);
    REQUIRE(merged.size() == 1);
    return merged;
}

void require_same_union(const ExPolygons &actual, const ExPolygons &expected)
{
    const ExPolygons actual_union = union_ex(actual);
    const ExPolygons expected_union = union_ex(expected);
    INFO("actual area " << area_sum(actual_union) << ", expected area " << area_sum(expected_union));
    REQUIRE(area_sum(diff_ex(actual_union, expected_union)) <= area_tolerance());
    REQUIRE(area_sum(diff_ex(expected_union, actual_union)) <= area_tolerance());
}

void require_no_positive_overlap(const ExPolygons &areas)
{
    for (size_t first_idx = 0; first_idx < areas.size(); ++first_idx) {
        for (size_t second_idx = first_idx + 1; second_idx < areas.size(); ++second_idx) {
            const ExPolygons overlap = intersection_ex(
                ExPolygons{areas[first_idx]},
                ExPolygons{areas[second_idx]});
            INFO("area pair " << first_idx << " / " << second_idx);
            CHECK(area_sum(overlap) <= area_tolerance());
        }
    }
}

void require_no_positive_overlap(const SurfaceCollection &surfaces)
{
    require_no_positive_overlap(surface_expolygons(surfaces));
}

void require_initial_typed_surface_builder_types(const SurfaceCollection &surfaces)
{
    for (const Surface &surface : surfaces) {
        const bool known_type = surface.surface_type == (stPosBottom | stDensSolid) ||
                                surface.surface_type == (stPosBottom | stDensSolid | stModBridge) ||
                                surface.surface_type == (stPosInternal | stDensSparse) ||
                                surface.surface_type == (stPosTop | stDensSolid);
        CHECK(known_type);
    }
}

const LayerRegionIsland &whole_region_island(const LayerSliceIsland &island)
{
    const LayerRegionIsland *fallback = nullptr;
    for (const LayerRegionIsland &region_island : island.regions_islands()) {
        if (region_island.regions() != island.regions())
            continue;
        if (!region_island.fill_surfaces().empty())
            return region_island;
        if (fallback == nullptr)
            fallback = &region_island;
    }

    REQUIRE(fallback != nullptr);
    return *fallback;
}

LayerRegionSetCPtrs region_set(const LayerRegion &region)
{
    LayerRegionSetCPtrs out;
    out.insert(&region);
    return out;
}

const LayerRegionIsland &region_island_for(const LayerSliceIsland &island,
                                           const LayerRegionSetCPtrs &regions,
                                           const uint16_t extruder_id)
{
    const LayerRegionIsland *found = nullptr;
    for (const LayerRegionIsland &region_island : island.regions_islands())
        if (region_island.regions() == regions && region_island.extruder_id() == extruder_id) {
            found = &region_island;
            break;
        }

    REQUIRE(found != nullptr);
    return *found;
}

void require_surface_contract(const SurfaceCollection &surfaces, const ExPolygons &expected_areas)
{
    // InitialTypedSurfaceBuilder may split one fill area into several typed
    // surfaces, but it must not change the total fillable area. Later infill
    // code relies on this partition having no positive overlaps and no missing
    // pieces.
    require_initial_typed_surface_builder_types(surfaces);
    require_no_positive_overlap(surfaces);
    require_same_union(surface_expolygons(surfaces), expected_areas);
}

void require_only_surface_type(const SurfaceCollection &surfaces,
                               const SurfaceType surface_type,
                               const ExPolygons &expected_areas)
{
    require_surface_contract(surfaces, expected_areas);
    require_same_union(surface_expolygons_of_type(surfaces, surface_type), expected_areas);
}

void require_surface_contract(const LayerSliceIsland &island)
{
    require_surface_contract(whole_region_island(island).fill_surfaces(), island.infill_areas());
}

void set_region_area(LayerRegion &region, const ExPolygon &area)
{
    ExPolygons &region_slices = ApiInternal::LayerRegionAccess::slices_mutable(region);
    region_slices = ExPolygons{area};
    ApiInternal::LayerRegionAccess::surfaces_mutable(region).set(region_slices, stPosInternal | stDensSparse);
}

void replace_layer_island(Layer &layer, const ExPolygon &area)
{
    ApiInternal::LayerAccess::set_islands(layer, ExPolygons{area});
    set_region_area(layer.region(0), area);
    layer.island(0).fill_regions(layer);
}

void add_region_with_infill_extruder(PreparedPerimeterPrint &prepared,
                                     Layer &layer,
                                     const ExPolygon &area,
                                     const int infill_extruder)
{
    PrintRegionConfig config = layer.region(0).region().config();
    config.set_deserialize_strict("infill_extruder", std::to_string(infill_extruder));
    prepared.extra_regions.push_back(std::make_unique<PrintRegion>(config));
    ApiInternal::LayerAccess::add_region(layer, *prepared.extra_regions.back());
    set_region_area(layer.region(layer.region_count() - 1), area);
}

void replace_layer_island_with_two_infill_extruders(PreparedPerimeterPrint &prepared,
                                                   Layer &layer,
                                                   const ExPolygon &area)
{
    ApiInternal::LayerAccess::set_islands(layer, ExPolygons{area});
    set_region_area(layer.region(0), rectangle_expolygon(-10., -10., 0., 10.));
    add_region_with_infill_extruder(prepared, layer, rectangle_expolygon(0., -10., 10., 10.), 2);
    layer.island(0).fill_regions(layer);
}

void rebuild_island_overlap_graph(PrintObject &object)
{
    for (Layer &layer : object.layers())
        for (LayerSliceIsland &island : layer.islands()) {
            island.overlaps_above.clear();
            island.overlaps_below.clear();
        }

    for (size_t layer_idx = 1; layer_idx < object.layer_count(); ++layer_idx)
        Layer::build_up_down_graph(object.layer(layer_idx - 1), object.layer(layer_idx));
}

void run_perimeter_and_surface_steps(Print &print)
{
    Orchestrator &orchestrator = Orchestrator::instance();
    Steps::StepGeneratePerimeter::clean_and_prepare(print);
    Steps::StepGeneratePerimeter::run_step(orchestrator, print);
    Steps::StepSurfaceGeneration::clean_and_prepare(print);
    Steps::StepSurfaceGeneration::run_step(orchestrator, print);

    std::string validation_error;
    const bool valid_surface_tree = Steps::StepSurfaceGeneration::validate_post(print, &validation_error);
    INFO("Surface-generation post validation: " << validation_error);
    REQUIRE(valid_surface_tree);
}

} // namespace

TEST_CASE("LayerIsland C API creates and reuses LayerRegionIslands",
          "[plugins][surface-generation][data-tree]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    PreparedPerimeterPrint prepared;
    prepare_cube_print(prepared, perimeter_config({{"perimeters", "1"}}));
    REQUIRE(prepared.print.object(0).layer_count() > 1);
    Layer &layer = prepared.print.object(0).layer(0);
    replace_layer_island_with_two_infill_extruders(
        prepared, layer, rectangle_expolygon(-10., -10., 10., 10.));
    LayerSliceIsland &island = layer.island(0);

    const layer_region_handle *left_region =
        reinterpret_cast<const layer_region_handle *>(&layer.region(0));
    const layer_region_handle *right_region =
        reinterpret_cast<const layer_region_handle *>(&layer.region(1));
    const layer_region_handle *left_regions[] = { left_region };
    const layer_region_handle *both_regions[] = { left_region, right_region };

    layer_region_island_handle *left_first =
        layer_island_get_or_create_region_island(
            reinterpret_cast<layer_island_handle *>(&island),
            left_regions,
            1,
            0);
    REQUIRE(left_first != nullptr);

    // Same island, same region set, same extruder returns the existing bucket.
    layer_region_island_handle *left_second =
        layer_island_get_or_create_region_island(
            reinterpret_cast<layer_island_handle *>(&island),
            left_regions,
            1,
            0);
    CHECK(left_second == left_first);

    // The extruder participates in the key, so the same regions can own a
    // separate output bucket for a different tool.
    layer_region_island_handle *left_other_extruder =
        layer_island_get_or_create_region_island(
            reinterpret_cast<layer_island_handle *>(&island),
            left_regions,
            1,
            1);
    REQUIRE(left_other_extruder != nullptr);
    CHECK(left_other_extruder != left_first);

    layer_region_island_handle *full =
        layer_island_get_or_create_region_island(
            reinterpret_cast<layer_island_handle *>(&island),
            nullptr,
            0,
            -1);
    REQUIRE(full != nullptr);
    CHECK(layer_region_island_count_region(full) == island.regions().size());

    layer_region_island_handle *explicit_full =
        layer_island_get_or_create_region_island(
            reinterpret_cast<layer_island_handle *>(&island),
            both_regions,
            2,
            -1);
    CHECK(explicit_full == full);

    const layer_region_handle *foreign_region =
        reinterpret_cast<const layer_region_handle *>(&prepared.print.object(0).layer(1).region(0));
    const layer_region_handle *foreign_regions[] = { foreign_region };
    CHECK(layer_island_get_or_create_region_island(
              reinterpret_cast<layer_island_handle *>(&island),
              foreign_regions,
              1,
              0) == nullptr);
}

TEST_CASE("InitialTypedSurfaceBuilder converts island infill areas to typed region-island surfaces",
          "[plugins][surface-generation]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    SECTION("normal island")
    {
        // The first layer has no material below, but the next layer covers the
        // same XY area. InitialTypedSurfaceBuilder should therefore publish bottom
        // surfaces for the island fill area and keep the total area unchanged.
        // Because this bottom is on the first layer, it is not a bridge.
        PreparedPerimeterPrint prepared;
        prepare_cube_print(prepared, perimeter_config({{"perimeters", "1"}}));
        PrintObject &object = prepared.print.object(0);
        Layer &layer = object.layer(0);
        replace_layer_island(layer, rectangle_expolygon(-10., -10., 10., 10.));
        rebuild_island_overlap_graph(object);

        ScopedActivePlugins active({SIMPLE_PERIMETER_GENERATOR, INITIAL_TYPED_SURFACE_BUILDER});
        run_perimeter_and_surface_steps(prepared.print);

        const LayerSliceIsland &island = layer.island(0);
        REQUIRE_FALSE(island.infill_areas().empty());
        require_surface_contract(island);
        require_only_surface_type(whole_region_island(island).fill_surfaces(), stPosBottom | stDensSolid, island.infill_areas());
    }

    SECTION("middle layer covered above and below becomes internal")
    {
        // On a fully covered middle layer, every fill point has model material
        // on the adjacent lower and upper layers. It is therefore a regular
        // internal sparse surface, not top or bottom skin.
        PreparedPerimeterPrint prepared;
        prepare_cube_print(prepared, perimeter_config({{"perimeters", "1"}}));
        PrintObject &object = prepared.print.object(0);
        REQUIRE(object.layer_count() > 2);
        Layer &layer = object.layer(1);
        replace_layer_island(layer, rectangle_expolygon(-10., -10., 10., 10.));
        rebuild_island_overlap_graph(object);

        ScopedActivePlugins active({SIMPLE_PERIMETER_GENERATOR, INITIAL_TYPED_SURFACE_BUILDER});
        run_perimeter_and_surface_steps(prepared.print);

        const LayerSliceIsland &island = layer.island(0);
        REQUIRE_FALSE(island.infill_areas().empty());
        require_only_surface_type(whole_region_island(island).fill_surfaces(), stPosInternal | stDensSparse, island.infill_areas());
    }

    SECTION("untouched cube uses the post-slicing upper/lower island graph")
    {
        // This is the production-shaped case: StepSlicing and StepPostSlicing
        // created the islands, and the test does not rebuild overlap links by
        // hand. A vertical cube must have bottom skin on the first layer,
        // sparse internal middle layers, and top skin only on the last layer.
        // If the upper/lower graph is missing, every layer looks isolated and
        // the classifier turns the first layer into top and all other layers
        // into bottom bridges.
        PreparedPerimeterPrint prepared;
        prepare_cube_print(prepared, perimeter_config({{"perimeters", "1"}, {"raft_layers", "0"}}));
        PrintObject &object = prepared.print.object(0);
        REQUIRE(object.layer_count() > 2);

        {
            ScopedActivePlugins no_post_slicing_plugins({});
            Steps::StepPostSlicing::run_step(Orchestrator::instance(), prepared.print);
        }

        ScopedActivePlugins active({SIMPLE_PERIMETER_GENERATOR, INITIAL_TYPED_SURFACE_BUILDER});
        run_perimeter_and_surface_steps(prepared.print);

        const LayerSliceIsland &first_island = object.layer(0).island(0);
        const LayerSliceIsland &middle_island = object.layer(1).island(0);
        const LayerSliceIsland &top_island = object.layer(layer_index_for_top(object)).island(0);

        REQUIRE_FALSE(first_island.infill_areas().empty());
        REQUIRE_FALSE(middle_island.infill_areas().empty());
        REQUIRE_FALSE(top_island.infill_areas().empty());
        require_only_surface_type(whole_region_island(first_island).fill_surfaces(),
                                  stPosBottom | stDensSolid,
                                  first_island.infill_areas());
        require_only_surface_type(whole_region_island(middle_island).fill_surfaces(),
                                  stPosInternal | stDensSparse,
                                  middle_island.infill_areas());
        require_only_surface_type(whole_region_island(top_island).fill_surfaces(),
                                  stPosTop | stDensSolid,
                                  top_island.infill_areas());
    }

    SECTION("top layer uncovered above becomes top")
    {
        // The last model layer is covered by the layer below and has no model
        // material above. Its fill area is top solid skin.
        PreparedPerimeterPrint prepared;
        prepare_cube_print(prepared, perimeter_config({{"perimeters", "1"}}));
        PrintObject &object = prepared.print.object(0);
        const size_t top_layer_idx = layer_index_for_top(object);
        Layer &layer = object.layer(top_layer_idx);
        replace_layer_island(layer, rectangle_expolygon(-10., -10., 10., 10.));
        rebuild_island_overlap_graph(object);

        ScopedActivePlugins active({SIMPLE_PERIMETER_GENERATOR, INITIAL_TYPED_SURFACE_BUILDER});
        run_perimeter_and_surface_steps(prepared.print);

        const LayerSliceIsland &island = layer.island(0);
        REQUIRE_FALSE(island.infill_areas().empty());
        require_only_surface_type(whole_region_island(island).fill_surfaces(), stPosTop | stDensSolid, island.infill_areas());
    }

    SECTION("isolated middle island is bottom when it is both bottom and top")
    {
        // If a non-first layer has no overlapping island below or above, the
        // area is both exposed below and above. The baseline rule gives that
        // overlap to bottom surfaces so the same area is not emitted twice.
        // Since this bottom is not on the first layer, it is also a bridge.
        PreparedPerimeterPrint prepared;
        prepare_cube_print(prepared, perimeter_config({{"perimeters", "1"}}));
        PrintObject &object = prepared.print.object(0);
        REQUIRE(object.layer_count() > 2);
        replace_layer_island(object.layer(0), rectangle_expolygon(30., -10., 50., 10.));
        replace_layer_island(object.layer(1), rectangle_expolygon(-10., -10., 10., 10.));
        replace_layer_island(object.layer(2), rectangle_expolygon(30., -10., 50., 10.));
        rebuild_island_overlap_graph(object);

        ScopedActivePlugins active({SIMPLE_PERIMETER_GENERATOR, INITIAL_TYPED_SURFACE_BUILDER});
        run_perimeter_and_surface_steps(prepared.print);

        const LayerSliceIsland &island = object.layer(1).island(0);
        REQUIRE_FALSE(island.infill_areas().empty());
        require_only_surface_type(whole_region_island(island).fill_surfaces(),
                                  stPosBottom | stDensSolid | stModBridge,
                                  island.infill_areas());
    }

    SECTION("isolated first layer resolves bottom/top overlap from raft setting")
    {
        // This island has no object material below or above, so the same fill
        // area is both bottom and top. The general rule gives that overlap to
        // bottom surfaces. The first layer is the only exception: with no raft
        // it is visible top skin, while with raft it keeps the bottom result
        // because the raft handles the external support-facing side.
        struct RaftCase
        {
            int         raft_layers;
            SurfaceType expected_surface_type;
        };
        const RaftCase cases[] = {
            { 0, stPosTop | stDensSolid },
            { 1, stPosBottom | stDensSolid },
            { 3, stPosBottom | stDensSolid }
        };

        for (const RaftCase &raft_case : cases) {
            CAPTURE(raft_case.raft_layers);

            PreparedPerimeterPrint prepared;
            prepare_cube_print(prepared, perimeter_config({
                {"perimeters", "1"},
                {"raft_layers", std::to_string(raft_case.raft_layers)}
            }));
            PrintObject &object = prepared.print.object(0);
            REQUIRE(object.layer_count() > 1);
            replace_layer_island(object.layer(0), rectangle_expolygon(-10., -10., 10., 10.));
            replace_layer_island(object.layer(1), rectangle_expolygon(30., -10., 50., 10.));
            rebuild_island_overlap_graph(object);

            ScopedActivePlugins active({SIMPLE_PERIMETER_GENERATOR, INITIAL_TYPED_SURFACE_BUILDER});
            run_perimeter_and_surface_steps(prepared.print);

            const LayerSliceIsland &island = object.layer(0).island(0);
            REQUIRE_FALSE(island.infill_areas().empty());
            require_only_surface_type(whole_region_island(island).fill_surfaces(),
                                      raft_case.expected_surface_type,
                                      island.infill_areas());
        }
    }

    SECTION("partially covered middle layer splits top and internal areas")
    {
        // The layer below covers the whole island while the layer above covers
        // only the left half. The uncovered right half must become top skin and
        // the still-covered left half must remain internal sparse infill.
        PreparedPerimeterPrint prepared;
        prepare_cube_print(prepared, perimeter_config({{"perimeters", "0"}}));
        PrintObject &object = prepared.print.object(0);
        REQUIRE(object.layer_count() > 2);
        const ExPolygon full_area = rectangle_expolygon(-10., -10., 10., 10.);
        replace_layer_island(object.layer(0), full_area);
        replace_layer_island(object.layer(1), full_area);
        replace_layer_island(object.layer(2), rectangle_expolygon(-10., -10., 0., 10.));
        rebuild_island_overlap_graph(object);

        ScopedActivePlugins active({SIMPLE_PERIMETER_GENERATOR, INITIAL_TYPED_SURFACE_BUILDER});
        run_perimeter_and_surface_steps(prepared.print);

        const LayerSliceIsland &island = object.layer(1).island(0);
        const SurfaceCollection &surfaces = whole_region_island(island).fill_surfaces();
        require_surface_contract(surfaces, island.infill_areas());
        require_same_union(surface_expolygons_of_type(surfaces, stPosTop | stDensSolid),
                           diff_ex(island.infill_areas(), ExPolygons{object.layer(2).island(0).get_slice()}));
        require_same_union(surface_expolygons_of_type(surfaces, stPosInternal | stDensSparse),
                           intersection_ex(island.infill_areas(), ExPolygons{object.layer(2).island(0).get_slice()}));
    }

    SECTION("perimeters consumed the whole island")
    {
        // This very narrow island can receive a perimeter line, but its inner
        // area disappears after the perimeter offset. Surface generation must
        // not resurrect the original island as infill; after host cleanup there
        // may be no fill-only LayerRegionIsland left at all.
        PreparedPerimeterPrint prepared;
        prepare_cube_print(prepared, perimeter_config({{"perimeters", "1"}}));
        PrintObject &object = prepared.print.object(0);
        Layer &layer = object.layer(0);
        replace_layer_island(layer, rectangle_expolygon(-0.1, -8., 0.1, 8.));
        rebuild_island_overlap_graph(object);

        ScopedActivePlugins active({SIMPLE_PERIMETER_GENERATOR, INITIAL_TYPED_SURFACE_BUILDER});
        run_perimeter_and_surface_steps(prepared.print);

        const LayerSliceIsland &island = layer.island(0);
        CHECK(island.infill_areas().empty());
        for (const LayerRegionIsland &region_island : island.regions_islands())
            CHECK(region_island.fill_surfaces().empty());
    }

    SECTION("one island split into multiple fill expolygons")
    {
        // The dumbbell starts as one island, but the thin neck is too narrow
        // to survive the inner perimeter offset. Perimeter generation publishes
        // multiple fill ExPolygons, and surface generation must preserve that
        // partition exactly: no overlaps and no missing area.
        PreparedPerimeterPrint prepared;
        prepare_cube_print(prepared, perimeter_config({{"perimeters", "1"}}));
        PrintObject &object = prepared.print.object(0);
        Layer &layer = object.layer(0);
        const ExPolygons island_area = dumbbell_expolygon();
        replace_layer_island(layer, island_area.front());
        rebuild_island_overlap_graph(object);

        ScopedActivePlugins active({SIMPLE_PERIMETER_GENERATOR, INITIAL_TYPED_SURFACE_BUILDER});
        run_perimeter_and_surface_steps(prepared.print);

        const LayerSliceIsland &island = layer.island(0);
        REQUIRE(island.infill_areas().size() >= 2);
        require_surface_contract(island);
    }

    SECTION("one island with two infill extruders")
    {
        // The perimeter generator still works on the full island, but infill
        // surfaces are later consumed by an infill extruder. When two regions
        // inside the same island use different infill extruders, the surface
        // step must create one LayerRegionIsland per extruder and clip the
        // island fill areas back to the regions owned by that extruder.
        PreparedPerimeterPrint prepared;
        prepare_cube_print(prepared, perimeter_config({{"perimeters", "1"}, {"nozzle_diameter", "0.4,0.4"}}));
        PrintObject &object = prepared.print.object(0);
        Layer &layer = object.layer(0);
        const ExPolygon island_area = rectangle_expolygon(-10., -10., 10., 10.);
        replace_layer_island_with_two_infill_extruders(prepared, layer, island_area);
        rebuild_island_overlap_graph(object);

        ScopedActivePlugins active({SIMPLE_PERIMETER_GENERATOR, INITIAL_TYPED_SURFACE_BUILDER});
        run_perimeter_and_surface_steps(prepared.print);

        const LayerSliceIsland &island = layer.island(0);
        REQUIRE_FALSE(island.infill_areas().empty());

        const LayerRegionIsland &left_infill =
            region_island_for(island, region_set(layer.region(0)), uint16_t(0));
        const LayerRegionIsland &right_infill =
            region_island_for(island, region_set(layer.region(1)), uint16_t(1));

        const ExPolygons expected_left = intersection_ex(island.infill_areas(), layer.region(0).get_raw_slices());
        const ExPolygons expected_right = intersection_ex(island.infill_areas(), layer.region(1).get_raw_slices());
        require_surface_contract(left_infill.fill_surfaces(), expected_left);
        require_surface_contract(right_infill.fill_surfaces(), expected_right);

        ExPolygons combined = surface_expolygons(left_infill.fill_surfaces());
        append(combined, surface_expolygons(right_infill.fill_surfaces()));
        require_no_positive_overlap(combined);
        require_same_union(combined, island.infill_areas());
    }

    SECTION("zero requested perimeters keeps the whole island as fill")
    {
        // With perimeters=0, perimeter generation does not consume the island.
        // The original island area is still the fill area, and the surface step
        // must convert it like any other perimeter output.
        PreparedPerimeterPrint prepared;
        prepare_cube_print(prepared, perimeter_config({{"perimeters", "0"}}));
        PrintObject &object = prepared.print.object(0);
        Layer &layer = object.layer(0);
        const ExPolygon island_area = rectangle_expolygon(-10., -10., 10., 10.);
        replace_layer_island(layer, island_area);
        rebuild_island_overlap_graph(object);

        ScopedActivePlugins active({SIMPLE_PERIMETER_GENERATOR, INITIAL_TYPED_SURFACE_BUILDER});
        run_perimeter_and_surface_steps(prepared.print);

        const LayerSliceIsland &island = layer.island(0);
        require_surface_contract(island);
        require_same_union(surface_expolygons(whole_region_island(island).fill_surfaces()), ExPolygons{island_area});
    }
}
