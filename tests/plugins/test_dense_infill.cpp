#include <catch2/catch.hpp>

#include "perimeter_test_helpers.hpp"
#include "plugin_test_helpers.hpp"

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/host/Plugin.hpp"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_infill.h"
#include "libslic3r/Api/plugin/cpp/SurfaceViews.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionProperty.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/LayerRegion.hpp"
#include "libslic3r/PrintObject.hpp"
#include "libslic3r/Steps/StepGeneratePerimeter.hpp"
#include "libslic3r/Steps/StepPostInfillGeneration.hpp"
#include "libslic3r/Steps/StepSurfaceGeneration.hpp"
#include "libslic3r/Surface.hpp"
#include "libslic3r/SurfaceCollection.hpp"
#include "plugins_cpp/DenseInfill/DenseInfill.hpp"

#include <cmath>
#include <cstdlib>
#include <initializer_list>
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

DynamicPrintConfig dense_infill_config(std::initializer_list<std::pair<std::string, std::string>> overrides)
{
    DynamicPrintConfig config = perimeter_config({
        {"perimeters", "0"},
        {"top_solid_layers", "0"},
        {"bottom_solid_layers", "0"},
        {"top_solid_min_thickness", "0"},
        {"bottom_solid_min_thickness", "0"},
        {"solid_over_perimeters", "0"},
        {"infill_dense", "1"},
        {"infill_dense_algo", "automatic"},
        {"fill_density", "20%"},
        {"external_infill_margin", "0.5"}
    });
    for (const std::pair<std::string, std::string> &entry : overrides)
        config.set_deserialize_strict(entry.first, entry.second);
    return config;
}

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

void require_same_union(const ExPolygons &actual, const ExPolygons &expected)
{
    const ExPolygons actual_union = union_ex(actual);
    const ExPolygons expected_union = union_ex(expected);
    INFO("actual area " << area_sum(actual_union) << ", expected area " << area_sum(expected_union));
    REQUIRE(area_sum(diff_ex(actual_union, expected_union)) <= area_tolerance());
    REQUIRE(area_sum(diff_ex(expected_union, actual_union)) <= area_tolerance());
}

void require_no_positive_overlap(const SurfaceCollection &surfaces)
{
    const ExPolygons areas = surface_expolygons(surfaces);
    for (size_t first_idx = 0; first_idx < areas.size(); ++first_idx) {
        for (size_t second_idx = first_idx + 1; second_idx < areas.size(); ++second_idx) {
            const ExPolygons overlap = intersection_ex(ExPolygons{areas[first_idx]}, ExPolygons{areas[second_idx]});
            INFO("surface pair " << first_idx << " / " << second_idx);
            CHECK(area_sum(overlap) <= area_tolerance());
        }
    }
}

const LayerRegionIsland &first_non_empty_region_island(const LayerSliceIsland &island)
{
    for (const LayerRegionIsland &region_island : island.regions_islands())
        if (!region_island.fill_surfaces().empty())
            return region_island;

    FAIL("No LayerRegionIsland with fill surfaces");
    std::abort();
}

slic3r_api::PluginPropertyKey<slic3r_api::DenseInfillPlugin::SurfaceDenseInfillHint>
dense_hint_property_key()
{
    orchestrator_handle *orchestrator =
        reinterpret_cast<orchestrator_handle *>(&Orchestrator::instance());
    return slic3r_api::PluginPropertyKey<
        slic3r_api::DenseInfillPlugin::SurfaceDenseInfillHint>::register_dynamic(
            orchestrator,
            slic3r_api::DenseInfillPlugin::DENSE_INFILL_HINT_PROPERTY_NAME);
}

size_t dense_hint_count(const PrintObject &object)
{
    const slic3r_api::PluginPropertyKey<slic3r_api::DenseInfillPlugin::SurfaceDenseInfillHint>
        hint_property = dense_hint_property_key();
    size_t out = 0;
    for (const Layer &layer : object.layers())
        for (const LayerSliceIsland &island : layer.islands())
            for (const LayerRegionIsland &region_island : island.regions_islands())
                for (const Surface &surface : region_island.fill_surfaces())
                    if (hint_property.get(slic3r_api::Surface(
                            reinterpret_cast<const surface_handle *>(&surface)).properties()) != nullptr)
                        ++out;
    return out;
}

void run_dense_surface_pipeline(PreparedPerimeterPrint &prepared)
{
    // Dense infill is a surface-generation refinement: the initial builder
    // creates top/bottom/internal surfaces first, then this plugin splits the
    // sparse surfaces that sit immediately below solid material above them.
    ScopedActivePlugins active({
        SIMPLE_PERIMETER_GENERATOR,
        INITIAL_TYPED_SURFACE_BUILDER,
        DENSE_INFILL_SURFACE_MARKER
    });
    Orchestrator &orchestrator = Orchestrator::instance();
    Steps::StepGeneratePerimeter::clean_and_prepare(prepared.print);
    Steps::StepGeneratePerimeter::run_step(orchestrator, prepared.print);
    Steps::StepSurfaceGeneration::clean_and_prepare(prepared.print);
    Steps::StepSurfaceGeneration::run_step(orchestrator, prepared.print);

    std::string validation_error;
    const bool valid_surface_tree = Steps::StepSurfaceGeneration::validate_post(prepared.print, &validation_error);
    INFO("Surface-generation post validation: " << validation_error);
    REQUIRE(valid_surface_tree);
}

void run_recipe_modifier_on_surface(Surface &surface, raw_infill_pattern_params &params)
{
    Orchestrator &orchestrator = Orchestrator::instance();
    Plugin *plugin = orchestrator.get_plugin(DENSE_INFILL_RECIPE_MODIFIER);
    REQUIRE(plugin != nullptr);

    plugin_host_context host_context =
        orchestrator.prepare_plugin_host_context(INFILL_SURFACE_RECIPE_MODIFIER, plugin, nullptr);
    plugin_run_context run_context =
        orchestrator.prepare_plugin_run_context(INFILL_SURFACE_RECIPE_MODIFIER, plugin, &host_context);

    run_ctx_infill_surface_recipe_modifier payload = {};
    payload.surface = reinterpret_cast<const surface_handle *>(&surface);
    payload.params = &params;
    run_context.data = &payload;

    plugin->run(run_context);
}

Surface &append_post_infill_test_surface(LayerRegionIsland &region_island,
                                         const ExPolygon &area,
                                         const uint16_t dense_priority)
{
    Surface &surface =
        region_island.set_fill_surfaces().surfaces.emplace_back(stPosInternal | stDensSparse, area);
    if (dense_priority != 0) {
        const slic3r_api::PluginPropertyKey<slic3r_api::DenseInfillPlugin::SurfaceDenseInfillHint>
            hint_property = dense_hint_property_key();
        slic3r_api::MutableSurface surface_view(reinterpret_cast<surface_handle *>(&surface));
        slic3r_api::DenseInfillPlugin::SurfaceDenseInfillHint &hint =
            hint_property.get_or_add(surface_view.mutable_properties());
        hint.max_solid_layers_on_top = 1;
        hint.priority = dense_priority;
    }
    return surface;
}

ExtrusionPath post_infill_test_path(const uint64_t source_surface_id, const double y_mm)
{
    ExtrusionPath path(ExtrusionAttributes(ExtrusionRole::InternalInfill, ExtrusionFlow(0.1, 0.4f, 0.2f)), nullptr, true);
    path.polyline().append(Point(scale_i(-4.), scale_i(y_mm)));
    path.polyline().append(Point(scale_i(4.), scale_i(y_mm)));
    path.get_or_add_property<ExtrusionPropertyInfill>(source_surface_id);
    return path;
}

uint64_t required_infill_surface_id(const ExtrusionEntity &entity)
{
    const ExtrusionPropertyInfill *property = entity.get_property<ExtrusionPropertyInfill>();
    REQUIRE(property != nullptr);
    return property->source_surface_id;
}

const ExtrusionEntity &required_group_child(const ExtrusionEntity &entity)
{
    REQUIRE_FALSE(entity.is_leaf());
    REQUIRE(entity.child_count() > 0);
    return entity;
}

} // namespace

TEST_CASE("Dense infill declares its processing dependencies",
          "[plugins][dense-infill][dependencies]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Slic3r::Orchestrator &orchestrator = Slic3r::Orchestrator::instance();
    const Slic3r::Plugin *marker = orchestrator.get_plugin("dense_infill.surface_marker");
    const Slic3r::Plugin *recipe = orchestrator.get_plugin("dense_infill.recipe_modifier");
    const Slic3r::Plugin *order = orchestrator.get_plugin("dense_infill.post_infill_order");
    REQUIRE(marker != nullptr);
    REQUIRE(recipe != nullptr);
    REQUIRE(order != nullptr);
    CHECK(marker->get_dependencies().empty());
    CHECK(recipe->get_dependencies() == std::vector<std::string>{"dense_infill.surface_marker"});
    CHECK(order->get_dependencies() == std::vector<std::string>{
        "dense_infill.recipe_modifier", "dense_infill.surface_marker"});
}

TEST_CASE("Dense infill blocks incomplete activation transitively",
          "[plugins][dense-infill][dependencies]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    const int mask = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    ScopedActivePlugins active_plugins({});
    Slic3r::Orchestrator &orchestrator = Slic3r::Orchestrator::instance();
    const bool marker = (mask & 1) != 0;
    const bool recipe = (mask & 2) != 0;
    const bool order = (mask & 4) != 0;
    REQUIRE(orchestrator.set_plugin_active("dense_infill.surface_marker", marker));
    REQUIRE(orchestrator.set_plugin_active("dense_infill.recipe_modifier", recipe));
    REQUIRE(orchestrator.set_plugin_active("dense_infill.post_infill_order", order));
    const std::vector<std::string> errors = orchestrator.active_plugin_dependency_errors();
    const size_t expected = size_t(recipe && !marker) + size_t(order && !marker) + size_t(order && !recipe);
    CHECK(errors.size() == expected);
    for (const std::string &error : errors) {
        CHECK(error.find("dense_infill.") != std::string::npos);
        CHECK(error.find("inactive") != std::string::npos);
    }
    CHECK(orchestrator.is_plugin_active("dense_infill.surface_marker") == marker);
    CHECK(orchestrator.is_plugin_active("dense_infill.recipe_modifier") == recipe);
    CHECK(orchestrator.is_plugin_active("dense_infill.post_infill_order") == order);
    orchestrator.block_unsatisfied_plugin_dependencies();
    CHECK(orchestrator.is_plugin_active("dense_infill.surface_marker") == (mask == 7));
    CHECK(orchestrator.is_plugin_active("dense_infill.recipe_modifier") == (mask == 7));
    CHECK(orchestrator.is_plugin_active("dense_infill.post_infill_order") == (mask == 7));
    CHECK(orchestrator.active_plugin_dependency_errors().empty());
    CHECK(orchestrator.blocked_plugin_activations().size() ==
          (mask == 7 ? 0 : size_t(marker) + size_t(recipe) + size_t(order)));
    for (const auto &[id, reason] : orchestrator.blocked_plugin_activations()) {
        CHECK(reason.find(id) != std::string::npos);
        CHECK_FALSE(orchestrator.is_plugin_active(id));
    }
}

TEST_CASE("Dense infill resolves dependency closure before activation",
          "[plugins][dense-infill][dependencies]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    ScopedActivePlugins active_plugins({});
    Slic3r::Orchestrator &orchestrator = Slic3r::Orchestrator::instance();
    std::vector<std::string> closure;
    std::string error;
    REQUIRE(orchestrator.plugin_dependency_closure({"dense_infill.post_infill_order"}, closure, error));
    CHECK(closure == std::vector<std::string>{"dense_infill.post_infill_order", "dense_infill.recipe_modifier", "dense_infill.surface_marker"});
    CHECK(orchestrator.active_plugins().empty());
    REQUIRE(orchestrator.validate_plugin_activation(closure, error));
    CHECK_FALSE(orchestrator.validate_plugin_activation({"dense_infill.post_infill_order"}, error));
    CHECK(error.find("dense_infill.recipe_modifier") != std::string::npos);
    REQUIRE_FALSE(orchestrator.plugin_dependency_closure({"dense_infill.post_infill_order", "missing.plugin"}, closure, error));
    CHECK(closure.empty());
    CHECK(error.find("missing.plugin") != std::string::npos);
    REQUIRE(orchestrator.plugin_dependency_closure({"dense_infill.post_infill_order"}, closure, error));
    // Lexical order intentionally requests the consumer before its dependencies.
    for (const std::string &id : closure) REQUIRE(orchestrator.set_plugin_active(id, true));
    orchestrator.block_unsatisfied_plugin_dependencies();
    CHECK(orchestrator.active_plugins().size() == 3);
    CHECK(orchestrator.blocked_plugin_activations().empty());
}

TEST_CASE("Dense infill marks sparse areas under upper solid surfaces",
          "[plugins][dense-infill]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    // A plain cube has a top solid layer. With top_solid_layers disabled, the
    // layer immediately below it starts as ordinary sparse internal infill.
    // DenseInfillSurfaceMarker should split/retype that layer into dense
    // sparse bridge-support surfaces while preserving the island fill area.
    PreparedPerimeterPrint prepared;
    prepare_cube_print(prepared, dense_infill_config({}));

    run_dense_surface_pipeline(prepared);

    const PrintObject &object = prepared.print.object(0);
    CHECK(dense_hint_count(object) > 0);

    const size_t below_top_idx = layer_index_for_top(object) - 1;
    const LayerSliceIsland &island = object.layer(below_top_idx).island(0);
    const SurfaceCollection &surfaces = first_non_empty_region_island(island).fill_surfaces();
    require_no_positive_overlap(surfaces);
    require_same_union(surface_expolygons(surfaces), island.infill_areas());

    bool saw_dense_surface = false;
    const slic3r_api::PluginPropertyKey<slic3r_api::DenseInfillPlugin::SurfaceDenseInfillHint>
        hint_property = dense_hint_property_key();
    for (const Surface &surface : surfaces) {
        const slic3r_api::DenseInfillPlugin::SurfaceDenseInfillHint *hint =
            hint_property.get(slic3r_api::Surface(
                reinterpret_cast<const surface_handle *>(&surface)).properties());
        if (hint == nullptr)
            continue;
        saw_dense_surface = true;
        CHECK(surface.surface_type == (stPosInternal | stDensSparse | stModBridge));
        CHECK(hint->max_solid_layers_on_top == 1);
        CHECK(hint->priority >= 1);
    }
    CHECK(saw_dense_surface);
}

TEST_CASE("Dense infill surface marker is a no-op when disabled or too dense already",
          "[plugins][dense-infill]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    SECTION("disabled kill switch")
    {
        // The user-facing kill switch must be cheap and total: when disabled,
        // the plugin may run in the pipeline, but it must not attach dense
        // hints or alter the surface partition.
        PreparedPerimeterPrint prepared;
        prepare_cube_print(prepared, dense_infill_config({{"infill_dense", "0"}}));

        run_dense_surface_pipeline(prepared);

        CHECK(dense_hint_count(prepared.print.object(0)) == 0);
    }

    SECTION("already dense sparse infill")
    {
        // The legacy dense-infill feature is intended only for low sparse
        // densities. At 40% and above it should not create extra dense patches.
        PreparedPerimeterPrint prepared;
        prepare_cube_print(prepared, dense_infill_config({{"fill_density", "45%"}}));

        run_dense_surface_pipeline(prepared);

        CHECK(dense_hint_count(prepared.print.object(0)) == 0);
    }
}

TEST_CASE("Dense infill supports the legacy algorithm choices without breaking surface invariants",
          "[plugins][dense-infill]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    const std::vector<std::string> algorithms = {
        "automatic",
        "autonotfull",
        "autoenlarged",
        "autosmall",
        "enlarged"
    };

    for (const std::string &algorithm : algorithms) {
        DYNAMIC_SECTION("algorithm " << algorithm)
        {
            // This is a broad contract test for the enum bridge. Each legacy
            // value must be accepted by the plugin and keep the surface tree
            // valid, even though the exact dense patch may differ by mode.
            PreparedPerimeterPrint prepared;
            prepare_cube_print(prepared, dense_infill_config({{"infill_dense_algo", algorithm}}));

            run_dense_surface_pipeline(prepared);

            const PrintObject &object = prepared.print.object(0);
            const size_t below_top_idx = layer_index_for_top(object) - 1;
            const LayerSliceIsland &island = object.layer(below_top_idx).island(0);
            const SurfaceCollection &surfaces = first_non_empty_region_island(island).fill_surfaces();
            require_no_positive_overlap(surfaces);
            require_same_union(surface_expolygons(surfaces), island.infill_areas());
        }
    }
}

TEST_CASE("Dense infill recipe modifier changes only marked surfaces",
          "[plugins][dense-infill]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    // The recipe modifier is the glue between the surface marker and pattern
    // generation. It should read only the property attached to the Surface and
    // leave unmarked surfaces untouched.
    Surface dense_surface(stPosInternal | stDensSparse, rectangle_expolygon(-5., -5., 5., 5.));
    const slic3r_api::PluginPropertyKey<slic3r_api::DenseInfillPlugin::SurfaceDenseInfillHint>
        hint_property = dense_hint_property_key();
    slic3r_api::MutableSurface dense_surface_view(
        reinterpret_cast<surface_handle *>(&dense_surface));
    slic3r_api::DenseInfillPlugin::SurfaceDenseInfillHint &hint =
        hint_property.get_or_add(dense_surface_view.mutable_properties());
    hint.priority = 4;

    raw_infill_pattern_params dense_params = {};
    dense_params.density = 0.15f;
    dense_params.priority = 0;
    run_recipe_modifier_on_surface(dense_surface, dense_params);

    CHECK(dense_params.density == Approx(0.5f));
    CHECK(dense_params.priority == 4);

    Surface sparse_surface(stPosInternal | stDensSparse, rectangle_expolygon(-5., -5., 5., 5.));
    raw_infill_pattern_params sparse_params = {};
    sparse_params.density = 0.15f;
    sparse_params.priority = 0;
    run_recipe_modifier_on_surface(sparse_surface, sparse_params);

    CHECK(sparse_params.density == Approx(0.15f));
    CHECK(sparse_params.priority == 0);
}

TEST_CASE("Dense infill post-process prints dense surface buckets in priority order",
          "[plugins][dense-infill]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    // This test builds the state that STEP_INFILL normally leaves behind:
    // every generated infill subtree is a child of a LayerRegionIsland infill
    // root and carries the id of the Surface that produced it. The dense
    // post-process must remove the marked dense subtrees from their original
    // roots, merge them into one destination root through the step callback,
    // and append non-sortable priority groups after normal infill.
    PreparedPerimeterPrint prepared;
    prepare_cube_print(prepared, dense_infill_config({}));

    PrintObject &object = prepared.print.object(0);
    LayerSliceIsland &island = object.layer(0).island(0);
    REQUIRE_FALSE(island.regions().empty());

    LayerRegionIsland &destination_region_island =
        island.get_or_add_region_island(island.regions(), 0);
    LayerRegionIsland &secondary_region_island =
        island.add_region_island(island.regions(), 1);

    const uint64_t normal_surface_id =
        append_post_infill_test_surface(
            destination_region_island,
            rectangle_expolygon(-8., -8., -5., -5.),
            0).id();
    const uint64_t low_priority_first_id =
        append_post_infill_test_surface(
            destination_region_island,
            rectangle_expolygon(-4., -8., -1., -5.),
            2).id();
    const uint64_t low_priority_second_id =
        append_post_infill_test_surface(
            destination_region_island,
            rectangle_expolygon(1., -8., 4., -5.),
            2).id();
    const uint64_t high_priority_id =
        append_post_infill_test_surface(
            secondary_region_island,
            rectangle_expolygon(5., -8., 8., -5.),
            4).id();

    ExtrusionEntityCollection &destination_root =
        destination_region_island.mutable_extrusion(LayerRegionIsland::INFILLS);
    destination_root.append(post_infill_test_path(normal_surface_id, -3.));
    destination_root.append(post_infill_test_path(low_priority_first_id, -2.));
    destination_root.append(post_infill_test_path(low_priority_second_id, -1.));

    ExtrusionEntityCollection &secondary_root =
        secondary_region_island.mutable_extrusion(LayerRegionIsland::INFILLS);
    secondary_root.append(post_infill_test_path(high_priority_id, 0.));

    {
        ScopedActivePlugins active({DENSE_INFILL_POST_INFILL_ORDER});
        Steps::StepPostInfillGeneration::run_step(Orchestrator::instance(), prepared.print);
    }

    REQUIRE(destination_region_island.has_extrusion(LayerRegionIsland::INFILLS));
    const ExtrusionEntityCollection &rebuilt_root =
        destination_region_island.extrusion(LayerRegionIsland::INFILLS);
    REQUIRE(rebuilt_root.child_count() == 3);
    CHECK_FALSE(rebuilt_root.can_sort());

    // Normal infill is not part of the dense ordering contract, so it stays as
    // an ordinary child before the dense groups. The dense groups themselves
    // are non-sortable collections: one for priority 2, then one for priority 4.
    CHECK(required_infill_surface_id(rebuilt_root.child(0)) == normal_surface_id);

    const ExtrusionEntity &low_priority_group =
        required_group_child(rebuilt_root.child(1));
    REQUIRE(low_priority_group.child_count() == 2);
    CHECK_FALSE(low_priority_group.can_sort());
    CHECK_FALSE(low_priority_group.can_reverse());
    CHECK(required_infill_surface_id(low_priority_group.child(0)) == low_priority_first_id);
    CHECK(required_infill_surface_id(low_priority_group.child(1)) == low_priority_second_id);

    const ExtrusionEntity &high_priority_group =
        required_group_child(rebuilt_root.child(2));
    REQUIRE(high_priority_group.child_count() == 1);
    CHECK_FALSE(high_priority_group.can_sort());
    CHECK_FALSE(high_priority_group.can_reverse());
    CHECK(required_infill_surface_id(high_priority_group.child(0)) == high_priority_id);

    // The high-priority dense child started in another LayerRegionIsland. After
    // the callback-based merge, its source bucket no longer owns infill
    // extrusions; the surface metadata may remain there for provenance.
    CHECK_FALSE(secondary_region_island.has_extrusion(LayerRegionIsland::INFILLS));
}
