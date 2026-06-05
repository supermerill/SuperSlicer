///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "DefaultOrdering.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "libslic3r/Api/plugin/c/steps/slic3r_step_ordering.h"
#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"
#include "libslic3r/Api/plugin/cpp/PrintingPlanViews.hpp"

namespace slic3r_api { namespace Ordering { namespace DefaultOrderingPlugin {
namespace {

/*
DefaultPlanBuilder is the first plugin in the ordering chain.

It creates the mutable PrintingPlan from the read-only Print using only the
public plugin views. The source data tree stays untouched: each printable role
bucket from a LayerRegionIsland is cloned into the plan, and the clone is shifted
for the object instance it belongs to. Later ordering plugins can reorder those
clones without changing perimeter, infill or support generation results.

The plan built here has this shape:
  - by-layer printing: one PrintingGroup for the whole print;
  - complete-object printing: one PrintingGroup per object instance;
  - inside a group, PrintingLayerGroups are keyed by scaled print_z;
  - inside a layer group, PrintingToolGroups are keyed by extruder id;
  - each stored PrintingExtrusion owns one cloned role bucket.

When adding new source buckets, keep the bucket list below in sync with
LayerRegionIsland::has_extrusion(). Do not call native PrintingPlan builders
from this plugin: the point of this file is to make construction replaceable by
another STEP_ORDERING plugin.
*/

struct SourceInstance
{
    // Borrowed object view. The plan stores the corresponding opaque object
    // handle only as source context; the extrusion geometry is cloned below.
    Object object;
    // Stable tie-breaks used by the plugin-side complete-object ordering.
    uint32_t object_idx = 0;
    uint32_t instance_idx = 0;
    // Platter-space translation applied to every cloned extrusion root.
    c_point shift = {};
    // Cached object height used by complete_objects_sort=lowz.
    coord_t max_z = 0;
};

const char *k_no_dependencies[] = { nullptr };
const char *k_group_builder = "ordering.plan_builder";
const raw_extrusion_role k_region_island_roles[] = {
    RAW_EXTRUSION_ROLE_SUPPORT_MATERIAL,
    RAW_EXTRUSION_ROLE_SUPPORT_MATERIAL_INTERFACE,
    RAW_EXTRUSION_ROLE_PERIMETER,
    // LayerRegionIsland stores the gap-fill bucket as the legacy Thin role.
    // RAW_EXTRUSION_ROLE_GAP_FILL includes Mixed and would miss this bucket.
    RAW_EXTRUSION_ROLE_INTERNAL_INFILL,
    RAW_EXTRUSION_ROLE_IRONING_INFILL,
    RAW_EXTRUSION_ROLE_MILLING,
};

/*
Build the normal by-layer plan: one PrintingGroup for the whole print, with all
objects contributing to the same Z buckets.
*/
void build_plan_by_layer(const Print &print, const PrintingPlan &plan);

/*
Build the complete-object style plan: one PrintingGroup per object instance.
The group order is chosen from complete_objects_sort when the plugin can express
that choice from public data.
*/
void build_plan_by_object(const Print &print, const PrintingPlan &plan);

/*
Read one bool setting from the Print config exposed to the plugin API.

Missing keys are treated as disabled. This keeps the default builder usable in
small tests that construct a Print without a fully populated preset.
*/
bool config_bool_value(const Config &config, const char *key);

/* Same helper as above, but for floating-point switches such as parallel object mode. */
double config_float_value(const Config &config, const char *key);

/* Read an enum setting as its serialized value, for example "object" or "nearest". */
std::string config_string_value(const Config &config, const char *key);

/*
Choose the initial plan shape.

Complete-object and parallel-object modes need one high-level PrintingGroup per
object instance so later G-code code can keep one object batch independent from
the next. Normal printing uses a single by-layer group so tools and islands can
be ordered across the whole Z.
*/
bool use_object_plan(const Print &print);

/* Return all object instances in model/source order with their shift cached. */
std::vector<SourceInstance> source_instances(const Print &print);

/*
Return the object-instance order for a complete-object plan.

This intentionally stays plugin-side. Exact host model order is not exposed as a
callback; the plugin uses source order, object height, instance Y, or a greedy
nearest-neighbor walk over instance shifts depending on complete_objects_sort.
*/
std::vector<SourceInstance> ordered_source_instances_for_object_plan(const Print &print);

/* Greedy nearest-neighbor ordering used for complete_objects_sort=nearest. */
void order_instances_by_nearest(std::vector<SourceInstance> &instances);

/*
Find or create the PrintingLayerGroup for print_z.

Appending to a group may invalidate older child handles, so callers keep only
indices in the map and request a fresh view for each use.
*/
PrintingLayerGroup layer_group_for_print_z(const PrintingGroup &group,
                                           std::map<coord_t, uint32_t> &layer_index_by_print_z,
                                           coord_t print_z);

/* Store each source Layer pointer once in a PrintingLayerGroup. */
void append_layer_once(const PrintingLayerGroup &layer_group, const Layer &layer);

/* Find or create the tool bucket for one extruder inside one layer group. */
PrintingToolGroup tool_group_for_extruder(const PrintingLayerGroup &layer_group, uint16_t extruder_id);

/* Store each source LayerRegionIsland pointer once in a tool group. */
void append_region_island_once(const PrintingToolGroup &tool_group, const LayerRegionIsland &region_island);

/* Return true when at least one ordering role bucket contains printable work. */
bool region_island_has_printable_extrusions(const LayerRegionIsland &region_island);

/*
Clone every printable role bucket of region_island for one object instance.

The source tree is object-local. The appended plan clone is translated by
instance_shift immediately, so later ordering and G-code code can read plan
coordinates directly.
*/
void append_region_island_instance_extrusions(const PrintingLayerGroup &layer_group,
                                              const LayerRegionIsland &region_island,
                                              c_point instance_shift,
                                              uint32_t instance_idx);

/*
Translate every local polyline in an extrusion tree.

The plugin ABI currently exposes path points and child trees, but not the
position carried by an ExtrusionNop. Normal printable roots are path trees, so
this is enough for the plan clones built here.
*/
void translate_extrusion_tree(MutableExtrusionEntity entity, c_point shift);

/* Sort the layer groups after a construction walk that discovered them object by object. */
void sort_layer_groups_by_print_z(const PrintingGroup &group);

bool config_bool_value(const Config &config, const char *key)
{
    const config_option_handle *option = config_get(config.handle(), key);
    return option != nullptr && ConfigOption(option).get_bool();
}

double config_float_value(const Config &config, const char *key)
{
    const config_option_handle *option = config_get(config.handle(), key);
    return option == nullptr ? 0.0 : ConfigOption(option).get_float();
}

std::string config_string_value(const Config &config, const char *key)
{
    const config_option_handle *option = config_get(config.handle(), key);
    return option == nullptr ? std::string() : ConfigOption(option).serialize();
}

bool use_object_plan(const Print &print)
{
    const Config config = print.config();
    return config_bool_value(config, "complete_objects") ||
           config_float_value(config, "parallel_objects_step") > 0.0;
}

std::vector<SourceInstance> source_instances(const Print &print)
{
    std::vector<SourceInstance> out;
    for (uint32_t object_idx = 0; object_idx < print.object_count(); ++object_idx) {
        const Object object = print.object(object_idx);
        /*
        The data tree stores layers once per PrintObject, not once per physical
        copy. The builder therefore creates one SourceInstance per object copy
        so the later clone step can duplicate the same layer extrusion tree with
        the correct instance shift.
        */
        for (uint32_t instance_idx = 0; instance_idx < object.instance_count(); ++instance_idx) {
            SourceInstance source;
            source.object = object;
            source.object_idx = object_idx;
            source.instance_idx = instance_idx;
            source.shift = object.instance_shift(instance_idx);
            source.max_z = object.max_z();
            out.push_back(source);
        }
    }
    return out;
}

std::vector<SourceInstance> ordered_source_instances_for_object_plan(const Print &print)
{
    std::vector<SourceInstance> out = source_instances(print);
    const std::string sort_mode = config_string_value(print.config(), "complete_objects_sort");

    if (sort_mode == "lowz") {
        /*
        Low-Z mode prints shorter objects first. Equal-height objects keep the
        source object/instance order so the result is deterministic and easy to
        compare in tests.
        */
        std::sort(out.begin(), out.end(),
                  [](const SourceInstance &lhs, const SourceInstance &rhs) {
                      if (lhs.max_z != rhs.max_z)
                          return lhs.max_z < rhs.max_z;
                      if (lhs.object_idx != rhs.object_idx)
                          return lhs.object_idx < rhs.object_idx;
                      return lhs.instance_idx < rhs.instance_idx;
                  });
    } else if (sort_mode == "lowy") {
        /*
        The plugin API exposes the instance center shift, not the full object
        convex hull used by legacy code. Sorting by shift.y keeps the mode
        plugin-side and still gives a stable front-to-back object order.
        */
        std::sort(out.begin(), out.end(),
                  [](const SourceInstance &lhs, const SourceInstance &rhs) {
                      if (lhs.shift.y != rhs.shift.y)
                          return lhs.shift.y < rhs.shift.y;
                      if (lhs.object_idx != rhs.object_idx)
                          return lhs.object_idx < rhs.object_idx;
                      return lhs.instance_idx < rhs.instance_idx;
                  });
    } else if (sort_mode == "nearest") {
        /*
        Nearest mode is intentionally greedy here: it walks from the origin to
        the closest remaining instance center. A later plugin can replace this
        builder if it wants a richer route optimizer.
        */
        order_instances_by_nearest(out);
    }

    return out;
}

void order_instances_by_nearest(std::vector<SourceInstance> &instances)
{
    std::vector<SourceInstance> remaining = std::move(instances);
    c_point current = {};
    instances.clear();
    instances.reserve(remaining.size());

    while (!remaining.empty()) {
        /*
        Each iteration fixes exactly one instance in the output order. We keep
        the selected instance position as the next start point, which makes the
        greedy walk approximate nozzle travel between complete-object batches.
        */
        size_t selected_idx = 0;
        distsqrf_t selected_distance = norm_square(remaining.front().shift - current);

        for (size_t idx = 1; idx < remaining.size(); ++idx) {
            const distsqrf_t candidate_distance = norm_square(remaining[idx].shift - current);
            if (candidate_distance < selected_distance ||
                (candidate_distance == selected_distance && remaining[idx].object_idx < remaining[selected_idx].object_idx)) {
                /*
                The object index tie-break is deliberately simple. It prevents
                equivalent distances from depending on vector erase order.
                */
                selected_idx = idx;
                selected_distance = candidate_distance;
            }
        }

        current = remaining[selected_idx].shift;
        instances.push_back(remaining[selected_idx]);
        remaining.erase(remaining.begin() + selected_idx);
    }
}

PrintingLayerGroup layer_group_for_print_z(const PrintingGroup &group,
                                           std::map<coord_t, uint32_t> &layer_index_by_print_z,
                                           const coord_t print_z)
{
    const std::map<coord_t, uint32_t>::iterator existing = layer_index_by_print_z.find(print_z);
    if (existing != layer_index_by_print_z.end())
        return group.layer_group(existing->second);

    /*
    Child handles may be invalidated when the group appends another layer group.
    Store only the numeric index in the map and fetch a fresh view from the
    parent every time the caller asks for this print_z again.
    */
    const uint32_t layer_idx = group.layer_group_count();
    PrintingLayerGroup layer_group = group.append_layer_group(print_z);
    layer_index_by_print_z.emplace(print_z, layer_idx);
    return layer_group;
}

void append_layer_once(const PrintingLayerGroup &layer_group, const Layer &layer)
{
    /*
    Several object instances may copy extrusions from the same source Layer into
    one PrintingLayerGroup. The source Layer pointer is context, so storing it
    once avoids repeated later scans over the same layer.
    */
    for (uint32_t idx = 0; idx < layer_group.layer_count(); ++idx)
        if (layer_group.layer(idx).same_handle(layer))
            return;
    layer_group.append_layer(layer);
}

PrintingToolGroup tool_group_for_extruder(const PrintingLayerGroup &layer_group, const uint16_t extruder_id)
{
    /*
    Tool groups are local to one print Z. Reusing the existing group keeps all
    roots printed by the same extruder together until the next ordering plugin
    decides whether that group should move.
    */
    for (uint32_t idx = 0; idx < layer_group.tool_group_count(); ++idx) {
        PrintingToolGroup tool_group = layer_group.tool_group(idx);
        if (tool_group.extruder_id() == extruder_id)
            return tool_group;
    }
    return layer_group.append_tool_group(extruder_id);
}

void append_region_island_once(const PrintingToolGroup &tool_group, const LayerRegionIsland &region_island)
{
    /*
    A single LayerRegionIsland may contribute several role buckets to the same
    tool group. The context list should still contain that source island once,
    while PrintingExtrusion entries carry the per-role cloned roots.
    */
    for (uint32_t idx = 0; idx < tool_group.region_island_count(); ++idx)
        if (tool_group.region_island(idx).same_handle(region_island))
            return;
    tool_group.append_region_island(region_island);
}

bool region_island_has_printable_extrusions(const LayerRegionIsland &region_island)
{
    for (const raw_extrusion_role role : k_region_island_roles) {
        if (!region_island.has_extrusion(role))
            continue;
        /*
        Empty buckets are ignored. Creating an empty tool group would look like
        real work to tool-change ordering even though there is nothing to print.
        */
        const ExtrusionEntity root(region_island.extrusion(role));
        if (!root.empty())
            return true;
    }
    return false;
}

void append_region_island_instance_extrusions(const PrintingLayerGroup &layer_group,
                                              const LayerRegionIsland &region_island,
                                              const c_point instance_shift,
                                              const uint32_t instance_idx)
{
    if (!region_island_has_printable_extrusions(region_island))
        return;

    const int32_t raw_extruder_id = region_island.extruder_id();
    /*
    A negative id means the source island has no meaningful extruder. Keep it as
    uint16_t(-1) rather than silently folding it into extruder 0; this makes bad
    source data visible to later debug/ordering code.
    */
    const uint16_t extruder_id = raw_extruder_id < 0 ? uint16_t(-1) : uint16_t(raw_extruder_id);
    PrintingToolGroup tool_group = tool_group_for_extruder(layer_group, extruder_id);

    for (const raw_extrusion_role role : k_region_island_roles) {
        if (!region_island.has_extrusion(role))
            continue;

        const ExtrusionEntity source_root(region_island.extrusion(role));
        if (source_root.empty())
            continue;

        /*
        Each role bucket is cloned as a separate PrintingExtrusion. Keeping the
        role boundary here lets later ordering still distinguish perimeters,
        infill, support, ironing and other buckets without flattening the tree.
        */
        PrintingExtrusion extrusion =
            tool_group.append_extrusion_clone_from_region_island(region_island, role, uint16_t(instance_idx));
        if (!extrusion.valid())
            continue;

        /*
        Source roots are object-local. The plan owns this clone, so translating
        it here makes the plan self-contained in platter coordinates.
        */
        translate_extrusion_tree(extrusion.mutable_root(), instance_shift);
    }

    append_region_island_once(tool_group, region_island);
}

void translate_extrusion_tree(MutableExtrusionEntity entity, const c_point shift)
{
    if (shift.x == 0 && shift.y == 0)
        return;

    if (entity.point_count() > 0) {
        /*
        A node with local points is a leaf path. Translate its polyline and stop:
        by construction it cannot also have children.
        */
        entity.translate(shift);
        return;
    }

    /*
    Collections do not own coordinates directly. Walk their children so every
    printable path under the cloned root receives the same instance shift.
    */
    for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        translate_extrusion_tree(entity.child_mutable(child_idx), shift);
}

void sort_layer_groups_by_print_z(const PrintingGroup &group)
{
    for (uint32_t target_idx = 0; target_idx < group.layer_group_count(); ++target_idx) {
        /*
        Use selection sort over the ABI move primitive. It is not the shortest
        C++ code, but it avoids storing child handles across moves and keeps the
        implementation entirely in plugin-visible operations.
        */
        uint32_t selected_idx = target_idx;
        coord_t selected_z = group.layer_group(target_idx).print_z();
        for (uint32_t candidate_idx = target_idx + 1; candidate_idx < group.layer_group_count(); ++candidate_idx) {
            const coord_t candidate_z = group.layer_group(candidate_idx).print_z();
            if (candidate_z < selected_z) {
                selected_idx = candidate_idx;
                selected_z = candidate_z;
            }
        }
        if (selected_idx != target_idx)
            group.move_layer_group(selected_idx, target_idx);
    }
}

void build_plan_by_layer(const Print &print, const PrintingPlan &plan)
{
    plan.clear();
    PrintingGroup group = plan.append_group();
    if (!group.valid())
        return;

    std::map<coord_t, uint32_t> layer_index_by_print_z;
    for (uint32_t object_idx = 0; object_idx < print.object_count(); ++object_idx) {
        const Object object = print.object(object_idx);

        /*
        The global by-layer group records every physical instance represented by
        the batch. Geometry is duplicated below; this list is only context for
        later ordering and debugging.
        */
        for (uint32_t instance_idx = 0; instance_idx < object.instance_count(); ++instance_idx)
            group.append_object_instance(object, instance_idx);

        for (uint32_t layer_idx = 0; layer_idx < object.layer_count(); ++layer_idx) {
            const Layer layer = object.layer(layer_idx);
            /*
            The map merges layers from several objects when their scaled print_z
            matches. That is the normal by-layer printing contract: all geometry
            at the same Z belongs to one layer batch before tool/island ordering.
            */
            PrintingLayerGroup layer_group =
                layer_group_for_print_z(group, layer_index_by_print_z, layer.print_z());
            append_layer_once(layer_group, layer);

            for (uint32_t island_idx = 0; island_idx < layer.island_count(); ++island_idx) {
                const LayerIsland island = layer.island(island_idx);
                for (uint32_t region_island_idx = 0; region_island_idx < island.region_island_count();
                     ++region_island_idx) {
                    const LayerRegionIsland region_island = island.region_island(region_island_idx);
                    /*
                    The source island exists once per object layer. Duplicate it
                    for every physical object instance so the plan has one
                    shifted printable root per actual copy on the bed.
                    */
                    for (uint32_t instance_idx = 0; instance_idx < object.instance_count(); ++instance_idx)
                        append_region_island_instance_extrusions(layer_group,
                                                                 region_island,
                                                                 object.instance_shift(instance_idx),
                                                                 instance_idx);
                }
            }
        }
    }

    sort_layer_groups_by_print_z(group);
}

void build_plan_by_object(const Print &print, const PrintingPlan &plan)
{
    plan.clear();
    const std::vector<SourceInstance> instances = ordered_source_instances_for_object_plan(print);

    for (const SourceInstance &source : instances) {
        /*
        Complete-object style printing isolates each physical object copy in its
        own group. Later stages may consume one group completely before moving
        to the next group.
        */
        PrintingGroup group = plan.append_group();
        if (!group.valid())
            continue;

        group.append_object_instance(source.object, source.instance_idx);
        std::map<coord_t, uint32_t> layer_index_by_print_z;

        /*
        A complete-object group owns only one object instance. The source layers
        still use object-local geometry, so each cloned root receives this
        instance shift during append_region_island_instance_extrusions().
        */
        for (uint32_t layer_idx = 0; layer_idx < source.object.layer_count(); ++layer_idx) {
            const Layer layer = source.object.layer(layer_idx);
            PrintingLayerGroup layer_group =
                layer_group_for_print_z(group, layer_index_by_print_z, layer.print_z());
            append_layer_once(layer_group, layer);

            for (uint32_t island_idx = 0; island_idx < layer.island_count(); ++island_idx) {
                const LayerIsland island = layer.island(island_idx);
                for (uint32_t region_island_idx = 0; region_island_idx < island.region_island_count();
                     ++region_island_idx) {
                    const LayerRegionIsland region_island = island.region_island(region_island_idx);
                    /*
                    Unlike by-layer mode, only the selected object instance is
                    cloned into this group. That keeps each complete-object
                    batch independent.
                    */
                    append_region_island_instance_extrusions(layer_group,
                                                             region_island,
                                                             source.shift,
                                                             source.instance_idx);
                }
            }
        }

        sort_layer_groups_by_print_z(group);
    }
}

class DefaultPlanBuilder : public PluginBase
{
public:
    static DefaultPlanBuilder &instance(orchestrator_handle *orch)
    {
        static DefaultPlanBuilder s_instance(orch);
        return s_instance;
    }

    explicit DefaultPlanBuilder(orchestrator_handle *orch) : PluginBase(orch) {}

private:
    const char *id_impl() const noexcept override { return "ordering.plan_builder.default"; }
    const char *name_impl() const noexcept override { return "Default printing-plan builder"; }
    const char *description_impl() const noexcept override
    {
        return "Creates the initial ordering work plan from the generated layer-region-island extrusions.";
    }
    const char *exclusive_group_impl() const noexcept override { return k_group_builder; }
    const char *exclusive_group_label_impl() const noexcept override { return "Printing-plan builder"; }
    const char *exclusive_group_tooltip_impl() const noexcept override
    {
        return "Selects how STEP_ORDERING creates the initial printing plan.";
    }
    slicing_step_t step_impl() const noexcept override { return STEP_ORDERING; }
    const char *const *dependencies_impl() const noexcept override { return k_no_dependencies; }
    int32_t priority_impl() const noexcept override { return 0; }

    int32_t used_config_keys(raw_used_config_key *keys) const noexcept override
    {
        if (keys == nullptr)
            return 3;
        keys[0] = raw_used_config_key{"complete_objects", RAW_CO_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE};
        keys[1] = raw_used_config_key{"parallel_objects_step", RAW_CO_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE};
        keys[2] = raw_used_config_key{"complete_objects_sort", RAW_CO_ENUM, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE};
        return 3;
    }

    void run_impl(const plugin_run_context *run_ctx) const override
    {
        const run_ctx_extrusion_ordering *ctx = plugin_ctx_as_extrusion_ordering(run_ctx);
        assert(ctx != nullptr);
        assert(ctx->print != nullptr);
        assert(ctx->plan != nullptr);
        if (ctx == nullptr || ctx->print == nullptr || ctx->plan == nullptr)
            return;

        PrintingPlan plan(ctx->plan);
        const Print print(ctx->print);
        /*
        The builder is the only default ordering plugin that clears the plan.
        Later plugins must preserve the plan and only reorder or annotate it.
        */
        if (use_object_plan(print))
            build_plan_by_object(print, plan);
        else
            build_plan_by_layer(print, plan);
    }
};

} // namespace

void register_default_plan_builder_plugin(orchestrator_handle *orch)
{
    orchestrator_register_plugin(orch, DefaultPlanBuilder::instance(orch).c_instance());
}

}}} // namespace slic3r_api::Ordering::DefaultOrderingPlugin
