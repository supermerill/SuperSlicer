///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_PluginProperty_hpp_
#define slic3r_PluginProperty_hpp_

#include <cstddef>
#include <cstdint>
#include <cassert>
#include <memory>
#include <type_traits>
#include <vector>

#include "Api/plugin/c/slic3r_data_tree.h"
#include "PropertyStorage.hpp"

namespace Slic3r {

/*
Built-in support marker for auxiliary layers.

Auxiliary layers are generic Layer objects. Support is not a Layer subclass and
does not have dedicated helper functions: code recognizes support by checking
for this property on the Layer's PluginPropertyContainer, then reads the
interface_id from the payload when it needs support-interface alternation.
*/
struct LayerSupportProperty : c_layer_support_property
{
    static constexpr plugin_property_type property_type = PLUGIN_PROPERTY_TYPE_LAYER_SUPPORT;
};

/*
Typed property payloads for plugin-controlled data-tree objects.

PluginPropertyContainer is the metadata side channel for objects that are not
extrusion entities: Surface, Layer, LayerSliceIsland, LayerRegionIsland, and
similar data-tree nodes. It replaces the old double-valued tags with typed C
payloads that can travel with an object when the host copies, clips or splits
that object.

Typical use:

1. A plugin registers a namespaced property name with the orchestrator and gets
   a numeric slic3r_property_type for this run.
2. The plugin stores a small plain C struct in this container using that type.
3. Later plugins read the same payload by the numeric type, without depending
   on native C++ classes.

This container is intentionally simpler than ExtrusionPropertyContainer:
- it stores small plain-data payloads directly next to the object;
- it does not own secondary string/buffer resources;
- it has no inheritance rules like extrusion properties have;
- it is copied when the host copies or splits the object.

Property payloads must be stable C-layout values. Do not store owning pointers,
std::string, std::vector, or references here: the container only copies bytes,
so such objects would be copied without running their own copy constructors.
*/
class PluginPropertyContainer
{
public:
    PluginPropertyContainer() = default;
    PluginPropertyContainer(const PluginPropertyContainer &rhs);
    PluginPropertyContainer(PluginPropertyContainer &&rhs) noexcept = default;
    PluginPropertyContainer &operator=(const PluginPropertyContainer &rhs);
    PluginPropertyContainer &operator=(PluginPropertyContainer &&rhs) noexcept = default;
    ~PluginPropertyContainer() = default;

    bool has_properties() const;
    size_t property_count() const;
    slic3r_property_type property_type_at(size_t idx) const;
    bool has_property(slic3r_property_type type) const;
    uint32_t property_data_size(slic3r_property_type type) const;
    const void *property_data(slic3r_property_type type) const;
    void *property_data_mutable(slic3r_property_type type);

    /*
    Return an existing payload or create a zero-initialized one.

    This low-level host method trusts the caller to pass the registered layout
    for type. Public C and C++ plugin helpers should first ask the orchestrator
    for byte_count/alignment, then call here. If the type already exists with a
    different size or alignment, nullptr is returned. That protects a plugin
    from interpreting another payload layout as its own struct after an API or
    plugin-version mismatch.
    */
    void *get_or_add_property_data_mutable(slic3r_property_type type, size_t byte_count, size_t alignment);
    bool remove_property(slic3r_property_type type);
    void clear_properties();
    void copy_properties_from(const PluginPropertyContainer &rhs);
    bool properties_equal(const PluginPropertyContainer &rhs) const;

    template<class PropertyType> const PropertyType *get_property() const
    {
        static_assert(std::is_trivially_copyable<PropertyType>::value,
                      "PluginProperty payloads are copied as bytes and must be trivially copyable.");
        const void *data = property_data(PropertyType::property_type);
        if (data == nullptr || property_data_size(PropertyType::property_type) != sizeof(PropertyType))
            return nullptr;
        return reinterpret_cast<const PropertyType *>(data);
    }

    template<class PropertyType> PropertyType *get_property()
    {
        static_assert(std::is_trivially_copyable<PropertyType>::value,
                      "PluginProperty payloads are copied as bytes and must be trivially copyable.");
        void *data = property_data_mutable(PropertyType::property_type);
        if (data == nullptr || property_data_size(PropertyType::property_type) != sizeof(PropertyType))
            return nullptr;
        return reinterpret_cast<PropertyType *>(data);
    }

    template<class PropertyType> PropertyType &get_or_add_property()
    {
        /*
        Host-only typed shortcut.

        Plugin ABI code should prefer the helper that takes an orchestrator
        handle, because custom property ids are registered per orchestrator.
        This shortcut is for built-in host code and tests that already own a
        compile-time property_type constant.
        */
        static_assert(std::is_trivially_copyable<PropertyType>::value,
                      "PluginProperty payloads are copied as bytes and must be trivially copyable.");
        void *data = get_or_add_property_data_mutable(
            PropertyType::property_type, sizeof(PropertyType), alignof(PropertyType));
        assert(data != nullptr);
        return *reinterpret_cast<PropertyType *>(data);
    }

    template<class PropertyType> bool remove_property()
    {
        return remove_property(PropertyType::property_type);
    }

private:
    PropertyStorageSlot *find_slot(slic3r_property_type type);
    const PropertyStorageSlot *find_slot(slic3r_property_type type) const;
    std::vector<PropertyStorageSlot> &mutable_properties();

    /*
    Most data-tree objects never receive plugin properties. Keeping the vector
    behind a unique_ptr makes the empty case one pointer instead of a full
    vector object in every Surface or LayerIsland.
    */
    std::unique_ptr<std::vector<PropertyStorageSlot>> m_properties;
};

} // namespace Slic3r

#endif // slic3r_PluginProperty_hpp_
