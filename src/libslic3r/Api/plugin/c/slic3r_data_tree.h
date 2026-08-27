///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_data_tree_h_
#define slic3r_data_tree_h_

#include <stddef.h>
#include <stdint.h>

#include "slic3r_def.h"
#include "slic3r_extrusions.h"
#include "slic3r_geometry.h"
#include "slic3r_utils.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================= HANDLES ========================= */

typedef struct print_handle print_handle;
typedef struct orchestrator_handle orchestrator_handle;
typedef struct print_region_handle print_region_handle;
typedef struct object_handle object_handle;
typedef struct layer_handle layer_handle;
typedef struct layer_region_handle layer_region_handle;
typedef struct layer_island_handle layer_island_handle;
typedef struct layer_region_island_handle layer_region_island_handle;
typedef struct surface_handle surface_handle;
typedef struct surface_collection_handle surface_collection_handle;
typedef struct plugin_property_container_handle plugin_property_container_handle;
typedef struct config_handle config_handle;

typedef slic3r_property_type plugin_property_type;

#define PLUGIN_PROPERTY_TYPE_INVALID ((plugin_property_type)SLIC3R_PROPERTY_TYPE_INVALID)
#define PLUGIN_PROPERTY_TYPE_LAYER_SUPPORT ((plugin_property_type)SLIC3R_PROPERTY_TYPE_LAYER_SUPPORT)
#define PLUGIN_PROPERTY_TYPE_LAYER_BRIM ((plugin_property_type)SLIC3R_PROPERTY_TYPE_LAYER_BRIM)
#define PLUGIN_PROPERTY_TYPE_LAYER_ADHESION ((plugin_property_type)SLIC3R_PROPERTY_TYPE_LAYER_ADHESION)

/*
Built-in payload stored on auxiliary Layers that represent generated support.

Support layers are ordinary Layer objects in the final data tree. The property
is the part that says "this auxiliary layer is support" and carries the interface_id
value used to alternate support-interface angles.
If a Layer does not have this property, plugin code must treat it as a generic
auxiliary layer, not as support.
*/
typedef struct c_layer_support_property {
    uint32_t interface_id;
    uint32_t reserved;
} c_layer_support_property;

/*
Built-in marker stored on auxiliary Layers that contain object-owned brim.

The payload deliberately has no behavior fields. Its presence says that this
auxiliary layer was created to host brim extrusions belonging to one object.
The reserved field keeps the binary payload non-empty, which is required by the
generic plugin-property backend.
*/
typedef struct c_layer_brim_property {
    uint32_t reserved;
} c_layer_brim_property;

typedef uint32_t raw_layer_adhesion_kind;
typedef uint32_t raw_layer_adhesion_flag;

#define RAW_LAYER_ADHESION_KIND_BRIM  1u
#define RAW_LAYER_ADHESION_KIND_SKIRT 2u

#define RAW_LAYER_ADHESION_FLAG_FIRST_LAYER_ONLY 1u

/*
Built-in marker stored on auxiliary Layers that contain first-layer adhesion.

Auxiliary layers are generic; this payload tells later code whether the layer
contains brim or skirt geometry. `flags` refines the behavior inside that kind.
For skirt, FIRST_LAYER_ONLY means the layer carries the special first-layer
skirt loops that replace the normal skirt only on the first printed Z.
*/
typedef struct c_layer_adhesion_property {
    raw_layer_adhesion_kind kind;
    raw_layer_adhesion_flag flags;
} c_layer_adhesion_property;

typedef struct c_curled_line
{
    c_point a;
    c_point b;
    float curled_height;
} c_curled_line;

/* ========================= SURFACE TYPE ========================= */

/*
Surface type bitmask (mapped from SurfaceType).
Multiple flags can be combined.
*/
typedef uint16_t raw_surface_type;

/* No surface */
#define RAW_SURFACE_TYPE_NONE 0

/* Position flags */

/* Top horizontal surface, visible from the top */
#define RAW_SURFACE_TYPE_POS_TOP (1 << 0)

/* Bottom horizontal surface, visible from the bottom */
#define RAW_SURFACE_TYPE_POS_BOTTOM (1 << 1)

/* Internal sparse infill */
#define RAW_SURFACE_TYPE_POS_INTERNAL (1 << 2)

/* Inner/outer perimeters (mainly for coloring) */
#define RAW_SURFACE_TYPE_POS_PERIMETER (1 << 3)

/* Density flags */

/* Solid infill (100%) */
#define RAW_SURFACE_TYPE_DENS_SOLID (1 << 4)

/* Sparse infill (>0% & <100%) */
#define RAW_SURFACE_TYPE_DENS_SPARSE (1 << 5)

/* Void / combined sparse layers */
#define RAW_SURFACE_TYPE_DENS_VOID (1 << 6)

/* Modifier flags */

/* First bridging layer */
#define RAW_SURFACE_TYPE_MOD_BRIDGE (1 << 7)

/* Second layer over bridge */
#define RAW_SURFACE_TYPE_MOD_OVERBRIDGE (1 << 8)

/* Check if flag is set */
#define RAW_SURFACE_TYPE_HAS(type, flag) (((type) & (flag)) != 0)

/* Add flag */
#define RAW_SURFACE_TYPE_ADD(type, flag) ((type) |= (flag))

/* Remove flag */
#define RAW_SURFACE_TYPE_REMOVE(type, flag) ((type) &= ~(flag))

/* ========================= PLUGIN PROPERTIES ========================= */

/*
Generic typed metadata attached to data-tree objects.

A plugin property is a numeric-keyed byte payload. It is meant for small C
structs such as:

    typedef struct my_surface_info {
        uint32_t priority;
        uint32_t max_layers;
    } my_surface_info;

Register a stable, namespaced key such as "my_plugin.surface_info" with
orchestrator_register_property(), then use the returned numeric id here. The
host copies these payloads when it copies or splits the owning object, so later
plugins can read metadata created by earlier plugins without knowing native C++
classes.

Normal creation flow:

    plugin_property_type type = orchestrator_register_property(
        orch, "my_plugin.surface_info", sizeof(my_surface_info), alignof(my_surface_info));

    my_surface_info *info = (my_surface_info *)plugin_property_get_or_add_data_mutable(
        orch, surface_get_properties(surface), type);

The host looks up the registered size and alignment from orch. A plugin should
not invent its own size/alignment at the point of use, because that would make
two plugins with the same property name disagree silently.

Property access is intentionally mutable even when the object handle itself is
const. The const handle still protects the real slicer object: geometry,
children, regions, extrusions and config are not made mutable by this API. Only
the side-channel metadata container is mutable. This lets a plugin annotate a
const LayerIsland while preserving the rule that the slice itself is read-only.

Threading rule: the host does not lock individual property containers. Parallel
steps usually assign one layer or island to one worker, which is safe. If a
plugin deliberately lets several workers write properties on the same object at
the same time, that plugin must add its own synchronization.

Important rules:
- store plain C-layout data only;
- do not store owning pointers or strings inside the payload;
- use the same orchestrator that registered the property when creating it;
- if get_or_add returns NULL, the type is unknown to the orchestrator or the
  existing payload layout does not match the registered one.
*/
SLIC3R_HOST_API uint32_t plugin_property_count(const plugin_property_container_handle *me);
SLIC3R_HOST_API plugin_property_type plugin_property_type_at(const plugin_property_container_handle *me, uint32_t idx);
SLIC3R_HOST_API int32_t plugin_property_has(const plugin_property_container_handle *me, plugin_property_type type);
SLIC3R_HOST_API uint32_t plugin_property_data_size(const plugin_property_container_handle *me, plugin_property_type type);
SLIC3R_HOST_API const void *plugin_property_data(const plugin_property_container_handle *me, plugin_property_type type);
SLIC3R_HOST_API void *plugin_property_data_mutable(plugin_property_container_handle *me, plugin_property_type type);
SLIC3R_HOST_API void *plugin_property_get_or_add_data_mutable(orchestrator_handle *orch,
                                                              plugin_property_container_handle *me,
                                                              plugin_property_type type);
SLIC3R_HOST_API int32_t plugin_property_remove(plugin_property_container_handle *me, plugin_property_type type);
SLIC3R_HOST_API void plugin_property_clear(plugin_property_container_handle *me);
SLIC3R_HOST_API void plugin_property_copy_all(plugin_property_container_handle *dst,
                                              const plugin_property_container_handle *src);

/* ========================= SURFACE ========================= */

struct c_surface
{
    const expolygon_handle *expolygon;
    raw_surface_type type;
    uint64_t id;
};
/*
Snapshot one Surface into a tiny C value. Prefer the handle API below when the
Surface may grow new fields, or when you need to mutate it.
*/
SLIC3R_HOST_API c_surface surface_c_view(const surface_handle *me);

/* Borrow the geometry owned by a read-only Surface view. */
SLIC3R_HOST_API const expolygon_handle *surface_get_expolygon(const surface_handle *me);

/* Surface type bitmask access. Surfaces are read-only views in the public API. */
SLIC3R_HOST_API raw_surface_type surface_get_type(const surface_handle *me);

/*
Runtime id of this Surface.

The id is unique only inside the current host process. It lets generated
extrusion trees remember which Surface produced them during the same slice.
It is not stable across copies, project saves, reloads, or different runs. A
value of 0 means "no host Surface id", which is possible for plugin-owned
temporary surface snapshots.
*/
SLIC3R_HOST_API uint64_t surface_get_id(const surface_handle *me);

/* Convenience helper for one flag inside the surface type bitmask. */
SLIC3R_HOST_API int32_t surface_get_flag(const surface_handle *me, raw_surface_type flag);

/*
Surface properties.

Surfaces from the host may be read-only as geometry, but their plugin-property
container remains writable for metadata. A plugin that is building a new
SurfaceCollection can append a Surface, then set or copy properties before
publishing the collection.
*/
SLIC3R_HOST_API plugin_property_container_handle *surface_get_properties(const surface_handle *me);

/*
Surface collection storage.

Surface remains a read-only data-tree view. Plugins create temporary
SurfaceCollections in their storage, fill them with copied or moved ExPolygons,
then pass the whole collection to a step callback that moves it into the host
data tree.
*/
SLIC3R_HOST_API surface_collection_handle *storage_new_surface_collection(storage_handle *storage);
SLIC3R_HOST_API void surface_collection_clear(surface_collection_handle *me);
SLIC3R_HOST_API void surface_collection_append_expolygon_copy(surface_collection_handle *me,
                                                              const expolygon_handle *area,
                                                              raw_surface_type surface_type);
SLIC3R_HOST_API void surface_collection_append_expolygon_move(surface_collection_handle *me,
                                                              expolygon_handle *area,
                                                              raw_surface_type surface_type);
SLIC3R_HOST_API void surface_collection_append_expolygons_copy(surface_collection_handle *me,
                                                               const expolygon_collection_handle *areas,
                                                               raw_surface_type surface_type);
SLIC3R_HOST_API void surface_collection_append_expolygons_move(surface_collection_handle *me,
                                                               expolygon_collection_handle *areas,
                                                               raw_surface_type surface_type);
SLIC3R_HOST_API uint32_t surface_collection_size(const surface_collection_handle *me);
SLIC3R_HOST_API const surface_handle *surface_collection_at(const surface_collection_handle *me, uint32_t idx);
SLIC3R_HOST_API surface_handle *surface_collection_at_mutable(surface_collection_handle *me, uint32_t idx);

/* ========================= LAYER ========================= */

SLIC3R_HOST_API coord_t layer_get_height(const layer_handle *me);
SLIC3R_HOST_API coord_t layer_get_print_z(const layer_handle *me);
/* Center Z of the slicing plane. It is exactly print_z - height / 2. */
SLIC3R_HOST_API coord_t layer_get_slice_z(const layer_handle *me);
/* Borrow the object that owns this normal or auxiliary layer. */
SLIC3R_HOST_API const object_handle *layer_get_object(const layer_handle *me);
SLIC3R_HOST_API const expolygon_collection_handle *layer_get_slices(const layer_handle *me);

/*
Curled-line estimates attached to this layer.

They are produced by the host curl-estimation step. Post-perimeter plugins use
them as a local slowdown/flow signal near already curled material without
needing access to the native AABBTreeLines helper.
*/
SLIC3R_HOST_API uint32_t layer_count_curled_line(const layer_handle *me);
SLIC3R_HOST_API c_curled_line layer_get_curled_line(const layer_handle *me, uint32_t idx);

SLIC3R_HOST_API layer_handle *layer_get_upper_layer_mutable(layer_handle *me);
SLIC3R_HOST_API const layer_handle *layer_get_upper_layer(const layer_handle *me);

SLIC3R_HOST_API layer_handle *layer_get_lower_layer_mutable(layer_handle *me);
SLIC3R_HOST_API const layer_handle *layer_get_lower_layer(const layer_handle *me);

// tag that can be used by processed to store some information
SLIC3R_HOST_API plugin_property_container_handle *layer_get_properties(const layer_handle *me);

SLIC3R_HOST_API uint32_t layer_count_region(const layer_handle *me);
SLIC3R_HOST_API layer_region_handle *layer_get_region_mutable(layer_handle *me, uint32_t idx);
SLIC3R_HOST_API const layer_region_handle *layer_get_region(const layer_handle *me, uint32_t idx);

SLIC3R_HOST_API uint32_t layer_count_island(const layer_handle *me);
SLIC3R_HOST_API layer_island_handle *layer_get_island_mutable(layer_handle *me, uint32_t idx);
SLIC3R_HOST_API const layer_island_handle *layer_get_island(const layer_handle *me, uint32_t idx);

/* ========================= LAYER REGION ========================= */

SLIC3R_HOST_API plugin_property_container_handle *layer_region_get_properties(const layer_region_handle *me);

SLIC3R_HOST_API c_flow layer_region_get_flow(const layer_region_handle *me, raw_extrusion_role flow_role);
/* Bridge/overhang flow for a perimeter or infill role. */
SLIC3R_HOST_API c_flow layer_region_get_bridging_flow(const layer_region_handle *me, raw_extrusion_role flow_role);
SLIC3R_HOST_API const expolygon_collection_handle *layer_region_get_slices(const layer_region_handle *me);
SLIC3R_HOST_API c_bounding_box layer_region_get_bounding_box(const layer_region_handle *me);

SLIC3R_HOST_API const layer_handle *layer_region_get_layer(const layer_region_handle *me);
SLIC3R_HOST_API const print_region_handle *layer_region_get_print_region(const layer_region_handle *me);

/* ========================= LAYER ISLAND ========================= */

SLIC3R_HOST_API expolygon_handle *layer_island_get_slice_mutable(layer_island_handle *me);
SLIC3R_HOST_API const expolygon_handle *layer_island_get_slice(const layer_island_handle *me);
SLIC3R_HOST_API c_bounding_box layer_island_get_bounding_box(const layer_island_handle *me);
/* give an expolygon included inside get_slice()  where the infill has to be extruded. */
SLIC3R_HOST_API const expolygon_handle *layer_island_get_infill_slice(const layer_island_handle *me);
/* All ExPolygons where infill has to be extruded. Prefer this when a previous step split the island fill area. */
SLIC3R_HOST_API const expolygon_collection_handle *layer_island_get_infill_areas(const layer_island_handle *me);
SLIC3R_HOST_API c_bounding_box layer_island_get_infill_bounding_box(const layer_island_handle *me);
/* give an expolygon included inside get_infill_slice() where the infill may be extruded if there was no
 * infill-perimeter encroachment. */
SLIC3R_HOST_API const expolygon_handle *layer_island_get_infill_no_overlap_slice(const layer_island_handle *me);
/* All no-overlap/free infill ExPolygons. */
SLIC3R_HOST_API const expolygon_collection_handle *layer_island_get_infill_no_overlap_areas(const layer_island_handle *me);

SLIC3R_HOST_API plugin_property_container_handle *layer_island_get_properties(const layer_island_handle *me);

SLIC3R_HOST_API uint32_t layer_island_count_region(const layer_island_handle *me);
SLIC3R_HOST_API layer_region_handle *layer_island_get_region_mutable(layer_island_handle *me, uint32_t idx);
SLIC3R_HOST_API const layer_region_handle *layer_island_get_region(const layer_island_handle *me, uint32_t idx);

SLIC3R_HOST_API uint32_t layer_island_count_region_island(const layer_island_handle *me);
SLIC3R_HOST_API layer_region_island_handle *layer_island_get_region_island_mutable(layer_island_handle *me, uint32_t idx);
SLIC3R_HOST_API const layer_region_island_handle *layer_island_get_region_island(const layer_island_handle *me, uint32_t idx);

/*
Return the LayerRegionIsland for one island, one region set, and one extruder.

LayerRegionIslands are the mutable work buckets shared by perimeter, surface,
infill, and post-process plugins. A plugin uses this function when it already
knows which regions and which extruder must own the output. The function only
validates and mutates the data tree; it does not decide which extruder a role
should use.

regions must contain LayerRegion handles that belong to island. Passing
regions == NULL or region_count == 0 means "all regions of the island".
extruder_id is a zero-based extruder index. Passing a negative value stores the
generic "no single extruder" value uint16_t(-1), matching existing host data.

Returns NULL when island is invalid, when any region is invalid or belongs to a
different island, or when the final region set is empty.
*/
SLIC3R_HOST_API layer_region_island_handle *layer_island_get_or_create_region_island(
    layer_island_handle *island,
    const layer_region_handle *const *regions,
    uint32_t region_count,
    int32_t extruder_id);

SLIC3R_HOST_API const layer_handle *layer_island_get_layer(const layer_island_handle *me);

/*
Linked islands on adjacent object layers.
*/
SLIC3R_HOST_API uint32_t layer_island_count_lower_island(const layer_island_handle *me);
SLIC3R_HOST_API const layer_island_handle *layer_island_get_lower_island(const layer_island_handle *me, uint32_t idx);
SLIC3R_HOST_API uint32_t layer_island_count_upper_island(const layer_island_handle *me);
SLIC3R_HOST_API const layer_island_handle *layer_island_get_upper_island(const layer_island_handle *me, uint32_t idx);

/* ========================= LAYER REGION ISLAND ========================= */

SLIC3R_HOST_API int32_t layer_region_island_extruder_id(const layer_region_island_handle *me);
SLIC3R_HOST_API int32_t layer_region_island_has_extrusions(const layer_region_island_handle *me);
SLIC3R_HOST_API int32_t layer_region_island_has_extrusion(const layer_region_island_handle *me, raw_extrusion_role role);
SLIC3R_HOST_API extrusion_entity_handle *layer_region_island_get_mutable_extrusion(layer_region_island_handle *me, raw_extrusion_role role);
SLIC3R_HOST_API const extrusion_entity_handle *layer_region_island_get_extrusion(const layer_region_island_handle *me,
                                                          raw_extrusion_role role);

/*
Island-level fill surfaces produced by STEP_SURFACE_GENERATION and refined by
later surface-processing steps. LayerRegion no longer exposes these caches in
the plugin ABI; plugins should read/write fill surfaces through the
LayerRegionIsland that owns the infill work.
*/
SLIC3R_HOST_API const surface_collection_handle *layer_region_island_get_fill_surfaces(const layer_region_island_handle *me);
SLIC3R_HOST_API uint32_t layer_region_island_count_fill_surface(const layer_region_island_handle *me);
SLIC3R_HOST_API const surface_handle *layer_region_island_get_fill_surface(const layer_region_island_handle *me, uint32_t idx);

SLIC3R_HOST_API plugin_property_container_handle *layer_region_island_get_properties(const layer_region_island_handle *me);

/*
Regions owned by this LayerRegionIsland.

Surface-generation plugins use this to refine an existing group without
falling back to the whole LayerIsland. The returned region handles are borrowed
from the host data tree and remain valid during the current step run.
*/
SLIC3R_HOST_API uint32_t layer_region_island_count_region(const layer_region_island_handle *me);
SLIC3R_HOST_API const layer_region_handle *layer_region_island_get_region(const layer_region_island_handle *me,
                                                                          uint32_t idx);

/* ========================= PRINT REGION ========================= */

SLIC3R_HOST_API config_handle *print_region_get_config_mutable(print_region_handle *me);
SLIC3R_HOST_API const config_handle *print_region_get_config(const print_region_handle *me);

/* ========================= OBJECT ========================= */

SLIC3R_HOST_API config_handle *object_get_config_mutable(object_handle *me);
SLIC3R_HOST_API const config_handle *object_get_config(const object_handle *me);
SLIC3R_HOST_API plugin_property_container_handle *object_get_properties(const object_handle *me);

// Deprecated: if not useful, it will be deleted
SLIC3R_HOST_API coord_t object_get_max_z(const object_handle *me);

/*
Transformation from the source model object into this printable Object.

This is the same transform as native PrintObject::trafo(): rotation / scaling /
mirroring / Z translation are included, but the temporary XY centering offset
used to keep Clipper coordinates small is not. To reproduce trafo_centered(),
apply object_get_center_offset() as a pre-translation:
    centered = translate(-unscaled(center.x), -unscaled(center.y), 0) * transform
*/
SLIC3R_HOST_API c_matrix4d object_get_transform(const object_handle *me);

/*
Scaled XY offset used by the host while slicing to center the mesh before it is
sent to Clipper. This is a slicer-space 2D value, so it uses coord_t through
c_point. Convert with unscaled() before composing it with c_matrix4d.
*/
SLIC3R_HOST_API c_point object_get_center_offset(const object_handle *me);

/*
Object instances.

Extrusion trees stored on Layers are in object-local coordinates. A plan or
G-code builder that duplicates those trees for each physical copy must translate
the clone by object_get_instance_shift(). The instance index is stable only for
the current Print object; it is context, not a persistent object identifier.
*/
SLIC3R_HOST_API uint32_t object_count_instance(const object_handle *me);
SLIC3R_HOST_API c_point object_get_instance_shift(const object_handle *me, uint32_t idx);

SLIC3R_HOST_API uint32_t object_count_layer(const object_handle *me);
SLIC3R_HOST_API layer_handle *object_get_layer_mutable(object_handle *me, uint32_t idx);
SLIC3R_HOST_API const layer_handle *object_get_layer(const object_handle *me, uint32_t idx);
/*
Auxiliary layers are exposed as read-only Layer views.

They share the Layer shape used by normal object layers: print_z, height,
slices(), islands(), and plugin properties work the same way. A plugin should
treat them as geometry already produced by earlier steps and must not assume
that their indices match normal object layer indices.

Support is represented by the built-in PLUGIN_PROPERTY_TYPE_LAYER_SUPPORT
property on the Layer. Read it through layer_get_properties() and the generic
plugin_property_* functions; an auxiliary layer without that property is not a
support layer.
*/
SLIC3R_HOST_API uint32_t object_count_auxiliary_layer(const object_handle *me);
/*
Return a mutable auxiliary Layer.

Auxiliary layers are plugin-owned working layers. They are reachable through a
const object_handle because plugins often receive read-only Object views while
still being responsible for generated helper geometry such as support, skirt,
brim or wipe tower. Mutability is intentionally limited to the returned Layer
and its plugin/public layer APIs; it does not make normal object layers mutable.
*/
SLIC3R_HOST_API layer_handle *object_get_auxiliary_layer(const object_handle *me, uint32_t idx);
/*
Create an empty auxiliary Layer and initialize its LayerRegion list from the
Object's PrintRegions.

height, print_z and slice_z are scaled slicer coordinates. slice_z may equal
print_z when the helper geometry lives on the print plane. The new layer is not
automatically filled; callers should write raw LayerRegion slices and then call
layer_recompute_slices_and_islands_from_layer_regions().
*/
SLIC3R_HOST_API layer_handle *object_add_auxiliary_layer(const object_handle *me,
                                                         coord_t height,
                                                         coord_t print_z,
                                                         coord_t slice_z);
/*
Remove an auxiliary Layer previously returned by object_get_auxiliary_layer() or
object_add_auxiliary_layer(). Returns non-zero on success.
*/
SLIC3R_HOST_API int32_t object_remove_auxiliary_layer(const object_handle *me, layer_handle *layer);

/*
Generic raw-slice mutation helpers.

These functions are intentionally not tied to STEP_POST_SLICING. A plugin that
creates an auxiliary layer in a later step still has to rebuild the Layer's
cached island list after writing LayerRegion raw slices. If the layer should be
used by perimeter/surface/infill code immediately, call
layer_add_regions_to_islands() after recomputing. Do not call it on an empty
layer.
*/
SLIC3R_HOST_API expolygon_collection_handle *layer_borrow_mutable_slices(layer_handle *me);
SLIC3R_HOST_API expolygon_collection_handle *layer_region_borrow_mutable_slices(const layer_region_handle *me);
SLIC3R_HOST_API void layer_recompute_slices_from_islands(layer_handle *me);
SLIC3R_HOST_API void layer_recompute_slices_and_islands_from_layer_regions(layer_handle *me);
SLIC3R_HOST_API void layer_add_regions_to_islands(layer_handle *me);

SLIC3R_HOST_API uint32_t object_count_region(const object_handle *me);
SLIC3R_HOST_API print_region_handle *object_get_print_region_mutable(object_handle *me, uint32_t idx);
SLIC3R_HOST_API const print_region_handle *object_get_print_region(const object_handle *me, uint32_t idx);

/* ========================= PRINT ========================= */

SLIC3R_HOST_API config_handle *print_get_config_mutable(print_handle *me);
SLIC3R_HOST_API const config_handle *print_get_config(const print_handle *me);

SLIC3R_HOST_API uint32_t print_count_object(const print_handle *me);

SLIC3R_HOST_API object_handle *print_get_object_mutable(print_handle *me, uint32_t idx);
SLIC3R_HOST_API const object_handle *print_get_object(const print_handle *me, uint32_t idx);
/*
Return the hidden Object that owns print-level auxiliary layers.

This Object is not counted by print_count_object() and is never returned by
print_get_object(). It exists only so plugins can store global helper geometry
with the same Object auxiliary-layer API used by object-local support, brim,
skirt and future generated layers. The returned Object is mutable because
auxiliary layers are plugin-owned work data; normal model-derived objects stay
reachable only through the regular object list above.
*/
SLIC3R_HOST_API object_handle *print_get_auxiliary_object(const print_handle *me);

#ifdef __cplusplus
}
#endif

#endif // slic3r_data_tree_h_
