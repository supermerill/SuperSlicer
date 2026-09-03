///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_SurfaceViews_hpp_
#define slic3r_Api_plugin_cpp_SurfaceViews_hpp_

/*
Surface C++ views
=================

This header contains the C++ views and helpers for fill surfaces. Surface and
SurfaceCollection are borrowed views over host-owned geometry. MutableSurface
can modify host-owned surface geometry and plugin properties through the
operations exposed by the API. StoredSurface and StoredSurfaceCollection own
temporary geometry in plugin storage and are used to publish replacement
collections.

Surface types are bitmasks composed of one position, one density and optional
modifier flags. The srf_type() builder and surface_type_* helpers keep that
contract explicit.

LayerRegionIsland is defined by DataTreeViews.hpp because it belongs to the
data-tree hierarchy. Its surface accessors are declared there and defined here
so DataTreeViews.hpp does not need the complete surface implementation.
*/

#include <cassert>
#include <cstddef>
#include <iterator>
#include <utility>
#include <vector>

#include "libslic3r/Api/plugin/cpp/DataTreeViews.hpp"

namespace slic3r_api {

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
inline SurfaceCollection LayerRegionIsland::fill_surfaces_collection() const
{
    return SurfaceCollection(layer_region_island_get_fill_surfaces(handle()));
}

inline uint32_t LayerRegionIsland::fill_surface_count() const
{
    return fill_surfaces_collection().size();
}

inline Surface LayerRegionIsland::fill_surface(uint32_t idx) const
{
    return Surface(layer_region_island_get_fill_surface(handle(), idx));
}

inline std::vector<Surface> LayerRegionIsland::fill_surfaces() const
{
    std::vector<Surface> result;
    const uint32_t count = fill_surface_count();
    result.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
        result.emplace_back(layer_region_island_get_fill_surface(handle(), i));
    return result;
}

} // namespace slic3r_api

#endif // slic3r_Api_plugin_cpp_SurfaceViews_hpp_