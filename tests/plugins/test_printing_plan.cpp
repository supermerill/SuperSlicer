#include <catch2/catch.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <utility>
#include <vector>

#include "plugin_test_helpers.hpp"

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/host/Plugin.hpp"
#include "libslic3r/Api/plugin/c/slic3r_data_tree.h"
#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/c/slic3r_printing_plan.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_extrusion_edit.h"
#include "libslic3r/Api/plugin/cpp/AuxiliaryLayerHelpers.hpp"
#include "libslic3r/Api/plugin/cpp/DataTreeViews.hpp"
#include "libslic3r/Api/plugin/cpp/PrintingPlanViews.hpp"
#include "libslic3r/Config/ConfigOption.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Printing/PrintingPlan.hpp"
#include "libslic3r/Steps/StepExtrusionEdition.hpp"
#include "libslic3r/Steps/StepExtrusionOrdering.hpp"
#include "libslic3r/Steps/StepPipeline.hpp"

namespace {
using namespace Slic3r;
using namespace Slic3r::Printing;
using slic3r_api::PluginPropertyKey;

/* Small typed marker used to verify every PrintingPlan scope property view. */
struct TestPrintingScopePayload
{
    uint32_t value = 0;
};

PluginPropertyKey<TestPrintingScopePayload> printing_scope_property_key(
    orchestrator_handle *orchestrator)
{
    return PluginPropertyKey<TestPrintingScopePayload>::register_dynamic(
        orchestrator, "tests.printing-plan.scope-properties");
}

PrintingToolGroup test_tool_group(const uint16_t extruder_id, const uint16_t marker)
{
    PrintingToolGroup group;
    group.extruder_id = extruder_id;

    /*
    PrintingToolGroup has no standalone debug id. Store a default
    PrintingExtrusion with a marker in object_instance_idx so the tests can
    verify that groups with the same extruder keep their relative order after
    being moved.
    */
    PrintingExtrusion extrusion;
    extrusion.object_instance_idx = marker;
    group.extrusions.push_back(std::move(extrusion));
    return group;
}

PrintingLayerGroup test_layer(std::initializer_list<uint16_t> extruders)
{
    PrintingLayerGroup layer;
    uint16_t marker = 1;
    for (const uint16_t extruder_id : extruders)
        layer.tool_groups.push_back(test_tool_group(extruder_id, marker++));
    return layer;
}

std::vector<uint16_t> extruder_order(const PrintingLayerGroup &layer)
{
    std::vector<uint16_t> out;
    out.reserve(layer.tool_groups.size());
    for (const PrintingToolGroup &tool_group : layer.tool_groups)
        out.push_back(tool_group.extruder_id);
    return out;
}

std::vector<uint16_t> marker_order(const PrintingLayerGroup &layer)
{
    std::vector<uint16_t> out;
    out.reserve(layer.tool_groups.size());
    for (const PrintingToolGroup &tool_group : layer.tool_groups) {
        REQUIRE_FALSE(tool_group.extrusions.empty());
        out.push_back(tool_group.extrusions.front().object_instance_idx);
    }
    return out;
}

ExtrusionEntityUPtr test_path(std::initializer_list<Point> points, const bool can_reverse = true)
{
    return std::make_unique<ExtrusionEntity>(can_reverse, ArcPolyline(Points(points)));
}

ExPolygon rectangle_expolygon(const double min_x, const double min_y, const double max_x, const double max_y)
{
    return ExPolygon(Polygon({
        Point(scale_i(min_x), scale_i(min_y)),
        Point(scale_i(max_x), scale_i(min_y)),
        Point(scale_i(max_x), scale_i(max_y)),
        Point(scale_i(min_x), scale_i(max_y))
    }));
}

slic3r_api::Print print_view(Print &print)
{
    return slic3r_api::Print(reinterpret_cast<const print_handle *>(&print));
}

slic3r_api::ExPolygonCollection expolygons_view(const ExPolygons &expolygons)
{
    return slic3r_api::ExPolygonCollection(reinterpret_cast<const expolygon_collection_handle *>(&expolygons));
}

std::unique_ptr<ExtrusionEntityCollection> test_root(const bool can_sort = true, const bool can_reverse = true)
{
    return std::make_unique<ExtrusionEntityCollection>(can_sort, can_reverse);
}

class TestLoopEntryAnalysis final : public LoopEntryAnalysis
{
public:
    TestLoopEntryAnalysis(const ExtrusionEntity &loop_root,
                          const Point preferred_point,
                          const bool has_preferred_point,
                          const Point omitted_point,
                          const bool has_omitted_point)
        : m_preferred_point(preferred_point)
        , m_has_preferred_point(has_preferred_point)
    {
        loop_root.collect_points(m_points);
        if (has_omitted_point)
            m_points.erase(std::remove(m_points.begin(), m_points.end(), omitted_point), m_points.end());
    }

    size_t candidate_count() const override { return m_points.size(); }

    Point candidate_point(const size_t candidate_idx) const override
    {
        assert(candidate_idx < m_points.size());
        return candidate_idx < m_points.size() ? m_points[candidate_idx] : Point();
    }

    double score_candidate(const size_t candidate_idx, const Point &start_near) const override
    {
        assert(candidate_idx < m_points.size());
        if (candidate_idx >= m_points.size())
            return (std::numeric_limits<double>::max)();
        if (m_has_preferred_point && m_points[candidate_idx] == m_preferred_point)
            return -1.0;
        return start_near.distance_to_square(m_points[candidate_idx]);
    }

    void rotate_loop_to_candidate(ExtrusionEntity &loop_root, const size_t candidate_idx) const override
    {
        REQUIRE(candidate_idx < m_points.size());
        ArcPolyline *polyline = loop_root.polyline_or_null();
        REQUIRE(polyline != nullptr);

        size_t point_idx = size_t(-1);
        for (size_t idx = 0; idx < polyline->size(); ++idx)
            if (polyline->get_point(idx) == m_points[candidate_idx]) {
                point_idx = idx;
                break;
            }

        REQUIRE(point_idx != size_t(-1));
        if (point_idx == 0 || (point_idx == polyline->size() - 1 && polyline->front() == polyline->back()))
            return;

        ArcPolyline before_entry;
        ArcPolyline after_entry;
        REQUIRE(polyline->split_at_index(point_idx, before_entry, after_entry));
        after_entry.append(std::move(before_entry));
        polyline->swap(after_entry);
    }

private:
    Points m_points;
    Point m_preferred_point;
    bool m_has_preferred_point = false;
};

class TestLoopEntryPolicy final : public LoopEntryPolicy
{
public:
    Point preferred_point;
    bool has_preferred_point = false;
    Point omitted_point;
    bool has_omitted_point = false;

    mutable const ExtrusionEntity *seen_loop_root = nullptr;
    mutable const ExtrusionEntity *seen_context_root = nullptr;
    mutable const LayerRegionIsland *seen_region_island = nullptr;
    mutable ExtrusionRole seen_role = ExtrusionRole::None;

    std::unique_ptr<LoopEntryAnalysis> analyze_loop(const ExtrusionEntity &loop_root,
                                                    const LoopEntryContext &context) const override
    {
        seen_loop_root = &loop_root;
        seen_context_root = context.root;
        seen_region_island = context.region_island;
        seen_role = context.role;
        return std::make_unique<TestLoopEntryAnalysis>(loop_root,
                                                       preferred_point,
                                                       has_preferred_point,
                                                       omitted_point,
                                                       has_omitted_point);
    }
};

// Preserve the process-wide active plugin list while a runner test selects
// only its local probes.
class ScopedActivePlugins
{
public:
    explicit ScopedActivePlugins(std::initializer_list<const char *> plugin_ids)
        : m_orchestrator(Orchestrator::instance())
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

struct PlanEventEditorState
{
    const char *id = nullptr;
    int32_t priority = 0;
    uint32_t expected_event_count = 0;
    std::atomic_uint32_t setup_count{0};
    std::atomic_uint32_t setup_run_count{0};
    std::atomic_uint32_t run_count{0};
    std::atomic_bool payload_valid{true};
};

PlanEventEditorState g_first_plan_event_editor{"test.extrusion_edit.events.first", -10, 0};
PlanEventEditorState g_second_plan_event_editor{"test.extrusion_edit.events.second", 10, 1};

const char *plan_event_editor_id(void *ctx) { return static_cast<PlanEventEditorState *>(ctx)->id; }
const char *plan_event_editor_name(void *ctx) { return static_cast<PlanEventEditorState *>(ctx)->id; }
const char *plan_event_editor_description(void *) { return "Records global plan event scheduling."; }
const char *plan_event_editor_group(void *) { return ""; }
const char *plan_event_editor_group_label(void *) { return ""; }
const char *plan_event_editor_group_tooltip(void *) { return ""; }
slicing_step_t plan_event_editor_step(void *) { return STEP_EXTRUSION_EDIT; }
int32_t plan_event_editor_priority(void *ctx) { return static_cast<PlanEventEditorState *>(ctx)->priority; }
const_strings_t plan_event_editor_dependencies(void *) { return {}; }
int32_t plan_event_editor_used_keys(void *, raw_used_config_key *) { return 0; }
int32_t plan_event_editor_defined_keys(void *, const char **) { return 0; }
void plan_event_editor_initialize(void *, storage_handle *) {}

void plan_event_editor_setup(void *ctx, const plugin_run_context *run_ctx, uint32_t run_count)
{
    PlanEventEditorState &state = *static_cast<PlanEventEditorState *>(ctx);
    const run_ctx_extrusion_edition *payload = plugin_ctx_as_extrusion_edition(run_ctx);
    state.payload_valid = payload != nullptr && payload->print != nullptr && payload->plan != nullptr &&
                          run_count == 1;
    ++state.setup_count;
}

void plan_event_editor_setup_run(void *ctx, const plugin_run_context *run_ctx)
{
    PlanEventEditorState &state = *static_cast<PlanEventEditorState *>(ctx);
    const run_ctx_extrusion_edition *payload = plugin_ctx_as_extrusion_edition(run_ctx);
    if (payload == nullptr || payload->plan == nullptr)
        state.payload_valid = false;
    ++state.setup_run_count;
}

void plan_event_editor_run(void *ctx, const plugin_run_context *run_ctx)
{
    PlanEventEditorState &state = *static_cast<PlanEventEditorState *>(ctx);
    const run_ctx_extrusion_edition *payload = plugin_ctx_as_extrusion_edition(run_ctx);
    if (payload == nullptr || payload->plan == nullptr) {
        state.payload_valid = false;
        return;
    }

    printing_scope_events_handle *events = printing_plan_get_events_mutable(payload->plan);
    const extrusion_entity_handle *before = printing_scope_events_get_before(events);
    if (before == nullptr || extrusion_child_count(before) != state.expected_event_count)
        state.payload_valid = false;

    // Each probe appends one event. The second probe's expected count proves
    // that lower-priority plugins complete before the next plugin starts.
    ExtrusionEntity event(true);
    if (printing_scope_events_append_before_clone(
            events, reinterpret_cast<const extrusion_entity_handle *>(&event)) == nullptr)
        state.payload_valid = false;
    ++state.run_count;
}

const plugin_vtable *plan_event_editor_vtable()
{
    static const plugin_vtable table = {
        SLIC3R_PLUGIN_ABI_VERSION,
        &plan_event_editor_id,
        &plan_event_editor_name,
        &plan_event_editor_description,
        &plan_event_editor_group,
        &plan_event_editor_group_label,
        &plan_event_editor_group_tooltip,
        &plan_event_editor_step,
        &plan_event_editor_dependencies,
        &plan_event_editor_priority,
        &plan_event_editor_used_keys,
        &plan_event_editor_defined_keys,
        &plan_event_editor_initialize,
        &plan_event_editor_setup,
        &plan_event_editor_setup_run,
        &plan_event_editor_run
    };
    return &table;
}

void register_plan_event_editors()
{
    Orchestrator &orchestrator = Orchestrator::instance();
    PlanEventEditorState *states[] = {&g_first_plan_event_editor, &g_second_plan_event_editor};
    for (PlanEventEditorState *state : states) {
        if (orchestrator.get_plugin(state->id) != nullptr)
            continue;
        plugin_instance instance = {state, plan_event_editor_vtable()};
        REQUIRE(orchestrator.register_plugin(instance));
    }
}

void reset_plan_event_editor(PlanEventEditorState &state)
{
    state.setup_count = 0;
    state.setup_run_count = 0;
    state.run_count = 0;
    state.payload_valid = true;
}

} // namespace

TEST_CASE("PrintingPlan scopes carry mutable plugin properties",
          "[printing][plan][properties]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Orchestrator &orchestrator = Orchestrator::instance();
    orchestrator_handle *orchestrator_handle_value =
        reinterpret_cast<orchestrator_handle *>(&orchestrator);
    const PluginPropertyKey<TestPrintingScopePayload> property_key =
        printing_scope_property_key(orchestrator_handle_value);
    PrintingPlan native_plan;
    printing_plan_handle *plan_handle = reinterpret_cast<printing_plan_handle *>(&native_plan);
    slic3r_api::PrintingPlan plan(plan_handle);

    slic3r_api::PrintingGroup group = plan.append_group();
    slic3r_api::PrintingLayerGroup layer = group.append_layer_group(42);
    slic3r_api::PrintingToolGroup tool = layer.append_tool_group(3);

    // Each view writes only its own auxiliary container. The C getters expose
    // those same borrowed containers, so C and C++ plugins observe one state.
    property_key.get_or_add(plan.properties()).value = 10;
    property_key.get_or_add(group.properties()).value = 20;
    property_key.get_or_add(layer.properties()).value = 30;
    property_key.get_or_add(tool.properties()).value = 40;

    REQUIRE(printing_plan_get_properties(plan_handle) != nullptr);
    REQUIRE(printing_group_get_properties(group.handle()) != nullptr);
    REQUIRE(printing_layer_group_get_properties(layer.handle()) != nullptr);
    REQUIRE(printing_tool_group_get_properties(tool.handle()) != nullptr);
    REQUIRE(property_key.get(plan.properties()) != nullptr);
    REQUIRE(property_key.get(group.properties()) != nullptr);
    REQUIRE(property_key.get(layer.properties()) != nullptr);
    REQUIRE(property_key.get(tool.properties()) != nullptr);
    CHECK(property_key.get(plan.properties())->value == 10);
    CHECK(property_key.get(group.properties())->value == 20);
    CHECK(property_key.get(layer.properties())->value == 30);
    CHECK(property_key.get(tool.properties())->value == 40);

    // Reordering moves complete scope values. Reacquire every child view after
    // moving its parent because vector operations invalidate borrowed handles.
    plan.append_group();
    REQUIRE(plan.move_group(0, 1));
    group = plan.group(1);
    REQUIRE(property_key.get(group.properties()) != nullptr);
    CHECK(property_key.get(group.properties())->value == 20);

    group.append_layer_group(84);
    REQUIRE(group.move_layer_group(0, 1));
    layer = group.layer_group(1);
    REQUIRE(property_key.get(layer.properties()) != nullptr);
    CHECK(property_key.get(layer.properties())->value == 30);

    layer.append_tool_group(7);
    REQUIRE(layer.move_tool_group(0, 1));
    tool = layer.tool_group(1);
    REQUIRE(property_key.get(tool.properties()) != nullptr);
    CHECK(property_key.get(tool.properties())->value == 40);

    // Metadata is meaningful content: cleanup may remove the other empty tool
    // group, but it must not silently discard the annotated one.
    CHECK_FALSE(layer.remove_empty_tool_group(1));
    CHECK(layer.remove_empty_tool_group(0));
    CHECK(layer.tool_group_count() == 1);

    // Clearing a group resets its own metadata and all descendants. Clearing
    // the plan then resets plan-wide metadata as well.
    group.clear();
    CHECK(group.properties().count() == 0);
    CHECK(group.layer_group_count() == 0);
    CHECK(plan.properties().count() == 1);

    plan.clear();
    CHECK(plan.properties().count() == 0);
    CHECK(plan.group_count() == 0);
}

TEST_CASE("PrintingPlan tool ordering keeps one-extruder layers stable", "[printing][plan]")
{
    // A layer using only one extruder cannot reduce tool changes by moving its
    // groups. The function may still rebuild the vector internally, but it must
    // keep duplicate tool groups in their original relative order.
    PrintingGroup group;
    group.layers.push_back(test_layer({2, 2}));

    order_printing_tool_groups(group);

    CHECK(extruder_order(group.layers.front()) == std::vector<uint16_t>({2, 2}));
    CHECK(marker_order(group.layers.front()) == std::vector<uint16_t>({1, 2}));
}

TEST_CASE("PrintingPlan tool ordering starts with requested first extruder", "[printing][plan]")
{
    // When the caller knows the active tool before this PrintingGroup, the first
    // printable layer should start with that tool if it is available. The rest
    // of the layer stays as close as possible to the original order.
    PrintingGroup group;
    group.layers.push_back(test_layer({2, 1, 3}));

    order_printing_tool_groups(group, 1);

    CHECK(extruder_order(group.layers.front()) == std::vector<uint16_t>({1, 2, 3}));
}

TEST_CASE("PrintingPlan tool ordering uses smallest extruder as default start", "[printing][plan]")
{
    // If no first tool is known, the smallest extruder id is only a deterministic
    // tie-break. This prevents the first layer from depending on incidental
    // source traversal order when several orders have the same tool-change cost.
    PrintingGroup group;
    group.layers.push_back(test_layer({2, 3, 1}));

    order_printing_tool_groups(group, uint16_t(-1));

    CHECK(extruder_order(group.layers.front()) == std::vector<uint16_t>({1, 2, 3}));
}

TEST_CASE("PrintingPlan tool ordering connects consecutive layers through shared tools", "[printing][plan]")
{
    // The last tool of one layer is chosen with the next layer in mind. Here the
    // first layer can end with extruder 2, allowing the second layer to start
    // with extruder 2 and avoid one tool change.
    PrintingGroup group;
    group.layers.push_back(test_layer({1, 2}));
    group.layers.push_back(test_layer({2, 3}));

    order_printing_tool_groups(group, uint16_t(-1));

    CHECK(extruder_order(group.layers[0]) == std::vector<uint16_t>({1, 2}));
    CHECK(extruder_order(group.layers[1]) == std::vector<uint16_t>({2, 3}));
}

TEST_CASE("PrintingPlan tool ordering minimizes transitions over several layers", "[printing][plan]")
{
    // This case needs a global choice, not a local per-layer shuffle. The best
    // sequence is 1->2, 2->3, then 3->1, which gives zero inter-layer tool
    // changes across all three printable layers.
    PrintingGroup group;
    group.layers.push_back(test_layer({1, 2}));
    group.layers.push_back(test_layer({2, 3}));
    group.layers.push_back(test_layer({1, 3}));

    order_printing_tool_groups(group, uint16_t(-1));

    CHECK(extruder_order(group.layers[0]) == std::vector<uint16_t>({1, 2}));
    CHECK(extruder_order(group.layers[1]) == std::vector<uint16_t>({2, 3}));
    CHECK(extruder_order(group.layers[2]) == std::vector<uint16_t>({3, 1}));
}

TEST_CASE("PrintingPlan tool ordering preserves duplicate tool group order", "[printing][plan]")
{
    // A layer may contain several groups with the same extruder. Ordering may
    // move all groups of an extruder together, but it must not reverse or sort
    // the groups inside that extruder bucket because they may already encode a
    // meaningful upstream sequence.
    PrintingGroup group;
    group.layers.push_back(test_layer({2, 1, 2, 3, 1}));

    order_printing_tool_groups(group, 1);

    CHECK(extruder_order(group.layers.front()) == std::vector<uint16_t>({1, 1, 2, 2, 3}));
    CHECK(marker_order(group.layers.front()) == std::vector<uint16_t>({2, 5, 1, 3, 4}));
}

TEST_CASE("PrintingPlan tool ordering skips empty layers in transition search", "[printing][plan]")
{
    // Empty layer groups are placeholders, not tool-change events. They should
    // remain empty and should not prevent the next printable layer from using
    // the requested first extruder.
    PrintingGroup group;
    group.layers.emplace_back();
    group.layers.push_back(test_layer({2, 1}));

    order_printing_tool_groups(group, 1);

    CHECK(group.layers.front().tool_groups.empty());
    CHECK(extruder_order(group.layers[1]) == std::vector<uint16_t>({1, 2}));
}

TEST_CASE("PrintingPlan extrusion ordering leaves non-sortable nodes unchanged", "[printing][plan]")
{
    // A non-sortable collection is an upstream contract: its child order may
    // encode a continuous multipath or a required process order. Ordering must
    // therefore treat it as one atomic sequence and leave its children alone.
    std::unique_ptr<ExtrusionEntityCollection> root = test_root(false, false);
    root->append(test_path({Point(100, 0), Point(110, 0)}));
    root->append(test_path({Point(0, 0), Point(10, 0)}));

    order_extrusion_tree(*root, Point(0, 0));

    CHECK(root->child(0).first_point() == Point(100, 0));
    CHECK(root->child(1).first_point() == Point(0, 0));
    CHECK_FALSE(root->can_sort());
}

TEST_CASE("PrintingPlan extrusion ordering chooses nearest sortable child", "[printing][plan]")
{
    // Sortable children are selected by their closest legal entry point. Once a
    // concrete order is chosen, the parent becomes non-sortable so later code
    // sees a fixed print sequence instead of an optimization request.
    std::unique_ptr<ExtrusionEntityCollection> root = test_root();
    root->append(test_path({Point(100, 0), Point(110, 0)}));
    root->append(test_path({Point(5, 0), Point(15, 0)}));

    order_extrusion_tree(*root, Point(0, 0));

    CHECK(root->child(0).first_point() == Point(5, 0));
    CHECK(root->child(1).first_point() == Point(100, 0));
    CHECK_FALSE(root->can_sort());
    CHECK_FALSE(root->can_reverse());
}

TEST_CASE("PrintingPlan extrusion ordering reverses only reversible open paths", "[printing][plan]")
{
    // A reversible child may be flipped when its end is closer to the current
    // position. A non-reversible child must keep its original direction even if
    // that forces a longer travel.
    std::unique_ptr<ExtrusionEntityCollection> reversible_root = test_root();
    reversible_root->append(test_path({Point(100, 0), Point(10, 0)}, true));

    order_extrusion_tree(*reversible_root, Point(0, 0));

    CHECK(reversible_root->child(0).first_point() == Point(10, 0));
    CHECK(reversible_root->child(0).last_point() == Point(100, 0));

    std::unique_ptr<ExtrusionEntityCollection> fixed_root = test_root();
    fixed_root->append(test_path({Point(100, 0), Point(10, 0)}, false));

    order_extrusion_tree(*fixed_root, Point(0, 0));

    CHECK(fixed_root->child(0).first_point() == Point(100, 0));
    CHECK(fixed_root->child(0).last_point() == Point(10, 0));
}

TEST_CASE("PrintingPlan extrusion ordering rotates leaf loops to nearest vertex", "[printing][plan]")
{
    // Loops are not reversed, but they may be rotated. The selected vertex
    // becomes both the first and last point, preserving the closed geometry
    // while avoiding a needless travel to the old arbitrary start point.
    std::unique_ptr<ExtrusionEntityCollection> root = test_root();
    root->append(test_path({Point(0, 0), Point(100, 0), Point(100, 100), Point(0, 0)}));

    order_extrusion_tree(*root, Point(100, 95));

    CHECK(root->child(0).is_loop());
    CHECK(root->child(0).first_point() == Point(100, 100));
    CHECK(root->child(0).last_point() == Point(100, 100));
}

TEST_CASE("PrintingPlan extrusion ordering lets a policy prefer a farther loop point", "[printing][plan]")
{
    // The loop policy owns the loop-entry decision. This policy gives one
    // distant vertex the best score, so ordering must use it even though the
    // default distance-only policy would start at Point(0, 0).
    TestLoopEntryPolicy policy;
    policy.preferred_point = Point(100, 100);
    policy.has_preferred_point = true;

    std::unique_ptr<ExtrusionEntityCollection> root = test_root();
    root->append(test_path({Point(0, 0), Point(100, 0), Point(100, 100), Point(0, 0)}));

    LoopEntryContext context;
    order_extrusion_tree(*root, Point(0, 0), policy, context);

    CHECK(root->child(0).is_loop());
    CHECK(root->child(0).first_point() == Point(100, 100));
    CHECK(policy.seen_context_root == root.get());
}

TEST_CASE("PrintingPlan extrusion ordering lets a policy remove a loop candidate", "[printing][plan]")
{
    // A future plugin may forbid a seam candidate because of painting, angle or
    // extrusion properties. This test models that by omitting the normally best
    // vertex; the next best available vertex must be selected instead.
    TestLoopEntryPolicy policy;
    policy.omitted_point = Point(0, 0);
    policy.has_omitted_point = true;

    std::unique_ptr<ExtrusionEntityCollection> root = test_root();
    root->append(test_path({Point(0, 0), Point(100, 0), Point(100, 100), Point(0, 0)}));

    LoopEntryContext context;
    order_extrusion_tree(*root, Point(0, 0), policy, context);

    CHECK(root->child(0).is_loop());
    CHECK(root->child(0).first_point() == Point(100, 0));
}

TEST_CASE("PrintingPlan extrusion ordering rotates composed loops without flattening", "[printing][plan]")
{
    // A loop may be represented as a non-sortable collection of several open
    // leaf paths. When the best entry point lies inside one leaf, ordering
    // splits only that leaf and rotates the child list; it must not flatten the
    // collection or lose the loop continuity.
    std::unique_ptr<ExtrusionEntityCollection> loop = test_root(false, false);
    loop->append(test_path({Point(0, 0), Point(100, 0)}));
    loop->append(test_path({Point(100, 0), Point(100, 100), Point(0, 0)}));
    REQUIRE(loop->is_loop());

    std::unique_ptr<ExtrusionEntityCollection> root = test_root();
    root->append(std::move(loop));

    order_extrusion_tree(*root, Point(100, 95));

    REQUIRE(root->child_count() == 1);
    const ExtrusionEntity &rotated_loop = root->child(0);
    CHECK(rotated_loop.is_loop());
    CHECK(rotated_loop.first_point() == Point(100, 100));
    CHECK(rotated_loop.last_point() == Point(100, 100));
    CHECK(rotated_loop.child_count() == 3);
}

TEST_CASE("PrintingPlan extrusion ordering preserves intermediate node properties", "[printing][plan]")
{
    // Ordering moves owned subtrees; it must not clone children into a flatter
    // shape or drop properties carried by intermediate collection nodes. Those
    // properties are how later processing stages inherit print state.
    std::unique_ptr<ExtrusionEntityCollection> nested = test_root(false, false);
    nested->add_property(ExtrusionPropertySpeed(42.f));
    nested->append(test_path({Point(20, 0), Point(30, 0)}));

    std::unique_ptr<ExtrusionEntityCollection> root = test_root();
    root->append(test_path({Point(100, 0), Point(110, 0)}));
    root->append(std::move(nested));

    order_extrusion_tree(*root, Point(0, 0));

    REQUIRE(root->child_count() == 2);
    CHECK(root->child(0).get_property<ExtrusionPropertySpeed>() != nullptr);
    CHECK(root->child(0).first_point() == Point(20, 0));
}

TEST_CASE("PrintingPlan group extrusion ordering propagates current position", "[printing][plan]")
{
    // The group overload keeps the higher-level print order unchanged, but the
    // end of one root becomes the start hint for the next root. This lets the
    // second root prefer work near the first root's final point.
    PrintingGroup group;
    group.layers.emplace_back();
    group.layers.front().tool_groups.push_back(test_tool_group(1, 1));
    PrintingToolGroup &tool_group = group.layers.front().tool_groups.front();
    tool_group.extrusions.clear();

    PrintingExtrusion first_extrusion;
    first_extrusion.root = test_root();
    static_cast<ExtrusionEntityCollection *>(first_extrusion.root.get())->append(
        test_path({Point(0, 0), Point(100, 0)}, false));
    tool_group.extrusions.push_back(std::move(first_extrusion));

    PrintingExtrusion second_extrusion;
    second_extrusion.root = test_root();
    static_cast<ExtrusionEntityCollection *>(second_extrusion.root.get())->append(
        test_path({Point(1, 0), Point(2, 0)}, false));
    static_cast<ExtrusionEntityCollection *>(second_extrusion.root.get())->append(
        test_path({Point(101, 0), Point(102, 0)}, false));
    tool_group.extrusions.push_back(std::move(second_extrusion));

    order_extrusion_tree(group, Point(0, 0));

    REQUIRE(tool_group.extrusions.size() == 2);
    REQUIRE(tool_group.extrusions[1].root != nullptr);
    CHECK(tool_group.extrusions[1].root->child(0).first_point() == Point(101, 0));
}

TEST_CASE("PrintingPlan group extrusion ordering passes source context to loop policy", "[printing][plan]")
{
    // The group overload is the bridge between a PrintingPlan and loop-entry
    // scoring. It must pass the source LayerRegionIsland and role bucket so a
    // later plugin-backed policy can read region settings or source properties.
    const LayerRegionIsland *region_island =
        reinterpret_cast<const LayerRegionIsland *>(uintptr_t(0x1234));
    TestLoopEntryPolicy policy;

    PrintingGroup group;
    group.layers.emplace_back();
    group.layers.front().tool_groups.push_back(test_tool_group(1, 1));
    PrintingToolGroup &tool_group = group.layers.front().tool_groups.front();
    tool_group.extrusions.clear();

    PrintingExtrusion extrusion;
    extrusion.region_island = region_island;
    extrusion.sregion_island_role = ExtrusionRole::Perimeter;
    extrusion.root = test_root();
    static_cast<ExtrusionEntityCollection *>(extrusion.root.get())->append(
        test_path({Point(0, 0), Point(100, 0), Point(100, 100), Point(0, 0)}));
    tool_group.extrusions.push_back(std::move(extrusion));

    order_extrusion_tree(group, Point(10, 0), policy);

    CHECK(policy.seen_region_island == region_island);
    CHECK(policy.seen_role == ExtrusionRole::Perimeter);
    REQUIRE_FALSE(tool_group.extrusions.empty());
    CHECK(policy.seen_context_root == tool_group.extrusions.front().root.get());
    CHECK(policy.seen_loop_root == &tool_group.extrusions.front().root->child(0));
}

TEST_CASE("PrintingPlan C API builds and mutates plan objects", "[printing][plan][api]")
{
    // Ordering plugins see only opaque C handles. This test exercises that ABI
    // directly on a local PrintingPlan so a future external plugin can rely on
    // the same append/read/move primitives without including native plan types.
    PrintingPlan plan;
    printing_plan_handle *plan_handle = reinterpret_cast<printing_plan_handle *>(&plan);
    printing_group_handle *group_handle = printing_plan_append_group(plan_handle);
    REQUIRE(group_handle != nullptr);

    printing_group_append_object_instance(group_handle, reinterpret_cast<const object_handle *>(uintptr_t(0x1234)), 2);
    CHECK(printing_group_count_object_instance(group_handle) == 1);
    CHECK(printing_group_get_object_instance(group_handle, 0).instance_idx == 2);

    printing_layer_group_handle *layer_handle = printing_group_append_layer_group(group_handle, 42);
    REQUIRE(layer_handle != nullptr);
    CHECK(printing_layer_group_get_print_z(layer_handle) == 42);

    printing_tool_group_handle *tool_handle = printing_layer_group_append_tool_group(layer_handle, 3);
    REQUIRE(tool_handle != nullptr);
    CHECK(printing_tool_group_get_extruder_id(tool_handle) == 3);

    const layer_region_island_handle *fake_region_island =
        reinterpret_cast<const layer_region_island_handle *>(uintptr_t(0x2345));
    printing_tool_group_append_region_island(tool_handle, fake_region_island);
    CHECK(printing_tool_group_get_region_island(tool_handle, 0) == fake_region_island);

    ExtrusionEntity source(true);
    source.append_child(test_path({Point(0, 0), Point(10, 0)}));
    const extrusion_entity_handle *source_handle = reinterpret_cast<const extrusion_entity_handle *>(&source);
    printing_extrusion_handle *extrusion_handle = printing_tool_group_append_extrusion_clone(
        tool_handle,
        fake_region_island,
        RAW_EXTRUSION_ROLE_PERIMETER,
        source_handle,
        7);
    REQUIRE(extrusion_handle != nullptr);

    CHECK(printing_tool_group_count_extrusion(tool_handle) == 1);
    CHECK(printing_extrusion_get_region_island(extrusion_handle) == fake_region_island);
    CHECK(printing_extrusion_get_role(extrusion_handle) == RAW_EXTRUSION_ROLE_PERIMETER);
    CHECK(printing_extrusion_get_object_instance_idx(extrusion_handle) == 7);

    extrusion_entity_handle *clone_root = printing_extrusion_get_root_mutable(extrusion_handle);
    REQUIRE(clone_root != nullptr);
    CHECK(clone_root != source_handle);
    CHECK(extrusion_child_count(clone_root) == 1);
    CHECK(source.child_count() == 1);
}

TEST_CASE("PrintingPlan C API moves extrusion content into the plan", "[printing][plan][api]")
{
    // The move API transfers the content of a plugin-owned extrusion handle
    // into the plan clone. The handle itself is still owned by plugin storage,
    // so the host leaves it valid but empty after the move.
    PrintingPlan plan;
    printing_plan_handle *plan_handle = reinterpret_cast<printing_plan_handle *>(&plan);
    printing_group_handle *group_handle = printing_plan_append_group(plan_handle);
    printing_layer_group_handle *layer_handle = printing_group_append_layer_group(group_handle, 0);
    printing_tool_group_handle *tool_handle = printing_layer_group_append_tool_group(layer_handle, 0);

    ExtrusionEntity source(true);
    source.append_child(test_path({Point(0, 0), Point(10, 0)}));
    extrusion_entity_handle *source_handle = reinterpret_cast<extrusion_entity_handle *>(&source);

    printing_extrusion_handle *extrusion_handle = printing_tool_group_append_extrusion_move(
        tool_handle,
        nullptr,
        RAW_EXTRUSION_ROLE_INFILL,
        source_handle,
        0);
    REQUIRE(extrusion_handle != nullptr);

    CHECK(source.empty());
    REQUIRE(printing_extrusion_get_root(extrusion_handle) != nullptr);
    CHECK(extrusion_child_count(printing_extrusion_get_root(extrusion_handle)) == 1);
}

TEST_CASE("PrintingPlan C API transfers extrusions between tool groups",
          "[printing][plan][api][plugins][ordering]")
{
    /* A transfer updates both cached region-island lists before the source
       group is removed. This is the mutation sequence used by ordering plugins
       when a marker changes the tool assigned to an object extrusion. */
    PrintingPlan plan;
    printing_plan_handle *plan_handle = reinterpret_cast<printing_plan_handle *>(&plan);
    printing_group_handle *group_handle = printing_plan_append_group(plan_handle);
    printing_layer_group_handle *layer_handle = printing_group_append_layer_group(group_handle, 0);
    REQUIRE(printing_layer_group_append_tool_group(layer_handle, 0) != nullptr);
    REQUIRE(printing_layer_group_append_tool_group(layer_handle, 1) != nullptr);
    // Appending to the vector may move its elements, so acquire stable handles
    // only after the layer contains both tool groups used by the transfer.
    printing_tool_group_handle *source = printing_layer_group_get_tool_group_mutable(layer_handle, 0);
    printing_tool_group_handle *destination = printing_layer_group_get_tool_group_mutable(layer_handle, 1);
    const layer_region_island_handle *region_island =
        reinterpret_cast<const layer_region_island_handle *>(uintptr_t(0x3456));

    ExtrusionEntity root(true);
    root.append_child(test_path({Point(0, 0), Point(10, 0)}));
    REQUIRE(printing_tool_group_append_extrusion_clone(
                source, region_island, RAW_EXTRUSION_ROLE_PERIMETER,
                reinterpret_cast<const extrusion_entity_handle *>(&root), 0) != nullptr);
    REQUIRE(printing_tool_group_transfer_extrusion(source, 0, destination) == 1);

    CHECK(printing_tool_group_count_extrusion(source) == 0);
    CHECK(printing_tool_group_count_region_island(source) == 0);
    CHECK(printing_tool_group_count_extrusion(destination) == 1);
    CHECK(printing_tool_group_count_region_island(destination) == 1);
    CHECK(printing_tool_group_get_region_island(destination, 0) == region_island);
    CHECK(printing_layer_group_remove_empty_tool_group(layer_handle, 0) == 1);
    CHECK(printing_layer_group_count_tool_group(layer_handle) == 1);
    CHECK(printing_tool_group_get_extruder_id(
              printing_layer_group_get_tool_group(layer_handle, 0)) == 1);
}

TEST_CASE("PrintingPlan scopes expose fixed ordered event roots", "[printing][plan][events][api]")
{
    PrintingPlan plan;
    printing_plan_handle *plan_handle = reinterpret_cast<printing_plan_handle *>(&plan);
    printing_group_handle *group_handle = printing_plan_append_group(plan_handle);
    printing_layer_group_handle *layer_handle = printing_group_append_layer_group(group_handle, 42);
    printing_tool_group_handle *tool_handle = printing_layer_group_append_tool_group(layer_handle, 3);

    const printing_scope_events_handle *scope_events[] = {
        printing_plan_get_events(plan_handle),
        printing_group_get_events(group_handle),
        printing_layer_group_get_events(layer_handle),
        printing_tool_group_get_events(tool_handle)
    };
    for (const printing_scope_events_handle *events : scope_events) {
        REQUIRE(events != nullptr);
        CHECK_FALSE(printing_scope_events_has_before(events));
        CHECK_FALSE(printing_scope_events_has_after(events));

        const extrusion_entity_handle *before = printing_scope_events_get_before(events);
        const extrusion_entity_handle *after = printing_scope_events_get_after(events);
        REQUIRE(before != nullptr);
        REQUIRE(after != nullptr);
        CHECK(extrusion_has_children(before));
        CHECK(extrusion_has_children(after));
        CHECK(extrusion_child_count(before) == 0);
        CHECK(extrusion_child_count(after) == 0);
        CHECK(extrusion_flags(before) == 0);
        CHECK(extrusion_flags(after) == 0);
    }

    ExtrusionEntity cloned_event(true);
    cloned_event.append_child(test_path({Point(0, 0), Point(10, 0)}));
    const extrusion_entity_handle *cloned_event_handle =
        reinterpret_cast<const extrusion_entity_handle *>(&cloned_event);
    printing_scope_events_handle *plan_events = printing_plan_get_events_mutable(plan_handle);
    extrusion_entity_handle *appended_event =
        printing_scope_events_append_before_clone(plan_events, cloned_event_handle);
    REQUIRE(appended_event != nullptr);
    CHECK(printing_scope_events_has_before(plan_events));
    CHECK(extrusion_child_count(printing_scope_events_get_before(plan_events)) == 1);
    CHECK(extrusion_child_count(appended_event) == 1);
    CHECK(cloned_event.child_count() == 1);

    ExtrusionEntity moved_event(true);
    moved_event.append_child(test_path({Point(20, 0), Point(30, 0)}));
    extrusion_entity_handle *moved_event_handle = reinterpret_cast<extrusion_entity_handle *>(&moved_event);
    slic3r_api::PrintingPlan plan_view(plan_handle);
    slic3r_api::PrintingScopeEvents group_events = plan_view.group(0).events();
    group_events.append_after_move(slic3r_api::MutableExtrusionEntity(moved_event_handle));
    CHECK(group_events.has_after());
    CHECK(group_events.after().child_count() == 1);
    CHECK(moved_event.empty());

    // Event sequences belong to their scope value, so vector reordering moves
    // the sequence together with the group instead of leaving it at an index.
    printing_group_handle *second_group = printing_plan_append_group(plan_handle);
    ExtrusionEntity second_group_event(true);
    REQUIRE(printing_scope_events_append_before_clone(
                printing_group_get_events_mutable(second_group),
                reinterpret_cast<const extrusion_entity_handle *>(&second_group_event)) != nullptr);
    REQUIRE(printing_plan_move_group(plan_handle, 1, 0));
    CHECK(printing_scope_events_has_before(
        printing_group_get_events(printing_plan_get_group(plan_handle, 0))));

    printing_plan_clear(plan_handle);
    CHECK(printing_plan_count_group(plan_handle) == 0);
    CHECK_FALSE(printing_scope_events_has_before(printing_plan_get_events(plan_handle)));
    CHECK(extrusion_flags(printing_scope_events_get_before(printing_plan_get_events(plan_handle))) == 0);
}

TEST_CASE("STEP_EXTRUSION_EDIT rebuilds scope events in plugin priority order",
          "[plugins][extrusion-edit][printing][plan][events]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    register_plan_event_editors();
    reset_plan_event_editor(g_first_plan_event_editor);
    reset_plan_event_editor(g_second_plan_event_editor);

    Print print;
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    plan.groups.front().layers.emplace_back();
    plan.groups.front().layers.front().tool_groups.emplace_back();

    // Seed every scope with stale data. clean_and_prepare() must remove these
    // events without changing the surrounding ordered plan hierarchy.
    ExtrusionEntity stale_event(true);
    plan.events.append_before(stale_event);
    plan.groups.front().events.append_after(stale_event);
    plan.groups.front().layers.front().events.append_before(stale_event);
    plan.groups.front().layers.front().tool_groups.front().events.append_after(stale_event);

    Steps::StepExtrusionEdition::clean_and_prepare(print);
    CHECK_FALSE(plan.events.has_before());
    CHECK_FALSE(plan.groups.front().events.has_after());
    CHECK_FALSE(plan.groups.front().layers.front().events.has_before());
    CHECK_FALSE(plan.groups.front().layers.front().tool_groups.front().events.has_after());

    ScopedActivePlugins active({g_second_plan_event_editor.id, g_first_plan_event_editor.id});
    Steps::StepExtrusionEdition::run_step(Orchestrator::instance(), print);

    CHECK(g_first_plan_event_editor.payload_valid.load());
    CHECK(g_second_plan_event_editor.payload_valid.load());
    CHECK(g_first_plan_event_editor.setup_count.load() == 1);
    CHECK(g_second_plan_event_editor.setup_count.load() == 1);
    CHECK(g_first_plan_event_editor.setup_run_count.load() == 1);
    CHECK(g_second_plan_event_editor.setup_run_count.load() == 1);
    CHECK(g_first_plan_event_editor.run_count.load() == 1);
    CHECK(g_second_plan_event_editor.run_count.load() == 1);
    CHECK(plan.events.before().child_count() == 2);

    // A repeated pipeline execution rebuilds the same ordered sequences. It
    // must not retain the two events produced by the previous execution.
    Steps::StepExtrusionEdition::clean_and_prepare(print);
    Steps::StepExtrusionEdition::run_step(Orchestrator::instance(), print);
    CHECK(g_first_plan_event_editor.run_count.load() == 2);
    CHECK(g_second_plan_event_editor.run_count.load() == 2);
    CHECK(plan.events.before().child_count() == 2);
}

TEST_CASE("STEP_ORDERING runs default plugin chain on a shared PrintingPlan", "[printing][plan][step-ordering]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Orchestrator &orchestrator = Orchestrator::instance();
    Print print;
    Steps::StepExtrusionOrdering::clean_and_prepare(print);
    REQUIRE(print.printing_plan() == nullptr);

    const std::vector<Plugin *> plugins =
        Steps::selected_or_active_plugins_for_step(orchestrator, STEP_ORDERING, &print.full_print_config());
    REQUIRE(plugins.size() >= 6);
    CHECK(plugins[0]->get_id() == "ordering.plan_builder.default");
    CHECK(plugins[1]->get_id() == "ordering.custom_gcode_tool_overrides");
    CHECK(plugins[2]->get_id() == "ordering.tool_groups.default");
    CHECK(plugins[3]->get_id() == "ordering.custom_gcode_event_tools");
    CHECK(plugins[4]->get_id() == "ordering.printing_extrusion.presort.default");
    CHECK(plugins[5]->get_id() == "ordering.extrusion_tree.default");
    CHECK(Steps::get_exclusive_steps().find(STEP_ORDERING) == Steps::get_exclusive_steps().end());

    Steps::StepExtrusionOrdering::run_step(orchestrator, print);

    REQUIRE(print.printing_plan() != nullptr);
    CHECK(print.printing_plan()->groups.size() == 1);
}

TEST_CASE("STEP_ORDERING includes print-level auxiliary layers", "[printing][plan][step-ordering]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    CHECK(print_count_object(reinterpret_cast<const print_handle *>(&print)) == 0);

    PluginStorage plugin_storage;
    storage_handle *storage = reinterpret_cast<storage_handle *>(&plugin_storage);
    const ExPolygons subject{ rectangle_expolygon(20., 20., 40., 40.) };

    /*
    Build a print-level auxiliary layer without adding any real PrintObject.
    The default plan builder must still see this layer through the explicit
    print auxiliary object API.
    */
    slic3r_api::AuxiliaryLayerBuildResult result =
        slic3r_api::build_auxiliary_layer_regions_from_subject(
            storage,
            print_view(print),
            print_view(print).auxiliary_object(),
            expolygons_view(subject),
            scale_i(1.),
            scale_i(1.),
            scale_i(0.5));
    REQUIRE(result.created);

    slic3r_api::Layer layer = result.layer;
    REQUIRE(layer.island_count() == 1);
    layer_region_island_handle *region_island_handle =
        layer_island_get_or_create_region_island(
            const_cast<layer_island_handle *>(layer.island(0).handle()),
            nullptr,
            0,
            0);
    REQUIRE(region_island_handle != nullptr);

    extrusion_entity_handle *extrusion_handle =
        layer_region_island_get_mutable_extrusion(region_island_handle, RAW_EXTRUSION_ROLE_PERIMETER);
    REQUIRE(extrusion_handle != nullptr);
    ExtrusionEntityCollection *extrusion_root =
        reinterpret_cast<ExtrusionEntityCollection *>(extrusion_handle);
    extrusion_root->append(test_path({Point(scale_i(20.), scale_i(20.)),
                                      Point(scale_i(40.), scale_i(20.))}));

    Orchestrator &orchestrator = Orchestrator::instance();
    Steps::StepExtrusionOrdering::clean_and_prepare(print);
    Steps::StepExtrusionOrdering::run_step(orchestrator, print);

    REQUIRE(print.printing_plan() != nullptr);
    REQUIRE(print.printing_plan()->groups.size() == 1);
    const PrintingGroup &group = print.printing_plan()->groups.front();
    REQUIRE(group.object_instances.size() == 1);
    CHECK(group.object_instances.front().object == print.auxiliary_object());
    REQUIRE(group.layers.size() == 1);
    REQUIRE(group.layers.front().tool_groups.size() == 1);
    REQUIRE(group.layers.front().tool_groups.front().extrusions.size() == 1);
    CHECK(group.layers.front().tool_groups.front().extrusions.front().object_instance_idx == 0);
}
