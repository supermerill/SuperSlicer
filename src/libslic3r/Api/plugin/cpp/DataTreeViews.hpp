///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_DataTreeViews_hpp_
#define slic3r_Api_plugin_cpp_DataTreeViews_hpp_

/*
Data-tree C++ views
===================

This header is the C++ view layer for the slicer's data tree. The views are
small, non-owning wrappers around C ABI handles; copying a view copies only a
handle, not the underlying Print, Object, Layer, or geometry. A view is usable
only while its native object still exists. Removing an auxiliary layer,
rebuilding a collection, or clearing the Print can invalidate views and
references obtained from that object.

The main hierarchy is:

    Print
    `-- Object
        `-- Layer
            `-- LayerIsland
                `-- LayerRegionIsland
                    |-- LayerRegion
                    |   `-- PrintRegion
                    `-- fill surfaces and extrusion roots

`LayerRegionIsland` is the region-specific part of one geometric island. It
groups one or more `LayerRegion` objects that share the same effective
extruder for the current operation. `LayerIsland` contains the geometric
slice, infill areas, and links to overlapping islands on the layers above and
below. `Layer` provides Z positions and the collection of islands. `Object`
provides layers, instances, volumes, object configuration, and object-owned
auxiliary layers. `Print` provides the objects, print configuration, records,
and the hidden auxiliary object used for print-level generated geometry.

A typical plugin traversal is:

    Print print(ctx->print);
    for (uint32_t object_idx = 0; object_idx < print.object_count(); ++object_idx) {
        Object object = print.object(object_idx);
        for (uint32_t layer_idx = 0; layer_idx < object.layer_count(); ++layer_idx) {
            Layer layer = object.layer(layer_idx);
            for (uint32_t island_idx = 0; island_idx < layer.island_count(); ++island_idx) {
                LayerIsland island = layer.island(island_idx);
                for (uint32_t group_idx = 0; group_idx < island.region_island_count(); ++group_idx) {
                    LayerRegionIsland group = island.region_island(group_idx);
                    for (LayerRegion region : group.regions()) {
                        Config config = region.print_region().config();
                        // Read the region-specific configuration here.
                    }
                }
            }
        }
    }

Most data-tree views are read-only because the host controls the native
slicing structures. Specific API methods still permit controlled mutation:
for example, `LayerIsland::get_or_create_region_island()` changes the tree and
`MutableSurface` changes a surface's geometry or plugin properties. A const
view therefore does not mean that every operation reachable through it is
immutable; it means that mutation is limited to methods whose API explicitly
provides it.

Surface views follow a separate ownership rule. `Surface`, `SurfaceCollection`,
and the data-tree views borrow host-owned handles. `StoredSurface`,
`StoredExPolygon`, and `StoredSurfaceCollection` own temporary data allocated
from a plugin `storage_handle`; they are move-only and are used to build a
complete replacement before transferring it to the host. Stored data must not
be confused with a persistent data-tree object.

Plugin property payloads are copied as trivially copyable bytes. Built-in keys
are available through types such as `LayerSupportProperty::key`; plugin-defined
keys must be registered for the current orchestrator. Property mutation does
not make native geometry writable.

Developer guides:

    [Using Plugin Properties](/doc/plugins/properties.md)
    [Using Unified Extrusion Entities](/doc/plugins/extrusions.md)
*/

#include <cassert>
#include <cstddef>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_data_tree.h"
#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/cpp/ConfigViews.hpp"
#include "libslic3r/Api/plugin/cpp/GeometryViews.hpp"
#include "libslic3r/Api/plugin/cpp/PluginPropertyKey.hpp"
#include "libslic3r/Api/plugin/cpp/properties/DataTreeProperties.hpp"

namespace slic3r_api {

class Surface;
class SurfaceCollection;
class MutableSurface;
class Volume;
class PrintRegion;
class Object;
class Print;
class LayerRegion;
class LayerRegionIsland;
class LayerIsland;
class Layer;

/*
View over the generic plugin-property container.

Payloads with a stable built-in id expose a static typed key:

    properties.get(LayerSupportProperty::key);

Plugin-private contracts should instead register their namespaced string for
each orchestrator and retain the returned PluginPropertyKey in the plugin
instance. Built-in and dynamic keys then expose the same has(), get(),
get_mutable(), get_or_add() and remove() operations. The payload is copied as
raw bytes by the host. Keep it a plain C-style struct: no std::string, no
std::vector, no owning pointers.

The view is mutable even when it comes from a const Layer, Island or Surface
handle. This mutates only plugin metadata; it does not make the object geometry
or native slicer fields writable.
*/
class PluginProperties
{
public:
    explicit PluginProperties(plugin_property_container_handle *handle = nullptr) : m_handle(handle) {}

    bool valid() const { return m_handle != nullptr; }
    const plugin_property_container_handle *handle() const { return m_handle; }
    plugin_property_container_handle *mutable_handle() const { return m_handle; }
    uint32_t count() const { return plugin_property_count(m_handle); }
    plugin_property_type type_at(uint32_t idx) const { return plugin_property_type_at(m_handle, idx); }
    bool has(plugin_property_type type) const { return plugin_property_has(m_handle, type) != 0; }
    template<class PropertyType> bool has(const PluginPropertyKey<PropertyType> &key) const {
        return has(key.type());
    }
    uint32_t data_size(plugin_property_type type) const { return plugin_property_data_size(m_handle, type); }
    const void *data(plugin_property_type type) const { return plugin_property_data(m_handle, type); }

    template<class PropertyType> const PropertyType *get(plugin_property_type type) const
    {
        static_assert(std::is_trivially_copyable<PropertyType>::value,
                      "Plugin property payloads are copied as bytes and must be trivially copyable.");
        if (data_size(type) != sizeof(PropertyType))
            return nullptr;
        return reinterpret_cast<const PropertyType *>(data(type));
    }

    template<class PropertyType> const PropertyType *get(const PluginPropertyKey<PropertyType> &key) const
    {
        return get<PropertyType>(key.type());
    }

    void clear() { plugin_property_clear(mutable_handle()); }
    void copy_from(const PluginProperties &other) {
        plugin_property_copy_all(mutable_handle(), other.handle());
    }
    bool remove(plugin_property_type type) { return plugin_property_remove(mutable_handle(), type) != 0; }
    template<class PropertyType> bool remove(const PluginPropertyKey<PropertyType> &key) {
        return remove(key.type());
    }

    template<class PropertyType> PropertyType *get(plugin_property_type type)
    {
        static_assert(std::is_trivially_copyable<PropertyType>::value,
                      "Plugin property payloads are copied as bytes and must be trivially copyable.");
        if (data_size(type) != sizeof(PropertyType))
            return nullptr;
        return reinterpret_cast<PropertyType *>(
            plugin_property_data_mutable(mutable_handle(), type));
    }

    template<class PropertyType> PropertyType *get_mutable(const PluginPropertyKey<PropertyType> &key) const
    {
        static_assert(std::is_trivially_copyable<PropertyType>::value,
                      "Plugin property payloads are copied as bytes and must be trivially copyable.");
        if (data_size(key.type()) != sizeof(PropertyType))
            return nullptr;
        return reinterpret_cast<PropertyType *>(
            plugin_property_data_mutable(mutable_handle(), key.type()));
    }

    template<class PropertyType> PropertyType &get_or_add(orchestrator_handle *orchestrator,
                                                           plugin_property_type type)
    {
        static_assert(std::is_trivially_copyable<PropertyType>::value,
                      "Plugin property payloads are copied as bytes and must be trivially copyable.");
        /*
        A new payload is zero-initialized. If the assertion fires, another
        property type is unknown to this orchestrator or an existing payload
        with that numeric id has a different binary layout.
        */
        void *data = plugin_property_get_or_add_data_mutable(orchestrator, mutable_handle(), type);
        assert(data != nullptr);
        return *reinterpret_cast<PropertyType *>(data);
    }

    template<class PropertyType> PropertyType &get_or_add(const PluginPropertyKey<PropertyType> &key) const
    {
        static_assert(std::is_trivially_copyable<PropertyType>::value,
                      "Plugin property payloads are copied as bytes and must be trivially copyable.");
        void *data = plugin_property_get_or_add_data_mutable(
            key.orchestrator(), mutable_handle(), key.type());
        if (data == nullptr)
            throw std::runtime_error("The typed plugin property could not be created.");
        return *reinterpret_cast<PropertyType *>(data);
    }

    void *get_or_add(orchestrator_handle *orchestrator, plugin_property_type type) {
        return plugin_property_get_or_add_data_mutable(orchestrator, mutable_handle(), type);
    }

private:
    plugin_property_container_handle *m_handle = nullptr;
};

/*
C++ convenience builder for raw_surface_type.

It enforces the expected order at compile time:
1. choose exactly one position: top(), bottom(), internal(), perimeter()
2. choose exactly one density: solid(), sparse(), empty()
3. optionally add modifiers: bridge(), overbridge()

Use raw() or operator() to retrieve the final C ABI bitmask.

Example:
    raw_surface_type type = srf_type().top().solid().bridge().raw();
    raw_surface_type same = srf_type().top().solid().bridge()();
*/

class srf_type_modifiers
{
public:
    explicit constexpr srf_type_modifiers(raw_surface_type value) : m_value(value) {}

    constexpr srf_type_modifiers bridge() const
    {
        return srf_type_modifiers(raw_surface_type(m_value | RAW_SURFACE_TYPE_MOD_BRIDGE));
    }

    constexpr srf_type_modifiers overbridge() const
    {
        return srf_type_modifiers(raw_surface_type(m_value | RAW_SURFACE_TYPE_MOD_OVERBRIDGE));
    }

    constexpr raw_surface_type raw() const { return m_value; }
    constexpr raw_surface_type operator()() const { return raw(); }

private:
    raw_surface_type m_value;
};

class srf_type_density
{
public:
    explicit constexpr srf_type_density(raw_surface_type value) : m_value(value) {}

    constexpr srf_type_modifiers solid() const
    {
        return srf_type_modifiers(raw_surface_type(m_value | RAW_SURFACE_TYPE_DENS_SOLID));
    }

    constexpr srf_type_modifiers sparse() const
    {
        return srf_type_modifiers(raw_surface_type(m_value | RAW_SURFACE_TYPE_DENS_SPARSE));
    }

    constexpr srf_type_modifiers empty() const
    {
        return srf_type_modifiers(raw_surface_type(m_value | RAW_SURFACE_TYPE_DENS_VOID));
    }

private:
    raw_surface_type m_value;
};

class srf_type_position
{
public:
    constexpr srf_type_density top() const
    {
        return srf_type_density(RAW_SURFACE_TYPE_POS_TOP);
    }

    constexpr srf_type_density bottom() const
    {
        return srf_type_density(RAW_SURFACE_TYPE_POS_BOTTOM);
    }

    constexpr srf_type_density internal() const
    {
        return srf_type_density(RAW_SURFACE_TYPE_POS_INTERNAL);
    }

    constexpr srf_type_density perimeter() const
    {
        return srf_type_density(RAW_SURFACE_TYPE_POS_PERIMETER);
    }
};

constexpr srf_type_position srf_type() { return srf_type_position(); }

inline constexpr raw_surface_type k_surface_type_position_flags =
    RAW_SURFACE_TYPE_POS_TOP |
    RAW_SURFACE_TYPE_POS_BOTTOM |
    RAW_SURFACE_TYPE_POS_INTERNAL |
    RAW_SURFACE_TYPE_POS_PERIMETER;

inline constexpr raw_surface_type k_surface_type_density_flags =
    RAW_SURFACE_TYPE_DENS_SOLID |
    RAW_SURFACE_TYPE_DENS_SPARSE |
    RAW_SURFACE_TYPE_DENS_VOID;

inline constexpr raw_surface_type k_surface_type_modifier_flags =
    RAW_SURFACE_TYPE_MOD_BRIDGE |
    RAW_SURFACE_TYPE_MOD_OVERBRIDGE;

// raw_surface_type is a bitmask. These helpers keep plugins readable and avoid
// repeating ad-hoc `(type & flag)` expressions with slightly different names.
constexpr bool surface_type_has_flag(raw_surface_type type, raw_surface_type flag)
{
    return (type & flag) != 0;
}

constexpr bool surface_type_has_any_flag(raw_surface_type type, raw_surface_type flags)
{
    return (type & flags) != 0;
}

constexpr bool surface_type_has_all_flags(raw_surface_type type, raw_surface_type flags)
{
    return (type & flags) == flags;
}

constexpr raw_surface_type surface_type_add_flags(raw_surface_type type, raw_surface_type flags)
{
    return raw_surface_type(type | flags);
}

constexpr raw_surface_type surface_type_remove_flags(raw_surface_type type, raw_surface_type flags)
{
    return raw_surface_type(type & ~flags);
}

constexpr raw_surface_type surface_type_set_flags(raw_surface_type type, raw_surface_type flags, bool enabled)
{
    return enabled ? surface_type_add_flags(type, flags) : surface_type_remove_flags(type, flags);
}

constexpr raw_surface_type surface_type_replace_flags(raw_surface_type type,
                                                      raw_surface_type mask,
                                                      raw_surface_type replacement)
{
    return raw_surface_type((type & ~mask) | (replacement & mask));
}

constexpr raw_surface_type surface_type_position(raw_surface_type type)
{
    return raw_surface_type(type & k_surface_type_position_flags);
}

constexpr raw_surface_type surface_type_density(raw_surface_type type)
{
    return raw_surface_type(type & k_surface_type_density_flags);
}

constexpr raw_surface_type surface_type_modifiers(raw_surface_type type)
{
    return raw_surface_type(type & k_surface_type_modifier_flags);
}

constexpr bool surface_type_is_top(raw_surface_type type)
{
    return surface_type_has_flag(type, RAW_SURFACE_TYPE_POS_TOP);
}

constexpr bool surface_type_is_bottom(raw_surface_type type)
{
    return surface_type_has_flag(type, RAW_SURFACE_TYPE_POS_BOTTOM);
}

constexpr bool surface_type_is_internal(raw_surface_type type)
{
    return surface_type_has_flag(type, RAW_SURFACE_TYPE_POS_INTERNAL);
}

constexpr bool surface_type_is_perimeter(raw_surface_type type)
{
    return surface_type_has_flag(type, RAW_SURFACE_TYPE_POS_PERIMETER);
}

constexpr bool surface_type_is_solid(raw_surface_type type)
{
    return surface_type_has_flag(type, RAW_SURFACE_TYPE_DENS_SOLID);
}

constexpr bool surface_type_is_sparse(raw_surface_type type)
{
    return surface_type_has_flag(type, RAW_SURFACE_TYPE_DENS_SPARSE);
}

constexpr bool surface_type_is_void(raw_surface_type type)
{
    return surface_type_has_flag(type, RAW_SURFACE_TYPE_DENS_VOID);
}

constexpr bool surface_type_is_bridge(raw_surface_type type)
{
    return surface_type_has_flag(type, RAW_SURFACE_TYPE_MOD_BRIDGE);
}

constexpr bool surface_type_is_overbridge(raw_surface_type type)
{
    return surface_type_has_flag(type, RAW_SURFACE_TYPE_MOD_OVERBRIDGE);
}

class Surface
{
public:
    explicit Surface(const surface_handle *handle) : m_handle(handle) { assert(handle != nullptr); }
    Surface(c_surface surface) : surface(surface) {}
    Surface(ExPolygon expoly, raw_surface_type type) : surface({expoly.handle(), type, 0}) {}

    const surface_handle *handle() const {
        assert(m_handle != nullptr);
        return m_handle;
    }

    bool has_handle() const { return m_handle != nullptr; }

    ExPolygon expolygon() const {
        return ExPolygon(m_handle != nullptr ? surface_get_expolygon(m_handle) : surface.expolygon);
    }

    raw_surface_type type() const {
        return m_handle != nullptr ? surface_get_type(m_handle) : surface.type;
    }

    uint64_t id() const {
        return m_handle != nullptr ? surface_get_id(m_handle) : surface.id;
    }

    bool has_flag(raw_surface_type flag) const {
        return m_handle != nullptr ? surface_get_flag(m_handle, flag) != 0 : surface_type_has_flag(surface.type, flag);
    }

    bool has_any_flag(raw_surface_type flags) const {
        return surface_type_has_any_flag(type(), flags);
    }

    bool has_all_flags(raw_surface_type flags) const {
        return surface_type_has_all_flags(type(), flags);
    }

    c_surface c_view() const {
        return m_handle != nullptr ? surface_c_view(m_handle) : surface;
    }

    PluginProperties properties() const {
        return PluginProperties(m_handle != nullptr ? surface_get_properties(m_handle) : nullptr);
    }

    template<class PropertyType>
    const PropertyType *get(const PluginPropertyKey<PropertyType> &key) const {
        return properties().get(key);
    }

private:
    const surface_handle *m_handle = nullptr;
    c_surface surface = {};
};

// Mutable borrowed view over one host-owned surface. It can change the surface
// geometry or plugin properties only through the operations exposed below; it
// never takes ownership of the native surface handle.
class MutableSurface
{
public:
    explicit MutableSurface(surface_handle *handle) : m_handle(handle) { assert(handle != nullptr); }

    surface_handle *mutable_handle() const {
        assert(m_handle != nullptr);
        return m_handle;
    }

    const surface_handle *handle() const { return mutable_handle(); }
    Surface readonly() const { return Surface(handle()); }
    operator Surface() const { return readonly(); }

    ExPolygon expolygon() const { return readonly().expolygon(); }
    raw_surface_type type() const { return readonly().type(); }
    bool has_flag(raw_surface_type flag) const { return readonly().has_flag(flag); }
    PluginProperties properties() const { return readonly().properties(); }
    PluginProperties mutable_properties() const {
        return PluginProperties(surface_get_properties(mutable_handle()));
    }

    void copy_properties_from(const Surface &other) {
        mutable_properties().copy_from(other.properties());
    }

    template<class PropertyType>
    const PropertyType *get(const PluginPropertyKey<PropertyType> &key) const {
        return properties().get(key);
    }

    template<class PropertyType>
    PropertyType *get_mutable(const PluginPropertyKey<PropertyType> &key) const {
        return mutable_properties().get_mutable(key);
    }

    template<class PropertyType>
    PropertyType &get_or_add(const PluginPropertyKey<PropertyType> &key) const {
        return mutable_properties().get_or_add(key);
    }

    template<class PropertyType>
    bool remove(const PluginPropertyKey<PropertyType> &key) const {
        return mutable_properties().remove(key);
    }

private:
    surface_handle *m_handle = nullptr;
};

class SurfaceCollection : public ConstDataTreeHandleView<surface_collection_handle>
{
public:
    using ConstDataTreeHandleView<surface_collection_handle>::ConstDataTreeHandleView;

    class iterator
    {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = Surface;
        using difference_type = std::ptrdiff_t;

        iterator() = default;
        iterator(const SurfaceCollection *owner, uint32_t index) : m_owner(owner), m_index(index) {}

        value_type operator*() const { return m_owner->at(m_index); }

        iterator &operator++() {
            ++m_index;
            return *this;
        }

        iterator operator++(int) {
            iterator copy = *this;
            ++(*this);
            return copy;
        }

        bool operator==(const iterator &other) const {
            return m_owner == other.m_owner && m_index == other.m_index;
        }

        bool operator!=(const iterator &other) const { return !(*this == other); }

    private:
        const SurfaceCollection *m_owner = nullptr;
        uint32_t m_index = 0;
    };

    uint32_t size() const { return surface_collection_size(handle()); }
    bool empty() const { return size() == 0; }
    Surface at(uint32_t idx) const { assert(idx < size()); return Surface(surface_collection_at(handle(), idx)); }
    Surface operator[](uint32_t idx) const { return at(idx); }
    Surface front() const { assert(!empty()); return at(0); }
    Surface back() const { assert(!empty()); return at(size() - 1); }
    iterator begin() const { return iterator(this, 0); }
    iterator end() const { return iterator(this, size()); }
};

// Storage-owned surface collection used by steps that publish a complete
// replacement through a callback. The collection is temporary plugin storage;
// the host may move its content into the data tree, so it must remain valid
// until the callback consumes it.
class StoredSurfaceCollection
{
public:
    explicit StoredSurfaceCollection(storage_handle *storage) :
        m_storage(storage), m_handle(storage_new_surface_collection(storage)) {
        assert(m_storage != nullptr);
        assert(m_handle != nullptr);
    }

    StoredSurfaceCollection(const StoredSurfaceCollection &) = delete;
    StoredSurfaceCollection &operator=(const StoredSurfaceCollection &) = delete;

    StoredSurfaceCollection(StoredSurfaceCollection &&other) noexcept :
        m_storage(other.m_storage), m_handle(other.m_handle) {
        other.m_storage = nullptr;
        other.m_handle = nullptr;
    }

    StoredSurfaceCollection &operator=(StoredSurfaceCollection &&other) noexcept {
        if (this == &other)
            return *this;
        reset();
        m_storage = other.m_storage;
        m_handle = other.m_handle;
        other.m_storage = nullptr;
        other.m_handle = nullptr;
        return *this;
    }

    ~StoredSurfaceCollection() { reset(); }

    surface_collection_handle *mutable_handle() const {
        assert(m_handle != nullptr);
        return m_handle;
    }

    const surface_collection_handle *handle() const {
        assert(m_handle != nullptr);
        return m_handle;
    }

    storage_handle *storage() const {
        assert(m_storage != nullptr);
        return m_storage;
    }

    operator SurfaceCollection() const & { return readonly(); }
    operator SurfaceCollection() && = delete;

    SurfaceCollection readonly() const & { return SurfaceCollection(handle()); }
    SurfaceCollection readonly() && = delete;

    uint32_t size() const { return surface_collection_size(handle()); }
    bool empty() const { return size() == 0; }
    void clear() { surface_collection_clear(mutable_handle()); }
    MutableSurface mutable_at(uint32_t idx) const {
        assert(idx < size());
        return MutableSurface(surface_collection_at_mutable(mutable_handle(), idx));
    }
    MutableSurface back_mutable() const {
        assert(!empty());
        return mutable_at(size() - 1);
    }

    // Copy borrowed geometry into new Surface objects with the requested type.
    // Use this for views returned by the host data tree or by Clipper outputs
    // that are still needed afterwards.
    void append(const ExPolygonCollection &areas, raw_surface_type surface_type) {
        if (!areas.empty())
            surface_collection_append_expolygons_copy(mutable_handle(), areas.handle(), surface_type);
    }

    // Move storage-owned geometry into Surface objects. This avoids copying
    // temporary areas produced only to build this SurfaceCollection.
    void append_move(StoredExPolygonCollection &&areas, raw_surface_type surface_type) {
        if (!areas.empty())
            surface_collection_append_expolygons_move(mutable_handle(), areas.mutable_handle(), surface_type);
    }

    void append(const ExPolygon &area, raw_surface_type surface_type) {
        surface_collection_append_expolygon_copy(mutable_handle(), area.handle(), surface_type);
    }

    // Single-area move variant for algorithms that naturally produce one
    // StoredExPolygon at a time.
    void append_move(StoredExPolygon &&area, raw_surface_type surface_type) {
        surface_collection_append_expolygon_move(mutable_handle(), area.mutable_handle(), surface_type);
    }

    /*
    Append geometry while preserving the metadata of an existing Surface.

    This is the common operation when a plugin clips or splits a surface: the
    new pieces have new geometry, but they still represent the same logical
    surface for later plugins, so their properties must be copied too.
    */
    void append_like(const ExPolygonCollection &areas, const Surface &surface_template) {
        const uint32_t first_new_idx = size();
        append(areas, surface_template.type());
        copy_properties_to_range(first_new_idx, surface_template);
    }

    void append_like_move(StoredExPolygonCollection &&areas, const Surface &surface_template) {
        const uint32_t first_new_idx = size();
        append_move(std::move(areas), surface_template.type());
        copy_properties_to_range(first_new_idx, surface_template);
    }

    void append_like(const ExPolygon &area, const Surface &surface_template) {
        const uint32_t first_new_idx = size();
        append(area, surface_template.type());
        mutable_at(first_new_idx).copy_properties_from(surface_template);
    }

    void append_like_move(StoredExPolygon &&area, const Surface &surface_template) {
        const uint32_t first_new_idx = size();
        append_move(std::move(area), surface_template.type());
        mutable_at(first_new_idx).copy_properties_from(surface_template);
    }

    bool free_from_storage() { return reset(); }

private:
    void copy_properties_to_range(uint32_t first_new_idx, const Surface &surface_template) {
        for (uint32_t idx = first_new_idx; idx < size(); ++idx)
            mutable_at(idx).copy_properties_from(surface_template);
    }

    bool reset() {
        if (m_storage == nullptr || m_handle == nullptr)
            return false;
        const bool freed = storage_free(m_storage, m_handle) != 0;
        assert(freed);
        m_storage = nullptr;
        m_handle = nullptr;
        return freed;
    }

    storage_handle *m_storage = nullptr;
    surface_collection_handle *m_handle = nullptr;
};

// as Surface, but it has the ownership of the expolygon.
class StoredSurface
{
public:
    StoredSurface(StoredExPolygon expolygon, raw_surface_type type)
        : m_expolygon(std::move(expolygon)), m_type(type) {}

    operator Surface() const & { return readonly(); }
    operator Surface() && = delete;

    Surface readonly() const & { return Surface(m_expolygon.readonly(), m_type); }
    Surface readonly() && = delete;

    ExPolygon expolygon() const { return m_expolygon.readonly(); }

    raw_surface_type type() const { return m_type; }

    c_surface c_view() const { return c_surface{m_expolygon.handle(), m_type, 0}; }

    StoredExPolygon &expolygon_mutable() { return m_expolygon; }
private:
    StoredExPolygon m_expolygon;
    raw_surface_type m_type;
};

// Configuration shared by LayerRegion objects that belong to the same print
// region. It is a link to print-wide region settings, not a layer geometry
// container.
class PrintRegion : public ConstDataTreeHandleView<print_region_handle>
{
public:
    using ConstDataTreeHandleView<print_region_handle>::ConstDataTreeHandleView;

    Config config() const {
        return Config(print_region_get_config(handle()));
    }

    c_flow flow(const LayerRegion &layer_region, raw_extrusion_role role) const;
};

// Layer-specific region data: its slices, flows, plugin properties, and link
// back to the corresponding print-wide PrintRegion configuration.
class LayerRegion : public ConstDataTreeHandleView<layer_region_handle>
{
public:
    using ConstDataTreeHandleView<layer_region_handle>::ConstDataTreeHandleView;

    PluginProperties properties() const {
        return PluginProperties(layer_region_get_properties(handle()));
    }

    c_flow flow(raw_extrusion_role role) const { return layer_region_get_flow(handle(), role); }
    c_flow bridging_flow(raw_extrusion_role role) const { return layer_region_get_bridging_flow(handle(), role); }

    ExPolygonCollection slices() const { return ExPolygonCollection(layer_region_get_slices(handle())); }

    c_bounding_box bounding_box() const { return layer_region_get_bounding_box(handle()); }

    PrintRegion print_region() const {
        return PrintRegion(layer_region_get_print_region(handle()));
    }

    Layer layer() const;
};

// Region-specific data attached to one LayerIsland. It owns the association
// with one or more LayerRegion objects and stores the fill surfaces and
// extrusion roots generated for that association.
class LayerRegionIsland : public ConstDataTreeHandleView<layer_region_island_handle>
{
public:
    using ConstDataTreeHandleView<layer_region_island_handle>::ConstDataTreeHandleView;

    int32_t extruder_id() const { return layer_region_island_extruder_id(handle()); }
    bool has_extrusions() const { return layer_region_island_has_extrusions(handle()) != 0; }
    bool has_extrusion(raw_extrusion_role role) const { return layer_region_island_has_extrusion(handle(), role) != 0; }

    const extrusion_entity_handle *extrusion(raw_extrusion_role role) const {
        return layer_region_island_get_extrusion(handle(), role);
    }

    PluginProperties properties() const {
        return PluginProperties(layer_region_island_get_properties(handle()));
    }
    uint32_t region_count() const { return layer_region_island_count_region(handle()); }

    // Regions owned by this LayerRegionIsland. Surface-generation plugins use
    // this when they refine an existing group instead of starting again from
    // the whole LayerIsland.
    LayerRegion region(uint32_t idx) const {
        return LayerRegion(layer_region_island_get_region(handle(), idx));
    }

    std::vector<LayerRegion> regions() const {
        std::vector<LayerRegion> result;
        const uint32_t count = region_count();
        result.reserve(count);
        for (uint32_t idx = 0; idx < count; ++idx) {
            const layer_region_handle *region = layer_region_island_get_region(handle(), idx);
            if (region != nullptr)
                result.emplace_back(region);
        }
        return result;
    }

    SurfaceCollection fill_surfaces_collection() const {
        return SurfaceCollection(layer_region_island_get_fill_surfaces(handle()));
    }

    uint32_t fill_surface_count() const { return fill_surfaces_collection().size(); }

    Surface fill_surface(uint32_t idx) const {
        return Surface(layer_region_island_get_fill_surface(handle(), idx));
    }

    std::vector<Surface> fill_surfaces() const {
        std::vector<Surface> result;
        const uint32_t count = fill_surface_count();
        result.reserve(count);
        for (uint32_t i = 0; i < count; ++i)
            result.emplace_back(layer_region_island_get_fill_surface(handle(), i));
        return result;
    }

};

// One connected geometric island on a Layer. It exposes the sliced shape,
// infill areas, region-island groups, and overlap links to neighboring layers.
class LayerIsland : public ConstDataTreeHandleView<layer_island_handle>
{
public:
    using ConstDataTreeHandleView<layer_island_handle>::ConstDataTreeHandleView;

    ExPolygon slice() const { return ExPolygon(layer_island_get_slice(handle())); }
    c_bounding_box bounding_box() const { return layer_island_get_bounding_box(handle()); }
    ExPolygon infill_slice() const { return ExPolygon(layer_island_get_infill_slice(handle())); }
    ExPolygonCollection infill_areas() const { return ExPolygonCollection(layer_island_get_infill_areas(handle())); }
    c_bounding_box infill_bounding_box() const { return layer_island_get_infill_bounding_box(handle()); }
    ExPolygon infill_no_overlap_slice() const { return ExPolygon(layer_island_get_infill_no_overlap_slice(handle())); }
    ExPolygonCollection infill_no_overlap_areas() const {
        return ExPolygonCollection(layer_island_get_infill_no_overlap_areas(handle()));
    }

    PluginProperties properties() const {
        return PluginProperties(layer_island_get_properties(handle()));
    }
    uint32_t region_count() const { return layer_island_count_region(handle()); }

    LayerRegion region(uint32_t idx) const {
        return LayerRegion(layer_island_get_region(handle(), idx));
    }

    uint32_t region_island_count() const { return layer_island_count_region_island(handle()); }

    LayerRegionIsland region_island(uint32_t idx) const {
        return LayerRegionIsland(layer_island_get_region_island(handle(), idx));
    }

    LayerRegionIsland get_or_create_region_island(const std::vector<LayerRegion> &regions,
                                                  int32_t extruder_id = -1) const {
        std::vector<const layer_region_handle *> handles;
        handles.reserve(regions.size());
        for (const LayerRegion &region : regions)
            handles.push_back(region.handle());

        // The C API intentionally receives the extruder explicitly. Helpers
        // that decide "which extruder for which role" should run before this
        // call, so this method remains a plain data-tree mutation.
        layer_region_island_handle *region_island =
            layer_island_get_or_create_region_island(
                const_cast<layer_island_handle *>(handle()),
                handles.empty() ? nullptr : handles.data(),
                uint32_t(handles.size()),
                extruder_id);
        return LayerRegionIsland(region_island);
    }

    LayerRegionIsland get_or_create_full_region_island(int32_t extruder_id = -1) const {
        layer_region_island_handle *region_island =
            layer_island_get_or_create_region_island(
                const_cast<layer_island_handle *>(handle()),
                nullptr,
                0,
                extruder_id);
        return LayerRegionIsland(region_island);
    }

    Layer layer() const;

    uint32_t lower_island_count() const { return layer_island_count_lower_island(handle()); }

    LayerIsland lower_island(uint32_t idx) const {
        return LayerIsland(layer_island_get_lower_island(handle(), idx));
    }

    std::vector<LayerIsland> lower_islands() const {
        std::vector<LayerIsland> result;
        const uint32_t count = lower_island_count();
        result.reserve(count);
        for (uint32_t idx = 0; idx < count; ++idx) {
            const layer_island_handle *island = layer_island_get_lower_island(handle(), idx);
            if (island != nullptr)
                result.emplace_back(island);
        }
        return result;
    }

    uint32_t upper_island_count() const { return layer_island_count_upper_island(handle()); }

    LayerIsland upper_island(uint32_t idx) const {
        return LayerIsland(layer_island_get_upper_island(handle(), idx));
    }

    std::vector<LayerIsland> upper_islands() const {
        std::vector<LayerIsland> result;
        const uint32_t count = upper_island_count();
        result.reserve(count);
        for (uint32_t idx = 0; idx < count; ++idx) {
            const layer_island_handle *island = layer_island_get_upper_island(handle(), idx);
            if (island != nullptr)
                result.emplace_back(island);
        }
        return result;
    }
};

// One printable Z level of an Object. Layer indices are object-local and the
// three Z values distinguish physical layer height, print position, and the
// slicing position used to obtain polygons.
class Layer : public ConstDataTreeHandleView<layer_handle>
{
public:
    using ConstDataTreeHandleView<layer_handle>::ConstDataTreeHandleView;

    coord_t height() const { return layer_get_height(handle()); }
    coord_t print_z() const { return layer_get_print_z(handle()); }
    coord_t slice_z() const { return layer_get_slice_z(handle()); }
    coord_t bottom_z() const { return print_z() - height(); }
    Object object() const;

    Layer upper_layer() const {
        return Layer(layer_get_upper_layer(handle()));
    }

    Layer lower_layer() const {
        return Layer(layer_get_lower_layer(handle()));
    }

    PluginProperties properties() const {
        return PluginProperties(layer_get_properties(handle()));
    }

    uint32_t region_count() const { return layer_count_region(handle()); }
    uint32_t island_count() const { return layer_count_island(handle()); }
    uint32_t curled_line_count() const { return layer_count_curled_line(handle()); }

    ExPolygonCollection slices() const { return ExPolygonCollection(layer_get_slices(handle())); }

    LayerRegion region(uint32_t idx) const {
        return LayerRegion(layer_get_region(handle(), idx));
    }

    LayerIsland island(uint32_t idx) const {
        return LayerIsland(layer_get_island(handle(), idx));
    }

    std::vector<c_curled_line> curled_lines() const {
        std::vector<c_curled_line> result;
        const uint32_t count = curled_line_count();
        result.reserve(count);
        for (uint32_t idx = 0; idx < count; ++idx)
            result.push_back(layer_get_curled_line(handle(), idx));
        return result;
    }
};

// One model object with its transform, instances, configuration, sliced layers,
// volumes, and object-owned auxiliary layers. Print-level auxiliary geometry is
// instead reached through Print::auxiliary_object().
class Object : public ConstDataTreeHandleView<object_handle>
{
public:
    using ConstDataTreeHandleView<object_handle>::ConstDataTreeHandleView;

    Config config() const {
        return Config(object_get_config(handle()));
    }

    PluginProperties properties() const {
        return PluginProperties(object_get_properties(handle()));
    }

    coord_t max_z() const { return object_get_max_z(handle()); }
    c_matrix4d transform() const { return object_get_transform(handle()); }
    c_point center_offset() const { return object_get_center_offset(handle()); }

    c_matrix4d transform_centered() const {
        const c_point offset = center_offset();
        return matrix4d_mul(matrix4d_translation(-unscaled(offset.x), -unscaled(offset.y), 0.0), transform());
    }

    uint32_t instance_count() const { return object_count_instance(handle()); }
    c_point instance_shift(uint32_t idx) const { return object_get_instance_shift(handle(), idx); }

    uint32_t layer_count() const { return object_count_layer(handle()); }
    Layer layer(uint32_t idx) const {
        return Layer(object_get_layer(handle(), idx));
    }

    uint32_t auxiliary_layer_count() const { return object_count_auxiliary_layer(handle()); }
    Layer auxiliary_layer(uint32_t idx) const {
        return Layer(object_get_auxiliary_layer(handle(), idx));
    }
    Layer add_auxiliary_layer(coord_t height, coord_t print_z, coord_t slice_z) const {
        return Layer(object_add_auxiliary_layer(handle(), height, print_z, slice_z));
    }
    bool remove_auxiliary_layer(const Layer &layer) const {
        return object_remove_auxiliary_layer(handle(), const_cast<layer_handle *>(layer.handle())) != 0;
    }

    uint32_t print_region_count() const { return object_count_region(handle()); }
    PrintRegion print_region(uint32_t idx) const {
        return PrintRegion(object_get_print_region(handle(), idx));
    }

    uint32_t volume_count() const;
    Volume volume(uint32_t idx) const;
};

/*
Borrowed view over the variable record channels owned by one Print.

The view may create or remove channels even though its Print handle is const,
because records are auxiliary pipeline metadata. These mutations must happen in
a sequential callback. Config views returned by find() and get_or_add() are
invalidated when their channel is removed or the Print is cleared.
*/
class PrintRecords
{
public:
    explicit PrintRecords(const print_handle *handle = nullptr) : m_print(handle) {}

    std::vector<std::string> channels() const {
        std::vector<std::string> out;
        const_strings_t names = print_records_channels(m_print);
        out.reserve(names.size);
        for (uint32_t idx = 0; idx < names.size; ++idx)
            if (names.items[idx] != nullptr)
                out.emplace_back(names.items[idx]);
        return out;
    }

    std::optional<Config> find(const std::string &channel) const {
        const config_handle *config = print_records_get(m_print, channel.c_str());
        return config == nullptr ? std::nullopt : std::optional<Config>(Config(config));
    }

    MutableConfig get_or_add(const std::string &channel) const {
        config_handle *config = print_records_get_or_add(m_print, channel.c_str());
        if (config == nullptr)
            throw std::runtime_error("The Print record channel could not be created.");
        return MutableConfig(config);
    }

    bool remove(const std::string &channel) const {
        return print_records_remove(m_print, channel.c_str()) != 0;
    }

    print_record_id allocate_id() const {
        const print_record_id id = print_records_allocate_id(m_print);
        if (id == PRINT_RECORD_ID_INVALID)
            throw std::runtime_error("The Print record identifier space is exhausted.");
        return id;
    }

private:
    const print_handle *m_print = nullptr;
};

class Print : public ConstDataTreeHandleView<print_handle>
{
public:
    using ConstDataTreeHandleView<print_handle>::ConstDataTreeHandleView;

    Config config() const {
        return Config(print_get_config(handle()));
    }

    PrintRecords records() const {
        return PrintRecords(handle());
    }

    uint32_t object_count() const { return print_count_object(handle()); }
    Object object(uint32_t idx) const {
        return Object(print_get_object(handle(), idx));
    }

    /*
    Hidden Object that owns print-level auxiliary layers. It is not part of
    object_count()/object(), so use it only for global generated geometry such
    as print-level skirt, brim or wipe-tower layers.
    */
    Object auxiliary_object() const {
        return Object(print_get_auxiliary_object(handle()));
    }
};

inline Layer LayerRegion::layer() const
{
    return Layer(layer_region_get_layer(handle()));
}

inline Layer LayerIsland::layer() const
{
    return Layer(layer_island_get_layer(handle()));
}

inline Object Layer::object() const
{
    return Object(layer_get_object(handle()));
}

} // namespace slic3r_api


#endif // slic3r_Api_plugin_cpp_DataTreeViews_hpp_
