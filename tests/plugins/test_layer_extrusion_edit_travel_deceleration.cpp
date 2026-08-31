#include <catch2/catch.hpp>

#include <algorithm>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#include "plugin_test_helpers.hpp"
#include "test_data.hpp"

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/host/Plugin.hpp"
#include "libslic3r/Api/plugin/cpp/AuxiliaryLayerHelpers.hpp"
#include "libslic3r/Api/plugin/cpp/ExtrusionViews.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/ExtrusionProperty.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Printing/PrintingPlan.hpp"
#include "libslic3r/Steps/StepLayerExtrusionEdition.hpp"

/*
Travel target-deceleration plugin tests
=======================================

These tests begin with an ordered PrintingPlan whose speed and acceleration
properties have already been resolved. This isolates the post-processor from
the providers it follows. The fixtures exercise direct and inherited process
values, plus Travel runs spanning several trees, while verifying that target
deceleration preserves travel speed and every unrelated process field.
*/

namespace {

using namespace Slic3r;
using namespace Slic3r::Printing;

constexpr const char *TRAVEL_DECELERATION_PLUGIN =
    "layer_extrusion_edit.travel_deceleration.default";
constexpr const char *ACCELERATION_PLUGIN = "layer_extrusion_edit.acceleration.default";

// Preserve the process-wide active plugin list while a test runs this single
// post-processor through the normal STEP_LAYER_EXTRUSION_EDIT scheduler.
class ScopedActivePlugins
{
public:
    explicit ScopedActivePlugins(std::initializer_list<const char *> plugin_ids) :
        m_orchestrator(Orchestrator::instance())
    {
        for (Plugin *plugin : m_orchestrator.active_plugins())
            m_previous.push_back(plugin);
        m_orchestrator.clear_active_plugins();
        for (const char *plugin_id : plugin_ids)
            REQUIRE(m_orchestrator.set_plugin_active(plugin_id, true));
    }

    ~ScopedActivePlugins()
    {
        m_orchestrator.clear_active_plugins();
        for (Plugin *plugin : m_previous)
            m_orchestrator.set_plugin_active(plugin, true);
    }

private:
    Orchestrator &m_orchestrator;
    std::vector<Plugin *> m_previous;
};

struct ProcessState
{
    ExtrusionRole role = ExtrusionRole::None;
    float speed = -1.f;
    float acceleration = -1.f;
};

struct ObservedLeaf
{
    const ExtrusionEntity *entity = nullptr;
    ExtrusionRole role = ExtrusionRole::None;
    float speed = -1.f;
    float acceleration = -1.f;
};

struct PreparedTravelDecelerationPrint
{
    Model model;
    Print print;
    PluginStorage storage;
    const LayerRegionIsland *source_region_island = nullptr;
};

struct TravelSpec
{
    double length_mm = 0.0;
    float speed = 100.f;
    float acceleration = 2000.f;
};

// Create one movement whose process fields already represent the output of
// the speed and acceleration plugins.
std::unique_ptr<ExtrusionPath> make_movement(const ExtrusionRole role,
                                             const double start_x_mm,
                                             const double end_x_mm,
                                             const float speed,
                                             const float acceleration,
                                             const bool add_process_property = true)
{
    const double flow = role == ExtrusionRole::Travel ? 0.0 : 0.2;
    ArcPolyline polyline;
    polyline.append(Point(scale_i(start_x_mm), scale_i(0.)));
    polyline.append(Point(scale_i(end_x_mm), scale_i(0.)));
    std::unique_ptr<ExtrusionPath> path = std::make_unique<ExtrusionPath>(
        polyline,
        ExtrusionAttributes(role, ExtrusionFlow(flow, 0.4f, 0.2f)),
        nullptr,
        true);
    if (add_process_property)
        path->add_property(ExtrusionPropertySpeed(speed, acceleration, 0.12f, 37.f, 215.f));
    return path;
}

// Append one independently owned extrusion root in final execution order.
ExtrusionEntity &append_movement(PrintingToolGroup &tool,
                                 const LayerRegionIsland &source,
                                 std::unique_ptr<ExtrusionEntity> root)
{
    PrintingExtrusion extrusion;
    extrusion.region_island = &source;
    extrusion.sregion_island_role = root->role();
    extrusion.root = std::move(root);
    ExtrusionEntity &result = *extrusion.root;
    tool.extrusions.push_back(std::move(extrusion));
    return result;
}

// Configure only the settings consumed by the target-deceleration plugin. The
// Print owns a complete default config even when its plan is assembled by hand.
void prepare_source_context(PreparedTravelDecelerationPrint &prepared,
                            const bool enabled,
                            const char *minimum_length)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"travel_deceleration_use_target", enabled ? "1" : "0"},
        {"travel_acceleration", "2000"},
        {"machine_limits_usage", "ignore"},
        {"gcode_min_length", minimum_length}
    });
    Slic3r::Test::init_print(
        {Slic3r::make_cube(20., 20., 20.)}, prepared.print, prepared.model, config);

    const ExPolygons subject{ExPolygon(Polygon({
        Point(scale_i(10.), scale_i(10.)),
        Point(scale_i(30.), scale_i(10.)),
        Point(scale_i(30.), scale_i(30.)),
        Point(scale_i(10.), scale_i(30.))
    }))};
    const slic3r_api::AuxiliaryLayerBuildResult layer =
        slic3r_api::build_auxiliary_layer_regions_from_subject(
            reinterpret_cast<storage_handle *>(&prepared.storage),
            slic3r_api::Print(reinterpret_cast<const print_handle *>(&prepared.print)),
            slic3r_api::Object(reinterpret_cast<const object_handle *>(&prepared.print.object(0))),
            slic3r_api::ExPolygonCollection(
                reinterpret_cast<const expolygon_collection_handle *>(&subject)),
            scale_i(0.2), scale_i(0.2), scale_i(0.1));
    REQUIRE(layer.created);
    REQUIRE(layer.layer.island_count() == 1);

    layer_region_island_handle *region_island = layer_island_get_or_create_region_island(
        const_cast<layer_island_handle *>(layer.layer.island(0).handle()), nullptr, 0, 0);
    REQUIRE(region_island != nullptr);
    prepared.source_region_island = reinterpret_cast<const LayerRegionIsland *>(region_island);
    REQUIRE_FALSE(prepared.source_region_island->regions().empty());
}

// Create the canonical previous movement, long travel, and slower target. The
// travel is long enough for the legacy kinematics to produce two phases.
PrintingLayerGroup &prepare_three_movement_plan(PreparedTravelDecelerationPrint &prepared,
                                                const double travel_length_mm = 100.)
{
    REQUIRE(prepared.source_region_island != nullptr);
    PrintingPlan &plan = prepared.print.mutable_printing_plan();
    plan.groups.emplace_back();
    plan.groups.back().layers.emplace_back();
    PrintingLayerGroup &layer = plan.groups.back().layers.back();
    layer.print_z = scale_i(0.2);
    layer.tool_groups.emplace_back();
    PrintingToolGroup &tool = layer.tool_groups.back();
    tool.extruder_id = 0;

    append_movement(tool, *prepared.source_region_island,
                    make_movement(ExtrusionRole::Perimeter, 0., 10., 20.f, 500.f));
    append_movement(
        tool, *prepared.source_region_island,
        make_movement(ExtrusionRole::Travel, 10., 10. + travel_length_mm, 100.f, 2000.f));
    append_movement(tool, *prepared.source_region_island, make_movement(
        ExtrusionRole::Perimeter, 10. + travel_length_mm, 20. + travel_length_mm,
        20.f, 500.f));
    return layer;
}

// Build one continuous Travel run from independently owned extrusion roots.
// The plugin must ignore those ownership boundaries when locating deceleration.
PrintingLayerGroup &prepare_travel_run_plan(PreparedTravelDecelerationPrint &prepared,
                                            const std::vector<TravelSpec> &travel_specs)
{
    REQUIRE(prepared.source_region_island != nullptr);
    REQUIRE_FALSE(travel_specs.empty());
    PrintingPlan &plan = prepared.print.mutable_printing_plan();
    plan.groups.emplace_back();
    plan.groups.back().layers.emplace_back();
    PrintingLayerGroup &layer = plan.groups.back().layers.back();
    layer.print_z = scale_i(0.2);
    layer.tool_groups.emplace_back();
    PrintingToolGroup &tool = layer.tool_groups.back();
    tool.extruder_id = 0;

    append_movement(tool, *prepared.source_region_island,
                    make_movement(ExtrusionRole::Perimeter, 0., 10., 20.f, 500.f));
    double current_x_mm = 10.0;
    for (const TravelSpec &spec : travel_specs) {
        append_movement(tool, *prepared.source_region_island,
                        make_movement(ExtrusionRole::Travel,
                                      current_x_mm,
                                      current_x_mm + spec.length_mm,
                                      spec.speed,
                                      spec.acceleration));
        current_x_mm += spec.length_mm;
    }
    append_movement(tool, *prepared.source_region_island,
                    make_movement(ExtrusionRole::Perimeter,
                                  current_x_mm,
                                  current_x_mm + 10.0,
                                  20.f,
                                  500.f));
    return layer;
}

// Resolve native direct process properties while walking the wrapper created
// by a split. This mirrors the inheritance observed by the firmware.
void collect_leaves(const ExtrusionEntity &entity,
                    ProcessState state,
                    std::vector<ObservedLeaf> &leaves)
{
    if (const ExtrusionAttributes *attributes =
            entity.get_property<ExtrusionAttributes>())
        state.role = attributes->extrusion_role();
    if (const ExtrusionPropertySpeed *process =
            entity.get_property<ExtrusionPropertySpeed>()) {
        if (process->speed_mm_per_s > 0.f)
            state.speed = process->speed_mm_per_s;
        if (process->accel_mm_per_s2 > 0.f)
            state.acceleration = process->accel_mm_per_s2;
    }
    if (entity.child_count() > 0) {
        for (size_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
            collect_leaves(entity.child(child_idx), state, leaves);
        return;
    }
    if (!entity.has_polyline())
        return;

    leaves.push_back(ObservedLeaf{
        &entity, state.role, state.speed, state.acceleration
    });
}

std::vector<ObservedLeaf> observed_leaves(const PrintingLayerGroup &layer)
{
    std::vector<ObservedLeaf> leaves;
    for (const PrintingToolGroup &tool : layer.tool_groups)
        for (const PrintingExtrusion &extrusion : tool.extrusions)
            collect_leaves(*extrusion.root, ProcessState{}, leaves);
    return leaves;
}

void run_layer_editors(Print &print, std::initializer_list<const char *> plugin_ids)
{
    ScopedActivePlugins active(plugin_ids);
    Orchestrator &orchestrator = Orchestrator::instance();
    orchestrator.reset_plugin_cancel();
    Steps::StepLayerExtrusionEdition::run_step(orchestrator, print);
    REQUIRE_FALSE(orchestrator.is_plugin_cancelled());
}

void run_travel_deceleration(Print &print)
{
    run_layer_editors(print, {TRAVEL_DECELERATION_PLUGIN});
}

} // namespace

TEST_CASE("Travel deceleration plugin exposes an independent post-processing contract",
          "[plugins][layer-extrusion-edit][travel-deceleration][api]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Plugin *plugin = Orchestrator::instance().get_plugin(TRAVEL_DECELERATION_PLUGIN);
    REQUIRE(plugin != nullptr);
    CHECK(plugin->get_step() == STEP_LAYER_EXTRUSION_EDIT);
    CHECK(plugin->get_priority() == 15);
    CHECK(plugin->get_exclusive_group() == "layer_extrusion_edit.travel_deceleration");
    CHECK(plugin->get_dependencies().empty());

    std::vector<std::string> keys;
    for (const Plugin::UsedConfigKey &key : plugin->get_used_config_keys())
        keys.push_back(key.key);
    CHECK(keys.size() == 2);
    CHECK(std::find(keys.begin(), keys.end(), "travel_deceleration_use_target") != keys.end());
    CHECK(std::find(keys.begin(), keys.end(), "gcode_min_length") != keys.end());
}

TEST_CASE("Travel deceleration splits a travel into ordered acceleration phases",
          "[plugins][layer-extrusion-edit][travel-deceleration]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    PreparedTravelDecelerationPrint prepared;
    prepare_source_context(prepared, true, "0");
    PrintingLayerGroup &layer = prepare_three_movement_plan(prepared);

    run_travel_deceleration(prepared.print);

    const ExtrusionEntity &travel_root = *layer.tool_groups[0].extrusions[1].root;
    REQUIRE(travel_root.child_count() == 2);
    CHECK_FALSE(travel_root.can_sort());
    CHECK_FALSE(travel_root.can_reverse());

    const std::vector<ObservedLeaf> leaves = observed_leaves(layer);
    REQUIRE(leaves.size() == 4);
    CHECK(leaves[1].role == ExtrusionRole::Travel);
    CHECK(leaves[2].role == ExtrusionRole::Travel);
    CHECK(leaves[1].speed == Approx(100.f));
    CHECK(leaves[2].speed == Approx(100.f));
    CHECK(leaves[1].acceleration == Approx(2000.f));
    CHECK(leaves[2].acceleration == Approx(500.f));
    CHECK(leaves[1].entity->last_point() == leaves[2].entity->first_point());
    CHECK(leaves[1].entity->length() + leaves[2].entity->length() ==
          Approx(scale_d(100.)).margin(double(SCALED_EPSILON)));

    // Unrelated fields stay on the original parent and remain inherited by
    // both phases instead of being recreated or lost during the split.
    const ExtrusionPropertySpeed *common_process =
        travel_root.get_property<ExtrusionPropertySpeed>();
    REQUIRE(common_process != nullptr);
    CHECK(common_process->pressure_adv == Approx(0.12f));
    CHECK(common_process->fan_speed_percent == Approx(37.f));
    CHECK(common_process->temperature_C == Approx(215.f));
}

TEST_CASE("Travel deceleration projects one virtual run onto its leaf polylines",
          "[plugins][layer-extrusion-edit][travel-deceleration][run]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    SECTION("existing junction") {
        PreparedTravelDecelerationPrint prepared;
        prepare_source_context(prepared, true, "0");
        PrintingLayerGroup &layer = prepare_travel_run_plan(
            prepared, {{20.}, {30.}, {50.}});

        run_travel_deceleration(prepared.print);

        const std::vector<ObservedLeaf> leaves = observed_leaves(layer);
        REQUIRE(leaves.size() == 5);
        CHECK(leaves[1].acceleration == Approx(2000.f));
        CHECK(leaves[2].acceleration == Approx(2000.f));
        CHECK(leaves[3].acceleration == Approx(500.f));
        CHECK(layer.tool_groups[0].extrusions[1].root->child_count() == 0);
        CHECK(layer.tool_groups[0].extrusions[2].root->child_count() == 0);
        CHECK(layer.tool_groups[0].extrusions[3].root->child_count() == 0);
    }

    SECTION("inside first leaf") {
        PreparedTravelDecelerationPrint prepared;
        prepare_source_context(prepared, true, "0");
        PrintingLayerGroup &layer = prepare_travel_run_plan(
            prepared, {{5.}, {2.}, {3.}});

        run_travel_deceleration(prepared.print);

        CHECK(layer.tool_groups[0].extrusions[1].root->child_count() == 2);
        const std::vector<ObservedLeaf> leaves = observed_leaves(layer);
        REQUIRE(leaves.size() == 6);
        CHECK(leaves[1].acceleration == Approx(2000.f));
        CHECK(leaves[2].acceleration == Approx(500.f));
        CHECK(leaves[3].acceleration == Approx(500.f));
        CHECK(leaves[4].acceleration == Approx(500.f));
    }

    SECTION("inside middle leaf") {
        PreparedTravelDecelerationPrint prepared;
        prepare_source_context(prepared, true, "0");
        PrintingLayerGroup &layer = prepare_travel_run_plan(
            prepared, {{50.}, {45.}, {5.}});

        run_travel_deceleration(prepared.print);

        CHECK(layer.tool_groups[0].extrusions[2].root->child_count() == 2);
        const std::vector<ObservedLeaf> leaves = observed_leaves(layer);
        REQUIRE(leaves.size() == 6);
        CHECK(leaves[1].acceleration == Approx(2000.f));
        CHECK(leaves[2].acceleration == Approx(2000.f));
        CHECK(leaves[3].acceleration == Approx(500.f));
        CHECK(leaves[4].acceleration == Approx(500.f));
    }

    SECTION("inside last leaf") {
        PreparedTravelDecelerationPrint prepared;
        prepare_source_context(prepared, true, "0");
        PrintingLayerGroup &layer = prepare_travel_run_plan(
            prepared, {{0.5}, {0.5}, {9.}});

        run_travel_deceleration(prepared.print);

        CHECK(layer.tool_groups[0].extrusions[3].root->child_count() == 2);
        const std::vector<ObservedLeaf> leaves = observed_leaves(layer);
        REQUIRE(leaves.size() == 6);
        CHECK(leaves[1].acceleration == Approx(2000.f));
        CHECK(leaves[2].acceleration == Approx(2000.f));
        CHECK(leaves[3].acceleration == Approx(2000.f));
        CHECK(leaves[4].acceleration == Approx(500.f));
    }

    SECTION("whole short run") {
        PreparedTravelDecelerationPrint prepared;
        prepare_source_context(prepared, true, "2");
        PrintingLayerGroup &layer = prepare_travel_run_plan(
            prepared, {{0.3}, {0.3}, {0.4}});

        run_travel_deceleration(prepared.print);

        const std::vector<ObservedLeaf> leaves = observed_leaves(layer);
        REQUIRE(leaves.size() == 5);
        CHECK(leaves[1].acceleration == Approx(500.f));
        CHECK(leaves[2].acceleration == Approx(500.f));
        CHECK(leaves[3].acceleration == Approx(500.f));
        CHECK(layer.tool_groups[0].extrusions[1].root->child_count() == 0);
        CHECK(layer.tool_groups[0].extrusions[2].root->child_count() == 0);
        CHECK(layer.tool_groups[0].extrusions[3].root->child_count() == 0);
    }
}

TEST_CASE("Travel deceleration respects process and tool-group run boundaries",
          "[plugins][layer-extrusion-edit][travel-deceleration][run]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    SECTION("different process values start another run") {
        PreparedTravelDecelerationPrint prepared;
        prepare_source_context(prepared, true, "0");
        PrintingLayerGroup &layer = prepare_travel_run_plan(
            prepared, {{30., 80.f, 1500.f}, {30.}, {50.}});

        run_travel_deceleration(prepared.print);

        const std::vector<ObservedLeaf> leaves = observed_leaves(layer);
        REQUIRE(leaves.size() == 5);
        CHECK(leaves[1].acceleration == Approx(1500.f));
        CHECK(leaves[2].acceleration == Approx(2000.f));
        CHECK(leaves[3].acceleration == Approx(500.f));
    }

    SECTION("tool-group boundary ends a run") {
        PreparedTravelDecelerationPrint prepared;
        prepare_source_context(prepared, true, "0");
        PrintingLayerGroup &layer = prepare_three_movement_plan(prepared);
        layer.tool_groups.emplace_back();
        PrintingToolGroup &first_tool = layer.tool_groups.front();
        PrintingToolGroup &second_tool = layer.tool_groups.back();
        second_tool.extruder_id = 0;
        second_tool.extrusions.push_back(std::move(first_tool.extrusions.back()));
        first_tool.extrusions.pop_back();

        run_travel_deceleration(prepared.print);

        const ExtrusionEntity &travel = *first_tool.extrusions.back().root;
        CHECK(travel.child_count() == 0);
        REQUIRE(travel.get_property<ExtrusionPropertySpeed>() != nullptr);
        CHECK(travel.get_property<ExtrusionPropertySpeed>()->accel_mm_per_s2 == Approx(2000.f));
    }
}

TEST_CASE("Travel deceleration overrides an inherited Travel acceleration locally",
          "[plugins][layer-extrusion-edit][travel-deceleration][inheritance]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    PreparedTravelDecelerationPrint prepared;
    prepare_source_context(prepared, true, "2");
    REQUIRE(prepared.source_region_island != nullptr);

    PrintingPlan &plan = prepared.print.mutable_printing_plan();
    plan.groups.emplace_back();
    plan.groups.back().layers.emplace_back();
    PrintingLayerGroup &layer = plan.groups.back().layers.back();
    layer.print_z = scale_i(0.2);
    layer.tool_groups.emplace_back();
    PrintingToolGroup &tool = layer.tool_groups.back();
    tool.extruder_id = 0;
    append_movement(tool, *prepared.source_region_island,
                    make_movement(ExtrusionRole::Perimeter, 0., 10., 20.f, 500.f));

    std::unique_ptr<ExtrusionEntityCollection> travel_root =
        std::make_unique<ExtrusionEntityCollection>(false, false);
    travel_root->add_property(ExtrusionPropertySpeed(100.f, 2000.f, 0.12f, 37.f, 215.f));
    travel_root->append(make_movement(
        ExtrusionRole::Travel, 10., 11., -1.f, -1.f, false));
    ExtrusionEntity &stored_root = append_movement(
        tool, *prepared.source_region_island, std::move(travel_root));
    append_movement(tool, *prepared.source_region_island,
                    make_movement(ExtrusionRole::Perimeter, 11., 21., 20.f, 500.f));

    run_travel_deceleration(prepared.print);

    const ExtrusionPropertySpeed *parent_process =
        stored_root.get_property<ExtrusionPropertySpeed>();
    REQUIRE(parent_process != nullptr);
    CHECK(parent_process->accel_mm_per_s2 == Approx(2000.f));
    REQUIRE(stored_root.child_count() == 1);
    const ExtrusionPropertySpeed *child_process =
        stored_root.child(0).get_property<ExtrusionPropertySpeed>();
    REQUIRE(child_process != nullptr);
    CHECK(child_process->accel_mm_per_s2 == Approx(500.f));
    CHECK(child_process->pressure_adv == Approx(-1.f));
}

TEST_CASE("Travel deceleration uses the target on unsplittable travels",
          "[plugins][layer-extrusion-edit][travel-deceleration]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    PreparedTravelDecelerationPrint prepared;
    prepare_source_context(prepared, true, "2");
    PrintingLayerGroup &layer = prepare_three_movement_plan(prepared, 1.);

    run_travel_deceleration(prepared.print);

    const ExtrusionEntity &travel = *layer.tool_groups[0].extrusions[1].root;
    CHECK(travel.child_count() == 0);
    const ExtrusionPropertySpeed *process = travel.get_property<ExtrusionPropertySpeed>();
    REQUIRE(process != nullptr);
    CHECK(process->speed_mm_per_s == Approx(100.f));
    CHECK(process->accel_mm_per_s2 == Approx(500.f));
}

TEST_CASE("Travel deceleration uses the layer entry speed prepared by setup",
          "[plugins][layer-extrusion-edit][travel-deceleration]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    PreparedTravelDecelerationPrint prepared;
    prepare_source_context(prepared, true, "0");
    prepare_three_movement_plan(prepared);

    // Keep the preceding extrusion in layer zero and move the travel plus its
    // target to layer one. The second parallel run can only split correctly if
    // setup() published the speed visible at this layer boundary.
    PrintingGroup &group = prepared.print.mutable_printing_plan().groups.front();
    group.layers.emplace_back();
    PrintingLayerGroup &second_layer = group.layers.back();
    second_layer.print_z = scale_i(0.4);
    second_layer.tool_groups.emplace_back();
    second_layer.tool_groups.back().extruder_id = 0;
    std::vector<PrintingExtrusion> &first_extrusions =
        group.layers.front().tool_groups.front().extrusions;
    second_layer.tool_groups.back().extrusions.push_back(std::move(first_extrusions[1]));
    second_layer.tool_groups.back().extrusions.push_back(std::move(first_extrusions[2]));
    first_extrusions.erase(first_extrusions.begin() + 1, first_extrusions.end());

    run_travel_deceleration(prepared.print);

    const ExtrusionEntity &travel =
        *second_layer.tool_groups.front().extrusions.front().root;
    REQUIRE(travel.child_count() == 2);
    const std::vector<ObservedLeaf> leaves = observed_leaves(second_layer);
    REQUIRE(leaves.size() == 3);
    CHECK(leaves[0].acceleration == Approx(2000.f));
    CHECK(leaves[1].acceleration == Approx(500.f));
}

TEST_CASE("Travel deceleration consumes acceleration resolved by the default provider",
          "[plugins][layer-extrusion-edit][travel-deceleration][integration]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    PreparedTravelDecelerationPrint prepared;
    prepare_source_context(prepared, true, "0");
    PrintingLayerGroup &layer = prepare_three_movement_plan(prepared);
    ExtrusionEntity &travel = *layer.tool_groups.front().extrusions[1].root;
    ExtrusionPropertySpeed *travel_process =
        travel.get_property<ExtrusionPropertySpeed>();
    REQUIRE(travel_process != nullptr);
    travel_process->accel_mm_per_s2 = -1.f;

    run_layer_editors(prepared.print, {ACCELERATION_PLUGIN, TRAVEL_DECELERATION_PLUGIN});

    REQUIRE(travel.child_count() == 2);
    const std::vector<ObservedLeaf> leaves = observed_leaves(layer);
    REQUIRE(leaves.size() == 4);
    CHECK(leaves[1].acceleration == Approx(2000.f));
    CHECK(leaves[2].acceleration == Approx(500.f));
}

TEST_CASE("Travel deceleration preserves arc geometry and Z offsets",
          "[plugins][layer-extrusion-edit][travel-deceleration]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    PreparedTravelDecelerationPrint prepared;
    prepare_source_context(prepared, true, "0");
    PrintingLayerGroup &layer = prepare_three_movement_plan(prepared);
    ExtrusionPath *travel = dynamic_cast<ExtrusionPath *>(
        layer.tool_groups.front().extrusions[1].root.get());
    REQUIRE(travel != nullptr);

    ArcPolyline curved_travel;
    curved_travel.append(Point(scale_i(10.), scale_i(0.)));
    curved_travel.append(Geometry::ArcWelder::Segment(
        Point(scale_i(110.), scale_i(0.)), float(scale_i(60.)),
        Geometry::ArcWelder::Orientation::CCW));
    curved_travel.set_z_offset(0, scale_i(0.03));
    curved_travel.set_z_offset(1, scale_i(0.09));
    travel->polyline() = std::move(curved_travel);

    run_travel_deceleration(prepared.print);

    const ExtrusionEntity &root = *layer.tool_groups.front().extrusions[1].root;
    REQUIRE(root.child_count() == 2);
    const ExtrusionEntity &first = root.child(0);
    const ExtrusionEntity &final = root.child(1);
    REQUIRE(first.polyline_ref().size() >= 2);
    REQUIRE(final.polyline_ref().size() >= 2);
    CHECK(first.polyline_ref().z_offset(0) == scale_i(0.03));
    CHECK(final.polyline_ref().z_offset(final.polyline_ref().size() - 1) == scale_i(0.09));
    CHECK(first.last_point() == final.first_point());

    const slic3r_api::ExtrusionEntity first_view(
        reinterpret_cast<const extrusion_entity_handle *>(&first));
    const slic3r_api::ExtrusionEntity final_view(
        reinterpret_cast<const extrusion_entity_handle *>(&final));
    REQUIRE(first_view.segment_count() > 0);
    REQUIRE(final_view.segment_count() > 0);
    CHECK(first_view.segment(first_view.segment_count() - 1).radius != 0.f);
    CHECK(final_view.segment(0).radius != 0.f);
}

TEST_CASE("Travel deceleration handles disabled processing and upstream values",
          "[plugins][layer-extrusion-edit][travel-deceleration]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    SECTION("disabled option") {
        PreparedTravelDecelerationPrint prepared;
        prepare_source_context(prepared, false, "0");
        PrintingLayerGroup &layer = prepare_three_movement_plan(prepared);
        run_travel_deceleration(prepared.print);

        const ExtrusionEntity &travel = *layer.tool_groups[0].extrusions[1].root;
        CHECK(travel.child_count() == 0);
        REQUIRE(travel.get_property<ExtrusionPropertySpeed>() != nullptr);
        CHECK(travel.get_property<ExtrusionPropertySpeed>()->accel_mm_per_s2 == Approx(2000.f));
    }

    SECTION("pre-existing acceleration") {
        PreparedTravelDecelerationPrint prepared;
        prepare_source_context(prepared, true, "0");
        PrintingLayerGroup &layer = prepare_three_movement_plan(prepared);
        run_travel_deceleration(prepared.print);

        const ExtrusionEntity &travel = *layer.tool_groups[0].extrusions[1].root;
        REQUIRE(travel.child_count() == 2);
        const std::vector<ObservedLeaf> leaves = observed_leaves(layer);
        REQUIRE(leaves.size() == 4);
        CHECK(leaves[1].acceleration == Approx(2000.f));
        CHECK(leaves[2].acceleration == Approx(500.f));
    }
}
