#include <catch2/catch.hpp>

#include <atomic>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "plugin_test_helpers.hpp"

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/host/Plugin.hpp"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_ordering.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_seam_placer.h"
#include "libslic3r/Api/plugin/cpp/OrchestratorViews.hpp"
#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"
#include "libslic3r/Api/plugin/cpp/SeamPlacerViews.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Printing/PrintingPlan.hpp"
#include "libslic3r/Plugins/PrintingPlan/EntryPointProperty.h"

/*
Advanced extrusion-tree ordering tests
======================================

These tests exercise the full provider: its shared seam session, coarse
parallel pass, sequential PrintingExtrusion barrier and final parallel pass.
The fixtures use small deterministic trees so each ordering decision remains
visible without duplicating the ordering engine's own tests.

PrintingExtrusion values move during the barrier. Tests reacquire them through
object_instance_idx and retain only stable extrusion-tree handles.
*/

namespace {

using namespace Slic3r;
using namespace Slic3r::Printing;
using slic3r_api::EntryPointProperty;
using slic3r_api::PluginPropertyKey;

const char *k_advanced_plugin = "ordering.extrusion_tree.advanced";
const char *k_seam_plugin = "slic3r.test.advanced_ordering.seam_placer";
const char *k_seam_group = "seam_placer_plugin";

enum class SeamPublishMode { Valid, Empty, ThrowDuringInitialization };
enum class SeamStrategy { Front, FixedPoint, NearestVertex };

struct SeamServiceState
{
    std::atomic<uint32_t> initialize_count { 0 };
    std::atomic<uint32_t> destroy_count { 0 };
    std::atomic<uint32_t> place_count { 0 };
    const printing_plan_handle *initialized_plan = nullptr;
    SeamPublishMode mode = SeamPublishMode::Valid;
    SeamStrategy strategy = SeamStrategy::Front;
    c_point fixed_point = {};

    void reset()
    {
        initialize_count.store(0, std::memory_order_relaxed);
        destroy_count.store(0, std::memory_order_relaxed);
        place_count.store(0, std::memory_order_relaxed);
        initialized_plan = nullptr;
        mode = SeamPublishMode::Valid;
        strategy = SeamStrategy::Front;
        fixed_point = {};
    }
};

SeamServiceState g_seam_state;

double squared_distance(const c_point lhs, const c_point rhs)
{
    const double dx = double(lhs.x) - double(rhs.x);
    const double dy = double(lhs.y) - double(rhs.y);
    return dx * dx + dy * dy;
}

/* Select the nearest stored vertex without flattening lines or arcs. */
c_point nearest_loop_vertex(const slic3r_api::ExtrusionEntity &entity,
                            const c_point start_position)
{
    c_point best = {};
    double best_distance = (std::numeric_limits<double>::max)();
    bool found = false;
    for (uint32_t point_idx = 0; point_idx < entity.point_count(); ++point_idx) {
        const c_point candidate = entity.point(point_idx);
        const double distance = squared_distance(candidate, start_position);
        if (!found || distance < best_distance ||
            (distance == best_distance &&
             (candidate.x < best.x ||
              (candidate.x == best.x && candidate.y < best.y)))) {
            best = candidate;
            best_distance = distance;
            found = true;
        }
    }
    for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx) {
        const slic3r_api::ExtrusionEntity child = entity.child(child_idx);
        if (child.empty())
            continue;
        const c_point candidate = nearest_loop_vertex(child, start_position);
        const double distance = squared_distance(candidate, start_position);
        if (!found || distance < best_distance ||
            (distance == best_distance &&
             (candidate.x < best.x ||
              (candidate.x == best.x && candidate.y < best.y)))) {
            best = candidate;
            best_distance = distance;
            found = true;
        }
    }
    if (!found)
        throw std::runtime_error("The seam fixture received an empty loop.");
    return best;
}

class OrderingSeamSession final : public slic3r_api::SeamPlacerSession
{
public:
    explicit OrderingSeamSession(SeamServiceState &state) : m_state(state) {}
    ~OrderingSeamSession() override
    {
        m_state.destroy_count.fetch_add(1, std::memory_order_relaxed);
    }

    void initialize(const slic3r_api::PrintingPlan &plan) override
    {
        m_state.initialize_count.fetch_add(1, std::memory_order_relaxed);
        m_state.initialized_plan = plan.handle();
        if (m_state.mode == SeamPublishMode::ThrowDuringInitialization)
            throw std::runtime_error(
                "intentional ordering seam initialization failure");
    }

    c_point place_seam(const slic3r_api::ExtrusionEntity &loop,
                       const c_point start_position) const override
    {
        m_state.place_count.fetch_add(1, std::memory_order_relaxed);
        if (m_state.strategy == SeamStrategy::FixedPoint)
            return m_state.fixed_point;
        if (m_state.strategy == SeamStrategy::NearestVertex)
            return nearest_loop_vertex(loop, start_position);
        return loop.front();
    }

private:
    SeamServiceState &m_state;
};

class OrderingSeamPlugin final : public slic3r_api::PluginBase
{
public:
    explicit OrderingSeamPlugin(orchestrator_handle *orchestrator) :
        PluginBase(orchestrator) {}

private:
    const char *id_impl() const noexcept override { return k_seam_plugin; }
    const char *exclusive_group_impl() const noexcept override { return k_seam_group; }
    slicing_step_t step_impl() const noexcept override { return SEAM_PLACER; }
    const char *const *dependencies_impl() const noexcept override
    {
        static const char *dependencies[] = { nullptr };
        return dependencies;
    }
    int32_t priority_impl() const noexcept override { return 0; }

    void run_impl(const plugin_run_context *run_ctx) const override
    {
        run_ctx_seam_placer *context = plugin_ctx_as_seam_placer(run_ctx);
        if (context == nullptr || context->plan == nullptr)
            throw std::invalid_argument(
                "The ordering seam fixture requires a PrintingPlan.");
        if (g_seam_state.mode == SeamPublishMode::Empty)
            return;
        context->instance = slic3r_api::make_seam_placer_instance(
            std::unique_ptr<slic3r_api::SeamPlacerSession>(
                new OrderingSeamSession(g_seam_state)),
            slic3r_api::PrintingPlan(context->plan));
    }
};

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
        m_orchestrator.reset_plugin_cancel();
    }

    ~ScopedActivePlugins()
    {
        m_orchestrator.clear_active_plugins();
        for (Plugin *plugin : m_previous)
            m_orchestrator.set_plugin_active(plugin, true);
        m_orchestrator.reset_plugin_cancel();
        g_seam_state.reset();
    }

private:
    Orchestrator &m_orchestrator;
    std::vector<Plugin *> m_previous;
};

void ensure_ordering_seam_provider_registered()
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Orchestrator &orchestrator = Orchestrator::instance();
    if (orchestrator.get_plugin(k_seam_plugin) != nullptr)
        return;
    static std::unique_ptr<OrderingSeamPlugin> plugin;
    plugin.reset(new OrderingSeamPlugin(
        reinterpret_cast<orchestrator_handle *>(&orchestrator)));
    REQUIRE(orchestrator.register_plugin(plugin->c_instance()));
}

c_point mm_point(const double x, const double y)
{
    return c_point{scale_i(x), scale_i(y)};
}

ExtrusionEntityUPtr make_open_path(const double x0, const double y0,
                                  const double x1, const double y1,
                                  const bool reversible)
{
    return std::make_unique<ExtrusionEntity>(
        reversible, ArcPolyline(Points{Point::new_scale(x0, y0),
                                       Point::new_scale(x1, y1)}));
}

ExtrusionEntityUPtr make_rectangular_loop(const double left,
                                         const double right,
                                         const double bottom = 0.0,
                                         const double top = 1.0)
{
    return std::make_unique<ExtrusionEntity>(
        false, ArcPolyline(Points{Point::new_scale(left, bottom),
                                  Point::new_scale(right, bottom),
                                  Point::new_scale(right, top),
                                  Point::new_scale(left, top),
                                  Point::new_scale(left, bottom)}));
}

ExtrusionEntityUPtr make_arc_loop(const bool with_z_offsets)
{
    const coord_t radius = scale_i(5.0);
    const Point center = Point::new_scale(10.0, 10.0);
    ArcPolyline polyline;
    polyline.append(center + Point(radius, 0));
    polyline.append(Geometry::ArcWelder::Segment(
        center + Point(0, radius), float(radius), Geometry::ArcWelder::Orientation::CCW));
    polyline.append(Geometry::ArcWelder::Segment(
        center + Point(-radius, 0), float(radius), Geometry::ArcWelder::Orientation::CCW));
    polyline.append(Geometry::ArcWelder::Segment(
        center + Point(0, -radius), float(radius), Geometry::ArcWelder::Orientation::CCW));
    polyline.append(Geometry::ArcWelder::Segment(
        center + Point(radius, 0), float(radius), Geometry::ArcWelder::Orientation::CCW));
    if (with_z_offsets) {
        polyline.set_z_offset(0, 0);
        polyline.set_z_offset(1, scale_i(0.2));
        polyline.set_z_offset(2, scale_i(0.4));
        polyline.set_z_offset(3, scale_i(0.2));
        polyline.set_z_offset(4, 0);
    }
    return std::make_unique<ExtrusionEntity>(false, std::move(polyline));
}

ExtrusionEntityUPtr make_collection(ExtrusionEntity::Children children,
                                   const bool sortable,
                                   const bool reversible)
{
    return std::make_unique<ExtrusionEntity>(
        std::move(children), sortable, reversible, false);
}

ExtrusionEntityUPtr make_composed_loop()
{
    ExtrusionEntity::Children children;
    children.push_back(make_open_path(0.0, 0.0, 10.0, 0.0, false));
    children.push_back(make_open_path(10.0, 0.0, 10.0, 10.0, false));
    children.push_back(make_open_path(10.0, 10.0, 0.0, 0.0, false));
    return make_collection(std::move(children), false, false);
}

PrintingLayerGroup &append_layer(PrintingGroup &group)
{
    group.layers.emplace_back();
    return group.layers.back();
}

PrintingToolGroup &append_tool(PrintingLayerGroup &layer, const uint16_t extruder_id)
{
    layer.tool_groups.emplace_back();
    PrintingToolGroup &tool = layer.tool_groups.back();
    tool.extruder_id = extruder_id;
    return tool;
}

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

PrintingExtrusion &find_extrusion(PrintingToolGroup &tool, const uint16_t identity)
{
    for (PrintingExtrusion &extrusion : tool.extrusions)
        if (extrusion.object_instance_idx == identity)
            return extrusion;
    throw std::runtime_error("The expected PrintingExtrusion was not found.");
}

PluginPropertyKey<EntryPointProperty> entry_point_key()
{
    return slic3r_api::entry_point_property_key(
        reinterpret_cast<orchestrator_handle *>(&Orchestrator::instance()));
}

slic3r_api::MutableExtrusionEntity mutable_root(PrintingExtrusion &extrusion)
{
    return slic3r_api::MutableExtrusionEntity(
        reinterpret_cast<extrusion_entity_handle *>(extrusion.root.get()));
}

void publish_entry_points(PrintingExtrusion &extrusion,
                          const c_point entry, const c_point exit)
{
    EntryPointProperty &property =
        entry_point_key().get_or_add(mutable_root(extrusion));
    property.entry = entry;
    property.exit = exit;
}

const EntryPointProperty &published_entry_points(PrintingExtrusion &extrusion)
{
    const EntryPointProperty *property =
        entry_point_key().get(mutable_root(extrusion));
    REQUIRE(property != nullptr);
    return *property;
}

void check_ordering_disabled_recursively(const ExtrusionEntity &entity)
{
    CHECK_FALSE(entity.can_sort());
    CHECK_FALSE(entity.can_reverse());
    for (const ExtrusionEntityUPtr &child : entity.children())
        check_ordering_disabled_recursively(*child);
}

raw_plugin_execution_status run_advanced_ordering(Print &print)
{
    Orchestrator &orchestrator = Orchestrator::instance();
    slic3r_api::OrchestratorView view(
        reinterpret_cast<orchestrator_handle *>(&orchestrator));
    const slic3r_api::PluginView plugin = view.find_plugin(k_advanced_plugin);
    REQUIRE(plugin.valid());
    PrintingPlan &plan = print.mutable_printing_plan();
    run_ctx_extrusion_ordering context = {};
    context.print = reinterpret_cast<const print_handle *>(&print);
    context.plan = reinterpret_cast<printing_plan_handle *>(&plan);
    return view.execute(plugin, context.print, context);
}

} // namespace

TEST_CASE("Advanced extrusion-tree ordering registers beside the fallback",
          "[plugins][ordering][advanced-extrusion-tree]")
{
    ensure_ordering_seam_provider_registered();
    Orchestrator &orchestrator = Orchestrator::instance();
    Plugin *advanced = orchestrator.get_plugin(k_advanced_plugin);
    Plugin *fallback = orchestrator.get_plugin("ordering.extrusion_tree.default");
    REQUIRE(advanced != nullptr);
    REQUIRE(fallback != nullptr);
    CHECK(advanced->get_step() == STEP_ORDERING);
    CHECK(advanced->get_priority() == 1050);
    CHECK(fallback->get_priority() == 1100);
    CHECK(advanced->get_exclusive_group() == "ordering.extrusion_tree");
    CHECK(fallback->get_exclusive_group() == advanced->get_exclusive_group());

    std::ifstream stream(
        std::string(TEST_DATA_DIR) + "/../../src/plugins/default_activated.ini.in");
    REQUIRE(stream.good());
    std::ostringstream contents;
    contents << stream.rdbuf();
    CHECK(contents.str().find("ordering.extrusion_tree.advanced = 1") != std::string::npos);
    CHECK(contents.str().find("ordering.extrusion_tree.default = 1") == std::string::npos);
}

TEST_CASE("Advanced ordering shares one seam session across both parallel passes",
          "[plugins][ordering][advanced-extrusion-tree]")
{
    ensure_ordering_seam_provider_registered();
    ScopedActivePlugins active{k_advanced_plugin, k_seam_plugin};
    g_seam_state.strategy = SeamStrategy::NearestVertex;

    Print print;
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    PrintingLayerGroup &first_layer = append_layer(plan.groups.back());
    PrintingToolGroup &first_tool = append_tool(first_layer, 0);
    append_extrusion(first_tool, make_rectangular_loop(0.0, 2.0), 1);
    publish_entry_points(first_tool.extrusions[0], mm_point(0.0, 0.0), mm_point(0.0, 0.0));
    append_tool(first_layer, 1);
    PrintingToolGroup &second_tool = append_tool(append_layer(plan.groups.back()), 1);
    append_extrusion(second_tool, make_rectangular_loop(5.0, 7.0), 2);
    publish_entry_points(second_tool.extrusions[0], mm_point(5.0, 0.0), mm_point(5.0, 0.0));
    plan.groups.emplace_back();
    PrintingToolGroup &third_tool = append_tool(append_layer(plan.groups.back()), 2);
    append_extrusion(third_tool, make_open_path(9.0, 0.0, 10.0, 0.0, true), 3);
    publish_entry_points(third_tool.extrusions[0], mm_point(9.0, 0.0), mm_point(10.0, 0.0));

    REQUIRE(run_advanced_ordering(print) == RAW_PLUGIN_EXECUTION_SUCCESS);
    CHECK(g_seam_state.initialize_count.load(std::memory_order_relaxed) == 1);
    CHECK(g_seam_state.destroy_count.load(std::memory_order_relaxed) == 1);
    CHECK(g_seam_state.place_count.load(std::memory_order_relaxed) == 12);
    CHECK(g_seam_state.initialized_plan == reinterpret_cast<const printing_plan_handle *>(&plan));
    check_ordering_disabled_recursively(
        *plan.groups[0].layers[0].tool_groups[0].extrusions[0].root);
    check_ordering_disabled_recursively(
        *plan.groups[0].layers[1].tool_groups[0].extrusions[0].root);
    check_ordering_disabled_recursively(
        *plan.groups[1].layers[0].tool_groups[0].extrusions[0].root);
}

TEST_CASE("Advanced ordering uses its sequential barrier before final refinement",
          "[plugins][ordering][advanced-extrusion-tree]")
{
    ensure_ordering_seam_provider_registered();
    ScopedActivePlugins active{k_advanced_plugin, k_seam_plugin};
    g_seam_state.strategy = SeamStrategy::NearestVertex;

    Print print;
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    PrintingLayerGroup &layer = append_layer(plan.groups.back());
    PrintingToolGroup &first_tool = append_tool(layer, 0);
    append_extrusion(first_tool, make_open_path(150.0, 0.0, 200.0, 0.0, false), 10);
    append_extrusion(first_tool, std::make_unique<ExtrusionEntity>(false), 99);
    ExtrusionEntity::Children sortable_children;
    sortable_children.push_back(make_open_path(10.0, 0.0, 11.0, 0.0, false));
    sortable_children.push_back(make_rectangular_loop(1.0, 100.0));
    append_extrusion(first_tool, make_collection(std::move(sortable_children), true, false), 20);
    publish_entry_points(find_extrusion(first_tool, 10), mm_point(150.0, 0.0), mm_point(200.0, 0.0));
    publish_entry_points(find_extrusion(first_tool, 20), mm_point(100.0, 0.0), mm_point(0.0, 0.0));

    PrintingToolGroup &second_tool = append_tool(layer, 1);
    ExtrusionEntity::Children following_children;
    following_children.push_back(make_open_path(0.0, 5.0, 1.0, 5.0, false));
    following_children.push_back(make_open_path(199.0, 5.0, 201.0, 5.0, false));
    append_extrusion(second_tool, make_collection(std::move(following_children), true, false), 30);

    publish_entry_points(find_extrusion(second_tool, 30), mm_point(0.0, 5.0), mm_point(201.0, 5.0));

    REQUIRE(run_advanced_ordering(print) == RAW_PLUGIN_EXECUTION_SUCCESS);
    PrintingToolGroup &ordered_first_tool = plan.groups[0].layers[0].tool_groups[0];
    PrintingToolGroup &ordered_second_tool = plan.groups[0].layers[0].tool_groups[1];
    REQUIRE(ordered_first_tool.extrusions.size() == 3);
    CHECK(ordered_first_tool.extrusions[0].object_instance_idx == 20);
    CHECK(ordered_first_tool.extrusions[1].object_instance_idx == 99);
    CHECK(ordered_first_tool.extrusions[2].object_instance_idx == 10);

    PrintingExtrusion &ordered = find_extrusion(ordered_first_tool, 20);
    REQUIRE(ordered.root->child_count() == 2);
    CHECK(ordered.root->child(0).is_loop());
    CHECK(ordered.root->child(1).first_point() == Point::new_scale(10.0, 0.0));
    const EntryPointProperty &entry_points = published_entry_points(ordered);
    CHECK(entry_points.entry.x == scale_i(1.0));
    CHECK(entry_points.exit.x == scale_i(11.0));
    REQUIRE(ordered_second_tool.extrusions[0].root->child_count() == 2);
    CHECK(ordered_second_tool.extrusions[0].root->child(0).first_point() == Point::new_scale(199.0, 5.0));
    CHECK(g_seam_state.place_count.load(std::memory_order_relaxed) == 6);
    check_ordering_disabled_recursively(*ordered.root);
    check_ordering_disabled_recursively(*ordered_second_tool.extrusions[0].root);
}

TEST_CASE("Advanced ordering fixes reversible and nested trees",
          "[plugins][ordering][advanced-extrusion-tree]")
{
    ensure_ordering_seam_provider_registered();
    ScopedActivePlugins active{k_advanced_plugin, k_seam_plugin};

    SECTION("an open leaf reverses only for a strictly nearer end") {
        Print print;
        PrintingPlan &plan = print.mutable_printing_plan();
        plan.groups.emplace_back();
        PrintingToolGroup &tool = append_tool(append_layer(plan.groups.back()), 0);
        append_extrusion(tool, make_open_path(10.0, 0.0, 1.0, 0.0, true), 1);
        publish_entry_points(tool.extrusions[0], mm_point(10.0, 0.0), mm_point(1.0, 0.0));
        REQUIRE(run_advanced_ordering(print) == RAW_PLUGIN_EXECUTION_SUCCESS);
        CHECK(tool.extrusions[0].root->first_point() == Point::new_scale(1.0, 0.0));
        CHECK(tool.extrusions[0].root->last_point() == Point::new_scale(10.0, 0.0));
        check_ordering_disabled_recursively(*tool.extrusions[0].root);
    }

    SECTION("equal endpoint distances preserve the open leaf direction") {
        Print print;
        PrintingPlan &plan = print.mutable_printing_plan();
        plan.groups.emplace_back();
        PrintingToolGroup &tool = append_tool(append_layer(plan.groups.back()), 0);
        append_extrusion(tool, make_open_path(-1.0, 0.0, 1.0, 0.0, true), 2);
        publish_entry_points(tool.extrusions[0], mm_point(-1.0, 0.0), mm_point(1.0, 0.0));
        REQUIRE(run_advanced_ordering(print) == RAW_PLUGIN_EXECUTION_SUCCESS);
        CHECK(tool.extrusions[0].root->first_point() == Point::new_scale(-1.0, 0.0));
        CHECK(tool.extrusions[0].root->last_point() == Point::new_scale(1.0, 0.0));
    }

    SECTION("a fixed collection reverses as one unit") {
        Print print;
        PrintingPlan &plan = print.mutable_printing_plan();
        plan.groups.emplace_back();
        PrintingLayerGroup &layer = append_layer(plan.groups.back());
        PrintingToolGroup &preceding = append_tool(layer, 0);
        append_extrusion(
            preceding, make_open_path(31.0, 0.0, 31.0, 0.0, false), 30);
        publish_entry_points(
            preceding.extrusions[0], mm_point(31.0, 0.0), mm_point(31.0, 0.0));
        PrintingToolGroup &tool = append_tool(layer, 1);
        ExtrusionEntity::Children children;
        children.push_back(make_open_path(10.0, 0.0, 20.0, 0.0, true));
        children.push_back(make_open_path(20.0, 0.0, 30.0, 0.0, true));
        ExtrusionEntityUPtr root = make_collection(std::move(children), false, true);
        const ExtrusionEntity *first_handle = root->children()[0].get();
        const ExtrusionEntity *second_handle = root->children()[1].get();
        append_extrusion(tool, std::move(root), 3);
        publish_entry_points(tool.extrusions[0], mm_point(10.0, 0.0), mm_point(30.0, 0.0));
        REQUIRE(run_advanced_ordering(print) == RAW_PLUGIN_EXECUTION_SUCCESS);
        CHECK(tool.extrusions[0].root->children()[0].get() == second_handle);
        CHECK(tool.extrusions[0].root->children()[1].get() == first_handle);
        CHECK(tool.extrusions[0].root->first_point() == Point::new_scale(30.0, 0.0));
        CHECK(tool.extrusions[0].root->last_point() == Point::new_scale(10.0, 0.0));
        check_ordering_disabled_recursively(*tool.extrusions[0].root);
    }

    SECTION("nested sortable nodes retain every pre-existing leaf handle") {
        Print print;
        PrintingPlan &plan = print.mutable_printing_plan();
        plan.groups.emplace_back();
        PrintingToolGroup &tool = append_tool(append_layer(plan.groups.back()), 0);
        ExtrusionEntity::Children nested_children;
        nested_children.push_back(make_open_path(20.0, 0.0, 21.0, 0.0, true));
        nested_children.push_back(make_open_path(2.0, 0.0, 3.0, 0.0, true));
        ExtrusionEntityUPtr nested = make_collection(std::move(nested_children), true, true);
        const ExtrusionEntity *nested_first = nested->children()[0].get();
        const ExtrusionEntity *nested_second = nested->children()[1].get();
        ExtrusionEntity::Children root_children;
        root_children.push_back(make_open_path(40.0, 0.0, 41.0, 0.0, true));
        root_children.push_back(std::move(nested));
        append_extrusion(tool, make_collection(std::move(root_children), true, true), 4);
        publish_entry_points(tool.extrusions[0], mm_point(2.0, 0.0), mm_point(41.0, 0.0));
        REQUIRE(run_advanced_ordering(print) == RAW_PLUGIN_EXECUTION_SUCCESS);
        check_ordering_disabled_recursively(*tool.extrusions[0].root);
        Points points;
        tool.extrusions[0].root->collect_points(points);
        CHECK(points.size() == 6);
        bool first_found = false;
        bool second_found = false;
        for (const ExtrusionEntityUPtr &child : tool.extrusions[0].root->children()) {
            first_found = first_found || child.get() == nested_first;
            second_found = second_found || child.get() == nested_second;
            for (const ExtrusionEntityUPtr &grandchild : child->children()) {
                first_found = first_found || grandchild.get() == nested_first;
                second_found = second_found || grandchild.get() == nested_second;
            }
        }
        CHECK(first_found);
        CHECK(second_found);
    }
}

TEST_CASE("Advanced ordering materializes seams on straight and arc loops",
          "[plugins][ordering][advanced-extrusion-tree]")
{
    ensure_ordering_seam_provider_registered();
    ScopedActivePlugins active{k_advanced_plugin, k_seam_plugin};
    g_seam_state.strategy = SeamStrategy::FixedPoint;

    SECTION("a straight segment is split without an empty fragment") {
        g_seam_state.fixed_point = mm_point(5.0, 0.0);
        Print print;
        PrintingPlan &plan = print.mutable_printing_plan();
        plan.groups.emplace_back();
        PrintingToolGroup &tool = append_tool(append_layer(plan.groups.back()), 0);
        append_extrusion(tool, make_rectangular_loop(0.0, 10.0), 1);
        publish_entry_points(tool.extrusions[0], mm_point(0.0, 0.0), mm_point(0.0, 0.0));
        REQUIRE(run_advanced_ordering(print) == RAW_PLUGIN_EXECUTION_SUCCESS);
        const ExtrusionEntity &loop = *tool.extrusions[0].root;
        CHECK(loop.first_point() == Point::new_scale(5.0, 0.0));
        CHECK(loop.last_point() == loop.first_point());
        REQUIRE(loop.polyline_ref().size() > 2);
        for (size_t idx = 1; idx < loop.polyline_ref().size(); ++idx)
            CHECK(loop.polyline_ref().get_point(idx - 1) != loop.polyline_ref().get_point(idx));
        CHECK(g_seam_state.place_count.load(std::memory_order_relaxed) == 6);
        CHECK(published_entry_points(tool.extrusions[0]).entry.x == scale_i(5.0));
        CHECK(published_entry_points(tool.extrusions[0]).exit.x == scale_i(5.0));
    }

    SECTION("an arc split preserves arc direction, Z and direct properties") {
        const coord_t expected_seam_z = scale_i(
            0.2 * (1.0 - std::atan2(3.0, 4.0) / (0.5 * std::acos(-1.0))));
        g_seam_state.fixed_point = mm_point(13.0, 6.0);
        Print print;
        PrintingPlan &plan = print.mutable_printing_plan();
        plan.groups.emplace_back();
        PrintingToolGroup &tool = append_tool(append_layer(plan.groups.back()), 0);
        append_extrusion(tool, make_arc_loop(true), 2);
        mutable_root(tool.extrusions[0]).get_or_add(slic3r_api::EPropertyZOffset::key).set(scale_i(0.3));
        publish_entry_points(tool.extrusions[0], mm_point(15.0, 10.0), mm_point(15.0, 10.0));
        REQUIRE(run_advanced_ordering(print) == RAW_PLUGIN_EXECUTION_SUCCESS);
        const ExtrusionEntity &loop = *tool.extrusions[0].root;
        CHECK(loop.first_point() == Point::new_scale(13.0, 6.0));
        CHECK(loop.last_point() == loop.first_point());
        CHECK(loop.polyline_ref().has_arc());
        CHECK(loop.polyline_ref().has_z_offset());
        CHECK(double(loop.polyline_ref().z_offset(0)) ==
              Approx(double(expected_seam_z)).margin(1.0));
        CHECK(double(loop.polyline_ref().z_offset(loop.polyline_ref().size() - 1)) ==
              Approx(double(expected_seam_z)).margin(1.0));
        bool found_ccw_arc = false;
        for (size_t idx = 1; idx < loop.polyline_ref().size(); ++idx) {
            const Geometry::ArcWelder::Segment &segment = loop.polyline_ref().get_arc(idx);
            found_ccw_arc = found_ccw_arc ||
                (segment.radius != 0.f && segment.orientation == Geometry::ArcWelder::Orientation::CCW);
        }
        CHECK(found_ccw_arc);
        const slic3r_api::EPropertyZOffset *property =
            slic3r_api::EPropertyZOffset::key.get(mutable_root(tool.extrusions[0]));
        REQUIRE(property != nullptr);
        CHECK(property->get() == scale_i(0.3));
    }
}

TEST_CASE("Advanced ordering rotates a composed loop without replacing its children",
          "[plugins][ordering][advanced-extrusion-tree]")
{
    ensure_ordering_seam_provider_registered();
    ScopedActivePlugins active{k_advanced_plugin, k_seam_plugin};
    g_seam_state.strategy = SeamStrategy::FixedPoint;
    g_seam_state.fixed_point = mm_point(5.0, 0.0);

    Print print;
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    PrintingToolGroup &tool = append_tool(append_layer(plan.groups.back()), 0);
    ExtrusionEntityUPtr loop = make_composed_loop();
    ExtrusionEntity *split_handle = loop->children()[0].get();
    ExtrusionEntity *vertical_handle = loop->children()[1].get();
    ExtrusionEntity *diagonal_handle = loop->children()[2].get();
    append_extrusion(tool, std::move(loop), 1);
    slic3r_api::MutableExtrusionEntity split_view(
        reinterpret_cast<extrusion_entity_handle *>(split_handle));
    split_view.get_or_add(slic3r_api::EPropertyZOffset::key).set(scale_i(0.4));
    publish_entry_points(tool.extrusions[0], mm_point(0.0, 0.0), mm_point(0.0, 0.0));

    REQUIRE(run_advanced_ordering(print) == RAW_PLUGIN_EXECUTION_SUCCESS);
    const ExtrusionEntity &result = *tool.extrusions[0].root;
    REQUIRE(result.child_count() == 4);
    CHECK(result.first_point() == Point::new_scale(5.0, 0.0));
    CHECK(result.last_point() == result.first_point());
    CHECK(result.children()[0].get() == split_handle);
    CHECK(result.children()[1].get() == vertical_handle);
    CHECK(result.children()[2].get() == diagonal_handle);
    CHECK(result.child(0).first_point() == Point::new_scale(5.0, 0.0));
    CHECK(result.child(0).last_point() == Point::new_scale(10.0, 0.0));
    CHECK(result.child(3).first_point() == Point::new_scale(0.0, 0.0));
    CHECK(result.child(3).last_point() == Point::new_scale(5.0, 0.0));
    REQUIRE(slic3r_api::EPropertyZOffset::key.get(split_view) != nullptr);
    CHECK(slic3r_api::EPropertyZOffset::key.get(split_view)->get() == scale_i(0.4));
    check_ordering_disabled_recursively(result);
}

TEST_CASE("Advanced ordering validates metadata and seam services",
          "[plugins][ordering][advanced-extrusion-tree]")
{
    ensure_ordering_seam_provider_registered();

    SECTION("a non-empty extrusion requires EntryPointProperty") {
        ScopedActivePlugins active{k_advanced_plugin, k_seam_plugin};
        Print print;
        PrintingPlan &plan = print.mutable_printing_plan();
        plan.groups.emplace_back();
        PrintingToolGroup &tool = append_tool(append_layer(plan.groups.back()), 0);
        append_extrusion(tool, std::make_unique<ExtrusionEntity>(false), 0);
        append_extrusion(tool, make_open_path(0.0, 0.0, 1.0, 0.0, false), 1);
        CHECK(run_advanced_ordering(print) == RAW_PLUGIN_EXECUTION_PLUGIN_ERROR);
        CHECK(g_seam_state.initialize_count.load(std::memory_order_relaxed) == 1);
        CHECK(g_seam_state.destroy_count.load(std::memory_order_relaxed) == 1);
    }

    SECTION("an empty extrusion is ignored") {
        ScopedActivePlugins active{k_advanced_plugin, k_seam_plugin};
        Print print;
        PrintingPlan &plan = print.mutable_printing_plan();
        plan.groups.emplace_back();
        PrintingToolGroup &tool = append_tool(append_layer(plan.groups.back()), 0);
        append_extrusion(tool, std::make_unique<ExtrusionEntity>(false), 0);
        REQUIRE(run_advanced_ordering(print) == RAW_PLUGIN_EXECUTION_SUCCESS);
        CHECK(g_seam_state.initialize_count.load(std::memory_order_relaxed) == 1);
    }

    SECTION("no active provider") {
        ScopedActivePlugins active{k_advanced_plugin};
        Print print;
        CHECK(run_advanced_ordering(print) == RAW_PLUGIN_EXECUTION_PLUGIN_ERROR);
        CHECK(g_seam_state.initialize_count.load(std::memory_order_relaxed) == 0);
    }

    SECTION("provider publishes no instance") {
        ScopedActivePlugins active{k_advanced_plugin, k_seam_plugin};
        g_seam_state.mode = SeamPublishMode::Empty;
        Print print;
        CHECK(run_advanced_ordering(print) == RAW_PLUGIN_EXECUTION_PLUGIN_ERROR);
        CHECK(g_seam_state.destroy_count.load(std::memory_order_relaxed) == 0);
    }

    SECTION("provider initialization fails") {
        ScopedActivePlugins active{k_advanced_plugin, k_seam_plugin};
        g_seam_state.mode = SeamPublishMode::ThrowDuringInitialization;
        Print print;
        CHECK(run_advanced_ordering(print) == RAW_PLUGIN_EXECUTION_PLUGIN_ERROR);
        CHECK(g_seam_state.initialize_count.load(std::memory_order_relaxed) == 1);
        CHECK(g_seam_state.destroy_count.load(std::memory_order_relaxed) == 1);
    }
}
