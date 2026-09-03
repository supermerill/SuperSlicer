///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_step_perimeter_h_
#define slic3r_step_perimeter_h_

#define SLIC3R_PLUGIN_API_STEP_PERIMETER_MAJOR 1u
#define SLIC3R_PLUGIN_API_STEP_PERIMETER_MINOR 0u

#include "slic3r_step_common.h"
#include "../slic3r_extrusion_entity.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
Payload for STEP_PERIMETER.

This step generates perimeters for one layer island of one object.

The input island may intersect several layer regions. A perimeter plugin should
decide which regions can be processed together, usually by grouping regions
whose perimeter-relevant configuration is compatible. For each group, the plugin
calls run_region_group(). The host then owns the temporary perimeter tree,
calls PerimeterGenerationModule plugins around each generated ring, and
publishes the final extrusion and fill areas into the data tree.

Normal plugin usage:

1. Cast the generic plugin_run_context with plugin_ctx_as_generate_perimeter().
   If it returns NULL, the current run is not STEP_PERIMETER.

2. Read the object/layer/island inputs from print, object, layer and island.
   These handles are borrowed from the host and are valid only for the current
   callback.

3. Inspect the layer regions attached to the island and build one or more
   region groups. Each group is represented by an array of layer_region_handle
   pointers. The array order has no semantic meaning; the group is treated as a
   set.

4. For each group, call run_region_group(ctx, regions, count, root_area,
   generator_state, generate_node).

5. The host calls generate_node once for each perimeter tree node that still
   needs another perimeter. generate_node writes the current node extrusion into
   node->extrusions and fills the two output polygon collections with child
   areas:
       - inner_areas_out: the geometric area available for the next ring;
       - inner_fill_areas_out: the area later used for infill anchoring.

6. The host builds child nodes from these output collections, runs modules, then
   publishes the aggregated result.

The set_region_island_*() callbacks are low-level host services kept for
experiments and compatibility. New perimeter generators should prefer
run_region_group(), because it keeps tree traversal and module ordering in one
host-owned implementation. If a generator needs an additional destination
LayerRegionIsland, use layer_island_get_or_create_region_island() from the
generic data-tree API.

The print/object/layer/island handles are processing context. Do not store them
for another run. If a plugin needs persistent data, store it in plugin storage
or in data owned by the plugin instance.
*/

typedef struct perimeter_node perimeter_node;
typedef struct perimeter_generation_context perimeter_generation_context;

/*
Generate one perimeter ring for a node.

generator_context is the opaque pointer passed by the plugin to
run_region_group(). It usually points to a small plugin-side state object for
the current region group.

node->area is the input area. The generator should write the generated
extrusion into node->extrusions. The host provides two empty mutable polygon
collections:

- inner_areas_out receives one polygon per child node to process next.
- inner_fill_areas_out optionally receives matching fill/anchor areas.
  If it is left empty or its size does not match inner_areas_out, the host
  will use each child area as its own fill area.

Return non-zero on success. Returning zero aborts this region group.
*/
typedef int32_t (*perimeter_generate_node_fn)(
    void *generator_context,
    perimeter_generation_context *context,
    perimeter_node *node,
    expolygon_collection_handle *inner_areas_out,
    expolygon_collection_handle *inner_fill_areas_out);

/*
Run the host-owned perimeter loop for one island/region group.

regions is the group selected by the perimeter plugin. root_area is the area
to process for the first perimeter ring; it is usually the whole island slice,
but a generator may pass a region-specific subset if it already split the
island.

The callback is synchronous: generator_context only needs to stay alive until
run_region_group() returns.
*/
typedef int32_t (*perimeter_run_region_group_fn)(
    const struct run_ctx_generate_perimeter *ctx,
    const layer_region_handle *const *regions,
    uint32_t region_count,
    const expolygon_handle *root_area,
    void *generator_context,
    perimeter_generate_node_fn generate_node);

/*
Move one extrusion entity into a layer region island.

role selects which extrusion bucket is replaced, for example perimeter or gap
fill. Passing extrusion == NULL clears the bucket for that role. Returns
non-zero on success.
*/
typedef int32_t (*perimeter_set_region_island_extrusion_fn)(
    layer_region_island_handle *region_island,
    raw_extrusion_role role,
    extrusion_entity_handle *extrusion);

/*
Move a surface collection into a layer region island.

Passing surfaces == NULL clears the destination collection. The same callback
shape is used for normal fill surfaces and non-encroaching/no-overlap fill
surfaces; the field name in run_ctx_generate_perimeter selects the destination.
Returns non-zero on success.
*/
typedef int32_t (*perimeter_set_region_island_surfaces_fn)(
    layer_region_island_handle *region_island,
    surface_collection_handle *surfaces);

typedef struct run_ctx_generate_perimeter {
    /*
    Borrowed print and object currently being processed.
    */
    const print_handle *print;
    const object_handle *object;

    /*
    Borrowed layer and island currently being processed.
    */
    const layer_handle *layer;
    const layer_island_handle *island;

    /*
    Host-private pointer used by callbacks. Plugins must not inspect or store
    it; pass the run_ctx_generate_perimeter pointer back to run_region_group().
    */
    void *host_context;

    /*
    Preferred entry point for perimeter generators.

    The plugin chooses compatible layer regions, then delegates the perimeter
    node tree, module calls and final publication to the host.
    */
    perimeter_run_region_group_fn run_region_group;

    /*
    Publish perimeter/gap-fill/other extrusions into a region island.
    */
    perimeter_set_region_island_extrusion_fn set_region_island_extrusion;

} run_ctx_generate_perimeter;

/*
Perimeter generation node.

This is the C view of the host-owned working tree used while creating perimeter
rings for one island/region area.

The root node represents the initial area. Each generated ring is written to
node->extrusions, and each remaining inner area becomes one child node. A
module can inspect or edit the current node before/after the generator creates
one ring.

Lifetime rules:
- all pointers are borrowed from the host perimeter loop;
- do not free area, fill_area, extrusions, children, or child nodes;
- pointers are valid only during the current perimeter-generation callback;
- child array storage may change when helper layers add/remove/split children.

Low-level C code may read and update the scalar counters directly. Structural
edits such as splitting nodes, appending children, or rebuilding child arrays
are intentionally left to C++/Python helper layers so this ABI stays compact.
*/
struct perimeter_node {
    /*
    Parent node, or NULL for the root.
    */
    struct perimeter_node *parent;

    /*
    Area available for the next perimeter ring. The handle is mutable so modules
    can refine the current node area before the next ring is generated.
    */
    expolygon_handle *area;

    /*
    Area later used as fill clipping/anchoring support. It is often equal to
    area, but modifiers may keep it larger so infill can anchor into already
    generated perimeters.
    */
    expolygon_handle *fill_area;

    /*
    Extrusion entity owned by this node. After a ring is generated this usually
    contains the perimeter extrusion(s) for the node. Modules may edit it, for
    example to tag overhangs, remove gap fill on overhangs, or mark scarf seams.
    */
    extrusion_entity_handle *extrusions;

    /*
    Direct child nodes. The array is borrowed and contains child_count entries.
    Treat it as read-only unless a helper explicitly documents that it owns the
    structural mutation it performs.
    */
    struct perimeter_node **children;
    uint32_t child_count;

    /*
    Index of the perimeter ring represented by this node, starting at zero for
    the root outer ring.
    */
    uint32_t perimeter_idx;

    /*
    Total number of perimeter rings requested for this branch. Modules may
    increase or decrease it to ask the generator to continue or stop a branch.
    */
    uint32_t perimeter_needed;
};

/*
Context shared by PerimeterGenerationModule callbacks.

A PerimeterGenerationModule is not a full slicing step. It is a service module
called by the host perimeter loop while it walks the perimeter-node tree chosen
by a STEP_PERIMETER plugin. The module receives the same object/layer/island
context as the generator plus the root node of the current area.

Use run_ctx for cancellation/progress/error callbacks and plugin storage. It is
the same common run context shape used by normal plugins, but it belongs to the
perimeter generation call that is currently invoking the module.
*/
/*
Borrowed list of perimeter nodes.

The array and the nodes are owned by the host perimeter loop. The array is only
valid until the next structural edit on the same tree. Plugin code should read
or edit the pointed nodes immediately, then discard the span.
*/
typedef struct perimeter_node_span {
    perimeter_node **items;
    uint32_t count;
} perimeter_node_span;

/*
Split one child node with clip.

The host loop owns node storage, so structural edits go through this callback.
If part of node->area is inside clip and part is outside, the generator may
keep the original node for one part and create sibling nodes for the remaining
parts. It writes into inside_nodes_out the exact nodes whose area is inside
clip. The plugin must not assume where those nodes are stored in the parent
child array: a generator may insert them near the source node or append them at
the end.

If no part of node is inside clip, inside_nodes_out->count is zero. If the whole
node is inside clip, inside_nodes_out usually contains only node and no
structural edit is needed.
*/
typedef void (*perimeter_node_split_fn)(perimeter_generation_context *context,
                                        perimeter_node *node,
                                        const expolygon_collection_handle *clip,
                                        perimeter_node_span *inside_nodes_out);

/*
Replace node children with one child per area.

This is for modules that remove or reshape the extrusion generated on node and
therefore need the next perimeter pass to continue from a different set of
inner areas. The host loop owns the child storage. areas contains the new node
areas. fill_areas may be NULL; if it is provided, the generator should pick the
best fill area for each new child.

The callback may rebuild the child array and invalidate child indexes. Existing
perimeter_node pointers remain valid only if the generator documents that.
Modules should query node->children again after calling this function.
*/
typedef void (*perimeter_node_rebuild_children_fn)(perimeter_generation_context *context,
                                                   perimeter_node *node,
                                                   const expolygon_collection_handle *areas,
                                                   const expolygon_collection_handle *fill_areas);

struct perimeter_generation_context {
    plugin_run_context *run_ctx;
    const print_handle *print;
    const object_handle *object;
    const layer_handle *layer;
    const layer_island_handle *island;
    layer_region_island_handle *region_island;
    perimeter_node *root;
    /*
    Host-private state used by split_node/rebuild_children. This is not the
    generator_context pointer passed to perimeter_generate_node_fn.
    */
    void *generator_context;
    perimeter_node_split_fn split_node;
    perimeter_node_rebuild_children_fn rebuild_children;
};

typedef struct perimeter_generation_module_vtable perimeter_generation_module_vtable;

/*
Created module instance.

The ctx pointer is owned by the module provider. PerimeterGenerationModule
instances are long-lived singletons registered/initialized at startup. The
perimeter generator only borrows them and must not free them.
*/
typedef struct perimeter_generation_module_instance {
    void *ctx;
    const perimeter_generation_module_vtable *vt;
} perimeter_generation_module_instance;

/*
Create the temporary state used by one module while one perimeter tree is being
processed.

module_ctx is the long-lived context published by the module provider in
perimeter_generation_module_instance. module_user_context, the returned pointer,
belongs to this one start/before/after/end sequence only. The host stores it and
passes it back to before(), after() and end().

The host never interprets or frees the returned pointer. If start() allocates
memory with malloc/new, the same module must release it from end(). Returning
NULL is valid for stateless modules.
*/
typedef void *(*perimeter_generation_module_start_fn)(void *module_ctx,
                                                      perimeter_generation_context *context);
typedef void (*perimeter_generation_module_node_fn)(void *module_ctx,
                                                    void *module_user_context,
                                                    perimeter_generation_context *context,
                                                    perimeter_node *node);
typedef void (*perimeter_generation_module_end_fn)(void *module_ctx,
                                                   void *module_user_context,
                                                   perimeter_generation_context *context);

/*
Function table for a PerimeterGenerationModule.

Call order for one generated area:

1. start(context)
   Called once after the root node has been initialized and before any node is
   processed. Use it to initialize per-area caches or edit root counters.
   Its return value is the temporary module_user_context for this area.

2. before(context, module_user_context, node)
   Called before the perimeter generator creates one ring for node.

3. after(context, module_user_context, node)
   Called after the generator has written node->extrusions and created/updated
   node children for the inner areas.

4. end(context, module_user_context)
   Called once after every pending node has been processed, before the generator
   publishes results back into the layer data tree. If start() returned an
   allocated pointer, end() is responsible for freeing it.

before/after may be called many times. start/end are called exactly once for the
area if generation starts normally. If generation is aborted after start(),
the host should still call end() when it can do so safely.
*/
struct perimeter_generation_module_vtable {
    perimeter_generation_module_start_fn start;
    perimeter_generation_module_node_fn before;
    perimeter_generation_module_node_fn after;
    perimeter_generation_module_end_fn end;
};

/*
Payload for PERIMETER_GENERATION_MODULE.

This service plugin type creates a PerimeterGenerationModule instance. The
provider fills module.ctx and module.vt during run(). The STEP_PERIMETER plugin
then calls the module callbacks while generating perimeters.
*/
typedef struct run_ctx_perimeter_generation_module {
    perimeter_generation_module_instance module;
} run_ctx_perimeter_generation_module;

static inline const run_ctx_generate_perimeter *
plugin_ctx_as_generate_perimeter(const plugin_run_context *ctx)
{
    if (!ctx || ctx->step != STEP_PERIMETER)
        return NULL;
    return (const run_ctx_generate_perimeter *)ctx->data;
}

static inline run_ctx_perimeter_generation_module *
plugin_ctx_as_perimeter_generation_module(const plugin_run_context *ctx)
{
    if (!ctx || ctx->step != PERIMETER_GENERATION_MODULE)
        return NULL;
    return (run_ctx_perimeter_generation_module *)ctx->data;
}

#ifdef __cplusplus
}
#endif

#endif // slic3r_step_perimeter_h_
