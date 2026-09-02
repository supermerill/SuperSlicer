#include <catch2/catch.hpp>

#include <initializer_list>
#include <memory>
#include <vector>

#include "plugin_test_helpers.hpp"

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/host/Plugin.hpp"
#include "libslic3r/Api/plugin/cpp/ExtrusionViews.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Printing/PrintingPlan.hpp"
#include "libslic3r/Plugins/PrintingPlan/EntryPointProperty.h"
#include "libslic3r/Steps/StepExtrusionOrdering.hpp"

/*
PrintingExtrusion pre-sort tests
===============================

These tests construct the final ownership hierarchy directly, then execute only
the pre-sort provider. This isolates its contract: each tool visit may reorder
its own PrintingExtrusion vector, while every selected coarse endpoint is
published on the corresponding root for the following pipeline step.
*/

namespace {
using namespace Slic3r;
using namespace Slic3r::Printing;
using slic3r_api::EntryPointProperty;
using slic3r_api::PluginPropertyKey;

constexpr const char *PRE_SORT_PLUGIN =
    "ordering.printing_extrusion.presort.default";

/* Restore the process-wide plugin selection after one focused test run. */
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

/* Build an atomic open path without making its internal orientation sortable. */
ExtrusionEntityUPtr make_path(const Point &first, const Point &last)
{
    return std::make_unique<ExtrusionEntity>(false, ArcPolyline(Points{first, last}));
}

/* Build a closed rectangle whose four stored corners are valid candidates. */
ExtrusionEntityUPtr make_rectangle(const coord_t min_x,
                                   const coord_t min_y,
                                   const coord_t max_x,
                                   const coord_t max_y)
{
    return std::make_unique<ExtrusionEntity>(
        false,
        ArcPolyline(Points{
            Point(min_x, min_y), Point(max_x, min_y), Point(max_x, max_y),
            Point(min_x, max_y), Point(min_x, min_y)}));
}

/* Append one independently identifiable extrusion to a native tool visit. */
PrintingExtrusion &append_extrusion(PrintingToolGroup &tool,
                                    ExtrusionEntityUPtr root,
                                    const uint16_t identity)
{
    PrintingExtrusion extrusion;
    extrusion.root = std::move(root);
    extrusion.sregion_island_role = ExtrusionRole::Perimeter;
    extrusion.object_instance_idx = identity;
    tool.extrusions.push_back(std::move(extrusion));
    return tool.extrusions.back();
}

/* Add one layer containing one tool visit. */
PrintingToolGroup &append_tool(PrintingGroup &group, const uint16_t extruder_id = 0)
{
    if (group.layers.empty())
        group.layers.emplace_back();
    group.layers.back().tool_groups.emplace_back();
    PrintingToolGroup &tool = group.layers.back().tool_groups.back();
    tool.extruder_id = extruder_id;
    return tool;
}

/* Add a later layer and return its first tool visit. */
PrintingToolGroup &append_layer_with_tool(PrintingGroup &group,
                                          const uint16_t extruder_id = 0)
{
    group.layers.emplace_back();
    group.layers.back().tool_groups.emplace_back();
    PrintingToolGroup &tool = group.layers.back().tool_groups.back();
    tool.extruder_id = extruder_id;
    return tool;
}

/* Resolve the shared property key from the production orchestrator. */
PluginPropertyKey<EntryPointProperty> entry_point_key()
{
    return slic3r_api::entry_point_property_key(
        reinterpret_cast<orchestrator_handle *>(&Orchestrator::instance()));
}

/* Return a mutable API view over one native root. */
slic3r_api::MutableExtrusionEntity mutable_root(PrintingExtrusion &extrusion)
{
    return slic3r_api::MutableExtrusionEntity(
        reinterpret_cast<extrusion_entity_handle *>(extrusion.root.get()));
}

/* Execute only the pre-sort provider on the plan already owned by print. */
void run_pre_sort(Print &print)
{
    ScopedActivePlugins active{PRE_SORT_PLUGIN};
    Orchestrator &orchestrator = Orchestrator::instance();
    orchestrator.reset_plugin_cancel();
    Steps::StepExtrusionOrdering::run_step(orchestrator, print);
    REQUIRE_FALSE(orchestrator.is_plugin_cancelled());
}

/* Read the direct estimate attached to one extrusion root. */
const EntryPointProperty *entry_points(PrintingExtrusion &extrusion)
{
    return entry_point_key().get(mutable_root(extrusion));
}

} // namespace

TEST_CASE("PrintingExtrusion pre-sort registers as the final default ordering provider",
          "[plugins][ordering][printing-extrusion-presort]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Plugin *plugin = Orchestrator::instance().get_plugin(PRE_SORT_PLUGIN);
    REQUIRE(plugin != nullptr);
    CHECK(Orchestrator::instance().is_plugin_active(PRE_SORT_PLUGIN));
    CHECK(plugin->get_step() == STEP_ORDERING);
    CHECK(plugin->get_priority() == 1000);
    CHECK(plugin->get_exclusive_group() == "ordering.printing_extrusion.presort");

    /* The setting is optional and not defined by this branch. Declaring an
       unavailable used key would make the orchestrator disable the plugin. */
    CHECK(plugin->get_used_config_keys().empty());
}

TEST_CASE("PrintingExtrusion pre-sort selects each real rectangle extremum",
          "[plugins][ordering][printing-extrusion-presort][entry-points]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    struct SelectionCase
    {
        Point preceding_point;
        Point expected;
    };
    const coord_t side = scale_i(10.0);
    const std::vector<SelectionCase> cases{
        {Point(scale_i(-1.0), coord_t(0)), Point(0, 0)},
        {Point(scale_i(11.0), side), Point(side, side)},
        {Point(side, scale_i(-1.0)), Point(side, 0)},
        {Point(0, scale_i(11.0)), Point(0, side)}};

    for (const SelectionCase &selection : cases) {
        Print print;
        PrintingPlan &plan = print.mutable_printing_plan();
        plan.groups.emplace_back();
        PrintingToolGroup &preceding = append_tool(plan.groups.back());
        append_extrusion(preceding,
            make_path(selection.preceding_point, selection.preceding_point), 0);
        PrintingToolGroup &tool = append_tool(plan.groups.back());
        append_extrusion(tool, make_rectangle(0, 0, side, side), 1);

        run_pre_sort(print);

        const EntryPointProperty *property = entry_points(tool.extrusions.front());
        REQUIRE(property != nullptr);
        CHECK(property->entry.x == selection.expected.x());
        CHECK(property->entry.y == selection.expected.y());
        CHECK(property->exit.x == selection.expected.x());
        CHECK(property->exit.y == selection.expected.y());
    }
}

TEST_CASE("PrintingExtrusion pre-sort moves only non-empty roots in one tool vector",
          "[plugins][ordering][printing-extrusion-presort]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Print print;
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    PrintingToolGroup &preceding = append_tool(plan.groups.back());
    append_extrusion(preceding,
        make_path(Point(scale_i(100.0), coord_t(0)), Point(scale_i(100.0), coord_t(0))), 0);
    PrintingToolGroup &tool = append_tool(plan.groups.back());

    append_extrusion(tool,
        make_path(Point(scale_i(0.0), coord_t(0)), Point(scale_i(1.0), coord_t(0))), 10);
    PrintingExtrusion &empty = append_extrusion(
        tool, std::make_unique<ExtrusionEntityCollection>(false, false), 20);
    EntryPointProperty &stale = entry_point_key().get_or_add(mutable_root(empty));
    stale.entry = c_point{123, 456};
    stale.exit = stale.entry;
    append_extrusion(tool,
        make_path(Point(scale_i(99.0), coord_t(0)), Point(scale_i(100.0), coord_t(0))), 30);

    const bool first_can_sort = tool.extrusions.front().root->can_sort();
    const bool first_can_reverse = tool.extrusions.front().root->can_reverse();
    Points first_points;
    tool.extrusions.front().root->collect_points(first_points);
    run_pre_sort(print);

    REQUIRE(tool.extrusions.size() == 3);
    CHECK(tool.extrusions[0].object_instance_idx == 30);
    CHECK(tool.extrusions[1].object_instance_idx == 20);
    CHECK(tool.extrusions[2].object_instance_idx == 10);
    CHECK(entry_points(tool.extrusions[1]) == nullptr);
    CHECK(tool.extrusions[2].root->can_sort() == first_can_sort);
    CHECK(tool.extrusions[2].root->can_reverse() == first_can_reverse);
    Points reordered_first_points;
    tool.extrusions[2].root->collect_points(reordered_first_points);
    CHECK(reordered_first_points == first_points);
    CHECK(entry_points(tool.extrusions[0]) != nullptr);
    CHECK(entry_points(tool.extrusions[2]) != nullptr);
}

TEST_CASE("PrintingExtrusion pre-sort carries its selected exit through the complete plan",
          "[plugins][ordering][printing-extrusion-presort][continuity]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    SECTION("between tool groups") {
        Print print;
        PrintingPlan &plan = print.mutable_printing_plan();
        plan.groups.emplace_back();
        PrintingToolGroup &source = append_tool(plan.groups.back(), 0);
        append_extrusion(source,
            make_path(Point(scale_i(99.0), coord_t(0)), Point(scale_i(100.0), coord_t(0))), 1);
        PrintingToolGroup &target = append_tool(plan.groups.back(), 1);
        append_extrusion(target,
            make_path(Point(scale_i(0.0), coord_t(0)), Point(scale_i(1.0), coord_t(0))), 2);
        append_extrusion(target,
            make_path(Point(scale_i(109.0), coord_t(0)), Point(scale_i(110.0), coord_t(0))), 3);

        run_pre_sort(print);
        CHECK(plan.groups[0].layers[0].tool_groups[1].extrusions.front().object_instance_idx == 3);
    }

    SECTION("between layers") {
        Print print;
        PrintingPlan &plan = print.mutable_printing_plan();
        plan.groups.emplace_back();
        PrintingToolGroup &source = append_tool(plan.groups.back(), 0);
        append_extrusion(source,
            make_path(Point(scale_i(99.0), coord_t(0)), Point(scale_i(100.0), coord_t(0))), 1);
        PrintingToolGroup &target = append_layer_with_tool(plan.groups.back(), 0);
        append_extrusion(target,
            make_path(Point(scale_i(0.0), coord_t(0)), Point(scale_i(1.0), coord_t(0))), 2);
        append_extrusion(target,
            make_path(Point(scale_i(109.0), coord_t(0)), Point(scale_i(110.0), coord_t(0))), 3);

        run_pre_sort(print);
        CHECK(plan.groups[0].layers[1].tool_groups[0].extrusions.front().object_instance_idx == 3);
    }

    SECTION("between printing groups") {
        Print print;
        PrintingPlan &plan = print.mutable_printing_plan();
        plan.groups.emplace_back();
        PrintingToolGroup &source = append_tool(plan.groups.back(), 0);
        append_extrusion(source,
            make_path(Point(scale_i(99.0), coord_t(0)), Point(scale_i(100.0), coord_t(0))), 1);
        plan.groups.emplace_back();
        PrintingToolGroup &target = append_tool(plan.groups.back(), 0);
        append_extrusion(target,
            make_path(Point(scale_i(0.0), coord_t(0)), Point(scale_i(1.0), coord_t(0))), 2);
        append_extrusion(target,
            make_path(Point(scale_i(109.0), coord_t(0)), Point(scale_i(110.0), coord_t(0))), 3);

        run_pre_sort(print);
        CHECK(plan.groups[1].layers[0].tool_groups[0].extrusions.front().object_instance_idx == 3);
    }
}

TEST_CASE("PrintingExtrusion pre-sort falls back to the origin and handles spatial requests",
          "[plugins][ordering][printing-extrusion-presort][spatial]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Print print;
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    PrintingToolGroup &tool = append_tool(plan.groups.back());

    /* Eleven items exceed the exact-solver threshold and exercise the KD-tree path. */
    for (uint16_t idx = 0; idx < 11; ++idx) {
        const double x = double(10 - idx) * 10.0;
        append_extrusion(tool,
            make_path(Point(scale_i(x), coord_t(0)), Point(scale_i(x + 1.0), coord_t(0))), idx);
    }

    run_pre_sort(print);

    REQUIRE(tool.extrusions.size() == 11);
    CHECK(tool.extrusions.front().object_instance_idx == 10);
    for (PrintingExtrusion &extrusion : tool.extrusions)
        CHECK(entry_points(extrusion) != nullptr);
}
