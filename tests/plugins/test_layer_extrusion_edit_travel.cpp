#include <catch2/catch.hpp>

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <vector>

#include "plugin_test_helpers.hpp"

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/host/Plugin.hpp"
#include "libslic3r/Api/plugin/cpp/ExtrusionViews.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/ExtrusionProperty.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Printing/PrintingPlan.hpp"
#include "libslic3r/Steps/StepLayerExtrusionEdition.hpp"

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
constexpr const char *TRAVEL_PLUGIN = "layer_extrusion_edit.travel.default";

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

void append_extrusion(PrintingToolGroup &tool, std::unique_ptr<ExtrusionEntity> root)
{
    PrintingExtrusion extrusion;
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

void run_travel_plugins(Print &print)
{
    ScopedActivePlugins active({ENTRY_STATE_PLUGIN, TRAVEL_PLUGIN});
    Orchestrator &orchestrator = Orchestrator::instance();
    orchestrator.reset_plugin_cancel();
    Steps::StepLayerExtrusionEdition::run_step(orchestrator, print);
    REQUIRE_FALSE(orchestrator.is_plugin_cancelled());
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
    REQUIRE(plugin->get_dependencies().size() == 1);
    CHECK(plugin->get_dependencies().front() == ENTRY_STATE_PLUGIN);
    CHECK(plugin->get_used_config_keys().empty());
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

    const ExtrusionEntity &wrapped =
        *plan.groups[0].layers[0].tool_groups[1].extrusions[0].root;
    REQUIRE(wrapped.child_count() == 2);
    CHECK_FALSE(wrapped.can_sort());
    CHECK_FALSE(wrapped.can_reverse());
    const ExtrusionAttributes *travel_attributes =
        wrapped.child(0).get_property<ExtrusionAttributes>();
    const ExtrusionAttributes *print_attributes =
        wrapped.child(1).get_property<ExtrusionAttributes>();
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

    ExtrusionEntity *closed_source = tool.extrusions[1].root.get();
    REQUIRE(closed_source->polyline_ref().has_arc());
    run_travel_plugins(print);

    CHECK(travel_count(print) == 2);
    REQUIRE(closed_source->has_polyline());
    CHECK(closed_source->first_point() == first_end);
    CHECK(closed_source->last_point() == first_end);
    CHECK(closed_source->polyline_ref().z_offset(0) == scale_i(0.03));
    CHECK(closed_source->polyline_ref().z_offset(closed_source->polyline_ref().size() - 1) ==
          scale_i(0.03));
    CHECK(closed_source->polyline_ref().has_arc());

    const ExtrusionEntity &xy_wrapper = *tool.extrusions[2].root;
    REQUIRE(xy_wrapper.child_count() == 2);
    REQUIRE(xy_wrapper.child(0).has_polyline());
    CHECK(xy_wrapper.child(0).polyline_ref().z_offset(0) == scale_i(0.03));
    CHECK(xy_wrapper.child(0).polyline_ref().z_offset(1) == scale_i(0.03));

    const ExtrusionEntity &z_wrapper = *tool.extrusions[3].root;
    REQUIRE(z_wrapper.child_count() == 2);
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
}
