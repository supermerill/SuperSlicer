///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_ClipperViews_hpp_
#define slic3r_Api_plugin_cpp_ClipperViews_hpp_

#include "libslic3r/Api/plugin/c/slic3r_clipper.h"
#include "libslic3r/Api/plugin/cpp/GeometryViews.hpp"

namespace slic3r_api {

/*
Clipper C++ views
=================

This header is the C++ plugin helper for polygon boolean operations, offsets and
polyline clipping. It wraps the C ABI from slic3r_clipper.h without exposing the
host-only C++ geometry internals to external plugins.

Mental model
------------
ClipperOperand is an intermediate shape object. It can be built from Polygon,
Polyline, PolygonCollection, ExPolygon or ExPolygonCollection views, then passed
to operations such as clipper_diff(), clipper_intersection(), clipper_union(),
clipper_offset() and clipper_offset2(). When the algorithm needs normal
geometry again, materialize the operand with to_expolygon_collection(),
to_polygon_collection(), write_expolygons_to() or write_polygons_to().

Typical pattern:

    ClipperContext clipper(storage);
    ClipperOperand subject = clipper(island.infill_areas());
    ClipperOperand forbidden = clipper(region_clip);
    StoredExPolygonCollection allowed =
        clipper_diff(subject, forbidden).to_expolygon_collection();

Ownership and lifetime
----------------------
All ClipperOperand handles are tied to one storage_handle. Most constructors and
all Clipper operations allocate a storage-owned handle, and the C++ object frees
that handle in its destructor. ClipperOperand is therefore move-only. Returning a
ClipperOperand by value is fine; copying one is not allowed.

The constructors from geometry views create an adapter over existing geometry.
They do not deep-copy every source point. Keep the source geometry view alive
until the ClipperOperand has been consumed. The materialized output collections
are independent storage-owned collections.

Concatenation versus union
--------------------------
concat(), concat_replace(), operator+ and operator+= append raw paths without
performing geometric cleanup. This is useful for accumulating many pieces
cheaply. Call clipper_union() afterwards when the result must be normalized,
merged, and have holes rebuilt.

Safety-offset helpers
---------------------
clipper_diff_with_safety_offset() and clipper_intersection_with_safety_offset()
mirror the common host-side "safety offset" behavior: the clip side is expanded
slightly before the boolean operation. Use them when tiny numerical gaps at a
boundary would otherwise leave slivers. Use the plain variants when exact area
accounting is more important than absorbing boundary noise.

Offsets
-------
clipper_offset(subject, delta) grows closed polygons for positive delta and
shrinks them for negative delta. clipper_offset2(subject, delta1, delta2) is the
two-stage offset helper used by many slicer algorithms to remove narrow features
or build rings. For open polylines, pass the appropriate CLIPPER_END_* type.

Choosing output form
--------------------
Use StoredExPolygonCollection when contour/hole topology matters. Use
StoredPolygonCollection only when a flat list of polygons is enough. For open
paths, use clipper_diff_polyline_expolygons() or
clipper_intersection_polyline_expolygons(); converting an open polyline through
ClipperOperand and back to polygons is not the same operation.
*/

/* ========================= generic read-only / stored views ========================= */
/*
MutableHandleView is the mixed read/write base used by data-tree and Clipper views.
It stores exactly one handle plus a mutability flag: the object is either a
borrowed read-only view or a borrowed mutable view, never two pointers at once.
*/
template<class Handle> class MutableHandleView
{
public:
    MutableHandleView() = default;
    explicit MutableHandleView(Handle *handle) : m_handle(handle), m_is_mutable(true) { assert(handle != nullptr); }
    explicit MutableHandleView(const Handle *handle) : m_handle(handle), m_is_mutable(false) { assert(handle != nullptr); }

    bool valid() const { return m_handle != nullptr; }
    explicit operator bool() const { return valid(); }
    bool is_mutable() const { return m_handle != nullptr && m_is_mutable; }

    Handle *mutable_handle() const {
        assert(is_mutable());
        return const_cast<Handle *>(m_handle);
    }
    const Handle *handle() const {
        assert(m_handle != nullptr);
        return m_handle;
    }

    bool same_handle(const MutableHandleView &other) const { return m_handle == other.m_handle; }

protected:
    const Handle *raw_handle() const { return m_handle; }
    Handle *raw_mutable_handle() const { return const_cast<Handle *>(m_handle); }
    void clear_handle() {
        m_handle = nullptr;
        m_is_mutable = false;
    }
    void set_handle(Handle *handle) {
        assert(handle != nullptr);
        m_handle = handle;
        m_is_mutable = true;
    }
    void set_handle(const Handle *handle) {
        assert(handle != nullptr);
        m_handle = handle;
        m_is_mutable = false;
    }

private:
    const Handle *m_handle = nullptr;
    bool m_is_mutable = false;
};


/* ========================= clipper shapes ========================= */
/*
Small C++ wrapper over the strict C Clipper ABI.

ClipperOperand is a C++ view over one storage-owned clipper_shapes_handle, not the
internal ApiClipper::ClipperShapes provider interface. It mirrors the C API
exactly while still allowing plugin code to read like normal C++:

    ClipperOperand subject(storage, polygon);
    ClipperOperand mask(storage, expolygons);
    ClipperOperand result = clipper_diff(storage, subject, mask);
    StoredExPolygonCollection islands = result.to_expolygon_collection();

Use free_from_storage() or storage_free(storage, handle) for temporary
ClipperOperand views that are no longer needed. A ClipperOperand view is always bound to
one storage at construction time; replace() may change the handle, but never the
storage.
*/

class ClipperOperand : public MutableHandleView<clipper_shapes_handle>
{
private:
    ClipperOperand(storage_handle *storage, clipper_shapes_handle *handle, bool owner) :
        MutableHandleView<clipper_shapes_handle>(handle), m_storage(storage), m_owner(owner) { assert(storage != nullptr); }
    ClipperOperand(storage_handle *storage, const clipper_shapes_handle *const_handle, bool owner) :
        MutableHandleView<clipper_shapes_handle>(const_handle), m_storage(storage), m_owner(owner) { assert(storage != nullptr); }
public:
    ClipperOperand() = delete;

     /* == non-owning constructors of ClipperOperand == */
    // Null view bound to storage: no storage allocation, useful when a variable
    // will receive a real storage-owned handle later. Most operations still
    // require valid().
    explicit ClipperOperand(storage_handle *storage) : m_storage(storage), m_owner(false) { assert(storage != nullptr); }

    //use constructor to construct the right kind of view (owning the handle or not)
    static ClipperOperand null(storage_handle *storage) { return ClipperOperand(storage); }
    
    //// Create a view of the handle created and stored inside storage. Be sure that the lifetime of the handle outlive this ClipperOperand.
    //static ClipperOperand borrowed(storage_handle *storage, const clipper_shapes_handle *handle) {
    //    return ClipperOperand(storage, handle, false);
    //}

    
     /* == owning constructors of ClipperOperand == */

    //move-only, as you don't know what you copy otherwise.
    ClipperOperand(const ClipperOperand &) = delete;
    ClipperOperand &operator=(const ClipperOperand &) = delete;

    ClipperOperand(ClipperOperand &&other) noexcept
        : MutableHandleView<clipper_shapes_handle>()
        , m_storage(other.m_storage)
        , m_owner(other.m_owner) {
        set_handle(other.raw_mutable_handle());
        other.clear_handle();
        other.m_owner = false;
    }


    static ClipperOperand create_empty(storage_handle *storage) {
        return ClipperOperand(storage, clipper_shapes_create_empty(storage), true);
    }

    // take ownership of the dynamic handle created and stored inside storage. Be sure to that this handle isn't owned by anything else.
    static ClipperOperand adopt(storage_handle *storage, clipper_shapes_handle *handle) {
        return ClipperOperand(storage, handle, true);
    }

    storage_handle *storage() const { return m_storage; }

    bool empty() const { return clipper_shapes_empty(raw_handle()) != 0; }
    // Count non-empty raw Clipper paths. This is not an ExPolygon count: a
    // contour and each non-empty hole are counted separately.
    uint32_t path_count() const { return clipper_shapes_path_count(raw_handle()); }
    // Return the bounding box of the raw Clipper points represented by this
    // operand. This avoids materializing polygons when only extents are needed.
    c_bounding_box bounding_box() const { return clipper_shapes_bounding_box(raw_handle()); }

    // Constructors create a storage-owned Clipper adapter over an existing view.
    // They do not copy the source geometry, so the source view must stay valid
    // while this ClipperOperand handle is in use
    ClipperOperand(storage_handle *storage, const Polygon &polygon) :
        ClipperOperand(storage, clipper_shapes_from_polygon(storage, polygon.handle()), true) {}

    ClipperOperand(storage_handle *storage, const Polyline &polyline) :
        ClipperOperand(storage, clipper_shapes_from_polyline(storage, polyline.handle()), true) {}

    ClipperOperand(storage_handle *storage, const PolygonCollection &polygons) :
        ClipperOperand(storage, clipper_shapes_from_polygons(storage, polygons.handle()), true) {}

    ClipperOperand(storage_handle *storage, const ExPolygon &expolygon) :
        ClipperOperand(storage, clipper_shapes_from_expolygon(storage, expolygon.handle()), true) {}

    ClipperOperand(storage_handle *storage, const ExPolygonCollection &expolygons) :
        ClipperOperand(storage, clipper_shapes_from_expolygons(storage, expolygons.handle()), true) {}

    // Named factories are equivalent to the constructors above. They are useful
    // when explicit intent reads better at the call site.
    static ClipperOperand from_polygon(storage_handle *storage, const Polygon &polygon) {
        return ClipperOperand(storage, clipper_shapes_from_polygon(storage, polygon.handle()), true);
    }

    static ClipperOperand from_polyline(storage_handle *storage, const Polyline &polyline) {
        return ClipperOperand(storage, clipper_shapes_from_polyline(storage, polyline.handle()), true);
    }

    static ClipperOperand from_polygons(storage_handle *storage, const PolygonCollection &polygons) {
        return ClipperOperand(storage, clipper_shapes_from_polygons(storage, polygons.handle()), true);
    }

    static ClipperOperand from_expolygon(storage_handle *storage, const ExPolygon &expolygon) {
        return ClipperOperand(storage, clipper_shapes_from_expolygon(storage, expolygon.handle()), true);
    }

    static ClipperOperand from_expolygons(storage_handle *storage, const ExPolygonCollection &expolygons) {
        return ClipperOperand(storage, clipper_shapes_from_expolygons(storage, expolygons.handle()), true);
    }


    // Destuctors
    ~ClipperOperand()
    {
        // if we're the owner, then we can delete it.
        // in the worst case (already freed, oups), the storage won't complain.
        free_from_storage();
    }

    // Free the handle from storage if we're the owner. This is idempotent, so it can be called safely even if we
    // don't own the handle or if it's already freed (it's doing nothing then). It's automatically called when the view is destroyed.
    // Return true if the free happened, false if the strage doesn't have the handle to free (anymore).
    bool free_from_storage() {
        assert(m_storage);
        assert(m_owner == is_mutable()); // it seems it's always the case currently.
        if (!m_owner || !is_mutable())
            return false;
        bool freed = storage_free(m_storage, mutable_handle());
        assert(freed);
        clear_handle();
        m_owner = false;
        return freed;
    }

    // Materialize the shape into a storage-owned ExPolygonCollection. Prefer
    // this when contour/hole hierarchy matters.
    // The returned collection is owned by storage, so free it with storage_free(storage, handle) when t's no longer used.
    StoredExPolygonCollection to_expolygon_collection() const {
        return StoredExPolygonCollection::adopt(m_storage, clipper_shapes_to_expolygons(m_storage, handle()));
    }

    // Materialize the shape into a storage-owned polygon collection. This is a
    // flat representation: holes are not grouped with contours.
    // The returned collection is owned by storage, so free it with storage_free(storage, handle) when t's no longer used.
    StoredPolygonCollection to_polygon_collection() const {
        return StoredPolygonCollection::adopt(m_storage, clipper_shapes_to_polygons(m_storage, handle()));
    }

    // Replace an existing destination collection without allocating a new collection handle.
    void write_expolygons_to(StoredExPolygonCollection &dst) const {
        clipper_shapes_replace_expolygons(dst.mutable_handle(), handle());
    }

    // Replace an existing destination collection without allocating a new collection handle.
    void write_polygons_to(StoredPolygonCollection &dst) const {
        clipper_shapes_replace_polygons(dst.mutable_handle(), handle());
    }

    // Replace this view by another storage-owned ClipperOperand result.
    //
    // If this currently owns a handle, that handle is released from its storage
    // before adopting other. This is meant for the common pattern:
    //
    //     my_operand.replace(clipper_intersection(my_operand, clip));
    //
    // Treat other as consumed after this call, as it's now a weak ptr without any way to know if its handle is
    // deleted or alive.
    //
    // Important: other must have been created from the same storage as this
    // object. Replacing with a handle from another storage would make ownership
    // ambiguous and is rejected.
    ClipperOperand &operator=(ClipperOperand &&other) {
        assert(other.m_storage == m_storage);
        if (this == &other)
            return *this;
        if (other.m_storage != m_storage)
            return *this;
        free_from_storage();
        set_handle(other.raw_mutable_handle());
        other.clear_handle();
        m_owner = other.m_owner;
        other.m_owner = false;
        return *this;
    }

    // Concatenate raw paths without performing a geometric union.
    //
    // This creates a new storage-owned operand and leaves this object unchanged.
    // The result is a flat path accumulator; call clipper_union() later if a
    // real geometric union is needed.
    ClipperOperand concat(const ClipperOperand &second) const {
        assert(second.m_storage == m_storage);
        if (second.m_storage != m_storage)
            return ClipperOperand(m_storage);
        return ClipperOperand(m_storage, ::clipper_concat(m_storage, handle(), second.handle()), true);
    }
    ClipperOperand operator+(const ClipperOperand &b) {
        return this->concat(b);
    }

    // Concatenate raw paths into this operand, replacing its current handle (if needed).
    void concat_replace(const ClipperOperand &second) {
        assert(m_owner == is_mutable()); // it seems it's always the case currently.
        assert(m_storage);
        assert(second.m_storage == m_storage);
        if (second.m_storage != m_storage)
            return;
        if (m_owner && is_mutable()) {
            // if we're the owner, we can reuse the handle and maybe avoid an allocation.
            set_handle(::clipper_concat_replace(m_storage, mutable_handle(), second.handle()));
        } else if (raw_handle() != nullptr) {
            if (m_owner) {
                free_from_storage();
            }
            // we're not an owner, so we need to use the existing handle as input, and take ownership of
            // the result.
            set_handle(::clipper_concat(m_storage, handle(), second.handle()));
            m_owner = true;
        } else {
            // we are empty, just copy
            set_handle(::clipper_concat(m_storage, nullptr, second.handle()));
            m_owner = true;
        }
    }
    ClipperOperand &operator+=(const ClipperOperand &other) {
        this->concat_replace(other);
        return *this;
    }

    // Materialize to a temporary ExPolygonCollection, visit each ExPolygon,
    // then free the temporary collection automatically.
    template<class Fn> void for_each_expolygon(Fn &&fn) const {
        StoredExPolygonCollection tmp = to_expolygon_collection();
        for (ExPolygon expolygon : tmp)
            fn(expolygon);
    }

    // Same as for_each_expolygon(), but stops when fn(expolygon) returns false.
    template<class Fn> void for_each_expolygon_until(Fn &&fn) const {
        StoredExPolygonCollection tmp = to_expolygon_collection();
        for (ExPolygon expolygon : tmp) {
            if (!fn(expolygon))
                break;
        }
    }

private:
    storage_handle *const m_storage;
    bool m_owner;
};

// Convenience wrappers around the C ABI. Each operation returns a new
// storage-owned ClipperOperand handle so operations can be chained. The wrappers
// also short-circuit empty inputs so plugin algorithms do not need to repeat the
// same "nothing to clip" checks before every boolean or offset operation.
inline ClipperOperand clipper_diff(storage_handle *storage, const ClipperOperand &subject, const ClipperOperand &clip) {
    if (subject.empty())
        return ClipperOperand::create_empty(storage);
    if (clip.empty())
        return ClipperOperand::adopt(storage, ::clipper_union(storage, subject.handle()));
    return ClipperOperand::adopt(storage, ::clipper_diff(storage, subject.handle(), clip.handle()));
}

inline ClipperOperand clipper_intersection(storage_handle *storage, const ClipperOperand &subject, const ClipperOperand &clip) {
    if (subject.empty() || clip.empty())
        return ClipperOperand::create_empty(storage);
    return ClipperOperand::adopt(storage, ::clipper_intersection(storage, subject.handle(), clip.handle()));
}

inline ClipperOperand clipper_diff(const ClipperOperand &subject, const ClipperOperand &clip) {
    assert(subject.storage() != nullptr);
    assert(clip.storage() == subject.storage());
    return clipper_diff(subject.storage(), subject, clip);
}

inline ClipperOperand clipper_intersection(const ClipperOperand &subject, const ClipperOperand &clip) {
    assert(subject.storage() != nullptr);
    assert(clip.storage() == subject.storage());
    return clipper_intersection(subject.storage(), subject, clip);
}

// Safety-offset variants mirror ApplySafetyOffset::Yes from ClipperUtils. The
// clip is expanded internally and no extra temporary handle is exposed to the
// caller.
inline ClipperOperand clipper_diff_with_safety_offset(storage_handle *storage,
                                                    const ClipperOperand &subject,
                                                    const ClipperOperand &clip) {
    if (subject.empty())
        return ClipperOperand::create_empty(storage);
    if (clip.empty())
        return ClipperOperand::adopt(storage, ::clipper_union(storage, subject.handle()));
    return ClipperOperand::adopt(storage, ::clipper_diff_with_safety_offset(storage, subject.handle(), clip.handle()));
}

inline ClipperOperand clipper_intersection_with_safety_offset(storage_handle *storage,
                                                           const ClipperOperand &subject,
                                                           const ClipperOperand &clip) {
    if (subject.empty() || clip.empty())
        return ClipperOperand::create_empty(storage);
    return ClipperOperand::adopt(storage, ::clipper_intersection_with_safety_offset(storage, subject.handle(), clip.handle()));
}

// basically union(offset(safety_offset))
inline ClipperOperand clipper_union_with_safety_offset(storage_handle *storage,
                                                           const ClipperOperand &subject) {
    if (subject.empty())
        return ClipperOperand::create_empty(storage);
    return ClipperOperand::adopt(storage, ::clipper_union_with_safety_offset(storage, subject.handle()));
}

inline ClipperOperand clipper_diff_with_safety_offset(const ClipperOperand &subject, const ClipperOperand &clip) {
    assert(subject.storage() != nullptr);
    assert(clip.storage() == subject.storage());
    return clipper_diff_with_safety_offset(subject.storage(), subject, clip);
}

inline ClipperOperand clipper_intersection_with_safety_offset(const ClipperOperand &subject, const ClipperOperand &clip) {
    assert(subject.storage() != nullptr);
    assert(clip.storage() == subject.storage());
    return clipper_intersection_with_safety_offset(subject.storage(), subject, clip);
}

// basically union(offset(safety_offset))
inline ClipperOperand clipper_union_with_safety_offset(const ClipperOperand &subject) {
    assert(subject.storage() != nullptr);
    return clipper_union_with_safety_offset(subject.storage(), subject);
}

inline ClipperOperand clipper_union(storage_handle *storage, const ClipperOperand &subject) {
    if (subject.empty())
        return ClipperOperand::create_empty(storage);
    return ClipperOperand::adopt(storage, ::clipper_union(storage, subject.handle()));
}

inline ClipperOperand clipper_union(const ClipperOperand &subject) {
    return clipper_union(subject.storage(), subject);
}

inline ClipperOperand clipper_union2(storage_handle *storage,
                                   const ClipperOperand &subject1,
                                   const ClipperOperand &subject2) {
    if (subject1.empty())
        return clipper_union(storage, subject2);
    if (subject2.empty())
        return clipper_union(storage, subject1);
    return ClipperOperand::adopt(storage, ::clipper_union2(storage, subject1.handle(), subject2.handle()));
}

inline ClipperOperand clipper_union2(const ClipperOperand &subject1, const ClipperOperand &subject2) {
    assert(subject2.storage() == subject1.storage());
    return clipper_union2(subject1.storage(), subject1, subject2);
}

// Offset defaults mirror the common ClipperUtils polygon offset defaults.
inline ClipperOperand clipper_offset(storage_handle *storage,
                                   const ClipperOperand &subject,
                                   double delta,
                                   clipper_join_type_t join_type = CLIPPER_JOIN_MITER,
                                   double miter_limit = 3.0,
                                   clipper_end_type_t end_type = CLIPPER_END_CLOSED_POLYGON) {
    if (subject.empty())
        return ClipperOperand::create_empty(storage);
    return ClipperOperand::adopt(storage, ::clipper_offset(storage, subject.handle(), delta, join_type, miter_limit, end_type));
}

inline ClipperOperand clipper_offset(const ClipperOperand &subject,
                                   double delta,
                                   clipper_join_type_t join_type = CLIPPER_JOIN_MITER,
                                   double miter_limit = 3.0,
                                   clipper_end_type_t end_type = CLIPPER_END_CLOSED_POLYGON) {
    return clipper_offset(subject.storage(), subject, delta, join_type, miter_limit, end_type);
}

inline ClipperOperand clipper_offset2(storage_handle *storage,
                                    const ClipperOperand &subject,
                                    double delta1,
                                    double delta2,
                                    clipper_join_type_t join_type = CLIPPER_JOIN_MITER,
                                    double miter_limit = 3.0,
                                    clipper_end_type_t end_type = CLIPPER_END_CLOSED_POLYGON) {
    if (subject.empty())
        return ClipperOperand::create_empty(storage);
    return ClipperOperand::adopt(storage, ::clipper_offset2(storage, subject.handle(), delta1, delta2, join_type, miter_limit, end_type));
}

inline ClipperOperand clipper_offset2(const ClipperOperand &subject,
                                    double delta1,
                                    double delta2,
                                    clipper_join_type_t join_type = CLIPPER_JOIN_MITER,
                                    double miter_limit = 3.0,
                                    clipper_end_type_t end_type = CLIPPER_END_CLOSED_POLYGON) {
    return clipper_offset2(subject.storage(), subject, delta1, delta2, join_type, miter_limit, end_type);
}

inline StoredPolylineCollection clipper_diff_polyline_expolygons(storage_handle *storage,
                                                                 const Polyline &subject,
                                                                 const ExPolygonCollection &clip)
{
    if (subject.empty())
        return StoredPolylineCollection(storage);
    if (clip.empty()) {
        StoredPolylineCollection out(storage);
        out.push_back(subject);
        return out;
    }
    return StoredPolylineCollection::adopt(storage, ::clipper_diff_polyline_expolygons(storage, subject.handle(), clip.handle()));
}

inline StoredPolylineCollection clipper_intersection_polyline_expolygons(storage_handle *storage,
                                                                         const Polyline &subject,
                                                                         const ExPolygonCollection &clip)
{
    if (subject.empty() || clip.empty())
        return StoredPolylineCollection(storage);
    return StoredPolylineCollection::adopt(storage, ::clipper_intersection_polyline_expolygons(storage, subject.handle(), clip.handle()));
}

inline StoredExPolygonCollection clipper_clip_expolygons_with_subject_bbox(storage_handle *storage,
                                                                           const ExPolygonCollection &src,
                                                                           c_bounding_box bbox)
{
    if (src.empty())
        return StoredExPolygonCollection(storage);
    return StoredExPolygonCollection::adopt(storage, ::clipper_clip_expolygons_with_subject_bbox(storage, src.handle(), bbox));
}

// Keep a large Clipper operand in the Clipper pipeline while discarding paths
// that cannot touch a small subject bbox. The result is a flat path list meant
// for the next boolean operation, not a final ExPolygon hierarchy.
inline ClipperOperand clipper_clip_shapes_with_subject_bbox(storage_handle *storage,
                                                            const ClipperOperand &src,
                                                            c_bounding_box bbox)
{
    if (src.empty())
        return ClipperOperand::create_empty(storage);
    return ClipperOperand::adopt(storage, ::clipper_clip_shapes_with_subject_bbox(storage, src.handle(), bbox));
}

inline ClipperOperand clipper_clip_shapes_with_subject_bbox(const ClipperOperand &src,
                                                            c_bounding_box bbox)
{
    assert(src.storage() != nullptr);
    return clipper_clip_shapes_with_subject_bbox(src.storage(), src, bbox);
}

// context utility method to shorten 'ClipperOperand(storage_handler, bridged_other_layers_area))' to a
// 'clipper(bridged_other_layers_area)' if you define ClipperContext clipper(my_storage_handler)
class ClipperContext
{
public:
    explicit ClipperContext(storage_handle *storage) : m_storage(storage) { assert(m_storage != nullptr); }
    
    ClipperOperand operator()() const { return ClipperOperand(m_storage); }
    ClipperOperand operator()(const Polygon &polygon) const { return ClipperOperand(m_storage, polygon); }
    ClipperOperand operator()(const Polyline &polyline) const { return ClipperOperand(m_storage, polyline); }
    ClipperOperand operator()(const PolygonCollection &polygons) const { return ClipperOperand(m_storage, polygons); }
    ClipperOperand operator()(const ExPolygon &expolygon) const { return ClipperOperand(m_storage, expolygon); }
    ClipperOperand operator()(const ExPolygonCollection &expolygons) const { return ClipperOperand(m_storage, expolygons); }

    ClipperOperand empty() const { return ClipperOperand::create_empty(m_storage); }

private:
    storage_handle *const m_storage;
};

} // namespace slic3r_api


#endif // slic3r_Api_plugin_cpp_ClipperViews_hpp_
