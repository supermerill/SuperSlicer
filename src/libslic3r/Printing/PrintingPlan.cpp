///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "PrintingPlan.hpp"

#include <algorithm>
#include <cassert>
#include <limits>
#include <map>
#include <memory>
#include <utility>

#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/LayerRegion.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintObject.hpp"
#include "libslic3r/ShortestPath.hpp"

namespace Slic3r::Printing {
namespace {

struct ToolOrderCandidate
{
    uint16_t first_extruder = uint16_t(-1);
    uint16_t last_extruder = uint16_t(-1);
    bool valid = false;
};

struct ToolOrderScore
{
    unsigned int tool_changes = (std::numeric_limits<unsigned int>::max)();
    unsigned int initial_penalty = (std::numeric_limits<unsigned int>::max)();
};

struct ExtrusionEntryCandidate
{
    Point point;
    double score = (std::numeric_limits<double>::max)();
    std::shared_ptr<const LoopEntryAnalysis> loop_analysis;
    size_t loop_candidate_idx = size_t(-1);
    bool reverse_before_printing = false;
    bool valid = false;
};

/*
Default analysis used when no advanced loop-entry policy is supplied.

It stores only the candidate points because the default behavior needs no richer
state. More advanced policies can implement their own LoopEntryAnalysis with a
different internal representation while keeping the same ordering contract.
*/
class DefaultLoopEntryAnalysis final : public LoopEntryAnalysis
{
public:
    explicit DefaultLoopEntryAnalysis(const ExtrusionEntity &loop_root);

    size_t candidate_count() const override { return m_points.size(); }
    Point candidate_point(size_t candidate_idx) const override;
    double score_candidate(size_t candidate_idx, const Point &start_near) const override;
    void rotate_loop_to_candidate(ExtrusionEntity &loop_root, size_t candidate_idx) const override;

private:
    Points m_points;
};

/*
PrintingPlan.cpp builds the first work copy used by STEP_ORDERING.

The normal slicer data tree remains the source of truth. A PrintingPlan stores
non-owning pointers back to Layers and LayerRegionIslands for context, but each
PrintingExtrusion owns a cloned extrusion root. The clone is translated by the
object instance shift while the plan is built. Code that later reads points from
the plan therefore sees platter coordinates and must not apply the instance
shift a second time.

The builders are intentionally conservative:
  - build_printing_plan_by_layer() creates one PrintingGroup for the whole print;
  - build_printing_plan_by_object() creates one PrintingGroup per object instance,
    ordered by complete_objects_sort;
  - both builders group object layers by scaled print Z;
  - both builders group extrusion roots by the LayerRegionIsland extruder id;
  - they do not try to optimize tool switches or local travel order yet.
*/

/*
Return the role buckets that can currently be stored on a LayerRegionIsland.

The order is only a construction order. Dedicated ordering code may later move
tool groups and extrusion roots according to print settings such as
infill-before-perimeters.
*/
std::vector<ExtrusionRole> layer_region_island_roles();

/*
Choose one first/last extruder pair for every PrintingLayerGroup.

The dynamic program minimizes tool changes between consecutive printable
layers. It does not inspect extrusion geometry. A layer with no tool groups is
kept in the output but receives an invalid candidate and is ignored by the
transition cost.
*/
std::vector<ToolOrderCandidate> select_tool_order_candidates(const PrintingGroup &printing_group,
                                                             uint16_t first_extruder);

/*
Return the unique extruder ids of a layer in their current order.

Multiple PrintingToolGroups may have the same extruder id when a previous step
split that tool into several visits. The ordering decision only needs the id
once; apply_tool_order_candidate() later moves all matching groups together
while preserving their relative order.
*/
std::vector<uint16_t> unique_layer_extruders(const PrintingLayerGroup &layer_group);

/*
Generate all valid first/last choices for one printable layer.

For a single extruder, first and last are the same. For several extruders, the
candidate order is deterministic and stable: first follows the current layer
order, while last is visited from the current end toward the beginning so a
fixed first extruder keeps the rest of the layer as close as possible to the
existing order when there is no transition reason to choose another last tool.
*/
std::vector<ToolOrderCandidate> tool_order_candidates_for_extruders(const std::vector<uint16_t> &extruders);

/*
Return true when lhs is a better DP score than rhs.

The primary goal is minimizing inter-layer tool changes. The initial penalty is
only a tie-break for the first printable layer, so an explicitly requested first
extruder does not override a lower total number of tool changes.
*/
bool tool_order_score_less(const ToolOrderScore &lhs, const ToolOrderScore &rhs);

/*
Move PrintingToolGroups in place to match the selected first/last pair.

All groups using the first extruder are moved first, all groups using the last
extruder are moved last, and the middle groups keep their current order. Groups
with the same extruder id are never reordered relative to each other.
*/
void apply_tool_order_candidate(PrintingLayerGroup &layer_group, const ToolOrderCandidate &candidate);

/*
Order one sortable extrusion node from a known current nozzle position.

The function moves the node children into a temporary vector, greedily chooses
the next child by nearest printable entry point, prepares that child so it
really starts at the chosen entry, and appends the children back in fixed order.
Only this node's direct children are permuted; non-sortable children remain
atomic.
*/
void order_sortable_extrusion_children(ExtrusionEntity &entity,
                                       const Point &start_near,
                                       const LoopEntryPolicy &loop_policy,
                                       const LoopEntryContext &context);

/*
Find the best entry point that entity could present to its parent.

Open paths expose their first point, and their last point when the whole entity
can be reversed. Loops expose every existing vertex because a loop can be
rotated without changing its geometry. Sortable children expose the best entry
of their own children, since they will be ordered recursively when selected.
*/
ExtrusionEntryCandidate best_entry_candidate(const ExtrusionEntity &entity,
                                             const Point &current_point,
                                             const LoopEntryPolicy &loop_policy,
                                             const LoopEntryContext &context);

/*
Add all existing vertices of a loop-like subtree to points.

The ordering pass deliberately uses only stored vertices. It does not split at a
projected foot point on a segment, which keeps arc/z-offset data under the same
constraints as the current ArcPolyline split API.
*/
void collect_loop_entry_points(const ExtrusionEntity &entity, Points &points);

/*
Prepare a selected child so its first printable point matches candidate.

This may reverse an open reversible path, rotate a loop to the selected vertex,
or recursively order a sortable child from the selected point.
*/
void prepare_child_for_entry(ExtrusionEntity &child,
                             const ExtrusionEntryCandidate &candidate,
                             const LoopEntryPolicy &loop_policy,
                             const LoopEntryContext &context);

/*
Rotate a loop-like entity to start at entry_point.

Leaf loops are rotated by splitting their ArcPolyline at an existing vertex.
Collection loops are rotated by moving the child containing that vertex to the
front; if the vertex is inside a child leaf, the child is split and the first
part is moved to the end of the collection.
*/
void rotate_loop_to_point(ExtrusionEntity &entity, const Point &entry_point);

/*
Rotate one closed ArcPolyline so point_idx becomes the new first point.

The polyline must already be a loop or be used as a loop segment by its parent.
The split keeps arc and z-offset bookkeeping inside ArcPolyline.
*/
void rotate_polyline_to_index(ArcPolyline &polyline, size_t point_idx);

/*
Return the first exact vertex index matching point, or size_t(-1).

The ordering algorithm only chooses existing vertices, so exact point equality
is the right lookup here. Tolerant geometric splitting belongs to a later, more
expensive travel optimizer.
*/
size_t find_polyline_point_index(const ArcPolyline &polyline, const Point &point);

/*
Return true when entity contains point as an existing vertex.

This is used to rotate composed loops: the parent first finds which direct child
contains the selected vertex, then rotates the child list around that child.
*/
bool entity_contains_point(const ExtrusionEntity &entity, const Point &point);

/*
Move all children out of entity while preserving the concrete node object.

ExtrusionEntityCollection has an entity cache, so collections must be drained
through release(). Plain ExtrusionEntity nodes can move their child vector
directly because they have no additional cache.
*/
ExtrusionEntity::Children release_all_children(ExtrusionEntity &entity);

/*
Append one owned child to entity and refresh any collection cache.

This is the inverse operation of release_all_children() and keeps collection
nodes consistent after ordering or loop rotation.
*/
void append_owned_child(ExtrusionEntity &entity, ExtrusionEntityUPtr &&child);

/*
Return the object instances in the order requested by complete_objects_sort.

The by-object plan prints one PrintingGroup per object instance, so the group
order is the object-completion order. cosNearest delegates to the existing
nearest-neighbor helper, which uses PrintInstance::shift as the instance center
in G-code coordinates.
*/
std::vector<const PrintInstance *> ordered_print_instances_for_object_plan(const Print &print);

/*
Return the index of instance inside object.instances().

The legacy ordering helpers return PrintInstance pointers. PrintingPlan keeps
the index as context, so the builder converts the pointer back to the index
owned by its PrintObject before cloning the extrusion roots.
*/
size_t print_instance_index(const PrintObject &object, const PrintInstance &instance);

/*
Return the PrintingLayerGroup for print_z, creating it when this is the first
Layer found at that Z.

The map lets us append layers while walking objects and instances in source
order, then sort the final group vector only once after construction.
*/
PrintingLayerGroup &layer_group_for_print_z(PrintingGroup &group,
                                            std::map<coord_t, size_t> &layer_index_by_print_z,
                                            coord_t print_z);

/*
Add a Layer pointer to a PrintingLayerGroup once.

A layer group may receive extrusion roots from several object instances. The
Layer pointer describes the source layer, not each duplicated instance copy, so
duplicates would only make later readers do repeated source-tree work.
*/
void append_layer_once(PrintingLayerGroup &layer_group, const Layer &layer);

/*
Return the PrintingToolGroup for extruder_id, creating it if needed.

Tool groups are local to one PrintingLayerGroup: two layers at different Z may
use the same extruder id but still need separate ordered extrusion lists.
*/
PrintingToolGroup &tool_group_for_extruder(PrintingLayerGroup &layer_group, uint16_t extruder_id);

/*
Store the source LayerRegionIsland pointer once inside a tool group.

The actual printable copies live in PrintingToolGroup::extrusions. This source
list is only a compact context list for algorithms that need to inspect all
LayerRegionIslands participating in a tool group.
*/
void append_region_island_once(PrintingToolGroup &tool_group, const LayerRegionIsland &region_island);

/*
Append all extrusion roots from one LayerRegionIsland for one object instance.

Each root is cloned, translated by the instance shift, tagged with the source
role, and stored under the tool group selected by region_island.extruder_id().
*/
void append_region_island_instance_extrusions(PrintingLayerGroup &layer_group,
                                              const LayerRegionIsland &region_island,
                                              const PrintInstance &instance,
                                              size_t instance_idx,
                                              const std::vector<ExtrusionRole> &roles);

/*
Translate every geometric point stored in an extrusion tree.

The source extrusion tree stays untouched. This operates only on the cloned
tree that belongs to the PrintingPlan. Empty placeholders keep their sentinel
position unchanged to avoid overflowing the special NOT_A_POINT value.
*/
void translate_extrusion_tree(ExtrusionEntity &entity, const Point &shift);

/*
Sort the layer groups by increasing print Z after all objects have contributed.

The builder discovers layers object-by-object. Sorting at the end makes the
final plan match the normal by-layer printing order.
*/
void sort_layer_groups_by_print_z(PrintingGroup &group);

} // namespace

PrintingScopeEvents::PrintingScopeEvents()
    : m_before(ExtrusionEntity::Children(), false, false, true)
    , m_after(ExtrusionEntity::Children(), false, false, true)
{
}

PrintingScopeEvents::PrintingScopeEvents(PrintingScopeEvents &&other) noexcept
    : m_before(std::move(other.m_before))
    , m_after(std::move(other.m_after))
{
}

PrintingScopeEvents &PrintingScopeEvents::operator=(PrintingScopeEvents &&other) noexcept
{
    if (this != &other) {
        m_before = std::move(other.m_before);
        m_after = std::move(other.m_after);
    }
    return *this;
}

ExtrusionEntity &PrintingScopeEvents::append_before(const ExtrusionEntity &event)
{
    // Cloning lets the caller keep its source tree while the scope owns an
    // independent event at the end of the fixed before sequence.
    return m_before.append_child(event);
}

ExtrusionEntity &PrintingScopeEvents::append_before(ExtrusionEntity &&event)
{
    // Moving transfers the prepared event content into the scope without
    // changing the identity or ordering contract of the before root.
    return m_before.append_child(std::move(event));
}

ExtrusionEntity &PrintingScopeEvents::append_after(const ExtrusionEntity &event)
{
    // After events use the same ownership and stable insertion order as the
    // before sequence so every PrintingPlan scope follows one contract.
    return m_after.append_child(event);
}

ExtrusionEntity &PrintingScopeEvents::append_after(ExtrusionEntity &&event)
{
    // The moved-from source remains valid but empty, matching the extrusion
    // tree move semantics already exposed by the plugin API.
    return m_after.append_child(std::move(event));
}

void PrintingScopeEvents::clear()
{
    // Clearing content preserves the private roots and their immutable
    // non-sortable/non-reversible flags for the next step execution.
    m_before.clear_content();
    m_after.clear_content();
}

PrintingExtrusion::PrintingExtrusion(const LayerRegionIsland &source,
                                     ExtrusionRole extrusion_role,
                                     const ExtrusionEntity &source_root)
    : region_island(&source)
    , sregion_island_role(extrusion_role)
    , root(ExtrusionEntityUPtr(source_root.clone()))
{
}

void order_printing_tool_groups(PrintingGroup &printing_group, const uint16_t first_extruder)
{
    const std::vector<ToolOrderCandidate> selected_candidates =
        select_tool_order_candidates(printing_group, first_extruder);
    assert(selected_candidates.size() == printing_group.layers.size());

    for (size_t layer_idx = 0; layer_idx < printing_group.layers.size(); ++layer_idx)
        apply_tool_order_candidate(printing_group.layers[layer_idx], selected_candidates[layer_idx]);
}

std::unique_ptr<LoopEntryAnalysis> DefaultLoopEntryPolicy::analyze_loop(const ExtrusionEntity &loop_root,
                                                                        const LoopEntryContext &context) const
{
    (void)context;
    return std::make_unique<DefaultLoopEntryAnalysis>(loop_root);
}

void order_extrusion_tree(ExtrusionEntity &entity, const Point start_near)
{
    const DefaultLoopEntryPolicy default_policy;
    LoopEntryContext context;
    context.root = &entity;
    context.role = entity.role();
    order_extrusion_tree(entity, start_near, default_policy, context);
}

void order_extrusion_tree(ExtrusionEntity &entity,
                          const Point start_near,
                          const LoopEntryPolicy &loop_policy,
                          const LoopEntryContext &context)
{
    if (!entity.can_sort())
        return;

    LoopEntryContext effective_context = context;
    if (effective_context.root == nullptr)
        effective_context.root = &entity;
    if (effective_context.role == ExtrusionRole::None)
        effective_context.role = entity.role();

    order_sortable_extrusion_children(entity, start_near, loop_policy, effective_context);
}

void order_extrusion_tree(PrintingGroup &printing_group, const Point start_near)
{
    const DefaultLoopEntryPolicy default_policy;
    order_extrusion_tree(printing_group, start_near, default_policy);
}

void order_extrusion_tree(PrintingGroup &printing_group,
                          const Point start_near,
                          const LoopEntryPolicy &loop_policy)
{
    Point current_point = start_near;

    /*
    The higher-level plan order is already decided before local extrusion
    ordering runs. This loop therefore keeps groups, layers and tool sections in
    place, and only refines the copied root trees inside each PrintingExtrusion.
    The end point of one root becomes the start hint for the next root.
    */
    for (PrintingLayerGroup &layer_group : printing_group.layers)
        for (PrintingToolGroup &tool_group : layer_group.tool_groups)
            for (PrintingExtrusion &extrusion : tool_group.extrusions) {
                if (!extrusion.root)
                    continue;

                LoopEntryContext context;
                context.root = extrusion.root.get();
                context.region_island = extrusion.region_island;
                context.role = extrusion.sregion_island_role;

                order_extrusion_tree(*extrusion.root, current_point, loop_policy, context);
                if (!extrusion.root->empty())
                    current_point = extrusion.root->last_point();
            }
}

PrintingPlan build_printing_plan_by_object(const Print &print)
{
    PrintingPlan plan;
    const std::vector<ExtrusionRole> roles = layer_region_island_roles();
    const std::vector<const PrintInstance *> ordered_instances = ordered_print_instances_for_object_plan(print);

    for (const PrintInstance *instance : ordered_instances) {
        assert(instance != nullptr);
        assert(instance == nullptr || instance->print_object != nullptr);
        if (instance == nullptr || instance->print_object == nullptr)
            continue;

        const PrintObject &object = *instance->print_object;
        const size_t instance_idx = print_instance_index(object, *instance);
        PrintingGroup group;
        std::map<coord_t, size_t> layer_index_by_print_z;

        /*
        This builder isolates each object instance into its own PrintingGroup.
        The groups are created in complete_objects_sort order, so later code can
        consume plan.groups directly when printing complete objects.
        */
        PrintingObjectInstance object_instance;
        object_instance.object = &object;
        object_instance.instance_idx = instance_idx;
        group.object_instances.push_back(object_instance);

        for (const Layer &layer : object.layers()) {
            /*
            Even in the object-instance mode, a single object may still have
            multiple source Layer pointers at the same print Z once future
            features split work more finely. Keep the same grouping rule as the
            by-layer builder so both plans expose a comparable shape.
            */
            PrintingLayerGroup &layer_group =
                layer_group_for_print_z(group, layer_index_by_print_z, layer.scaled_print_z());
            append_layer_once(layer_group, layer);

            /*
            Only the current instance is duplicated into this group. The source
            LayerRegionIsland pointer remains shared context, but the owned
            extrusion root is a per-instance clone in platter coordinates.
            */
            for (const LayerSliceIsland &slice_island : layer.islands())
                for (const LayerRegionIsland &region_island : slice_island.regions_islands())
                    append_region_island_instance_extrusions(layer_group,
                                                             region_island,
                                                             *instance,
                                                             instance_idx,
                                                             roles);
        }

        sort_layer_groups_by_print_z(group);
        plan.groups.push_back(std::move(group));
    }

    return plan;
}

PrintingPlan build_printing_plan_by_layer(const Print &print)
{
    PrintingPlan plan;
    PrintingGroup group;
    std::map<coord_t, size_t> layer_index_by_print_z;
    const std::vector<ExtrusionRole> roles = layer_region_island_roles();

    for (const PrintObject &object : print.objects()) {
        /*
        The by-layer plan is a single global print batch, but it still records
        every object instance that contributes geometry to that batch. The
        extrusion clones are created below; this list is only the high-level
        context for the group.
        */
        for (size_t instance_idx = 0; instance_idx < object.instances().size(); ++instance_idx) {
            PrintingObjectInstance object_instance;
            object_instance.object = &object;
            object_instance.instance_idx = instance_idx;
            group.object_instances.push_back(object_instance);
        }

        for (const Layer &layer : object.layers()) {
            /*
            Several objects may have a layer at the same print Z. They must be
            merged into the same PrintingLayerGroup so later ordering can choose
            the tool and island order across the whole physical layer.
            */
            PrintingLayerGroup &layer_group =
                layer_group_for_print_z(group, layer_index_by_print_z, layer.scaled_print_z());
            append_layer_once(layer_group, layer);

            /*
            A source LayerRegionIsland contains extrusion trees in object-local
            coordinates. The plan needs one shifted clone per object instance, so
            the same source island is duplicated once for each instance here.
            */
            for (const LayerSliceIsland &slice_island : layer.islands())
                for (const LayerRegionIsland &region_island : slice_island.regions_islands())
                    for (size_t instance_idx = 0; instance_idx < object.instances().size(); ++instance_idx)
                        append_region_island_instance_extrusions(layer_group,
                                                                 region_island,
                                                                 object.instances()[instance_idx],
                                                                 instance_idx,
                                                                 roles);
        }
    }

    /*
    The construction walk follows object order, not print-Z order. Sorting once
    here keeps the builder simple while making the returned plan match the
    expected by-layer printing sequence.
    */
    sort_layer_groups_by_print_z(group);
    plan.groups.push_back(std::move(group));
    return plan;
}

namespace {

DefaultLoopEntryAnalysis::DefaultLoopEntryAnalysis(const ExtrusionEntity &loop_root)
{
    collect_loop_entry_points(loop_root, m_points);
    if (m_points.empty() && !loop_root.empty())
        m_points.push_back(loop_root.first_point());
}

Point DefaultLoopEntryAnalysis::candidate_point(const size_t candidate_idx) const
{
    assert(candidate_idx < m_points.size());
    return candidate_idx < m_points.size() ? m_points[candidate_idx] : Point();
}

double DefaultLoopEntryAnalysis::score_candidate(const size_t candidate_idx, const Point &start_near) const
{
    assert(candidate_idx < m_points.size());
    return candidate_idx < m_points.size() ? start_near.distance_to_square(m_points[candidate_idx]) :
                                             (std::numeric_limits<double>::max)();
}

void DefaultLoopEntryAnalysis::rotate_loop_to_candidate(ExtrusionEntity &loop_root,
                                                        const size_t candidate_idx) const
{
    assert(candidate_idx < m_points.size());
    if (candidate_idx < m_points.size())
        rotate_loop_to_point(loop_root, m_points[candidate_idx]);
}

std::vector<ExtrusionRole> layer_region_island_roles()
{
    std::vector<ExtrusionRole> roles;
    roles.reserve(7);
    roles.push_back(LayerRegionIsland::SUPPORT);
    roles.push_back(LayerRegionIsland::SUPPORT_INTERFACE);
    roles.push_back(LayerRegionIsland::PERIMETERS);
    roles.push_back(LayerRegionIsland::GAP_FILLS);
    roles.push_back(LayerRegionIsland::INFILLS);
    roles.push_back(LayerRegionIsland::IRONINGS);
    roles.push_back(LayerRegionIsland::MILLS);
    return roles;
}

std::vector<ToolOrderCandidate> select_tool_order_candidates(const PrintingGroup &printing_group,
                                                             const uint16_t first_extruder)
{
    std::vector<ToolOrderCandidate> selected_candidates(printing_group.layers.size());
    std::vector<size_t> active_layer_indices;
    std::vector<std::vector<uint16_t>> active_layer_extruders;
    std::vector<std::vector<ToolOrderCandidate>> active_layer_candidates;

    /*
    Compress away empty layers for the DP. Empty layers still remain in
    selected_candidates as invalid entries, but they do not create artificial
    tool changes between the surrounding printable layers.
    */
    for (size_t layer_idx = 0; layer_idx < printing_group.layers.size(); ++layer_idx) {
        std::vector<uint16_t> extruders = unique_layer_extruders(printing_group.layers[layer_idx]);
        if (extruders.empty())
            continue;

        active_layer_indices.push_back(layer_idx);
        active_layer_candidates.push_back(tool_order_candidates_for_extruders(extruders));
        active_layer_extruders.push_back(std::move(extruders));
    }

    if (active_layer_indices.empty())
        return selected_candidates;

    uint16_t preferred_first_extruder = first_extruder;
    if (preferred_first_extruder == uint16_t(-1)) {
        preferred_first_extruder = active_layer_extruders.front().front();
        for (const uint16_t extruder_id : active_layer_extruders.front())
            preferred_first_extruder = std::min(preferred_first_extruder, extruder_id);
    }

    std::vector<std::vector<size_t>> predecessors(active_layer_candidates.size());
    std::vector<ToolOrderScore> previous_scores(active_layer_candidates.front().size());
    for (size_t candidate_idx = 0; candidate_idx < active_layer_candidates.front().size(); ++candidate_idx) {
        const ToolOrderCandidate &candidate = active_layer_candidates.front()[candidate_idx];
        previous_scores[candidate_idx].tool_changes = 0;
        previous_scores[candidate_idx].initial_penalty =
            candidate.first_extruder == preferred_first_extruder ? 0 : 1;
    }

    for (size_t active_idx = 1; active_idx < active_layer_candidates.size(); ++active_idx) {
        const std::vector<ToolOrderCandidate> &previous_candidates = active_layer_candidates[active_idx - 1];
        const std::vector<ToolOrderCandidate> &current_candidates = active_layer_candidates[active_idx];
        predecessors[active_idx].assign(current_candidates.size(), size_t(-1));
        std::vector<ToolOrderScore> current_scores(current_candidates.size());

        for (size_t current_idx = 0; current_idx < current_candidates.size(); ++current_idx) {
            for (size_t previous_idx = 0; previous_idx < previous_candidates.size(); ++previous_idx) {
                ToolOrderScore candidate_score = previous_scores[previous_idx];
                if (candidate_score.tool_changes == (std::numeric_limits<unsigned int>::max)())
                    continue;

                if (previous_candidates[previous_idx].last_extruder != current_candidates[current_idx].first_extruder)
                    ++candidate_score.tool_changes;

                if (tool_order_score_less(candidate_score, current_scores[current_idx])) {
                    current_scores[current_idx] = candidate_score;
                    predecessors[active_idx][current_idx] = previous_idx;
                }
            }
        }

        previous_scores = std::move(current_scores);
    }

    size_t selected_idx = 0;
    for (size_t candidate_idx = 1; candidate_idx < previous_scores.size(); ++candidate_idx)
        if (tool_order_score_less(previous_scores[candidate_idx], previous_scores[selected_idx]))
            selected_idx = candidate_idx;

    for (size_t active_idx = active_layer_candidates.size(); active_idx-- > 0;) {
        const size_t layer_idx = active_layer_indices[active_idx];
        selected_candidates[layer_idx] = active_layer_candidates[active_idx][selected_idx];
        if (active_idx > 0) {
            assert(selected_idx < predecessors[active_idx].size());
            selected_idx = predecessors[active_idx][selected_idx];
            assert(selected_idx != size_t(-1));
        }
    }

    return selected_candidates;
}

std::vector<uint16_t> unique_layer_extruders(const PrintingLayerGroup &layer_group)
{
    std::vector<uint16_t> extruders;
    extruders.reserve(layer_group.tool_groups.size());
    for (const PrintingToolGroup &tool_group : layer_group.tool_groups)
        if (std::find(extruders.begin(), extruders.end(), tool_group.extruder_id) == extruders.end())
            extruders.push_back(tool_group.extruder_id);
    return extruders;
}

std::vector<ToolOrderCandidate> tool_order_candidates_for_extruders(const std::vector<uint16_t> &extruders)
{
    std::vector<ToolOrderCandidate> candidates;
    if (extruders.empty())
        return candidates;

    if (extruders.size() == 1) {
        ToolOrderCandidate candidate;
        candidate.first_extruder = extruders.front();
        candidate.last_extruder = extruders.front();
        candidate.valid = true;
        candidates.push_back(candidate);
        return candidates;
    }

    candidates.reserve(extruders.size() * (extruders.size() - 1));
    for (const uint16_t first : extruders) {
        for (std::vector<uint16_t>::const_reverse_iterator last_it = extruders.rbegin();
             last_it != extruders.rend();
             ++last_it) {
            if (first == *last_it)
                continue;

            ToolOrderCandidate candidate;
            candidate.first_extruder = first;
            candidate.last_extruder = *last_it;
            candidate.valid = true;
            candidates.push_back(candidate);
        }
    }

    return candidates;
}

bool tool_order_score_less(const ToolOrderScore &lhs, const ToolOrderScore &rhs)
{
    if (lhs.tool_changes != rhs.tool_changes)
        return lhs.tool_changes < rhs.tool_changes;
    return lhs.initial_penalty < rhs.initial_penalty;
}

void apply_tool_order_candidate(PrintingLayerGroup &layer_group, const ToolOrderCandidate &candidate)
{
    if (!candidate.valid || layer_group.tool_groups.size() < 2)
        return;

    std::vector<PrintingToolGroup> reordered;
    reordered.reserve(layer_group.tool_groups.size());

    for (PrintingToolGroup &tool_group : layer_group.tool_groups)
        if (tool_group.extruder_id == candidate.first_extruder)
            reordered.push_back(std::move(tool_group));

    if (candidate.first_extruder != candidate.last_extruder)
        for (PrintingToolGroup &tool_group : layer_group.tool_groups)
            if (tool_group.extruder_id != candidate.first_extruder &&
                tool_group.extruder_id != candidate.last_extruder)
                reordered.push_back(std::move(tool_group));

    if (candidate.first_extruder != candidate.last_extruder)
        for (PrintingToolGroup &tool_group : layer_group.tool_groups)
            if (tool_group.extruder_id == candidate.last_extruder)
                reordered.push_back(std::move(tool_group));

    assert(reordered.size() == layer_group.tool_groups.size());
    layer_group.tool_groups = std::move(reordered);
}

void order_sortable_extrusion_children(ExtrusionEntity &entity,
                                       const Point &start_near,
                                       const LoopEntryPolicy &loop_policy,
                                       const LoopEntryContext &context)
{
    assert(entity.can_sort());

    ExtrusionEntity::Children remaining_children = release_all_children(entity);
    Point current_point = start_near;

    /*
    Greedy local ordering is intentionally simple: at each step, the next child
    is the one that can present the closest legal start point. This keeps the
    hierarchy intact while giving later, more global ordering passes a fixed and
    easy-to-print sequence.
    */
    while (!remaining_children.empty()) {
        size_t selected_idx = size_t(-1);
        ExtrusionEntryCandidate selected_candidate;

        for (size_t child_idx = 0; child_idx < remaining_children.size(); ++child_idx) {
            assert(remaining_children[child_idx] != nullptr);
            if (!remaining_children[child_idx])
                continue;

            const ExtrusionEntryCandidate candidate =
                best_entry_candidate(*remaining_children[child_idx], current_point, loop_policy, context);
            if (!candidate.valid)
                continue;

            if (selected_idx == size_t(-1) ||
                candidate.score < selected_candidate.score) {
                selected_idx = child_idx;
                selected_candidate = candidate;
            }
        }

        /*
        Empty and nop children do not have a printable entry point. Once only
        such children remain, keep their relative order and append them after
        the printable work already selected.
        */
        if (selected_idx == size_t(-1)) {
            for (ExtrusionEntityUPtr &child : remaining_children)
                append_owned_child(entity, std::move(child));
            remaining_children.clear();
            break;
        }

        ExtrusionEntityUPtr child = std::move(remaining_children[selected_idx]);
        remaining_children.erase(remaining_children.begin() + selected_idx);

        prepare_child_for_entry(*child, selected_candidate, loop_policy, context);
        if (!child->empty())
            current_point = child->last_point();
        append_owned_child(entity, std::move(child));
    }

    /*
    The node now stores a concrete print sequence. Leaving it sortable would make
    the computed continuity ambiguous again for later G-code and debug code.
    */
    entity.set_can_sort_reverse(false, false);
}

ExtrusionEntryCandidate best_entry_candidate(const ExtrusionEntity &entity,
                                             const Point &current_point,
                                             const LoopEntryPolicy &loop_policy,
                                             const LoopEntryContext &context)
{
    ExtrusionEntryCandidate best;
    if (entity.empty())
        return best;

    if (entity.can_sort()) {
        /*
        A sortable child can choose one of its own children as the first printed
        element. Look through that child one level at a time so the parent does
        not judge it by an arbitrary current first_point()/last_point().
        */
        for (const ExtrusionEntityUPtr &child : entity.children()) {
            assert(child != nullptr);
            if (!child)
                continue;

            ExtrusionEntryCandidate candidate =
                best_entry_candidate(*child, current_point, loop_policy, context);
            if (candidate.valid &&
                (!best.valid || candidate.score < best.score)) {
                candidate.reverse_before_printing = false;
                candidate.loop_analysis.reset();
                candidate.loop_candidate_idx = size_t(-1);
                best = candidate;
            }
        }
        return best;
    }

    if (entity.is_loop()) {
        std::unique_ptr<LoopEntryAnalysis> analysis = loop_policy.analyze_loop(entity, context);
        if (!analysis || analysis->candidate_count() == 0)
            return best;

        std::shared_ptr<const LoopEntryAnalysis> shared_analysis(std::move(analysis));
        const size_t candidate_count = shared_analysis->candidate_count();

        for (size_t candidate_idx = 0; candidate_idx < candidate_count; ++candidate_idx) {
            ExtrusionEntryCandidate candidate;
            candidate.point = shared_analysis->candidate_point(candidate_idx);
            candidate.score = shared_analysis->score_candidate(candidate_idx, current_point);
            candidate.loop_analysis = shared_analysis;
            candidate.loop_candidate_idx = candidate_idx;
            candidate.valid = true;
            if (!best.valid || candidate.score < best.score)
                best = candidate;
        }
        return best;
    }

    best.point = entity.first_point();
    best.score = current_point.distance_to_square(best.point);
    best.valid = true;

    if (entity.can_reverse()) {
        ExtrusionEntryCandidate reversed;
        reversed.point = entity.last_point();
        reversed.score = current_point.distance_to_square(reversed.point);
        reversed.reverse_before_printing = true;
        reversed.valid = true;
        if (reversed.score < best.score)
            best = reversed;
    }

    return best;
}

void collect_loop_entry_points(const ExtrusionEntity &entity, Points &points)
{
    if (const ArcPolyline *polyline = entity.polyline_or_null()) {
        if (polyline->empty())
            return;

        /*
        Closed polylines store the first vertex again as the last vertex. Skip
        that duplicate so a loop does not get two equivalent candidates with
        different split behavior.
        */
        const bool skip_duplicate_last = polyline->size() > 1 && polyline->front() == polyline->back();
        const size_t point_count = skip_duplicate_last ? polyline->size() - 1 : polyline->size();
        for (size_t point_idx = 0; point_idx < point_count; ++point_idx)
            points.push_back(polyline->get_point(point_idx));
        return;
    }

    if (!entity.is_leaf())
        for (const ExtrusionEntityUPtr &child : entity.children())
            if (child)
                collect_loop_entry_points(*child, points);
}

void prepare_child_for_entry(ExtrusionEntity &child,
                             const ExtrusionEntryCandidate &candidate,
                             const LoopEntryPolicy &loop_policy,
                             const LoopEntryContext &context)
{
    assert(candidate.valid);

    if (candidate.loop_analysis)
        candidate.loop_analysis->rotate_loop_to_candidate(child, candidate.loop_candidate_idx);
    else if (candidate.reverse_before_printing)
        child.reverse();

    if (child.can_sort())
        order_sortable_extrusion_children(child, candidate.point, loop_policy, context);
}

void rotate_loop_to_point(ExtrusionEntity &entity, const Point &entry_point)
{
    if (ArcPolyline *polyline = entity.polyline_or_null()) {
        const size_t point_idx = find_polyline_point_index(*polyline, entry_point);
        if (point_idx != size_t(-1))
            rotate_polyline_to_index(*polyline, point_idx);
        return;
    }

    if (entity.is_leaf())
        return;

    ExtrusionEntity::Children children = release_all_children(entity);
    if (children.empty())
        return;

    size_t containing_idx = size_t(-1);
    for (size_t child_idx = 0; child_idx < children.size(); ++child_idx) {
        if (children[child_idx] && entity_contains_point(*children[child_idx], entry_point)) {
            containing_idx = child_idx;
            break;
        }
    }

    if (containing_idx == size_t(-1)) {
        for (ExtrusionEntityUPtr &child : children)
            append_owned_child(entity, std::move(child));
        return;
    }

    size_t start_idx = containing_idx;
    ExtrusionEntityUPtr closing_piece;

    if (ArcPolyline *polyline = children[containing_idx]->polyline_or_null()) {
        const size_t point_idx = find_polyline_point_index(*polyline, entry_point);
        if (point_idx == size_t(-1)) {
            assert(false);
        } else if (point_idx == 0) {
            start_idx = containing_idx;
        } else if (point_idx == polyline->size() - 1) {
            /*
            The chosen point is the end of this child and the start of the next
            child in the continuous loop. Starting from the next child avoids
            making an unnecessary one-point split.
            */
            start_idx = (containing_idx + 1) % children.size();
        } else {
            /*
            The chosen point is inside a leaf path that participates in a larger
            loop. Split that leaf into tail and head, print the tail first, then
            append the head after the rest of the loop closes back to it.
            */
            ArcPolyline before_entry;
            ArcPolyline after_entry;
            const bool split = polyline->split_at_index(point_idx, before_entry, after_entry);
            assert(split);
            if (split) {
                closing_piece = ExtrusionEntityUPtr(children[containing_idx]->clone());
                closing_piece->set_polyline(std::move(before_entry));
                children[containing_idx]->set_polyline(std::move(after_entry));
            }
            start_idx = containing_idx;
        }
    } else {
        /*
        A child collection may itself contain the selected vertex. Rotate it
        first, then move the child collection to the front of this loop.
        */
        rotate_loop_to_point(*children[containing_idx], entry_point);
        start_idx = containing_idx;
    }

    for (size_t offset = 0; offset < children.size(); ++offset) {
        const size_t child_idx = (start_idx + offset) % children.size();
        append_owned_child(entity, std::move(children[child_idx]));
    }

    if (closing_piece)
        append_owned_child(entity, std::move(closing_piece));
}

void rotate_polyline_to_index(ArcPolyline &polyline, const size_t point_idx)
{
    if (polyline.empty() || point_idx == 0)
        return;
    if (point_idx >= polyline.size())
        return;
    if (point_idx == polyline.size() - 1 && polyline.front() == polyline.back())
        return;

    ArcPolyline before_entry;
    ArcPolyline after_entry;
    const bool split = polyline.split_at_index(point_idx, before_entry, after_entry);
    assert(split);
    if (!split)
        return;

    after_entry.append(std::move(before_entry));
    polyline.swap(after_entry);
}

size_t find_polyline_point_index(const ArcPolyline &polyline, const Point &point)
{
    for (size_t point_idx = 0; point_idx < polyline.size(); ++point_idx)
        if (polyline.get_point(point_idx) == point)
            return point_idx;
    return size_t(-1);
}

bool entity_contains_point(const ExtrusionEntity &entity, const Point &point)
{
    if (const ArcPolyline *polyline = entity.polyline_or_null())
        return find_polyline_point_index(*polyline, point) != size_t(-1);

    if (!entity.is_leaf())
        for (const ExtrusionEntityUPtr &child : entity.children())
            if (child && entity_contains_point(*child, point))
                return true;
    return false;
}

ExtrusionEntity::Children release_all_children(ExtrusionEntity &entity)
{
    ExtrusionEntity::Children out;

    if (ExtrusionEntityCollection *collection = dynamic_cast<ExtrusionEntityCollection *>(&entity)) {
        out.reserve(collection->child_count());
        while (collection->child_count() > 0)
            out.push_back(collection->release(0));
        return out;
    }

    ExtrusionEntity::Children &children = entity.children();
    out = std::move(children);
    children.clear();
    return out;
}

void append_owned_child(ExtrusionEntity &entity, ExtrusionEntityUPtr &&child)
{
    if (!child)
        return;

    if (ExtrusionEntityCollection *collection = dynamic_cast<ExtrusionEntityCollection *>(&entity)) {
        collection->append(std::move(child));
        return;
    }

    entity.append_child(std::move(child));
}

std::vector<const PrintInstance *> ordered_print_instances_for_object_plan(const Print &print)
{
    switch (print.config().complete_objects_sort.value) {
    case cosZ:
        return print.sort_object_instances_by_max_z();
    case cosY:
        return print.sort_object_instances_by_max_y();
    case cosNearest:
        return chain_print_object_instances(print);
    case cosObject:
    default:
        return print.sort_object_instances_by_model_order();
    }
}

size_t print_instance_index(const PrintObject &object, const PrintInstance &instance)
{
    for (size_t instance_idx = 0; instance_idx < object.instances().size(); ++instance_idx)
        if (&object.instances()[instance_idx] == &instance)
            return instance_idx;

    assert(false);
    return size_t(-1);
}

PrintingLayerGroup &layer_group_for_print_z(PrintingGroup &group,
                                            std::map<coord_t, size_t> &layer_index_by_print_z,
                                            const coord_t print_z)
{
    const std::map<coord_t, size_t>::iterator existing = layer_index_by_print_z.find(print_z);
    if (existing != layer_index_by_print_z.end())
        return group.layers[existing->second];

    PrintingLayerGroup layer_group;
    layer_group.print_z = print_z;
    group.layers.push_back(std::move(layer_group));
    const size_t layer_idx = group.layers.size() - 1;
    layer_index_by_print_z.emplace(print_z, layer_idx);
    return group.layers.back();
}

void append_layer_once(PrintingLayerGroup &layer_group, const Layer &layer)
{
    for (const Layer *existing : layer_group.layers)
        if (existing == &layer)
            return;
    layer_group.layers.push_back(&layer);
}

PrintingToolGroup &tool_group_for_extruder(PrintingLayerGroup &layer_group, const uint16_t extruder_id)
{
    for (PrintingToolGroup &tool_group : layer_group.tool_groups)
        if (tool_group.extruder_id == extruder_id)
            return tool_group;

    PrintingToolGroup tool_group;
    tool_group.extruder_id = extruder_id;
    layer_group.tool_groups.push_back(std::move(tool_group));
    return layer_group.tool_groups.back();
}

void append_region_island_once(PrintingToolGroup &tool_group, const LayerRegionIsland &region_island)
{
    for (const LayerRegionIsland *existing : tool_group.region_islands)
        if (existing == &region_island)
            return;
    tool_group.region_islands.push_back(&region_island);
}

void append_region_island_instance_extrusions(PrintingLayerGroup &layer_group,
                                              const LayerRegionIsland &region_island,
                                              const PrintInstance &instance,
                                              const size_t instance_idx,
                                              const std::vector<ExtrusionRole> &roles)
{
    /*
    Do the cheap scan first so empty LayerRegionIslands do not create empty tool
    groups. Empty tool groups are awkward later because they look like real work
    when ordering algorithms count tool changes.
    */
    bool has_printable_root = false;
    for (const ExtrusionRole role : roles)
        if (region_island.has_extrusion(role) && !region_island.extrusion(role).empty()) {
            has_printable_root = true;
            break;
        }
    if (!has_printable_root)
        return;

    PrintingToolGroup &tool_group = tool_group_for_extruder(layer_group, region_island.extruder_id());
    for (const ExtrusionRole role : roles) {
        if (!region_island.has_extrusion(role))
            continue;

        const ExtrusionEntityCollection &source_root = region_island.extrusion(role);
        if (source_root.empty())
            continue;

        /*
        Each role bucket becomes its own PrintingExtrusion. Keeping role buckets
        separate is useful for the first ordering pass because perimeters, infill
        and support are not interchangeable even when they use the same tool.
        */
        PrintingExtrusion extrusion(region_island, role, source_root);
        assert(extrusion.root != nullptr);
        if (extrusion.root != nullptr)
            translate_extrusion_tree(*extrusion.root, instance.shift);
        extrusion.object_instance_idx = uint16_t(instance_idx);
        tool_group.extrusions.push_back(std::move(extrusion));
    }

    /*
    The source island is stored once after at least one printable root was added.
    The ordering plan may contain several cloned roots for this island and
    instance, but source-context algorithms only need the island pointer once per
    tool group.
    */
    append_region_island_once(tool_group, region_island);
}

void translate_extrusion_tree(ExtrusionEntity &entity, const Point &shift)
{
    if (shift == Point(0, 0))
        return;

    /*
    ExtrusionNop is not a path, but it may carry a position used by sequencing
    code. Move that position when it is real; leave NOT_A_POINT untouched because
    it is a sentinel, not a coordinate.
    */
    if (ExtrusionNop *nop = dynamic_cast<ExtrusionNop *>(&entity)) {
        if (nop->position != ExtrusionNop::NOT_A_POINT)
            nop->position += shift;
        return;
    }

    /*
    Leaf extrusion paths store their geometry in ArcPolyline. Collections do not
    have their own points, so they fall through to the recursive child walk.
    */
    if (ArcPolyline *polyline = entity.polyline_or_null()) {
        polyline->translate(shift);
        return;
    }

    if (!entity.is_leaf())
        for (ExtrusionEntityUPtr &child : entity.children())
            if (child)
                translate_extrusion_tree(*child, shift);
}

void sort_layer_groups_by_print_z(PrintingGroup &group)
{
    std::sort(group.layers.begin(), group.layers.end(),
              [](const PrintingLayerGroup &lhs, const PrintingLayerGroup &rhs) {
                  return lhs.print_z < rhs.print_z;
              });
}

} // namespace

} // namespace Slic3r::Printing
