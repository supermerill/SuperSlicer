///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_PluginPropertyKey_hpp_
#define slic3r_Api_plugin_cpp_PluginPropertyKey_hpp_

/*
Typed keys for plugin properties
================================

A property payload describes the bytes stored on an object. Its numeric type
identifies which property those bytes represent. Built-in payloads know that
number at compile time, while plugin-defined payloads receive it from the
orchestrator after registering a stable namespaced name.

PluginPropertyKey keeps those two pieces together: the numeric type and the
orchestrator that owns it. Code accessing a property therefore cannot
accidentally create a dynamic payload with another orchestrator. The key is a
small non-owning value and should normally be retained by the plugin instance.

The owner passed to has(), get(), get_mutable(), get_or_add() or remove() may be
a generic PluginProperties view or an extrusion entity view. Those views
provide the storage-specific operation while this class provides the common
typed identity.

Developer guide: [Using Plugin Properties](/doc/plugins/properties.md)
*/

#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"

namespace slic3r_api {

template<class Payload> class PluginPropertyKey
{
public:
    /*
    Build a key for a property whose numeric id is part of the public API.

    Payload must expose a static property_type member, normally through
    BuiltInPluginPropertyPayload. Built-in ids are understood directly by the
    host, so their keys do not retain an orchestrator.
    */
    static PluginPropertyKey built_in()
    {
        validate_payload();
        return PluginPropertyKey(nullptr, Payload::property_type);
    }

    /*
    Register a private payload contract for one orchestrator.

    Reusing the same name and layout returns the existing id. A conflicting
    layout is rejected before any property bytes can be interpreted with the
    wrong C++ type. Keep the returned key in the plugin instance: dynamic ids
    are meaningful only for the orchestrator that registered them and must not
    be serialized as stable values.
    */
    static PluginPropertyKey register_dynamic(orchestrator_handle *orchestrator,
                                              const char *namespaced_name)
    {
        validate_payload();
        if (orchestrator == nullptr)
            throw std::invalid_argument("A dynamic plugin property needs its owning orchestrator.");
        if (namespaced_name == nullptr || namespaced_name[0] == '\0')
            throw std::invalid_argument("A dynamic plugin property needs a non-empty namespaced name.");

        const slic3r_property_type type = orchestrator_register_property(
            orchestrator, namespaced_name, uint32_t(sizeof(Payload)), uint32_t(alignof(Payload)));
        if (type == SLIC3R_PROPERTY_TYPE_INVALID)
            throw std::runtime_error("The dynamic plugin property has an incompatible registered layout.");
        return PluginPropertyKey(orchestrator, type);
    }

    /*
    Return the numeric property id passed to the low-level C storage API.

    Built-in ids are stable API constants. A dynamic id is valid only inside
    the orchestrator returned by orchestrator().
    */
    slic3r_property_type type() const { return m_type; }

    /*
    Return the orchestrator needed to create this property.

    The pointer is borrowed and remains owned by the plugin runtime. It is null
    for built-in properties because the host already knows their layouts.
    */
    orchestrator_handle *orchestrator() const { return m_orchestrator; }

    /*
    Report whether owner currently stores this property.

    The key delegates the lookup to Owner, allowing the same call for a generic
    PluginProperties container or an extrusion view. No property is created.
    */
    template<class Owner> bool has(const Owner &owner) const
    {
        return owner.has(*this);
    }

    /*
    Return a read-only view of this property, or nullptr when it is absent.

    The returned payload is borrowed from owner. Do not retain it after owner
    is modified, moved or destroyed; the concrete Owner documents any stricter
    invalidation rules.
    */
    template<class Owner> const Payload *get(const Owner &owner) const
    {
        return owner.get(*this);
    }

    /*
    Return an existing mutable property, or nullptr when it is absent.

    This operation never creates a payload. The returned pointer is borrowed
    and follows the same lifetime rules as get().
    */
    template<class Owner> Payload *get_mutable(Owner &&owner) const
    {
        return std::forward<Owner>(owner).get_mutable(*this);
    }

    /*
    Return the existing property or ask owner to create a zero-initialized one.

    The key supplies both the payload type and, for dynamic properties, the
    orchestrator required by the host. Creation failure is reported by the
    concrete Owner, normally as std::runtime_error.

    Example:
        Payload &property = key.get_or_add(entity);
    */
    template<class Owner> Payload &get_or_add(Owner &&owner) const
    {
        return std::forward<Owner>(owner).get_or_add(*this);
    }

    /*
    Remove this property from owner.

    Returns true when a property was removed and false when it was not present.
    Any pointers or references previously obtained for that payload become
    invalid immediately.
    */
    template<class Owner> bool remove(Owner &&owner) const
    {
        return std::forward<Owner>(owner).remove(*this);
    }

private:
    /*
    Keep construction behind the two factories so built-in and dynamic keys
    cannot be assembled with an inconsistent orchestrator/id pair.
    */
    PluginPropertyKey(orchestrator_handle *orchestrator, slic3r_property_type type) :
        m_orchestrator(orchestrator), m_type(type)
    {
    }

    /*
    Enforce the storage contract before a key can expose Payload as raw bytes.
    */
    static void validate_payload()
    {
        static_assert(std::is_trivially_copyable<Payload>::value,
                      "Plugin property payloads are copied as bytes and must be trivially copyable.");
    }

    // Borrowed registry that owns a dynamic id; null for a built-in id.
    orchestrator_handle *m_orchestrator;
    // Numeric identity used by both data-tree and extrusion property storage.
    slic3r_property_type m_type;
};

/*
Attach one compile-time property id and its convenient typed key to a public
built-in payload without adding bytes to that payload.

Derived is part of the template identity so EPropertySpeed::key has the exact
type PluginPropertyKey<EPropertySpeed>, not a key for the C ABI base struct.
The inline static key is initialized only when used, after Derived is complete.
*/
template<class Derived, class CAbiPayload, slic3r_property_type TypeValue>
struct BuiltInPluginPropertyPayload : CAbiPayload
{
    // Stable property id published by the corresponding C payload contract.
    static constexpr slic3r_property_type property_type = TypeValue;
    // Ready-to-use typed key shared by every instance of this built-in payload.
    static const PluginPropertyKey<Derived> key;
};

template<class Derived, class CAbiPayload, slic3r_property_type TypeValue>
inline const PluginPropertyKey<Derived>
BuiltInPluginPropertyPayload<Derived, CAbiPayload, TypeValue>::key = [] {
    // This definition is instantiated when the concrete payload's key is used,
    // after the CRTP-derived helper has become a complete type.
    static_assert(sizeof(Derived) == sizeof(CAbiPayload),
                  "The C++ property helper must not add bytes to its C payload.");
    static_assert(alignof(Derived) == alignof(CAbiPayload),
                  "The C++ property helper must preserve its C payload alignment.");
    return PluginPropertyKey<Derived>::built_in();
}();

} // namespace slic3r_api

#endif // slic3r_Api_plugin_cpp_PluginPropertyKey_hpp_
