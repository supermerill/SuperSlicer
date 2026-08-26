///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_PrintingPlanViews_hpp_
#define slic3r_Api_plugin_cpp_PrintingPlanViews_hpp_

#include <cassert>
#include <cstdint>

#include "libslic3r/Api/plugin/c/slic3r_printing_plan.h"
#include "libslic3r/Api/plugin/cpp/DataTreeViews.hpp"
#include "libslic3r/Api/plugin/cpp/ExtrusionViews.hpp"

namespace slic3r_api {

class PrintingPlan;
class PrintingScopeEvents;
class PrintingGroup;
class PrintingLayerGroup;
class PrintingToolGroup;
class PrintingExtrusion;

/*
PrintingPlan C++ views
======================

STEP_ORDERING plugins receive two pieces of data: the read-only Print and a
mutable PrintingPlan. The plan is a work copy owned by the host for the duration
of the ordering step. It points back to source Layers and LayerRegionIslands for
context, but every PrintingExtrusion owns a cloned extrusion root that plugins
may reorder.

Ownership rules:
- These classes are borrowed views. They never free the handles.
- Child views are invalidated when their parent vector is cleared, appended to
  or reordered.
- PrintingExtrusion::root() is the mutable clone that ordering plugins should
  modify. It is not the source LayerRegionIsland extrusion tree.

Typical step flow:

    const run_ctx_extrusion_ordering *ctx = plugin_ctx_as_extrusion_ordering(run_ctx);
    PrintingPlan plan(ctx->plan);
    plan.clear();                                // builder plugin
    PrintingGroup group = plan.append_group();
    // append layer/tool/extrusion groups with the helpers below
    for (uint32_t idx = 0; idx < plan.group_count(); ++idx)
        my_plugin_order_tool_groups(plan.group(idx));
    for (uint32_t idx = 0; idx < plan.group_count(); ++idx)
        my_plugin_order_extrusion_trees(plan.group(idx));

The views expose only plan editing primitives. Algorithms such as by-layer or
by-object construction belong in STEP_ORDERING plugins, where they can be
replaced independently.
*/

class PrintingObjectInstance
{
public:
    explicit PrintingObjectInstance(c_printing_object_instance value) : m_value(value) {}

    Object object() const { return Object(m_value.object); }
    uint64_t instance_idx() const { return m_value.instance_idx; }

private:
    c_printing_object_instance m_value;
};

/*
Borrowed view over the fixed before/after sequences of one plan scope.

The roots are intentionally read-only so plugin code cannot make the enclosing
sequence sortable or replace it. Mutation is limited to appending complete
event trees in their execution order.
*/
class PrintingScopeEvents
{
public:
    explicit PrintingScopeEvents(printing_scope_events_handle *handle) : m_handle(handle) {}
    explicit PrintingScopeEvents(const printing_scope_events_handle *handle) :
        m_handle(const_cast<printing_scope_events_handle *>(handle)) {}

    bool valid() const { return m_handle != nullptr; }
    printing_scope_events_handle *mutable_handle() const { return m_handle; }
    const printing_scope_events_handle *handle() const { return m_handle; }

    bool has_before() const { return printing_scope_events_has_before(handle()) != 0; }
    bool has_after() const { return printing_scope_events_has_after(handle()) != 0; }
    ExtrusionEntity before() const { return ExtrusionEntity(printing_scope_events_get_before(handle())); }
    ExtrusionEntity after() const { return ExtrusionEntity(printing_scope_events_get_after(handle())); }

    MutableExtrusionEntity append_before_clone(const ExtrusionEntity &event) const {
        return MutableExtrusionEntity(
            printing_scope_events_append_before_clone(mutable_handle(), event.handle()));
    }
    MutableExtrusionEntity append_before_move(MutableExtrusionEntity event) const {
        return MutableExtrusionEntity(
            printing_scope_events_append_before_move(mutable_handle(), event.mutable_handle()));
    }
    MutableExtrusionEntity append_after_clone(const ExtrusionEntity &event) const {
        return MutableExtrusionEntity(
            printing_scope_events_append_after_clone(mutable_handle(), event.handle()));
    }
    MutableExtrusionEntity append_after_move(MutableExtrusionEntity event) const {
        return MutableExtrusionEntity(
            printing_scope_events_append_after_move(mutable_handle(), event.mutable_handle()));
    }

private:
    printing_scope_events_handle *m_handle = nullptr;
};

class PrintingExtrusion
{
public:
    explicit PrintingExtrusion(printing_extrusion_handle *handle) : m_handle(handle) {}
    explicit PrintingExtrusion(const printing_extrusion_handle *handle) :
        m_handle(const_cast<printing_extrusion_handle *>(handle)) {}

    bool valid() const { return m_handle != nullptr; }
    printing_extrusion_handle *mutable_handle() const { return m_handle; }
    const printing_extrusion_handle *handle() const { return m_handle; }

    LayerRegionIsland region_island() const {
        return LayerRegionIsland(printing_extrusion_get_region_island(handle()));
    }

    raw_extrusion_role role() const { return printing_extrusion_get_role(handle()); }
    void set_role(raw_extrusion_role role) const { printing_extrusion_set_role(mutable_handle(), role); }

    uint16_t object_instance_idx() const { return printing_extrusion_get_object_instance_idx(handle()); }
    void set_object_instance_idx(uint16_t idx) const {
        printing_extrusion_set_object_instance_idx(mutable_handle(), idx);
    }

    ExtrusionEntity root() const {
        return ExtrusionEntity(printing_extrusion_get_root(handle()));
    }

    MutableExtrusionEntity mutable_root() const {
        return MutableExtrusionEntity(printing_extrusion_get_root_mutable(mutable_handle()));
    }

    bool set_root_clone(const ExtrusionEntity &root) const {
        return printing_extrusion_set_root_clone(mutable_handle(), root.handle()) != 0;
    }

    bool set_root_move(MutableExtrusionEntity root) const {
        return printing_extrusion_set_root_move(mutable_handle(), root.mutable_handle()) != 0;
    }

private:
    printing_extrusion_handle *m_handle = nullptr;
};

class PrintingToolGroup
{
public:
    explicit PrintingToolGroup(printing_tool_group_handle *handle) : m_handle(handle) {}
    explicit PrintingToolGroup(const printing_tool_group_handle *handle) :
        m_handle(const_cast<printing_tool_group_handle *>(handle)) {}

    bool valid() const { return m_handle != nullptr; }
    printing_tool_group_handle *mutable_handle() const { return m_handle; }
    const printing_tool_group_handle *handle() const { return m_handle; }

    PrintingScopeEvents events() const {
        return PrintingScopeEvents(printing_tool_group_get_events_mutable(mutable_handle()));
    }

    uint16_t extruder_id() const { return printing_tool_group_get_extruder_id(handle()); }
    void set_extruder_id(uint16_t extruder_id) const {
        printing_tool_group_set_extruder_id(mutable_handle(), extruder_id);
    }

    uint32_t region_island_count() const { return printing_tool_group_count_region_island(handle()); }
    LayerRegionIsland region_island(uint32_t idx) const {
        return LayerRegionIsland(printing_tool_group_get_region_island(handle(), idx));
    }
    void append_region_island(const LayerRegionIsland &region_island) const {
        printing_tool_group_append_region_island(mutable_handle(), region_island.handle());
    }

    uint32_t extrusion_count() const { return printing_tool_group_count_extrusion(handle()); }
    PrintingExtrusion extrusion(uint32_t idx) const {
        return PrintingExtrusion(printing_tool_group_get_extrusion_mutable(mutable_handle(), idx));
    }

    PrintingExtrusion append_extrusion_clone(const LayerRegionIsland &source,
                                             raw_extrusion_role role,
                                             const ExtrusionEntity &root,
                                             uint16_t object_instance_idx) const {
        return PrintingExtrusion(printing_tool_group_append_extrusion_clone(
            mutable_handle(), source.handle(), role, root.handle(), object_instance_idx));
    }

    PrintingExtrusion append_extrusion_clone_from_region_island(const LayerRegionIsland &source,
                                                                raw_extrusion_role role,
                                                                uint16_t object_instance_idx) const {
        return PrintingExtrusion(printing_tool_group_append_extrusion_clone_from_region_island(
            mutable_handle(), source.handle(), role, object_instance_idx));
    }

    PrintingExtrusion append_extrusion_move(const LayerRegionIsland &source,
                                            raw_extrusion_role role,
                                            MutableExtrusionEntity root,
                                            uint16_t object_instance_idx) const {
        return PrintingExtrusion(printing_tool_group_append_extrusion_move(
            mutable_handle(), source.handle(), role, root.mutable_handle(), object_instance_idx));
    }

    bool move_extrusion(uint32_t from_idx, uint32_t to_idx) const {
        return printing_tool_group_move_extrusion(mutable_handle(), from_idx, to_idx) != 0;
    }

private:
    printing_tool_group_handle *m_handle = nullptr;
};

class PrintingLayerGroup
{
public:
    explicit PrintingLayerGroup(printing_layer_group_handle *handle) : m_handle(handle) {}
    explicit PrintingLayerGroup(const printing_layer_group_handle *handle) :
        m_handle(const_cast<printing_layer_group_handle *>(handle)) {}

    bool valid() const { return m_handle != nullptr; }
    printing_layer_group_handle *mutable_handle() const { return m_handle; }
    const printing_layer_group_handle *handle() const { return m_handle; }

    PrintingScopeEvents events() const {
        return PrintingScopeEvents(printing_layer_group_get_events_mutable(mutable_handle()));
    }

    coord_t print_z() const { return printing_layer_group_get_print_z(handle()); }
    void set_print_z(coord_t print_z) const { printing_layer_group_set_print_z(mutable_handle(), print_z); }

    uint32_t layer_count() const { return printing_layer_group_count_layer(handle()); }
    Layer layer(uint32_t idx) const { return Layer(printing_layer_group_get_layer(handle(), idx)); }
    void append_layer(const Layer &layer) const {
        printing_layer_group_append_layer(mutable_handle(), layer.handle());
    }

    uint32_t tool_group_count() const { return printing_layer_group_count_tool_group(handle()); }
    PrintingToolGroup tool_group(uint32_t idx) const {
        return PrintingToolGroup(printing_layer_group_get_tool_group_mutable(mutable_handle(), idx));
    }
    PrintingToolGroup append_tool_group(uint16_t extruder_id) const {
        return PrintingToolGroup(printing_layer_group_append_tool_group(mutable_handle(), extruder_id));
    }
    bool move_tool_group(uint32_t from_idx, uint32_t to_idx) const {
        return printing_layer_group_move_tool_group(mutable_handle(), from_idx, to_idx) != 0;
    }

private:
    printing_layer_group_handle *m_handle = nullptr;
};

class PrintingGroup
{
public:
    explicit PrintingGroup(printing_group_handle *handle) : m_handle(handle) {}
    explicit PrintingGroup(const printing_group_handle *handle) :
        m_handle(const_cast<printing_group_handle *>(handle)) {}

    bool valid() const { return m_handle != nullptr; }
    printing_group_handle *mutable_handle() const { return m_handle; }
    const printing_group_handle *handle() const { return m_handle; }

    PrintingScopeEvents events() const {
        return PrintingScopeEvents(printing_group_get_events_mutable(mutable_handle()));
    }

    void clear() const { printing_group_clear(mutable_handle()); }

    uint32_t object_instance_count() const { return printing_group_count_object_instance(handle()); }
    PrintingObjectInstance object_instance(uint32_t idx) const {
        return PrintingObjectInstance(printing_group_get_object_instance(handle(), idx));
    }
    void append_object_instance(const Object &object, uint64_t instance_idx) const {
        printing_group_append_object_instance(mutable_handle(), object.handle(), instance_idx);
    }

    uint32_t layer_group_count() const { return printing_group_count_layer_group(handle()); }
    PrintingLayerGroup layer_group(uint32_t idx) const {
        return PrintingLayerGroup(printing_group_get_layer_group_mutable(mutable_handle(), idx));
    }
    PrintingLayerGroup append_layer_group(coord_t print_z) const {
        return PrintingLayerGroup(printing_group_append_layer_group(mutable_handle(), print_z));
    }
    bool move_layer_group(uint32_t from_idx, uint32_t to_idx) const {
        return printing_group_move_layer_group(mutable_handle(), from_idx, to_idx) != 0;
    }

private:
    printing_group_handle *m_handle = nullptr;
};

class PrintingPlan
{
public:
    PrintingPlan() = default;
    explicit PrintingPlan(printing_plan_handle *handle) : m_handle(handle) {}
    explicit PrintingPlan(const printing_plan_handle *handle) :
        m_handle(const_cast<printing_plan_handle *>(handle)) {}

    bool valid() const { return m_handle != nullptr; }
    printing_plan_handle *mutable_handle() const { return m_handle; }
    const printing_plan_handle *handle() const { return m_handle; }

    PrintingScopeEvents events() const {
        return PrintingScopeEvents(printing_plan_get_events_mutable(mutable_handle()));
    }

    void clear() const { printing_plan_clear(mutable_handle()); }

    uint32_t group_count() const { return printing_plan_count_group(handle()); }
    PrintingGroup group(uint32_t idx) const {
        return PrintingGroup(printing_plan_get_group_mutable(mutable_handle(), idx));
    }
    PrintingGroup append_group() const {
        return PrintingGroup(printing_plan_append_group(mutable_handle()));
    }
    bool move_group(uint32_t from_idx, uint32_t to_idx) const {
        return printing_plan_move_group(mutable_handle(), from_idx, to_idx) != 0;
    }

private:
    printing_plan_handle *m_handle = nullptr;
};

} // namespace slic3r_api

#endif // slic3r_Api_plugin_cpp_PrintingPlanViews_hpp_
