///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_ExtrusionViews_hpp_
#define slic3r_Api_plugin_cpp_ExtrusionViews_hpp_

/*
Developer guides:

    [Using Plugin Properties](../../../../../doc/plugins/properties.md)
    [Using Unified Extrusion Entities](../../../../../doc/plugins/extrusions.md)
*/

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_extrusions.h"
#include "libslic3r/Api/plugin/c/slic3r_extrusion_entity.h"
#include "libslic3r/Api/plugin/c/slic3r_extrusion_polyline.h"
#include "libslic3r/Api/plugin/c/slic3r_extrusion_property.h"
#include "libslic3r/Api/plugin/cpp/ConfigViews.hpp"
#include "libslic3r/Api/plugin/cpp/GeometryViews.hpp"
#include "libslic3r/Api/plugin/cpp/PluginPropertyKey.hpp"
#include "libslic3r/Api/plugin/cpp/properties/ExtrusionProperties.hpp"

namespace slic3r_api {

class ExtrusionEntity;
class MutableExtrusionEntity;
class StoredExtrusionEntity;
struct ExtrusionAreaFragment;

/* Select the boundary occupied by a new ordered leaf. */
enum class OrderedLeafPosition : int32_t
{
    Before = RAW_EXTRUSION_ORDERED_LEAF_BEFORE,
    After = RAW_EXTRUSION_ORDERED_LEAF_AFTER
};

/* Select whether existing direct properties remain on the stable parent. */
enum class ExistingPropertyPlacement : int32_t
{
    KeepOnParent = RAW_EXTRUSION_EXISTING_PROPERTIES_KEEP_ON_PARENT,
    MoveWithExistingContent = RAW_EXTRUSION_EXISTING_PROPERTIES_MOVE_WITH_CONTENT
};

/*
Extrusion entity C++ views
==========================

This file is the ergonomic C++ layer over the strict C extrusion ABI. It
represents a printable path as an ordered tree rather than as a collection of
unrelated path types.

Ownership model
---------------
- `ExtrusionEntity` is a borrowed read-only view.
- `MutableExtrusionEntity` is a borrowed mutable view; it never frees the
  handle.
- `StoredExtrusionEntity` owns an entity created in a `storage_handle` and frees
  it with `storage_free()` when destroyed.

The views do not extend the lifetime of the entity they refer to. A borrowed
view is valid only while its host-owned or storage-owned entity remains alive
and is not invalidated by a structural mutation. Do not return a borrowed view
to an entity created as a local `StoredExtrusionEntity`; return the stored owner
when the function creates ownership.

Entity content model
--------------------
An entity has one of three useful content states:

- A leaf has a local polyline and no children.
- A node has children and normally no local polyline. Its child order is part
  of the printable path order.
- An empty entity has neither a polyline nor children and can be used as a
  temporary destination while a plugin builds a tree.

The helper methods keep local and recursive operations distinct. Point and
segment methods affect only the local polyline. `local_front()`,
`local_back()`, and `local_length()` also remain local, whereas `front()`,
`back()`, `middle()`, `length()`, and `empty()` account for child content when
the entity is a node.

A read-only traversal can therefore inspect a leaf and recurse into a node
without knowing which legacy extrusion type produced it:

    void inspect(const ExtrusionEntity &entity)
    {
        if (entity.has_polyline()) {
            const c_point start = entity.local_front();
            const c_point end = entity.local_back();
            // Process this leaf's local path.
        }

        for (const ExtrusionEntity child : entity.children())
            inspect(child);
    }

Mutable plugins commonly build a complete tree in storage before publishing it:

    StoredExtrusionEntity root(storage);
    MutableExtrusionEntity child = root.emplace_child();
    child.set_points(points);
    child.get_or_add(EPropertyAttributes::key)
        .extrusion_role(RAW_EXTRUSION_ROLE_EXTERNAL_PERIMETER);

`emplace_child()` returns a borrowed mutable view into `root`; `root` remains
the owner. Moving a standalone stored entity into a parent transfers its
content and leaves the source entity empty. The `sortable()` and
`reversible()` flags tell later ordering steps whether child order or direction
may be changed.

Invalidation rules
------------------
Reacquire borrowed views, child ranges, iterators, property pointers, and
stored-data pointers after inserting, removing, moving, or replacing content.
Property pointers are also invalidated when their property is removed or when
the entity is copied over or moved over. A `StoredExtrusionEntity` keeps its
own root handle alive, but it does not make borrowed views into transferred or
replaced children permanent.

Property model
--------------
Properties are small typed payloads stored directly on one entity. These helpers
do not perform inherited lookup through parents: `get(key)` reads only the
property physically present on that entity, and `get_or_add(key)` creates or
edits only a direct property. Use an extrusion tree visitor, or track parent
state yourself, when inherited properties matter.

The most important printable-path property is EPropertyAttributes. It stores:
    role        raw_extrusion_role describing perimeter/infill/support/etc.;
    mm3_per_mm  volumetric extrusion per millimeter of path;
    width       visual/physical extrusion width in millimeters;
    height      layer/extrusion height in millimeters;
    no_seam     request that seam placement does not move this path start.

Role constants are RAW_EXTRUSION_ROLE_* values from the C plugin API. Step
contexts usually provide the correct flow for a role through LayerRegion views.

Typical read:
    if (const EPropertyAttributes *attr = entity.get(EPropertyAttributes::key))
        ... attr->extrusion_role() ...

Typical write on a mutable or stored entity:
    EPropertyAttributes &attr = entity.get_or_add(EPropertyAttributes::key);
    attr.extrusion_role(RAW_EXTRUSION_ROLE_EXTERNAL_PERIMETER)
        .mm3_per_mm(flow.mm3_per_mm)
        .width(unscaled_width)
        .height(unscaled_height);

Built-in and custom properties may both be represented by
PluginPropertyKey<T>. A built-in key reads T::property_type, while a dynamic key
registers a stable name and retains its orchestrator. The same key can then be
used for reading, creation and removal without passing a separate numeric id.

Large data model
----------------
Property payloads must stay small and trivially copyable. Text or binary blobs
are stored separately with an extrusion_data_id. Use store_property_string() or
store_property_value() when a field inside a property owns that data: replacing
or removing the property will then release the old buffer automatically. Use
store_string() or store_value() only for standalone data whose lifetime is not
tied to one property field.
*/

inline bool points_equal(c_point lhs, c_point rhs)
{
    return lhs.x == rhs.x && lhs.y == rhs.y;
}

inline bool is_invalid_index(uint32_t idx)
{
    return idx == EXTRUSION_INDEX_INVALID;
}

template<class Payload>
inline const Payload *property_payload_cast(const void *data)
{
    // The ABI gives us an untyped byte pointer. Property helpers may reinterpret
    // it only if the payload is raw data with no constructors or hidden state.
    static_assert(std::is_trivially_copyable<Payload>::value, "Extrusion property payloads must be trivially copyable");
    return reinterpret_cast<const Payload *>(data);
}

template<class Payload>
inline Payload *property_payload_cast(void *data)
{
    // Same rule as the const overload: mutable access changes bytes in-place
    // inside the entity, so the C++ type must exactly describe those bytes.
    static_assert(std::is_trivially_copyable<Payload>::value, "Extrusion property payloads must be trivially copyable");
    return reinterpret_cast<Payload *>(data);
}

template<class Payload>
inline extrusion_property_type register_extrusion_property_type(orchestrator_handle *orchestrator,
                                                                const char *namespaced_name)
{
    // Custom type ids belong to one orchestrator. Store the returned id in the
    // plugin instance for this run; do not serialize it as a stable file value.
    assert(orchestrator != nullptr);
    static_assert(std::is_trivially_copyable<Payload>::value, "Custom extrusion properties must be trivially copyable");
    return extrusion_property_register_type(orchestrator, namespaced_name, sizeof(Payload), alignof(Payload));
}

/* ========================= read / mutable CRTP APIs ========================= */

/*
Read access to properties stored directly on one entity.

Available on:
    ExtrusionEntity, MutableExtrusionEntity, StoredExtrusionEntity

The methods here are intentionally local. They do not climb parent entities to
find inherited state. This keeps a single entity view cheap and predictable; a
tree traversal helper can combine parent properties when an algorithm needs the
effective state at a leaf.
*/
template<class Derived> class ExtrusionPropertyReadApi
{
public:
    /* Enumerate direct properties. The order is only for inspection/debugging. */
    uint32_t property_count() const { return extrusion_property_count(self().handle()); }
    extrusion_property_type property_type_at(uint32_t idx) const { return extrusion_property_type_at(self().handle(), idx); }
    bool has_property(extrusion_property_type type) const { return extrusion_property_has(self().handle(), type) != 0; }

    template<class Payload> bool has(const PluginPropertyKey<Payload> &key) const {
        return has_property(key.type());
    }

    /*
    Return a raw pointer to one direct property payload, or nullptr.

    The pointer is owned by the entity. It becomes invalid if the entity is
    modified, destroyed, copied over, moved over, or if the property is removed.
    Prefer get(key) when the payload type is known.
    */
    const void *property_data(extrusion_property_type type) const {
        return extrusion_property_data(self().handle(), type);
    }

    /* Typed view of property_data(). Returns nullptr when the property is absent. */
    template<class Payload> const Payload *get(const PluginPropertyKey<Payload> &key) const {
        return property_payload_cast<Payload>(property_data(key.type()));
    }

    /*
    Return a copy of the direct property or fallback if absent. This is useful
    when a plugin wants simple value semantics and does not need to distinguish
    "missing property" from "property equal to fallback".
    */
    template<class Payload>
    Payload get_or(const PluginPropertyKey<Payload> &key, Payload fallback) const {
        const Payload *payload = get(key);
        return payload != nullptr ? *payload : fallback;
    }

    /*
    Read auxiliary data referenced by an extrusion_data_id.

    This is independent from property payloads. The returned pointer is borrowed
    from the entity and follows the same short lifetime rules as property_data().
    */
    const void *stored_data(extrusion_data_id id, uint32_t *byte_size_out = nullptr) const {
        return extrusion_data(self().handle(), id, byte_size_out);
    }

    /* Return a typed stored POD only when the buffer has its exact size. */
    template<class Payload> const Payload *stored_value(extrusion_data_id id) const {
        static_assert(std::is_trivially_copyable<Payload>::value, "Stored data payloads must be trivially copyable");
        uint32_t byte_size = 0;
        const void *data = stored_data(id, &byte_size);
        return data != nullptr && byte_size == sizeof(Payload) ?
            reinterpret_cast<const Payload *>(data) : nullptr;
    }

    /*
    Copy a stored data buffer into a std::string.

    Stored strings are written with a terminating '\0' by the helper below. The
    copy returned here removes that terminator so normal C++ string operations
    see only the user text.
    */
    std::string stored_string(extrusion_data_id id) const {
        uint32_t byte_size = 0;
        const char *data = static_cast<const char *>(stored_data(id, &byte_size));
        if (data == nullptr || byte_size == 0)
            return {};
        if (data[byte_size - 1] == '\0')
            --byte_size;
        return std::string(data, data + byte_size);
    }

protected:
    const Derived &self() const { return static_cast<const Derived &>(*this); }
};

/*
Mutable access to direct properties and auxiliary data.

Available on:
    MutableExtrusionEntity, StoredExtrusionEntity

Use get_or_add(key) for both built-in and plugin-registered properties. The key
carries the orchestrator needed to create a dynamic payload.
*/
template<class Derived> class ExtrusionPropertyMutableApi
{
public:
    /* Return an existing mutable property payload. This never creates it. */
    void *property_data_mutable(extrusion_property_type type) {
        return extrusion_property_data_mutable(self().mutable_handle(), type);
    }

    /*
    Fetch or create a mutable property payload. Newly created payload bytes are
    zero-initialized by the host, which makes bool/int fields deterministic.
    */
    void *get_or_add_property_data_mutable(orchestrator_handle *orchestrator, extrusion_property_type type) {
        return extrusion_property_get_or_add_data_mutable(orchestrator, self().mutable_handle(), type);
    }

    /* Typed mutable pointer to an existing property, or nullptr if absent. */
    template<class Payload> Payload *get_mutable(const PluginPropertyKey<Payload> &key) {
        return property_payload_cast<Payload>(property_data_mutable(key.type()));
    }

    /*
    Typed fetch-or-create helper. This is the normal way to add attributes:

        EPropertyAttributes &attr = entity.get_or_add(EPropertyAttributes::key);
        attr.extrusion_role(role).mm3_per_mm(mm3).width(width).height(height);
    */
    template<class Payload> Payload &get_or_add(const PluginPropertyKey<Payload> &key) {
        Payload *payload = property_payload_cast<Payload>(
            get_or_add_property_data_mutable(key.orchestrator(), key.type()));
        if (payload == nullptr)
            throw std::runtime_error("The typed extrusion property could not be created.");
        return *payload;
    }

    bool remove_property(extrusion_property_type type) {
        return extrusion_property_remove(self().mutable_handle(), type) != 0;
    }

    template<class Payload> bool remove(const PluginPropertyKey<Payload> &key) {
        return remove_property(key.type());
    }

    /*
    Store standalone bytes on the entity.

    The returned id is not owned by any property field. Free it explicitly with
    free_data() if it should disappear before the whole entity is destroyed.
    */
    extrusion_data_id store_data(const void *data, uint32_t byte_size, uint32_t alignment) {
        return extrusion_store_data_aligned(self().mutable_handle(), data, byte_size, alignment);
    }

    /*
    Store bytes owned by one extrusion_data_id field inside a property payload.

    The host replaces the previous id in *field, frees the old buffer for this
    field, writes the new id, and later frees it automatically when owner_type is
    removed. field must point inside the mutable payload of owner_type.
    */
    extrusion_data_id store_property_data(extrusion_property_type owner_type,
                                          extrusion_data_id *field,
                                          const void *data,
                                          uint32_t byte_size,
                                          uint32_t alignment) {
        return extrusion_property_store_data_aligned(self().mutable_handle(), owner_type, field, data, byte_size, alignment);
    }

    template<class Payload> extrusion_data_id store_value(const Payload &payload) {
        static_assert(std::is_trivially_copyable<Payload>::value, "Stored data payloads must be trivially copyable");
        return store_data(&payload, sizeof(Payload), alignof(Payload));
    }

    template<class Payload> extrusion_data_id store_property_value(extrusion_property_type owner_type,
                                                                   extrusion_data_id *field,
                                                                   const Payload &payload) {
        static_assert(std::is_trivially_copyable<Payload>::value, "Stored data payloads must be trivially copyable");
        return store_property_data(owner_type, field, &payload, sizeof(Payload), alignof(Payload));
    }

    /* Store a standalone null-terminated string and return its data id. */
    extrusion_data_id store_string(std::string_view text) {
        const std::string text_copy(text);
        return store_data(text_copy.c_str(), static_cast<uint32_t>(text_copy.size() + 1), alignof(char));
    }

    /* Store a null-terminated string owned by a property field. */
    extrusion_data_id store_property_string(extrusion_property_type owner_type,
                                            extrusion_data_id *field,
                                            std::string_view text) {
        const std::string text_copy(text);
        return store_property_data(owner_type, field, text_copy.c_str(), static_cast<uint32_t>(text_copy.size() + 1), alignof(char));
    }

    bool free_data(extrusion_data_id id) {
        return extrusion_free_data(self().mutable_handle(), id) != 0;
    }

    /*
    Convenience helper for the common "custom G-code with text" property.

    It creates the property if needed and stores text_id with property-owned
    lifetime, so remove(EPropertyCustomGcode::key) also releases the text.
    */
    EPropertyCustomGcode &custom_gcode(std::string_view text,
                                       c_extrusion_custom_gcode_kind kind = C_EXTRUSION_CUSTOM_GCODE_GCODE)
    {
        EPropertyCustomGcode &payload = get_or_add(EPropertyCustomGcode::key);
        payload.set_kind(kind);
        payload.processing_extruder_id = GCODE_SCRIPT_PROCESSING_EXTRUDER_INVALID;
        if (store_property_string(EPropertyCustomGcode::property_type, &payload.text_id, text) ==
            EXTRUSION_DATA_ID_INVALID) {
            remove(EPropertyCustomGcode::key);
            throw std::runtime_error("The custom G-code text could not be stored.");
        }
        if (payload.config_id != EXTRUSION_DATA_ID_INVALID)
            free_data(payload.config_id);
        payload.config_id = EXTRUSION_DATA_ID_INVALID;
        return payload;
    }

    /* Store a PlaceholderParser script without producer-specific Config values. */
    EPropertyCustomGcode &script_gcode(
        std::string_view text,
        gcode_script_type script_type,
        uint16_t processing_extruder_id = GCODE_SCRIPT_PROCESSING_EXTRUDER_INVALID) {
        EPropertyCustomGcode &payload = custom_gcode(text, C_EXTRUSION_CUSTOM_GCODE_SCRIPT);
        payload.script_type = script_type;
        payload.processing_extruder_id = processing_extruder_id;
        return payload;
    }

    /* Serialize and store the immutable producer context with one script event. */
    EPropertyCustomGcode &script_gcode(
        std::string_view text,
        gcode_script_type script_type,
        const Config &config,
        uint16_t processing_extruder_id = GCODE_SCRIPT_PROCESSING_EXTRUDER_INVALID) {
        // Serialize before touching the entity so an invalid Config leaves any
        // existing property and its owned buffers unchanged.
        const std::string serialized_config = config.serialize_all();
        EPropertyCustomGcode &payload = script_gcode(text, script_type, processing_extruder_id);
        if (store_property_string(EPropertyCustomGcode::property_type,
                                  &payload.config_id,
                                  serialized_config) == EXTRUSION_DATA_ID_INVALID) {
            remove(EPropertyCustomGcode::key);
            throw std::runtime_error("The G-code script Config could not be stored.");
        }
        return payload;
    }

protected:
    Derived &self() { return static_cast<Derived &>(*this); }
};

/*
Read access to the local polyline stored on an entity.

Available on:
    ExtrusionEntity, MutableExtrusionEntity, StoredExtrusionEntity

These methods do not look at children. For tree-wide endpoints and length, use
front(), back(), middle(), length(), empty(), and collect_points() from
ExtrusionEntityReadApi.
*/
template<class Derived> class ExtrusionPolylineReadApi
{
public:
    bool has_polyline() const { return extrusion_has_polyline(self().handle()) != 0; }
    uint32_t point_count() const { return extrusion_polyline_point_count(self().handle()); }
    uint32_t segment_count() const { return extrusion_polyline_segment_count(self().handle()); }
    bool has_local_points() const { return point_count() > 0; }
    c_point point(uint32_t idx) const { return extrusion_polyline_point(self().handle(), idx); }
    c_point operator[](uint32_t idx) const { return point(idx); }
    c_point local_front() const { assert(point_count() > 0); return point(0); }
    c_point local_back() const { assert(point_count() > 0); return point(point_count() - 1); }
    c_point local_middle() const { assert(point_count() > 0); return point(point_count() / 2); }
    distf_t local_length() const { return extrusion_polyline_length(self().handle()); }
    bool local_is_closed() const {
        return point_count() > 1 && points_equal(local_front(), local_back());
    }

    uint32_t find_point(c_point point, coord_t max_distance) const {
        return extrusion_polyline_find_point(self().handle(), point, max_distance);
    }

    /*
    Return a copied point located distance from the beginning of the local path.

    The C ABI exposes point_from_end(), so this helper converts the distance
    without mutating or reversing the entity.
    */
    c_point point_from_begin(distf_t distance) const {
        const distf_t length = local_length();
        return point_from_end(distance >= length ? 0. : length - distance);
    }

    c_point point_from_start(distf_t distance) const { return point_from_begin(distance); }

    c_point point_from_end(distf_t distance) const {
        return extrusion_polyline_point_from_end(self().handle(), distance);
    }

    /*
    Read one true segment, including both endpoints, optional arc data, and the
    Z offsets at those endpoints. A polyline with N points has N - 1 segments.
    */
    c_extrusion_segment segment(uint32_t idx) const {
        c_extrusion_segment out = {};
        const int32_t ok = extrusion_polyline_segment(self().handle(), idx, &out);
        assert(ok != 0);
        (void) ok;
        return out;
    }

    std::vector<c_point> points() const {
        std::vector<c_point> out(point_count());
        if (!out.empty())
            extrusion_polyline_copy_points(self().handle(), out.data(), static_cast<uint32_t>(out.size()));
        return out;
    }

    std::vector<c_extrusion_segment> segments() const {
        std::vector<c_extrusion_segment> out(segment_count());
        if (!out.empty())
            extrusion_polyline_copy_segments(self().handle(), out.data(), static_cast<uint32_t>(out.size()));
        return out;
    }

    bool has_z_offsets() const { return extrusion_polyline_has_z_offsets(self().handle()) != 0; }

    /*
    Return one point Z offset. When the entity has no explicit Z-offset array,
    the ABI reports 0, so callers can treat missing offsets as flat extrusion.
    */
    coord_t z_offset(uint32_t idx) const {
        coord_t out = 0;
        const int32_t ok = extrusion_polyline_z_offset(self().handle(), idx, &out);
        assert(ok != 0);
        (void) ok;
        return out;
    }

protected:
    const Derived &self() const { return static_cast<const Derived &>(*this); }
};

/*
Mutable access to the local polyline stored on an entity.

Available on:
    MutableExtrusionEntity, StoredExtrusionEntity

Replacing a local polyline fails if the entity currently has children. Call
clear_content() first only when discarding those children is intentional.
*/
template<class Derived> class ExtrusionPolylineMutableApi
{
public:
    bool clear_polyline() { return extrusion_polyline_clear(self().mutable_handle()) != 0; }
    bool set_point(uint32_t idx, c_point point) {
        return extrusion_polyline_set_point(self().mutable_handle(), idx, point) != 0;
    }
    uint32_t insert_point(uint32_t idx, c_point point) {
        return extrusion_polyline_insert_point(self().mutable_handle(), idx, point);
    }
    bool remove_point(uint32_t idx) {
        return extrusion_polyline_remove_point(self().mutable_handle(), idx) != 0;
    }
    bool set_points(const c_point *points, uint32_t count) {
        return extrusion_polyline_set_points(self().mutable_handle(), points, count) != 0;
    }
    bool set_points(const std::vector<c_point> &points) {
        return set_points(points.empty() ? nullptr : points.data(), static_cast<uint32_t>(points.size()));
    }
    bool set_segment(uint32_t idx, const c_extrusion_segment &segment) {
        return extrusion_polyline_set_segment(self().mutable_handle(), idx, &segment) != 0;
    }
    bool set_segments(const c_extrusion_segment *segments, uint32_t count) {
        return extrusion_polyline_set_segments(self().mutable_handle(), segments, count) != 0;
    }
    bool set_segments(const std::vector<c_extrusion_segment> &segments) {
        return set_segments(segments.empty() ? nullptr : segments.data(), static_cast<uint32_t>(segments.size()));
    }
    bool clip_end(distf_t distance) { return extrusion_polyline_clip_end(self().mutable_handle(), distance) != 0; }

    /*
    Remove distance from the beginning of the local polyline.

    The ABI has only clip_end(). Reversing twice keeps arc directions and
    per-point Z offsets synchronized with the clipped path.
    */
    bool clip_start(distf_t distance) {
        const bool reversed = reverse();
        assert(reversed);
        if (!reversed)
            return false;
        const bool clipped = clip_end(distance);
        const bool restored = reverse();
        assert(restored);
        return clipped && restored;
    }
    bool translate(c_point offset) { return extrusion_polyline_translate(self().mutable_handle(), offset) != 0; }
    bool rotate(double angle) { return extrusion_polyline_rotate(self().mutable_handle(), angle) != 0; }
    bool reverse() { return extrusion_polyline_reverse(self().mutable_handle()) != 0; }
    bool set_z_offset(uint32_t idx, coord_t z_offset) {
        return extrusion_polyline_set_z_offset(self().mutable_handle(), idx, z_offset) != 0;
    }
    bool clear_z_offsets() { return extrusion_polyline_clear_z_offsets(self().mutable_handle()) != 0; }

    /*
    Replace the local polyline from a simple object exposing points().

    This intentionally writes only straight points. Use set_segments() when arc
    metadata or explicit Z offsets must be preserved.
    */
    template<class PolylineLike> bool set(const PolylineLike &polyline) {
        return set_points(polyline.points());
    }

protected:
    Derived &self() { return static_cast<Derived &>(*this); }
};

/*
Read access to the extrusion tree.

Available on:
    ExtrusionEntity, MutableExtrusionEntity, StoredExtrusionEntity

The local-polyline helpers above inspect only the current node. The helpers here
walk children when needed, so they are appropriate for algorithms that want to
treat an entity tree as one printable unit.
*/
template<class Derived> class ExtrusionEntityReadApi
{
public:
    bool valid() const { return self().handle() != nullptr; }
    explicit operator bool() const { return valid(); }
    uint32_t flags() const { return extrusion_flags(self().handle()); }

    /*
    Flags describe how path-planning code may transform this node:
        reversible: the whole entity may be reversed;
        sortable: children may be reordered;
        continuous: computed from the current child order and points.
    */
    bool reversible() const { return (flags() & RAW_EXTRUSION_FLAG_REVERSIBLE) != 0; }
    bool sortable() const { return (flags() & RAW_EXTRUSION_FLAG_SORTABLE) != 0; }
    bool continuous() const { return extrusion_is_continuous(self().handle()) != 0; }
    /* Return whether this is a non-empty, fixed, continuous closed path. */
    bool is_loop() const { return extrusion_is_loop(self().handle()) != 0; }
    bool is_leaf() const { return extrusion_has_children(self().handle()) == 0; }
    uint32_t child_count() const { return extrusion_child_count(self().handle()); }

    ExtrusionEntity child(uint32_t idx) const;

    /* Return true when neither this node nor any descendant has local points. */
    bool empty() const {
        if (self().point_count() > 0)
            return false;
        for (uint32_t idx = 0; idx < child_count(); ++idx)
            if (!child(idx).empty())
                return false;
        return true;
    }

    /* First point of the first non-empty descendant, or the local front point. */
    c_point front() const {
        assert(!empty());
        if (self().point_count() > 0)
            return self().local_front();
        for (uint32_t idx = 0; idx < child_count(); ++idx) {
            ExtrusionEntity candidate = child(idx);
            if (!candidate.empty())
                return candidate.front();
        }
        return {};
    }

    /* Last point of the last non-empty descendant, or the local back point. */
    c_point back() const {
        assert(!empty());
        if (self().point_count() > 0)
            return self().local_back();
        for (uint32_t idx = child_count(); idx > 0; --idx) {
            ExtrusionEntity candidate = child(idx - 1);
            if (!candidate.empty())
                return candidate.back();
        }
        return {};
    }

    /*
    Representative point near the middle of the entity tree.

    This is not an arclength midpoint. It is a cheap stable point for UI/debug
    and simple heuristics that only need something inside the entity.
    */
    c_point middle() const {
        assert(!empty());
        if (self().point_count() > 0)
            return self().local_middle();
        for (uint32_t idx = child_count() / 2; idx < child_count(); ++idx) {
            ExtrusionEntity candidate = child(idx);
            if (!candidate.empty())
                return candidate.middle();
        }
        return front();
    }

    /* Sum local path length recursively through all descendants. */
    distf_t length() const {
        if (self().point_count() > 0)
            return self().local_length();
        distf_t out = 0;
        for (uint32_t idx = 0; idx < child_count(); ++idx)
            out += child(idx).length();
        return out;
    }

    bool is_closed() const {
        return !empty() && points_equal(front(), back());
    }

    /* Copy all local points in tree order. This flattens structure and arcs. */
    std::vector<c_point> collect_points() const {
        std::vector<c_point> out;
        collect_points(out);
        return out;
    }

    void collect_points(std::vector<c_point> &out) const {
        const std::vector<c_point> local_points = self().points();
        out.insert(out.end(), local_points.begin(), local_points.end());
        for (uint32_t idx = 0; idx < child_count(); ++idx)
            child(idx).collect_points(out);
    }

    class children_iterator
    {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = ExtrusionEntity;
        using difference_type = std::ptrdiff_t;

        children_iterator() = default;
        children_iterator(const Derived *owner, uint32_t idx) : m_owner(owner), m_idx(idx) {}
        value_type operator*() const;
        children_iterator &operator++() { ++m_idx; return *this; }
        children_iterator operator++(int) { children_iterator copy = *this; ++(*this); return copy; }
        bool operator==(const children_iterator &rhs) const { return m_owner == rhs.m_owner && m_idx == rhs.m_idx; }
        bool operator!=(const children_iterator &rhs) const { return !(*this == rhs); }

    private:
        const Derived *m_owner = nullptr;
        uint32_t m_idx = 0;
    };

    class children_range
    {
    public:
        explicit children_range(const Derived *owner) : m_owner(owner) {}
        children_iterator begin() const { return children_iterator(m_owner, 0); }
        children_iterator end() const { return children_iterator(m_owner, m_owner == nullptr ? 0 : m_owner->child_count()); }

    private:
        const Derived *m_owner = nullptr;
    };

    children_range children() const { return children_range(&self()); }

    /* Deep-copy this entity and all descendants into storage. */
    StoredExtrusionEntity clone(storage_handle *storage) const;

protected:
    const Derived &self() const { return static_cast<const Derived &>(*this); }
};

/*
Mutable access to tree structure and entity flags.

Available on:
    MutableExtrusionEntity, StoredExtrusionEntity

Adding children and owning a local polyline are exclusive states. If an entity
has local points, insert_child_*() may fail. Clear or move the polyline first
when intentionally converting a leaf into a collection node.
*/
template<class Derived> class ExtrusionEntityMutableApi
{
public:
    MutableExtrusionEntity child_mutable(uint32_t idx) const;

    bool set_flags(uint32_t flags) { return extrusion_set_flags(self().mutable_handle(), flags) != 0; }

    /* Allow or forbid path planners from reversing this entity. */
    Derived &reversible(bool enabled = true) {
        const uint32_t new_flags = enabled ?
            (self().flags() | RAW_EXTRUSION_FLAG_REVERSIBLE) :
            (self().flags() & ~RAW_EXTRUSION_FLAG_REVERSIBLE);
        const bool ok = set_flags(new_flags);
        assert(ok);
        (void) ok;
        return self();
    }
    Derived &disable_reverse() { return reversible(false); }

    /*
    Allow or forbid reordering of children. Sorting is meaningful only for
    non-continuous collections, not for one local polyline.
    */
    Derived &sortable(bool enabled = true) {
        const uint32_t new_flags = enabled ?
            (self().flags() | RAW_EXTRUSION_FLAG_SORTABLE) :
            (self().flags() & ~RAW_EXTRUSION_FLAG_SORTABLE);
        const bool ok = set_flags(new_flags);
        assert(ok);
        (void) ok;
        return self();
    }
    Derived &disable_sort() { return sortable(false); }

    /* Remove local polyline and children. Properties and flags stay attached. */
    bool clear_content() { return extrusion_clear_content(self().mutable_handle()) != 0; }

    /* Insert a deep copy of child. The source child is unchanged. */
    uint32_t insert_child_copy(uint32_t idx, const ExtrusionEntity &child);

    /*
    Insert child content by move. The source handle remains valid but becomes an
    empty entity after the move succeeds.
    */
    uint32_t insert_child_move(uint32_t idx, MutableExtrusionEntity child);

    /*
    Create a fixed empty leaf before or after the entity's previous content.

    A rejected operation returns an invalid view and leaves the entity
    unchanged. On success, reacquire borrowed views of this entity's direct
    structure and properties; handles to transferred existing children remain
    valid. A later structural mutation may invalidate the returned leaf view.
    ExistingPropertyPlacement makes property inheritance explicit instead of
    silently applying parent state to the inserted leaf.
    */
    MutableExtrusionEntity emplace_ordered_leaf(
        OrderedLeafPosition position,
        ExistingPropertyPlacement property_placement);

    /* Append at the end of the child list while making the ownership behavior explicit. */
    uint32_t append_child_copy(const ExtrusionEntity &child);
    uint32_t append_child_move(MutableExtrusionEntity child);
    bool remove_child(uint32_t idx) { return extrusion_remove_child(self().mutable_handle(), idx) != 0; }

    /*
    Move one existing child between parents. This is the tree-edit operation;
    insert_child_move() moves from a standalone mutable entity handle.
    */
    uint32_t move_child_from(uint32_t dst_idx, MutableExtrusionEntity src_parent, uint32_t src_idx);

protected:
    Derived &self() { return static_cast<Derived &>(*this); }
    const Derived &self() const { return static_cast<const Derived &>(*this); }
};

/*
Borrowed read-only entity view.

It can inspect direct properties, local polyline geometry, and the recursive
tree shape. It never owns or frees the handle, so the caller must ensure the
host object or storage that owns the entity outlives the view.
*/
class ExtrusionEntity :
    public ExtrusionPropertyReadApi<ExtrusionEntity>,
    public ExtrusionPolylineReadApi<ExtrusionEntity>,
    public ExtrusionEntityReadApi<ExtrusionEntity>
{
public:
    ExtrusionEntity() = default;
    explicit ExtrusionEntity(const extrusion_entity_handle *handle) : m_handle(handle) { assert(handle != nullptr); }

    const extrusion_entity_handle *handle() const {
        assert(m_handle != nullptr);
        return m_handle;
    }
    bool same_handle(const ExtrusionEntity &other) const { return m_handle == other.m_handle; }

protected:
    const extrusion_entity_handle *m_handle = nullptr;
};

/*
Borrowed mutable entity view.

It exposes every read helper plus property, polyline, flag and child mutation.
It does not own the handle; use StoredExtrusionEntity when a helper function
needs to create an entity and transfer ownership to its caller.
*/
class MutableExtrusionEntity :
    public ExtrusionPropertyReadApi<MutableExtrusionEntity>,
    public ExtrusionPropertyMutableApi<MutableExtrusionEntity>,
    public ExtrusionPolylineReadApi<MutableExtrusionEntity>,
    public ExtrusionPolylineMutableApi<MutableExtrusionEntity>,
    public ExtrusionEntityReadApi<MutableExtrusionEntity>,
    public ExtrusionEntityMutableApi<MutableExtrusionEntity>
{
public:
    MutableExtrusionEntity() = default;
    explicit MutableExtrusionEntity(extrusion_entity_handle *handle) : m_handle(handle) { assert(handle != nullptr); }

    bool valid() const { return m_handle != nullptr; }

    const extrusion_entity_handle *handle() const {
        assert(m_handle != nullptr);
        return m_handle;
    }
    extrusion_entity_handle *mutable_handle() const {
        assert(m_handle != nullptr);
        return m_handle;
    }
    ExtrusionEntity readonly() const { return ExtrusionEntity(handle()); }
    operator ExtrusionEntity() const { return readonly(); }

    /*
    Split this leaf with a complete, disjoint area partition.

    Returned entities are mutable borrowed views into this leaf. A later
    structural mutation of the leaf invalidates them. The host preserves the
    source traversal order and returns the input vector index for every piece.
    */
    std::vector<ExtrusionAreaFragment> split_leaf_by_areas(
        const std::vector<ExPolygonCollection> &areas,
        coord_t max_deviation = SCALED_EPSILON) const;

private:
    extrusion_entity_handle *m_handle = nullptr;
};

struct ExtrusionAreaFragment
{
    MutableExtrusionEntity entity;
    uint32_t area_index = 0;
};

/*
Owned entity stored in a storage_handle.

Use this for temporary plugin-owned entities, for cloned output, or when
building a subtree before moving it into a host-owned LayerRegionIsland. Moving
the C++ object transfers ownership of the handle; copying is disabled to avoid
double-free bugs.
*/
class StoredExtrusionEntity :
    public ExtrusionPropertyReadApi<StoredExtrusionEntity>,
    public ExtrusionPropertyMutableApi<StoredExtrusionEntity>,
    public ExtrusionPolylineReadApi<StoredExtrusionEntity>,
    public ExtrusionPolylineMutableApi<StoredExtrusionEntity>,
    public ExtrusionEntityReadApi<StoredExtrusionEntity>,
    public ExtrusionEntityMutableApi<StoredExtrusionEntity>
{
public:
    StoredExtrusionEntity() = delete;
    StoredExtrusionEntity(const StoredExtrusionEntity &) = delete;
    StoredExtrusionEntity &operator=(const StoredExtrusionEntity &) = delete;

    explicit StoredExtrusionEntity(storage_handle *storage) :
        StoredExtrusionEntity(storage, extrusion_create_empty(storage)) {}

    /* Deep-copy an existing borrowed entity into storage. */
    StoredExtrusionEntity(storage_handle *storage, const ExtrusionEntity &src) :
        StoredExtrusionEntity(storage, extrusion_clone(storage, src.handle())) {}

    /* Create an entity whose local polyline is the supplied straight points. */
    StoredExtrusionEntity(storage_handle *storage, const std::vector<c_point> &points) :
        StoredExtrusionEntity(storage) { set_points(points); }

    StoredExtrusionEntity(storage_handle *storage, std::initializer_list<c_point> points) :
        StoredExtrusionEntity(storage, std::vector<c_point>(points)) {}

    template<class PolylineLike> StoredExtrusionEntity(storage_handle *storage, const PolylineLike &polyline) :
        StoredExtrusionEntity(storage) { set(polyline); }

    StoredExtrusionEntity(StoredExtrusionEntity &&rhs) noexcept :
        m_storage(rhs.m_storage), m_handle(rhs.m_handle) {
        rhs.m_storage = nullptr;
        rhs.m_handle = nullptr;
    }

    StoredExtrusionEntity &operator=(StoredExtrusionEntity &&rhs) noexcept {
        if (this == &rhs)
            return *this;
        reset();
        m_storage = rhs.m_storage;
        m_handle = rhs.m_handle;
        rhs.m_storage = nullptr;
        rhs.m_handle = nullptr;
        return *this;
    }

    ~StoredExtrusionEntity() { reset(); }

    static StoredExtrusionEntity adopt(storage_handle *storage, extrusion_entity_handle *handle) {
        return StoredExtrusionEntity(storage, handle);
    }

    const extrusion_entity_handle *handle() const {
        assert(m_handle != nullptr);
        return m_handle;
    }
    extrusion_entity_handle *mutable_handle() const {
        assert(m_handle != nullptr);
        return m_handle;
    }
    storage_handle *storage() const {
        assert(m_storage != nullptr);
        return m_storage;
    }
    ExtrusionEntity readonly() const & { return ExtrusionEntity(handle()); }
    ExtrusionEntity readonly() && = delete;
    MutableExtrusionEntity mutable_view() const { return MutableExtrusionEntity(mutable_handle()); }
    operator ExtrusionEntity() const & { return readonly(); }
    operator ExtrusionEntity() && = delete;
    operator MutableExtrusionEntity() const { return mutable_view(); }

    /* Release the owned handle before destruction. The object becomes invalid. */
    bool free_from_storage() { return reset(); }

    /* Replace this handle's contents while preserving the handle identity. */
    bool copy_from(const ExtrusionEntity &src) { return extrusion_copy_from(mutable_handle(), src.handle()) != 0; }

    /* Move another mutable entity into this handle. The source becomes empty. */
    bool move_from(MutableExtrusionEntity src) { return extrusion_move_from(mutable_handle(), src.mutable_handle()) != 0; }

    /*
    Append a new empty child and return a mutable borrowed view to it.

    The temporary StoredExtrusionEntity is moved into the parent before it is
    destroyed, so the returned child is owned by this entity tree.
    */
    MutableExtrusionEntity emplace_child() {
        StoredExtrusionEntity child(storage());
        const uint32_t idx = append_child_move(child.mutable_view());
        assert(!is_invalid_index(idx));
        return child_mutable(idx);
    }

private:
    StoredExtrusionEntity(storage_handle *storage, extrusion_entity_handle *handle) :
        m_storage(storage), m_handle(handle) {
        assert(storage != nullptr);
        assert(handle != nullptr);
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
    extrusion_entity_handle *m_handle = nullptr;
};

/*
Factory for the medial-axis extrusion helper.

The raw C ABI uses one parameter struct because the medial-axis stage and the
extrusion conversion stage must agree on widths, height and endpoint behavior.
This factory gives C++ plugins a safer fluent interface: start from role+flow,
override only the options that matter for the local algorithm, then build the
storage-owned extrusion tree.
*/
class MedialAxisExtrusionFactory
{
public:
    MedialAxisExtrusionFactory(raw_extrusion_role role, c_flow flow)
    {
        m_params = {};
        m_params.role = role;
        m_params.flow = flow;
        m_params.min_medial_width = flow.width;
        m_params.max_medial_width = std::max(flow.width, flow.spacing);
        m_params.flags = MEDIAL_AXIS_EXTRUSION_TRIM_THIN_ENDPOINTS;
    }

    MedialAxisExtrusionFactory &medial_widths(coord_t min_width, coord_t max_width) {
        m_params.min_medial_width = min_width;
        m_params.max_medial_width = max_width;
        return *this;
    }

    MedialAxisExtrusionFactory &extrusion_widths(coord_t min_width, coord_t max_width) {
        m_params.min_extrusion_width = min_width;
        m_params.max_extrusion_width = max_width;
        return *this;
    }

    MedialAxisExtrusionFactory &min_centerline_length(coord_t value) {
        m_params.min_centerline_length = value;
        return *this;
    }

    MedialAxisExtrusionFactory &extension_area(const ExPolygon &area) {
        m_params.extension_area = area.handle();
        return *this;
    }

    MedialAxisExtrusionFactory &endpoint_extension(coord_t length) {
        m_params.endpoint_extension_length = length;
        return *this;
    }

    MedialAxisExtrusionFactory &endpoint_taper(coord_t length) {
        m_params.endpoint_taper_length = length;
        return *this;
    }

    MedialAxisExtrusionFactory &role(raw_extrusion_role value) {
        m_params.role = value;
        return *this;
    }

    MedialAxisExtrusionFactory &flow(c_flow value) {
        m_params.flow = value;
        return *this;
    }

    MedialAxisExtrusionFactory &variable_width_resolution(coord_t value) {
        m_params.variable_width_resolution = value;
        return *this;
    }

    MedialAxisExtrusionFactory &width_change_tolerance(coord_t value) {
        m_params.width_change_tolerance = value;
        return *this;
    }

    MedialAxisExtrusionFactory &min_extrusion_length(coord_t value) {
        m_params.min_extrusion_length = value;
        return *this;
    }

    MedialAxisExtrusionFactory &trim_thin_endpoints(bool enabled = true) {
        set_flag(MEDIAL_AXIS_EXTRUSION_TRIM_THIN_ENDPOINTS, enabled);
        return *this;
    }

    MedialAxisExtrusionFactory &can_reverse(bool enabled = true) {
        set_flag(MEDIAL_AXIS_EXTRUSION_CAN_REVERSE, enabled);
        return *this;
    }

    MedialAxisExtrusionFactory &constant_width(bool enabled = true) {
        set_flag(MEDIAL_AXIS_EXTRUSION_CONSTANT_WIDTH, enabled);
        return *this;
    }

    MedialAxisExtrusionFactory &keep_empty_root(bool enabled = true) {
        set_flag(MEDIAL_AXIS_EXTRUSION_KEEP_EMPTY_ROOT, enabled);
        return *this;
    }

    const c_medial_axis_extrusion_params &params() const { return m_params; }

    // Use try_build() when "no printable centerline" is useful information for
    // the caller. Degenerate or too-small areas are valid inputs for medial
    // axis generation, and they may simply produce no extrusion.
    std::optional<StoredExtrusionEntity> try_build(storage_handle *storage, const ExPolygon &src) const {
        extrusion_entity_handle *handle = build_handle(storage, src);
        if (handle == nullptr)
            return std::nullopt;
        return StoredExtrusionEntity::adopt(storage, handle);
    }

    // Use build() when the caller only needs a storage-owned entity container.
    // If the medial axis cannot produce printable paths, the returned entity is
    // empty but still valid, so callers can append it or inspect child_count()
    // without adding an optional branch.
    StoredExtrusionEntity build(storage_handle *storage, const ExPolygon &src) const {
        std::optional<StoredExtrusionEntity> out = try_build(storage, src);
        if (out.has_value())
            return std::move(*out);
        return StoredExtrusionEntity(storage);
    }

private:
    extrusion_entity_handle *build_handle(storage_handle *storage, const ExPolygon &src) const {
        return expolygon_medial_axis_extrusion(storage, src.handle(), &m_params);
    }

    void set_flag(uint32_t flag, bool enabled) {
        if (enabled)
            m_params.flags |= flag;
        else
            m_params.flags &= ~flag;
    }

    c_medial_axis_extrusion_params m_params;
};

inline MedialAxisExtrusionFactory medial_axis_extrusion(raw_extrusion_role role, c_flow flow)
{
    return MedialAxisExtrusionFactory(role, flow);
}

inline MedialAxisExtrusionFactory medial_axis_thin_wall(c_flow flow)
{
    return MedialAxisExtrusionFactory(RAW_EXTRUSION_ROLE_THIN_WALL, flow);
}

inline MedialAxisExtrusionFactory medial_axis_gap_fill(c_flow flow)
{
    return MedialAxisExtrusionFactory(RAW_EXTRUSION_ROLE_GAP_FILL, flow);
}

template<class Derived>
inline ExtrusionEntity ExtrusionEntityReadApi<Derived>::child(uint32_t idx) const
{
    const extrusion_entity_handle *child_handle = extrusion_child(self().handle(), idx);
    assert(child_handle != nullptr);
    return ExtrusionEntity(child_handle);
}

template<class Derived>
inline typename ExtrusionEntityReadApi<Derived>::children_iterator::value_type
ExtrusionEntityReadApi<Derived>::children_iterator::operator*() const
{
    return m_owner->child(m_idx);
}

template<class Derived>
inline StoredExtrusionEntity ExtrusionEntityReadApi<Derived>::clone(storage_handle *storage) const
{
    return StoredExtrusionEntity(storage, ExtrusionEntity(self().handle()));
}

template<class Derived>
inline MutableExtrusionEntity ExtrusionEntityMutableApi<Derived>::child_mutable(uint32_t idx) const
{
    extrusion_entity_handle *child_handle = extrusion_child_mutable(self().mutable_handle(), idx);
    assert(child_handle != nullptr);
    return MutableExtrusionEntity(child_handle);
}

template<class Derived>
inline uint32_t ExtrusionEntityMutableApi<Derived>::insert_child_move(uint32_t idx, MutableExtrusionEntity child)
{
    return extrusion_insert_child_move(self().mutable_handle(), idx, child.mutable_handle());
}

template<class Derived>
inline uint32_t ExtrusionEntityMutableApi<Derived>::insert_child_copy(uint32_t idx, const ExtrusionEntity &child)
{
    return extrusion_insert_child_copy(self().mutable_handle(), idx, child.handle());
}

template<class Derived>
inline MutableExtrusionEntity ExtrusionEntityMutableApi<Derived>::emplace_ordered_leaf(
    OrderedLeafPosition position,
    ExistingPropertyPlacement property_placement)
{
    extrusion_entity_handle *leaf = extrusion_emplace_ordered_leaf(
        self().mutable_handle(),
        static_cast<raw_extrusion_ordered_leaf_position>(position),
        static_cast<raw_extrusion_existing_property_placement>(property_placement));
    return leaf == nullptr ? MutableExtrusionEntity() : MutableExtrusionEntity(leaf);
}

template<class Derived>
inline uint32_t ExtrusionEntityMutableApi<Derived>::append_child_copy(const ExtrusionEntity &child)
{
    return insert_child_copy(self().child_count(), child);
}

template<class Derived>
inline uint32_t ExtrusionEntityMutableApi<Derived>::append_child_move(MutableExtrusionEntity child)
{
    return insert_child_move(self().child_count(), child);
}

template<class Derived>
inline uint32_t ExtrusionEntityMutableApi<Derived>::move_child_from(uint32_t dst_idx,
                                                                    MutableExtrusionEntity src_parent,
                                                                    uint32_t src_idx)
{
    return extrusion_move_child(self().mutable_handle(), dst_idx, src_parent.mutable_handle(), src_idx);
}

inline std::vector<ExtrusionAreaFragment> MutableExtrusionEntity::split_leaf_by_areas(
    const std::vector<ExPolygonCollection> &areas,
    coord_t max_deviation) const
{
    struct CallbackContext
    {
        std::vector<ExtrusionAreaFragment> fragments;
        std::exception_ptr exception;
    };

    std::vector<const expolygon_collection_handle *> area_handles;
    area_handles.reserve(areas.size());
    for (const ExPolygonCollection &area : areas)
        area_handles.push_back(area.handle());

    CallbackContext context;
    const extrusion_split_fragment_fn collect_fragment =
        [](extrusion_entity_handle *fragment, uint32_t area_index, void *user_data) noexcept {
            CallbackContext &callback_context = *static_cast<CallbackContext *>(user_data);
            if (callback_context.exception != nullptr)
                return;
            try {
                callback_context.fragments.push_back({ MutableExtrusionEntity(fragment), area_index });
            } catch (...) {
                // Never let a C++ allocation exception cross the C callback
                // boundary. The wrapper rethrows it once host execution ends.
                callback_context.exception = std::current_exception();
            }
        };

    const raw_extrusion_split_status status = extrusion_split_leaf_by_areas(
        mutable_handle(),
        area_handles.empty() ? nullptr : area_handles.data(),
        static_cast<uint32_t>(area_handles.size()),
        max_deviation,
        collect_fragment,
        &context);

    if (context.exception != nullptr)
        std::rethrow_exception(context.exception);
    if (status == RAW_EXTRUSION_SPLIT_STATUS_SUCCESS)
        return context.fragments;

    switch (status) {
    case RAW_EXTRUSION_SPLIT_STATUS_INVALID_ARGUMENT:
        throw std::invalid_argument("extrusion_split_leaf_by_areas: invalid argument");
    case RAW_EXTRUSION_SPLIT_STATUS_NOT_A_LEAF:
        throw std::invalid_argument("extrusion_split_leaf_by_areas: entity is not a leaf");
    case RAW_EXTRUSION_SPLIT_STATUS_INVALID_GEOMETRY:
        throw std::invalid_argument("extrusion_split_leaf_by_areas: invalid leaf or area geometry");
    case RAW_EXTRUSION_SPLIT_STATUS_CLIPPING_FAILED:
        throw std::runtime_error("extrusion_split_leaf_by_areas: clipping failed");
    case RAW_EXTRUSION_SPLIT_STATUS_INTERNAL_ERROR:
    default:
        throw std::runtime_error("extrusion_split_leaf_by_areas: internal error");
    }
}

} // namespace slic3r_api

#endif // slic3r_Api_plugin_cpp_ExtrusionViews_hpp_
