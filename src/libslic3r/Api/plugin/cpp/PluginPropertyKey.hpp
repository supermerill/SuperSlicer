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
    /* Build the key carried directly by a public built-in payload type. */
    static PluginPropertyKey built_in()
    {
        validate_payload();
        return PluginPropertyKey(nullptr, Payload::property_type);
    }

    /*
    Register a private payload contract for one orchestrator.

    Reusing the same name and layout returns the existing id. A conflicting
    layout is rejected before any property bytes can be interpreted with the
    wrong C++ type.
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

    slic3r_property_type type() const { return m_type; }
    orchestrator_handle *orchestrator() const { return m_orchestrator; }

    template<class Owner> bool has(const Owner &owner) const
    {
        return owner.has(*this);
    }

    template<class Owner> const Payload *get(const Owner &owner) const
    {
        return owner.get(*this);
    }

    template<class Owner> Payload *get_mutable(Owner &&owner) const
    {
        return std::forward<Owner>(owner).get_mutable(*this);
    }

    template<class Owner> Payload &get_or_add(Owner &&owner) const
    {
        return std::forward<Owner>(owner).get_or_add(*this);
    }

    template<class Owner> bool remove(Owner &&owner) const
    {
        return std::forward<Owner>(owner).remove(*this);
    }

private:
    PluginPropertyKey(orchestrator_handle *orchestrator, slic3r_property_type type) :
        m_orchestrator(orchestrator), m_type(type)
    {
    }

    static void validate_payload()
    {
        static_assert(std::is_trivially_copyable<Payload>::value,
                      "Plugin property payloads are copied as bytes and must be trivially copyable.");
    }

    orchestrator_handle *m_orchestrator;
    slic3r_property_type m_type;
};

} // namespace slic3r_api

#endif // slic3r_Api_plugin_cpp_PluginPropertyKey_hpp_
