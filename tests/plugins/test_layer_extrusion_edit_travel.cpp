#include <catch2/catch.hpp>

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <vector>

#include "plugin_test_helpers.hpp"
#include "test_data.hpp"

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/host/Plugin.hpp"
#include "libslic3r/Api/internal/LayerIslandAccess.hpp"
#include "libslic3r/Api/plugin/cpp/AuxiliaryLayerHelpers.hpp"
#include "libslic3r/Api/plugin/cpp/ExtrusionViews.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/ExtrusionProperty.hpp"
#include "libslic3r/GCode/AvoidCrossingPerimeters.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintObject.hpp"
#include "libslic3r/Printing/PrintingPlan.hpp"
#include "libslic3r/Plugins/LayerExtrusionEdit/ExtrusionScopeHelpers.hpp"
#include "libslic3r/Plugins/PrintingPlan/PrintingExtrusionScopeProperty.h"
#include "libslic3r/Steps/StepLayerHeightGeneration.hpp"
#include "libslic3r/Steps/StepLayerExtrusionEdition.hpp"
#include "libslic3r/Steps/StepPostSlicing.hpp"
#include "libslic3r/Steps/StepSlicing.hpp"

/*
Straight-travel plugin tests
============================

The fixtures build already ordered PrintingPlan trees directly. The entry-state
producer first publishes each layer boundary; the travel producer then runs in
the real parallel host and must connect the leaves without consulting another
worker's layer or changing their original printable properties.
*/

namespace {
using namespace Slic3r;
using namespace Slic3r::Printing;

constexpr const char *ENTRY_STATE_PLUGIN = "layer_extrusion_edit.entry_state.default";
constexpr const char *TRANSITION_SCOPE_PLUGIN =
    "layer_extrusion_edit.transition_scope.default";
constexpr const char *TRAVEL_PLUGIN = "layer_extrusion_edit.travel.default";
constexpr const char *AVOID_CROSSING_TRAVEL_PLUGIN =
    "layer_extrusion_edit.travel.avoid_crossing_perimeters";

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

std::unique_ptr<ExtrusionPath> make_path(const Point &start,
                                        const Point &end,
                                        coord_t start_z_offset = 0,
                                        coord_t end_z_offset = 0,
                                        ExtrusionRole role = ExtrusionRole::Perimeter)
{
    ArcPolyline polyline;
    polyline.append(start);
    polyline.append(end);
    polyline.set_z_offset(0, start_z_offset);
    polyline.set_z_offset(1, end_z_offset);
    return std::make_unique<ExtrusionPath>(
        polyline, ExtrusionAttributes(role, ExtrusionFlow(
            role == ExtrusionRole::Travel ? 0.0 : 0.2, 0.4f, 0.2f)), nullptr, true);
}

std::unique_ptr<ExtrusionPath> make_closed_arc_path(const Point &seam)
{
    ArcPolyline polyline;
    polyline.append(seam);
    polyline.append(Geometry::ArcWelder::Segment(
        Point(seam.x() + scale_i(1.), seam.y() + scale_i(1.)),
        float(scale_i(1.)),
        Geometry::ArcWelder::Orientation::CCW));
    polyline.append(seam);
    polyline.set_z_offset(0, scale_i(0.03));
    polyline.set_z_offset(1, scale_i(0.04));
    polyline.set_z_offset(2, scale_i(0.03));
    return std::make_unique<ExtrusionPath>(
        polyline, ExtrusionAttributes(
            ExtrusionRole::Perimeter, ExtrusionFlow(0.2, 0.4f, 0.2f)), nullptr, true);
}

void append_extrusion(PrintingToolGroup &tool,
                      std::unique_ptr<ExtrusionEntity> root,
                      const LayerRegionIsland *region_island = nullptr,
                      uint16_t object_instance_idx = 0)
{
    PrintingExtrusion extrusion;
    extrusion.region_island = region_island;
    extrusion.object_instance_idx = object_instance_idx;
    extrusion.root = std::move(root);
    extrusion.sregion_island_role = ExtrusionRole::Perimeter;
    tool.extrusions.push_back(std::move(extrusion));
}

void collect_geometric_leaves(const ExtrusionEntity &entity,
                              std::vector<const ExtrusionEntity *> &leaves)
{
    if (entity.child_count() > 0) {
        for (size_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
            collect_geometric_leaves(entity.child(child_idx), leaves);
        return;
    }
    if (entity.has_polyline())
        leaves.push_back(&entity);
}

std::vector<const ExtrusionEntity *> plan_leaves(const Print &print)
{
    std::vector<const ExtrusionEntity *> leaves;
    REQUIRE(print.printing_plan() != nullptr);
    for (const PrintingGroup &group : print.printing_plan()->groups)
        for (const PrintingLayerGroup &layer : group.layers)
            for (const PrintingToolGroup &tool : layer.tool_groups)
                for (const PrintingExtrusion &extrusion : tool.extrusions)
                    collect_geometric_leaves(*extrusion.root, leaves);
    return leaves;
}

size_t travel_count(const Print &print)
{
    size_t count = 0;
    for (const ExtrusionEntity *leaf : plan_leaves(print)) {
        const ExtrusionAttributes *attributes = leaf->get_property<ExtrusionAttributes>();
        if (attributes != nullptr && attributes->extrusion_role() == ExtrusionRole::Travel)
            ++count;
    }
    return count;
}

/* Return the private scope key shared by the transition and travel providers. */
slic3r_api::PluginPropertyKey<slic3r_api::PrintingExtrusionScopeProperty>
scope_property_key()
{
    return slic3r_api::printing_extrusion_scope_property_key(
        reinterpret_cast<orchestrator_handle *>(&Orchestrator::instance()));
}

/* Open one core root through the named compact-scope API. */
slic3r_api::LayerExtrusionEdit::ExtrusionScope::OrderedExtrusionScope
ordered_scope(ExtrusionEntity &root)
{
    return slic3r_api::LayerExtrusionEdit::ExtrusionScope::OrderedExtrusionScope(
        slic3r_api::MutableExtrusionEntity(
            reinterpret_cast<extrusion_entity_handle *>(&root)),
        scope_property_key());
}

/* Run one explicit layer-edit chain through the production step host. */
void run_layer_edit_plugins(Print &print,
                            std::initializer_list<const char *> plugin_ids)
{
    ScopedActivePlugins active(plugin_ids);
    Orchestrator &orchestrator = Orchestrator::instance();
    orchestrator.reset_plugin_cancel();
    Steps::StepLayerExtrusionEdition::run_step(orchestrator, print);
    REQUIRE_FALSE(orchestrator.is_plugin_cancelled());
}

void run_travel_plugins(Print &print, const char *travel_plugin = TRAVEL_PLUGIN)
{
    run_layer_edit_plugins(
        print, {TRANSITION_SCOPE_PLUGIN, ENTRY_STATE_PLUGIN, travel_plugin});
}

TEST_CASE("Avoid-crossing travel keeps the straight fallback without regional context",
          "[plugins][layer-extrusion-edit][travel][avoid-crossing-perimeters]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    plan.groups.back().layers.emplace_back();
    PrintingLayerGroup &layer = plan.groups.back().layers.back();
    layer.print_z = scale_i(0.2);
    layer.tool_groups.emplace_back();
    PrintingToolGroup &tool = layer.tool_groups.back();
    tool.extruder_id = 0;
    append_extrusion(tool, make_path(
        Point(scale_i(0.), 0), Point(scale_i(1.), 0)));
    append_extrusion(tool, make_path(
        Point(scale_i(3.), 0), Point(scale_i(4.), 0)));

    run_travel_plugins(print, AVOID_CROSSING_TRAVEL_PLUGIN);

    REQUIRE(travel_count(print) == 1);
    const slic3r_api::LayerExtrusionEdit::ExtrusionScope::OrderedExtrusionScope scope(
        slic3r_api::MutableExtrusionEntity(
            reinterpret_cast<extrusion_entity_handle *>(tool.extrusions[1].root.get())),
        scope_property_key());
    REQUIRE(scope.travel().segment_count() == 1);
    CHECK(scope.travel().point(0).x == scale_i(1.));
    CHECK(scope.travel().point(1).x == scale_i(3.));
}

} // namespace

TEST_CASE("Straight travel plugin exposes its ordered-layer contract",
          "[plugins][layer-extrusion-edit][travel]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Plugin *plugin = Orchestrator::instance().get_plugin(TRAVEL_PLUGIN);
    REQUIRE(plugin != nullptr);
    CHECK(plugin->get_step() == STEP_LAYER_EXTRUSION_EDIT);
    CHECK(plugin->get_priority() == -50);
    CHECK(plugin->get_exclusive_group() == "layer_extrusion_edit.travel");
    REQUIRE(plugin->get_dependencies().size() == 2);
    CHECK(plugin->get_dependencies()[0] == TRANSITION_SCOPE_PLUGIN);
    CHECK(plugin->get_dependencies()[1] == ENTRY_STATE_PLUGIN);
    CHECK(plugin->get_used_config_keys().empty());
}

TEST_CASE("Avoid-crossing travel is an optional provider in the travel group",
          "[plugins][layer-extrusion-edit][travel][avoid-crossing-perimeters]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Plugin *plugin = Orchestrator::instance().get_plugin(AVOID_CROSSING_TRAVEL_PLUGIN);
    REQUIRE(plugin != nullptr);
    CHECK(plugin->get_step() == STEP_LAYER_EXTRUSION_EDIT);
    CHECK(plugin->get_priority() == -50);
    CHECK(plugin->get_exclusive_group() == "layer_extrusion_edit.travel");
    REQUIRE(plugin->get_dependencies().size() == 2);
    CHECK(plugin->get_dependencies()[0] == TRANSITION_SCOPE_PLUGIN);
    CHECK(plugin->get_dependencies()[1] == ENTRY_STATE_PLUGIN);

    const std::vector<Plugin::UsedConfigKey> keys = plugin->get_used_config_keys();
    CHECK(std::any_of(keys.begin(), keys.end(), [](const Plugin::UsedConfigKey &key) {
        return key.key == "avoid_crossing_perimeters" && key.type == RAW_CO_BOOL;
    }));
}

TEST_CASE("AvoidCrossingPerimeters crossing test rejects contained travels",
          "[AvoidCrossingPerimeters][plugins][layer-extrusion-edit][travel]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"avoid_crossing_perimeters", "1"},
        {"nozzle_diameter", "0.4"}
    });
    Print print;
    Model model;
    Slic3r::Test::init_print(
        {Slic3r::Test::TestMesh::cube_20x20x20}, print, model, config);

    Orchestrator &orchestrator = Orchestrator::instance();
    Steps::StepLayerHeightGeneration::run_step(orchestrator, print);
    Steps::StepSlicing::run_step(orchestrator, print);
    Steps::StepPostSlicing::run_step(orchestrator, print);

    REQUIRE(print.object(0).layer_count() > 0);
    const Layer &layer = print.object(0).layer(0);
    REQUIRE_FALSE(layer.lslices().empty());
    const BoundingBox bounds = get_extents(layer.lslices());
    const Point center = bounds.center();

    AvoidCrossingPerimeters detector;
    const std::vector<const Layer *> printed_layers{&layer};
    const AvoidCrossingPerimeters::PerimeterCrossingContext context{
        layer,
        printed_layers,
        0,
        0,
        scale_i(0.2),
        []() {}
    };
    detector.prepare_crossing_test(context);

    // A short segment wholly inside the printable island is the hot-path
    // rejection used before the travel router builds a detour.
    Polyline inside;
    inside.points = {
        Point(center.x() - scale_i(1.), center.y()),
        Point(center.x() + scale_i(1.), center.y())
    };
    CHECK_FALSE(detector.can_cross_perimeter(inside, true));

    // Leaving the object crosses its offset contour and must request routing.
    Polyline leaving;
    leaving.points = {
        center,
        Point(bounds.max.x() + scale_i(2.), center.y())
    };
    CHECK(detector.can_cross_perimeter(leaving, true));
}

TEST_CASE("Avoid-crossing travel routes around a hole between enabled endpoints",
          "[plugins][layer-extrusion-edit][travel][avoid-crossing-perimeters]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"avoid_crossing_perimeters", "1"},
        {"avoid_crossing_not_first_layer", "0"},
        {"avoid_crossing_perimeters_max_detour", "0"},
        {"nozzle_diameter", "0.4"}
    });
    Print print;
    Model model;
    Slic3r::Test::init_print(
        {Slic3r::Test::TestMesh::cube_20x20x20}, print, model, config);

    Polygon contour({
        Point(scale_i(0.), scale_i(0.)),
        Point(scale_i(30.), scale_i(0.)),
        Point(scale_i(30.), scale_i(30.)),
        Point(scale_i(0.), scale_i(30.))
    });
    contour.make_counter_clockwise();
    Polygon hole({
        Point(scale_i(10.), scale_i(10.)),
        Point(scale_i(20.), scale_i(10.)),
        Point(scale_i(20.), scale_i(20.)),
        Point(scale_i(10.), scale_i(20.))
    });
    hole.make_clockwise();
    const ExPolygons subject{ExPolygon(std::move(contour), {std::move(hole)})};

    PluginStorage storage;
    const slic3r_api::Print print_view(reinterpret_cast<const print_handle *>(&print));
    const slic3r_api::Object object_view(
        reinterpret_cast<const object_handle *>(&print.object(0)));
    const slic3r_api::AuxiliaryLayerBuildResult built =
        slic3r_api::build_auxiliary_layer_regions_from_subject(
            reinterpret_cast<storage_handle *>(&storage),
            print_view,
            object_view,
            slic3r_api::ExPolygonCollection(
                reinterpret_cast<const expolygon_collection_handle *>(&subject)),
            scale_i(0.2), scale_i(0.2), scale_i(0.1));
    REQUIRE(built.created);
    REQUIRE(built.layer.island_count() == 1);
    Layer *built_layer = reinterpret_cast<Layer *>(
        const_cast<layer_handle *>(built.layer.handle()));
    REQUIRE(built_layer != nullptr);
    REQUIRE(built_layer->islands().size() == 1);
    Slic3r::ApiInternal::LayerIslandAccess::perimeter_slices_mutable(
        built_layer->islands()[0]) = subject;
    const slic3r_api::LayerRegionIsland region_view =
        built.layer.island(0).get_or_create_full_region_island(0);
    REQUIRE(region_view.valid());
    const LayerRegionIsland *region_island =
        reinterpret_cast<const LayerRegionIsland *>(region_view.handle());

    const c_point instance_shift = object_view.instance_shift(0);
    const Point shift(instance_shift.x, instance_shift.y);
    const Point source = Point(scale_i(5.), scale_i(15.)) + shift;
    const Point target = Point(scale_i(25.), scale_i(15.)) + shift;

    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    plan.groups.back().layers.emplace_back();
    PrintingLayerGroup &plan_layer = plan.groups.back().layers.back();
    plan_layer.print_z = scale_i(0.2);
    plan_layer.tool_groups.emplace_back();
    PrintingToolGroup &tool = plan_layer.tool_groups.back();
    tool.extruder_id = 0;
    append_extrusion(tool, make_path(
        Point(source.x() - scale_i(1.), source.y()), source), region_island);
    append_extrusion(tool, make_path(
        target, Point(target.x() + scale_i(1.), target.y())), region_island);

    run_travel_plugins(print, AVOID_CROSSING_TRAVEL_PLUGIN);

    REQUIRE(travel_count(print) == 1);
    const ExtrusionEntity &scope = *tool.extrusions[1].root;
    REQUIRE(scope.child_count() == 3);
    const ExtrusionEntity &travel = scope.child(0);
    REQUIRE(travel.has_polyline());
    CHECK(travel.polyline_ref().size() > 2);
    CHECK(travel.first_point() == source);
    CHECK(travel.last_point() == target);
}

TEST_CASE("Straight travels connect trees tools layers and printing groups",
          "[plugins][layer-extrusion-edit][travel]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    PrintingGroup &first_group = plan.groups.back();
    first_group.layers.emplace_back();
    PrintingLayerGroup &first_layer = first_group.layers.back();
    first_layer.print_z = scale_i(0.2);
    first_layer.tool_groups.emplace_back();
    first_layer.tool_groups.back().extruder_id = 0;
    append_extrusion(first_layer.tool_groups.back(), make_path(
        Point(scale_i(0.), scale_i(0.)), Point(scale_i(1.), scale_i(0.))));

    // A new tool section starts away from the preceding path and receives one
    // connector before its original extrusion content.
    first_layer.tool_groups.emplace_back();
    first_layer.tool_groups.back().extruder_id = 1;
    append_extrusion(first_layer.tool_groups.back(), make_path(
        Point(scale_i(2.), scale_i(0.)), Point(scale_i(3.), scale_i(0.))));

    // Empty scopes do not break the entry-state prefix. The next non-empty
    // layer still starts from the last point observed above.
    first_group.layers.emplace_back();
    first_group.layers.back().print_z = scale_i(0.4);
    first_group.layers.emplace_back();
    PrintingLayerGroup &third_layer = first_group.layers.back();
    third_layer.print_z = scale_i(0.6);
    third_layer.tool_groups.emplace_back();
    third_layer.tool_groups.back().extruder_id = 1;
    append_extrusion(third_layer.tool_groups.back(), make_path(
        Point(scale_i(4.), scale_i(0.)), Point(scale_i(5.), scale_i(0.))));

    // PrintingGroup boundaries preserve machine continuity as well.
    plan.groups.emplace_back();
    plan.groups.back().layers.emplace_back();
    PrintingLayerGroup &second_group_layer = plan.groups.back().layers.back();
    second_group_layer.print_z = scale_i(0.2);
    second_group_layer.tool_groups.emplace_back();
    second_group_layer.tool_groups.back().extruder_id = 0;
    append_extrusion(second_group_layer.tool_groups.back(), make_path(
        Point(scale_i(6.), scale_i(0.)), Point(scale_i(7.), scale_i(0.))));

    run_travel_plugins(print);
    CHECK(travel_count(print) == 3);

    // The pair facts are final and symmetric even when empty layers separate
    // two scopes and each worker may only mutate its own layer.
    const slic3r_api::PrintingExtrusionScopeProperty *first_source =
        slic3r_api::MutableExtrusionEntity(reinterpret_cast<extrusion_entity_handle *>(
            plan.groups[0].layers[0].tool_groups[0].extrusions[0].root.get()))
            .get(scope_property_key());
    const slic3r_api::PrintingExtrusionScopeProperty *same_layer_target =
        slic3r_api::MutableExtrusionEntity(reinterpret_cast<extrusion_entity_handle *>(
            plan.groups[0].layers[0].tool_groups[1].extrusions[0].root.get()))
            .get(scope_property_key());
    const slic3r_api::PrintingExtrusionScopeProperty *later_layer_target =
        slic3r_api::MutableExtrusionEntity(reinterpret_cast<extrusion_entity_handle *>(
            plan.groups[0].layers[2].tool_groups[0].extrusions[0].root.get()))
            .get(scope_property_key());
    const slic3r_api::PrintingExtrusionScopeProperty *next_group_target =
        slic3r_api::MutableExtrusionEntity(reinterpret_cast<extrusion_entity_handle *>(
            plan.groups[1].layers[0].tool_groups[0].extrusions[0].root.get()))
            .get(scope_property_key());
    REQUIRE(first_source != nullptr);
    REQUIRE(same_layer_target != nullptr);
    REQUIRE(later_layer_target != nullptr);
    REQUIRE(next_group_target != nullptr);
    CHECK(slic3r_api::printing_extrusion_scope_has_flag(
        *first_source, slic3r_api::PRINTING_EXTRUSION_SCOPE_OUTGOING_TRAVEL_MATERIALIZED));
    CHECK(slic3r_api::printing_extrusion_scope_has_flag(
        *same_layer_target, slic3r_api::PRINTING_EXTRUSION_SCOPE_INCOMING_TRAVEL_MATERIALIZED));
    CHECK(slic3r_api::printing_extrusion_scope_has_flag(
        *same_layer_target, slic3r_api::PRINTING_EXTRUSION_SCOPE_OUTGOING_TRAVEL_MATERIALIZED));
    CHECK(slic3r_api::printing_extrusion_scope_has_flag(
        *later_layer_target, slic3r_api::PRINTING_EXTRUSION_SCOPE_INCOMING_TRAVEL_MATERIALIZED));
    CHECK(slic3r_api::printing_extrusion_scope_has_flag(
        *later_layer_target, slic3r_api::PRINTING_EXTRUSION_SCOPE_OUTGOING_TRAVEL_MATERIALIZED));
    CHECK(slic3r_api::printing_extrusion_scope_has_flag(
        *next_group_target, slic3r_api::PRINTING_EXTRUSION_SCOPE_INCOMING_TRAVEL_MATERIALIZED));

    const ExtrusionEntity &wrapped =
        *plan.groups[0].layers[0].tool_groups[1].extrusions[0].root;
    REQUIRE(wrapped.child_count() == 4);
    CHECK_FALSE(wrapped.can_sort());
    CHECK_FALSE(wrapped.can_reverse());
    const ExtrusionAttributes *travel_attributes =
        wrapped.child(0).get_property<ExtrusionAttributes>();
    const ExtrusionAttributes *print_attributes =
        wrapped.child(2).get_property<ExtrusionAttributes>();
    REQUIRE(travel_attributes != nullptr);
    REQUIRE(print_attributes != nullptr);
    CHECK(travel_attributes->extrusion_role() == ExtrusionRole::Travel);
    CHECK(travel_attributes->mm3_per_mm == 0.0);
    CHECK(print_attributes->extrusion_role() == ExtrusionRole::Perimeter);

    const std::vector<const ExtrusionEntity *> leaves = plan_leaves(print);
    REQUIRE(leaves.size() == 7);
    for (size_t leaf_idx = 1; leaf_idx < leaves.size(); ++leaf_idx)
        CHECK(leaves[leaf_idx - 1]->last_point() == leaves[leaf_idx]->first_point());

    // A second run sees the explicit connectors as part of the continuous
    // stream and therefore inserts nothing else.
    run_travel_plugins(print);
    CHECK(travel_count(print) == 3);
    CHECK(plan_leaves(print).size() == 7);
}

TEST_CASE("Travel connects distinct scopes in one PrintingExtrusion",
          "[plugins][layer-extrusion-edit][travel][inherited-z]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    plan.groups.back().layers.emplace_back();
    PrintingLayerGroup &layer = plan.groups.back().layers.back();
    layer.print_z = scale_i(0.2);
    layer.tool_groups.emplace_back();
    PrintingToolGroup &tool = layer.tool_groups.back();
    tool.extruder_id = 0;

    std::unique_ptr<ExtrusionEntityCollection> root =
        std::make_unique<ExtrusionEntityCollection>(false, false);
    root->add_property(ExtrusionPropertyZOffset(scale_i(0.04)));
    root->append(make_path(Point(scale_i(0.), 0), Point(scale_i(1.), 0),
                           scale_i(0.01), scale_i(0.01)));
    root->append(make_path(Point(scale_i(3.), 0), Point(scale_i(4.), 0),
                           scale_i(0.03), scale_i(0.03)));
    ExtrusionEntity *const owner_root = root.get();
    append_extrusion(tool, std::move(root));

    run_travel_plugins(print);

    // The PrintingExtrusion root remains the common owner. Its two children
    // become sibling compact scopes, and only the target owns the connector.
    REQUIRE(tool.extrusions[0].root.get() == owner_root);
    REQUIRE(owner_root->child_count() == 2);
    slic3r_api::LayerExtrusionEdit::ExtrusionScope::OrderedExtrusionScope source =
        ordered_scope(owner_root->child(0));
    slic3r_api::LayerExtrusionEdit::ExtrusionScope::OrderedExtrusionScope target =
        ordered_scope(owner_root->child(1));
    REQUIRE(source.has_outgoing_transition());
    REQUIRE(target.has_incoming_transition());
    REQUIRE(target.travel().point_count() == 2);
    CHECK(target.travel().point(0).x == scale_i(1.));
    CHECK(target.travel().point(1).x == scale_i(3.));
    CHECK(target.travel().z_offset(0) == scale_i(0.01));
    CHECK(target.travel().z_offset(1) == scale_i(0.03));
}

TEST_CASE("Straight travel snapping uses XYZ epsilon and preserves closed seams",
          "[plugins][layer-extrusion-edit][travel]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    plan.groups.back().layers.emplace_back();
    PrintingLayerGroup &layer = plan.groups.back().layers.back();
    layer.print_z = scale_i(0.2);
    layer.tool_groups.emplace_back();
    PrintingToolGroup &tool = layer.tool_groups.back();
    tool.extruder_id = 0;

    const Point origin(scale_i(0.), scale_i(0.));
    const Point first_end(scale_i(1.), scale_i(0.));
    append_extrusion(tool, make_path(origin, first_end, 0, scale_i(0.03)));

    // This closed arc begins less than epsilon away in XY and at the same Z.
    // Both seam copies must snap while its arc metadata survives.
    const Point near_seam(first_end.x() + SCALED_EPSILON - 1, first_end.y());
    append_extrusion(tool, make_closed_arc_path(near_seam));

    // An exact-epsilon XY gap and a later pure-Z gap both require explicit
    // connector leaves because only strictly smaller distances are snapped.
    const Point arc_end = near_seam;
    append_extrusion(tool, make_path(
        Point(arc_end.x() + SCALED_EPSILON, arc_end.y()),
        Point(scale_i(3.), scale_i(0.)), scale_i(0.03), scale_i(0.03)));
    append_extrusion(tool, make_path(
        Point(scale_i(3.), scale_i(0.)),
        Point(scale_i(4.), scale_i(0.)),
        scale_i(0.03) + SCALED_EPSILON,
        scale_i(0.03) + SCALED_EPSILON));

    ExtrusionEntity *closed_scope = tool.extrusions[1].root.get();
    REQUIRE(closed_scope->polyline_ref().has_arc());
    run_travel_plugins(print);

    CHECK(travel_count(print) == 2);
    REQUIRE(closed_scope->child_count() == 2);
    const ExtrusionEntity &closed_source = closed_scope->child(0);
    REQUIRE(closed_source.has_polyline());
    CHECK(closed_source.first_point() == first_end);
    CHECK(closed_source.last_point() == first_end);
    CHECK(closed_source.polyline_ref().z_offset(0) == scale_i(0.03));
    CHECK(closed_source.polyline_ref().z_offset(closed_source.polyline_ref().size() - 1) ==
          scale_i(0.03));
    CHECK(closed_source.polyline_ref().has_arc());

    const ExtrusionEntity &xy_wrapper = *tool.extrusions[2].root;
    REQUIRE(xy_wrapper.child_count() == 4);
    REQUIRE(xy_wrapper.child(0).has_polyline());
    CHECK(xy_wrapper.child(0).polyline_ref().z_offset(0) == scale_i(0.03));
    CHECK(xy_wrapper.child(0).polyline_ref().z_offset(1) == scale_i(0.03));

    const ExtrusionEntity &z_wrapper = *tool.extrusions[3].root;
    REQUIRE(z_wrapper.child_count() == 3);
    REQUIRE(z_wrapper.child(0).has_polyline());
    CHECK(z_wrapper.child(0).first_point() == z_wrapper.child(0).last_point());
    CHECK(z_wrapper.child(0).polyline_ref().z_offset(0) == scale_i(0.03));
    CHECK(z_wrapper.child(0).polyline_ref().z_offset(1) ==
          scale_i(0.03) + SCALED_EPSILON);

    const std::vector<const ExtrusionEntity *> leaves = plan_leaves(print);
    for (size_t leaf_idx = 1; leaf_idx < leaves.size(); ++leaf_idx) {
        CHECK(leaves[leaf_idx - 1]->last_point() == leaves[leaf_idx]->first_point());
    }
}

TEST_CASE("Existing travels remain ordinary connected leaves",
          "[plugins][layer-extrusion-edit][travel]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    plan.groups.back().layers.emplace_back();
    PrintingLayerGroup &layer = plan.groups.back().layers.back();
    layer.print_z = scale_i(0.2);
    layer.tool_groups.emplace_back();
    PrintingToolGroup &tool = layer.tool_groups.back();
    tool.extruder_id = 0;
    append_extrusion(tool, make_path(
        Point(scale_i(0.), 0), Point(scale_i(1.), 0)));
    append_extrusion(tool, make_path(
        Point(scale_i(1.), 0), Point(scale_i(2.), 0), 0, 0, ExtrusionRole::Travel));
    append_extrusion(tool, make_path(
        Point(scale_i(2.), 0), Point(scale_i(3.), 0)));

    ExtrusionEntity *existing_travel = tool.extrusions[1].root.get();
    run_travel_plugins(print);

    CHECK(travel_count(print) == 1);
    CHECK(tool.extrusions[1].root.get() == existing_travel);
    CHECK(existing_travel->first_point() == Point(scale_i(1.), 0));
    CHECK(existing_travel->last_point() == Point(scale_i(2.), 0));

    const slic3r_api::PrintingExtrusionScopeProperty *source =
        slic3r_api::MutableExtrusionEntity(reinterpret_cast<extrusion_entity_handle *>(
            tool.extrusions[0].root.get())).get(scope_property_key());
    const slic3r_api::PrintingExtrusionScopeProperty *target =
        slic3r_api::MutableExtrusionEntity(reinterpret_cast<extrusion_entity_handle *>(
            tool.extrusions[2].root.get())).get(scope_property_key());
    REQUIRE(source != nullptr);
    REQUIRE(target != nullptr);
    CHECK(slic3r_api::printing_extrusion_scope_has_flag(
        *source, slic3r_api::PRINTING_EXTRUSION_SCOPE_OUTGOING_TRAVEL_MATERIALIZED));
    CHECK(slic3r_api::printing_extrusion_scope_has_flag(
        *target, slic3r_api::PRINTING_EXTRUSION_SCOPE_INCOMING_TRAVEL_MATERIALIZED));
    CHECK(ordered_scope(*tool.extrusions[2].root).travel().segment_count() == 0);
}

TEST_CASE("Travel connector uses source after and target before phases",
          "[plugins][layer-extrusion-edit][travel][scope-phases]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    plan.groups.back().layers.emplace_back();
    PrintingLayerGroup &layer = plan.groups.back().layers.back();
    layer.print_z = scale_i(0.2);
    layer.tool_groups.emplace_back();
    PrintingToolGroup &tool = layer.tool_groups.back();
    tool.extruder_id = 0;
    append_extrusion(tool, make_path(Point(scale_i(0.), 0), Point(scale_i(1.), 0)));
    append_extrusion(tool, make_path(Point(scale_i(3.), 0), Point(scale_i(4.), 0)));

    // Prepare the compact shapes first, then emulate process plugins which
    // append geometry without changing the marked roots or their child lists.
    run_layer_edit_plugins(print, {TRANSITION_SCOPE_PLUGIN});
    slic3r_api::LayerExtrusionEdit::ExtrusionScope::OrderedExtrusionScope source =
        ordered_scope(*tool.extrusions[0].root);
    slic3r_api::LayerExtrusionEdit::ExtrusionScope::OrderedExtrusionScope target =
        ordered_scope(*tool.extrusions[1].root);
    REQUIRE(source.after().valid());
    REQUIRE(target.before().valid());
    REQUIRE(source.after().set_points(std::vector<c_point>{
        c_point{scale_i(1.), 0}, c_point{scale_i(1.5), 0}}));
    REQUIRE(target.before().set_points(std::vector<c_point>{
        c_point{scale_i(2.5), 0}, c_point{scale_i(3.), 0}}));
    source.after().get_or_add(slic3r_api::EPropertyAttributes::key)
        .extrusion_role(RAW_EXTRUSION_ROLE_TRAVEL | RAW_EXTRUSION_ROLE_WIPE)
        .mm3_per_mm(0.).width(0.f).height(0.f);
    target.before().get_or_add(slic3r_api::EPropertyAttributes::key)
        .extrusion_role(RAW_EXTRUSION_ROLE_TRAVEL | RAW_EXTRUSION_ROLE_WIPE)
        .mm3_per_mm(0.).width(0.f).height(0.f);

    run_layer_edit_plugins(print, {ENTRY_STATE_PLUGIN, TRAVEL_PLUGIN});

    target = ordered_scope(*tool.extrusions[1].root);
    REQUIRE(target.travel().point_count() == 2);
    CHECK(target.travel().point(0).x == scale_i(1.5));
    CHECK(target.travel().point(1).x == scale_i(2.5));
}

TEST_CASE("Semantic continuous transitions keep the travel slot empty",
          "[plugins][layer-extrusion-edit][travel][semantic-transition]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    plan.groups.back().layers.emplace_back();
    PrintingLayerGroup &layer = plan.groups.back().layers.back();
    layer.print_z = scale_i(0.2);
    layer.tool_groups.emplace_back();
    PrintingToolGroup &tool = layer.tool_groups.back();
    tool.extruder_id = 0;
    std::unique_ptr<ExtrusionPath> source = make_path(
        Point(scale_i(0.), 0), Point(scale_i(1.), 0));
    ExtrusionPropertyModifier modifier;
    modifier.set_enforce_retraction(true);
    source->add_property(modifier);
    append_extrusion(tool, std::move(source));
    append_extrusion(tool, make_path(
        Point(scale_i(1.), 0), Point(scale_i(2.), 0)));

    run_travel_plugins(print);

    const slic3r_api::LayerExtrusionEdit::ExtrusionScope::OrderedExtrusionScope target =
        ordered_scope(*tool.extrusions[1].root);
    REQUIRE(target.has_incoming_transition());
    CHECK(target.travel().segment_count() == 0);
    const slic3r_api::PrintingExtrusionScopeProperty *source_property =
        slic3r_api::MutableExtrusionEntity(reinterpret_cast<extrusion_entity_handle *>(
            tool.extrusions[0].root.get())).get(scope_property_key());
    const slic3r_api::PrintingExtrusionScopeProperty *target_property =
        slic3r_api::MutableExtrusionEntity(reinterpret_cast<extrusion_entity_handle *>(
            tool.extrusions[1].root.get())).get(scope_property_key());
    REQUIRE(source_property != nullptr);
    REQUIRE(target_property != nullptr);
    CHECK_FALSE(slic3r_api::printing_extrusion_scope_has_flag(
        *source_property,
        slic3r_api::PRINTING_EXTRUSION_SCOPE_OUTGOING_TRAVEL_MATERIALIZED));
    CHECK_FALSE(slic3r_api::printing_extrusion_scope_has_flag(
        *target_property,
        slic3r_api::PRINTING_EXTRUSION_SCOPE_INCOMING_TRAVEL_MATERIALIZED));
}
