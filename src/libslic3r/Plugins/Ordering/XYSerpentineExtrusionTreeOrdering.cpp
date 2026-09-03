///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "XYSerpentineExtrusionTreeOrdering.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "ExtrusionTreeOrderingGeometry.hpp"

#include "libslic3r/Api/plugin/c/steps/slic3r_step_ordering.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_seam_placer.h"
#include "libslic3r/Api/plugin/cpp/OrchestratorViews.hpp"
#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"
#include "libslic3r/Api/plugin/cpp/PrintingPlanViews.hpp"
#include "libslic3r/Api/plugin/cpp/SeamPlacerViews.hpp"
#include "libslic3r/Plugins/PrintingPlan/EntryPointProperty.h"

namespace slic3r_api { namespace Ordering { namespace DefaultOrderingPlugin {
namespace {

/*
XY-serpentine extrusion-tree ordering
=====================================

This provider offers a predictable spatial alternative to travel-distance
optimization. It first computes one bounding box per entity in post-order, so
every parent reuses child bounds instead of rescanning complete subtrees.
Within each tool visit it orders complete PrintingExtrusion objects by their
box centres. It then recursively applies the same rule to direct children of
sortable nodes. Empty slots and all ownership boundaries remain fixed.

The greedy walk starts at the lowest, then leftmost, centre. It keeps moving in
the current X direction and chooses the candidate minimizing planar distance
plus its height above the lowest unvisited Y. When no candidate remains ahead,
the direction reverses. Resetting that rule for each sortable collection gives
the characteristic broad S without introducing a model-scale-dependent row
height.

Spatial ordering never guesses a printable endpoint. Once every permutation is
fixed, one sequential pass follows the real PrintingPlan order. Reversible
trees face the incoming position, loops ask the shared SeamPlacer for an exact
entry, and every ordering flag is disabled. The resulting root front/back pair
replaces the coarse EntryPointProperty.

The normal call flow is:

    XYSerpentineExtrusionTreeOrdering::run_impl()
    |-- build_bounds_for_plan()
    |   `-- collect_entity_bounds() post-order
    |-- spatially_order_plan()
    |   |-- serpentine_order() for each PrintingToolGroup
    |   `-- spatially_order_entity() recursively
    `-- finalize_plan_geometry()
        `-- finalize_entity()
            |-- reverse_tree() for nearer reversible exits
            |-- SeamPlacer::place_seam() for atomic loops
            `-- disable_ordering_flags_recursively()
*/

const char *k_no_dependencies[] = { nullptr };
const char *k_exclusive_group = "ordering.extrusion_tree";
const char *k_seam_placer_group = "seam_placer_plugin";

struct EntityBounds
{
    c_point min = {};
    c_point max = {};
    bool valid = false;

    c_point center() const
    {
        assert(valid);
        return c_point{
            min.x + (max.x - min.x) / 2,
            min.y + (max.y - min.y) / 2};
    }
};

using EntityBoundsCache = std::unordered_map<
    const extrusion_entity_handle *, EntityBounds>;

struct SpatialItem
{
    uint32_t original_index = 0;
    c_point center = {};
};

/* Select and initialize the active seam-placement service for this plan. */
SeamPlacer create_seam_placer(orchestrator_handle *orchestrator,
                              const Print &print,
                              const PrintingPlan &plan);

/* Return the optional print start in scaled PrintingPlan coordinates. */
c_point initial_print_position(const Print &print);

/* Include one point in a mutable subtree bounding box. */
void include_point(EntityBounds &bounds, c_point point);

/* Merge a previously computed child box into its parent box. */
void merge_bounds(EntityBounds &destination, const EntityBounds &source);

/* Compute and cache one subtree box after recursively computing its children. */
EntityBounds collect_entity_bounds(const ExtrusionEntity &entity,
                                   EntityBoundsCache &cache);

/* Populate boxes for every extrusion root in the plan. */
void build_bounds_for_plan(const PrintingPlan &plan, EntityBoundsCache &cache);

/* Return a deterministic broad-S permutation of the supplied centres. */
std::vector<uint32_t> serpentine_order(const std::vector<SpatialItem> &items);

/* Compare candidates whose floating-point scores are exactly equal. */
bool deterministic_candidate_less(const SpatialItem &lhs,
                                  const SpatialItem &rhs,
                                  bool increasing_x);

/* Reorder non-empty PrintingExtrusion values while preserving empty slots. */
void spatially_order_tool_group(PrintingToolGroup tool_group,
                                const EntityBoundsCache &cache);

/* Apply an original-index permutation to one PrintingExtrusion vector. */
void apply_printing_extrusion_permutation(
    PrintingToolGroup tool_group,
    const std::vector<uint32_t> &target_original_indices);

/* Reorder sortable descendants from cached centres; loops remain atomic. */
void spatially_order_entity(MutableExtrusionEntity entity,
                            const EntityBoundsCache &cache);

/* Reorder non-empty direct children while preserving empty child slots. */
void apply_child_permutation(MutableExtrusionEntity parent,
                             const std::vector<const extrusion_entity_handle *> &handles,
                             const std::vector<uint32_t> &order);

/* Apply spatial ordering to every local vector and sortable collection. */
void spatially_order_plan(const PrintingPlan &plan,
                          const EntityBoundsCache &cache);

/* Fix one tree from the real incoming position and return its exact exit. */
c_point finalize_entity(MutableExtrusionEntity entity,
                        c_point current_position,
                        const SeamPlacer &seam_placer,
                        storage_handle *scratch_storage);

/* Materialize seams/orientations and publish exact root endpoints. */
void finalize_plan_geometry(
    const PrintingPlan &plan,
    c_point initial_position,
    const SeamPlacer &seam_placer,
    storage_handle *scratch_storage,
    const PluginPropertyKey<EntryPointProperty> &entry_point_key,
    PluginProgress &progress);

/* Count roots so progress represents both empty and geometric extrusions. */
uint32_t extrusion_count(const PrintingPlan &plan);

/* Squared distance is sufficient for strict orientation comparisons. */
double squared_distance(c_point lhs, c_point rhs);

SeamPlacer create_seam_placer(orchestrator_handle *orchestrator,
                              const Print &print,
                              const PrintingPlan &plan)
{
    OrchestratorView orchestrator_view(orchestrator);
    const PluginView provider = orchestrator_view.select_plugin(
        print.config().handle(), SEAM_PLACER, k_seam_placer_group);
    if (!provider.valid())
        throw std::runtime_error(
            "XY-serpentine ordering requires an active SEAM_PLACER provider.");

    run_ctx_seam_placer context = {};
    context.plan = plan.handle();
    const raw_plugin_execution_status status = orchestrator_view.execute(
        provider, print.handle(), context);
    if (status != RAW_PLUGIN_EXECUTION_SUCCESS)
        throw std::runtime_error(
            "The selected SEAM_PLACER provider could not initialize its session.");

    SeamPlacer seam_placer(context.instance);
    if (!seam_placer.valid())
        throw std::runtime_error(
            "The selected SEAM_PLACER provider published an invalid session.");
    return seam_placer;
}

c_point initial_print_position(const Print &print)
{
    const ConfigPoint position =
        print.config().point_or_default("print_start_position", ConfigPoint{});
    if (!std::isfinite(position.x) || !std::isfinite(position.y))
        throw std::invalid_argument(
            "print_start_position must contain finite coordinates.");
    return c_point{scale_i(position.x), scale_i(position.y)};
}

void include_point(EntityBounds &bounds, const c_point point)
{
    if (!bounds.valid) {
        bounds.min = point;
        bounds.max = point;
        bounds.valid = true;
        return;
    }
    bounds.min.x = std::min(bounds.min.x, point.x);
    bounds.min.y = std::min(bounds.min.y, point.y);
    bounds.max.x = std::max(bounds.max.x, point.x);
    bounds.max.y = std::max(bounds.max.y, point.y);
}

void merge_bounds(EntityBounds &destination, const EntityBounds &source)
{
    if (!source.valid)
        return;
    include_point(destination, source.min);
    include_point(destination, source.max);
}

EntityBounds collect_entity_bounds(const ExtrusionEntity &entity,
                                   EntityBoundsCache &cache)
{
    EntityBounds bounds;
    for (uint32_t point_idx = 0; point_idx < entity.point_count(); ++point_idx)
        include_point(bounds, entity.point(point_idx));
    for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        merge_bounds(bounds, collect_entity_bounds(entity.child(child_idx), cache));
    cache[entity.handle()] = bounds;
    return bounds;
}

void build_bounds_for_plan(const PrintingPlan &plan, EntityBoundsCache &cache)
{
    for (uint32_t group_idx = 0; group_idx < plan.group_count(); ++group_idx) {
        const PrintingGroup group = plan.group(group_idx);
        for (uint32_t layer_idx = 0; layer_idx < group.layer_group_count(); ++layer_idx) {
            const PrintingLayerGroup layer = group.layer_group(layer_idx);
            for (uint32_t tool_idx = 0; tool_idx < layer.tool_group_count(); ++tool_idx) {
                const PrintingToolGroup tool_group = layer.tool_group(tool_idx);
                for (uint32_t extrusion_idx = 0;
                     extrusion_idx < tool_group.extrusion_count(); ++extrusion_idx)
                    collect_entity_bounds(
                        tool_group.extrusion(extrusion_idx).root(), cache);
            }
        }
    }
}

std::vector<uint32_t> serpentine_order(const std::vector<SpatialItem> &items)
{
    std::vector<uint32_t> result;
    if (items.empty())
        return result;

    std::vector<uint8_t> selected(items.size(), uint8_t(0));
    uint32_t current_idx = 0;
    for (uint32_t idx = 1; idx < items.size(); ++idx)
        if (deterministic_candidate_less(items[idx], items[current_idx], true))
            current_idx = idx;
    selected[current_idx] = 1;
    result.push_back(current_idx);
    bool increasing_x = true;

    while (result.size() < items.size()) {
        coord_t minimum_y = (std::numeric_limits<coord_t>::max)();
        for (uint32_t idx = 0; idx < items.size(); ++idx)
            if (selected[idx] == 0)
                minimum_y = std::min(minimum_y, items[idx].center.y);

        uint32_t best_idx = uint32_t(items.size());
        double best_score = (std::numeric_limits<double>::max)();
        for (uint32_t pass = 0; pass < 2 && best_idx == items.size(); ++pass) {
            for (uint32_t idx = 0; idx < items.size(); ++idx) {
                if (selected[idx] != 0)
                    continue;
                const double delta_x = double(items[idx].center.x) -
                                       double(items[current_idx].center.x);
                if ((increasing_x && delta_x < 0) || (!increasing_x && delta_x > 0))
                    continue;

                const double dy = double(items[idx].center.y) -
                                  double(items[current_idx].center.y);
                const double y_penalty = std::max(
                    0.0, double(items[idx].center.y) - double(minimum_y));
                const double score = std::hypot(delta_x, dy) + y_penalty;
                if (best_idx == items.size() || score < best_score ||
                    (score == best_score && deterministic_candidate_less(
                        items[idx], items[best_idx], increasing_x))) {
                    best_idx = idx;
                    best_score = score;
                }
            }
            if (best_idx == items.size())
                increasing_x = !increasing_x;
        }
        if (best_idx == items.size())
            throw std::runtime_error(
                "The XY-serpentine ordering could not select a remaining item.");
        selected[best_idx] = 1;
        result.push_back(best_idx);
        current_idx = best_idx;
    }
    return result;
}

bool deterministic_candidate_less(const SpatialItem &lhs,
                                  const SpatialItem &rhs,
                                  const bool increasing_x)
{
    if (lhs.center.y != rhs.center.y)
        return lhs.center.y < rhs.center.y;
    if (lhs.center.x != rhs.center.x)
        return increasing_x ? lhs.center.x < rhs.center.x
                            : lhs.center.x > rhs.center.x;
    return lhs.original_index < rhs.original_index;
}

void spatially_order_tool_group(PrintingToolGroup tool_group,
                                const EntityBoundsCache &cache)
{
    std::vector<SpatialItem> items;
    std::vector<uint32_t> non_empty_slots;
    items.reserve(tool_group.extrusion_count());
    non_empty_slots.reserve(tool_group.extrusion_count());
    for (uint32_t idx = 0; idx < tool_group.extrusion_count(); ++idx) {
        const ExtrusionEntity root = tool_group.extrusion(idx).root();
        const EntityBoundsCache::const_iterator found = cache.find(root.handle());
        if (found == cache.end())
            throw std::runtime_error("An extrusion root is missing its bounding box.");
        if (!found->second.valid)
            continue;
        non_empty_slots.push_back(idx);
        items.push_back(SpatialItem{idx, found->second.center()});
    }
    if (items.empty())
        return;

    const std::vector<uint32_t> order = serpentine_order(items);
    std::vector<uint32_t> target_indices(tool_group.extrusion_count());
    std::iota(target_indices.begin(), target_indices.end(), 0u);
    for (uint32_t ordered_idx = 0; ordered_idx < order.size(); ++ordered_idx)
        target_indices[non_empty_slots[ordered_idx]] =
            items[order[ordered_idx]].original_index;
    apply_printing_extrusion_permutation(tool_group, target_indices);
}

void apply_printing_extrusion_permutation(
    PrintingToolGroup tool_group,
    const std::vector<uint32_t> &target_original_indices)
{
    std::vector<uint32_t> current(target_original_indices.size());
    std::iota(current.begin(), current.end(), 0u);
    for (uint32_t target_idx = 0; target_idx < target_original_indices.size(); ++target_idx) {
        const std::vector<uint32_t>::iterator source = std::find(
            current.begin() + target_idx, current.end(),
            target_original_indices[target_idx]);
        if (source == current.end())
            throw std::runtime_error(
                "Cannot reconstruct the XY-serpentine PrintingExtrusion order.");
        const uint32_t source_idx = uint32_t(source - current.begin());
        if (source_idx == target_idx)
            continue;
        if (!tool_group.move_extrusion(source_idx, target_idx))
            throw std::runtime_error(
                "The PrintingToolGroup rejected its XY-serpentine order.");
        const uint32_t moved = current[source_idx];
        current.erase(current.begin() + source_idx);
        current.insert(current.begin() + target_idx, moved);
    }
}

void spatially_order_entity(MutableExtrusionEntity entity,
                            const EntityBoundsCache &cache)
{
    if (entity.empty() || entity.readonly().is_loop())
        return;

    if (entity.readonly().sortable()) {
        std::vector<SpatialItem> items;
        std::vector<const extrusion_entity_handle *> handles;
        items.reserve(entity.child_count());
        handles.reserve(entity.child_count());
        for (uint32_t idx = 0; idx < entity.child_count(); ++idx) {
            const ExtrusionEntity child = entity.child(idx);
            const EntityBoundsCache::const_iterator found = cache.find(child.handle());
            if (found == cache.end())
                throw std::runtime_error("An extrusion child is missing its bounding box.");
            if (!found->second.valid)
                continue;
            items.push_back(SpatialItem{idx, found->second.center()});
            handles.push_back(child.handle());
        }
        apply_child_permutation(entity, handles, serpentine_order(items));
    }

    for (uint32_t idx = 0; idx < entity.child_count(); ++idx)
        spatially_order_entity(entity.child_mutable(idx), cache);
}

void apply_child_permutation(
    MutableExtrusionEntity parent,
    const std::vector<const extrusion_entity_handle *> &handles,
    const std::vector<uint32_t> &order)
{
    if (handles.size() != order.size())
        throw std::runtime_error("The child serpentine permutation is incomplete.");
    std::vector<const extrusion_entity_handle *> current;
    std::vector<const extrusion_entity_handle *> target;
    std::vector<uint32_t> non_empty_slots;
    current.reserve(parent.child_count());
    target.reserve(parent.child_count());
    non_empty_slots.reserve(handles.size());
    for (uint32_t child_idx = 0; child_idx < parent.child_count(); ++child_idx) {
        const ExtrusionEntity child = parent.child(child_idx);
        current.push_back(child.handle());
        target.push_back(child.handle());
        if (!child.empty())
            non_empty_slots.push_back(child_idx);
    }
    if (non_empty_slots.size() != order.size())
        throw std::runtime_error("The child serpentine permutation is invalid.");
    for (uint32_t ordered_idx = 0; ordered_idx < order.size(); ++ordered_idx) {
        if (order[ordered_idx] >= handles.size())
            throw std::runtime_error("The child serpentine permutation is invalid.");
        target[non_empty_slots[ordered_idx]] = handles[order[ordered_idx]];
    }

    /* Reconstruct the complete sequence so every empty child returns to its slot. */
    for (uint32_t target_idx = 0; target_idx < parent.child_count(); ++target_idx) {
        if (current[target_idx] == target[target_idx])
            continue;
        uint32_t source_idx = target_idx + 1;
        while (source_idx < parent.child_count() &&
               current[source_idx] != target[target_idx])
            ++source_idx;
        if (source_idx == parent.child_count() ||
            parent.move_child_from(target_idx, parent, source_idx) != target_idx)
            throw std::runtime_error(
                "The extrusion node rejected its XY-serpentine child order.");
        const extrusion_entity_handle *moved = current[source_idx];
        current.erase(current.begin() + source_idx);
        current.insert(current.begin() + target_idx, moved);
    }
}

void spatially_order_plan(const PrintingPlan &plan,
                          const EntityBoundsCache &cache)
{
    for (uint32_t group_idx = 0; group_idx < plan.group_count(); ++group_idx) {
        const PrintingGroup group = plan.group(group_idx);
        for (uint32_t layer_idx = 0; layer_idx < group.layer_group_count(); ++layer_idx) {
            const PrintingLayerGroup layer = group.layer_group(layer_idx);
            for (uint32_t tool_idx = 0; tool_idx < layer.tool_group_count(); ++tool_idx) {
                const PrintingToolGroup tool_group = layer.tool_group(tool_idx);
                spatially_order_tool_group(tool_group, cache);
                for (uint32_t extrusion_idx = 0;
                     extrusion_idx < tool_group.extrusion_count(); ++extrusion_idx)
                    spatially_order_entity(
                        tool_group.extrusion(extrusion_idx).mutable_root(), cache);
            }
        }
    }
}

c_point finalize_entity(MutableExtrusionEntity entity,
                        const c_point current_position,
                        const SeamPlacer &seam_placer,
                        storage_handle *scratch_storage)
{
    if (entity.empty()) {
        TreeOrderingGeometry::disable_ordering_flags_recursively(entity);
        return current_position;
    }
    if (entity.readonly().is_loop()) {
        const c_point seam = seam_placer.place_seam(
            entity.readonly(), current_position);
        TreeOrderingGeometry::rotate_loop_to_seam(
            scratch_storage, entity, seam);
        TreeOrderingGeometry::disable_ordering_flags_recursively(entity);
        return entity.back();
    }

    if (entity.readonly().reversible() &&
        squared_distance(current_position, entity.back()) <
            squared_distance(current_position, entity.front()))
        TreeOrderingGeometry::reverse_tree(entity);

    c_point position = current_position;
    for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        position = finalize_entity(
            entity.child_mutable(child_idx), position,
            seam_placer, scratch_storage);
    entity.disable_sort();
    entity.disable_reverse();
    return entity.point_count() > 0 ? entity.back() : position;
}

void finalize_plan_geometry(
    const PrintingPlan &plan,
    c_point current_position,
    const SeamPlacer &seam_placer,
    storage_handle *scratch_storage,
    const PluginPropertyKey<EntryPointProperty> &entry_point_key,
    PluginProgress &progress)
{
    for (uint32_t group_idx = 0; group_idx < plan.group_count(); ++group_idx) {
        const PrintingGroup group = plan.group(group_idx);
        for (uint32_t layer_idx = 0; layer_idx < group.layer_group_count(); ++layer_idx) {
            const PrintingLayerGroup layer = group.layer_group(layer_idx);
            for (uint32_t tool_idx = 0; tool_idx < layer.tool_group_count(); ++tool_idx) {
                const PrintingToolGroup tool_group = layer.tool_group(tool_idx);
                for (uint32_t extrusion_idx = 0;
                     extrusion_idx < tool_group.extrusion_count(); ++extrusion_idx) {
                    MutableExtrusionEntity root =
                        tool_group.extrusion(extrusion_idx).mutable_root();
                    if (root.empty()) {
                        entry_point_key.remove(root);
                        TreeOrderingGeometry::disable_ordering_flags_recursively(root);
                        progress.increment();
                        continue;
                    }
                    current_position = finalize_entity(
                        root, current_position, seam_placer, scratch_storage);
                    EntryPointProperty &property = entry_point_key.get_or_add(root);
                    property.entry = root.front();
                    property.exit = root.back();
                    current_position = property.exit;
                    progress.increment();
                }
            }
        }
    }
}

uint32_t extrusion_count(const PrintingPlan &plan)
{
    uint32_t count = 0;
    for (uint32_t group_idx = 0; group_idx < plan.group_count(); ++group_idx) {
        const PrintingGroup group = plan.group(group_idx);
        for (uint32_t layer_idx = 0; layer_idx < group.layer_group_count(); ++layer_idx) {
            const PrintingLayerGroup layer = group.layer_group(layer_idx);
            for (uint32_t tool_idx = 0; tool_idx < layer.tool_group_count(); ++tool_idx)
                count += layer.tool_group(tool_idx).extrusion_count();
        }
    }
    return count;
}

double squared_distance(const c_point lhs, const c_point rhs)
{
    const double dx = double(lhs.x) - double(rhs.x);
    const double dy = double(lhs.y) - double(rhs.y);
    return dx * dx + dy * dy;
}

class XYSerpentineExtrusionTreeOrdering final : public PluginBase
{
public:
    static XYSerpentineExtrusionTreeOrdering &instance(
        orchestrator_handle *orchestrator)
    {
        static XYSerpentineExtrusionTreeOrdering instance(orchestrator);
        return instance;
    }

    explicit XYSerpentineExtrusionTreeOrdering(orchestrator_handle *orchestrator) :
        PluginBase(orchestrator),
        m_entry_point_key(entry_point_property_key(orchestrator))
    {
    }

private:
    const char *id_impl() const noexcept override
    {
        return "ordering.extrusion_tree.xy_serpentine";
    }
    const char *name_impl() const noexcept override
    {
        return "XY-serpentine extrusion-tree ordering";
    }
    const char *description_impl() const noexcept override
    {
        return "Orders extrusion trees in a deterministic bottom-to-top spatial S.";
    }
    const char *exclusive_group_impl() const noexcept override
    {
        return k_exclusive_group;
    }
    const char *exclusive_group_label_impl() const noexcept override
    {
        return "Extrusion-tree ordering";
    }
    const char *exclusive_group_tooltip_impl() const noexcept override
    {
        return "Selects how STEP_ORDERING fixes the order inside each PrintingExtrusion tree.";
    }
    slicing_step_t step_impl() const noexcept override { return STEP_ORDERING; }
    const char *const *dependencies_impl() const noexcept override
    {
        return k_no_dependencies;
    }
    int32_t priority_impl() const noexcept override { return 1075; }
    const char *progress_message_format_impl() const noexcept override
    {
        return "Ordering extrusion trees in an XY serpentine: %u / %u roots";
    }

    void run_impl(const plugin_run_context *run_ctx) const override
    {
        const run_ctx_extrusion_ordering *ctx =
            plugin_ctx_as_extrusion_ordering(run_ctx);
        if (ctx == nullptr || ctx->print == nullptr || ctx->plan == nullptr ||
            run_ctx == nullptr || run_ctx->plugin_storage == nullptr)
            throw std::invalid_argument(
                "XY-serpentine ordering needs a Print, PrintingPlan, and storage.");

        const Print print(ctx->print);
        const PrintingPlan plan(ctx->plan);
        SeamPlacer seam_placer = create_seam_placer(m_orchestrator, print, plan);
        EntityBoundsCache bounds;
        build_bounds_for_plan(plan, bounds);
        spatially_order_plan(plan, bounds);

        progress().add_max(extrusion_count(plan));
        finalize_plan_geometry(
            plan, initial_print_position(print), seam_placer,
            run_ctx->plugin_storage, m_entry_point_key, progress());
    }

    PluginPropertyKey<EntryPointProperty> m_entry_point_key;
};

} // namespace

void register_xy_serpentine_extrusion_tree_ordering_plugin(
    orchestrator_handle *orchestrator)
{
    orchestrator_register_plugin(
        orchestrator,
        XYSerpentineExtrusionTreeOrdering::instance(orchestrator).c_instance());
}

}}} // namespace slic3r_api::Ordering::DefaultOrderingPlugin
