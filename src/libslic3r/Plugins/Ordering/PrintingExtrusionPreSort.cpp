///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "PrintingExtrusionPreSort.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <vector>

#include "KDTreeOrderingEngine.hpp"

#include "libslic3r/Api/plugin/c/steps/slic3r_step_ordering.h"
#include "libslic3r/Api/plugin/cpp/DataTreeViews.hpp"
#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"
#include "libslic3r/Api/plugin/cpp/PrintingPlanViews.hpp"
#include "libslic3r/Plugins/PrintingPlan/EntryPointProperty.h"

namespace slic3r_api { namespace Ordering { namespace DefaultOrderingPlugin {
namespace {

/*
Coarse PrintingExtrusion ordering
=================================

The trees entering this provider may still contain sortable children and loops
without a final seam. Their exact entry and exit are therefore unknown. This
provider uses four real extreme vertices as inexpensive approximations, orders
the independent PrintingExtrusion objects of each tool visit, and publishes the
selected approximation through EntryPointProperty.

Only PrintingToolGroup::extrusions changes order. The provider walks groups,
layers and tool visits sequentially so the selected exit of one vector can seed
the next vector without moving any object across an ownership boundary.
*/

const char *k_no_dependencies[] = { nullptr };
const char *k_exclusive_group = "ordering.printing_extrusion.presort";

struct ExtremePoints
{
    c_point left = {};
    c_point right = {};
    c_point bottom = {};
    c_point top = {};
    bool valid = false;
};

/* Add all local vertices in one tree to the four deterministic extrema. */
void collect_extreme_points(const ExtrusionEntity &entity, ExtremePoints &extremes);

/* Compare one vertex with the extrema accumulated so far. */
void include_extreme_point(c_point point, ExtremePoints &extremes);

/* Build the distinct real entry/exit estimates for one non-empty root. */
OrderingItem make_ordering_item(const ExtrusionEntity &root);

/* Append one candidate unless the same point is already present. */
void append_distinct_candidate(OrderingItem &item, c_point point);

/*
Order one extrusion vector and return the last selected exit.

Empty roots keep their positions relative to the vector and do not participate
in route costs. Their stale EntryPointProperty is removed because they no
longer describe printable geometry.
*/
c_point order_tool_group(const PrintingToolGroup &tool_group,
                         c_point initial_position,
                         const OrderingEngine &engine,
                         const PluginPropertyKey<EntryPointProperty> &entry_point_key);

/* Apply a full target permutation with the PrintingPlan's index-based mover. */
void apply_permutation(const PrintingToolGroup &tool_group,
                       const std::vector<uint32_t> &target_original_indices);

/* Convert the optional unscaled print start setting to plan coordinates. */
c_point initial_print_position(const Print &print);

void collect_extreme_points(const ExtrusionEntity &entity, ExtremePoints &extremes)
{
    for (uint32_t point_idx = 0; point_idx < entity.point_count(); ++point_idx)
        include_extreme_point(entity.point(point_idx), extremes);

    for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        collect_extreme_points(entity.child(child_idx), extremes);
}

void include_extreme_point(const c_point point, ExtremePoints &extremes)
{
    if (!extremes.valid) {
        extremes.left = point;
        extremes.right = point;
        extremes.bottom = point;
        extremes.top = point;
        extremes.valid = true;
        return;
    }

    /*
    Secondary comparisons deliberately select opposite rectangle corners. A
    four-vertex rectangle therefore exposes four candidates instead of
    collapsing left/bottom and right/top onto only two points.
    */
    if (point.x < extremes.left.x ||
        (point.x == extremes.left.x && point.y < extremes.left.y))
        extremes.left = point;
    if (point.x > extremes.right.x ||
        (point.x == extremes.right.x && point.y > extremes.right.y))
        extremes.right = point;
    if (point.y < extremes.bottom.y ||
        (point.y == extremes.bottom.y && point.x > extremes.bottom.x))
        extremes.bottom = point;
    if (point.y > extremes.top.y ||
        (point.y == extremes.top.y && point.x < extremes.top.x))
        extremes.top = point;
}

OrderingItem make_ordering_item(const ExtrusionEntity &root)
{
    assert(!root.empty());
    ExtremePoints extremes;
    collect_extreme_points(root, extremes);
    if (!extremes.valid)
        throw std::invalid_argument("A non-empty PrintingExtrusion has no geometric point.");

    OrderingItem item;
    item.candidates.reserve(4);
    append_distinct_candidate(item, extremes.left);
    append_distinct_candidate(item, extremes.right);
    append_distinct_candidate(item, extremes.bottom);
    append_distinct_candidate(item, extremes.top);
    return item;
}

void append_distinct_candidate(OrderingItem &item, const c_point point)
{
    for (const OrderingCandidate &candidate : item.candidates)
        if (points_equal(candidate.estimated_entry, point))
            return;

    OrderingCandidate candidate;
    candidate.estimated_entry = point;
    candidate.estimated_exit = point;
    item.candidates.push_back(candidate);
}

c_point order_tool_group(const PrintingToolGroup &tool_group,
                         const c_point initial_position,
                         const OrderingEngine &engine,
                         const PluginPropertyKey<EntryPointProperty> &entry_point_key)
{
    const uint32_t extrusion_count = tool_group.extrusion_count();
    std::vector<OrderingItem> items;
    std::vector<uint32_t> non_empty_slots;
    items.reserve(extrusion_count);
    non_empty_slots.reserve(extrusion_count);

    /*
    Build one engine item per geometric root. Property publication happens
    before vector movement, while each original index still identifies the
    corresponding root unambiguously.
    */
    for (uint32_t extrusion_idx = 0; extrusion_idx < extrusion_count; ++extrusion_idx) {
        MutableExtrusionEntity root = tool_group.extrusion(extrusion_idx).mutable_root();
        if (root.empty()) {
            entry_point_key.remove(root);
            continue;
        }
        non_empty_slots.push_back(extrusion_idx);
        items.push_back(make_ordering_item(root.readonly()));
    }

    if (items.empty())
        return initial_position;

    const std::vector<OrderedItem> ordered = engine.order(items, initial_position);
    if (ordered.size() != items.size())
        throw std::runtime_error("The ordering engine returned an incomplete PrintingExtrusion permutation.");

    std::vector<uint32_t> target_original_indices(extrusion_count);
    std::iota(target_original_indices.begin(), target_original_indices.end(), 0u);
    std::vector<uint8_t> selected_items(items.size(), uint8_t(0));

    for (uint32_t order_idx = 0; order_idx < ordered.size(); ++order_idx) {
        const OrderedItem &selection = ordered[order_idx];
        if (selection.item_index >= items.size() || selected_items[selection.item_index] != 0)
            throw std::runtime_error("The ordering engine returned an invalid PrintingExtrusion permutation.");
        selected_items[selection.item_index] = 1;

        const uint32_t original_slot = non_empty_slots[selection.item_index];
        target_original_indices[non_empty_slots[order_idx]] = original_slot;

        MutableExtrusionEntity original_root = tool_group.extrusion(original_slot).mutable_root();
        EntryPointProperty &entry_points = entry_point_key.get_or_add(original_root);
        entry_points.entry = selection.selected_candidate.estimated_entry;
        entry_points.exit = selection.selected_candidate.estimated_exit;
    }

    apply_permutation(tool_group, target_original_indices);
    return ordered.back().selected_candidate.estimated_exit;
}

void apply_permutation(const PrintingToolGroup &tool_group,
                       const std::vector<uint32_t> &target_original_indices)
{
    std::vector<uint32_t> current_original_indices(target_original_indices.size());
    std::iota(current_original_indices.begin(), current_original_indices.end(), 0u);

    /*
    Fix one destination at a time. Tracking original indices outside the moved
    vector avoids retaining borrowed PrintingExtrusion handles, which vector
    insertion would invalidate.
    */
    for (uint32_t destination_idx = 0; destination_idx < target_original_indices.size(); ++destination_idx) {
        const std::vector<uint32_t>::iterator source = std::find(
            current_original_indices.begin() + destination_idx,
            current_original_indices.end(),
            target_original_indices[destination_idx]);
        if (source == current_original_indices.end())
            throw std::runtime_error("Cannot reconstruct the PrintingExtrusion permutation.");

        const uint32_t source_idx = uint32_t(source - current_original_indices.begin());
        if (source_idx == destination_idx)
            continue;
        if (!tool_group.move_extrusion(source_idx, destination_idx))
            throw std::runtime_error("Cannot move a PrintingExtrusion to its ordered position.");

        const uint32_t moved = current_original_indices[source_idx];
        current_original_indices.erase(current_original_indices.begin() + source_idx);
        current_original_indices.insert(current_original_indices.begin() + destination_idx, moved);
    }
}

c_point initial_print_position(const Print &print)
{
    /*
    print_start_position is optional in this branch. It cannot be advertised
    through used_config_keys() yet because that registry currently treats a
    missing definition as a fatal plugin contract mismatch rather than an
    optional lookup.
    */
    const ConfigPoint position =
        print.config().point_or_default("print_start_position", ConfigPoint{});
    if (!std::isfinite(position.x) || !std::isfinite(position.y))
        throw std::invalid_argument("print_start_position must contain finite coordinates.");
    return c_point{scale_i(position.x), scale_i(position.y)};
}

class PrintingExtrusionPreSort : public PluginBase
{
public:
    static PrintingExtrusionPreSort &instance(orchestrator_handle *orchestrator)
    {
        static PrintingExtrusionPreSort instance(orchestrator);
        return instance;
    }

    explicit PrintingExtrusionPreSort(orchestrator_handle *orchestrator) :
        PluginBase(orchestrator),
        m_entry_point_key(entry_point_property_key(orchestrator))
    {
    }

private:
    const char *id_impl() const noexcept override
    {
        return "ordering.printing_extrusion.presort.default";
    }
    const char *name_impl() const noexcept override { return "Printing extrusion pre-sort"; }
    const char *description_impl() const noexcept override
    {
        return "Coarsely orders each tool visit and publishes estimated extrusion entry points.";
    }
    const char *exclusive_group_impl() const noexcept override { return k_exclusive_group; }
    const char *exclusive_group_label_impl() const noexcept override
    {
        return "Printing extrusion pre-sort";
    }
    const char *exclusive_group_tooltip_impl() const noexcept override
    {
        return "Selects how complete PrintingExtrusion objects are coarsely ordered.";
    }
    slicing_step_t step_impl() const noexcept override { return STEP_ORDERING; }
    const char *const *dependencies_impl() const noexcept override { return k_no_dependencies; }
    int32_t priority_impl() const noexcept override { return 1000; }

    void run_impl(const plugin_run_context *run_ctx) const override
    {
        const run_ctx_extrusion_ordering *ctx = plugin_ctx_as_extrusion_ordering(run_ctx);
        assert(ctx != nullptr);
        assert(ctx == nullptr || ctx->print != nullptr);
        assert(ctx == nullptr || ctx->plan != nullptr);
        if (ctx == nullptr || ctx->print == nullptr || ctx->plan == nullptr)
            return;

        c_point current_position = initial_print_position(Print(ctx->print));
        const PrintingPlan plan(ctx->plan);

        /*
        The outer hierarchy is already final. Carrying one scalar position
        through it improves continuity while every call below remains confined
        to the current tool group's extrusion vector.
        */
        for (uint32_t group_idx = 0; group_idx < plan.group_count(); ++group_idx) {
            const PrintingGroup group = plan.group(group_idx);
            for (uint32_t layer_idx = 0; layer_idx < group.layer_group_count(); ++layer_idx) {
                const PrintingLayerGroup layer = group.layer_group(layer_idx);
                for (uint32_t tool_idx = 0; tool_idx < layer.tool_group_count(); ++tool_idx)
                    current_position = order_tool_group(
                        layer.tool_group(tool_idx), current_position,
                        m_ordering_engine, m_entry_point_key);
            }
        }
    }

    PluginPropertyKey<EntryPointProperty> m_entry_point_key;
    KDTreeOrderingEngine m_ordering_engine;
};

} // namespace

void register_printing_extrusion_pre_sort_plugin(orchestrator_handle *orchestrator)
{
    orchestrator_register_plugin(
        orchestrator, PrintingExtrusionPreSort::instance(orchestrator).c_instance());
}

}}} // namespace slic3r_api::Ordering::DefaultOrderingPlugin
