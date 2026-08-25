///|/ Copyright (c) SuperSlicer 2026 Remi Durand @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_ExtrusionProperty_hpp_
#define slic3r_ExtrusionProperty_hpp_

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "Api/plugin/c/slic3r_extrusion_property.h"
#include "ExtrusionRole.hpp"
#include "PropertyStorage.hpp"
#include "libslic3r.h"

namespace Slic3r {

class Flow;
using PropertySlot = PropertyStorageSlot;
struct ExtrusionAttributes;
class ExtrusionPropertySpeed;
class ExtrusionPropertyModifier;
class ExtrusionPropertyCustomGcode;
class ExtrusionPropertyCustomGcodeText;
class ExtrusionPropertySpecialCommand;
class ExtrusionPropertyOverhang;
class ExtrusionPropertyZOffset;
class ExtrusionPropertyZProfile;
class ExtrusionPropertyLoopRole;
class ExtrusionPropertyInfill;
namespace ApiInternal { struct ExtrusionPropertyAccess; }

using ExtrusionPropertyUPtr = std::unique_ptr<PropertySlot>;
using ExtrusionPropertyUPtrs = std::vector<ExtrusionPropertyUPtr>;

using extrusion_property_type = ::extrusion_property_type;
using extrusion_data_id = ::extrusion_data_id;

enum : extrusion_property_type {
    extrusion_property_type_invalid         = EXTRUSION_PROPERTY_TYPE_INVALID,
    extrusion_property_type_attributes      = EXTRUSION_PROPERTY_TYPE_ATTRIBUTES,
    extrusion_property_type_speed           = EXTRUSION_PROPERTY_TYPE_SPEED,
    extrusion_property_type_modifier        = EXTRUSION_PROPERTY_TYPE_MODIFIER,
    extrusion_property_type_custom_gcode    = EXTRUSION_PROPERTY_TYPE_CUSTOM_GCODE,
    extrusion_property_type_special_command = EXTRUSION_PROPERTY_TYPE_SPECIAL_COMMAND,
    extrusion_property_type_overhang        = EXTRUSION_PROPERTY_TYPE_OVERHANG,
    extrusion_property_type_z_offset        = EXTRUSION_PROPERTY_TYPE_Z_OFFSET,
    extrusion_property_type_z_profile       = 8,
    extrusion_property_type_loop_role       = EXTRUSION_PROPERTY_TYPE_PERIMETER,
    extrusion_property_type_infill          = EXTRUSION_PROPERTY_TYPE_INFILL,
};

struct ExtrusionFlow : c_extrusion_flow
{
    ExtrusionFlow() : c_extrusion_flow{ -1., -1.f, -1.f } {}
    ExtrusionFlow(double mm3_per_mm, float width, float height) :
        c_extrusion_flow{ mm3_per_mm, width, height } {}
    ExtrusionFlow(const Flow &flow);

    void set_force_e_per_mm() { this->height = -2; }
    bool force_e_per_mm() const { return this->height == -2; }

    // Volumetric velocity. mm^3 of plastic per mm of linear head motion. Used by the G-code generator.
    // !!! if height == -2, then mm3_per_mm is changed into e_per_mm (used for exact unretraction) !!! (very unsafe, don't use it for normal extrusions)
};

inline bool operator==(const ExtrusionFlow &lhs, const ExtrusionFlow &rhs)
{
    return lhs.mm3_per_mm == rhs.mm3_per_mm && lhs.width == rhs.width && lhs.height == rhs.height;
}

struct ExtrusionAttributes : c_extrusion_property_attributes
{
    static constexpr extrusion_property_type property_type = extrusion_property_type_attributes;

    ExtrusionAttributes();
    ExtrusionAttributes(ExtrusionRole role);
    ExtrusionAttributes(ExtrusionRole role, const Flow &flow);
    ExtrusionAttributes(ExtrusionRole role, const ExtrusionFlow &flow);

    ExtrusionRole extrusion_role() const { return ExtrusionRole(ExtrusionRoleModifier(this->role)); }
    void set_role(ExtrusionRole new_role) { this->role = uint16_t(new_role()); }
    ExtrusionFlow flow() const { return ExtrusionFlow(this->mm3_per_mm, this->width, this->height); }
    operator ExtrusionFlow() const { return this->flow(); }
    void set_force_e_per_mm() { this->height = -2; }
    bool force_e_per_mm() const { return this->height == -2; }
    ExtrusionPropertyUPtr clone() const;

    // What is the role / purpose of this extrusion?
    // set to true to prevent seam on this path.
};

inline bool operator==(const ExtrusionAttributes &lhs, const ExtrusionAttributes &rhs)
{
    return lhs.mm3_per_mm == rhs.mm3_per_mm && lhs.width == rhs.width && lhs.height == rhs.height &&
           lhs.role == rhs.role;
}

// These are a state. They are used for all children if it's not overriden.
// After the end of this entity, it's reverted to previous state.
class ExtrusionPropertySpeed : public c_extrusion_property_speed
{
public:
    static constexpr extrusion_property_type property_type = extrusion_property_type_speed;

    ExtrusionPropertySpeed(float speed = -1, float accel = -1, float pa = -1, float fan = -1, float temp = -1)
        : c_extrusion_property_speed{ speed, accel, pa, fan, temp } {}

    ExtrusionPropertySpeed& speed(float speed) { speed_mm_per_s = speed; return *this; }
    ExtrusionPropertySpeed& acceleration(float accel) { accel_mm_per_s2 = accel; return *this; }
    ExtrusionPropertySpeed& presure_advance(float pa) { pressure_adv = pa; return *this; }
    ExtrusionPropertySpeed& fan_speed(float fspeed) { assert(fspeed >= -1 && fspeed <= 100); fan_speed_percent = fspeed; return *this; }
    ExtrusionPropertySpeed& temperature(float temp) { temperature_C = temp; return *this; }

    ExtrusionPropertyUPtr clone() const;
};

// Store switches to activate/deactivate/enforce gcode features like retract, lift, etc.
class ExtrusionPropertyModifier : public c_extrusion_property_modifier
{
public:
    static constexpr extrusion_property_type property_type = extrusion_property_type_modifier;

    ExtrusionPropertyModifier();

    ExtrusionPropertyModifier& set_enforce_travel(bool enforce = true) { enforce_travel = enforce; return *this; }
    ExtrusionPropertyModifier& set_enforce_retraction(bool enforce = true) { enforce_retraction = enforce; return *this; }
    ExtrusionPropertyModifier& set_enforce_unlift(bool enforce = true) { enforce_unlift = enforce; return *this; }
    ExtrusionPropertyModifier& set_disable_retraction(bool disable = true) { disable_retraction = disable; return *this; }
    ExtrusionPropertyModifier& set_disable_lift(bool disable = true) { disable_lift = disable; return *this; }
    ExtrusionPropertyModifier& set_toolchange_retraction(bool is = true) { toolchange_retraction = is; return *this; }

    ExtrusionPropertyUPtr clone() const;
};

class ExtrusionPropertyCustomGcode : public c_extrusion_property_custom_gcode
{
public:
    static constexpr extrusion_property_type property_type = extrusion_property_type_custom_gcode;

    enum class Code : uint32_t {
        GCODE = C_EXTRUSION_CUSTOM_GCODE_GCODE,
        COMMENT = C_EXTRUSION_CUSTOM_GCODE_COMMENT,
        SCRIPT = C_EXTRUSION_CUSTOM_GCODE_SCRIPT,
    };

    ExtrusionPropertyCustomGcode();
    ExtrusionPropertyCustomGcode(Code c, extrusion_data_id text_id);

    Code code() const { return Code(this->kind); }
    ExtrusionPropertyUPtr clone() const;
};

class ExtrusionPropertyCustomGcodeText
{
public:
    using Code = ExtrusionPropertyCustomGcode::Code;

    Code code = Code::GCODE;
    std::string gcode;

    explicit ExtrusionPropertyCustomGcodeText(const std::string &str);
    ExtrusionPropertyCustomGcodeText(Code c, const std::string &str) : code(c), gcode(str) {}
};

class ExtrusionPropertySpecialCommand : public c_extrusion_property_special_command
{
public:
    static constexpr extrusion_property_type property_type = extrusion_property_type_special_command;

    enum class Code : uint32_t {
        TOOLCHANGE = C_EXTRUSION_SPECIAL_COMMAND_TOOLCHANGE,
        SAVE_AND_RESET_SPEED_RATIO = C_EXTRUSION_SPECIAL_COMMAND_SAVE_AND_RESET_SPEED_RATIO,
        RESTORE_SPEED_RATIO = C_EXTRUSION_SPECIAL_COMMAND_RESTORE_SPEED_RATIO,
        FLUSH_PLANNER_QUEUE = C_EXTRUSION_SPECIAL_COMMAND_FLUSH_PLANNER_QUEUE,
        EXTRUSION = C_EXTRUSION_SPECIAL_COMMAND_EXTRUSION,
        RETRACT = C_EXTRUSION_SPECIAL_COMMAND_RETRACT,
        PAUSE = C_EXTRUSION_SPECIAL_COMMAND_PAUSE,
        WAIT_FOR_TEMP = C_EXTRUSION_SPECIAL_COMMAND_WAIT_FOR_TEMP,
        DISABLE_PREVIEW = C_EXTRUSION_SPECIAL_COMMAND_DISABLE_PREVIEW,
        ENABLE_PREVIEW = C_EXTRUSION_SPECIAL_COMMAND_ENABLE_PREVIEW,
        EXTRUDER_CURRENT = C_EXTRUSION_SPECIAL_COMMAND_EXTRUDER_CURRENT,
    };

    ExtrusionPropertySpecialCommand(Code c) : c_extrusion_property_special_command{ c_extrusion_special_command(c), 0. } {}
    ExtrusionPropertySpecialCommand(Code c, double data) : c_extrusion_property_special_command{ c_extrusion_special_command(c), data } {}

    Code command_code() const { return Code(this->code); }
    ExtrusionPropertyUPtr clone() const;
};

class ExtrusionPropertyOverhang : public c_extrusion_property_overhang
{
public:
    static constexpr extrusion_property_type property_type = extrusion_property_type_overhang;

    ExtrusionPropertyOverhang();
    ExtrusionPropertyOverhang(float start_dist, float end_dist)
        : c_extrusion_property_overhang{ start_dist, end_dist, 0.f, 0, 0, 0, 0 } {}
    ExtrusionPropertyOverhang(float start_dist, float end_dist, float curled_ratio)
        : c_extrusion_property_overhang{ start_dist, end_dist, curled_ratio, 0, 0, 0, 0 } {}
    ExtrusionPropertyOverhang(float start_dist, float end_dist, float curled_ratio, bool full_flow, bool full_speed, bool dynamic_flow, bool dynamic_speed)
        : c_extrusion_property_overhang{
              start_dist, end_dist, curled_ratio,
              uint8_t(full_flow), uint8_t(full_speed), uint8_t(dynamic_flow), uint8_t(dynamic_speed) } {}

    ExtrusionPropertyUPtr clone() const;
};

class ExtrusionPropertyZOffset : public c_extrusion_property_z_offset
{
public:
    static constexpr extrusion_property_type property_type = extrusion_property_type_z_offset;

    ExtrusionPropertyZOffset() : c_extrusion_property_z_offset{ 0 } {}
    explicit ExtrusionPropertyZOffset(coord_t offset) : c_extrusion_property_z_offset{ offset } {}

    ExtrusionPropertyUPtr clone() const;
};

class ExtrusionPropertyLoopRole : public c_extrusion_property_perimeter
{
public:
    static constexpr extrusion_property_type property_type = extrusion_property_type_loop_role;

    // if perimeter, this is the perimeter count. 0 = external, negative = not a perimeter.
    // Set of tags to identify the loop.
    // important one: elrHole => hole-perimeter, else it's a contour-perimeter

    ExtrusionPropertyLoopRole();
    explicit ExtrusionPropertyLoopRole(ExtrusionLoopRole role);

    ExtrusionLoopRole perimeter_role() const { return ExtrusionLoopRole(this->perimeter_flags); }
    void set_perimeter_role(ExtrusionLoopRole role) { this->perimeter_flags = uint16_t(role); }
    ExtrusionPropertyUPtr clone() const;
};

class ExtrusionPropertyInfill : public c_extrusion_property_infill
{
public:
    static constexpr extrusion_property_type property_type = extrusion_property_type_infill;

    ExtrusionPropertyInfill() : c_extrusion_property_infill{ 0 } {}
    explicit ExtrusionPropertyInfill(uint64_t surface_id) : c_extrusion_property_infill{ surface_id } {}

    ExtrusionPropertyUPtr clone() const;
};

template<typename PropertyType>
struct ExtrusionPropertyTraits
{
    static constexpr extrusion_property_type type = PropertyType::property_type;
};

static_assert(sizeof(ExtrusionFlow) == sizeof(c_extrusion_flow), "ExtrusionFlow must keep the C ABI layout");
static_assert(sizeof(ExtrusionAttributes) == sizeof(c_extrusion_property_attributes), "ExtrusionAttributes must keep the C ABI layout");
static_assert(sizeof(ExtrusionPropertySpeed) == sizeof(c_extrusion_property_speed), "ExtrusionPropertySpeed must keep the C ABI layout");
static_assert(sizeof(ExtrusionPropertyModifier) == sizeof(c_extrusion_property_modifier), "ExtrusionPropertyModifier must keep the C ABI layout");
static_assert(sizeof(ExtrusionPropertyCustomGcode) == sizeof(c_extrusion_property_custom_gcode), "ExtrusionPropertyCustomGcode must keep the C ABI layout");
static_assert(sizeof(ExtrusionPropertySpecialCommand) == sizeof(c_extrusion_property_special_command), "ExtrusionPropertySpecialCommand must keep the C ABI layout");
static_assert(sizeof(ExtrusionPropertyOverhang) == sizeof(c_extrusion_property_overhang), "ExtrusionPropertyOverhang must keep the C ABI layout");
static_assert(sizeof(ExtrusionPropertyZOffset) == sizeof(c_extrusion_property_z_offset), "ExtrusionPropertyZOffset must keep the C ABI layout");
static_assert(sizeof(ExtrusionPropertyLoopRole) == sizeof(c_extrusion_property_perimeter), "ExtrusionPropertyLoopRole must keep the C ABI layout");
static_assert(sizeof(ExtrusionPropertyInfill) == sizeof(c_extrusion_property_infill), "ExtrusionPropertyInfill must keep the C ABI layout");

static_assert(alignof(ExtrusionFlow) == alignof(c_extrusion_flow), "ExtrusionFlow must keep the C ABI alignment");
static_assert(alignof(ExtrusionAttributes) == alignof(c_extrusion_property_attributes), "ExtrusionAttributes must keep the C ABI alignment");
static_assert(alignof(ExtrusionPropertySpeed) == alignof(c_extrusion_property_speed), "ExtrusionPropertySpeed must keep the C ABI alignment");
static_assert(alignof(ExtrusionPropertyModifier) == alignof(c_extrusion_property_modifier), "ExtrusionPropertyModifier must keep the C ABI alignment");
static_assert(alignof(ExtrusionPropertyCustomGcode) == alignof(c_extrusion_property_custom_gcode), "ExtrusionPropertyCustomGcode must keep the C ABI alignment");
static_assert(alignof(ExtrusionPropertySpecialCommand) == alignof(c_extrusion_property_special_command), "ExtrusionPropertySpecialCommand must keep the C ABI alignment");
static_assert(alignof(ExtrusionPropertyOverhang) == alignof(c_extrusion_property_overhang), "ExtrusionPropertyOverhang must keep the C ABI alignment");
static_assert(alignof(ExtrusionPropertyZOffset) == alignof(c_extrusion_property_z_offset), "ExtrusionPropertyZOffset must keep the C ABI alignment");
static_assert(alignof(ExtrusionPropertyLoopRole) == alignof(c_extrusion_property_perimeter), "ExtrusionPropertyLoopRole must keep the C ABI alignment");
static_assert(alignof(ExtrusionPropertyInfill) == alignof(c_extrusion_property_infill), "ExtrusionPropertyInfill must keep the C ABI alignment");

static_assert(std::is_standard_layout<ExtrusionFlow>::value, "ExtrusionFlow must keep the C ABI layout");
static_assert(std::is_standard_layout<ExtrusionAttributes>::value, "ExtrusionAttributes must keep the C ABI layout");
static_assert(std::is_standard_layout<ExtrusionPropertySpeed>::value, "ExtrusionPropertySpeed must keep the C ABI layout");
static_assert(std::is_standard_layout<ExtrusionPropertyModifier>::value, "ExtrusionPropertyModifier must keep the C ABI layout");
static_assert(std::is_standard_layout<ExtrusionPropertyCustomGcode>::value, "ExtrusionPropertyCustomGcode must keep the C ABI layout");
static_assert(std::is_standard_layout<ExtrusionPropertySpecialCommand>::value, "ExtrusionPropertySpecialCommand must keep the C ABI layout");
static_assert(std::is_standard_layout<ExtrusionPropertyOverhang>::value, "ExtrusionPropertyOverhang must keep the C ABI layout");
static_assert(std::is_standard_layout<ExtrusionPropertyZOffset>::value, "ExtrusionPropertyZOffset must keep the C ABI layout");
static_assert(std::is_standard_layout<ExtrusionPropertyLoopRole>::value, "ExtrusionPropertyLoopRole must keep the C ABI layout");
static_assert(std::is_standard_layout<ExtrusionPropertyInfill>::value, "ExtrusionPropertyInfill must keep the C ABI layout");

static_assert(std::is_trivially_copyable<ExtrusionAttributes>::value, "Stored extrusion properties must be trivially copyable");
static_assert(std::is_trivially_copyable<ExtrusionPropertySpeed>::value, "Stored extrusion properties must be trivially copyable");
static_assert(std::is_trivially_copyable<ExtrusionPropertyModifier>::value, "Stored extrusion properties must be trivially copyable");
static_assert(std::is_trivially_copyable<ExtrusionPropertyCustomGcode>::value, "Stored extrusion properties must be trivially copyable");
static_assert(std::is_trivially_copyable<ExtrusionPropertySpecialCommand>::value, "Stored extrusion properties must be trivially copyable");
static_assert(std::is_trivially_copyable<ExtrusionPropertyOverhang>::value, "Stored extrusion properties must be trivially copyable");
static_assert(std::is_trivially_copyable<ExtrusionPropertyZOffset>::value, "Stored extrusion properties must be trivially copyable");
static_assert(std::is_trivially_copyable<ExtrusionPropertyLoopRole>::value, "Stored extrusion properties must be trivially copyable");
static_assert(std::is_trivially_copyable<ExtrusionPropertyInfill>::value, "Stored extrusion properties must be trivially copyable");

// Type-tagged raw storage for one extrusion property.
// Built-in extrusion properties and generic plugin properties now use the same
// byte-storage backend. The extrusion container keeps the higher-level rules:
// inherited property semantics, C++ wrappers and data resources owned by a
// property field.

template<typename PropertyType>
inline ExtrusionPropertyUPtr clone_property_to_slot(const PropertyType &property)
{
    ExtrusionPropertyUPtr out = std::make_unique<PropertySlot>();
    out->emplace<PropertyType>(property);
    return out;
}

inline ExtrusionPropertyUPtr ExtrusionAttributes::clone() const { return clone_property_to_slot(*this); }
inline ExtrusionPropertyUPtr ExtrusionPropertySpeed::clone() const { return clone_property_to_slot(*this); }
inline ExtrusionPropertyUPtr ExtrusionPropertyModifier::clone() const { return clone_property_to_slot(*this); }
inline ExtrusionPropertyUPtr ExtrusionPropertyCustomGcode::clone() const { return clone_property_to_slot(*this); }
inline ExtrusionPropertyUPtr ExtrusionPropertySpecialCommand::clone() const { return clone_property_to_slot(*this); }
inline ExtrusionPropertyUPtr ExtrusionPropertyOverhang::clone() const { return clone_property_to_slot(*this); }
inline ExtrusionPropertyUPtr ExtrusionPropertyZOffset::clone() const { return clone_property_to_slot(*this); }
inline ExtrusionPropertyUPtr ExtrusionPropertyLoopRole::clone() const { return clone_property_to_slot(*this); }
inline ExtrusionPropertyUPtr ExtrusionPropertyInfill::clone() const { return clone_property_to_slot(*this); }

// Small typed property bag for extrusion interpretation modifiers.
// Most entities have no property, and the few that do usually carry one or two;
// the vector keeps storage compact while PropertySlot makes the "one property per
// type" rule explicit and avoids dynamic casts on typed lookups.
class ExtrusionPropertyContainer
{
    friend struct ApiInternal::ExtrusionPropertyAccess;

public:
    ExtrusionPropertyContainer() = default;
    explicit ExtrusionPropertyContainer(ExtrusionPropertyUPtr &&property);
    explicit ExtrusionPropertyContainer(ExtrusionPropertyUPtrs &&properties);
    ExtrusionPropertyContainer(const ExtrusionPropertyContainer &rhs);
    ExtrusionPropertyContainer(ExtrusionPropertyContainer &&rhs) noexcept = default;
    ExtrusionPropertyContainer& operator=(const ExtrusionPropertyContainer &rhs);
    ExtrusionPropertyContainer& operator=(ExtrusionPropertyContainer &&rhs) noexcept = default;

    bool has_properties() const { return !m_properties.empty(); }
    void clear_properties();
    ExtrusionPropertyUPtrs clone_properties() const;

    template<typename PropertyType> PropertyType& add_property(const PropertyType &property)
    {
        PropertySlot slot;
        slot.emplace<PropertyType>(property);
        PropertySlot *stored = this->find_slot(slot.type());
        if (stored != nullptr) {
            this->release_property_resources(slot.type());
            *stored = std::move(slot);
            return stored->as<PropertyType>();
        }
        m_properties.emplace_back(std::move(slot));
        return m_properties.back().as<PropertyType>();
    }

    void add_property(ExtrusionPropertyUPtr &&property);
    ExtrusionPropertyCustomGcode& add_property(const ExtrusionPropertyCustomGcodeText &property);
    std::string custom_gcode_string(const ExtrusionPropertyCustomGcode &property) const;

    template<typename PropertyType> PropertyType* get_property()
    {
        PropertySlot *slot = this->find_slot(ExtrusionPropertyTraits<PropertyType>::type);
        if (slot != nullptr)
            return slot->get_if<PropertyType>();
        return nullptr;
    }

    template<typename PropertyType> const PropertyType* get_property() const
    {
        const PropertySlot *slot = this->find_slot(ExtrusionPropertyTraits<PropertyType>::type);
        if (slot != nullptr)
            return slot->get_if<PropertyType>();
        return nullptr;
    }

    template<typename PropertyType, typename... Args> PropertyType& get_or_add_property(Args&&... args)
    {
        if (PropertyType *property = this->get_property<PropertyType>())
            return *property;
        PropertySlot slot;
        PropertyType &out = slot.emplace<PropertyType>(std::forward<Args>(args)...);
        m_properties.emplace_back(std::move(slot));
        return out;
    }

    template<typename PropertyType> bool remove_property()
    {
        for (std::vector<PropertySlot>::iterator it = m_properties.begin(); it != m_properties.end(); ++ it)
            if (it->type() == ExtrusionPropertyTraits<PropertyType>::type) {
                this->release_property_resources(it->type());
                m_properties.erase(it);
                return true;
            }
        return false;
    }

protected:

    size_t property_count() const { return m_properties.size(); }
    extrusion_property_type property_type_at(size_t idx) const;
    bool has_property(extrusion_property_type type) const { return this->find_slot(type) != nullptr; }
    const void* property_data(extrusion_property_type type) const;
    void* property_data_mutable(extrusion_property_type type);
    void* get_or_add_property_data_mutable(extrusion_property_type type, size_t byte_count, size_t alignment);
    bool remove_property(extrusion_property_type type);

    uint32_t store_data_aligned(const void *data, size_t byte_count, size_t alignment);
    uint32_t store_property_data_aligned(extrusion_property_type owner_type, extrusion_data_id *field, const void *data, size_t byte_count, size_t alignment);
    const void* stored_data(uint32_t data_id, uint32_t *byte_size_out) const;
    bool free_data(uint32_t data_id);
    void release_property_resources(extrusion_property_type owner_type);
    void release_property_field_resources(extrusion_property_type owner_type, uint32_t owner_field_offset);

    PropertySlot* find_slot(extrusion_property_type type);
    const PropertySlot* find_slot(extrusion_property_type type) const;

    struct DataResource
    {
        uint32_t id = 0;
        extrusion_property_type owner_type = extrusion_property_type_invalid;
        uint32_t owner_field_offset = uint32_t(-1);
        PropertyRawBuffer data;

        DataResource() = default;
        DataResource(const DataResource &rhs);
        DataResource(DataResource &&rhs) noexcept = default;
        DataResource& operator=(const DataResource &rhs);
        DataResource& operator=(DataResource &&rhs) noexcept = default;
    };

    std::vector<PropertySlot> m_properties;
    std::vector<DataResource> m_data_resources;
    uint32_t m_next_data_resource_id = 1;
};

template<typename PropertyType>
class AddGetEEAttribute
{
public:
    PropertyType *found = nullptr;
    PropertyType& add_or_get(ExtrusionPropertyContainer &entity)
    {
        found = &entity.get_or_add_property<PropertyType>();
        return *found;
    }
};

template<typename PropertyType>
class GetEEAttribute
{
public:
    const PropertyType *found = nullptr;
    const PropertyType* get(const ExtrusionPropertyContainer &entity)
    {
        found = entity.get_property<PropertyType>();
        return found;
    }
};

} // namespace Slic3r

#endif
