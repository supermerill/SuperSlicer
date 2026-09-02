///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include <memory>
#include <utility>

#include "libslic3r/Api/plugin/c/slic3r_extrusion_entity.h"
#include "libslic3r/ExtrusionEntity.hpp"

#include "Orchestrator.hpp"

namespace Slic3r {

static ExtrusionEntity *to_extrusion(extrusion_entity_handle *me)
{
    return reinterpret_cast<ExtrusionEntity *>(me);
}

static const ExtrusionEntity *to_extrusion(const extrusion_entity_handle *me)
{
    return reinterpret_cast<const ExtrusionEntity *>(me);
}

static extrusion_entity_handle *to_handle(ExtrusionEntity *me)
{
    return reinterpret_cast<extrusion_entity_handle *>(me);
}

static PluginStorage *to_storage(storage_handle *storage)
{
    return reinterpret_cast<PluginStorage *>(storage);
}

static bool may_insert_children(const ExtrusionEntity &entity)
{
    return !entity.has_polyline();
}

} // namespace Slic3r

extern "C" {

extrusion_entity_handle *extrusion_create_empty(storage_handle *storage)
{
    if (storage == nullptr)
        return nullptr;

    Slic3r::PluginStorage *plugin_storage = Slic3r::to_storage(storage);
    Slic3r::ExtrusionEntity &entity = plugin_storage->extrusions.emplace_back(true);
    plugin_storage->generic_storage.insert(&entity);
    return Slic3r::to_handle(&entity);
}

extrusion_entity_handle *extrusion_clone(storage_handle *storage, const extrusion_entity_handle *src)
{
    if (storage == nullptr || src == nullptr)
        return nullptr;

    Slic3r::PluginStorage *plugin_storage = Slic3r::to_storage(storage);
    std::unique_ptr<Slic3r::ExtrusionEntity> clone(Slic3r::to_extrusion(src)->clone());
    Slic3r::ExtrusionEntity &entity = plugin_storage->extrusions.push_back(std::move(clone));
    plugin_storage->generic_storage.insert(&entity);
    return Slic3r::to_handle(&entity);
}

int32_t extrusion_copy_from(extrusion_entity_handle *dst, const extrusion_entity_handle *src)
{
    if (dst == nullptr || src == nullptr)
        return 0;
    *Slic3r::to_extrusion(dst) = *Slic3r::to_extrusion(src);
    return 1;
}

int32_t extrusion_move_from(extrusion_entity_handle *dst, extrusion_entity_handle *src)
{
    if (dst == nullptr || src == nullptr)
        return 0;
    if (dst == src)
        return 1;

    Slic3r::ExtrusionEntity *dst_entity = Slic3r::to_extrusion(dst);
    Slic3r::ExtrusionEntity *src_entity = Slic3r::to_extrusion(src);
    *dst_entity = std::move(*src_entity);
    src_entity->clear_content();
    src_entity->clear_properties();
    return 1;
}

int32_t extrusion_clear_content(extrusion_entity_handle *entity)
{
    if (entity == nullptr)
        return 0;
    Slic3r::to_extrusion(entity)->clear_content();
    return 1;
}

uint32_t extrusion_flags(const extrusion_entity_handle *entity)
{
    if (entity == nullptr)
        return 0;

    const Slic3r::ExtrusionEntity &extrusion = *Slic3r::to_extrusion(entity);
    uint32_t flags = 0;
    if (extrusion.can_reverse())
        flags |= RAW_EXTRUSION_FLAG_REVERSIBLE;
    if (extrusion.can_sort())
        flags |= RAW_EXTRUSION_FLAG_SORTABLE;
    return flags;
}

int32_t extrusion_set_flags(extrusion_entity_handle *entity, uint32_t flags)
{
    if (entity == nullptr)
        return 0;

    const bool reversible = (flags & RAW_EXTRUSION_FLAG_REVERSIBLE) != 0;
    const bool sortable = (flags & RAW_EXTRUSION_FLAG_SORTABLE) != 0;

    Slic3r::ExtrusionEntity *extrusion = Slic3r::to_extrusion(entity);
    if (sortable && extrusion->has_polyline())
        return 0;

    extrusion->set_can_sort_reverse(sortable, reversible);
    return 1;
}

int32_t extrusion_is_continuous(const extrusion_entity_handle *entity)
{
    return entity != nullptr && Slic3r::to_extrusion(entity)->is_continuous();
}

int32_t extrusion_is_loop(const extrusion_entity_handle *entity)
{
    return entity != nullptr && Slic3r::to_extrusion(entity)->is_loop();
}

int32_t extrusion_has_polyline(const extrusion_entity_handle *entity)
{
    return entity != nullptr && Slic3r::to_extrusion(entity)->has_polyline();
}

int32_t extrusion_has_children(const extrusion_entity_handle *entity)
{
    return entity != nullptr && !Slic3r::to_extrusion(entity)->is_leaf();
}

uint32_t extrusion_child_count(const extrusion_entity_handle *entity)
{
    if (entity == nullptr)
        return 0;
    return static_cast<uint32_t>(Slic3r::to_extrusion(entity)->child_count());
}

extrusion_entity_handle *extrusion_child_mutable(extrusion_entity_handle *entity, uint32_t idx)
{
    if (entity == nullptr)
        return nullptr;

    Slic3r::ExtrusionEntity *extrusion = Slic3r::to_extrusion(entity);
    if (idx >= extrusion->child_count())
        return nullptr;
    return Slic3r::to_handle(&extrusion->child(idx));
}

const extrusion_entity_handle *extrusion_child(const extrusion_entity_handle *entity, uint32_t idx)
{
    if (entity == nullptr)
        return nullptr;

    const Slic3r::ExtrusionEntity *extrusion = Slic3r::to_extrusion(entity);
    if (idx >= extrusion->child_count())
        return nullptr;
    return reinterpret_cast<const extrusion_entity_handle *>(&extrusion->child(idx));
}

uint32_t extrusion_insert_child_copy(extrusion_entity_handle *parent, uint32_t idx, const extrusion_entity_handle *child)
{
    if (parent == nullptr || child == nullptr)
        return EXTRUSION_INDEX_INVALID;

    Slic3r::ExtrusionEntity *parent_entity = Slic3r::to_extrusion(parent);
    if (!Slic3r::may_insert_children(*parent_entity) || idx > parent_entity->child_count())
        return EXTRUSION_INDEX_INVALID;

    parent_entity->insert_child(idx, std::unique_ptr<Slic3r::ExtrusionEntity>(Slic3r::to_extrusion(child)->clone()));
    return idx;
}

uint32_t extrusion_insert_child_move(extrusion_entity_handle *parent, uint32_t idx, extrusion_entity_handle *child)
{
    if (parent == nullptr || child == nullptr)
        return EXTRUSION_INDEX_INVALID;

    Slic3r::ExtrusionEntity *parent_entity = Slic3r::to_extrusion(parent);
    if (!Slic3r::may_insert_children(*parent_entity) || idx > parent_entity->child_count())
        return EXTRUSION_INDEX_INVALID;

    Slic3r::ExtrusionEntity *child_entity = Slic3r::to_extrusion(child);
    parent_entity->insert_child(idx, std::unique_ptr<Slic3r::ExtrusionEntity>(child_entity->clone_move()));
    child_entity->clear_content();
    child_entity->clear_properties();
    return idx;
}

extrusion_entity_handle *extrusion_emplace_ordered_leaf(
    extrusion_entity_handle *entity,
    raw_extrusion_ordered_leaf_position position,
    raw_extrusion_existing_property_placement property_placement)
{
    if (entity == nullptr ||
        (position != RAW_EXTRUSION_ORDERED_LEAF_BEFORE &&
         position != RAW_EXTRUSION_ORDERED_LEAF_AFTER) ||
        (property_placement != RAW_EXTRUSION_EXISTING_PROPERTIES_KEEP_ON_PARENT &&
         property_placement != RAW_EXTRUSION_EXISTING_PROPERTIES_MOVE_WITH_CONTENT))
        return nullptr;

    try {
        const Slic3r::OrderedLeafPosition core_position =
            position == RAW_EXTRUSION_ORDERED_LEAF_BEFORE ?
                Slic3r::OrderedLeafPosition::Before : Slic3r::OrderedLeafPosition::After;
        const Slic3r::ExistingPropertyPlacement core_property_placement =
            property_placement == RAW_EXTRUSION_EXISTING_PROPERTIES_KEEP_ON_PARENT ?
                Slic3r::ExistingPropertyPlacement::KeepOnParent :
                Slic3r::ExistingPropertyPlacement::MoveWithExistingContent;
        Slic3r::ExtrusionEntity *leaf =
            Slic3r::to_extrusion(entity)->emplace_ordered_leaf(core_position, core_property_placement);
        return Slic3r::to_handle(leaf);
    } catch (...) {
        return nullptr;
    }
}

int32_t extrusion_remove_child(extrusion_entity_handle *parent, uint32_t idx)
{
    if (parent == nullptr)
        return 0;

    Slic3r::ExtrusionEntity *parent_entity = Slic3r::to_extrusion(parent);
    if (parent_entity->is_leaf() || idx >= parent_entity->child_count())
        return 0;

    parent_entity->remove_child(idx);
    return 1;
}

uint32_t extrusion_move_child(extrusion_entity_handle *dst_parent,
                              uint32_t dst_idx,
                              extrusion_entity_handle *src_parent,
                              uint32_t src_idx)
{
    if (dst_parent == nullptr || src_parent == nullptr)
        return EXTRUSION_INDEX_INVALID;

    Slic3r::ExtrusionEntity *dst = Slic3r::to_extrusion(dst_parent);
    Slic3r::ExtrusionEntity *src = Slic3r::to_extrusion(src_parent);
    if (!Slic3r::may_insert_children(*dst) || src->is_leaf() || src_idx >= src->child_count())
        return EXTRUSION_INDEX_INVALID;
    if (dst_idx > dst->child_count())
        return EXTRUSION_INDEX_INVALID;
    if (dst == src && (dst_idx == src_idx || dst_idx == src_idx + 1))
        return src_idx;

    Slic3r::ExtrusionEntity::Children &src_children = src->children();
    Slic3r::ExtrusionEntityUPtr moved_child = std::move(src_children[src_idx]);
    src_children.erase(src_children.begin() + src_idx);

    if (dst == src && dst_idx > src_idx)
        --dst_idx;

    Slic3r::ExtrusionEntity::Children &dst_children = dst->children();
    if (dst_idx > dst_children.size())
        return EXTRUSION_INDEX_INVALID;
    dst_children.insert(dst_children.begin() + dst_idx, std::move(moved_child));
    return dst_idx;
}

} // extern "C"
