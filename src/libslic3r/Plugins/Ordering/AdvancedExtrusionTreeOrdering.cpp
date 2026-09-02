///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "AdvancedExtrusionTreeOrdering.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include "libslic3r/Api/plugin/c/steps/slic3r_step_ordering.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_seam_placer.h"
#include "libslic3r/Api/plugin/cpp/OrchestratorViews.hpp"
#include "libslic3r/Api/plugin/cpp/ParallelFor.hpp"
#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"
#include "libslic3r/Api/plugin/cpp/PrintingPlanViews.hpp"
#include "libslic3r/Api/plugin/cpp/SeamPlacerViews.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Plugins/Ordering/KDTreeOrderingEngine.hpp"
#include "libslic3r/Plugins/PrintingPlan/EntryPointProperty.h"

namespace slic3r_api { namespace Ordering { namespace DefaultOrderingPlugin {
namespace {

/*
Parallel seam-aware extrusion-tree ordering
===========================================

PrintingExtrusionPreSort has already fixed the order of complete
PrintingExtrusion objects inside every tool visit and attached approximate
entry and exit points to their roots. This provider creates one seam-placement
session for the whole plan and gives each PrintingToolGroup to an independent
worker.

The pre-sort exits are copied before the first parallel pass. That pass creates
real seams and orientations inside every PrintingExtrusion, but deliberately
keeps all ordering flags available. A sequential barrier then orders each
tool-group's PrintingExtrusion vector from those newly published endpoints and
rebuilds the start position of every tool-group job. The final parallel pass
refines each tree and fixes all remaining ordering flags. Workers share only
the const, thread-safe SeamPlacer and OrderingEngine instances; each worker
receives an independent scratch storage.

Ordering one PrintingExtrusion has two complementary passes. The post-order
analysis does not mutate the tree. A loop is one atomic seam-placement unit,
even when it contains children. An open leaf contributes its current endpoints.
A fixed node propagates the possible entries of its first child and exits of
its last child, while a sortable node summarizes all children with at most four
cardinal representatives. Every ordinary parent also merges the four geometric
extrema already calculated by its children, so nested reductions never rescan
the same subtree. Entry and exit lists intentionally remain independent and
bounded: the coarse sorter may combine estimates that do not describe a real
route, because they guide only child order.

The first descending pass materializes one real route. A sortable node asks the
OrderingEngine to arrange its direct non-empty children from their bounded
estimates. Each selected child is subsequently processed from the actual exit
of its predecessor, so reversible paths choose a real direction and loops ask
the SeamPlacer for a fresh seam. Loop rotation preserves line, arc and Z-offset
segments, including seams projected into a segment. This pass deliberately
retains the sortable and reversible flags for later refinement.

The final descending pass uses only concrete front and back points. A sortable
parent orders its children once, lets every child recursively finalize its seam
and orientation, then orders those fixed children once more. The second order
cannot reverse a child and does not recurse again. Every entity is marked
non-sortable and non-reversible while the recursion unwinds.

The normal call flow is:

    AdvancedExtrusionTreeOrdering::run_impl()
    |-- parallel_for_storage_with_progress(): coarse jobs
    |   `-- coarsely_order_tool_group_job()
    |       `-- coarsely_order_printing_extrusion()
                |-- build_entry_exit_estimates(root)
                |   `-- estimate_entity_entry_exit(entity)
                |       |-- loop: estimate_loop_entry_exit()
                |       |   `-- SeamPlacer::place_seam() for four probes
                |       |-- open leaf: estimate_leaf_entry_exit()
                |       |-- sortable node: recurse, then summarize children
                |       `-- fixed node: recurse, then propagate boundaries
                `-- coarsely_order_entity_descending(root)
                    |-- loop: SeamPlacer::place_seam()
                    |   `-- rotate_loop_to_seam()
                    |-- open or fixed reversible entity:
                    |   `-- reverse_entity_tree()
                    `-- sortable node:
                        |-- build_child_ordering_items()
                        |-- OrderingEngine::order()
                        |-- apply_child_permutation()
                        `-- coarsely_order_entity_descending() for each child
    |-- reorder_printing_extrusions_and_make_jobs()
    |   `-- OrderingEngine::order() once per PrintingToolGroup
    `-- parallel_for_storage_with_progress(): final jobs
        `-- finalize_tool_group_job()
            `-- finalize_printing_extrusion()
                `-- finalize_entity_descending(root)
                    |-- reorder_children_with_current_endpoints(reverse=true)
                    |-- finalize_entity_descending() for each child
                    `-- reorder_children_with_current_endpoints(reverse=false)

The recursive call returns only through the cache: children are inserted before
their parent, and loops deliberately stop recursion even when represented by a
collection. A loop therefore scans its complete geometry once, while ordinary
nodes obtain their extrema by merging cached child summaries. Cardinal reduction
and extreme-point helpers do not change the traversal order.
*/

const char *k_no_dependencies[] = { nullptr };
const char *k_exclusive_group = "ordering.extrusion_tree";
const char *k_seam_placer_group = "seam_placer_plugin";
const coord_t k_cardinal_probe_offset = scale_i(5.0);

/* Four deterministic real vertices delimiting one entity's geometry. */
struct ExtremePoints
{
    c_point left = {};
    c_point right = {};
    c_point bottom = {};
    c_point top = {};
    bool valid = false;
};

/* Independent, deliberately bounded entry and exit estimates for one node. */
struct EntryExitEstimates
{
    std::vector<c_point> entries;
    std::vector<c_point> exits;
    ExtremePoints extremes;
};

using EntryExitEstimateCache = std::unordered_map<
    const extrusion_entity_handle *, EntryExitEstimates>;

struct ToolGroupOrderingJob
{
    PrintingToolGroup tool_group;
    c_point start_position;
};

/* Select, initialize and take ownership of the seam service for this plan. */
SeamPlacer create_seam_placer(orchestrator_handle *orchestrator,
                              const Print &print,
                              const PrintingPlan &plan);

/* Build immutable job inputs while the pre-sort entry points are stable. */
std::vector<ToolGroupOrderingJob> make_tool_group_jobs(
    const PrintingPlan &plan,
    c_point initial_position,
    const PluginPropertyKey<EntryPointProperty> &entry_point_key);

/* Return the configured print start in scaled PrintingPlan coordinates. */
c_point initial_print_position(const Print &print);

/*
Order complete PrintingExtrusion objects after the coarse parallel pass.

This sequential barrier carries one position through the complete plan and
returns fresh immutable job seeds for the final parallel pass.
*/
std::vector<ToolGroupOrderingJob> reorder_printing_extrusions_and_make_jobs(
    const PrintingPlan &plan,
    c_point initial_position,
    const OrderingEngine &ordering_engine,
    const PluginPropertyKey<EntryPointProperty> &entry_point_key);

/* Order one tool-group vector from its published concrete root endpoints. */
c_point reorder_printing_extrusions(
    PrintingToolGroup tool_group,
    c_point start_position,
    const OrderingEngine &ordering_engine,
    const PluginPropertyKey<EntryPointProperty> &entry_point_key);

/* Apply an index permutation while keeping empty extrusions in their slots. */
void apply_printing_extrusion_permutation(
    PrintingToolGroup tool_group,
    const std::vector<uint32_t> &target_original_indices);

/* Analyze one complete tree and require a usable estimate for its root. */
EntryExitEstimateCache build_entry_exit_estimates(
    const ExtrusionEntity &root,
    const SeamPlacer &seam_placer);

/* Populate estimates below entity, then publish entity's own post-order result. */
void estimate_entity_entry_exit(
    const ExtrusionEntity &entity,
    const SeamPlacer &seam_placer,
    EntryExitEstimateCache &estimates);

/* Ask the seam service for up to four distinct entries of one atomic loop. */
EntryExitEstimates estimate_loop_entry_exit(
    const ExtrusionEntity &loop,
    const SeamPlacer &seam_placer);

/* Describe the current direction alternatives of one open leaf. */
EntryExitEstimates estimate_leaf_entry_exit(const ExtrusionEntity &leaf);

/* Propagate a fixed node's ends, including whole-node reversal when allowed. */
EntryExitEstimates estimate_fixed_node_entry_exit(
    const ExtrusionEntity &node,
    const ExtremePoints &extremes,
    const EntryExitEstimateCache &estimates);

/* Reduce every child endpoint of a sortable node to one cardinal point set. */
EntryExitEstimates estimate_sortable_node_entry_exit(
    const ExtrusionEntity &node,
    const ExtremePoints &extremes,
    const EntryExitEstimateCache &estimates);

/* Keep at most four candidates representing the entity's cardinal directions. */
void reduce_to_cardinal_points(
    const ExtremePoints &extremes,
    std::vector<c_point> &points);

/* Read all local vertices recursively without allocating a flattened point list. */
void collect_extreme_points(
    const ExtrusionEntity &entity,
    ExtremePoints &extremes);

/* Merge one vertex into the deterministic left/right/bottom/top extrema. */
void include_extreme_point(c_point point, ExtremePoints &extremes);

/* Merge one child's four extrema into its parent without visiting geometry. */
void merge_extreme_points(
    const ExtremePoints &source,
    ExtremePoints &destination);

/* Return four approach points displaced five millimetres outside the extrema. */
std::array<c_point, 4> cardinal_probe_points(const ExtremePoints &extremes);

/* Append a point unless an equivalent point is already present. */
void append_distinct_point(std::vector<c_point> &points, c_point point);

/* Compare candidate distances without converting scaled points to millimetres. */
double squared_distance(c_point lhs, c_point rhs);

/*
Materialize one real traversal from current_position and return its exact exit.

The estimate cache chooses only child permutations. Every recursive child still
selects its actual orientation or seam from the position left by its predecessor.
*/
c_point coarsely_order_entity_descending(
    MutableExtrusionEntity entity,
    c_point current_position,
    EntryExitEstimateCache &estimates,
    const OrderingEngine &ordering_engine,
    const SeamPlacer &seam_placer,
    storage_handle *scratch_storage);

/* Build at most sixteen approximate entry/exit pairs for each non-empty child. */
std::vector<OrderingItem> build_child_ordering_items(
    const MutableExtrusionEntity &parent,
    const EntryExitEstimateCache &estimates,
    std::vector<const extrusion_entity_handle *> &ordered_handles);

/* Reject a partial or duplicated engine result before changing the child list. */
void validate_child_order(
    const std::vector<OrderedItem> &order,
    uint32_t item_count);

/* Reorder child handles while restoring empty children to their original slots. */
void apply_child_permutation(
    MutableExtrusionEntity parent,
    const std::vector<const extrusion_entity_handle *> &non_empty_handles,
    const std::vector<OrderedItem> &order);

/*
Reverse one complete subtree.

When estimates is non-null, exchange every cached entry/exit list as well. The
final pass passes null because it no longer consumes speculative estimates.
*/
void reverse_entity_tree(
    MutableExtrusionEntity entity,
    EntryExitEstimateCache *estimates);

/* Place an arbitrary seam on a local or composed loop without flattening it. */
void rotate_loop_to_seam(
    storage_handle *scratch_storage,
    MutableExtrusionEntity loop,
    c_point seam);

/*
Project a point onto the nearest local polyline in an entity tree.

Splitting temporary clones delegates both line and arc projection to the host's
ArcPolyline implementation. The source tree remains unchanged.
*/
bool projected_point_on_entity(
    storage_handle *scratch_storage,
    const ExtrusionEntity &entity,
    c_point requested,
    c_point &projected,
    double &distance_squared);

/* Rotate one closed local polyline by joining its split suffix and prefix. */
void rotate_local_loop_to_seam(
    storage_handle *scratch_storage,
    MutableExtrusionEntity loop,
    c_point seam);

/*
Fix one subtree using its current concrete endpoints and return its final exit.

Sortable parents run a provisional order before recursion and a non-reversing
order afterwards. All flags are disabled as each call returns.
*/
c_point finalize_entity_descending(
    MutableExtrusionEntity entity,
    c_point current_position,
    const OrderingEngine &ordering_engine,
    const SeamPlacer &seam_placer,
    storage_handle *scratch_storage);

/* Reorder direct children from current endpoints and optionally apply reversal. */
void reorder_children_with_current_endpoints(
    MutableExtrusionEntity parent,
    c_point start_position,
    bool allow_reverse,
    const OrderingEngine &ordering_engine);

/* Describe each non-empty direct child with its current legal orientations. */
std::vector<OrderingItem> build_current_endpoint_items(
    const MutableExtrusionEntity &parent,
    bool allow_reverse,
    std::vector<const extrusion_entity_handle *> &ordered_handles);

/* Return whether an engine selection chose the child's reversed endpoints. */
bool selected_candidate_reverses(
    const ExtrusionEntity &child,
    const OrderingCandidate &selected,
    bool allow_reverse);

/* Clear both ordering permissions throughout one atomic loop subtree. */
void disable_entity_ordering_flags_recursively(
    MutableExtrusionEntity entity);

/*
Prepare one complete PrintingExtrusion for seam-aware internal ordering.

The post-order cache first describes approximate alternatives without mutation.
The descending pass then consumes those alternatives, changes child order,
orientation and seams, and publishes the exact resulting root endpoints. Flags
remain available for the later refinement pass.
*/
void coarsely_order_printing_extrusion(
    PrintingExtrusion extrusion,
    EntryPointProperty &entry_points,
    c_point start_position,
    const OrderingEngine &ordering_engine,
    const SeamPlacer &seam_placer,
    storage_handle *scratch_storage);

/* Process one pre-described tool visit without accessing another job. */
void coarsely_order_tool_group_job(
    uint32_t job_idx,
    storage_handle *scratch_storage,
    const std::vector<ToolGroupOrderingJob> *jobs,
    const PluginPropertyKey<EntryPointProperty> *entry_point_key,
    const OrderingEngine *ordering_engine,
    const SeamPlacer *seam_placer);

/* Finalize one root and publish the exact entry and exit left by that pass. */
void finalize_printing_extrusion(
    PrintingExtrusion extrusion,
    EntryPointProperty &entry_points,
    c_point start_position,
    const OrderingEngine &ordering_engine,
    const SeamPlacer &seam_placer,
    storage_handle *scratch_storage);

/* Process one rebuilt tool-group job during the final parallel pass. */
void finalize_tool_group_job(
    uint32_t job_idx,
    storage_handle *scratch_storage,
    const std::vector<ToolGroupOrderingJob> *jobs,
    const PluginPropertyKey<EntryPointProperty> *entry_point_key,
    const OrderingEngine *ordering_engine,
    const SeamPlacer *seam_placer);

SeamPlacer create_seam_placer(orchestrator_handle *orchestrator,
                              const Print &print,
                              const PrintingPlan &plan)
{
    OrchestratorView orchestrator_view(orchestrator);
    const PluginView provider = orchestrator_view.select_plugin(
        print.config().handle(), SEAM_PLACER, k_seam_placer_group);
    if (!provider.valid())
        throw std::runtime_error(
            "Advanced extrusion-tree ordering requires an active SEAM_PLACER provider.");

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

std::vector<ToolGroupOrderingJob> make_tool_group_jobs(
    const PrintingPlan &plan,
    const c_point initial_position,
    const PluginPropertyKey<EntryPointProperty> &entry_point_key)
{
    std::vector<ToolGroupOrderingJob> jobs;
    c_point current_position = initial_position;

    /*
    The hierarchy order is already final. Record one job per tool visit, then
    advance the global seed from its last non-empty extrusion. Empty visits do
    not disturb continuity and still receive a job so the parallel structure
    mirrors the PrintingPlan exactly.
    */
    for (uint32_t group_idx = 0; group_idx < plan.group_count(); ++group_idx) {
        const PrintingGroup group = plan.group(group_idx);
        for (uint32_t layer_idx = 0; layer_idx < group.layer_group_count(); ++layer_idx) {
            const PrintingLayerGroup layer = group.layer_group(layer_idx);
            for (uint32_t tool_idx = 0; tool_idx < layer.tool_group_count(); ++tool_idx) {
                const PrintingToolGroup tool_group = layer.tool_group(tool_idx);
                jobs.push_back(ToolGroupOrderingJob{tool_group, current_position});

                for (uint32_t extrusion_idx = tool_group.extrusion_count();
                     extrusion_idx > 0; --extrusion_idx) {
                    const ExtrusionEntity root =
                        tool_group.extrusion(extrusion_idx - 1).root();
                    if (root.empty())
                        continue;
                    const EntryPointProperty *entry_points =
                        entry_point_key.get(root);
                    if (entry_points == nullptr)
                        throw std::runtime_error(
                            "A non-empty PrintingExtrusion is missing the EntryPointProperty required by advanced ordering.");
                    current_position = entry_points->exit;
                    break;
                }
            }
        }
    }
    return jobs;
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

std::vector<ToolGroupOrderingJob> reorder_printing_extrusions_and_make_jobs(
    const PrintingPlan &plan,
    const c_point initial_position,
    const OrderingEngine &ordering_engine,
    const PluginPropertyKey<EntryPointProperty> &entry_point_key)
{
    std::vector<ToolGroupOrderingJob> jobs;
    c_point current_position = initial_position;

    /*
    This traversal is intentionally sequential. A job starts where the
    preceding tool visit is estimated to finish, so its seed cannot be known
    before the preceding vector has selected its final root order.
    */
    for (uint32_t group_idx = 0; group_idx < plan.group_count(); ++group_idx) {
        const PrintingGroup group = plan.group(group_idx);
        for (uint32_t layer_idx = 0; layer_idx < group.layer_group_count(); ++layer_idx) {
            const PrintingLayerGroup layer = group.layer_group(layer_idx);
            for (uint32_t tool_idx = 0; tool_idx < layer.tool_group_count(); ++tool_idx) {
                const PrintingToolGroup tool_group = layer.tool_group(tool_idx);
                jobs.push_back(ToolGroupOrderingJob{tool_group, current_position});
                current_position = reorder_printing_extrusions(
                    tool_group, current_position, ordering_engine,
                    entry_point_key);
            }
        }
    }
    return jobs;
}

c_point reorder_printing_extrusions(
    const PrintingToolGroup tool_group,
    const c_point start_position,
    const OrderingEngine &ordering_engine,
    const PluginPropertyKey<EntryPointProperty> &entry_point_key)
{
    const uint32_t extrusion_count = tool_group.extrusion_count();
    std::vector<OrderingItem> items;
    std::vector<uint32_t> non_empty_slots;
    items.reserve(extrusion_count);
    non_empty_slots.reserve(extrusion_count);

    /* Every candidate now describes one concrete root traversal. */
    for (uint32_t extrusion_idx = 0; extrusion_idx < extrusion_count; ++extrusion_idx) {
        const ExtrusionEntity root = tool_group.extrusion(extrusion_idx).root();
        if (root.empty())
            continue;

        const EntryPointProperty *entry_points = entry_point_key.get(root);
        if (entry_points == nullptr)
            throw std::runtime_error(
                "A non-empty PrintingExtrusion is missing its coarse EntryPointProperty at the ordering barrier.");

        OrderingCandidate candidate;
        candidate.estimated_entry = entry_points->entry;
        candidate.estimated_exit = entry_points->exit;
        OrderingItem item;
        item.candidates.push_back(candidate);
        items.push_back(std::move(item));
        non_empty_slots.push_back(extrusion_idx);
    }
    if (items.empty())
        return start_position;

    const std::vector<OrderedItem> order = ordering_engine.order(
        items, start_position);
    validate_child_order(order, uint32_t(items.size()));

    std::vector<uint32_t> target_original_indices(extrusion_count);
    for (uint32_t extrusion_idx = 0; extrusion_idx < extrusion_count; ++extrusion_idx)
        target_original_indices[extrusion_idx] = extrusion_idx;
    for (uint32_t ordered_idx = 0; ordered_idx < order.size(); ++ordered_idx)
        target_original_indices[non_empty_slots[ordered_idx]] =
            non_empty_slots[order[ordered_idx].item_index];

    apply_printing_extrusion_permutation(
        tool_group, target_original_indices);
    return order.back().selected_candidate.estimated_exit;
}

void apply_printing_extrusion_permutation(
    const PrintingToolGroup tool_group,
    const std::vector<uint32_t> &target_original_indices)
{
    if (target_original_indices.size() != tool_group.extrusion_count())
        throw std::invalid_argument(
            "A PrintingExtrusion permutation must describe the complete tool group.");

    std::vector<uint32_t> current_original_indices(
        target_original_indices.size());
    for (uint32_t extrusion_idx = 0;
         extrusion_idx < current_original_indices.size(); ++extrusion_idx)
        current_original_indices[extrusion_idx] = extrusion_idx;

    /*
    Fix the vector from left to right. Indices are tracked separately because
    moving a PrintingExtrusion invalidates borrowed vector-element views.
    */
    for (uint32_t target_idx = 0;
         target_idx < target_original_indices.size(); ++target_idx) {
        uint32_t source_idx = target_idx;
        while (source_idx < current_original_indices.size() &&
               current_original_indices[source_idx] !=
                   target_original_indices[target_idx])
            ++source_idx;
        if (source_idx == current_original_indices.size())
            throw std::runtime_error(
                "Cannot reconstruct the PrintingExtrusion permutation.");
        if (source_idx == target_idx)
            continue;
        if (!tool_group.move_extrusion(source_idx, target_idx))
            throw std::runtime_error(
                "The PrintingToolGroup rejected its extrusion permutation.");

        const uint32_t moved = current_original_indices[source_idx];
        current_original_indices.erase(
            current_original_indices.begin() + source_idx);
        current_original_indices.insert(
            current_original_indices.begin() + target_idx, moved);
    }
}

EntryExitEstimateCache build_entry_exit_estimates(
    const ExtrusionEntity &root,
    const SeamPlacer &seam_placer)
{
    EntryExitEstimateCache estimates;
    estimate_entity_entry_exit(root, seam_placer, estimates);

    const EntryExitEstimateCache::const_iterator root_estimate =
        estimates.find(root.handle());
    if (root_estimate == estimates.end() ||
        root_estimate->second.entries.empty() ||
        root_estimate->second.exits.empty())
        throw std::runtime_error(
            "A non-empty PrintingExtrusion produced no usable ordering estimate.");
    return estimates;
}

void estimate_entity_entry_exit(
    const ExtrusionEntity &entity,
    const SeamPlacer &seam_placer,
    EntryExitEstimateCache &estimates)
{
    if (entity.empty())
        return;

    /*
    A composed loop is still one seam-placement unit. Its descendants must not
    publish independent candidates because the future sorter may only rotate
    the complete closed path, not reorder its internal path fragments.
    */
    if (entity.is_loop()) {
        estimates.emplace(
            entity.handle(), estimate_loop_entry_exit(entity, seam_placer));
        return;
    }

    /*
    Child estimates are completed before a collection summarizes them. This is
    the post-order invariant that lets a parent read only small bounded lists.
    */
    for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        estimate_entity_entry_exit(entity.child(child_idx), seam_placer, estimates);

    /*
    Reuse child extrema while unwinding the post-order recursion. Local points
    are included for leaves and for robustness if a future entity form combines
    local geometry with children.
    */
    ExtremePoints entity_extremes;
    for (uint32_t point_idx = 0; point_idx < entity.point_count(); ++point_idx)
        include_extreme_point(entity.point(point_idx), entity_extremes);
    for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx) {
        const ExtrusionEntity child = entity.child(child_idx);
        const EntryExitEstimateCache::const_iterator found =
            estimates.find(child.handle());
        if (found != estimates.end())
            merge_extreme_points(found->second.extremes, entity_extremes);
    }
    if (!entity_extremes.valid)
        throw std::runtime_error(
            "A non-empty extrusion node contains no geometric point.");

    EntryExitEstimates entity_estimates;
    if (entity.is_leaf())
        entity_estimates = estimate_leaf_entry_exit(entity);
    else if (entity.sortable())
        entity_estimates = estimate_sortable_node_entry_exit(
            entity, entity_extremes, estimates);
    else
        entity_estimates = estimate_fixed_node_entry_exit(
            entity, entity_extremes, estimates);

    if (entity_estimates.entries.empty() || entity_estimates.exits.empty())
        throw std::runtime_error(
            "A non-empty extrusion node produced no usable ordering estimate.");
    entity_estimates.extremes = entity_extremes;
    estimates.emplace(entity.handle(), std::move(entity_estimates));
}

EntryExitEstimates estimate_loop_entry_exit(
    const ExtrusionEntity &loop,
    const SeamPlacer &seam_placer)
{
    ExtremePoints extremes;
    collect_extreme_points(loop, extremes);
    if (!extremes.valid)
        throw std::runtime_error(
            "A non-empty extrusion loop contains no geometric point.");

    EntryExitEstimates result;
    result.entries.reserve(4);
    const std::array<c_point, 4> probes = cardinal_probe_points(extremes);
    for (const c_point probe : probes)
        append_distinct_point(
            result.entries, seam_placer.place_seam(loop, probe));

    /* A closed loop exits at the selected seam, so both bounded sets match. */
    result.exits = result.entries;
    result.extremes = extremes;
    return result;
}

EntryExitEstimates estimate_leaf_entry_exit(const ExtrusionEntity &leaf)
{
    EntryExitEstimates result;
    result.entries.reserve(2);
    result.exits.reserve(2);
    append_distinct_point(result.entries, leaf.front());
    append_distinct_point(result.exits, leaf.back());

    /* Reversing an open leaf exchanges its only concrete entry and exit. */
    if (leaf.reversible()) {
        append_distinct_point(result.entries, leaf.back());
        append_distinct_point(result.exits, leaf.front());
    }
    return result;
}

EntryExitEstimates estimate_fixed_node_entry_exit(
    const ExtrusionEntity &node,
    const ExtremePoints &extremes,
    const EntryExitEstimateCache &estimates)
{
    const EntryExitEstimates *first = nullptr;
    const EntryExitEstimates *last = nullptr;

    /* Empty children do not contribute to the physical beginning or end. */
    for (uint32_t child_idx = 0; child_idx < node.child_count(); ++child_idx) {
        const ExtrusionEntity child = node.child(child_idx);
        const EntryExitEstimateCache::const_iterator found =
            estimates.find(child.handle());
        if (found == estimates.end())
            continue;
        if (first == nullptr)
            first = &found->second;
        last = &found->second;
    }
    if (first == nullptr || last == nullptr)
        return {};

    EntryExitEstimates result;
    result.entries = first->entries;
    result.exits = last->exits;

    /* Whole-node reversal exchanges the possibilities at the two boundaries. */
    if (node.reversible()) {
        for (const c_point point : last->exits)
            append_distinct_point(result.entries, point);
        for (const c_point point : first->entries)
            append_distinct_point(result.exits, point);
        reduce_to_cardinal_points(extremes, result.entries);
        reduce_to_cardinal_points(extremes, result.exits);
    }
    return result;
}

EntryExitEstimates estimate_sortable_node_entry_exit(
    const ExtrusionEntity &node,
    const ExtremePoints &extremes,
    const EntryExitEstimateCache &estimates)
{
    std::vector<c_point> child_points;
    child_points.reserve(size_t(node.child_count()) * 8u);

    /*
    A sortable node may choose any non-empty child first or last. Keep only the
    spatial envelope of all child boundaries; their pairing is intentionally
    deferred until the coarse ordering phase.
    */
    for (uint32_t child_idx = 0; child_idx < node.child_count(); ++child_idx) {
        const ExtrusionEntity child = node.child(child_idx);
        const EntryExitEstimateCache::const_iterator found =
            estimates.find(child.handle());
        if (found == estimates.end())
            continue;
        for (const c_point point : found->second.entries)
            append_distinct_point(child_points, point);
        for (const c_point point : found->second.exits)
            append_distinct_point(child_points, point);
    }

    reduce_to_cardinal_points(extremes, child_points);
    EntryExitEstimates result;
    result.entries = child_points;
    result.exits = std::move(child_points);
    return result;
}

void reduce_to_cardinal_points(
    const ExtremePoints &extremes,
    std::vector<c_point> &points)
{
    if (points.size() <= 4)
        return;
    if (!extremes.valid)
        throw std::invalid_argument(
            "Cardinal candidate reduction requires valid geometric extrema.");

    const std::array<c_point, 4> probes = cardinal_probe_points(extremes);
    std::vector<c_point> selected;
    selected.reserve(4);
    for (const c_point probe : probes) {
        uint32_t best_idx = 0;
        double best_distance = (std::numeric_limits<double>::max)();
        for (uint32_t point_idx = 0; point_idx < points.size(); ++point_idx) {
            const double distance = squared_distance(points[point_idx], probe);
            const bool deterministic_tie =
                distance == best_distance &&
                (points[point_idx].x < points[best_idx].x ||
                 (points[point_idx].x == points[best_idx].x &&
                  points[point_idx].y < points[best_idx].y));
            if (distance < best_distance || deterministic_tie) {
                best_idx = point_idx;
                best_distance = distance;
            }
        }
        append_distinct_point(selected, points[best_idx]);
    }
    points = std::move(selected);
}

void collect_extreme_points(
    const ExtrusionEntity &entity,
    ExtremePoints &extremes)
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

    /* Secondary coordinates choose four different corners when possible. */
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

void merge_extreme_points(
    const ExtremePoints &source,
    ExtremePoints &destination)
{
    if (!source.valid)
        return;

    /*
    Each source field is a real vertex. Feeding all four through the same
    comparator preserves the parent's deterministic secondary-coordinate ties.
    */
    include_extreme_point(source.left, destination);
    include_extreme_point(source.right, destination);
    include_extreme_point(source.bottom, destination);
    include_extreme_point(source.top, destination);
}

std::array<c_point, 4> cardinal_probe_points(const ExtremePoints &extremes)
{
    assert(extremes.valid);
    std::array<c_point, 4> probes{
        extremes.left, extremes.right, extremes.bottom, extremes.top};
    probes[0].x -= k_cardinal_probe_offset;
    probes[1].x += k_cardinal_probe_offset;
    probes[2].y -= k_cardinal_probe_offset;
    probes[3].y += k_cardinal_probe_offset;
    return probes;
}

void append_distinct_point(std::vector<c_point> &points, const c_point point)
{
    for (const c_point existing : points)
        if (points_equal(existing, point))
            return;
    points.push_back(point);
}

double squared_distance(const c_point lhs, const c_point rhs)
{
    const double dx = double(lhs.x) - double(rhs.x);
    const double dy = double(lhs.y) - double(rhs.y);
    return dx * dx + dy * dy;
}

c_point coarsely_order_entity_descending(
    MutableExtrusionEntity entity,
    const c_point current_position,
    EntryExitEstimateCache &estimates,
    const OrderingEngine &ordering_engine,
    const SeamPlacer &seam_placer,
    storage_handle *scratch_storage)
{
    if (entity.empty())
        return current_position;

    /*
    A loop is atomic even when its geometry is distributed among children.
    Asking again from the real current position replaces the four speculative
    seams used by the post-order pass with the seam that will actually print.
    */
    if (entity.is_loop()) {
        const c_point seam = seam_placer.place_seam(
            entity.readonly(), current_position);
        rotate_loop_to_seam(scratch_storage, entity, seam);
        return entity.back();
    }

    if (entity.is_leaf()) {
        /* Strict comparison keeps the existing direction on an exact tie. */
        if (entity.readonly().reversible() &&
            squared_distance(current_position, entity.back()) <
                squared_distance(current_position, entity.front()))
            reverse_entity_tree(entity, &estimates);
        return entity.back();
    }

    if (!entity.readonly().sortable()) {
        /*
        A fixed collection may reverse only as one unit. Its current recursive
        endpoints are concrete, so this decision does not use approximate
        entry/exit combinations.
        */
        if (entity.readonly().reversible() &&
            squared_distance(current_position, entity.back()) <
                squared_distance(current_position, entity.front()))
            reverse_entity_tree(entity, &estimates);

        c_point child_position = current_position;
        for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
            child_position = coarsely_order_entity_descending(
                entity.child_mutable(child_idx), child_position, estimates,
                ordering_engine, seam_placer, scratch_storage);
        return child_position;
    }

    /*
    A sortable collection first chooses a coarse child permutation. Its
    selected candidate is intentionally not imposed on the child: the child is
    processed below from the exact position left by the preceding child.
    */
    std::vector<const extrusion_entity_handle *> non_empty_handles;
    const std::vector<OrderingItem> items = build_child_ordering_items(
        entity, estimates, non_empty_handles);
    if (!items.empty()) {
        const std::vector<OrderedItem> order = ordering_engine.order(
            items, current_position);
        validate_child_order(order, uint32_t(items.size()));
        apply_child_permutation(entity, non_empty_handles, order);
    }

    c_point child_position = current_position;
    for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        child_position = coarsely_order_entity_descending(
            entity.child_mutable(child_idx), child_position, estimates,
            ordering_engine, seam_placer, scratch_storage);
    return child_position;
}

std::vector<OrderingItem> build_child_ordering_items(
    const MutableExtrusionEntity &parent,
    const EntryExitEstimateCache &estimates,
    std::vector<const extrusion_entity_handle *> &ordered_handles)
{
    std::vector<OrderingItem> items;
    items.reserve(parent.child_count());
    ordered_handles.clear();
    ordered_handles.reserve(parent.child_count());

    for (uint32_t child_idx = 0; child_idx < parent.child_count(); ++child_idx) {
        const ExtrusionEntity child = parent.child(child_idx);
        if (child.empty())
            continue;

        const EntryExitEstimateCache::const_iterator found =
            estimates.find(child.handle());
        if (found == estimates.end() || found->second.entries.empty() ||
            found->second.exits.empty())
            throw std::runtime_error(
                "A sortable extrusion child has no usable ordering estimate.");

        OrderingItem item;
        item.candidates.reserve(
            found->second.entries.size() * found->second.exits.size());
        for (const c_point entry : found->second.entries) {
            for (const c_point exit : found->second.exits) {
                bool duplicate = false;
                for (const OrderingCandidate &candidate : item.candidates) {
                    if (points_equal(candidate.estimated_entry, entry) &&
                        points_equal(candidate.estimated_exit, exit)) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate) {
                    OrderingCandidate candidate;
                    candidate.estimated_entry = entry;
                    candidate.estimated_exit = exit;
                    item.candidates.push_back(candidate);
                }
            }
        }
        if (item.candidates.empty() || item.candidates.size() > 16)
            throw std::runtime_error(
                "A sortable extrusion child produced an invalid candidate set.");

        ordered_handles.push_back(child.handle());
        items.push_back(std::move(item));
    }
    return items;
}

void validate_child_order(
    const std::vector<OrderedItem> &order,
    const uint32_t item_count)
{
    if (order.size() != item_count)
        throw std::runtime_error(
            "The ordering engine returned a partial child permutation.");

    std::vector<uint8_t> seen(item_count, uint8_t(0));
    for (const OrderedItem &ordered : order) {
        if (ordered.item_index >= item_count || seen[ordered.item_index] != 0)
            throw std::runtime_error(
                "The ordering engine returned an invalid child permutation.");
        seen[ordered.item_index] = uint8_t(1);
    }
}

void apply_child_permutation(
    MutableExtrusionEntity parent,
    const std::vector<const extrusion_entity_handle *> &non_empty_handles,
    const std::vector<OrderedItem> &order)
{
    const uint32_t child_count = parent.child_count();
    std::vector<const extrusion_entity_handle *> current_handles;
    std::vector<const extrusion_entity_handle *> target_handles;
    std::vector<uint32_t> non_empty_slots;
    current_handles.reserve(child_count);
    target_handles.reserve(child_count);
    non_empty_slots.reserve(non_empty_handles.size());

    /*
    Keep a complete target sequence, including empty children. This allows the
    generic move operation to shift elements temporarily while guaranteeing
    that every empty child returns to its original final index.
    */
    for (uint32_t child_idx = 0; child_idx < child_count; ++child_idx) {
        const ExtrusionEntity child = parent.child(child_idx);
        current_handles.push_back(child.handle());
        target_handles.push_back(child.handle());
        if (!child.empty())
            non_empty_slots.push_back(child_idx);
    }
    if (non_empty_slots.size() != order.size() ||
        non_empty_handles.size() != order.size())
        throw std::runtime_error(
            "The child permutation does not match the sortable node.");

    for (uint32_t ordered_idx = 0; ordered_idx < order.size(); ++ordered_idx)
        target_handles[non_empty_slots[ordered_idx]] =
            non_empty_handles[order[ordered_idx].item_index];

    /*
    Fix the sequence from left to right. Once a prefix matches target_handles,
    the wanted handle for the next slot must be in the remaining suffix.
    */
    for (uint32_t target_idx = 0; target_idx < child_count; ++target_idx) {
        if (current_handles[target_idx] == target_handles[target_idx])
            continue;

        uint32_t source_idx = target_idx + 1;
        while (source_idx < child_count &&
               current_handles[source_idx] != target_handles[target_idx])
            ++source_idx;
        if (source_idx == child_count)
            throw std::runtime_error(
                "A child handle disappeared while applying its permutation.");

        const uint32_t moved_idx = parent.move_child_from(
            target_idx, parent, source_idx);
        if (moved_idx != target_idx)
            throw std::runtime_error(
                "The extrusion tree rejected a valid child permutation.");

        const extrusion_entity_handle *moved = current_handles[source_idx];
        current_handles.erase(current_handles.begin() + source_idx);
        current_handles.insert(current_handles.begin() + target_idx, moved);
    }
}

void reverse_entity_tree(
    MutableExtrusionEntity entity,
    EntryExitEstimateCache *estimates)
{
    if (entity.point_count() > 0) {
        if (!entity.reverse())
            throw std::runtime_error(
                "The extrusion polyline could not be reversed.");
    } else {
        /* Reverse each path first, then reverse their order as one unit. */
        for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
            reverse_entity_tree(entity.child_mutable(child_idx), estimates);

        std::vector<const extrusion_entity_handle *> desired;
        std::vector<const extrusion_entity_handle *> current;
        desired.reserve(entity.child_count());
        current.reserve(entity.child_count());
        for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
            current.push_back(entity.child(child_idx).handle());
        for (uint32_t child_idx = entity.child_count(); child_idx > 0; --child_idx)
            desired.push_back(entity.child(child_idx - 1).handle());

        for (uint32_t target_idx = 0; target_idx < desired.size(); ++target_idx) {
            if (current[target_idx] == desired[target_idx])
                continue;
            uint32_t source_idx = target_idx + 1;
            while (source_idx < current.size() &&
                   current[source_idx] != desired[target_idx])
                ++source_idx;
            if (source_idx == current.size() ||
                entity.move_child_from(target_idx, entity, source_idx) != target_idx)
                throw std::runtime_error(
                    "The extrusion collection could not be reversed.");
            const extrusion_entity_handle *moved = current[source_idx];
            current.erase(current.begin() + source_idx);
            current.insert(current.begin() + target_idx, moved);
        }
    }

    /*
    Descendant entries were exchanged by their recursive calls. Exchange this
    node's summary as the recursion unwinds so later sibling ordering observes
    its new direction.
    */
    if (estimates != nullptr) {
        const EntryExitEstimateCache::iterator found =
            estimates->find(entity.handle());
        if (found != estimates->end())
            std::swap(found->second.entries, found->second.exits);
    }
}

void rotate_loop_to_seam(
    storage_handle *scratch_storage,
    MutableExtrusionEntity loop,
    const c_point seam)
{
    if (loop.point_count() > 0) {
        rotate_local_loop_to_seam(scratch_storage, loop, seam);
        return;
    }
    if (loop.child_count() == 0)
        throw std::runtime_error(
            "A seam cannot be materialized on an empty loop.");

    /*
    Locate the direct child whose complete subtree is nearest to the seam. The
    projection helper understands both lines and arcs and resolves a seam on a
    shared child boundary deterministically by child order.
    */
    uint32_t containing_idx = EXTRUSION_INDEX_INVALID;
    c_point projected = {};
    double best_distance = (std::numeric_limits<double>::max)();
    for (uint32_t child_idx = 0; child_idx < loop.child_count(); ++child_idx) {
        c_point child_projection = {};
        double child_distance = (std::numeric_limits<double>::max)();
        if (projected_point_on_entity(
                scratch_storage, loop.child(child_idx), seam,
                child_projection, child_distance) &&
            child_distance < best_distance) {
            containing_idx = child_idx;
            projected = child_projection;
            best_distance = child_distance;
        }
    }
    const double tolerance_squared =
        double(SCALED_EPSILON) * double(SCALED_EPSILON);
    if (containing_idx == EXTRUSION_INDEX_INVALID ||
        best_distance > tolerance_squared)
        throw std::runtime_error(
            "The seam placer returned a point outside the extrusion loop.");

    uint32_t start_idx = containing_idx;
    std::unique_ptr<StoredExtrusionEntity> closing_piece;
    MutableExtrusionEntity child = loop.child_mutable(containing_idx);
    if (child.point_count() > 0) {
        if (points_equal(projected, child.local_front())) {
            start_idx = containing_idx;
        } else if (points_equal(projected, child.local_back())) {
            start_idx = (containing_idx + 1) % loop.child_count();
        } else {
            /*
            Keep the original child handle for the printable suffix. The cloned
            prefix becomes a new final child that closes the rotated loop.
            */
            StoredExtrusionEntity before(scratch_storage, child.readonly());
            StoredExtrusionEntity after(scratch_storage, child.readonly());
            if (extrusion_polyline_split_at_point(
                    child.handle(), projected,
                    before.mutable_handle(), after.mutable_handle()) == 0 ||
                extrusion_move_from(
                    child.mutable_handle(), after.mutable_handle()) == 0)
                throw std::runtime_error(
                    "The extrusion loop could not be split at its seam.");
            closing_piece = std::make_unique<StoredExtrusionEntity>(
                std::move(before));
            start_idx = containing_idx;
        }
    } else {
        rotate_loop_to_seam(scratch_storage, child, projected);
    }

    /* Move the original prefix behind the selected child without cloning it. */
    for (uint32_t moved_count = 0; moved_count < start_idx; ++moved_count) {
        const uint32_t moved_idx = loop.move_child_from(
            loop.child_count(), loop, 0);
        if (moved_idx == EXTRUSION_INDEX_INVALID)
            throw std::runtime_error(
                "The extrusion loop rejected its seam rotation.");
    }
    if (closing_piece != nullptr &&
        loop.append_child_move(closing_piece->mutable_view()) ==
            EXTRUSION_INDEX_INVALID)
        throw std::runtime_error(
            "The extrusion loop rejected its closing seam fragment.");
}

bool projected_point_on_entity(
    storage_handle *scratch_storage,
    const ExtrusionEntity &entity,
    const c_point requested,
    c_point &projected,
    double &distance_squared_out)
{
    bool found_projection = false;
    double best_distance = (std::numeric_limits<double>::max)();
    c_point best_point = {};

    if (entity.point_count() > 0) {
        /*
        This built-in provider may inspect the host entity directly. A geometric
        projection is both cheaper and safer than splitting scratch copies of
        every sibling: an unrelated sibling often projects onto an endpoint,
        where split_at() would temporarily create a degenerate segment.
        */
        const Slic3r::ExtrusionEntity *core_entity =
            reinterpret_cast<const Slic3r::ExtrusionEntity *>(entity.handle());
        const Slic3r::ArcPolyline path = core_entity->as_polyline();
        const Slic3r::Geometry::ArcWelder::PathSegmentProjection projection =
            Slic3r::Geometry::ArcWelder::point_to_path_projection(
                path.get_arc(), Slic3r::Point(requested.x, requested.y));
        if (projection.valid()) {
            best_point = c_point{projection.point.x(), projection.point.y()};
            best_distance = projection.distance2;
            found_projection = true;
        }
    }

    for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx) {
        c_point child_point = {};
        double child_distance = (std::numeric_limits<double>::max)();
        if (projected_point_on_entity(
                scratch_storage, entity.child(child_idx), requested,
                child_point, child_distance) &&
            (!found_projection || child_distance < best_distance)) {
            best_point = child_point;
            best_distance = child_distance;
            found_projection = true;
        }
    }

    if (found_projection) {
        projected = best_point;
        distance_squared_out = best_distance;
    }
    return found_projection;
}

void rotate_local_loop_to_seam(
    storage_handle *scratch_storage,
    MutableExtrusionEntity loop,
    const c_point seam)
{
    StoredExtrusionEntity before(scratch_storage, loop.readonly());
    StoredExtrusionEntity after(scratch_storage, loop.readonly());
    if (extrusion_polyline_split_at_point(
            loop.handle(), seam,
            before.mutable_handle(), after.mutable_handle()) == 0)
        throw std::runtime_error(
            "The local extrusion loop could not be split at its seam.");

    c_point projected = {};
    if (before.point_count() > 0)
        projected = before.local_back();
    else if (after.point_count() > 0)
        projected = after.local_front();
    else
        throw std::runtime_error(
            "The local extrusion loop produced no seam projection.");

    const double tolerance_squared =
        double(SCALED_EPSILON) * double(SCALED_EPSILON);
    if (squared_distance(seam, projected) > tolerance_squared)
        throw std::runtime_error(
            "The seam placer returned a point outside the local loop.");
    if (points_equal(projected, loop.local_front()) ||
        points_equal(projected, loop.local_back()))
        return;

    /*
    Recompose the ring as suffix followed by prefix. Copying true segments
    preserves arc orientation and both endpoint Z offsets at the split.
    */
    std::vector<c_extrusion_segment> rotated_segments = after.segments();
    const std::vector<c_extrusion_segment> prefix_segments = before.segments();
    rotated_segments.insert(
        rotated_segments.end(), prefix_segments.begin(), prefix_segments.end());
    if (!loop.set_segments(rotated_segments))
        throw std::runtime_error(
            "The local extrusion loop rejected its rotated segments.");
}

c_point finalize_entity_descending(
    MutableExtrusionEntity entity,
    const c_point current_position,
    const OrderingEngine &ordering_engine,
    const SeamPlacer &seam_placer,
    storage_handle *scratch_storage)
{
    if (entity.empty()) {
        entity.disable_sort();
        entity.disable_reverse();
        return current_position;
    }

    /*
    Loops remain atomic during ordering. Rotation may create one closing leaf,
    so clear ordering permissions recursively only after the final seam has
    produced the complete final subtree.
    */
    if (entity.is_loop()) {
        const c_point seam = seam_placer.place_seam(
            entity.readonly(), current_position);
        rotate_loop_to_seam(scratch_storage, entity, seam);
        disable_entity_ordering_flags_recursively(entity);
        return entity.back();
    }

    if (entity.is_leaf()) {
        if (entity.readonly().reversible() &&
            squared_distance(current_position, entity.back()) <
                squared_distance(current_position, entity.front()))
            reverse_entity_tree(entity, nullptr);
        entity.disable_sort();
        entity.disable_reverse();
        return entity.back();
    }

    if (!entity.readonly().sortable()) {
        /*
        A fixed collection may still reverse as one unit. Its children are then
        finalized in that concrete order and cannot move independently here.
        */
        if (entity.readonly().reversible() &&
            squared_distance(current_position, entity.back()) <
                squared_distance(current_position, entity.front()))
            reverse_entity_tree(entity, nullptr);

        c_point child_position = current_position;
        for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
            child_position = finalize_entity_descending(
                entity.child_mutable(child_idx), child_position,
                ordering_engine, seam_placer, scratch_storage);
        entity.disable_sort();
        entity.disable_reverse();
        return child_position;
    }

    /*
    The first order uses the best direction currently exposed by each child.
    Recursion then replaces provisional seams and orientations with real ones.
    */
    reorder_children_with_current_endpoints(
        entity, current_position, true, ordering_engine);
    c_point child_position = current_position;
    for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        child_position = finalize_entity_descending(
            entity.child_mutable(child_idx), child_position,
            ordering_engine, seam_placer, scratch_storage);

    /*
    The second order sees final child geometry. It may move complete children,
    but cannot reverse or recursively alter them, so their finalized state is
    preserved even when the local route changes once more.
    */
    reorder_children_with_current_endpoints(
        entity, current_position, false, ordering_engine);
    entity.disable_sort();
    entity.disable_reverse();
    return entity.back();
}

void reorder_children_with_current_endpoints(
    MutableExtrusionEntity parent,
    const c_point start_position,
    const bool allow_reverse,
    const OrderingEngine &ordering_engine)
{
    std::vector<const extrusion_entity_handle *> non_empty_handles;
    const std::vector<OrderingItem> items = build_current_endpoint_items(
        parent, allow_reverse, non_empty_handles);
    if (items.empty())
        return;

    const std::vector<OrderedItem> order = ordering_engine.order(
        items, start_position);
    validate_child_order(order, uint32_t(items.size()));
    apply_child_permutation(parent, non_empty_handles, order);

    /*
    Empty slots remain fixed, so the Nth non-empty child after permutation
    corresponds to order[N]. Apply the selected direction before recursion.
    */
    uint32_t ordered_idx = 0;
    for (uint32_t child_idx = 0; child_idx < parent.child_count(); ++child_idx) {
        MutableExtrusionEntity child = parent.child_mutable(child_idx);
        if (child.empty())
            continue;
        if (selected_candidate_reverses(
                child.readonly(), order[ordered_idx].selected_candidate,
                allow_reverse))
            reverse_entity_tree(child, nullptr);
        ++ordered_idx;
    }
    if (ordered_idx != order.size())
        throw std::runtime_error(
            "The finalized child permutation no longer matches its parent.");
}

std::vector<OrderingItem> build_current_endpoint_items(
    const MutableExtrusionEntity &parent,
    const bool allow_reverse,
    std::vector<const extrusion_entity_handle *> &ordered_handles)
{
    std::vector<OrderingItem> items;
    items.reserve(parent.child_count());
    ordered_handles.clear();
    ordered_handles.reserve(parent.child_count());

    for (uint32_t child_idx = 0; child_idx < parent.child_count(); ++child_idx) {
        const ExtrusionEntity child = parent.child(child_idx);
        if (child.empty())
            continue;

        OrderingCandidate forward;
        forward.estimated_entry = child.front();
        forward.estimated_exit = child.back();
        OrderingItem item;
        item.candidates.push_back(forward);

        /* A closed path has no distinct reverse endpoint candidate. */
        if (allow_reverse && child.reversible() &&
            (!points_equal(child.front(), child.back()))) {
            OrderingCandidate reversed;
            reversed.estimated_entry = child.back();
            reversed.estimated_exit = child.front();
            item.candidates.push_back(reversed);
        }

        ordered_handles.push_back(child.handle());
        items.push_back(std::move(item));
    }
    return items;
}

bool selected_candidate_reverses(
    const ExtrusionEntity &child,
    const OrderingCandidate &selected,
    const bool allow_reverse)
{
    if (!allow_reverse || !child.reversible() ||
        points_equal(child.front(), child.back()))
        return false;

    const bool selected_forward =
        points_equal(selected.estimated_entry, child.front()) &&
        points_equal(selected.estimated_exit, child.back());
    const bool selected_reverse =
        points_equal(selected.estimated_entry, child.back()) &&
        points_equal(selected.estimated_exit, child.front());
    if (!selected_forward && !selected_reverse)
        throw std::runtime_error(
            "The ordering engine selected an endpoint pair that was not offered by the child.");
    return selected_reverse;
}

void disable_entity_ordering_flags_recursively(
    MutableExtrusionEntity entity)
{
    for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        disable_entity_ordering_flags_recursively(
            entity.child_mutable(child_idx));
    entity.disable_sort();
    entity.disable_reverse();
}

void coarsely_order_printing_extrusion(
    const PrintingExtrusion extrusion,
    EntryPointProperty &entry_points,
    const c_point start_position,
    const OrderingEngine &ordering_engine,
    const SeamPlacer &seam_placer,
    storage_handle *scratch_storage)
{
    /*
    Keep this cache local to the independent PrintingExtrusion job. Later
    ordering phases will consume it before returning from this function, so no
    borrowed entity handle or seam estimate needs cross-thread ownership.
    */
    EntryExitEstimateCache estimates = build_entry_exit_estimates(
        extrusion.root(), seam_placer);
    MutableExtrusionEntity root = extrusion.mutable_root();
    coarsely_order_entity_descending(
        root, start_position, estimates, ordering_engine,
        seam_placer, scratch_storage);

    /* Publish only endpoints that now exist in the mutated root geometry. */
    entry_points.entry = root.front();
    entry_points.exit = root.back();
}

void coarsely_order_tool_group_job(
    const uint32_t job_idx,
    storage_handle *scratch_storage,
    const std::vector<ToolGroupOrderingJob> *jobs,
    const PluginPropertyKey<EntryPointProperty> *entry_point_key,
    const OrderingEngine *ordering_engine,
    const SeamPlacer *seam_placer)
{
    assert(jobs != nullptr);
    assert(entry_point_key != nullptr);
    assert(ordering_engine != nullptr);
    assert(seam_placer != nullptr);
    assert(job_idx < jobs->size());

    const ToolGroupOrderingJob &job = (*jobs)[job_idx];
    c_point current_position = job.start_position;

    /*
    PrintingExtrusionPreSort supplied this provisional vector order. This pass
    modifies only each owned root and its EntryPointProperty; the sequential
    barrier will subsequently reorder the roots inside this same tool visit.
    */
    for (uint32_t extrusion_idx = 0;
         extrusion_idx < job.tool_group.extrusion_count(); ++extrusion_idx) {
        const PrintingExtrusion extrusion =
            job.tool_group.extrusion(extrusion_idx);
        MutableExtrusionEntity root = extrusion.mutable_root();
        if (root.empty())
            continue;

        EntryPointProperty *entry_points = entry_point_key->get_mutable(root);
        if (entry_points == nullptr)
            throw std::runtime_error(
                "A non-empty PrintingExtrusion is missing the EntryPointProperty required by advanced ordering.");

        coarsely_order_printing_extrusion(
            extrusion, *entry_points, current_position,
            *ordering_engine, *seam_placer, scratch_storage);
        current_position = entry_points->exit;
    }
}

void finalize_printing_extrusion(
    const PrintingExtrusion extrusion,
    EntryPointProperty &entry_points,
    const c_point start_position,
    const OrderingEngine &ordering_engine,
    const SeamPlacer &seam_placer,
    storage_handle *scratch_storage)
{
    MutableExtrusionEntity root = extrusion.mutable_root();
    finalize_entity_descending(
        root, start_position, ordering_engine,
        seam_placer, scratch_storage);

    /* The barrier estimates are replaced by the completely fixed root ends. */
    entry_points.entry = root.front();
    entry_points.exit = root.back();
}

void finalize_tool_group_job(
    const uint32_t job_idx,
    storage_handle *scratch_storage,
    const std::vector<ToolGroupOrderingJob> *jobs,
    const PluginPropertyKey<EntryPointProperty> *entry_point_key,
    const OrderingEngine *ordering_engine,
    const SeamPlacer *seam_placer)
{
    assert(jobs != nullptr);
    assert(entry_point_key != nullptr);
    assert(ordering_engine != nullptr);
    assert(seam_placer != nullptr);
    assert(job_idx < jobs->size());

    const ToolGroupOrderingJob &job = (*jobs)[job_idx];
    c_point current_position = job.start_position;

    /*
    The sequential barrier has fixed this vector order. Exact exits are now
    propagated inside the job, where no other worker can access these roots.
    */
    for (uint32_t extrusion_idx = 0;
         extrusion_idx < job.tool_group.extrusion_count(); ++extrusion_idx) {
        const PrintingExtrusion extrusion =
            job.tool_group.extrusion(extrusion_idx);
        MutableExtrusionEntity root = extrusion.mutable_root();
        if (root.empty())
            continue;

        EntryPointProperty *entry_points = entry_point_key->get_mutable(root);
        if (entry_points == nullptr)
            throw std::runtime_error(
                "A non-empty PrintingExtrusion lost its EntryPointProperty before final ordering.");

        finalize_printing_extrusion(
            extrusion, *entry_points, current_position,
            *ordering_engine, *seam_placer, scratch_storage);
        current_position = entry_points->exit;
    }
}

class AdvancedExtrusionTreeOrdering final : public PluginBase
{
public:
    static AdvancedExtrusionTreeOrdering &instance(orchestrator_handle *orchestrator)
    {
        static AdvancedExtrusionTreeOrdering instance(orchestrator);
        return instance;
    }

    explicit AdvancedExtrusionTreeOrdering(orchestrator_handle *orchestrator) :
        PluginBase(orchestrator),
        m_entry_point_key(entry_point_property_key(orchestrator))
    {
    }

private:
    const char *id_impl() const noexcept override
    {
        return "ordering.extrusion_tree.advanced";
    }
    const char *name_impl() const noexcept override
    {
        return "Advanced extrusion-tree ordering";
    }
    const char *description_impl() const noexcept override
    {
        return "Orders PrintingExtrusion trees with parallel seam-aware refinement.";
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
    int32_t priority_impl() const noexcept override { return 1050; }
    const char *progress_message_format_impl() const noexcept override
    {
        return "Preparing seam-aware extrusion ordering: %u / %u tool groups";
    }

    void run_impl(const plugin_run_context *run_ctx) const override
    {
        const run_ctx_extrusion_ordering *ctx =
            plugin_ctx_as_extrusion_ordering(run_ctx);
        if (ctx == nullptr || ctx->print == nullptr || ctx->plan == nullptr)
            throw std::invalid_argument(
                "Advanced extrusion-tree ordering needs a Print and PrintingPlan.");

        const Print print(ctx->print);
        const PrintingPlan plan(ctx->plan);
        SeamPlacer seam_placer = create_seam_placer(m_orchestrator, print, plan);
        const KDTreeOrderingEngine ordering_engine;
        const c_point print_start = initial_print_position(print);
        const std::vector<ToolGroupOrderingJob> coarse_jobs =
            make_tool_group_jobs(plan, print_start, m_entry_point_key);

        /*
        Each parallel helper is a barrier: it returns only after every worker
        has completed. The sequential vector ordering therefore observes all
        coarse properties before it constructs the final immutable job seeds.
        */
        progress().add_max(uint32_t(coarse_jobs.size()) * 2u);
        parallel_for_storage_with_progress(
            0, uint32_t(coarse_jobs.size()), run_ctx, &progress(),
            coarsely_order_tool_group_job, &coarse_jobs, &m_entry_point_key,
            &ordering_engine, &seam_placer);

        const std::vector<ToolGroupOrderingJob> final_jobs =
            reorder_printing_extrusions_and_make_jobs(
                plan, print_start, ordering_engine, m_entry_point_key);
        if (final_jobs.size() != coarse_jobs.size())
            throw std::runtime_error(
                "The PrintingPlan tool-group count changed during advanced ordering.");

        parallel_for_storage_with_progress(
            0, uint32_t(final_jobs.size()), run_ctx, &progress(),
            finalize_tool_group_job, &final_jobs, &m_entry_point_key,
            &ordering_engine, &seam_placer);
    }

    PluginPropertyKey<EntryPointProperty> m_entry_point_key;
};

} // namespace

void register_advanced_extrusion_tree_ordering_plugin(
    orchestrator_handle *orchestrator)
{
    orchestrator_register_plugin(
        orchestrator,
        AdvancedExtrusionTreeOrdering::instance(orchestrator).c_instance());
}

}}} // namespace slic3r_api::Ordering::DefaultOrderingPlugin
