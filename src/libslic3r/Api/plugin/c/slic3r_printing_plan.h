///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_printing_plan_h_
#define slic3r_printing_plan_h_

#include <stdint.h>

#include "slic3r_data_tree.h"
#include "slic3r_extrusion_entity.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
PrintingPlan API
================

PrintingPlan is the host-owned work model used when code needs to prepare the
final print order without modifying the slicing data tree.

The source data remains in Print, Layer, LayerRegionIsland, and their extrusion
roots. A PrintingPlan only keeps non-owning references back to that source data
for context. Printable extrusion roots are the exception: when an extrusion is
added to the plan it is cloned or moved into the plan, so callers may reorder
the plan copy without changing the source LayerRegionIsland.

Hierarchy:
  printing_plan      top-level ordered list of independent print batches
  printing_group     one independent batch, for example the whole print or one
                     complete object instance
  layer_group        all source layers printed at one print Z inside a group
  tool_group         one extruder/tool section inside that layer group
  printing_extrusion one cloned extrusion tree plus its source context

Handle lifetime:
  All handles are borrowed. They are valid only while the owning plan and the
  vector element they point to still exist. Any clear(), append(), or
  move() on the same parent may invalidate child handles because the underlying
  vectors may reallocate or reorder their elements.

Null and range behavior:
  Getters return null, zero, or an empty value when the input handle is null or
  the index is out of range. Mutators ignore null handles unless they return an
  int32_t status; status functions return 1 on success and 0 on failure.
*/
typedef struct printing_plan_handle printing_plan_handle;
typedef struct printing_group_handle printing_group_handle;
typedef struct printing_layer_group_handle printing_layer_group_handle;
typedef struct printing_tool_group_handle printing_tool_group_handle;
typedef struct printing_extrusion_handle printing_extrusion_handle;
typedef struct printing_scope_events_handle printing_scope_events_handle;

typedef struct c_printing_object_instance
{
    /* Non-owning source object pointer. Geometry in plan extrusions is already
       shifted for this instance when the builder duplicated it. */
    const object_handle *object;
    uint64_t instance_idx;
} c_printing_object_instance;

/* ---- scope events -------------------------------------------------------

Every plan hierarchy scope owns one before and one after event sequence. The
sequence roots always exist and stay non-sortable and non-reversible. Plugins
may inspect them and append complete event trees, but cannot replace or mutate
the roots themselves.

Returned event handles are borrowed children of the selected sequence. A later
append may invalidate an earlier child handle if the child vector reallocates.
*/
SLIC3R_HOST_API int32_t printing_scope_events_has_before(const printing_scope_events_handle *me);
SLIC3R_HOST_API int32_t printing_scope_events_has_after(const printing_scope_events_handle *me);
SLIC3R_HOST_API const extrusion_entity_handle *printing_scope_events_get_before(
    const printing_scope_events_handle *me);
SLIC3R_HOST_API const extrusion_entity_handle *printing_scope_events_get_after(
    const printing_scope_events_handle *me);
SLIC3R_HOST_API extrusion_entity_handle *printing_scope_events_append_before_clone(
    printing_scope_events_handle *me,
    const extrusion_entity_handle *event);
SLIC3R_HOST_API extrusion_entity_handle *printing_scope_events_append_before_move(
    printing_scope_events_handle *me,
    extrusion_entity_handle *event);
SLIC3R_HOST_API extrusion_entity_handle *printing_scope_events_append_after_clone(
    printing_scope_events_handle *me,
    const extrusion_entity_handle *event);
SLIC3R_HOST_API extrusion_entity_handle *printing_scope_events_append_after_move(
    printing_scope_events_handle *me,
    extrusion_entity_handle *event);

/* ---- plan ---------------------------------------------------------------

The plan owns all groups and all cloned extrusion roots stored under them.
Use clear() before rebuilding a plan. Construction is intentionally explicit:
plugins append groups, layer groups, tool groups, and cloned extrusion roots so
the ordering strategy lives in the plugin that owns it.
*/
SLIC3R_HOST_API void printing_plan_clear(printing_plan_handle *me);
SLIC3R_HOST_API printing_scope_events_handle *printing_plan_get_events_mutable(printing_plan_handle *me);
SLIC3R_HOST_API const printing_scope_events_handle *printing_plan_get_events(const printing_plan_handle *me);
/* Count/read/append/reorder top-level groups. The move operation keeps the
   moved group and its cloned extrusion roots intact. */
SLIC3R_HOST_API uint32_t printing_plan_count_group(const printing_plan_handle *me);
SLIC3R_HOST_API printing_group_handle *printing_plan_get_group_mutable(printing_plan_handle *me, uint32_t idx);
SLIC3R_HOST_API const printing_group_handle *printing_plan_get_group(const printing_plan_handle *me, uint32_t idx);
SLIC3R_HOST_API printing_group_handle *printing_plan_append_group(printing_plan_handle *me);
SLIC3R_HOST_API int32_t printing_plan_move_group(printing_plan_handle *me, uint32_t from_idx, uint32_t to_idx);

/* ---- group --------------------------------------------------------------

A group is an independent batch of printable work. A simple by-layer plan often
has one group for the whole print. Complete-object style plans may use one group
per object instance. The object-instance list is context only; it tells later
code which source objects are represented by this batch.
*/
SLIC3R_HOST_API void printing_group_clear(printing_group_handle *me);
SLIC3R_HOST_API printing_scope_events_handle *printing_group_get_events_mutable(printing_group_handle *me);
SLIC3R_HOST_API const printing_scope_events_handle *printing_group_get_events(const printing_group_handle *me);
SLIC3R_HOST_API uint32_t printing_group_count_object_instance(const printing_group_handle *me);
SLIC3R_HOST_API c_printing_object_instance printing_group_get_object_instance(const printing_group_handle *me,
                                                                              uint32_t idx);
SLIC3R_HOST_API void printing_group_append_object_instance(printing_group_handle *me,
                                                           const object_handle *object,
                                                           uint64_t instance_idx);
/* Layer groups are expected to be visited in stored order. A builder usually
   appends them in increasing print_z, but ordering plugins may move them if
   they intentionally change the high-level schedule. */
SLIC3R_HOST_API uint32_t printing_group_count_layer_group(const printing_group_handle *me);
SLIC3R_HOST_API printing_layer_group_handle *printing_group_get_layer_group_mutable(printing_group_handle *me,
                                                                                   uint32_t idx);
SLIC3R_HOST_API const printing_layer_group_handle *printing_group_get_layer_group(const printing_group_handle *me,
                                                                                  uint32_t idx);
SLIC3R_HOST_API printing_layer_group_handle *printing_group_append_layer_group(printing_group_handle *me,
                                                                              coord_t print_z);
SLIC3R_HOST_API int32_t printing_group_move_layer_group(printing_group_handle *me, uint32_t from_idx, uint32_t to_idx);

/* ---- layer group --------------------------------------------------------

A layer group represents one print Z inside a group. It may reference several
source Layer objects, for example when multiple object instances have geometry
at the same Z. The source Layer pointers are non-owning and are used only for
context and settings lookup.
*/
SLIC3R_HOST_API printing_scope_events_handle *printing_layer_group_get_events_mutable(
    printing_layer_group_handle *me);
SLIC3R_HOST_API const printing_scope_events_handle *printing_layer_group_get_events(
    const printing_layer_group_handle *me);
SLIC3R_HOST_API coord_t printing_layer_group_get_print_z(const printing_layer_group_handle *me);
SLIC3R_HOST_API void printing_layer_group_set_print_z(printing_layer_group_handle *me, coord_t print_z);
SLIC3R_HOST_API uint32_t printing_layer_group_count_layer(const printing_layer_group_handle *me);
SLIC3R_HOST_API const layer_handle *printing_layer_group_get_layer(const printing_layer_group_handle *me,
                                                                   uint32_t idx);
SLIC3R_HOST_API void printing_layer_group_append_layer(printing_layer_group_handle *me, const layer_handle *layer);
/* Tool groups are visited in stored order. They split the work at one print Z
   into extruder/tool sections, so moving them changes tool-change order but
   does not alter the extrusion geometry. */
SLIC3R_HOST_API uint32_t printing_layer_group_count_tool_group(const printing_layer_group_handle *me);
SLIC3R_HOST_API printing_tool_group_handle *printing_layer_group_get_tool_group_mutable(printing_layer_group_handle *me,
                                                                                       uint32_t idx);
SLIC3R_HOST_API const printing_tool_group_handle *printing_layer_group_get_tool_group(
    const printing_layer_group_handle *me,
    uint32_t idx);
SLIC3R_HOST_API printing_tool_group_handle *printing_layer_group_append_tool_group(printing_layer_group_handle *me,
                                                                                  uint16_t extruder_id);
SLIC3R_HOST_API int32_t printing_layer_group_move_tool_group(printing_layer_group_handle *me,
                                                             uint32_t from_idx,
                                                             uint32_t to_idx);

/* ---- tool group ---------------------------------------------------------

A tool group contains the ordered extrusion work for one extruder/tool section.
region_islands is a compact source-context list: it records which
LayerRegionIslands contributed work to this tool group. printing_extrusion
entries store the actual cloned roots to print.
*/
SLIC3R_HOST_API printing_scope_events_handle *printing_tool_group_get_events_mutable(
    printing_tool_group_handle *me);
SLIC3R_HOST_API const printing_scope_events_handle *printing_tool_group_get_events(
    const printing_tool_group_handle *me);
SLIC3R_HOST_API uint16_t printing_tool_group_get_extruder_id(const printing_tool_group_handle *me);
SLIC3R_HOST_API void printing_tool_group_set_extruder_id(printing_tool_group_handle *me, uint16_t extruder_id);
SLIC3R_HOST_API uint32_t printing_tool_group_count_region_island(const printing_tool_group_handle *me);
SLIC3R_HOST_API const layer_region_island_handle *printing_tool_group_get_region_island(
    const printing_tool_group_handle *me,
    uint32_t idx);
SLIC3R_HOST_API void printing_tool_group_append_region_island(printing_tool_group_handle *me,
                                                              const layer_region_island_handle *region_island);
SLIC3R_HOST_API uint32_t printing_tool_group_count_extrusion(const printing_tool_group_handle *me);
SLIC3R_HOST_API printing_extrusion_handle *printing_tool_group_get_extrusion_mutable(printing_tool_group_handle *me,
                                                                                     uint32_t idx);
SLIC3R_HOST_API const printing_extrusion_handle *printing_tool_group_get_extrusion(
    const printing_tool_group_handle *me,
    uint32_t idx);
/* Clone from an explicit extrusion root. Use this when the caller already
   selected or prepared a source tree and wants the plan to own an independent
   copy. A null root creates an empty root in the plan. */
SLIC3R_HOST_API printing_extrusion_handle *printing_tool_group_append_extrusion_clone(
    printing_tool_group_handle *me,
    const layer_region_island_handle *source_region_island,
    raw_extrusion_role source_role,
    const extrusion_entity_handle *root,
    uint16_t object_instance_idx);
/* Clone directly from a LayerRegionIsland role bucket. This is the common path
   for builders that copy generated perimeters, infill, support, etc. It fails
   when the source island does not have that role bucket. */
SLIC3R_HOST_API printing_extrusion_handle *printing_tool_group_append_extrusion_clone_from_region_island(
    printing_tool_group_handle *me,
    const layer_region_island_handle *source_region_island,
    raw_extrusion_role source_role,
    uint16_t object_instance_idx);
/* Move from a mutable extrusion root into the plan. The source handle remains
   owned by its original storage, but its content is moved away. Use this only
   for temporary roots created specifically to be handed to the plan. */
SLIC3R_HOST_API printing_extrusion_handle *printing_tool_group_append_extrusion_move(
    printing_tool_group_handle *me,
    const layer_region_island_handle *source_region_island,
    raw_extrusion_role source_role,
    extrusion_entity_handle *root,
    uint16_t object_instance_idx);
/* Reorder cloned roots inside this tool section without copying them. */
SLIC3R_HOST_API int32_t printing_tool_group_move_extrusion(printing_tool_group_handle *me,
                                                           uint32_t from_idx,
                                                           uint32_t to_idx);

/* ---- extrusion ----------------------------------------------------------

A printing extrusion is one printable cloned root plus the source information
needed to interpret it. region_island and role identify where the clone came
from. object_instance_idx is context only; plan geometry should already be in
the coordinate system expected by later consumers.
*/
SLIC3R_HOST_API const layer_region_island_handle *printing_extrusion_get_region_island(
    const printing_extrusion_handle *me);
SLIC3R_HOST_API raw_extrusion_role printing_extrusion_get_role(const printing_extrusion_handle *me);
SLIC3R_HOST_API void printing_extrusion_set_role(printing_extrusion_handle *me, raw_extrusion_role role);
SLIC3R_HOST_API uint16_t printing_extrusion_get_object_instance_idx(const printing_extrusion_handle *me);
SLIC3R_HOST_API void printing_extrusion_set_object_instance_idx(printing_extrusion_handle *me,
                                                               uint16_t object_instance_idx);
/* Access the owned root. Mutating this root changes only the PrintingPlan copy,
   never the source LayerRegionIsland. */
SLIC3R_HOST_API extrusion_entity_handle *printing_extrusion_get_root_mutable(printing_extrusion_handle *me);
SLIC3R_HOST_API const extrusion_entity_handle *printing_extrusion_get_root(const printing_extrusion_handle *me);
/* Replace the owned root. clone keeps the source unchanged; move transfers
   content out of the mutable source handle. */
SLIC3R_HOST_API int32_t printing_extrusion_set_root_clone(printing_extrusion_handle *me,
                                                          const extrusion_entity_handle *root);
SLIC3R_HOST_API int32_t printing_extrusion_set_root_move(printing_extrusion_handle *me,
                                                         extrusion_entity_handle *root);

#ifdef __cplusplus
}
#endif

#endif // slic3r_printing_plan_h_
