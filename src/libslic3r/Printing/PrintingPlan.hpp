///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_Printing_PrintingPlan_hpp_
#define slic3r_Printing_PrintingPlan_hpp_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "libslic3r/DataTreeFwd.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionRole.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/Point.hpp"

namespace Slic3r {
class Print;
}

namespace Slic3r::Printing {

/*
Ordered events executed around the contents of one PrintingPlan scope.

The two roots always exist and remain non-sortable and non-reversible. Callers
may append complete event trees, but they cannot replace the roots themselves.
This keeps event ordering stable while allowing every plan, group, layer group,
and tool group to expose the same before/after contract.
*/
class PrintingScopeEvents
{
public:
    PrintingScopeEvents();
    PrintingScopeEvents(const PrintingScopeEvents &) = delete;
    PrintingScopeEvents &operator=(const PrintingScopeEvents &) = delete;
    PrintingScopeEvents(PrintingScopeEvents &&other) noexcept;
    PrintingScopeEvents &operator=(PrintingScopeEvents &&other) noexcept;

    const ExtrusionEntity &before() const { return m_before; }
    const ExtrusionEntity &after() const { return m_after; }
    bool has_before() const { return m_before.child_count() != 0; }
    bool has_after() const { return m_after.child_count() != 0; }

    ExtrusionEntity &append_before(const ExtrusionEntity &event);
    ExtrusionEntity &append_before(ExtrusionEntity &&event);
    ExtrusionEntity &append_after(const ExtrusionEntity &event);
    ExtrusionEntity &append_after(ExtrusionEntity &&event);

    // Rebuilds keep the two fixed sequence roots and remove only their events.
    void clear();

private:
    ExtrusionEntity m_before;
    ExtrusionEntity m_after;
};

/*
PrintingPlan is the data model that STEP_ORDERING will progressively refine
before G-code generation.

The normal slicing data tree remains the source of truth: Layers and
LayerRegionIslands are not owned by this plan, and every pointer stored here is
only a reference back to that source data. The printable extrusion trees are the
exception. They are deep-cloned into PrintingExtrusion::root so ordering code can
reorder, group, or split them without mutating the LayerRegionIsland output of
the generation steps.

Coordinates stored in cloned extrusions are expected to be platter coordinates.
When a builder duplicates a source extrusion for each object instance, it applies
the instance shift immediately. Later ordering code should therefore read points
directly from the plan and must not apply the instance shift again.

The hierarchy intentionally mirrors the decisions made by the future ordering
pipeline:
  - PrintingGroup chooses a large independent print batch.
  - PrintingLayerGroup chooses the source layers printed at one Z.
  - PrintingToolGroup chooses a tool/extruder section inside that Z.
  - PrintingExtrusion owns one printable extrusion tree copied from one source
    LayerRegionIsland role bucket.

This first model is still conservative. It describes the contracts and keeps
the data movable. Ordering algorithms refine only this work copy, so they may
turn sortable extrusion collections into fixed sequences without changing the
generation result stored on the source LayerRegionIsland.
*/

/*
One object instance participating in a PrintingGroup.

The pointer is non-owning and stays valid only as long as the Print object used
to build the plan stays alive. The instance index is kept as context for debug
output and for algorithms that still need to know which duplicated object copy an
extrusion came from; the geometry itself should already be shifted.
*/
struct PrintingObjectInstance
{
    const PrintObject *object = nullptr;
    size_t instance_idx = size_t(-1);
};

/*
One cloned extrusion tree ready to be ordered and printed.

region_island and sregion_island_role identify the source role bucket, for
example perimeters or infill, that produced this root. root owns a deep clone of
that source tree. Keeping a clone is important: STEP_ORDERING can reorganize the
tree while other pipeline stages and debug tools can still inspect the original
LayerRegionIsland.
*/
struct PrintingExtrusion
{
    // Non-owning source pointer used to recover settings and island context.
    const LayerRegionIsland *region_island = nullptr;
    // Source role bucket copied into root.
    ExtrusionRole sregion_island_role = ExtrusionRole::None;
    // Owned, mutable work copy of the extrusion tree.
    ExtrusionEntityUPtr root;
    // Context only: the instance shift is already applied to root coordinates.
    uint16_t object_instance_idx = 0;

    PrintingExtrusion() = default;
    PrintingExtrusion(const LayerRegionIsland &source, ExtrusionRole extrusion_role, const ExtrusionEntity &source_root);

    PrintingExtrusion(const PrintingExtrusion&) = delete;
    PrintingExtrusion &operator=(const PrintingExtrusion&) = delete;
    PrintingExtrusion(PrintingExtrusion&&) noexcept = default;
    PrintingExtrusion &operator=(PrintingExtrusion&&) noexcept = default;
};

/*
A tool/extruder section inside one PrintingLayerGroup.

extrusions is the ordered work list for this tool section. region_islands is a
compact source-context list: it records which LayerRegionIslands contributed
extrusions to this tool group, without duplicating that pointer for every cloned
root. A layer group may eventually contain several PrintingToolGroups with the
same extruder if the tool-ordering algorithm decides to split a tool into
separate visits.
*/
struct PrintingToolGroup
{
    PrintingScopeEvents events;
    uint16_t extruder_id = uint16_t(-1);
    std::vector<const LayerRegionIsland*> region_islands;
    std::vector<PrintingExtrusion> extrusions;
};

/*
All source layers printed at one scaled print Z.

Most prints will have one object layer per Z, but multi-object and multi-instance
prints may contribute several Layer pointers to the same PrintingLayerGroup.
tool_groups is ordered: G-code generation will later visit those tool groups in
the stored order. The same source Layer can appear in multiple groups if a
future builder splits a layer by object instance or by a more advanced scheduling
rule.
*/
struct PrintingLayerGroup
{
    PrintingScopeEvents events;
    coord_t print_z = 0;
    std::vector<const Layer*> layers;
    std::vector<PrintingToolGroup> tool_groups;
};

/*
A large independent batch of printable work.

The by-layer builder currently creates one PrintingGroup for the whole print.
Other builders may create one group per object instance, or split an object
instance across several groups. object_instances describes the source instances
represented by this batch; layers contains the ordered Z groups that will be
printed for it.
*/
struct PrintingGroup
{
    PrintingScopeEvents events;
    std::vector<PrintingObjectInstance> object_instances;
    std::vector<PrintingLayerGroup> layers;
};

/*
Top-level ordering work product.

The plan is deliberately a value-like object: it owns cloned extrusion trees and
can be rebuilt from Print whenever an ordering experiment needs a clean source.
It does not own the Print, Layers, or LayerRegionIslands referenced by its
context pointers.
*/
struct PrintingPlan
{
    PrintingScopeEvents events;
    std::vector<PrintingGroup> groups;
};

/*
Context available while choosing where a loop should start.

The loop scorer gets the loop itself as a function argument. This context adds
the source information that is not carried by every ExtrusionEntity: the copied
root currently being ordered, the source LayerRegionIsland when the tree comes
from a PrintingPlan, and the role bucket copied from that island. Unit tests and
standalone callers may leave those pointers empty.
*/
struct LoopEntryContext
{
    const ExtrusionEntity *root = nullptr;
    const LayerRegionIsland *region_island = nullptr;
    ExtrusionRole role = ExtrusionRole::None;
};

/*
Analysis object created for one loop candidate search.

The ordering algorithm deliberately does not know how loop candidates are
represented. A simple analysis may expose stored vertices, while a future one
may keep a rich map of tree leaves, inherited properties, angles or plugin
weights. The analysis is short-lived and is valid only while the loop tree has
not been mutated.
*/
class LoopEntryAnalysis
{
public:
    virtual ~LoopEntryAnalysis() = default;

    virtual size_t candidate_count() const = 0;
    virtual Point candidate_point(size_t candidate_idx) const = 0;
    virtual double score_candidate(size_t candidate_idx, const Point &start_near) const = 0;
    virtual void rotate_loop_to_candidate(ExtrusionEntity &loop_root, size_t candidate_idx) const = 0;
};

/*
Policy used by local extrusion ordering when it needs to choose a loop entry.

The policy receives the whole loop root, not a flattened list of points. This is
important for future scorers: they can inspect the tree shape and properties
before deciding which candidate representation they want to expose through the
returned LoopEntryAnalysis.
*/
class LoopEntryPolicy
{
public:
    virtual ~LoopEntryPolicy() = default;

    virtual std::unique_ptr<LoopEntryAnalysis> analyze_loop(const ExtrusionEntity &loop_root,
                                                            const LoopEntryContext &context) const = 0;
};

/*
Default loop-entry policy.

It reproduces the current conservative behavior: every existing loop vertex is a
candidate, the score is the squared distance to start_near, and rotation is done
on the selected stored vertex.
*/
class DefaultLoopEntryPolicy final : public LoopEntryPolicy
{
public:
    std::unique_ptr<LoopEntryAnalysis> analyze_loop(const ExtrusionEntity &loop_root,
                                                    const LoopEntryContext &context) const override;
};

/*
Order all PrintingToolGroups in place.

This function orders the PrintingToolGroup vector inside each PrintingLayerGroup
of printing_group. It minimizes tool changes between layer groups; it does not
look at extrusion geometry and does not minimize travel moves.

The intended algorithm first builds the list of available extruders for each
PrintingLayerGroup. It then searches for a good starting extruder per layer. A
good start avoids an unnecessary tool change from the previous layer when that is
possible; impossible transitions receive a penalty. The chosen solution is the
first penalty-free solution, or the lowest-penalty solution if every path needs a
tool change.

After the tool order is known, each layer group can be rearranged so the selected
starting extruder is first, the remaining extruders stay grouped, and the last
extruder is chosen to help the next layer. first_extruder may be uint16_t(-1)
when no previous tool is known; in that case the smallest extruder id is used
only as a deterministic tie-break for the first printable layer.

*/
void order_printing_tool_groups(PrintingGroup &printing_group,
                                uint16_t first_extruder = uint16_t(-1));

/*
Build a work plan from the current Print data tree.

Every extrusion root copied into the plan is a deep clone of the source root.
The returned plan may be mutated freely by ordering experiments; callers can
rebuild it from Print when they need a clean source again.

This builder is intended to create one PrintingGroup per PrintObject instance.
Within each group, layers are ordered by print Z. Each LayerRegionIsland role
bucket is duplicated for that specific instance and shifted into platter
coordinates.

Each PrintingGroup produced by this builder contains one object instance. The
same source Layer pointer may therefore appear in several groups, but each group
owns its own shifted extrusion clones. The groups are ordered with
PrintConfig::complete_objects_sort: model order, object height, object Y, or
nearest-neighbor chaining from the instance centers.
*/
PrintingPlan build_printing_plan_by_object(const Print &print);
/*
Build a work plan from the current Print data tree.

Every extrusion root copied into the plan is a deep clone of the source root.
The returned plan may be mutated freely by ordering experiments; callers can
rebuild it from Print when they need a clean source again.

This builder creates one PrintingGroup for the whole Print. It is the simplest
by-layer ordering model: all object instances that share a print Z are placed in
the same PrintingLayerGroup.

It walks all objects and layers, creates one PrintingLayerGroup for each scaled
print Z, and adds every LayerRegionIsland extrusion root once per object
instance. The cloned root is translated by the instance shift before it is stored
in the plan.
*/
PrintingPlan build_printing_plan_by_layer(const Print &print);

/*
Reorder the copied extrusion tree rooted at entity.

Only sortable nodes may have their direct children reordered. Non-sortable
nodes are treated as atomic sequences: their internal order is part of the
generation contract and is left untouched. Loops may be rotated to start near
the current position, but they are not reversed because the winding can carry
meaning for perimeter and infill consumers.
*/
void order_extrusion_tree(ExtrusionEntity &entity, const Point start_near);

/*
Reorder one copied extrusion tree using a caller-provided loop policy.

This is the extension point for advanced loop-entry selection. The policy may
choose candidates using richer information than a point list; the ordering
algorithm only consumes candidate scores and asks the selected analysis to apply
the matching rotation.
*/
void order_extrusion_tree(ExtrusionEntity &entity,
                          const Point start_near,
                          const LoopEntryPolicy &loop_policy,
                          const LoopEntryContext &context);

/*
Order all copied extrusion trees in a PrintingGroup.

This is the group-level entry point for local extrusion ordering. It uses
start_near as the current nozzle position for the first tree, then passes each
tree's final position to the next tree in the already selected printing order.
*/
void order_extrusion_tree(PrintingGroup &printing_group, const Point start_near);

/*
Order all copied extrusion trees in a PrintingGroup using a caller-provided loop
policy. Each PrintingExtrusion contributes its source LayerRegionIsland and role
to the LoopEntryContext passed to the policy.
*/
void order_extrusion_tree(PrintingGroup &printing_group,
                          const Point start_near,
                          const LoopEntryPolicy &loop_policy);

} // namespace Slic3r::Printing

#endif // slic3r_Printing_PrintingPlan_hpp_
