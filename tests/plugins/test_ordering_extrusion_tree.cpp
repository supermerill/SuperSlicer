#include <catch2/catch.hpp>

#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <vector>

#include "plugin_test_helpers.hpp"

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/host/Plugin.hpp"
#include "libslic3r/Api/plugin/cpp/PrintingPlanViews.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Printing/PrintingPlan.hpp"
#include "libslic3r/Plugins/PrintingPlan/EntryPointProperty.h"
#include "libslic3r/Steps/StepExtrusionOrdering.hpp"

/*
Fallback extrusion-tree ordering tests
======================================

The provider runs after the coarse PrintingExtrusion pre-sort. These tests
prebuild the relevant PrintingPlan fragments so they can verify how the first
estimate starts the sequential pass and how exact exits then propagate through
the remaining plan.
*/

namespace {
using namespace Slic3r;
using namespace Slic3r::Printing;
using slic3r_api::EntryPointProperty;
using slic3r_api::PluginPropertyKey;

constexpr const char *TREE_ORDERING_PLUGIN =
    "ordering.extrusion_tree.default";

/* Restore the process-wide active providers after one focused step run. */
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

/* Build one open leaf whose endpoints make ordering decisions easy to read. */
ExtrusionEntityUPtr make_path(const Point &start,
                              const Point &end,
                              const bool reversible = false)
{
    return std::make_unique<ExtrusionEntity>(
        reversible, ArcPolyline(Points{start, end}));
}

/* Build a sortable root containing the supplied open paths in source order. */
std::unique_ptr<ExtrusionEntityCollection> make_sortable_root(
    std::initializer_list<std::pair<Point, Point>> paths)
{
    std::unique_ptr<ExtrusionEntityCollection> root =
        std::make_unique<ExtrusionEntityCollection>(true, true);
    for (const std::pair<Point, Point> &path : paths)
        root->append(make_path(path.first, path.second));
    return root;
}

/* Append one owned root to a tool visit without requiring regional context. */
PrintingExtrusion &append_extrusion(PrintingToolGroup &tool,
                                    ExtrusionEntityUPtr root)
{
    PrintingExtrusion extrusion;
    extrusion.root = std::move(root);
    extrusion.sregion_island_role = ExtrusionRole::Perimeter;
    tool.extrusions.push_back(std::move(extrusion));
    return tool.extrusions.back();
}

/* Add one layer with one tool visit to a native PrintingGroup. */
PrintingLayerGroup &append_layer(PrintingGroup &group)
{
    group.layers.emplace_back();
    PrintingLayerGroup &layer = group.layers.back();
    layer.tool_groups.emplace_back();
    layer.tool_groups.back().extruder_id = 0;
    return layer;
}

/* Resolve the private key from the same orchestrator as the production plugin. */
PluginPropertyKey<EntryPointProperty> entry_point_key()
{
    return slic3r_api::entry_point_property_key(
        reinterpret_cast<orchestrator_handle *>(&Orchestrator::instance()));
}

/* Return a mutable API view over a native extrusion root. */
slic3r_api::MutableExtrusionEntity mutable_root(PrintingExtrusion &extrusion)
{
    return slic3r_api::MutableExtrusionEntity(
        reinterpret_cast<extrusion_entity_handle *>(extrusion.root.get()));
}

/* Execute only the fallback tree-ordering provider through the real host. */
void run_tree_ordering(Print &print)
{
    ScopedActivePlugins active{TREE_ORDERING_PLUGIN};
    Orchestrator &orchestrator = Orchestrator::instance();
    orchestrator.reset_plugin_cancel();
    Steps::StepExtrusionOrdering::run_step(orchestrator, print);
    REQUIRE_FALSE(orchestrator.is_plugin_cancelled());
}

} // namespace

TEST_CASE("EntryPointProperty has one compatible private dynamic contract",
          "[plugins][ordering][extrusion-tree-ordering][properties]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Orchestrator &orchestrator = Orchestrator::instance();
    orchestrator_handle *handle = reinterpret_cast<orchestrator_handle *>(&orchestrator);
    Plugin *plugin = orchestrator.get_plugin(TREE_ORDERING_PLUGIN);
    REQUIRE(plugin != nullptr);
    CHECK(plugin->get_step() == STEP_ORDERING);
    CHECK(plugin->get_priority() == 1100);
    CHECK(plugin->get_exclusive_group() == "ordering.extrusion_tree");
    CHECK(plugin->get_dependencies().empty());

    const PluginPropertyKey<EntryPointProperty> first =
        slic3r_api::entry_point_property_key(handle);
    const PluginPropertyKey<EntryPointProperty> second =
        slic3r_api::entry_point_property_key(handle);
    CHECK(first.type() == second.type());

    struct IncompatibleEntryPointProperty
    {
        c_point entry;
    };
    CHECK_THROWS_AS(
        PluginPropertyKey<IncompatibleEntryPointProperty>::register_dynamic(
            handle, PRINTING_EXTRUSION_ENTRY_POINT_PROPERTY_NAME),
        std::runtime_error);
}

TEST_CASE("Extrusion-tree ordering publishes exact endpoints and propagates local exits",
          "[plugins][ordering][extrusion-tree-ordering]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    PrintingLayerGroup &layer = append_layer(plan.groups.back());
    PrintingToolGroup &tool = layer.tool_groups.back();
    tool.extrusions.reserve(2);

    PrintingExtrusion &first = append_extrusion(
        tool, make_path(Point(0, 0), Point(100, 0)));
    PrintingExtrusion &second = append_extrusion(
        tool, make_sortable_root({
            {Point(1, 0), Point(2, 0)},
            {Point(101, 0), Point(102, 0)}}));

    run_tree_ordering(print);

    REQUIRE(second.root->child_count() == 2);
    CHECK(second.root->child(0).first_point() == Point(101, 0));
    CHECK_FALSE(second.root->can_sort());

    const PluginPropertyKey<EntryPointProperty> key = entry_point_key();
    const EntryPointProperty *first_points = key.get(mutable_root(first));
    const EntryPointProperty *second_points = key.get(mutable_root(second));
    REQUIRE(first_points != nullptr);
    REQUIRE(second_points != nullptr);
    CHECK(first_points->entry.x == 0);
    CHECK(first_points->exit.x == 100);
    CHECK(second_points->entry.x == 101);
    CHECK(second_points->exit.x == 2);
}

TEST_CASE("Extrusion-tree ordering propagates exact exits across layers",
          "[plugins][ordering][extrusion-tree-ordering]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    PrintingGroup &group = plan.groups.back();
    group.layers.reserve(2);

    PrintingLayerGroup &first_layer = append_layer(group);
    append_extrusion(first_layer.tool_groups.back(), make_path(Point(0, 0), Point(10, 0)));

    PrintingLayerGroup &second_layer = append_layer(group);
    append_extrusion(
        second_layer.tool_groups.back(),
        make_sortable_root({
            {Point(1, 0), Point(2, 0)},
            {Point(101, 0), Point(102, 0)}}));

    PrintingExtrusion &source = group.layers[0].tool_groups[0].extrusions[0];
    PrintingExtrusion &target = group.layers[1].tool_groups[0].extrusions[0];

    // The pre-sort estimate is deliberately stale. Sequential tree ordering
    // replaces it before selecting the first child of the following layer.
    EntryPointProperty &estimate = entry_point_key().get_or_add(mutable_root(source));
    estimate.entry = c_point{0, 0};
    estimate.exit = c_point{100, 0};

    run_tree_ordering(print);

    CHECK(target.root->child(0).first_point() == Point(1, 0));
    const EntryPointProperty *source_points = entry_point_key().get(mutable_root(source));
    REQUIRE(source_points != nullptr);
    CHECK(source_points->exit.x == 10);
}

TEST_CASE("Extrusion-tree ordering falls back to origin and removes empty metadata",
          "[plugins][ordering][extrusion-tree-ordering]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    PrintingGroup &group = plan.groups.back();
    group.layers.reserve(2);
    append_layer(group);

    PrintingLayerGroup &second_layer = append_layer(group);
    PrintingToolGroup &tool = second_layer.tool_groups.back();
    tool.extrusions.reserve(2);
    append_extrusion(
        tool, make_sortable_root({
            {Point(100, 0), Point(110, 0)},
            {Point(1, 0), Point(2, 0)}}));
    append_extrusion(
        tool, std::make_unique<ExtrusionEntityCollection>(true, true));

    PrintingExtrusion &ordered = group.layers[1].tool_groups[0].extrusions[0];
    PrintingExtrusion &empty = group.layers[1].tool_groups[0].extrusions[1];

    const PluginPropertyKey<EntryPointProperty> key = entry_point_key();
    EntryPointProperty &stale = key.get_or_add(mutable_root(empty));
    stale.entry = c_point{7, 8};
    stale.exit = c_point{9, 10};

    run_tree_ordering(print);

    // No earlier geometric root or pre-sort estimate exists, so the first
    // sortable root starts from the documented neutral origin.
    CHECK(ordered.root->child(0).first_point() == Point(1, 0));
    CHECK(key.get(mutable_root(empty)) == nullptr);
}

TEST_CASE("Extrusion-tree ordering uses the first pre-sort estimate",
          "[plugins][ordering][extrusion-tree-ordering]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    PrintingLayerGroup &layer = append_layer(plan.groups.back());
    PrintingExtrusion &extrusion = append_extrusion(
        layer.tool_groups.back(),
        make_sortable_root({
            {Point(1, 0), Point(2, 0)},
            {Point(101, 0), Point(102, 0)}}));

    EntryPointProperty &estimate =
        entry_point_key().get_or_add(mutable_root(extrusion));
    estimate.entry = c_point{100, 0};
    estimate.exit = c_point{100, 0};

    run_tree_ordering(print);

    CHECK(extrusion.root->child(0).first_point() == Point(101, 0));
    const EntryPointProperty *exact =
        entry_point_key().get(mutable_root(extrusion));
    REQUIRE(exact != nullptr);
    CHECK(exact->entry.x == 101);
    CHECK(exact->exit.x == 2);
}
