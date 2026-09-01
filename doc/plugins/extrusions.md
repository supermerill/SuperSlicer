# Using Unified Extrusion Entities

> API snapshot commit: `eb858c6cefbb705042d1f127e906d2afcce35927`
>
> Plugin ABI version: `50`

The commit above identifies the source-tree state against which this guide and
its examples were checked. It is not the commit that adds this document: a
commit cannot contain its own final hash. Update both the snapshot hash and the
ABI value whenever this guide is revised for a later plugin API.

The plugin API represents every printable path, ordered path group, and
non-geometric event with the same `slic3r_api::ExtrusionEntity` tree model. A
plugin does not need to know which legacy C++ subclass the host may use
internally. It reads the node's geometry, children, flags, and properties
through one stable interface.

This guide uses the C++ API as the primary interface. The final section maps the
same concepts to the C ABI and Python views.

## One Node Type, Three Content States

An extrusion entity is one tree node in exactly one of these states:

1. **Empty:** no local polyline and no children. Empty nodes may still carry
   properties, for example a custom G-code event.
2. **Polyline leaf:** one local polyline and no children. The polyline contains
   points, optional arc metadata, and optional per-point Z offsets.
3. **Collection node:** ordered children and no local polyline. Properties on
   the parent may describe state inherited by its descendants.

A node cannot have a local polyline and children at the same time. An operation
that adds children to a polyline leaf, or sets a polyline on a collection, is
rejected. Call `clear_content()` first only when discarding the existing
content is intentional.

Do not use a C++ class name to decide what a node means. Inspect its structure:

```cpp
void inspect(const slic3r_api::ExtrusionEntity &entity)
{
    if (entity.has_polyline()) {
        // Local leaf geometry is available through points() and segments().
    } else if (entity.child_count() > 0) {
        // Children preserve their current traversal order.
    } else {
        // The node is empty, although it may still carry direct properties.
    }
}
```

`is_leaf()` means that the node has no children. An empty entity is therefore a
leaf even though it has no polyline.

## The Three C++ Views

The three wrappers expose the same entity but have different ownership and
mutability contracts.

| View | Mutability | Ownership | Typical source |
| --- | --- | --- | --- |
| `ExtrusionEntity` | Read-only | Borrowed | Input data, a const child, `PrintingExtrusion::root()` |
| `MutableExtrusionEntity` | Mutable | Borrowed | A mutable step output, a mutable child, `PrintingExtrusion::mutable_root()` |
| `StoredExtrusionEntity` | Mutable | Owned through `storage_handle` | A temporary tree created or cloned by a plugin |

A borrowed view never frees its handle. Its owner must outlive the view. A
`StoredExtrusionEntity` is move-only and calls `storage_free()` when destroyed.
Return a stored entity from a function that creates ownership; never return a
borrowed view into a local stored entity.

```cpp
#include "libslic3r/Api/plugin/cpp/ExtrusionViews.hpp"

using namespace slic3r_api;

StoredExtrusionEntity clone_for_output(
    storage_handle *storage,
    const ExtrusionEntity &source)
{
    return StoredExtrusionEntity(storage, source);
}
```

Handles obtained from a host-owned layer or printing plan remain host-owned.
Wrapping such a handle in `MutableExtrusionEntity` does not transfer ownership.

## Creating A Printable Leaf

Coordinates use the scaled `coord_t` representation. Use `scale_i()` for points
and Z offsets. Width, height, speed, and volumetric values stored by extrusion
properties use the units documented by their payload, usually millimeters or
millimeters per second.

```cpp
#include "libslic3r/Api/plugin/cpp/ExtrusionViews.hpp"

using namespace slic3r_api;

StoredExtrusionEntity make_perimeter(storage_handle *storage)
{
    StoredExtrusionEntity path(storage, {
        make_point(scale_i(0.0),  scale_i(0.0)),
        make_point(scale_i(20.0), scale_i(0.0)),
        make_point(scale_i(20.0), scale_i(10.0))
    });

    EPropertyAttributes &attributes =
        path.get_or_add(EPropertyAttributes::key);
    attributes
        .extrusion_role(RAW_EXTRUSION_ROLE_EXTERNAL_PERIMETER)
        .mm3_per_mm(0.08)
        .width(0.45f)
        .height(0.20f);

    path.reversible(true);
    return path;
}
```

`EPropertyAttributes` is normally required on printable leaves, either directly
or inherited from a parent. Other process decisions, such as speed,
acceleration, temperature, or fan speed, use additional properties. See
[Using Plugin Properties](properties.md) for direct and dynamic property access.

### Retraction And Wipe Process Leaves

The ordered G-code pipeline also represents extrusion-axis preparation with
ordinary entities. Retract and Unretract requests may be empty E-only events or
geometric wipe leaves. An empty event carries only `RAW_EXTRUSION_ROLE_RETRACT`
or `RAW_EXTRUSION_ROLE_UNRETRACT`. A geometric event additionally carries
`RAW_EXTRUSION_ROLE_TRAVEL | RAW_EXTRUSION_ROLE_WIPE`.

Both forms use the same semantic axis request:

```cpp
MutableExtrusionEntity retract = /* ordered empty event leaf */;
retract.get_or_add(EPropertyAttributes::key)
    .extrusion_role(RAW_EXTRUSION_ROLE_RETRACT)
    .mm3_per_mm(0.0);
retract.get_or_add(EPropertyExtrusionAxis::key)
    .retract_to(0.8, false);

MutableExtrusionEntity unretract = /* ordered empty event leaf */;
unretract.get_or_add(EPropertyAttributes::key)
    .extrusion_role(RAW_EXTRUSION_ROLE_UNRETRACT)
    .mm3_per_mm(0.0);
unretract.get_or_add(EPropertyExtrusionAxis::key)
    .unretract(0.05, false);
```

These requests deliberately do not contain firmware syntax. A firmware session
may encode them as explicit E-only moves or native `G10`/`G11` commands while
its `ExtrusionAxisState` remains responsible for the physical E state. On a
geometric Wipe leaf, explicit E is distributed over the complete planar line
and arc length; Z lift does not increase the requested E movement. A Wipe that
only moves the nozzle has no `EPropertyExtrusionAxis`.

## Building An Ordered Tree

Build trees from the bottom up. Create complete leaves first, attach their
properties, then move them into parent entities in the desired order.

```cpp
StoredExtrusionEntity make_two_path_sequence(storage_handle *storage)
{
    StoredExtrusionEntity root(storage);
    root.disable_sort();
    root.disable_reverse();

    StoredExtrusionEntity first = make_perimeter(storage);
    StoredExtrusionEntity second(storage, {
        make_point(scale_i(20.0), scale_i(10.0)),
        make_point(scale_i(0.0),  scale_i(10.0))
    });

    EPropertyAttributes &attributes =
        second.get_or_add(EPropertyAttributes::key);
    attributes
        .extrusion_role(RAW_EXTRUSION_ROLE_EXTERNAL_PERIMETER)
        .mm3_per_mm(0.08)
        .width(0.45f)
        .height(0.20f);

    const uint32_t first_idx = root.append_child_move(first.mutable_view());
    const uint32_t second_idx = root.append_child_move(second.mutable_view());
    if (is_invalid_index(first_idx) || is_invalid_index(second_idx))
        throw std::runtime_error("The extrusion sequence could not be built.");

    return root;
}
```

After `append_child_move()`, the source handle remains valid but is empty. The
new child is owned by the parent tree. Use `append_child_copy()` when the source
must remain unchanged:

```cpp
void append_clone(
    StoredExtrusionEntity &root,
    const StoredExtrusionEntity &source)
{
    const uint32_t idx = root.append_child_copy(source.readonly());
    if (is_invalid_index(idx))
        throw std::runtime_error("The extrusion could not be cloned into the tree.");
}
```

Use `move_child_from()` when moving an already attached child between parents.
Do not combine an insertion with manual removal: the dedicated operation keeps
ownership and indices consistent.

### Inserting An Ordered Boundary Leaf

Use `emplace_ordered_leaf()` when a new empty leaf must execute immediately
before or after everything already represented by an entity. The method keeps
the current entity handle stable and makes the resulting outer node
non-sortable and non-reversible:

```cpp
MutableExtrusionEntity event = root.emplace_ordered_leaf(
    OrderedLeafPosition::Before,
    ExistingPropertyPlacement::MoveWithExistingContent);
if (!event.valid())
    throw std::runtime_error("The ordered event leaf could not be inserted.");

event.script_gcode(script, script_type);
```

`KeepOnParent` leaves the current node's direct properties on the outer parent.
The new leaf therefore inherits them. If the current entity is already a fixed
collection, the leaf is inserted directly; otherwise the previous content is
moved below a wrapper that preserves its former flags.

`MoveWithExistingContent` always creates that wrapper, even when the current
entity is already fixed or has no properties. Existing direct properties and
their variable-size resources move with the old content, so the new leaf does
not inherit them. This is the appropriate policy for inserting an event before
a leaf that already carries a different event or process state.

`Before` places the new leaf first and `After` places it last. Existing children
are transferred without cloning, so their handles remain stable. The returned
leaf is empty, non-sortable, and non-reversible; populate it only after checking
that the returned view is valid.

The operation is structurally atomic: invalid arguments or an allocation
failure leave the tree unchanged. A successful insertion invalidates borrowed
views of the modified node's direct structure and properties. Reacquire those
views before further use; borrowed views of transferred existing children stay
valid.

## Flags, Order, And Continuity

`RAW_EXTRUSION_FLAG_REVERSIBLE` allows an optimizer to reverse an entity.
`RAW_EXTRUSION_FLAG_SORTABLE` allows it to reorder the entity's direct children.
The C++ helpers expose these as `reversible()` and `sortable()`.

Continuity is computed, not stored as a mutable flag. A non-sortable collection
is continuous when all non-empty descendants are continuous and adjacent paths
touch end-to-start. A sortable collection is never reported as continuous
because its order may change later.

Use a non-sortable, non-reversible parent whenever child order carries semantic
meaning, such as a command that must execute immediately before a path.

## Points, Segments, Arcs, And Z Offsets

Point operations create or edit straight geometry. Segment operations preserve
arc orientation, radius, and endpoint Z offsets.

```cpp
StoredExtrusionEntity make_arc(storage_handle *storage)
{
    StoredExtrusionEntity arc(storage);

    c_extrusion_segment segment = {};
    segment.point_a = make_point(scale_i(0.0),  scale_i(0.0));
    segment.point_b = make_point(scale_i(10.0), scale_i(10.0));
    segment.radius = static_cast<float>(scale_d(10.0));
    segment.orientation = RAW_EXTRUSION_ARC_ORIENTATION_CCW;
    segment.z_offset_a = scale_i(0.0);
    segment.z_offset_b = scale_i(0.2);

    if (!arc.set_segments({ segment }))
        throw std::runtime_error("The arc extrusion is invalid.");
    return arc;
}
```

For a straight segment, set `radius` to zero and use
`RAW_EXTRUSION_ARC_ORIENTATION_UNKNOWN`. Adjacent segments must agree on their
shared point and shared Z offset.

The local methods `point_count()`, `segments()`, `local_front()`, and
`local_length()` inspect only the current node's polyline. The tree-wide methods
`front()`, `back()`, `length()`, `empty()`, and `collect_points()` recursively
inspect descendants when the current node is a collection.

`collect_points()` deliberately flattens structure and arcs. Do not use it to
copy or transform an entity when preserving exact geometry matters; clone the
entity or operate on its segments instead.

## Traversal And Inherited Properties

`entity.get(key)` reads only a property stored directly on that entity. Use an
extrusion tree visitor when a parent property applies to its descendants.

```cpp
#include "libslic3r/Api/plugin/cpp/ExtrusionTreeVisitors.hpp"

class PrintableLeafVisitor : public slic3r_api::ExtrusionTreeConstVisitor<>
{
protected:
    void visit_leaf(slic3r_api::ExtrusionEntity leaf) override
    {
        if (!leaf.has_polyline())
            return;

        const slic3r_api::EPropertyAttributes *attributes =
            current_property(slic3r_api::EPropertyAttributes::key);
        if (attributes == nullptr)
            throw std::runtime_error("A printable leaf has no attributes.");

        process_leaf(leaf, *attributes);
    }

private:
    void process_leaf(
        slic3r_api::ExtrusionEntity leaf,
        const slic3r_api::EPropertyAttributes &attributes)
    {
        // The real plugin may inspect or aggregate the validated leaf here.
        (void) leaf;
        (void) attributes;
    }
};

void inspect_printable_leaves(slic3r_api::ExtrusionEntity root)
{
    PrintableLeafVisitor visitor;
    visitor.traverse(root);
}
```

`current_property()` searches from the current node toward the root and returns
the closest direct value. It works with built-in and dynamically registered
`PluginPropertyKey` instances.

The mutable visitor permits local edits to the current entity. Do not add or
remove siblings or ancestors during traversal. Replacing the current leaf by a
collection is supported, but the newly inserted children are not visited in
that same pass.

## Structural Mutation And Borrowed Views

Treat child views and property pointers like iterators into a mutable container.
They may be invalidated by:

- inserting, removing, moving, or reordering children;
- replacing an entity with `copy_from()` or `move_from()`;
- removing or recreating a property;
- clearing the owner, its storage, or its printing-plan container.

Read the needed value before mutation, then reacquire child views and property
pointers afterwards. Do not keep fragment handles returned by
`split_leaf_by_areas()` across a later structural mutation of their parent.

`copy_from()` and `move_from()` preserve the destination handle identity while
replacing its content, properties, flags, children, and stored data. The move
operation leaves the source valid but empty.

## Editing A PrintingPlan Clone

`PrintingExtrusion` owns a clone of its source extrusion root. Ordering and
later printing-plan plugins should edit this clone, not the original
`LayerRegionIsland` tree.

```cpp
#include "libslic3r/Api/plugin/cpp/PrintingPlanViews.hpp"

void disable_reordering(slic3r_api::PrintingExtrusion extrusion)
{
    slic3r_api::MutableExtrusionEntity root = extrusion.mutable_root();
    root.disable_sort();
    root.disable_reverse();
}
```

Use `PrintingExtrusion::root()` for a read-only view and `mutable_root()` only
in a step whose contract allows tree mutation. `set_root_clone()` keeps its
source unchanged; `set_root_move()` consumes a standalone mutable entity.

Extrusions attached directly to data-tree objects may use object-local
coordinates. Printing-plan roots are cloned and transformed according to their
plan instance. Check the contract of the step that supplied the handle before
applying an instance transform.

### Traversing PrintingPlan Entities By Direct Property

`PrintingEntityPropertyTraversal` streams the entities carrying one direct
property across a `PrintingLayerGroup`. It also supplies the owning tool group
and `PrintingExtrusion`, without allocating a flattened vector:

```cpp
#include "libslic3r/Api/plugin/cpp/PrintingEntityPropertyTraversal.hpp"

struct TransitionMarker
{
    uint32_t id;
};

using MarkerEntity = slic3r_api::PrintingEntity<TransitionMarker>;

void process_marked_entities(
    orchestrator_handle *orchestrator,
    slic3r_api::PrintingLayerGroup layer_group)
{
    const slic3r_api::PluginPropertyKey<TransitionMarker> marker_key =
        slic3r_api::PluginPropertyKey<TransitionMarker>::register_dynamic(
            orchestrator, "example.transition_marker");

    slic3r_api::PrintingEntityPropertyTraversal<TransitionMarker> traversal(
        marker_key,
        [](MarkerEntity *previous, MarkerEntity *next) {
            // previous is null at layer start; next is null at layer end.
            if (previous != nullptr && next != nullptr)
                previous->property->id = next->property->id;
        });
    traversal.process(layer_group);
}
```

The traversal uses depth-first pre-order inside each extrusion tree and does
not resolve inherited properties. Its context and property pointers are
borrowed for the callback only.

A callback may update payloads, add unrelated properties, and edit a dedicated
descendant subtree which does not carry the selecting property. Transition
plugins use this rule to populate a scope's `travel`, `before`, or `after`
phase while keeping the marked scope root stable. Do not change a matching
root's child list or an active ancestor's child list, add or remove selecting
properties, reorder matching entities, or retain callback pointers. The
traversal reacquires a matching payload after callback-side property storage
changes, but structural identity and order must remain unchanged.

Separate `process()` calls do not share a previous entity, so their null
boundaries are layer-local.

By default, descendants of a matching entity are still traversed, so a direct
property on both a parent and a child produces two matches. When a property
defines non-nested logical roots, pass
`MatchingEntityDescendants::Skip` as the third constructor argument. The
traversal then reports the marked root and skips its complete subtree. This is
both faster for large scope contents and allows callbacks to populate dedicated
phase children without making those new descendants part of the active walk.

## Migrating Legacy Extrusion Classes

Migration from the old extrusion hierarchy is a change of representation, not
a call to a generic converter. Plugin code should stop branching on
`ExtrusionPath`, `ExtrusionEntityCollection`, `ExtrusionMultiPath`, or
`ExtrusionLoop` and instead use the unified node's content, flags, and
properties. An old path becomes a leaf with a local polyline and
`EPropertyAttributes`; an old collection or multi-path becomes a parent with
ordered children; sort and reverse capabilities become entity flags; and a
loop is represented by closed geometry or by an ordered non-sortable parent
when it contains multiple pieces. Empty command-like entities remain empty
nodes carrying command properties.

When porting construction code, migrate bottom-up:

1. Create each path as a `StoredExtrusionEntity`.
2. Copy its points or segments and add its attributes and process properties.
3. Create parent `StoredExtrusionEntity` nodes and set their flags.
4. Move children into each parent in final traversal order.
5. Move or clone the completed root into the host-owned output.

Legacy raw owning pointers and collection-specific `append()` calls should not
cross the plugin API. Use storage-owned entities and explicit copy/move methods
instead. If the host supplies an entity whose native implementation is still a
legacy subclass, its `extrusion_entity_handle` already exposes the unified base
view; no cast or wrapper conversion is required.

## C And Python Correspondence

| Operation | C++ | C ABI | Python |
| --- | --- | --- | --- |
| Borrow read-only | `ExtrusionEntity(handle)` | `const extrusion_entity_handle *` | `api.extrusion(handle)` |
| Borrow mutable | `MutableExtrusionEntity(handle)` | `extrusion_entity_handle *` | `api.mutable_extrusion(handle)` |
| Create owned | `StoredExtrusionEntity(storage)` | `extrusion_create_empty(storage)` | `api.new_extrusion(storage)` |
| Deep clone | `entity.clone(storage)` | `extrusion_clone(storage, entity)` | `entity.clone(storage)` |
| Read children | `entity.children()` | `extrusion_child_count()` / `extrusion_child()` | `entity.children()` |
| Append copy | `append_child_copy()` | `extrusion_insert_child_copy()` | `add_child_copy()` |
| Append move | `append_child_move()` | `extrusion_insert_child_move()` | `add_child_move()` |
| Set straight geometry | `set_points()` | `extrusion_polyline_set_points()` | `set_points()` |
| Preserve arcs and Z | `set_segments()` | `extrusion_polyline_set_segments()` | `set_segments()` |
| Direct property lookup | `entity.get(key)` | `extrusion_property_data()` | `entity.property(Payload)` |

The C ABI is authoritative for binary compatibility. C callers must release
standalone storage-owned entities with `storage_free()`. Python
`StoredExtrusionEntity` supports explicit release through `free_from_storage()`
and automatic release with the `with` statement; borrowed Python wrappers never
own the host object.

Python mutable property payloads are live `ctypes` views, while read-only
property access returns a copy. Reacquire a mutable payload after structural
mutation for the same reasons as in C++.
