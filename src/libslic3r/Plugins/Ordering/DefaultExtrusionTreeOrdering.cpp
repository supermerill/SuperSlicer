///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "DefaultOrdering.hpp"

#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "libslic3r/Api/plugin/c/steps/slic3r_step_ordering.h"
#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"
#include "libslic3r/Api/plugin/cpp/PrintingPlanViews.hpp"

namespace slic3r_api { namespace Ordering { namespace DefaultOrderingPlugin {
namespace {

/*
DefaultExtrusionTreeOrdering is the third plugin in the ordering chain.

The previous plugins have already selected the high-level order of groups,
layers and tools. This plugin walks the cloned extrusion roots stored in the
PrintingPlan and fixes only sortable nodes inside those roots. Non-sortable
subtrees remain atomic, so properties and forced sequencing carried by plugin
wrappers stay attached to their original subtree.

Important tree rules:
  - only nodes marked sortable may reorder their direct children;
  - open reversible paths may be reversed to reduce travel;
  - loops are rotated to a stored vertex, never split at an arbitrary point;
  - once a sortable node is ordered, it is marked non-sortable/non-reversible so
    downstream code sees a fixed printing sequence.
*/

const char *k_no_dependencies[] = { nullptr };
const char *k_group_extrusions = "ordering.extrusion_tree";

struct ExtrusionEntryCandidate
{
    c_point point = {};
    double score = (std::numeric_limits<double>::max)();
    bool reverse_before_printing = false;
    bool valid = false;
};

/*
Order the direct children of one sortable node from the given current point.

This is the core algorithm of the plugin. It greedily selects the child that
can start closest to the current nozzle position, prepares that child to really
start there, and moves it into the fixed prefix of the same parent.
*/
void order_sortable_extrusion_children(storage_handle *storage, MutableExtrusionEntity entity, c_point start_near);

/* Prepare one selected child before it is considered fixed in its parent. */
void prepare_child_for_entry(storage_handle *storage,
                             MutableExtrusionEntity child,
                             const ExtrusionEntryCandidate &candidate);

/*
Find the best legal start point a child can present to a sortable parent.

Open reversible paths may start from either end. Loops may start from any stored
vertex. Sortable children expose the best candidate of their descendants because
they will be ordered recursively after they are selected.
*/
ExtrusionEntryCandidate best_entry_candidate(const ExtrusionEntity &entity, c_point current_point);

/* Rotate a loop-like subtree so the selected existing vertex becomes its front. */
void rotate_loop_to_point(storage_handle *storage, MutableExtrusionEntity entity, c_point entry_point);

/*
Collect loop entry candidates from the stored vertices of a loop tree.

The plugin intentionally does not split paths at projected closest points. It
uses existing vertices only, which keeps arc and Z-offset data aligned with the
polyline API.
*/
void collect_loop_entry_points(const ExtrusionEntity &entity, std::vector<c_point> &points);

/* Return true when entity contains point as an exact stored vertex. */
bool entity_contains_point(const ExtrusionEntity &entity, c_point point);

/* Rotate a closed local polyline so point_idx becomes the first point. */
void rotate_local_loop_to_index(MutableExtrusionEntity entity, uint32_t point_idx);

/* Return the exact local point index, or EXTRUSION_INDEX_INVALID. */
uint32_t local_point_index(const ExtrusionEntity &entity, c_point point);

/* Return true when this tree currently behaves as one closed printable loop. */
bool entity_is_loop(const ExtrusionEntity &entity);

/* Squared distance is enough for nearest-candidate comparison and avoids sqrt. */
double distance_to_square(c_point lhs, c_point rhs);

void order_sortable_extrusion_children(storage_handle *storage, MutableExtrusionEntity entity, c_point start_near)
{
    c_point current_point = start_near;
    uint32_t fixed_count = 0;

    while (fixed_count < entity.child_count()) {
        /*
        Children before fixed_count are already ordered and must not move again.
        Children from fixed_count to the end are candidates for the next printed
        subtree.
        */
        uint32_t selected_idx = EXTRUSION_INDEX_INVALID;
        ExtrusionEntryCandidate selected_candidate;

        for (uint32_t child_idx = fixed_count; child_idx < entity.child_count(); ++child_idx) {
            const ExtrusionEntryCandidate candidate =
                best_entry_candidate(entity.child(child_idx), current_point);
            if (!candidate.valid)
                continue;
            if (selected_idx == EXTRUSION_INDEX_INVALID || candidate.score < selected_candidate.score) {
                selected_idx = child_idx;
                selected_candidate = candidate;
            }
        }

        if (selected_idx == EXTRUSION_INDEX_INVALID)
            break;

        /*
        Move the selected child into the fixed prefix before mutating it. That
        way recursive ordering or loop rotation works on the child at its final
        parent index.
        */
        const uint32_t moved_idx = entity.move_child_from(fixed_count, entity, selected_idx);
        assert(moved_idx != EXTRUSION_INDEX_INVALID);
        MutableExtrusionEntity child = entity.child_mutable(fixed_count);

        prepare_child_for_entry(storage, child, selected_candidate);
        if (!child.empty())
            current_point = child.back();
        ++fixed_count;
    }

    /*
    This node now contains a concrete printing sequence. Disable later automatic
    sorting/reversing so downstream code observes the order selected here.
    */
    entity.disable_sort();
    entity.disable_reverse();
}

void prepare_child_for_entry(storage_handle *storage,
                             MutableExtrusionEntity child,
                             const ExtrusionEntryCandidate &candidate)
{
    assert(candidate.valid);

    if (child.readonly().sortable()) {
        /*
        A sortable child can choose its own first descendant. Recursing here
        turns the child subtree into a fixed sequence starting from the point
        chosen by the parent.
        */
        order_sortable_extrusion_children(storage, child, candidate.point);
        return;
    }

    /*
    Non-sortable children are atomic. The only legal preparation is changing
    their entry point without changing their internal ordering: rotate a loop or
    reverse an explicitly reversible open path.
    */
    if (entity_is_loop(child.readonly()))
        rotate_loop_to_point(storage, child, candidate.point);
    else if (candidate.reverse_before_printing)
        child.reverse();
}

ExtrusionEntryCandidate best_entry_candidate(const ExtrusionEntity &entity, c_point current_point)
{
    ExtrusionEntryCandidate best;
    if (entity.empty())
        return best;

    if (entity.sortable()) {
        /*
        A sortable child does not have a fixed first point yet. Ask its children
        what entry point they could present after recursive ordering, and report
        the best of those possibilities to this parent.
        */
        for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx) {
            ExtrusionEntryCandidate candidate = best_entry_candidate(entity.child(child_idx), current_point);
            if (candidate.valid && (!best.valid || candidate.score < best.score)) {
                candidate.reverse_before_printing = false;
                best = candidate;
            }
        }
        return best;
    }

    if (entity_is_loop(entity)) {
        std::vector<c_point> candidates;
        collect_loop_entry_points(entity, candidates);
        if (candidates.empty())
            candidates.push_back(entity.front());

        /*
        Loop entry is chosen from existing vertices only. This keeps the
        operation cheap and avoids creating new arc/z-offset split points that
        the current ABI cannot fully describe.
        */
        for (c_point point : candidates) {
            ExtrusionEntryCandidate candidate;
            candidate.point = point;
            candidate.score = distance_to_square(point, current_point);
            candidate.valid = true;
            if (!best.valid || candidate.score < best.score)
                best = candidate;
        }
        return best;
    }

    /*
    Open paths normally start at front(). If the whole entity is reversible,
    back() is also a legal start, and prepare_child_for_entry() will reverse it
    only if that candidate wins.
    */
    best.point = entity.front();
    best.score = distance_to_square(best.point, current_point);
    best.valid = true;

    if (entity.reversible()) {
        ExtrusionEntryCandidate reversed;
        reversed.point = entity.back();
        reversed.score = distance_to_square(reversed.point, current_point);
        reversed.reverse_before_printing = true;
        reversed.valid = true;
        if (reversed.score < best.score)
            best = reversed;
    }

    return best;
}

void rotate_loop_to_point(storage_handle *storage, MutableExtrusionEntity entity, c_point entry_point)
{
    if (entity.point_count() > 0) {
        /*
        Leaf loop: the selected entry point is a local vertex, so rotating the
        local segment array is enough. No child order changes are needed.
        */
        const uint32_t point_idx = local_point_index(entity.readonly(), entry_point);
        if (point_idx != EXTRUSION_INDEX_INVALID)
            rotate_local_loop_to_index(entity, point_idx);
        return;
    }

    if (entity.child_count() == 0)
        return;

    uint32_t containing_idx = EXTRUSION_INDEX_INVALID;
    for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        if (entity_contains_point(entity.child(child_idx), entry_point)) {
            containing_idx = child_idx;
            break;
        }

    if (containing_idx == EXTRUSION_INDEX_INVALID)
        return;

    /*
    Composed loop: find the child that owns the chosen vertex. The parent loop
    will be rotated by moving children before that child to the end.
    */
    uint32_t start_idx = containing_idx;
    std::unique_ptr<StoredExtrusionEntity> closing_piece;
    MutableExtrusionEntity child = entity.child_mutable(containing_idx);

    if (child.point_count() > 0) {
        const uint32_t point_idx = local_point_index(child.readonly(), entry_point);
        if (point_idx == EXTRUSION_INDEX_INVALID) {
            assert(false);
        } else if (point_idx == 0) {
            start_idx = containing_idx;
        } else if (point_idx == child.point_count() - 1) {
            /*
            The end of this child is also the beginning of the next child in a
            continuous loop. Starting at the next child avoids creating a
            zero-length closing fragment.
            */
            start_idx = (containing_idx + 1) % entity.child_count();
        } else {
            /*
            The selected vertex lies in the middle of one child path. Split that
            child, print the tail first, and append the head as a closing piece
            after the rest of the loop has been printed.
            */
            StoredExtrusionEntity before_entry(storage, child.readonly());
            StoredExtrusionEntity after_entry(storage, child.readonly());
            const int32_t split_ok = extrusion_polyline_split_at_index(
                child.handle(), point_idx, before_entry.mutable_handle(), after_entry.mutable_handle());
            assert(split_ok != 0);
            if (split_ok != 0) {
                const int32_t move_ok = extrusion_move_from(child.mutable_handle(), after_entry.mutable_handle());
                assert(move_ok != 0);
                (void) move_ok;
                closing_piece = std::make_unique<StoredExtrusionEntity>(std::move(before_entry));
            }
            start_idx = containing_idx;
        }
    } else {
        /*
        The chosen vertex is inside a child collection. Rotate that collection
        first so it starts at the right point, then rotate this parent around
        the now-prepared child.
        */
        rotate_loop_to_point(storage, child, entry_point);
        start_idx = containing_idx;
    }

    /*
    Move the prefix before start_idx to the end. Because we always move child 0
    to the current end, the next original prefix child becomes child 0 and the
    loop rotates without needing random-access extraction.
    */
    for (uint32_t move_idx = 0; move_idx < start_idx; ++move_idx) {
        const uint32_t moved_idx = entity.move_child_from(entity.child_count(), entity, 0);
        assert(moved_idx != EXTRUSION_INDEX_INVALID);
        (void) moved_idx;
    }

    if (closing_piece) {
        /*
        A mid-child split created a head fragment that must close the loop after
        all rotated children have printed. Move that stored fragment into the
        parent as the final child.
        */
        const uint32_t appended_idx = entity.append_child_move(closing_piece->mutable_view());
        assert(appended_idx != EXTRUSION_INDEX_INVALID);
        (void) appended_idx;
    }
}

void collect_loop_entry_points(const ExtrusionEntity &entity, std::vector<c_point> &points)
{
    if (entity.point_count() > 0) {
        const bool skip_duplicate_last =
            entity.point_count() > 1 && points_equal(entity.local_front(), entity.local_back());
        const uint32_t point_count = skip_duplicate_last ? entity.point_count() - 1 : entity.point_count();
        /*
        Closed polylines usually repeat the first vertex as the last vertex.
        Exposing both as candidates would give two equivalent starts with
        different split behavior, so the duplicate last point is skipped.
        */
        for (uint32_t point_idx = 0; point_idx < point_count; ++point_idx)
            points.push_back(entity.point(point_idx));
        return;
    }

    // Collection loop: candidates are the stored vertices of every child path.
    for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        collect_loop_entry_points(entity.child(child_idx), points);
}

bool entity_contains_point(const ExtrusionEntity &entity, c_point point)
{
    if (entity.point_count() > 0)
        return local_point_index(entity, point) != EXTRUSION_INDEX_INVALID;

    for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        if (entity_contains_point(entity.child(child_idx), point))
            return true;
    return false;
}

void rotate_local_loop_to_index(MutableExtrusionEntity entity, const uint32_t point_idx)
{
    if (point_idx == 0 || point_idx >= entity.point_count())
        return;
    if (points_equal(entity.local_front(), entity.local_back()) && point_idx == entity.point_count() - 1)
        return;

    const uint32_t segment_count = entity.segment_count();
    if (point_idx >= segment_count)
        return;

    /*
    Rebuilding from segments preserves arc radius/orientation and per-point
    Z-offsets. Rebuilding from points would silently flatten that extra geometry.
    */
    std::vector<c_extrusion_segment> rotated_segments;
    rotated_segments.reserve(segment_count);
    /*
    The segment array is rotated exactly like a ring buffer: tail first, head
    second. The resulting local front point is the selected entry point.
    */
    for (uint32_t segment_idx = point_idx; segment_idx < segment_count; ++segment_idx)
        rotated_segments.push_back(entity.segment(segment_idx));
    for (uint32_t segment_idx = 0; segment_idx < point_idx; ++segment_idx)
        rotated_segments.push_back(entity.segment(segment_idx));

    const bool ok = entity.set_segments(rotated_segments);
    assert(ok);
    (void) ok;
}

uint32_t local_point_index(const ExtrusionEntity &entity, c_point point)
{
    for (uint32_t point_idx = 0; point_idx < entity.point_count(); ++point_idx)
        if (points_equal(entity.point(point_idx), point))
            return point_idx;
    return EXTRUSION_INDEX_INVALID;
}

bool entity_is_loop(const ExtrusionEntity &entity)
{
    return !entity.empty() && !entity.sortable() && entity.continuous() && entity.is_closed();
}

double distance_to_square(c_point lhs, c_point rhs)
{
    const double dx = double(lhs.x) - double(rhs.x);
    const double dy = double(lhs.y) - double(rhs.y);
    return dx * dx + dy * dy;
}

class DefaultExtrusionTreeOrdering : public PluginBase
{
public:
    static DefaultExtrusionTreeOrdering &instance(orchestrator_handle *orch)
    {
        static DefaultExtrusionTreeOrdering s_instance(orch);
        return s_instance;
    }

    explicit DefaultExtrusionTreeOrdering(orchestrator_handle *orch) : PluginBase(orch) {}

private:
    const char *id_impl() const noexcept override { return "ordering.extrusion_tree.default"; }
    const char *name_impl() const noexcept override { return "Default extrusion-tree ordering"; }
    const char *description_impl() const noexcept override
    {
        return "Orders sortable cloned extrusion trees in the PrintingPlan while preserving source trees.";
    }
    const char *exclusive_group_impl() const noexcept override { return k_group_extrusions; }
    const char *exclusive_group_label_impl() const noexcept override { return "Extrusion-tree ordering"; }
    const char *exclusive_group_tooltip_impl() const noexcept override
    {
        return "Selects how STEP_ORDERING orders extrusion trees inside each tool section.";
    }
    slicing_step_t step_impl() const noexcept override { return STEP_ORDERING; }
    const char *const *dependencies_impl() const noexcept override { return k_no_dependencies; }
    int32_t priority_impl() const noexcept override { return 200; }

    void run_impl(const plugin_run_context *run_ctx) const override
    {
        const run_ctx_extrusion_ordering *ctx = plugin_ctx_as_extrusion_ordering(run_ctx);
        assert(ctx != nullptr);
        assert(ctx->plan != nullptr);
        if (ctx == nullptr || ctx->plan == nullptr)
            return;

        assert(run_ctx != nullptr);
        assert(run_ctx->plugin_storage != nullptr);
        if (run_ctx == nullptr || run_ctx->plugin_storage == nullptr)
            return;

        PrintingPlan plan(ctx->plan);
        for (uint32_t group_idx = 0; group_idx < plan.group_count(); ++group_idx) {
            const PrintingGroup group = plan.group(group_idx);
            c_point current_point = {};

            /*
            The plan already defines the high-level order: group, layer, tool,
            then source extrusion bucket. This plugin only fixes sortable
            extrusion trees inside that order. The last point of one root is
            passed to the next root so local child sorting can reduce the next
            travel without flattening the source tree.
            */
            for (uint32_t layer_idx = 0; layer_idx < group.layer_group_count(); ++layer_idx) {
                const PrintingLayerGroup layer_group = group.layer_group(layer_idx);
                for (uint32_t tool_idx = 0; tool_idx < layer_group.tool_group_count(); ++tool_idx) {
                    const PrintingToolGroup tool_group = layer_group.tool_group(tool_idx);
                    for (uint32_t extrusion_idx = 0; extrusion_idx < tool_group.extrusion_count(); ++extrusion_idx) {
                        const PrintingExtrusion extrusion = tool_group.extrusion(extrusion_idx);
                        MutableExtrusionEntity root = extrusion.mutable_root();
                        /*
                        Only roots marked sortable are changed. Non-sortable
                        roots may still be printed after a previous root, so
                        their back point still updates current_point.
                        */
                        if (root.readonly().sortable())
                            order_sortable_extrusion_children(run_ctx->plugin_storage, root, current_point);
                        if (!root.empty())
                            current_point = root.back();
                    }
                }
            }
        }
    }
};

} // namespace

void register_default_extrusion_tree_ordering_plugin(orchestrator_handle *orch)
{
    orchestrator_register_plugin(orch, DefaultExtrusionTreeOrdering::instance(orch).c_instance());
}

}}} // namespace slic3r_api::Ordering::DefaultOrderingPlugin
