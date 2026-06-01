///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#include <cmath>
#include <cstdint>
#include <iterator>
#include <memory>

#include "libslic3r/Api/plugin/c/slic3r_clipper.h"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Polyline.hpp"

#include "ClipperShapes.hpp"
#include "Orchestrator.hpp"

namespace Slic3r {

/* ---- ABI handle casts ------------------------------------------------
The public headers expose opaque C handles. Internally they are pointers to the
native Slic3r objects or to the ApiClipper::ClipperShapes interface.
*/

static PluginStorage *to_storage(storage_handle *storage) {
    return reinterpret_cast<PluginStorage *>(storage);
}

static const Polygon *to_polygon(const polygon_handle *handle) {
    return reinterpret_cast<const Polygon *>(handle);
}

static const Polyline *to_polyline(const polyline_handle *handle) {
    return reinterpret_cast<const Polyline *>(handle);
}

static Polylines *to_polylines(polyline_collection_handle *handle) {
    return reinterpret_cast<Polylines *>(handle);
}

static const std::vector<MultiPoint> *to_multipoints(const polygon_collection_handle *handle) {
    return reinterpret_cast<const std::vector<MultiPoint> *>(handle);
}

static std::vector<MultiPoint> *to_multipoints(polygon_collection_handle *handle) {
    return reinterpret_cast<std::vector<MultiPoint> *>(handle);
}

static const ExPolygon *to_expolygon(const expolygon_handle *handle) {
    return reinterpret_cast<const ExPolygon *>(handle);
}

static ExPolygons *to_expolygons(expolygon_collection_handle *handle) {
    return reinterpret_cast<ExPolygons *>(handle);
}

static const ExPolygons *to_expolygons(const expolygon_collection_handle *handle) {
    return reinterpret_cast<const ExPolygons *>(handle);
}

static BoundingBox to_bounding_box(c_bounding_box bbox) {
    return BoundingBox(Point(bbox.min.x, bbox.min.y), Point(bbox.max.x, bbox.max.y));
}

static c_point to_c_point(const Point &point) {
    c_point out = {};
    out.x = point.x();
    out.y = point.y();
    return out;
}

static c_bounding_box to_c_bounding_box(const BoundingBox &bbox) {
    c_bounding_box out = {};
    out.min = to_c_point(bbox.min);
    out.max = to_c_point(bbox.max);
    return out;
}

static ApiClipper::ClipperShapes *to_shapes(clipper_shapes_handle *handle) {
    return reinterpret_cast<ApiClipper::ClipperShapes *>(handle);
}

static const ApiClipper::ClipperShapes *to_shapes(const clipper_shapes_handle *handle) {
    return reinterpret_cast<const ApiClipper::ClipperShapes *>(handle);
}

static ApiClipper::ClipperShapes *store_shape(PluginStorage &plugin_storage, std::unique_ptr<ApiClipper::ClipperShapes> shape) {
    if (!shape)
        return nullptr;
    ApiClipper::ClipperShapes *raw = shape.get();
    plugin_storage.clipper_shapes.emplace_back(std::move(shape));
    plugin_storage.generic_storage.insert(raw);
    return raw;
}

/*
Store a newly created ClipperShapes implementation in plugin temporary storage.

The returned raw pointer is the ABI handle. The unique_ptr stays in
PluginStorage::clipper_shapes, so the pointer remains valid until storage_free()
or storage_clear() removes it.
*/
static clipper_shapes_handle *store_shape(storage_handle *storage, std::unique_ptr<ApiClipper::ClipperShapes> shape) {
    if (storage == nullptr || !shape)
        return nullptr;
    PluginStorage *plugin_storage = to_storage(storage);
    return reinterpret_cast<clipper_shapes_handle *>(store_shape(*plugin_storage, std::move(shape)));
}

/* Translate small ABI enums to ClipperLib enums without leaking Clipper headers
through the C ABI.
*/
static ClipperLib::ClipType to_clip_type(clipper_operation_t operation) {
    switch (operation) {
    case CLIPPER_OPERATION_DIFFERENCE:   return ClipperLib::ctDifference;
    case CLIPPER_OPERATION_INTERSECTION: return ClipperLib::ctIntersection;
    case CLIPPER_OPERATION_UNION:        return ClipperLib::ctUnion;
    case CLIPPER_OPERATION_XOR:          return ClipperLib::ctXor;
    default:                             return ClipperLib::ctDifference;
    }
}

static ClipperLib::JoinType to_join_type(clipper_join_type_t join_type) {
    switch (join_type) {
    case CLIPPER_JOIN_SQUARE: return ClipperLib::jtSquare;
    case CLIPPER_JOIN_ROUND:  return ClipperLib::jtRound;
    case CLIPPER_JOIN_MITER:  return ClipperLib::jtMiter;
    default:                  return ClipperLib::jtMiter;
    }
}

static ClipperLib::EndType to_end_type(clipper_end_type_t end_type) {
    switch (end_type) {
    case CLIPPER_END_CLOSED_POLYGON: return ClipperLib::etClosedPolygon;
    case CLIPPER_END_CLOSED_LINE:    return ClipperLib::etClosedLine;
    case CLIPPER_END_OPEN_BUTT:      return ClipperLib::etOpenButt;
    case CLIPPER_END_OPEN_SQUARE:    return ClipperLib::etOpenSquare;
    case CLIPPER_END_OPEN_ROUND:     return ClipperLib::etOpenRound;
    default:                         return ClipperLib::etClosedPolygon;
    }
}

static ClipperLib::Paths safety_offset_paths(const ApiClipper::ClipperShapes &clip_shapes) {
    ClipperLib::ClipperOffset offsetter;
    offsetter.MiterLimit = DefaultMiterLimit;
    offsetter.ShortestEdgeLength = ClipperSafetyOffset * Slic3r::ClipperOffsetShortestEdgeFactor;
    offsetter.AddPaths(clip_shapes.to_paths(), DefaultJoinType, ClipperLib::etClosedPolygon);

    ClipperLib::Paths out;
    offsetter.Execute(out, ClipperSafetyOffset);
    return out;
}
} // namespace Slic3r

extern "C" {

/* ---- Adapter creation ------------------------------------------------
These functions allocate only a small ClipperShapes adapter in storage. Source
geometry is intentionally not copied, keeping setup cheap for plugin code that
creates short-lived boolean operations.
*/

clipper_shapes_handle *clipper_shapes_create_empty(storage_handle *storage) {
    return Slic3r::store_shape(storage, Slic3r::ApiClipper::make_path_list_shapes(Slic3r::ClipperLib::Paths{}));
}

clipper_shapes_handle *clipper_shapes_from_polygon(storage_handle *storage, const polygon_handle *polygon) {
    return Slic3r::store_shape(storage, Slic3r::ApiClipper::make_polygon_shapes(Slic3r::to_polygon(polygon)));
}

clipper_shapes_handle *clipper_shapes_from_polyline(storage_handle *storage, const polyline_handle *polyline) {
    return Slic3r::store_shape(storage, Slic3r::ApiClipper::make_polyline_shapes(Slic3r::to_polyline(polyline)));
}

clipper_shapes_handle *clipper_shapes_from_polygons(storage_handle *storage, const polygon_collection_handle *polygons) {
    return Slic3r::store_shape(storage, Slic3r::ApiClipper::make_multipoint_collection_shapes(Slic3r::to_multipoints(polygons)));
}

clipper_shapes_handle *clipper_shapes_from_expolygon(storage_handle *storage, const expolygon_handle *expolygon) {
    return Slic3r::store_shape(storage, Slic3r::ApiClipper::make_expolygon_shapes(Slic3r::to_expolygon(expolygon)));
}

clipper_shapes_handle *clipper_shapes_from_expolygons(storage_handle *storage, const expolygon_collection_handle *expolygons) {
    return Slic3r::store_shape(storage, Slic3r::ApiClipper::make_expolygons_shapes(Slic3r::to_expolygons(expolygons)));
}

int32_t clipper_shapes_empty(const clipper_shapes_handle *shapes) {
    const Slic3r::ApiClipper::ClipperShapes *source = Slic3r::to_shapes(shapes);
    return source == nullptr || source->empty() ? 1 : 0;
}

uint32_t clipper_shapes_path_count(const clipper_shapes_handle *shapes) {
    const Slic3r::ApiClipper::ClipperShapes *source = Slic3r::to_shapes(shapes);
    return source == nullptr ? 0 : source->path_count();
}

c_bounding_box clipper_shapes_bounding_box(const clipper_shapes_handle *shapes) {
    const Slic3r::ApiClipper::ClipperShapes *source = Slic3r::to_shapes(shapes);
    return source == nullptr ? c_bounding_box{} : Slic3r::to_c_bounding_box(source->bounding_box());
}

/* ---- Boolean operations ----------------------------------------------
Boolean operations create a PolyTree-backed ClipperShapes result. Returning a
ClipperShapes handle instead of immediately materializing Polygons/ExPolygons
lets callers chain operations without repeatedly converting between formats.
*/

static clipper_shapes_handle *clipper_execute_impl(storage_handle *storage,
                                                   clipper_operation_t operation,
                                                   const clipper_shapes_handle *subject,
                                                   const clipper_shapes_handle *clip,
                                                   bool apply_safety_offset)
{
    const Slic3r::ApiClipper::ClipperShapes *subject_shapes = Slic3r::to_shapes(subject);
    const Slic3r::ApiClipper::ClipperShapes *clip_shapes = Slic3r::to_shapes(clip);
    if (storage == nullptr || subject_shapes == nullptr)
        return nullptr;

    Slic3r::ClipperLib::Clipper clipper;
    subject_shapes->add_to_clipper(clipper, Slic3r::ClipperLib::ptSubject, true);
    if (clip_shapes != nullptr) {
        const bool can_apply_safety_offset =
            operation == CLIPPER_OPERATION_DIFFERENCE || operation == CLIPPER_OPERATION_INTERSECTION;
        if (can_apply_safety_offset && apply_safety_offset) {
            const Slic3r::ClipperLib::Paths expanded_clip = Slic3r::safety_offset_paths(*clip_shapes);
            clipper.AddPaths(expanded_clip, Slic3r::ClipperLib::ptClip, true);
        } else {
            clip_shapes->add_to_clipper(clipper, Slic3r::ClipperLib::ptClip, true);
        }
    }

    // PolyTree keeps the contour/hole hierarchy needed by to_expolygons().
    Slic3r::ClipperLib::PolyTree result;
    clipper.Execute(Slic3r::to_clip_type(operation), result, Slic3r::ClipperLib::pftNonZero, Slic3r::ClipperLib::pftNonZero);
    return Slic3r::store_shape(storage, Slic3r::ApiClipper::make_polytree_shapes(std::move(result)));
}

clipper_shapes_handle *clipper_execute(storage_handle *storage,
                                       clipper_operation_t operation,
                                       const clipper_shapes_handle *subject,
                                       const clipper_shapes_handle *clip)
{
    return clipper_execute_impl(storage, operation, subject, clip, false);
}

clipper_shapes_handle *clipper_diff(storage_handle *storage,
                                    const clipper_shapes_handle *subject,
                                    const clipper_shapes_handle *clip)
{
    return clipper_execute(storage, CLIPPER_OPERATION_DIFFERENCE, subject, clip);
}

clipper_shapes_handle *clipper_intersection(storage_handle *storage,
                                           const clipper_shapes_handle *subject,
                                           const clipper_shapes_handle *clip)
{
    return clipper_execute(storage, CLIPPER_OPERATION_INTERSECTION, subject, clip);
}

clipper_shapes_handle *clipper_diff_with_safety_offset(storage_handle *storage,
                                                       const clipper_shapes_handle *subject,
                                                       const clipper_shapes_handle *clip)
{
    return clipper_execute_impl(storage, CLIPPER_OPERATION_DIFFERENCE, subject, clip, true);
}

clipper_shapes_handle *clipper_intersection_with_safety_offset(storage_handle *storage,
                                                              const clipper_shapes_handle *subject,
                                                              const clipper_shapes_handle *clip)
{
    return clipper_execute_impl(storage, CLIPPER_OPERATION_INTERSECTION, subject, clip, true);
}

clipper_shapes_handle *clipper_union(storage_handle *storage, const clipper_shapes_handle *subject) {
    return clipper_execute(storage, CLIPPER_OPERATION_UNION, subject, nullptr);
}

clipper_shapes_handle *clipper_union_with_safety_offset(storage_handle *storage,
                                                        const clipper_shapes_handle *subject) {
    const Slic3r::ApiClipper::ClipperShapes *subject_shapes = Slic3r::to_shapes(subject);
    if (storage == nullptr || subject_shapes == nullptr)
        return nullptr;

    Slic3r::ClipperLib::Paths expanded = Slic3r::safety_offset_paths(*subject_shapes);

    Slic3r::ClipperLib::Clipper clipper;
    clipper.AddPaths(expanded, Slic3r::ClipperLib::ptSubject, true);

    Slic3r::ClipperLib::PolyTree result;
    clipper.Execute(Slic3r::ClipperLib::ctUnion,
                    result,
                    Slic3r::ClipperLib::pftNonZero,
                    Slic3r::ClipperLib::pftNonZero);

    return Slic3r::store_shape(storage, Slic3r::ApiClipper::make_polytree_shapes(std::move(result)));
}

clipper_shapes_handle *clipper_union2(storage_handle *storage,
                                      const clipper_shapes_handle *subject1,
                                      const clipper_shapes_handle *subject2)
{
    const Slic3r::ApiClipper::ClipperShapes *first = Slic3r::to_shapes(subject1);
    const Slic3r::ApiClipper::ClipperShapes *second = Slic3r::to_shapes(subject2);
    if (storage == nullptr || first == nullptr || second == nullptr)
        return nullptr;

    Slic3r::ClipperLib::Clipper clipper;
    first->add_to_clipper(clipper, Slic3r::ClipperLib::ptSubject, true);
    second->add_to_clipper(clipper, Slic3r::ClipperLib::ptSubject, true);

    Slic3r::ClipperLib::PolyTree result;
    clipper.Execute(Slic3r::ClipperLib::ctUnion, result, Slic3r::ClipperLib::pftNonZero, Slic3r::ClipperLib::pftNonZero);
    return Slic3r::store_shape(storage, Slic3r::ApiClipper::make_polytree_shapes(std::move(result)));
}

// Concatenation intentionally does not run a boolean operation. It creates a
// fresh PathListShapes accumulator containing the raw paths of both inputs.
clipper_shapes_handle *clipper_concat(storage_handle *storage,
                                      const clipper_shapes_handle *first,
                                      const clipper_shapes_handle *second)
{
    const Slic3r::ApiClipper::ClipperShapes *first_shapes = Slic3r::to_shapes(first);
    const Slic3r::ApiClipper::ClipperShapes *second_shapes = Slic3r::to_shapes(second);
    if (storage == nullptr || (first_shapes == nullptr && second_shapes == nullptr))
        return nullptr;

    Slic3r::ClipperLib::Paths paths = first_shapes != nullptr ? first_shapes->to_paths() : Slic3r::ClipperLib::Paths{};
    if (second_shapes != nullptr) {
        Slic3r::ClipperLib::Paths other = second_shapes->to_paths();
        paths.insert(paths.end(),
                     std::make_move_iterator(other.begin()),
                     std::make_move_iterator(other.end()));
    }
    return Slic3r::store_shape(storage, Slic3r::ApiClipper::make_path_list_shapes(std::move(paths)));
}

clipper_shapes_handle *clipper_concat_replace(storage_handle *storage,
                                              clipper_shapes_handle *first,
                                              const clipper_shapes_handle *second)
{
    if (storage == nullptr || first == nullptr || second == nullptr)
        return nullptr;

    Slic3r::PluginStorage *plugin_storage = Slic3r::to_storage(storage);
    Slic3r::ApiClipper::ClipperShapes *first_shapes = Slic3r::to_shapes(first);
    const Slic3r::ApiClipper::ClipperShapes *second_shapes = Slic3r::to_shapes(second);
    if (first_shapes == nullptr || second_shapes == nullptr)
        return nullptr;

    clipper_shapes_handle *result =
        reinterpret_cast<clipper_shapes_handle *>(first_shapes->concat(*second_shapes, *plugin_storage));
    if (result != nullptr && result != first)
        storage_free(storage, first);
    return result;
}

/* ---- Offset operations ------------------------------------------------
ClipperOffset works from raw Paths, so every ClipperShapes implementation first
materializes itself as Paths. The result is again kept as a PolyTree-backed
ClipperShapes so it can be chained or converted later.
*/

clipper_shapes_handle *clipper_offset(storage_handle *storage,
                                      const clipper_shapes_handle *subject,
                                      double delta,
                                      clipper_join_type_t join_type,
                                      double miter_limit,
                                      clipper_end_type_t end_type)
{
    const Slic3r::ApiClipper::ClipperShapes *subject_shapes = Slic3r::to_shapes(subject);
    if (storage == nullptr || subject_shapes == nullptr)
        return nullptr;

    Slic3r::ClipperLib::ClipperOffset offsetter;
    if (Slic3r::to_join_type(join_type) == Slic3r::ClipperLib::jtRound)
        offsetter.ArcTolerance = miter_limit;
    else
        offsetter.MiterLimit = miter_limit;
    // Keep the same shortest-edge heuristic as ClipperUtils offsets.
    offsetter.ShortestEdgeLength = std::abs(delta * Slic3r::ClipperOffsetShortestEdgeFactor);
    offsetter.AddPaths(subject_shapes->to_paths(), Slic3r::to_join_type(join_type), Slic3r::to_end_type(end_type));

    Slic3r::ClipperLib::PolyTree result;
    offsetter.Execute(result, delta);
    return Slic3r::store_shape(storage, Slic3r::ApiClipper::make_polytree_shapes(std::move(result)));
}

clipper_shapes_handle *clipper_offset2(storage_handle *storage,
                                       const clipper_shapes_handle *subject,
                                       double delta1,
                                       double delta2,
                                       clipper_join_type_t join_type,
                                       double miter_limit,
                                       clipper_end_type_t end_type)
{
    // Store only the final result for the caller. The intermediate offset is
    // explicitly freed from the same storage before returning.
    clipper_shapes_handle *first = clipper_offset(storage, subject, delta1, join_type, miter_limit, end_type);
    clipper_shapes_handle *second = clipper_offset(storage, first, delta2, join_type, miter_limit, end_type);
    storage_free(storage, first);
    return second;
}

/* ---- Conversion back to public geometry handles ----------------------
These functions materialize the abstract ClipperShapes result into normal ABI
collections. The output collections are also storage-owned and can be returned
from plugin code or freed with storage_free().
*/

polygon_collection_handle *clipper_shapes_to_polygons(storage_handle *storage, const clipper_shapes_handle *shapes) {
    const Slic3r::ApiClipper::ClipperShapes *source = Slic3r::to_shapes(shapes);
    if (storage == nullptr || source == nullptr)
        return nullptr;

    polygon_collection_handle *out_handle = storage_new_polygons(storage);
    std::vector<Slic3r::MultiPoint> *out = reinterpret_cast<std::vector<Slic3r::MultiPoint> *>(out_handle);
    out->clear();
    for (const Slic3r::Polygon &polygon : source->to_polygons())
        out->emplace_back(polygon.points);
    return out_handle;
}

expolygon_collection_handle *clipper_shapes_to_expolygons(storage_handle *storage, const clipper_shapes_handle *shapes) {
    const Slic3r::ApiClipper::ClipperShapes *source = Slic3r::to_shapes(shapes);
    if (storage == nullptr || source == nullptr)
        return nullptr;

    expolygon_collection_handle *out_handle = storage_new_expolygons(storage);
    *Slic3r::to_expolygons(out_handle) = source->to_expolygons();
    return out_handle;
}

void clipper_shapes_replace_polygons(polygon_collection_handle *dst, const clipper_shapes_handle *shapes) {
    const Slic3r::ApiClipper::ClipperShapes *source = Slic3r::to_shapes(shapes);
    if (dst == nullptr || source == nullptr)
        return;

    std::vector<Slic3r::MultiPoint> &out = *Slic3r::to_multipoints(dst);
    // TODO: optimiser, en réutilisant les objets MultiPoint déjà alloués dans la collection de destination, plutôt
    // que de tout clear() et ré-emplace_back() à chaque fois. Mais cela demande de faire de nouvelles méthodes dans source
    // pour remplir directement un object déjà crée.
    out.clear();
    for (const Slic3r::Polygon &polygon : source->to_polygons())
        out.emplace_back(polygon.points);
}

void clipper_shapes_replace_expolygons(expolygon_collection_handle *dst, const clipper_shapes_handle *shapes) {
    const Slic3r::ApiClipper::ClipperShapes *source = Slic3r::to_shapes(shapes);
    if (dst == nullptr || source == nullptr)
        return;

    *Slic3r::to_expolygons(dst) = source->to_expolygons();
}

expolygon_collection_handle *clipper_clip_expolygons_with_subject_bbox(storage_handle *storage,
                                                                       const expolygon_collection_handle *src,
                                                                       c_bounding_box bbox)
{
    if (storage == nullptr || src == nullptr)
        return nullptr;

    expolygon_collection_handle *out_handle = storage_new_expolygons(storage);
    *Slic3r::to_expolygons(out_handle) =
        Slic3r::ClipperUtils::clip_clipper_expolygons_with_subject_bbox(*Slic3r::to_expolygons(src),
                                                                        Slic3r::to_bounding_box(bbox));
    return out_handle;
}

polyline_collection_handle *clipper_diff_polyline_expolygons(storage_handle *storage,
                                                             const polyline_handle *subject,
                                                             const expolygon_collection_handle *clip)
{
    if (storage == nullptr || subject == nullptr)
        return nullptr;

    polyline_collection_handle *out_handle = storage_new_polylines(storage);
    Slic3r::Polylines &out = *Slic3r::to_polylines(out_handle);
    if (clip == nullptr || Slic3r::to_expolygons(clip)->empty())
        out.push_back(*Slic3r::to_polyline(subject));
    else
        out = Slic3r::diff_pl(*Slic3r::to_polyline(subject), *Slic3r::to_expolygons(clip));
    return out_handle;
}

polyline_collection_handle *clipper_intersection_polyline_expolygons(storage_handle *storage,
                                                                     const polyline_handle *subject,
                                                                     const expolygon_collection_handle *clip)
{
    if (storage == nullptr || subject == nullptr)
        return nullptr;

    polyline_collection_handle *out_handle = storage_new_polylines(storage);
    Slic3r::Polylines &out = *Slic3r::to_polylines(out_handle);
    if (clip != nullptr && !Slic3r::to_expolygons(clip)->empty())
        out = Slic3r::intersection_pl(*Slic3r::to_polyline(subject), *Slic3r::to_expolygons(clip));
    return out_handle;
}

} // extern "C"
