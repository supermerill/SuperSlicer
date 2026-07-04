///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "PluginProperty.hpp"

#include "Layer.hpp"

#include <algorithm>
#include <cassert>
#include <limits>

namespace Slic3r {

const LayerAdhesionProperty *LayerAdhesionProperty::get(const Layer &layer)
{
    return layer.get_property<LayerAdhesionProperty>();
}

bool LayerAdhesionProperty::layer_has_kind(const Layer &layer, raw_layer_adhesion_kind kind)
{
    const LayerAdhesionProperty *adhesion = get(layer);
    if (adhesion != nullptr)
        return adhesion->has_kind(kind);

    /*
    Compatibility path for layers produced while the old brim-only marker still
    existed. New code writes LayerAdhesionProperty, but readers may still see
    LayerBrimProperty until all persisted/intermediate producers are migrated.
    */
    return kind == RAW_LAYER_ADHESION_KIND_BRIM &&
           layer.get_property<LayerBrimProperty>() != nullptr;
}

bool LayerAdhesionProperty::layer_is_brim(const Layer &layer)
{
    return layer_has_kind(layer, RAW_LAYER_ADHESION_KIND_BRIM);
}

bool LayerAdhesionProperty::layer_is_normal_skirt(const Layer &layer)
{
    const LayerAdhesionProperty *adhesion = get(layer);
    return adhesion != nullptr && adhesion->is_skirt() && !adhesion->is_first_layer_only();
}

bool LayerAdhesionProperty::layer_is_skirt_first_layer_only(const Layer &layer)
{
    const LayerAdhesionProperty *adhesion = get(layer);
    return adhesion != nullptr && adhesion->is_skirt() && adhesion->is_first_layer_only();
}

PluginPropertyContainer::PluginPropertyContainer(const PluginPropertyContainer &rhs)
{
    copy_properties_from(rhs);
}

PluginPropertyContainer &PluginPropertyContainer::operator=(const PluginPropertyContainer &rhs)
{
    if (this != &rhs)
        copy_properties_from(rhs);
    return *this;
}

bool PluginPropertyContainer::has_properties() const
{
    return m_properties != nullptr && !m_properties->empty();
}

size_t PluginPropertyContainer::property_count() const
{
    return m_properties == nullptr ? 0 : m_properties->size();
}

slic3r_property_type PluginPropertyContainer::property_type_at(size_t idx) const
{
    if (m_properties == nullptr || idx >= m_properties->size())
        return SLIC3R_PROPERTY_TYPE_INVALID;
    return (*m_properties)[idx].type();
}

bool PluginPropertyContainer::has_property(slic3r_property_type type) const
{
    return find_slot(type) != nullptr;
}

uint32_t PluginPropertyContainer::property_data_size(slic3r_property_type type) const
{
    const PropertyStorageSlot *slot = find_slot(type);
    if (slot == nullptr)
        return 0;
    assert(slot->byte_count() <= std::numeric_limits<uint32_t>::max());
    return static_cast<uint32_t>(slot->byte_count());
}

const void *PluginPropertyContainer::property_data(slic3r_property_type type) const
{
    const PropertyStorageSlot *slot = find_slot(type);
    return slot == nullptr ? nullptr : slot->data();
}

void *PluginPropertyContainer::property_data_mutable(slic3r_property_type type)
{
    PropertyStorageSlot *slot = find_slot(type);
    return slot == nullptr ? nullptr : slot->data_mutable();
}

void *PluginPropertyContainer::get_or_add_property_data_mutable(slic3r_property_type type,
                                                                size_t byte_count,
                                                                size_t alignment)
{
    if (type == SLIC3R_PROPERTY_TYPE_INVALID || byte_count == 0 || alignment == 0)
        return nullptr;

    PropertyStorageSlot *slot = find_slot(type);
    if (slot != nullptr) {
        /*
        A duplicate numeric id must mean the same payload layout. Returning
        nullptr on mismatch makes the bug visible instead of letting the caller
        cast bytes to the wrong struct.
        */
        if (slot->byte_count() != byte_count ||
            slot->alignment() != PropertyRawBuffer::normalized_alignment(alignment))
            return nullptr;
        return slot->data_mutable();
    }

    std::vector<PropertyStorageSlot> &properties = mutable_properties();
    properties.emplace_back(type, byte_count, alignment);
    return properties.back().data_mutable();
}

bool PluginPropertyContainer::remove_property(slic3r_property_type type)
{
    if (m_properties == nullptr || type == SLIC3R_PROPERTY_TYPE_INVALID)
        return false;

    std::vector<PropertyStorageSlot>::iterator it =
        std::find_if(m_properties->begin(), m_properties->end(),
                     [type](const PropertyStorageSlot &slot) { return slot.type() == type; });
    if (it == m_properties->end())
        return false;

    m_properties->erase(it);
    if (m_properties->empty())
        m_properties.reset();
    return true;
}

void PluginPropertyContainer::clear_properties()
{
    m_properties.reset();
}

void PluginPropertyContainer::copy_properties_from(const PluginPropertyContainer &rhs)
{
    if (!rhs.has_properties()) {
        m_properties.reset();
        return;
    }
    m_properties = std::make_unique<std::vector<PropertyStorageSlot>>(*rhs.m_properties);
}

bool PluginPropertyContainer::properties_equal(const PluginPropertyContainer &rhs) const
{
    if (property_count() != rhs.property_count())
        return false;
    if (!has_properties())
        return true;

    for (const PropertyStorageSlot &slot : *m_properties) {
        const PropertyStorageSlot *rhs_slot = rhs.find_slot(slot.type());
        if (rhs_slot == nullptr || !slot.same_payload(*rhs_slot))
            return false;
    }
    return true;
}

PropertyStorageSlot *PluginPropertyContainer::find_slot(slic3r_property_type type)
{
    if (m_properties == nullptr || type == SLIC3R_PROPERTY_TYPE_INVALID)
        return nullptr;

    std::vector<PropertyStorageSlot>::iterator it =
        std::find_if(m_properties->begin(), m_properties->end(),
                     [type](const PropertyStorageSlot &slot) { return slot.type() == type; });
    return it == m_properties->end() ? nullptr : &*it;
}

const PropertyStorageSlot *PluginPropertyContainer::find_slot(slic3r_property_type type) const
{
    if (m_properties == nullptr || type == SLIC3R_PROPERTY_TYPE_INVALID)
        return nullptr;

    std::vector<PropertyStorageSlot>::const_iterator it =
        std::find_if(m_properties->begin(), m_properties->end(),
                     [type](const PropertyStorageSlot &slot) { return slot.type() == type; });
    return it == m_properties->end() ? nullptr : &*it;
}

std::vector<PropertyStorageSlot> &PluginPropertyContainer::mutable_properties()
{
    if (m_properties == nullptr)
        m_properties = std::make_unique<std::vector<PropertyStorageSlot>>();
    return *m_properties;
}

} // namespace Slic3r
