///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_extrusion_entity_h_
#define slic3r_extrusion_entity_h_

#define SLIC3R_PLUGIN_API_EXTRUSION_ENTITY_MAJOR 1u
#define SLIC3R_PLUGIN_API_EXTRUSION_ENTITY_MINOR 0u

#include <stdint.h>

#include "slic3r_extrusion_polyline.h"
#include "slic3r_extrusion_property.h"
#include "slic3r_utils.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
Extrusion entity API.

An extrusion entity is a tree node. It may contain:
    - no local polyline and no children;
    - one local polyline and no children;
    - children and no local polyline.

Children are ordered. If an entity is marked sortable, the host may reorder its
children during path planning. Continuity is not a mutable flag: it is computed
from the current tree order, the current child points, and the sortable flag.

Use the polyline API to edit local points/segments. Use the property API to
describe how an entity or its descendants should be interpreted.

Developer guide:
[Using Unified Extrusion Entities](/doc/plugins/extrusions.md)
*/

typedef struct extrusion_entity_handle extrusion_entity_handle;

typedef enum raw_extrusion_ordered_leaf_position {
    RAW_EXTRUSION_ORDERED_LEAF_BEFORE = 0,
    RAW_EXTRUSION_ORDERED_LEAF_AFTER = 1
} raw_extrusion_ordered_leaf_position;

typedef enum raw_extrusion_existing_property_placement {
    RAW_EXTRUSION_EXISTING_PROPERTIES_KEEP_ON_PARENT = 0,
    RAW_EXTRUSION_EXISTING_PROPERTIES_MOVE_WITH_CONTENT = 1
} raw_extrusion_existing_property_placement;

typedef enum raw_extrusion_split_status {
    RAW_EXTRUSION_SPLIT_STATUS_SUCCESS = 0,
    RAW_EXTRUSION_SPLIT_STATUS_INVALID_ARGUMENT,
    RAW_EXTRUSION_SPLIT_STATUS_NOT_A_LEAF,
    RAW_EXTRUSION_SPLIT_STATUS_INVALID_GEOMETRY,
    RAW_EXTRUSION_SPLIT_STATUS_CLIPPING_FAILED,
    RAW_EXTRUSION_SPLIT_STATUS_INTERNAL_ERROR
} raw_extrusion_split_status;

typedef void (*extrusion_split_fragment_fn)(
    extrusion_entity_handle *fragment,
    uint32_t area_index,
    void *user_data);

#ifndef EXTRUSION_INDEX_INVALID
#define EXTRUSION_INDEX_INVALID ((uint32_t)UINT32_MAX)
#endif

/*
Entity flags.

REVERSIBLE means the entity may be reversed by algorithms that optimize travel.
SORTABLE means the entity's children may be reordered. It is meaningful only for
child collections. A sortable entity is never reported as continuous.
*/
#define RAW_EXTRUSION_FLAG_REVERSIBLE ((uint32_t)(1u << 0))
#define RAW_EXTRUSION_FLAG_SORTABLE   ((uint32_t)(1u << 1))

/* Create an empty extrusion entity owned by storage. Release it with storage_free(). */
SLIC3R_HOST_API extrusion_entity_handle *extrusion_create_empty(storage_handle *storage);

/* Create a deep copy of src owned by storage. Release it with storage_free(). */
SLIC3R_HOST_API extrusion_entity_handle *extrusion_clone(storage_handle *storage, const extrusion_entity_handle *src);

/*
Replace dst with a deep copy of src.

dst keeps its handle identity, but its content, properties, flags, children and
stored data are replaced.
*/
SLIC3R_HOST_API int32_t extrusion_copy_from(extrusion_entity_handle *dst, const extrusion_entity_handle *src);

/*
Move src into dst.

dst keeps its handle identity. src remains valid but becomes empty afterwards.
*/
SLIC3R_HOST_API int32_t extrusion_move_from(extrusion_entity_handle *dst, extrusion_entity_handle *src);

/* Remove local polyline and all children. Properties and flags are unchanged. */
SLIC3R_HOST_API int32_t extrusion_clear_content(extrusion_entity_handle *entity);

/* Return the current entity flags bitset. Returns 0 for NULL. */
SLIC3R_HOST_API uint32_t extrusion_flags(const extrusion_entity_handle *entity);

/*
Set mutable entity flags.

Only REVERSIBLE and SORTABLE are mutable. Continuity is computed by
extrusion_is_continuous() and cannot be forced through flags. Returns non-zero
on success.
*/
SLIC3R_HOST_API int32_t extrusion_set_flags(extrusion_entity_handle *entity, uint32_t flags);

/*
Return non-zero if the entity is currently one continuous ordered path.

An entity with SORTABLE set is never continuous, because path planners may
reorder its children. Empty entities and leaf polylines are continuous by
nature. Child collections are continuous only when every non-empty child is
continuous and adjacent children touch end-to-start.
*/
SLIC3R_HOST_API int32_t extrusion_is_continuous(const extrusion_entity_handle *entity);

/*
Return non-zero when the complete entity is an ordered closed loop.

A loop is non-empty, non-sortable, continuous, and has identical first and
last points. The test applies to both one local polyline and a fixed collection
of continuous child paths.
*/
SLIC3R_HOST_API int32_t extrusion_is_loop(const extrusion_entity_handle *entity);

/* Return non-zero if the entity has a local polyline. */
SLIC3R_HOST_API int32_t extrusion_has_polyline(const extrusion_entity_handle *entity);

/* Return non-zero if the entity has children. */
SLIC3R_HOST_API int32_t extrusion_has_children(const extrusion_entity_handle *entity);

/* Return the number of direct children. */
SLIC3R_HOST_API uint32_t extrusion_child_count(const extrusion_entity_handle *entity);

/* Return one direct child, or NULL if idx is invalid. */
SLIC3R_HOST_API extrusion_entity_handle *extrusion_child_mutable(extrusion_entity_handle *entity, uint32_t idx);
SLIC3R_HOST_API const extrusion_entity_handle *extrusion_child(const extrusion_entity_handle *entity, uint32_t idx);

/*
Insert a deep copy of child into parent.

Insertion at idx == extrusion_child_count(parent) appends to the end. idx larger
than child_count is invalid and returns EXTRUSION_INDEX_INVALID. If parent has a
local polyline, the operation fails: clear the local polyline first if changing
the entity into a child collection is intended.
*/
SLIC3R_HOST_API uint32_t extrusion_insert_child_copy(extrusion_entity_handle *parent,
                                                     uint32_t idx,
                                                     const extrusion_entity_handle *child);

/*
Insert child content into parent, then leave child empty.

This moves the content of the child handle, not a node already attached to some
other parent. To move an existing child between parents, use extrusion_move_child().
*/
SLIC3R_HOST_API uint32_t extrusion_insert_child_move(extrusion_entity_handle *parent,
                                                     uint32_t idx,
                                                     extrusion_entity_handle *child);

/*
Create one empty, non-sortable and non-reversible leaf at an ordered boundary.

The entity handle keeps its identity and becomes a fixed parent. BEFORE places
the new leaf before all previous logical content; AFTER places it after that
content. Existing child objects are moved without changing their addresses.

KEEP_ON_PARENT leaves direct properties on entity. It inserts directly when
entity is already a fixed collection, so the new leaf inherits those
properties. MOVE_WITH_CONTENT always creates a wrapper for the old content and
moves entity's direct properties and associated data into that wrapper.

The returned handle is borrowed from entity and is invalidated when a later
structural mutation removes the leaf. NULL or an unknown enum value returns
NULL without modifying entity. Allocation failures are contained by the host
and also return NULL. After success, reacquire borrowed views of entity's
direct child list and, with MOVE_WITH_CONTENT, its direct properties. Handles
to existing child objects remain valid.
*/
SLIC3R_HOST_API extrusion_entity_handle *extrusion_emplace_ordered_leaf(
    extrusion_entity_handle *entity,
    raw_extrusion_ordered_leaf_position position,
    raw_extrusion_existing_property_placement property_placement);

/* Remove one direct child. Returns non-zero on success. */
SLIC3R_HOST_API int32_t extrusion_remove_child(extrusion_entity_handle *parent, uint32_t idx);

/*
Move an existing child from one parent to another.

The moved child keeps its content and properties. dst_idx follows normal insert
semantics: dst_idx == extrusion_child_count(dst_parent) appends to the end.
Indices larger than the destination child count are invalid.
*/
SLIC3R_HOST_API uint32_t extrusion_move_child(extrusion_entity_handle *dst_parent,
                                              uint32_t dst_idx,
                                              extrusion_entity_handle *src_parent,
                                              uint32_t src_idx);

/*
Split one extrusion leaf according to an ordered 2D area partition.

The caller must provide disjoint areas that completely cover the leaf path.
This function deliberately does not detect or repair overlaps or uncovered
sections. Empty area collections are accepted and simply produce no fragment.

The leaf handle keeps its identity. If one area owns the complete leaf, the
tree is left unchanged. Otherwise the leaf becomes a non-sortable collection
whose children follow source traversal order. Each child receives the leaf's
direct properties and flags. Arc geometry and interpolated Z offsets are
preserved by cutting the original ArcPolyline after clipping its temporary
linearization.

on_fragment is optional and is called synchronously after a successful
publication. It must not throw or structurally modify leaf. Borrowed fragment
handles remain valid only until a later structural mutation of leaf.
*/
SLIC3R_HOST_API raw_extrusion_split_status extrusion_split_leaf_by_areas(
    extrusion_entity_handle *leaf,
    const expolygon_collection_handle *const *areas,
    uint32_t area_count,
    coord_t max_deviation,
    extrusion_split_fragment_fn on_fragment,
    void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* slic3r_extrusion_entity_h_ */
