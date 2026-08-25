///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "libslic3r/Api/plugin/c/slic3r_printing_plan.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionRole.hpp"
#include "libslic3r/LayerRegion.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Printing/PrintingPlan.hpp"

namespace Slic3r {
namespace {

/*
These casts are the C ABI boundary for PrintingPlan. The C side only sees
opaque handles; the host side always immediately converts them back to the C++
objects that own the vectors and extrusion clones.
*/
Printing::PrintingPlan *to_plan(printing_plan_handle *me)
{
    return reinterpret_cast<Printing::PrintingPlan *>(me);
}

const Printing::PrintingPlan *to_plan(const printing_plan_handle *me)
{
    return reinterpret_cast<const Printing::PrintingPlan *>(me);
}

Printing::PrintingScopeEvents *to_scope_events(printing_scope_events_handle *me)
{
    return reinterpret_cast<Printing::PrintingScopeEvents *>(me);
}

const Printing::PrintingScopeEvents *to_scope_events(const printing_scope_events_handle *me)
{
    return reinterpret_cast<const Printing::PrintingScopeEvents *>(me);
}

Printing::PrintingGroup *to_group(printing_group_handle *me)
{
    return reinterpret_cast<Printing::PrintingGroup *>(me);
}

const Printing::PrintingGroup *to_group(const printing_group_handle *me)
{
    return reinterpret_cast<const Printing::PrintingGroup *>(me);
}

Printing::PrintingLayerGroup *to_layer_group(printing_layer_group_handle *me)
{
    return reinterpret_cast<Printing::PrintingLayerGroup *>(me);
}

const Printing::PrintingLayerGroup *to_layer_group(const printing_layer_group_handle *me)
{
    return reinterpret_cast<const Printing::PrintingLayerGroup *>(me);
}

Printing::PrintingToolGroup *to_tool_group(printing_tool_group_handle *me)
{
    return reinterpret_cast<Printing::PrintingToolGroup *>(me);
}

const Printing::PrintingToolGroup *to_tool_group(const printing_tool_group_handle *me)
{
    return reinterpret_cast<const Printing::PrintingToolGroup *>(me);
}

Printing::PrintingExtrusion *to_printing_extrusion(printing_extrusion_handle *me)
{
    return reinterpret_cast<Printing::PrintingExtrusion *>(me);
}

const Printing::PrintingExtrusion *to_printing_extrusion(const printing_extrusion_handle *me)
{
    return reinterpret_cast<const Printing::PrintingExtrusion *>(me);
}

const Print *to_print(const print_handle *me)
{
    return reinterpret_cast<const Print *>(me);
}

const PrintObject *to_object(const object_handle *me)
{
    return reinterpret_cast<const PrintObject *>(me);
}

const Layer *to_layer(const layer_handle *me)
{
    return reinterpret_cast<const Layer *>(me);
}

const LayerRegionIsland *to_region_island(const layer_region_island_handle *me)
{
    return reinterpret_cast<const LayerRegionIsland *>(me);
}

ExtrusionEntity *to_extrusion(extrusion_entity_handle *me)
{
    return reinterpret_cast<ExtrusionEntity *>(me);
}

const ExtrusionEntity *to_extrusion(const extrusion_entity_handle *me)
{
    return reinterpret_cast<const ExtrusionEntity *>(me);
}

ExtrusionRole to_extrusion_role(raw_extrusion_role role)
{
    return ExtrusionRole(static_cast<ExtrusionRoleModifier>(role));
}

raw_extrusion_role to_raw_role(ExtrusionRole role)
{
    return static_cast<raw_extrusion_role>(role());
}

template<class Vector>
bool move_vector_item(Vector &items, uint32_t from_idx, uint32_t to_idx)
{
    // Reordering APIs move owned vector elements rather than copying them.
    // This preserves cloned extrusion trees and any properties stored on those
    // trees while giving plugins simple index-based ordering primitives.
    if (from_idx >= items.size() || to_idx >= items.size())
        return false;
    if (from_idx == to_idx)
        return true;

    typename Vector::value_type moved = std::move(items[from_idx]);
    items.erase(items.begin() + from_idx);
    items.insert(items.begin() + to_idx, std::move(moved));
    return true;
}

std::unique_ptr<ExtrusionEntity> clone_or_empty(const extrusion_entity_handle *root)
{
    if (root == nullptr)
        return std::make_unique<ExtrusionEntity>(true);
    return ExtrusionEntityUPtr(to_extrusion(root)->clone());
}

std::unique_ptr<ExtrusionEntity> move_or_empty(extrusion_entity_handle *root)
{
    std::unique_ptr<ExtrusionEntity> out = std::make_unique<ExtrusionEntity>(true);
    if (root != nullptr)
        extrusion_move_from(reinterpret_cast<extrusion_entity_handle *>(out.get()), root);
    return out;
}

} // namespace
} // namespace Slic3r

extern "C" {

int32_t printing_scope_events_has_before(const printing_scope_events_handle *me)
{
    return me != nullptr && Slic3r::to_scope_events(me)->has_before();
}

int32_t printing_scope_events_has_after(const printing_scope_events_handle *me)
{
    return me != nullptr && Slic3r::to_scope_events(me)->has_after();
}

const extrusion_entity_handle *printing_scope_events_get_before(const printing_scope_events_handle *me)
{
    if (me == nullptr)
        return nullptr;
    return reinterpret_cast<const extrusion_entity_handle *>(&Slic3r::to_scope_events(me)->before());
}

const extrusion_entity_handle *printing_scope_events_get_after(const printing_scope_events_handle *me)
{
    if (me == nullptr)
        return nullptr;
    return reinterpret_cast<const extrusion_entity_handle *>(&Slic3r::to_scope_events(me)->after());
}

extrusion_entity_handle *printing_scope_events_append_before_clone(
    printing_scope_events_handle *me,
    const extrusion_entity_handle *event)
{
    if (me == nullptr || event == nullptr)
        return nullptr;
    Slic3r::ExtrusionEntity &appended = Slic3r::to_scope_events(me)->append_before(*Slic3r::to_extrusion(event));
    return reinterpret_cast<extrusion_entity_handle *>(&appended);
}

extrusion_entity_handle *printing_scope_events_append_before_move(
    printing_scope_events_handle *me,
    extrusion_entity_handle *event)
{
    if (me == nullptr || event == nullptr)
        return nullptr;
    Slic3r::ExtrusionEntity &appended =
        Slic3r::to_scope_events(me)->append_before(std::move(*Slic3r::to_extrusion(event)));
    return reinterpret_cast<extrusion_entity_handle *>(&appended);
}

extrusion_entity_handle *printing_scope_events_append_after_clone(
    printing_scope_events_handle *me,
    const extrusion_entity_handle *event)
{
    if (me == nullptr || event == nullptr)
        return nullptr;
    Slic3r::ExtrusionEntity &appended = Slic3r::to_scope_events(me)->append_after(*Slic3r::to_extrusion(event));
    return reinterpret_cast<extrusion_entity_handle *>(&appended);
}

extrusion_entity_handle *printing_scope_events_append_after_move(
    printing_scope_events_handle *me,
    extrusion_entity_handle *event)
{
    if (me == nullptr || event == nullptr)
        return nullptr;
    Slic3r::ExtrusionEntity &appended =
        Slic3r::to_scope_events(me)->append_after(std::move(*Slic3r::to_extrusion(event)));
    return reinterpret_cast<extrusion_entity_handle *>(&appended);
}

void printing_plan_clear(printing_plan_handle *me)
{
    if (me != nullptr) {
        Slic3r::to_plan(me)->events.clear();
        Slic3r::to_plan(me)->groups.clear();
    }
}

printing_scope_events_handle *printing_plan_get_events_mutable(printing_plan_handle *me)
{
    return me == nullptr ? nullptr : reinterpret_cast<printing_scope_events_handle *>(&Slic3r::to_plan(me)->events);
}

const printing_scope_events_handle *printing_plan_get_events(const printing_plan_handle *me)
{
    return me == nullptr ?
               nullptr : reinterpret_cast<const printing_scope_events_handle *>(&Slic3r::to_plan(me)->events);
}

uint32_t printing_plan_count_group(const printing_plan_handle *me)
{
    return me == nullptr ? 0u : uint32_t(Slic3r::to_plan(me)->groups.size());
}

printing_group_handle *printing_plan_get_group_mutable(printing_plan_handle *me, uint32_t idx)
{
    if (me == nullptr || idx >= Slic3r::to_plan(me)->groups.size())
        return nullptr;
    return reinterpret_cast<printing_group_handle *>(&Slic3r::to_plan(me)->groups[idx]);
}

const printing_group_handle *printing_plan_get_group(const printing_plan_handle *me, uint32_t idx)
{
    if (me == nullptr || idx >= Slic3r::to_plan(me)->groups.size())
        return nullptr;
    return reinterpret_cast<const printing_group_handle *>(&Slic3r::to_plan(me)->groups[idx]);
}

printing_group_handle *printing_plan_append_group(printing_plan_handle *me)
{
    if (me == nullptr)
        return nullptr;
    return reinterpret_cast<printing_group_handle *>(&Slic3r::to_plan(me)->groups.emplace_back());
}

int32_t printing_plan_move_group(printing_plan_handle *me, uint32_t from_idx, uint32_t to_idx)
{
    return me != nullptr && Slic3r::move_vector_item(Slic3r::to_plan(me)->groups, from_idx, to_idx);
}

void printing_group_clear(printing_group_handle *me)
{
    if (me != nullptr)
        *Slic3r::to_group(me) = Slic3r::Printing::PrintingGroup();
}

printing_scope_events_handle *printing_group_get_events_mutable(printing_group_handle *me)
{
    return me == nullptr ? nullptr : reinterpret_cast<printing_scope_events_handle *>(&Slic3r::to_group(me)->events);
}

const printing_scope_events_handle *printing_group_get_events(const printing_group_handle *me)
{
    return me == nullptr ?
               nullptr : reinterpret_cast<const printing_scope_events_handle *>(&Slic3r::to_group(me)->events);
}

uint32_t printing_group_count_object_instance(const printing_group_handle *me)
{
    return me == nullptr ? 0u : uint32_t(Slic3r::to_group(me)->object_instances.size());
}

c_printing_object_instance printing_group_get_object_instance(const printing_group_handle *me, uint32_t idx)
{
    c_printing_object_instance out = {};
    if (me == nullptr || idx >= Slic3r::to_group(me)->object_instances.size())
        return out;

    const Slic3r::Printing::PrintingObjectInstance &instance = Slic3r::to_group(me)->object_instances[idx];
    out.object = reinterpret_cast<const object_handle *>(instance.object);
    out.instance_idx = static_cast<uint64_t>(instance.instance_idx);
    return out;
}

void printing_group_append_object_instance(printing_group_handle *me,
                                           const object_handle *object,
                                           uint64_t instance_idx)
{
    if (me == nullptr)
        return;

    Slic3r::Printing::PrintingObjectInstance instance;
    instance.object = Slic3r::to_object(object);
    instance.instance_idx = static_cast<size_t>(instance_idx);
    Slic3r::to_group(me)->object_instances.push_back(instance);
}

uint32_t printing_group_count_layer_group(const printing_group_handle *me)
{
    return me == nullptr ? 0u : uint32_t(Slic3r::to_group(me)->layers.size());
}

printing_layer_group_handle *printing_group_get_layer_group_mutable(printing_group_handle *me, uint32_t idx)
{
    if (me == nullptr || idx >= Slic3r::to_group(me)->layers.size())
        return nullptr;
    return reinterpret_cast<printing_layer_group_handle *>(&Slic3r::to_group(me)->layers[idx]);
}

const printing_layer_group_handle *printing_group_get_layer_group(const printing_group_handle *me, uint32_t idx)
{
    if (me == nullptr || idx >= Slic3r::to_group(me)->layers.size())
        return nullptr;
    return reinterpret_cast<const printing_layer_group_handle *>(&Slic3r::to_group(me)->layers[idx]);
}

printing_layer_group_handle *printing_group_append_layer_group(printing_group_handle *me, coord_t print_z)
{
    if (me == nullptr)
        return nullptr;
    Slic3r::Printing::PrintingLayerGroup &layer_group = Slic3r::to_group(me)->layers.emplace_back();
    layer_group.print_z = print_z;
    return reinterpret_cast<printing_layer_group_handle *>(&layer_group);
}

int32_t printing_group_move_layer_group(printing_group_handle *me, uint32_t from_idx, uint32_t to_idx)
{
    return me != nullptr && Slic3r::move_vector_item(Slic3r::to_group(me)->layers, from_idx, to_idx);
}

printing_scope_events_handle *printing_layer_group_get_events_mutable(printing_layer_group_handle *me)
{
    return me == nullptr ?
               nullptr : reinterpret_cast<printing_scope_events_handle *>(&Slic3r::to_layer_group(me)->events);
}

const printing_scope_events_handle *printing_layer_group_get_events(const printing_layer_group_handle *me)
{
    return me == nullptr ?
               nullptr : reinterpret_cast<const printing_scope_events_handle *>(&Slic3r::to_layer_group(me)->events);
}

coord_t printing_layer_group_get_print_z(const printing_layer_group_handle *me)
{
    return me == nullptr ? 0 : Slic3r::to_layer_group(me)->print_z;
}

void printing_layer_group_set_print_z(printing_layer_group_handle *me, coord_t print_z)
{
    if (me != nullptr)
        Slic3r::to_layer_group(me)->print_z = print_z;
}

uint32_t printing_layer_group_count_layer(const printing_layer_group_handle *me)
{
    return me == nullptr ? 0u : uint32_t(Slic3r::to_layer_group(me)->layers.size());
}

const layer_handle *printing_layer_group_get_layer(const printing_layer_group_handle *me, uint32_t idx)
{
    if (me == nullptr || idx >= Slic3r::to_layer_group(me)->layers.size())
        return nullptr;
    return reinterpret_cast<const layer_handle *>(Slic3r::to_layer_group(me)->layers[idx]);
}

void printing_layer_group_append_layer(printing_layer_group_handle *me, const layer_handle *layer)
{
    if (me != nullptr)
        Slic3r::to_layer_group(me)->layers.push_back(Slic3r::to_layer(layer));
}

uint32_t printing_layer_group_count_tool_group(const printing_layer_group_handle *me)
{
    return me == nullptr ? 0u : uint32_t(Slic3r::to_layer_group(me)->tool_groups.size());
}

printing_tool_group_handle *printing_layer_group_get_tool_group_mutable(printing_layer_group_handle *me,
                                                                        uint32_t idx)
{
    if (me == nullptr || idx >= Slic3r::to_layer_group(me)->tool_groups.size())
        return nullptr;
    return reinterpret_cast<printing_tool_group_handle *>(&Slic3r::to_layer_group(me)->tool_groups[idx]);
}

const printing_tool_group_handle *printing_layer_group_get_tool_group(const printing_layer_group_handle *me,
                                                                      uint32_t idx)
{
    if (me == nullptr || idx >= Slic3r::to_layer_group(me)->tool_groups.size())
        return nullptr;
    return reinterpret_cast<const printing_tool_group_handle *>(&Slic3r::to_layer_group(me)->tool_groups[idx]);
}

printing_tool_group_handle *printing_layer_group_append_tool_group(printing_layer_group_handle *me,
                                                                   uint16_t extruder_id)
{
    if (me == nullptr)
        return nullptr;
    Slic3r::Printing::PrintingToolGroup &tool_group = Slic3r::to_layer_group(me)->tool_groups.emplace_back();
    tool_group.extruder_id = extruder_id;
    return reinterpret_cast<printing_tool_group_handle *>(&tool_group);
}

int32_t printing_layer_group_move_tool_group(printing_layer_group_handle *me, uint32_t from_idx, uint32_t to_idx)
{
    return me != nullptr && Slic3r::move_vector_item(Slic3r::to_layer_group(me)->tool_groups, from_idx, to_idx);
}

printing_scope_events_handle *printing_tool_group_get_events_mutable(printing_tool_group_handle *me)
{
    return me == nullptr ?
               nullptr : reinterpret_cast<printing_scope_events_handle *>(&Slic3r::to_tool_group(me)->events);
}

const printing_scope_events_handle *printing_tool_group_get_events(const printing_tool_group_handle *me)
{
    return me == nullptr ?
               nullptr : reinterpret_cast<const printing_scope_events_handle *>(&Slic3r::to_tool_group(me)->events);
}

uint16_t printing_tool_group_get_extruder_id(const printing_tool_group_handle *me)
{
    return me == nullptr ? uint16_t(-1) : Slic3r::to_tool_group(me)->extruder_id;
}

void printing_tool_group_set_extruder_id(printing_tool_group_handle *me, uint16_t extruder_id)
{
    if (me != nullptr)
        Slic3r::to_tool_group(me)->extruder_id = extruder_id;
}

uint32_t printing_tool_group_count_region_island(const printing_tool_group_handle *me)
{
    return me == nullptr ? 0u : uint32_t(Slic3r::to_tool_group(me)->region_islands.size());
}

const layer_region_island_handle *printing_tool_group_get_region_island(const printing_tool_group_handle *me,
                                                                        uint32_t idx)
{
    if (me == nullptr || idx >= Slic3r::to_tool_group(me)->region_islands.size())
        return nullptr;
    return reinterpret_cast<const layer_region_island_handle *>(Slic3r::to_tool_group(me)->region_islands[idx]);
}

void printing_tool_group_append_region_island(printing_tool_group_handle *me,
                                              const layer_region_island_handle *region_island)
{
    if (me != nullptr)
        Slic3r::to_tool_group(me)->region_islands.push_back(Slic3r::to_region_island(region_island));
}

uint32_t printing_tool_group_count_extrusion(const printing_tool_group_handle *me)
{
    return me == nullptr ? 0u : uint32_t(Slic3r::to_tool_group(me)->extrusions.size());
}

printing_extrusion_handle *printing_tool_group_get_extrusion_mutable(printing_tool_group_handle *me, uint32_t idx)
{
    if (me == nullptr || idx >= Slic3r::to_tool_group(me)->extrusions.size())
        return nullptr;
    return reinterpret_cast<printing_extrusion_handle *>(&Slic3r::to_tool_group(me)->extrusions[idx]);
}

const printing_extrusion_handle *printing_tool_group_get_extrusion(const printing_tool_group_handle *me,
                                                                   uint32_t idx)
{
    if (me == nullptr || idx >= Slic3r::to_tool_group(me)->extrusions.size())
        return nullptr;
    return reinterpret_cast<const printing_extrusion_handle *>(&Slic3r::to_tool_group(me)->extrusions[idx]);
}

printing_extrusion_handle *printing_tool_group_append_extrusion_clone(
    printing_tool_group_handle *me,
    const layer_region_island_handle *source_region_island,
    raw_extrusion_role source_role,
    const extrusion_entity_handle *root,
    uint16_t object_instance_idx)
{
    if (me == nullptr)
        return nullptr;

    Slic3r::Printing::PrintingExtrusion extrusion;
    extrusion.region_island = Slic3r::to_region_island(source_region_island);
    extrusion.sregion_island_role = Slic3r::to_extrusion_role(source_role);
    extrusion.root = Slic3r::clone_or_empty(root);
    extrusion.object_instance_idx = object_instance_idx;
    Slic3r::to_tool_group(me)->extrusions.push_back(std::move(extrusion));
    return reinterpret_cast<printing_extrusion_handle *>(&Slic3r::to_tool_group(me)->extrusions.back());
}

printing_extrusion_handle *printing_tool_group_append_extrusion_clone_from_region_island(
    printing_tool_group_handle *me,
    const layer_region_island_handle *source_region_island,
    raw_extrusion_role source_role,
    uint16_t object_instance_idx)
{
    const Slic3r::LayerRegionIsland *region_island = Slic3r::to_region_island(source_region_island);
    const Slic3r::ExtrusionRole role = Slic3r::to_extrusion_role(source_role);
    if (me == nullptr || region_island == nullptr || !region_island->has_extrusion(role))
        return nullptr;

    return printing_tool_group_append_extrusion_clone(
        me,
        source_region_island,
        source_role,
        reinterpret_cast<const extrusion_entity_handle *>(&region_island->extrusion(role)),
        object_instance_idx);
}

printing_extrusion_handle *printing_tool_group_append_extrusion_move(
    printing_tool_group_handle *me,
    const layer_region_island_handle *source_region_island,
    raw_extrusion_role source_role,
    extrusion_entity_handle *root,
    uint16_t object_instance_idx)
{
    if (me == nullptr)
        return nullptr;

    Slic3r::Printing::PrintingExtrusion extrusion;
    extrusion.region_island = Slic3r::to_region_island(source_region_island);
    extrusion.sregion_island_role = Slic3r::to_extrusion_role(source_role);
    extrusion.root = Slic3r::move_or_empty(root);
    extrusion.object_instance_idx = object_instance_idx;
    Slic3r::to_tool_group(me)->extrusions.push_back(std::move(extrusion));
    return reinterpret_cast<printing_extrusion_handle *>(&Slic3r::to_tool_group(me)->extrusions.back());
}

int32_t printing_tool_group_move_extrusion(printing_tool_group_handle *me, uint32_t from_idx, uint32_t to_idx)
{
    return me != nullptr && Slic3r::move_vector_item(Slic3r::to_tool_group(me)->extrusions, from_idx, to_idx);
}

const layer_region_island_handle *printing_extrusion_get_region_island(const printing_extrusion_handle *me)
{
    return me == nullptr ?
               nullptr :
               reinterpret_cast<const layer_region_island_handle *>(Slic3r::to_printing_extrusion(me)->region_island);
}

raw_extrusion_role printing_extrusion_get_role(const printing_extrusion_handle *me)
{
    return me == nullptr ? RAW_EXTRUSION_ROLE_NONE : Slic3r::to_raw_role(Slic3r::to_printing_extrusion(me)->sregion_island_role);
}

void printing_extrusion_set_role(printing_extrusion_handle *me, raw_extrusion_role role)
{
    if (me != nullptr)
        Slic3r::to_printing_extrusion(me)->sregion_island_role = Slic3r::to_extrusion_role(role);
}

uint16_t printing_extrusion_get_object_instance_idx(const printing_extrusion_handle *me)
{
    return me == nullptr ? 0u : Slic3r::to_printing_extrusion(me)->object_instance_idx;
}

void printing_extrusion_set_object_instance_idx(printing_extrusion_handle *me, uint16_t object_instance_idx)
{
    if (me != nullptr)
        Slic3r::to_printing_extrusion(me)->object_instance_idx = object_instance_idx;
}

extrusion_entity_handle *printing_extrusion_get_root_mutable(printing_extrusion_handle *me)
{
    if (me == nullptr || !Slic3r::to_printing_extrusion(me)->root)
        return nullptr;
    return reinterpret_cast<extrusion_entity_handle *>(Slic3r::to_printing_extrusion(me)->root.get());
}

const extrusion_entity_handle *printing_extrusion_get_root(const printing_extrusion_handle *me)
{
    if (me == nullptr || !Slic3r::to_printing_extrusion(me)->root)
        return nullptr;
    return reinterpret_cast<const extrusion_entity_handle *>(Slic3r::to_printing_extrusion(me)->root.get());
}

int32_t printing_extrusion_set_root_clone(printing_extrusion_handle *me, const extrusion_entity_handle *root)
{
    if (me == nullptr)
        return 0;
    Slic3r::to_printing_extrusion(me)->root = Slic3r::clone_or_empty(root);
    return 1;
}

int32_t printing_extrusion_set_root_move(printing_extrusion_handle *me, extrusion_entity_handle *root)
{
    if (me == nullptr)
        return 0;
    Slic3r::to_printing_extrusion(me)->root = Slic3r::move_or_empty(root);
    return 1;
}

} // extern "C"
