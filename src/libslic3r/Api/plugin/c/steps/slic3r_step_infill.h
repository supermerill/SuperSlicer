///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_step_infill_h_
#define slic3r_step_infill_h_

#define SLIC3R_PLUGIN_API_STEP_INFILL_MAJOR 1u
#define SLIC3R_PLUGIN_API_STEP_INFILL_MINOR 0u

#include "slic3r_step_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
Runtime id for an active INFILL_PATTERN plugin.

The value is assigned by the STEP_INFILL host wrapper for the current process
run. It is intentionally not serialized: config files and presets keep using
the stable string plugin id. Recipe code uses this compact id only after asking
the host to resolve a string id for the current active pattern set.
*/
typedef uint32_t infill_pattern_runtime_id;

#define INFILL_PATTERN_RUNTIME_ID_INVALID ((infill_pattern_runtime_id)0)

/*
Parameters prepared by STEP_INFILL before it calls one INFILL_PATTERN plugin.

The pattern plugin should treat this struct as the complete per-surface recipe:
the host has already resolved the selected pattern id, role, flow, density,
angle, overlap and gap-fill flags from the print/object/region settings.

All distances stored as coord_t are scaled integer coordinates. All double or
float distances are unscaled millimeters unless the field comment says
otherwise. Angles are radians.
*/
typedef struct raw_infill_pattern_params {
    /*
    Runtime id of the active INFILL_PATTERN plugin selected for this surface.

    STEP_INFILL generators obtain it with resolve_pattern_id(). A recipe
    modifier may replace it with another resolved runtime id. Pattern plugins
    normally do not need to read this value; they are called because the host
    selected their plugin from this id.
    */
    infill_pattern_runtime_id pattern_id;

    raw_surface_type surface_type;
    raw_extrusion_role extrusion_role;
    c_flow flow;

    float density;
    float flow_mult;
    int32_t connection;

    /*
    add_gap_fill is the exact per-surface decision that old FillParams exposed:
    a solid/full pattern may split its own area into normal infill plus a gap
    fill pass. gap_fill_enabled is the broader pipeline state, useful to
    pattern implementations that want to leave narrow residuals for a later
    post-process instead of trying to cover every sliver themselves.
    */
    int32_t add_gap_fill;
    int32_t gap_fill_enabled;

    int32_t dont_adjust;
    int32_t monotonic;
    int32_t fill_exactly;
    int32_t complete;
    int32_t use_arachne;
    int32_t can_angle_cross;

    uint32_t extruder;
    int32_t priority;

    double spacing;
    double angle;
    double bridge_angle;
    int32_t bridge_type;
    double layer_height;
    double z;
    uint32_t layer_id;
    double overlap;

    coord_t bridge_offset;
    coord_t fill_resolution;
    coord_t link_max_length;
    coord_t loop_clipping;
    float anchor_length;
    float anchor_length_max;
    float max_sparse_infill_spacing;
} raw_infill_pattern_params;

/*
Resolve a stable INFILL_PATTERN plugin id to the runtime id used by this run.

pattern_plugin_id is the serialized string value stored in the project config,
for example "rectilinear" or "line". The callback returns a non-zero runtime id
for an active pattern plugin. If the requested plugin is not active, the host
may report a warning and return a fallback pattern so slicing can continue.

The ctx argument is the parent run_ctx_generate_infill. The callback needs it
because this is a C ABI: function pointers do not capture the host registry
state by themselves.
*/
typedef infill_pattern_runtime_id (*infill_resolve_pattern_id_fn)(
    const struct run_ctx_generate_infill *ctx,
    const char *pattern_plugin_id);

/*
Return the stable plugin id behind a runtime INFILL_PATTERN id.

This is mostly for diagnostics and tests. The returned pointer is borrowed from
the host plugin registry and must not be stored beyond the current callback.
As with resolve_pattern_id(), ctx is the parent run_ctx_generate_infill used to
reach the host-side runtime registry.
*/
typedef const char *(*infill_pattern_plugin_id_fn)(
    const struct run_ctx_generate_infill *ctx,
    infill_pattern_runtime_id pattern_id);

/*
Generate one surface with the INFILL_PATTERN plugin selected by pattern_id.

STEP_INFILL plugins should call this instead of directly looking up pattern
plugins. The host owns plugin selection, stale-pattern fallback, per-pattern
setup_run(), progress plumbing and error reporting. output must be an empty
mutable extrusion entity owned by the caller for the duration of the call; the
selected pattern writes its generated paths into it.
*/
typedef int32_t (*infill_generate_pattern_fn)(
    const struct run_ctx_generate_infill *ctx,
    infill_pattern_runtime_id pattern_id,
    const layer_handle *layer,
    const layer_island_handle *island,
    const layer_region_island_handle *region_island,
    const layer_region_handle *primary_region,
    const surface_handle *surface,
    const expolygon_collection_handle *no_overlap_areas,
    const raw_infill_pattern_params *params,
    extrusion_entity_handle *output);

/*
Run all active INFILL_SURFACE_RECIPE_MODIFIER plugins for one fill surface.

STEP_INFILL generators should call this after creating the default
raw_infill_pattern_params and before calling generate_pattern(). The host calls
the active recipe modifiers in plugin priority order. Each modifier receives the
same mutable params, so later modifiers see changes made by earlier modifiers.
*/
typedef int32_t (*infill_modify_surface_recipe_fn)(
    const struct run_ctx_generate_infill *ctx,
    const layer_handle *layer,
    const layer_island_handle *island,
    const layer_region_island_handle *region_island,
    const layer_region_handle *primary_region,
    const surface_handle *surface,
    const expolygon_collection_handle *no_overlap_areas,
    raw_infill_pattern_params *params);

/*
Move generated extrusion into the LayerRegionIsland bucket matching role.

The host receives ownership by moving children out of extrusion. The caller may
reuse or destroy extrusion after the function returns, but must not expect it to
still contain the moved paths.
*/
typedef int32_t (*infill_append_region_island_extrusion_fn)(
    layer_region_island_handle *region_island,
    raw_extrusion_role role,
    extrusion_entity_handle *extrusion);

/*
Payload for STEP_INFILL.

Normal usage:
- iterate the object layers, islands, region islands and fill surfaces;
- build one raw_infill_pattern_params for each fill surface to generate;
- call modify_surface_recipe() to let recipe modules adjust the per-surface
  recipe;
- call generate_pattern() to delegate line creation to the selected
  INFILL_PATTERN plugin;
- call append_region_island_extrusion() to publish non-empty output.

host_context is an opaque pointer reserved for the host callbacks. Plugins must
pass the original run_ctx_generate_infill pointer back to callbacks and must not
inspect or store host_context.
*/
typedef struct run_ctx_generate_infill {
    const print_handle *print;
    const object_handle *object;
    void *host_context;

    infill_resolve_pattern_id_fn resolve_pattern_id;
    infill_pattern_plugin_id_fn pattern_plugin_id;
    infill_generate_pattern_fn generate_pattern;
    infill_modify_surface_recipe_fn modify_surface_recipe;
    infill_append_region_island_extrusion_fn append_region_island_extrusion;
} run_ctx_generate_infill;

static inline const run_ctx_generate_infill *
plugin_ctx_as_generate_infill(const plugin_run_context *ctx)
{
    if (!ctx || ctx->step != STEP_INFILL)
        return NULL;
    return (const run_ctx_generate_infill *)ctx->data;
}

/*
Payload for INFILL_PATTERN service plugins.

Normal usage:
- read surface and no_overlap_areas as the geometry to fill;
- read params as the already-resolved process settings;
- write generated extrusion into output;
- return with output empty when this surface produces no extrusion.

output is a mutable, storage-independent extrusion entity borrowed from the
host. It starts empty for each call and is valid only during the call. The host
moves its children under the LayerRegionIsland infill root if the plugin writes
anything into it.
*/
typedef struct run_ctx_infill_pattern {
    const print_handle *print;
    const object_handle *object;
    const layer_handle *layer;
    const layer_island_handle *island;
    const layer_region_island_handle *region_island;
    const layer_region_handle *primary_region;
    const surface_handle *surface;
    const expolygon_collection_handle *no_overlap_areas;
    const raw_infill_pattern_params *params;
    extrusion_entity_handle *output;
} run_ctx_infill_pattern;

static inline const run_ctx_infill_pattern *
plugin_ctx_as_infill_pattern(const plugin_run_context *ctx)
{
    if (!ctx || ctx->step != INFILL_PATTERN)
        return NULL;
    return (const run_ctx_infill_pattern *)ctx->data;
}

/*
Payload for INFILL_SURFACE_RECIPE_MODIFIER service plugins.

Normal usage:
- inspect surface, no_overlap_areas, layer/island context and plugin
  properties stored by earlier surface-generation plugins;
- edit params in place to change how this surface will be filled;
- optionally replace params->pattern_id with another id returned by
  resolve_pattern_id().

This payload is deliberately small and per-surface. Recipe modifiers should not
split surfaces or create extrusion. Geometry changes belong in surface
generation or post-infill plugins; this service only changes the recipe consumed
by the selected infill pattern.
*/
typedef struct run_ctx_infill_surface_recipe_modifier {
    const print_handle *print;
    const object_handle *object;
    const layer_handle *layer;
    const layer_island_handle *island;
    const layer_region_island_handle *region_island;
    const layer_region_handle *primary_region;
    const surface_handle *surface;
    const expolygon_collection_handle *no_overlap_areas;

    /*
    Parent STEP_INFILL context required by resolve_pattern_id() and
    pattern_plugin_id().

    The modifier payload is intentionally small and does not duplicate the host
    plugin registry state. Pass this pointer back to the resolver callbacks when
    replacing params->pattern_id:

        params->pattern_id =
            ctx->resolve_pattern_id(ctx->generate_infill_ctx, "line");
    */
    const struct run_ctx_generate_infill *generate_infill_ctx;
    infill_resolve_pattern_id_fn resolve_pattern_id;
    infill_pattern_plugin_id_fn pattern_plugin_id;
    raw_infill_pattern_params *params;
} run_ctx_infill_surface_recipe_modifier;

static inline const run_ctx_infill_surface_recipe_modifier *
plugin_ctx_as_infill_surface_recipe_modifier(const plugin_run_context *ctx)
{
    if (!ctx || ctx->step != INFILL_SURFACE_RECIPE_MODIFIER)
        return NULL;
    return (const run_ctx_infill_surface_recipe_modifier *)ctx->data;
}

#ifdef __cplusplus
}
#endif

#endif // slic3r_step_infill_h_
