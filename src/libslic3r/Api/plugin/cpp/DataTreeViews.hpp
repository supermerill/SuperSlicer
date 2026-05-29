///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_DataTreeViews_hpp_
#define slic3r_Api_plugin_cpp_DataTreeViews_hpp_

#include <cassert>
#include <cstddef>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_config_option.h"
#include "libslic3r/Api/plugin/c/slic3r_data_tree.h"
#include "libslic3r/Api/plugin/cpp/GeometryViews.hpp"

namespace slic3r_api {

class ConfigOption;
class Config;
class Surface;
class SurfaceCollection;
class Volume;
class PrintRegion;
class Object;
class Print;
class LayerRegion;
class LayerRegionIsland;
class LayerIsland;
class Layer;

/* ========================= generic read-only / stored views ========================= */

/*
ConstDataTreeHandleView is the mixed read/write base used by data-tree and Clipper views.
It stores exactly one handle plus a mutability flag: the object is either a
borrowed read-only view or a borrowed mutable view, never two pointers at once.
*/
template<class Handle> class ConstDataTreeHandleView
{
public:
    ConstDataTreeHandleView() = default;
    explicit ConstDataTreeHandleView(const Handle *handle) : m_handle(handle) { assert(handle != nullptr); }

    const Handle *handle() const {
        assert(m_handle != nullptr);
        return m_handle;
    }

    bool same_handle(const ConstDataTreeHandleView &other) const { return m_handle == other.m_handle; }

protected:
    const Handle *m_handle = nullptr;
};

/* ========================= config views ========================= */
/*
Views over config handles and individual config options.
*/

class ConfigOption : public ConstDataTreeHandleView<config_option_handle>
{
public:
    using ConstDataTreeHandleView<config_option_handle>::ConstDataTreeHandleView;

    config_option_type type() const { return config_option_type_get(handle()); }
    uint32_t size() const { return config_option_size(handle()); }
    bool get_bool(uint32_t idx = 0) const { return config_option_get_bool(handle(), idx) != 0; }
    int32_t get_int(uint32_t idx = 0) const { return config_option_get_int(handle(), idx); }
    double get_float(uint32_t idx = 0) const { return config_option_get_float(handle(), idx); }
    c_float_or_percent get_float_or_percent(uint32_t idx = 0) const {
        return config_option_get_float_or_percent(handle(), idx);
    }
    bool is_percent(uint32_t idx = 0) const {
        return config_option_get_float_or_percent(handle(), idx).percent != 0;
    }
    double get_effective_value(double ratio, uint32_t idx = 0) const {
        c_float_or_percent value = config_option_get_float_or_percent(handle(), idx);
        return c_float_or_percent_get_effective_value(&value, ratio);
    }
    bool is_enabled(uint32_t idx = 0) const { return config_option_is_enabled(handle(), idx) != 0; }
    bool is_vector() const { return config_option_is_vector(handle()) != 0; }

    // Serialized values are the stable comparison form used by config diffs
    // and by plugin selectors. They are useful when the C++ enum type is not
    // available on the plugin side.
    std::string serialize() const {
        const uint32_t needed = config_option_serialize(handle(), nullptr, 0);
        std::string out(needed + 1, '\0');
        if (needed > 0)
            config_option_serialize(handle(), &out[0], needed + 1);
        out.resize(needed);
        return out;
    }
};

class Config : public ConstDataTreeHandleView<config_handle>
{
public:
    using ConstDataTreeHandleView<config_handle>::ConstDataTreeHandleView;

    std::vector<std::string> keys() const {
        std::vector<std::string> out;
        const_strings_t c_keys = config_keys(handle());
        out.reserve(c_keys.size);
        for (uint32_t idx = 0; idx < c_keys.size; ++idx) {
            if (c_keys.items[idx] != nullptr)
                out.emplace_back(c_keys.items[idx]);
        }
        return out;
    }

    // Test whether an optional setting exists before reading it. Some plugin
    // steps can be reused in contexts where a dynamic option or selector was
    // not generated because only one implementation is active.
    bool has(const char *key) const {
        return config_get(handle(), key) != nullptr;
    }

    ConfigOption get(const char *key) const {
        return ConfigOption(config_get(handle(), key));
    }
};

/* ========================= surface views ========================= */

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

class Surface
{
public:
    explicit Surface(const surface_handle *handle) : m_handle(handle) { assert(handle != nullptr); }
    Surface(c_surface surface) : surface(surface) {}
    Surface(ExPolygon expoly, raw_surface_type type) : surface({expoly.handle(), type}) {}

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

    bool has_flag(raw_surface_type flag) const {
        return m_handle != nullptr ? surface_get_flag(m_handle, flag) != 0 : (surface.type & flag) != 0;
    }

    c_surface c_view() const {
        return m_handle != nullptr ? surface_c_view(m_handle) : surface;
    }

private:
    const surface_handle *m_handle = nullptr;
    c_surface surface = {};
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

// Storage-owned SurfaceCollection used by steps that publish full surface
// results through a callback. The collection itself is temporary plugin
// storage; the host may move its content into the data tree.
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

    bool free_from_storage() { return reset(); }

private:
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

    c_surface c_view() const { return c_surface{m_expolygon.handle(), m_type}; }

    StoredExPolygon &expolygon_mutable() { return m_expolygon; }
private:
    StoredExPolygon m_expolygon;
    raw_surface_type m_type;
};

/* ========================= data tree views ========================= */
/*
Views over the print / object / layer / region hierarchy exposed by the C ABI.
*/

class PrintRegion : public ConstDataTreeHandleView<print_region_handle>
{
public:
    using ConstDataTreeHandleView<print_region_handle>::ConstDataTreeHandleView;

    Config config() const {
        return Config(print_region_get_config(handle()));
    }

    c_flow flow(const LayerRegion &layer_region, raw_extrusion_role role) const;
};

class LayerRegion : public ConstDataTreeHandleView<layer_region_handle>
{
public:
    using ConstDataTreeHandleView<layer_region_handle>::ConstDataTreeHandleView;

    double get_tag(const char *tag) const { return layer_region_get_tag(handle(), tag); }

    c_flow flow(raw_extrusion_role role) const { return layer_region_get_flow(handle(), role); }

    ExPolygonCollection slices() const { return ExPolygonCollection(layer_region_get_slices(handle())); }

    c_bounding_box bounding_box() const { return layer_region_get_bounding_box(handle()); }

    PrintRegion print_region() const {
        return PrintRegion(layer_region_get_print_region(handle()));
    }

    Layer layer() const;
};

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

    double get_tag(const char *tag) const { return layer_region_island_get_tag(handle(), tag); }
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

    double get_tag(const char *tag) const { return layer_island_get_tag(handle(), tag); }
    uint32_t region_count() const { return layer_island_count_region(handle()); }

    LayerRegion region(uint32_t idx) const {
        return LayerRegion(layer_island_get_region(handle(), idx));
    }

    uint32_t region_island_count() const { return layer_island_count_region_island(handle()); }

    LayerRegionIsland region_island(uint32_t idx) const {
        return LayerRegionIsland(layer_island_get_region_island(handle(), idx));
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

class Layer : public ConstDataTreeHandleView<layer_handle>
{
public:
    using ConstDataTreeHandleView<layer_handle>::ConstDataTreeHandleView;

    coord_t height() const { return layer_get_height(handle()); }
    coord_t print_z() const { return layer_get_print_z(handle()); }
    coord_t slice_z() const { return layer_get_slice_z(handle()); }
    coord_t bottom_z() const { return print_z() - height(); }
    coord_t support_id() const { return layer_get_support_id(handle()); }

    Layer upper_layer() const {
        return Layer(layer_get_upper_layer(handle()));
    }

    Layer lower_layer() const {
        return Layer(layer_get_lower_layer(handle()));
    }

    double get_tag(const char *tag) const { return layer_get_tag(handle(), tag); }

    uint32_t region_count() const { return layer_count_region(handle()); }
    uint32_t island_count() const { return layer_count_island(handle()); }

    ExPolygonCollection slices() const { return ExPolygonCollection(layer_get_slices(handle())); }

    LayerRegion region(uint32_t idx) const {
        return LayerRegion(layer_get_region(handle(), idx));
    }

    LayerIsland island(uint32_t idx) const {
        return LayerIsland(layer_get_island(handle(), idx));
    }
};

class Object : public ConstDataTreeHandleView<object_handle>
{
public:
    using ConstDataTreeHandleView<object_handle>::ConstDataTreeHandleView;

    Config config() const {
        return Config(object_get_config(handle()));
    }

    coord_t max_z() const { return object_get_max_z(handle()); }
    c_matrix4d transform() const { return object_get_transform(handle()); }
    c_point center_offset() const { return object_get_center_offset(handle()); }

    c_matrix4d transform_centered() const {
        const c_point offset = center_offset();
        return matrix4d_mul(matrix4d_translation(-unscaled(offset.x), -unscaled(offset.y), 0.0), transform());
    }

    uint32_t layer_count() const { return object_count_layer(handle()); }
    Layer layer(uint32_t idx) const {
        return Layer(object_get_layer(handle(), idx));
    }

    uint32_t print_region_count() const { return object_count_region(handle()); }
    PrintRegion print_region(uint32_t idx) const {
        return PrintRegion(object_get_print_region(handle(), idx));
    }

    uint32_t volume_count() const;
    Volume volume(uint32_t idx) const;
};

class Print : public ConstDataTreeHandleView<print_handle>
{
public:
    using ConstDataTreeHandleView<print_handle>::ConstDataTreeHandleView;

    Config config() const {
        return Config(print_get_config(handle()));
    }

    uint32_t object_count() const { return print_count_object(handle()); }
    Object object(uint32_t idx) const {
        return Object(print_get_object(handle(), idx));
    }
};

} // namespace slic3r_api


#endif // slic3r_Api_plugin_cpp_DataTreeViews_hpp_
