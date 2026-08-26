///|/ Copyright (c) SuperSlicer 2026 Remi Durand @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "ExtrusionProperty.hpp"

#include <algorithm>
#include "Api/internal/ExtrusionPropertyAccess.hpp"
#include "Flow.hpp"

namespace Slic3r {

ExtrusionFlow::ExtrusionFlow(const Flow &flow)
    : c_extrusion_flow{ flow.mm3_per_mm(), flow.width(), flow.height() }
{
}

ExtrusionAttributes::ExtrusionAttributes()
    : c_extrusion_property_attributes{ -1., -1.f, -1.f, uint16_t(ExtrusionRole::None), 0 }
{
}

ExtrusionAttributes::ExtrusionAttributes(ExtrusionRole role)
    : ExtrusionAttributes()
{
    this->set_role(role);
}

ExtrusionAttributes::ExtrusionAttributes(ExtrusionRole role, const Flow &flow)
    : c_extrusion_property_attributes{
          flow.mm3_per_mm(), flow.width(), flow.height(), uint16_t(role()), 0 }
{
}

ExtrusionAttributes::ExtrusionAttributes(ExtrusionRole role, const ExtrusionFlow &flow)
    : c_extrusion_property_attributes{ flow.mm3_per_mm, flow.width, flow.height, uint16_t(role()), 0 }
{
}

ExtrusionPropertyModifier::ExtrusionPropertyModifier()
    : c_extrusion_property_modifier{ 0, 0, 0, 0, 0, 0 }
{
}

ExtrusionPropertyCustomGcode::ExtrusionPropertyCustomGcode()
    : c_extrusion_property_custom_gcode{C_EXTRUSION_CUSTOM_GCODE_GCODE, GCODE_SCRIPT_TYPE_INVALID,
                                        EXTRUSION_DATA_ID_INVALID, GCODE_SCRIPT_TARGET_EXTRUDER_INVALID} {}

ExtrusionPropertyCustomGcode::ExtrusionPropertyCustomGcode(Code c,
                                                           extrusion_data_id text_id,
                                                           gcode_script_type script_type)
    : c_extrusion_property_custom_gcode{c_extrusion_custom_gcode_kind(c),
                                        c == Code::SCRIPT && script_type == GCODE_SCRIPT_TYPE_INVALID ?
                                            GCODE_SCRIPT_TYPE_EXTRUSION_CUSTOM :
                                            script_type,
                                        text_id,
                                        GCODE_SCRIPT_TARGET_EXTRUDER_INVALID} {}

ExtrusionPropertyCustomGcodeText::ExtrusionPropertyCustomGcodeText(const std::string &str)
    : code(Code::GCODE)
    , gcode(str)
{
    if (!str.empty() && str[0] == ';') {
        code = Code::COMMENT;
        if (str.size() > 1 && str[1] == ' ')
            gcode = gcode.substr(2);
        else
            gcode = gcode.substr(1);
    }
}

ExtrusionPropertyCustomGcodeText::ExtrusionPropertyCustomGcodeText(Code c,
                                                                   const std::string &str,
                                                                   gcode_script_type script_type)
    : code(c)
    , script_type(c == Code::SCRIPT && script_type == GCODE_SCRIPT_TYPE_INVALID ? GCODE_SCRIPT_TYPE_EXTRUSION_CUSTOM :
                                                                                  script_type)
    , gcode(str) {}

ExtrusionPropertyOverhang::ExtrusionPropertyOverhang()
    : c_extrusion_property_overhang{ -1.f, -1.f, 0.f, 0, 0, 0, 0 }
{
}

ExtrusionPropertyLoopRole::ExtrusionPropertyLoopRole()
    : c_extrusion_property_perimeter{ -1, 0, uint16_t(elrDefault) }
{
}

ExtrusionPropertyLoopRole::ExtrusionPropertyLoopRole(ExtrusionLoopRole role)
    : c_extrusion_property_perimeter{ -1, 0, uint16_t(role) }
{
}

ExtrusionPropertyContainer::ExtrusionPropertyContainer(ExtrusionPropertyUPtr &&property)
{
    if (property)
        m_properties.emplace_back(std::move(*property));
}

ExtrusionPropertyContainer::ExtrusionPropertyContainer(ExtrusionPropertyUPtrs &&properties)
{
    m_properties.reserve(properties.size());
    for (ExtrusionPropertyUPtr &property : properties) {
        if (property)
            m_properties.emplace_back(std::move(*property));
    }
}

ExtrusionPropertyContainer::ExtrusionPropertyContainer(const ExtrusionPropertyContainer &rhs)
    : m_properties(rhs.m_properties)
    , m_data_resources(rhs.m_data_resources)
    , m_next_data_resource_id(rhs.m_next_data_resource_id)
{
}

ExtrusionPropertyContainer& ExtrusionPropertyContainer::operator=(const ExtrusionPropertyContainer &rhs)
{
    if (this != &rhs) {
        m_properties = rhs.m_properties;
        m_data_resources = rhs.m_data_resources;
        m_next_data_resource_id = rhs.m_next_data_resource_id;
    }
    return *this;
}

ExtrusionPropertyUPtrs ExtrusionPropertyContainer::clone_properties() const
{
    ExtrusionPropertyUPtrs out;
    out.reserve(m_properties.size());
    for (const PropertySlot &property : m_properties)
        out.emplace_back(std::make_unique<PropertySlot>(property));
    return out;
}

void ExtrusionPropertyContainer::clear_properties()
{
    for (const PropertySlot &property : m_properties)
        this->release_property_resources(property.type());
    m_properties.clear();
}

void ExtrusionPropertyContainer::add_property(ExtrusionPropertyUPtr &&property)
{
    assert(property != nullptr);
    PropertySlot slot = std::move(*property);
    PropertySlot *stored = this->find_slot(slot.type());
    if (stored != nullptr) {
        this->release_property_resources(slot.type());
        *stored = std::move(slot);
        return;
    }
    m_properties.emplace_back(std::move(slot));
}

ExtrusionPropertyCustomGcode&
ExtrusionPropertyContainer::add_property(const ExtrusionPropertyCustomGcodeText &property)
{
    ExtrusionPropertyCustomGcode &out = this->get_or_add_property<ExtrusionPropertyCustomGcode>();
    out.kind = c_extrusion_custom_gcode_kind(property.code);
    out.script_type = property.script_type;
    out.target_extruder_id = GCODE_SCRIPT_TARGET_EXTRUDER_INVALID;
    this->store_property_data_aligned(
        ExtrusionPropertyCustomGcode::property_type, &out.text_id,
        property.gcode.c_str(), property.gcode.size() + 1, alignof(char));
    return out;
}

std::string ExtrusionPropertyContainer::custom_gcode_string(const ExtrusionPropertyCustomGcode &property) const
{
    uint32_t byte_size = 0;
    const char *data = static_cast<const char*>(this->stored_data(property.text_id, &byte_size));
    if (data == nullptr || byte_size == 0)
        return {};
    if (data[byte_size - 1] == '\0')
        --byte_size;
    return std::string(data, data + byte_size);
}

PropertySlot* ExtrusionPropertyContainer::find_slot(extrusion_property_type type)
{
    for (PropertySlot &slot : m_properties)
        if (slot.type() == type)
            return &slot;
    return nullptr;
}

const PropertySlot* ExtrusionPropertyContainer::find_slot(extrusion_property_type type) const
{
    for (const PropertySlot &slot : m_properties)
        if (slot.type() == type)
            return &slot;
    return nullptr;
}

ExtrusionPropertyContainer::DataResource::DataResource(const DataResource &rhs)
    : id(rhs.id)
    , owner_type(rhs.owner_type)
    , owner_field_offset(rhs.owner_field_offset)
{
    data.assign_copy(rhs.data.data(), rhs.data.byte_count(), rhs.data.alignment());
}

ExtrusionPropertyContainer::DataResource&
ExtrusionPropertyContainer::DataResource::operator=(const DataResource &rhs)
{
    if (this != &rhs) {
        id = rhs.id;
        owner_type = rhs.owner_type;
        owner_field_offset = rhs.owner_field_offset;
        data.assign_copy(rhs.data.data(), rhs.data.byte_count(), rhs.data.alignment());
    }
    return *this;
}

extrusion_property_type ExtrusionPropertyContainer::property_type_at(size_t idx) const
{
    return idx < m_properties.size() ? m_properties[idx].type() : extrusion_property_type_invalid;
}

const void* ExtrusionPropertyContainer::property_data(extrusion_property_type type) const
{
    const PropertySlot *slot = this->find_slot(type);
    return slot != nullptr ? slot->data() : nullptr;
}

void* ExtrusionPropertyContainer::property_data_mutable(extrusion_property_type type)
{
    PropertySlot *slot = this->find_slot(type);
    return slot != nullptr ? slot->data_mutable() : nullptr;
}

void* ExtrusionPropertyContainer::get_or_add_property_data_mutable(extrusion_property_type type, size_t byte_count, size_t alignment)
{
    if (PropertySlot *slot = this->find_slot(type))
        return slot->data_mutable();

    switch (type) {
    case ExtrusionAttributes::property_type:
        return &this->get_or_add_property<ExtrusionAttributes>();
    case ExtrusionPropertySpeed::property_type:
        return &this->get_or_add_property<ExtrusionPropertySpeed>().speed_mm_per_s;
    case ExtrusionPropertyModifier::property_type:
        return &this->get_or_add_property<ExtrusionPropertyModifier>().enforce_travel;
    case ExtrusionPropertySpecialCommand::property_type:
        return &this->get_or_add_property<ExtrusionPropertySpecialCommand>(ExtrusionPropertySpecialCommand::Code::TOOLCHANGE).code;
    case ExtrusionPropertyOverhang::property_type:
        return &this->get_or_add_property<ExtrusionPropertyOverhang>().start_distance_from_prev_layer;
    case ExtrusionPropertyZOffset::property_type:
        return &this->get_or_add_property<ExtrusionPropertyZOffset>().z_offset;
    case ExtrusionPropertyLoopRole::property_type:
        return &this->get_or_add_property<ExtrusionPropertyLoopRole>().perimeter_idx;
    case ExtrusionPropertyInfill::property_type:
        return &this->get_or_add_property<ExtrusionPropertyInfill>().source_surface_id;
    case ExtrusionPropertyCustomGcode::property_type:
        return &this->get_or_add_property<ExtrusionPropertyCustomGcode>();
    default:
        if (type == extrusion_property_type_invalid || byte_count == 0 || alignment == 0)
            return nullptr;
        PropertySlot raw_slot;
        raw_slot.emplace_zeroed(type, byte_count, alignment);
        m_properties.emplace_back(std::move(raw_slot));
        return m_properties.back().data_mutable();
    }
}

bool ExtrusionPropertyContainer::remove_property(extrusion_property_type type)
{
    for (std::vector<PropertySlot>::iterator it = m_properties.begin(); it != m_properties.end(); ++it)
        if (it->type() == type) {
            this->release_property_resources(type);
            m_properties.erase(it);
            return true;
        }
    return false;
}

uint32_t ExtrusionPropertyContainer::store_data_aligned(const void *data, size_t byte_count, size_t alignment)
{
    if (alignment == 0 || (byte_count > 0 && data == nullptr))
        return uint32_t(-1);

    DataResource resource;
    resource.id = m_next_data_resource_id++;
    if (resource.id == uint32_t(-1))
        resource.id = m_next_data_resource_id++;
    resource.data.assign_copy(data, byte_count, alignment);
    m_data_resources.emplace_back(std::move(resource));
    return m_data_resources.back().id;
}

uint32_t ExtrusionPropertyContainer::store_property_data_aligned(
    extrusion_property_type owner_type, extrusion_data_id *field, const void *data, size_t byte_count, size_t alignment)
{
    if (owner_type == extrusion_property_type_invalid || field == nullptr ||
        alignment == 0 || (byte_count > 0 && data == nullptr))
        return uint32_t(-1);

    PropertySlot *slot = this->find_slot(owner_type);
    if (slot == nullptr)
        return uint32_t(-1);

    const uintptr_t slot_begin = reinterpret_cast<uintptr_t>(slot->data());
    const uintptr_t slot_end = slot_begin + slot->byte_count();
    const uintptr_t field_begin = reinterpret_cast<uintptr_t>(field);
    const uintptr_t field_end = field_begin + sizeof(extrusion_data_id);
    if (slot->data() == nullptr || field_begin < slot_begin || field_end > slot_end)
        return uint32_t(-1);

    const uint32_t owner_field_offset = uint32_t(field_begin - slot_begin);

    DataResource resource;
    resource.id = m_next_data_resource_id++;
    if (resource.id == uint32_t(-1))
        resource.id = m_next_data_resource_id++;
    resource.owner_type = owner_type;
    resource.owner_field_offset = owner_field_offset;
    resource.data.assign_copy(data, byte_count, alignment);

    this->release_property_field_resources(owner_type, owner_field_offset);
    m_data_resources.emplace_back(std::move(resource));
    *field = m_data_resources.back().id;
    return m_data_resources.back().id;
}

const void* ExtrusionPropertyContainer::stored_data(uint32_t data_id, uint32_t *byte_size_out) const
{
    if (byte_size_out != nullptr)
        *byte_size_out = 0;
    for (const DataResource &resource : m_data_resources)
        if (resource.id == data_id) {
            if (byte_size_out != nullptr)
                *byte_size_out = uint32_t(resource.data.byte_count());
            return resource.data.data();
        }
    return nullptr;
}

bool ExtrusionPropertyContainer::free_data(uint32_t data_id)
{
    for (std::vector<DataResource>::iterator it = m_data_resources.begin(); it != m_data_resources.end(); ++it)
        if (it->id == data_id) {
            m_data_resources.erase(it);
            return true;
        }
    return false;
}

void ExtrusionPropertyContainer::release_property_resources(extrusion_property_type owner_type)
{
    if (owner_type == extrusion_property_type_invalid)
        return;
    m_data_resources.erase(std::remove_if(m_data_resources.begin(), m_data_resources.end(),
        [owner_type](const DataResource &resource) {
            return resource.owner_type == owner_type;
        }), m_data_resources.end());
}

void ExtrusionPropertyContainer::release_property_field_resources(
    extrusion_property_type owner_type, uint32_t owner_field_offset)
{
    if (owner_type == extrusion_property_type_invalid || owner_field_offset == uint32_t(-1))
        return;
    m_data_resources.erase(std::remove_if(m_data_resources.begin(), m_data_resources.end(),
        [owner_type, owner_field_offset](const DataResource &resource) {
            return resource.owner_type == owner_type && resource.owner_field_offset == owner_field_offset;
        }), m_data_resources.end());
}

namespace ApiInternal {

size_t ExtrusionPropertyAccess::property_count(const ExtrusionPropertyContainer &container)
{
    return container.property_count();
}

extrusion_property_type ExtrusionPropertyAccess::property_type_at(const ExtrusionPropertyContainer &container,
                                                                  size_t idx)
{
    return container.property_type_at(idx);
}

bool ExtrusionPropertyAccess::has_property(const ExtrusionPropertyContainer &container, extrusion_property_type type)
{
    return container.has_property(type);
}

bool ExtrusionPropertyAccess::same_property_payload(const ExtrusionPropertyContainer &lhs,
                                                    extrusion_property_type type,
                                                    const ExtrusionPropertyContainer &rhs)
{
    const PropertySlot *lhs_slot = lhs.find_slot(type);
    const PropertySlot *rhs_slot = rhs.find_slot(type);
    return lhs_slot != nullptr && rhs_slot != nullptr && lhs_slot->same_payload(*rhs_slot);
}

const void *ExtrusionPropertyAccess::property_data(const ExtrusionPropertyContainer &container,
                                                   extrusion_property_type type)
{
    return container.property_data(type);
}

void *ExtrusionPropertyAccess::property_data_mutable(ExtrusionPropertyContainer &container,
                                                     extrusion_property_type type)
{
    return container.property_data_mutable(type);
}

void *ExtrusionPropertyAccess::get_or_add_property_data_mutable(ExtrusionPropertyContainer &container,
                                                                extrusion_property_type type,
                                                                size_t byte_count,
                                                                size_t alignment)
{
    return container.get_or_add_property_data_mutable(type, byte_count, alignment);
}

bool ExtrusionPropertyAccess::remove_property(ExtrusionPropertyContainer &container, extrusion_property_type type)
{
    return container.remove_property(type);
}

uint32_t ExtrusionPropertyAccess::store_data_aligned(ExtrusionPropertyContainer &container,
                                                     const void *data,
                                                     size_t byte_count,
                                                     size_t alignment)
{
    return container.store_data_aligned(data, byte_count, alignment);
}

uint32_t ExtrusionPropertyAccess::store_property_data_aligned(ExtrusionPropertyContainer &container,
                                                              extrusion_property_type owner_type,
                                                              extrusion_data_id *field,
                                                              const void *data,
                                                              size_t byte_count,
                                                              size_t alignment)
{
    return container.store_property_data_aligned(owner_type, field, data, byte_count, alignment);
}

const void *ExtrusionPropertyAccess::stored_data(const ExtrusionPropertyContainer &container,
                                                 uint32_t data_id,
                                                 uint32_t *byte_size_out)
{
    return container.stored_data(data_id, byte_size_out);
}

bool ExtrusionPropertyAccess::free_data(ExtrusionPropertyContainer &container, uint32_t data_id)
{
    return container.free_data(data_id);
}

} // namespace ApiInternal

} // namespace Slic3r
