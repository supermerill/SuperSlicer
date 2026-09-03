///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_step_support_demand_h_
#define slic3r_step_support_demand_h_

#define SLIC3R_PLUGIN_API_STEP_SUPPORT_DEMAND_MAJOR 1u
#define SLIC3R_PLUGIN_API_STEP_SUPPORT_DEMAND_MINOR 0u

#include "slic3r_step_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
Payload for STEP_SUPPORT_DEMAND.

This step builds the list of island areas that ask for support. It runs before
support generation: the output of the last plugin in this step becomes the
support demand consumed by later support steps.

Normal plugin usage:

1. Cast the generic plugin_run_context with plugin_ctx_as_support_demand().
   If it returns NULL, the current run is not STEP_SUPPORT_DEMAND.

2. Read the object geometry from ctx->object. The object, its layers, and its
   layer islands are borrowed host handles. They are stable only for the
   current setup_run()/run() call and must not be stored for later runs.

3. Read or edit the current demand set through ctx->demand and the callbacks in
   run_ctx_support_demand. The demand set is keyed by layer_island_handle:
   each entry says "this island has these ExPolygons asking for support".

4. To add or replace demand for one island, create/fill an
   expolygon_collection_handle in plugin storage, then call ctx->set().
   ctx->set() moves the collection contents into the demand set, so the source
   handle remains valid but should be considered empty afterwards.

5. To refine previous plugins, iterate islands with entry_count() and
   entry_island(), then fetch each island's polygons with get(). You may mutate
   the returned expolygon_collection_handle directly.

6. To remove demand for one island, call set() with polygons == NULL, or with
   an empty polygon collection. To discard every demand entry, call clear().

The print/object data tree is read-only context for this step. Only the demand
working set is mutable.
*/
typedef struct support_demand_handle support_demand_handle;

/*
Return the number of demand entries currently stored in demand.

The count may change after set() or clear(). If a plugin
mutates the demand set while iterating, it should query the count again or use a
simple restart/while loop strategy instead of keeping old indices.
*/
typedef uint32_t (*support_demand_entry_count_fn)(const support_demand_handle *demand);

/*
Return the layer island key for entry idx.

The returned handle is borrowed from the host object data tree. It identifies
which layer island owns the demand polygons at the same idx. Returns NULL when
demand is NULL or idx is invalid.
*/
typedef const layer_island_handle *(*support_demand_entry_island_fn)(const support_demand_handle *demand, uint32_t idx);

/*
Return mutable demand polygons for one layer island.

Returns NULL if this island currently has no demand entry. The returned handle
is owned by the demand working set. Do not release it with storage_free().
Mutating this collection immediately mutates the demand entry. The handle may be
invalidated by set() or clear() on the same support_demand_handle.
*/
typedef expolygon_collection_handle *(*support_demand_get_fn)(support_demand_handle *demand,
                                                              const layer_island_handle *island);

/*
Replace the demand polygons for one layer island.

polygons is a mutable collection handle created by the plugin, usually through
storage_new_expolygons(ctx->plugin_storage). The host moves the contents of
polygons into the demand set; it does not take ownership of the handle itself.
After the call, polygons remains valid for the plugin but its content is empty
or otherwise unspecified by the host move operation.

If an entry already exists for island, it is replaced. If it does not exist, it
is created. If polygons is NULL, or if polygons is empty, the island entry is
removed instead. Empty demand entries are not kept in the demand set.

Returns non-zero on success.
*/
typedef int32_t (*support_demand_set_fn)(support_demand_handle *demand,
                                         const layer_island_handle *island,
                                         expolygon_collection_handle *polygons);

/*
Remove every demand entry from the working set.

Use this when a plugin intentionally replaces the entire demand result instead
of refining previous plugins.
*/
typedef void (*support_demand_clear_fn)(support_demand_handle *demand);

typedef struct run_ctx_support_demand {
    /*
    Borrowed print context. It gives access to global print config and objects.
    The handle is read-only for this step and valid only during the callback.
    */
    const print_handle *print;

    /*
    Borrowed object currently being processed. Plugins usually iterate its
    layers and layer islands to decide where support is needed.
    */
    const object_handle *object;

    /*
    Mutable island-keyed demand working set. Do not access it directly; use the
    callbacks below so the host can keep its internal representation consistent.
    */
    support_demand_handle *demand;

    /*
    Enumerate current demand entries.

    entry_count() gives the number of entries. For each index in
    [0, entry_count), entry_island() returns the island key. Use get() with that
    key to access the mutable ExPolygons for the island.
    */
    support_demand_entry_count_fn entry_count;
    support_demand_entry_island_fn entry_island;

    /*
    Find the mutable demand polygons for one island. Returns NULL if the island
    does not currently ask for support.
    */
    support_demand_get_fn get;

    /*
    Add or replace demand for one island by moving polygon contents from a
    plugin-owned collection handle. Passing NULL or an empty collection removes
    the island entry.
    */
    support_demand_set_fn set;

    /* Remove all island entries from the demand working set. */
    support_demand_clear_fn clear;
} run_ctx_support_demand;

/*
Return the STEP_SUPPORT_DEMAND payload from a generic plugin_run_context.

Returns NULL when ctx is NULL, when ctx belongs to another step, or when the
host did not provide this payload. Plugins should call this once at the
beginning of setup_run()/run() and use the returned pointer for the rest of the
callback.
*/
static inline const run_ctx_support_demand *
plugin_ctx_as_support_demand(const plugin_run_context *ctx)
{
    if (!ctx || ctx->step != STEP_SUPPORT_DEMAND)
        return NULL;
    return (const run_ctx_support_demand *)ctx->data;
}

#ifdef __cplusplus
}
#endif

#endif // slic3r_step_support_demand_h_
