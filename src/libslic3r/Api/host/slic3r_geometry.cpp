///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>

#include "libslic3r/Api/plugin/c/slic3r_extrusions.h"
#include "libslic3r/Api/plugin/c/slic3r_geometry.h"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Geometry/MedialAxis.hpp"
#include "libslic3r/MultiPoint.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Polyline.hpp"

#include <clipper/clipper.hpp>
#include "Orchestrator.hpp"

namespace Slic3r {
static PluginStorage *to_storage(storage_handle *storage) { return reinterpret_cast<PluginStorage *>(storage); }

static extrusion_entity_handle *to_handle(ExtrusionEntity *entity)
{
    return reinterpret_cast<extrusion_entity_handle *>(entity);
}

static Slic3r::Point to_point(c_point point) { return Slic3r::Point(point.x, point.y); }

static Slic3r::Point &as_point(c_point &point) { return reinterpret_cast<Slic3r::Point &>(point); }

static c_point to_c_point(const Slic3r::Point &point) {
    c_point out = {};
    out.x = point.x();
    out.y = point.y();
    return out;
}
static void set(c_point &out, const Slic3r::Point &point) {
    out.x = point.x();
    out.y = point.y();
}

c_matrix4d matrix4d_identity(void) {
    c_matrix4d out = {};
    out.value[0] = 1.0;
    out.value[5] = 1.0;
    out.value[10] = 1.0;
    out.value[15] = 1.0;
    return out;
}

c_matrix4d matrix4d_translation(double x, double y, double z) {
    c_matrix4d out = matrix4d_identity();
    out.value[3] = x;
    out.value[7] = y;
    out.value[11] = z;
    return out;
}

c_matrix4d matrix4d_mul(c_matrix4d lhs, c_matrix4d rhs) {
    c_matrix4d out = {};
    for (uint32_t row = 0; row < 4; ++row) {
        for (uint32_t col = 0; col < 4; ++col) {
            double value = 0.0;
            for (uint32_t k = 0; k < 4; ++k)
                value += lhs.value[row * 4 + k] * rhs.value[k * 4 + col];
            out.value[row * 4 + col] = value;
        }
    }
    return out;
}

static c_bounding_box to_c_bounding_box(const Slic3r::BoundingBox &box) {
    c_bounding_box out = {};
    out.min = to_c_point(box.min);
    out.max = to_c_point(box.max);
    return out;
}

static Slic3r::MultiPoint *to_multipoint(multipoint_handle *me) { return reinterpret_cast<Slic3r::MultiPoint *>(me); }

static const Slic3r::MultiPoint *to_multipoint(const multipoint_handle *me) {
    return reinterpret_cast<const Slic3r::MultiPoint *>(me);
}

static Slic3r::Polygon *to_polygon(polygon_handle *me) { return reinterpret_cast<Slic3r::Polygon *>(me); }

static const Slic3r::Polygon *to_polygon(const polygon_handle *me) {
    return reinterpret_cast<const Slic3r::Polygon *>(me);
}

static Slic3r::Polyline *to_polyline(polyline_handle *me) { return reinterpret_cast<Slic3r::Polyline *>(me); }

static const Slic3r::Polyline *to_polyline(const polyline_handle *me) {
    return reinterpret_cast<const Slic3r::Polyline *>(me);
}

static Slic3r::Polygons *to_polygons(polygon_collection_handle *me) {
    return reinterpret_cast<Slic3r::Polygons *>(me);
}

static const Slic3r::Polygons *to_polygons(const polygon_collection_handle *me) {
    return reinterpret_cast<const Slic3r::Polygons *>(me);
}

static Slic3r::Polylines *to_polylines(polyline_collection_handle *me) {
    return reinterpret_cast<Slic3r::Polylines *>(me);
}

static const Slic3r::Polylines *to_polylines(const polyline_collection_handle *me) {
    return reinterpret_cast<const Slic3r::Polylines *>(me);
}

static Slic3r::ExPolygon *to_expolygon(expolygon_handle *me) { return reinterpret_cast<Slic3r::ExPolygon *>(me); }

static const Slic3r::ExPolygon *to_expolygon(const expolygon_handle *me) {
    return reinterpret_cast<const Slic3r::ExPolygon *>(me);
}

static Slic3r::ExtrusionRole to_extrusion_role(raw_extrusion_role role)
{
    return Slic3r::ExtrusionRole(static_cast<Slic3r::ExtrusionRoleModifier>(role));
}

static Slic3r::Flow to_flow(c_flow flow)
{
    const float width = float(Slic3r::unscaled(flow.width));
    const float height = float(Slic3r::unscaled(flow.height));
    const float nozzle_diameter = float(Slic3r::unscaled(flow.nozzle_diameter));
    if (flow.is_bridge != 0)
        return Slic3r::Flow::bridging_flow(width, nozzle_diameter);
    return Slic3r::Flow::new_from_width(width, nozzle_diameter, height, flow.spacing_ratio);
}

static Slic3r::ExtrusionAttributes to_attributes(raw_extrusion_role role, c_flow flow)
{
    Slic3r::ExtrusionAttributes out;
    out.role = uint16_t(role);
    out.mm3_per_mm = flow.mm3_per_mm;
    out.width = float(Slic3r::unscaled(flow.width));
    out.height = float(Slic3r::unscaled(flow.height));
    out.no_seam = 0;
    return out;
}

static coord_t effective_or(coord_t value, coord_t fallback)
{
    return value > 0 ? value : fallback;
}

static bool medial_axis_input_is_valid(const ExPolygon &src, const c_medial_axis_extrusion_params &params)
{
    return !src.empty() &&
           params.min_medial_width > 0 &&
           params.max_medial_width >= params.min_medial_width &&
           params.flow.width > 0 &&
           params.flow.height > 0 &&
           params.flow.nozzle_diameter > 0;
}

static coord_t medial_axis_resolution(const c_medial_axis_extrusion_params &params)
{
    if (params.variable_width_resolution > SCALED_EPSILON)
        return params.variable_width_resolution;
    return std::max<coord_t>(params.flow.width / 4, SCALED_EPSILON * 2);
}

static void configure_medial_axis(Geometry::MedialAxis &medial_axis, const c_medial_axis_extrusion_params &params)
{
    /*
    The ABI exposes all optional medial-axis behaviors explicitly. The native
    MedialAxis object still has historical defaults, so the host wrapper applies
    every option here instead of relying on constructor defaults that are hard to
    discover from plugin code.
    */
    medial_axis.use_min_real_width(effective_or(params.min_extrusion_width, params.min_medial_width));
    medial_axis.set_biggest_width(effective_or(params.max_extrusion_width, params.max_medial_width));
    medial_axis.set_stop_at_min_width((params.flags & MEDIAL_AXIS_EXTRUSION_TRIM_THIN_ENDPOINTS) != 0);

    if (params.min_centerline_length > 0)
        medial_axis.set_min_length(params.min_centerline_length);
    if (params.endpoint_extension_length > 0)
        medial_axis.set_extension_length(params.endpoint_extension_length);
    if (params.endpoint_taper_length > 0)
        medial_axis.use_tapers(params.endpoint_taper_length);
    if (params.extension_area != nullptr)
        medial_axis.use_bounds(*to_expolygon(params.extension_area));
}

static bool entity_is_long_enough(const ExtrusionEntity &entity, coord_t min_extrusion_length)
{
    return min_extrusion_length <= 0 || entity.length() >= min_extrusion_length;
}

static std::unique_ptr<ExtrusionEntity> constant_width_entity_from_medial_axis(
    const ThickPolyline &polyline,
    const ExtrusionAttributes &attributes,
    bool can_reverse)
{
    /*
    Constant-width mode still uses the medial-axis centerline, but it discards
    the per-point width profile. This is useful for gap-fill-like plugins that
    want the skeleton topology without letting the output flow vary along a
    single line.
    */
    if (polyline.size() < 2)
        return nullptr;

    ArcPolyline centerline(polyline.points);
    return std::make_unique<ExtrusionPath>(std::move(centerline), attributes, ExtrusionPropertyUPtr{}, can_reverse);
}

static std::unique_ptr<ExtrusionEntity> build_medial_axis_tree(
    const ExPolygon &src,
    const c_medial_axis_extrusion_params &params)
{
    const bool can_reverse = (params.flags & MEDIAL_AXIS_EXTRUSION_CAN_REVERSE) != 0;
    Geometry::MedialAxis medial_axis(src, params.max_medial_width, params.min_medial_width, params.flow.height);
    configure_medial_axis(medial_axis, params);

    ThickPolylines centerlines;
    medial_axis.build(centerlines);

    std::unique_ptr<ExtrusionEntity> root = std::make_unique<ExtrusionEntity>(true);
    root->set_can_sort_reverse(true, true);

    if ((params.flags & MEDIAL_AXIS_EXTRUSION_CONSTANT_WIDTH) != 0) {
        const ExtrusionAttributes attributes = to_attributes(params.role, params.flow);
        for (const ThickPolyline &centerline : centerlines) {
            std::unique_ptr<ExtrusionEntity> child =
                constant_width_entity_from_medial_axis(centerline, attributes, can_reverse);
            if (child != nullptr && entity_is_long_enough(*child, params.min_extrusion_length))
                root->append_child(std::move(child));
        }
    } else {
        const Flow flow = to_flow(params.flow);
        const coord_t resolution = medial_axis_resolution(params);
        ExtrusionEntitiesPtr children = params.width_change_tolerance > 0 ?
            Geometry::thin_variable_width(centerlines, to_extrusion_role(params.role), flow, resolution, params.width_change_tolerance, can_reverse) :
            Geometry::thin_variable_width(centerlines, to_extrusion_role(params.role), flow, resolution, can_reverse);

        for (ExtrusionEntity *child : children) {
            std::unique_ptr<ExtrusionEntity> owned_child(child);
            if (owned_child != nullptr && entity_is_long_enough(*owned_child, params.min_extrusion_length))
                root->append_child(std::move(owned_child));
        }
        children.clear();
    }

    return root;
}

static extrusion_entity_handle *store_extrusion_entity(storage_handle *storage,
                                                       std::unique_ptr<ExtrusionEntity> entity)
{
    if (storage == nullptr || entity == nullptr)
        return nullptr;

    PluginStorage *plugin_storage = to_storage(storage);
    ExtrusionEntity &stored = plugin_storage->extrusions.push_back(std::move(entity));
    plugin_storage->generic_storage.insert(&stored);
    return to_handle(&stored);
}

static Slic3r::ExPolygons *to_expolygons(expolygon_collection_handle *me) {
    return reinterpret_cast<Slic3r::ExPolygons *>(me);
}

static const Slic3r::ExPolygons *to_expolygons(const expolygon_collection_handle *me) {
    return reinterpret_cast<const Slic3r::ExPolygons *>(me);
}

static Slic3r::Points to_points(const multipoint_view *me) {
    Slic3r::Points points;
    if (me != nullptr && me->array != nullptr && me->size > 0) {
        points.reserve(me->size);
        for (uint32_t i = 0; i < me->size; ++i)
            points.emplace_back(me->array[i].x, me->array[i].y);
    }
    return points;
}

static const Slic3r::Point *as_points(const c_point *points) {
    return reinterpret_cast<const Slic3r::Point*>(points);
}
}


extern "C" {

c_matrix4d matrix4d_identity(void) { return Slic3r::matrix4d_identity(); }

c_matrix4d matrix4d_translation(double x, double y, double z)
{
    return Slic3r::matrix4d_translation(x, y, z);
}

c_matrix4d matrix4d_mul(c_matrix4d lhs, c_matrix4d rhs)
{
    return Slic3r::matrix4d_mul(lhs, rhs);
}

distf_t c_point_distance_to(c_point lhs, c_point rhs) { return std::sqrt(c_point_distance_to_square(lhs, rhs)); }

distf_t c_point_distance_to_square(c_point lhs, c_point rhs) {
    coordf_t dx = double(lhs.x - rhs.x);
    coordf_t dy = double(lhs.y - rhs.y);
    return dx * dx + dy * dy;
}

multipoint_handle *polygon_as_multipoint(polygon_handle *me) {
    return reinterpret_cast<multipoint_handle *>(me);
}

const multipoint_handle *polygon_as_multipoint_const(const polygon_handle *me) {
    return reinterpret_cast<const multipoint_handle *>(me);
}

multipoint_handle *polyline_as_multipoint(polyline_handle *me) {
    return reinterpret_cast<multipoint_handle *>(me);
}

const multipoint_handle *polyline_as_multipoint_const(const polyline_handle *me) {
    return reinterpret_cast<const multipoint_handle *>(me);
}

/* ========================= point views ========================= */

int32_t points_is_valid(const multipoint_const_view *me) { return Slic3r::multipoint_is_valid(Slic3r::as_points(me->array), me->size); }

int32_t points_intersection(const multipoint_const_view *me, c_point line_a, c_point line_b, c_point *out_intersection) {
    return Slic3r::multipoint_intersection(Slic3r::as_points(me->array), me->size, Slic3r::as_point(line_a), Slic3r::as_point(line_b),
                                          reinterpret_cast<Slic3r::Point *>(out_intersection), me->flags & MULTIPOINT_CLOSED);
}

int32_t points_first_intersection(const multipoint_const_view *me,
                              c_point line_a,
                              c_point line_b,
                              c_point *out_intersection) {
    return Slic3r::multipoint_first_intersection(Slic3r::as_points(me->array), me->size, Slic3r::as_point(line_a), Slic3r::as_point(line_b),
                                         reinterpret_cast<Slic3r::Point *>(out_intersection), me->flags & MULTIPOINT_CLOSED);
}

uint32_t points_intersections(const multipoint_const_view *me,
                            c_point line_a,
                            c_point line_b,
                            multipoint_view *out_intersections) {
    Slic3r::Points intersections;
    bool result = Slic3r::multipoint_intersections(Slic3r::as_points(me->array), me->size, Slic3r::as_point(line_a), Slic3r::as_point(line_b),
                                           &intersections, me->flags & MULTIPOINT_CLOSED);
    if (!result)
        return 0;

    if (out_intersections != nullptr && out_intersections->array != nullptr) {
        for (uint32_t i = 0; i < intersections.size(); ++i) {
            if (i < out_intersections->size) {
                Slic3r::set(out_intersections->array[i], intersections[i]);
            }
        }
    }

    return intersections.size();
}

c_bounding_box points_bounding_box(const multipoint_const_view *me) {
    return Slic3r::to_c_bounding_box(Slic3r::multipoint_bounding_box(Slic3r::as_points(me->array), me->size));
}

int32_t points_find_point_index(const multipoint_const_view *me, c_point point_search, coord_t max_distance) {
    if (me == nullptr)
        return -1;
    return Slic3r::multipoint_find_point_index(Slic3r::as_points(me->array), me->size, Slic3r::as_point(point_search), coordf_t(max_distance));
}

int32_t points_closest_point_index(const multipoint_const_view *me, c_point point_search) {
    if(me == nullptr)
        return -1;
    return Slic3r::multipoint_closest_point_index(Slic3r::as_points(me->array), me->size, Slic3r::as_point(point_search));
}

/* ========================= multipoint ========================= */

uint32_t multipoint_size(const multipoint_handle *me) {
    return me == nullptr ? 0 : Slic3r::to_multipoint(me)->size();
}

distf_t multipoint_length(const multipoint_handle *me) {
    return me == nullptr ? 0.0 : Slic3r::length(Slic3r::to_multipoint(me)->begin(), Slic3r::to_multipoint(me)->end());
}

int32_t multipoint_valid(const multipoint_handle *me) {
    return me == nullptr ? 0 : Slic3r::to_multipoint(me)->is_valid();
}

c_point multipoint_get(const multipoint_handle *me, uint32_t idx) {
    c_point out = {};
    if (me == nullptr || idx >= Slic3r::to_multipoint(me)->size())
        return out;
    out = Slic3r::to_c_point((*Slic3r::to_multipoint(me))[idx]);
    return out;
}

int32_t multipoint_set(multipoint_handle *me, uint32_t idx, c_point new_value) {
    if (me == nullptr || idx >= Slic3r::to_multipoint(me)->size())
        return 0;
    (*Slic3r::to_multipoint(me))[idx] = Slic3r::to_point(new_value);
    return 1;
}

int32_t multipoint_equals(const multipoint_handle *me, const multipoint_handle *other) {
    if (me == other)
        return 1;
    if (me == nullptr || other == nullptr)
        return 0;
    return *Slic3r::to_multipoint(me) == *Slic3r::to_multipoint(other);
}

multipoint_view multipoint_view_mut(multipoint_handle *me) {
    multipoint_view out = {};
    if (me == nullptr)
        return out;
    out.array = reinterpret_cast<c_point*>(Slic3r::to_multipoint(me)->data());
    out.size  = Slic3r::to_multipoint(me)->size();
    out.flags = Slic3r::to_multipoint(me)->is_loop() ? MULTIPOINT_CLOSED : 0u;
    return out;
}

multipoint_const_view multipoint_view_const(const multipoint_handle *me) {
    multipoint_const_view out = {};
    if (me == nullptr)
        return out;
    out.array = reinterpret_cast<const c_point*>(Slic3r::to_multipoint(me)->data());
    out.size  = Slic3r::to_multipoint(me)->size();
    out.flags = Slic3r::to_multipoint(me)->is_loop() ? MULTIPOINT_CLOSED : 0u;
    return out;
}

int32_t multipoint_is_cw(multipoint_view *me) {
    // a bit inneficient, do better
    Slic3r::Points pts = Slic3r::to_points(me);
    Slic3r::Polygon poly(pts);
    bool is_ccw = poly.is_counter_clockwise();
    if (is_ccw) {
        me->flags |= MULTIPOINT_CCW;
        me->flags &= ~MULTIPOINT_CW;
        return 0;
    } else {
        me->flags |= MULTIPOINT_CW;
        me->flags &= ~MULTIPOINT_CCW;
        return 1;
    }
}

void multipoint_push_back(multipoint_handle *me, c_point point) {
    if (me == nullptr)
        return;
    Slic3r::to_multipoint(me)->push_back(Slic3r::to_point(point));
}

void multipoint_insert(multipoint_handle *me, uint32_t idx, c_point point) {
    if (me == nullptr)
        return;
    Slic3r::MultiPoint *mp = Slic3r::to_multipoint(me);
    idx = std::min(idx, uint32_t(mp->size()));
    mp->insert(mp->begin() + idx, Slic3r::to_point(point));
}

void multipoint_insert_array(multipoint_handle *me, uint32_t idx, const c_point *array, uint32_t array_size)
{
    if (me == nullptr || array == nullptr || array_size == 0)
        return;

    Slic3r::MultiPoint *mp = Slic3r::to_multipoint(me);
    const size_t old_size = mp->size();
    const size_t insert_idx = std::min<size_t>(idx, old_size);

    mp->resize(old_size + array_size);
    std::move_backward(mp->begin() + insert_idx, mp->begin() + old_size, mp->end());

    for (uint32_t i = 0; i < array_size; ++i)
        (*mp)[insert_idx + i] = Slic3r::Point(array[i].x, array[i].y);
}

void multipoint_pop_back(multipoint_handle *me) {
    if (me != nullptr && !Slic3r::to_multipoint(me)->empty())
        Slic3r::to_multipoint(me)->pop_back();
}

void multipoint_clear(multipoint_handle *me) {
    if (me != nullptr)
        Slic3r::to_multipoint(me)->clear();
}

void multipoint_erase(multipoint_handle *me, uint32_t begin_idx, uint32_t erase_size) {
    if (me == nullptr || erase_size == 0)
        return;
    Slic3r::MultiPoint *mp = Slic3r::to_multipoint(me);
    if (begin_idx >= mp->size())
        return;
    const uint32_t end_idx = std::min(begin_idx + erase_size, uint32_t(mp->size()));
    mp->erase(mp->begin() + begin_idx, mp->begin() + end_idx);
}

void multipoint_copy(multipoint_handle *dst, const multipoint_handle *src) {
    if (dst != nullptr && src != nullptr)
        *Slic3r::to_multipoint(dst) = *Slic3r::to_multipoint(src);
}

void multipoint_scale(multipoint_handle *me, double factor) {
    if (me != nullptr)
        Slic3r::to_multipoint(me)->scale(factor);
}

void multipoint_scale_xy(multipoint_handle *me, double factor_x, double factor_y) {
    if (me != nullptr)
        Slic3r::to_multipoint(me)->scale(factor_x, factor_y);
}

void multipoint_translate(multipoint_handle *me, double x, double y) {
    if (me != nullptr)
        Slic3r::to_multipoint(me)->translate(x, y);
}

void multipoint_rotate_origin(multipoint_handle *me, double angle) {
    if (me != nullptr)
        Slic3r::to_multipoint(me)->rotate(angle);
}

void multipoint_rotate_around(multipoint_handle *me, double angle, c_point center) {
    if (me != nullptr)
        Slic3r::to_multipoint(me)->rotate(angle, Slic3r::to_point(center));
}

void multipoint_reverse(multipoint_handle *me) {
    if (me != nullptr)
        Slic3r::to_multipoint(me)->reverse();
}

void multipoint_densify(multipoint_handle *me, coord_t min_length) {
    if (me == nullptr)
        return;
        Slic3r::to_multipoint(me)->densify(distf_t(min_length));
}

void multipoint_simplify(multipoint_handle *me, coord_t min_length, coord_t min_deviation) {
    if (me == nullptr)
        return;
    Slic3r::to_multipoint(me)->douglas_peucker(min_length);
    (void) min_deviation;
}

void multipoint_ensure_valid(multipoint_handle *me, coord_t resolution) {
    if (me != nullptr)
        Slic3r::to_multipoint(me)->douglas_peucker(resolution);
}

/* ========================= polygon ========================= */

int32_t multipoint_is_polygon(const multipoint_handle *me) {
    if (me == nullptr)
        return false;
    return Slic3r::to_multipoint(me)->is_polygon();
}

polygon_handle *storage_new_polygon(storage_handle *me) {
    Slic3r::PluginStorage *storage = reinterpret_cast<Slic3r::PluginStorage *>(me);
    polygon_handle *out_new_object = reinterpret_cast<polygon_handle *>(&storage->polygons.emplace_back());
    storage->generic_storage.insert(out_new_object);
    return out_new_object;
}

int32_t polygon_valid(const polygon_handle *me) {
    return me == nullptr ? 0 : Slic3r::to_polygon(me)->is_valid();
}

double polygon_area(const polygon_handle *me) {
    return me == nullptr ? 0.0 : Slic3r::to_polygon(me)->area();
}

int32_t polygon_is_counter_clockwise(const polygon_handle *me) {
    return me == nullptr ? 0 : Slic3r::to_polygon(me)->is_counter_clockwise();
}

int32_t polygon_is_clockwise(const polygon_handle *me) {
    return me == nullptr ? 0 : Slic3r::to_polygon(me)->is_clockwise();
}

int32_t polygon_make_counter_clockwise(polygon_handle *me) {
    return me == nullptr ? 0 : Slic3r::to_polygon(me)->make_counter_clockwise();
}

int32_t polygon_make_clockwise(polygon_handle *me) {
    return me == nullptr ? 0 : Slic3r::to_polygon(me)->make_clockwise();
}

int32_t polygon_is_valid(const polygon_handle *me) {
    return me == nullptr ? 0 : Slic3r::to_polygon(me)->is_valid();
}

int32_t polygon_contains(const polygon_handle *me, c_point point) {
    return me == nullptr ? 0 : Slic3r::to_polygon(me)->contains(Slic3r::to_point(point));
}

int32_t polygon_on_boundary(const polygon_handle *me, c_point point, coord_t max_dist) {
    return me == nullptr ? 0 : Slic3r::to_polygon(me)->on_boundary(Slic3r::to_point(point), double(max_dist));
}

c_point polygon_centroid(const polygon_handle *me) {
    c_point out = {};
    if (me != nullptr)
        out = Slic3r::to_c_point(Slic3r::to_polygon(me)->centroid());
    return out;
}

uint32_t polygon_concave_points_idx(
    const polygon_handle *me, double min_angle, double max_angle, uint32_t *out, uint32_t max_size) {
    if (me == nullptr)
        return 0;
    const std::vector<size_t> idxs = Slic3r::to_polygon(me)->concave_points_idx(min_angle, max_angle);
    if (out != nullptr) {
        const uint32_t count = std::min(max_size, uint32_t(idxs.size()));
        for (uint32_t i = 0; i < count; ++i)
            out[i] = uint32_t(idxs[i]);
    }
    return idxs.size();
}

uint32_t polygon_convex_points_idx(
    const polygon_handle *me, double min_angle, double max_angle, uint32_t *out, uint32_t max_size) {
    if (me == nullptr)
        return 0;
    const std::vector<size_t> idxs = Slic3r::to_polygon(me)->convex_points_idx(min_angle, max_angle);
    if (out != nullptr) {
        const uint32_t count = std::min(max_size, uint32_t(idxs.size()));
        for (uint32_t i = 0; i < count; ++i)
            out[i] = uint32_t(idxs[i]);
    }
    return idxs.size();
}

c_point polygon_point_projection(const polygon_handle *me, c_point point, uint32_t *out_idx) {
    c_point out = {};
    if (me == nullptr)
        return out;
    const std::pair<Slic3r::Point, uint32_t> projection = Slic3r::to_polygon(me)->point_projection(Slic3r::to_point(point));
    if (out_idx != nullptr)
        *out_idx = projection.second;
    out = Slic3r::to_c_point(projection.first);
    return out;
}

void polygon_move(polygon_handle *dst, polygon_handle *src) {
    if (dst == nullptr || src == nullptr || dst == src)
        return;
    *Slic3r::to_polygon(dst) = std::move(*Slic3r::to_polygon(src));
    Slic3r::to_polygon(src)->clear();
}

/* ========================= polyline ========================= */
int32_t multipoint_is_polyline(const multipoint_handle *me) {
    if (me == nullptr)
        return false;
    return Slic3r::to_multipoint(me)->is_polyline();
}

polyline_handle *storage_new_polyline(storage_handle *me) {
    Slic3r::PluginStorage *storage = reinterpret_cast<Slic3r::PluginStorage *>(me);
    polyline_handle *out_new_object = reinterpret_cast<polyline_handle *>(&storage->polylines.emplace_back());
    storage->generic_storage.insert(out_new_object);
    return out_new_object;
}

int32_t polyline_valid(const polyline_handle *me) {
    return me == nullptr ? 0 : Slic3r::to_polyline(me)->is_valid();
}

void polyline_clip_end(polyline_handle *me, distf_t distance) {
    if (me != nullptr)
        Slic3r::to_polyline(me)->clip_end(distance);
}

void polyline_clip_start(polyline_handle *me, distf_t distance) {
    if (me != nullptr)
        Slic3r::to_polyline(me)->clip_start(distance);
}

void polyline_extend_end(polyline_handle *me, distf_t distance) {
    if (me != nullptr)
        Slic3r::to_polyline(me)->extend_end(distance);
}

void polyline_extend_start(polyline_handle *me, distf_t distance) {
    if (me != nullptr)
        Slic3r::to_polyline(me)->extend_start(distance);
}

void polyline_move(polyline_handle *dst, polyline_handle *src) {
    if (dst == nullptr || src == nullptr || dst == src)
        return;
    *Slic3r::to_polyline(dst) = std::move(*Slic3r::to_polyline(src));
    Slic3r::to_polyline(src)->clear();
}

int32_t polygon_split(const polygon_handle *me, uint32_t index, polyline_handle *out) {
    if (me == nullptr || out == nullptr)
        return 0;
    *Slic3r::to_polyline(out) = Slic3r::to_polygon(me)->split_at_index(index);
    return 1;
}

int32_t polyline_close(const polyline_handle *me, polygon_handle *out) {
    if (me == nullptr || out == nullptr)
        return 0;
    *Slic3r::to_polygon(out) = Slic3r::Polygon(Slic3r::to_polyline(me)->points);
    return Slic3r::to_polygon(out)->is_valid();
}

/* ========================= polygon collection ========================= */

polygon_collection_handle *storage_new_polygons(storage_handle *me) {
    Slic3r::PluginStorage *storage = reinterpret_cast<Slic3r::PluginStorage *>(me);
    polygon_collection_handle *out_new_object =
        reinterpret_cast<polygon_collection_handle *>(&storage->polygon_collections.emplace_back());
    storage->generic_storage.insert(out_new_object);
    return out_new_object;
}

int32_t polygons_valid(const polygon_collection_handle *me) {
    if (me == nullptr)
        return 0;
    for (const Slic3r::Polygon &poly : *Slic3r::to_polygons(me)) {
        if (!poly.is_valid())
            return 0;
    }
    return 1;
}

uint32_t polygons_size(const polygon_collection_handle *me) {
    return me == nullptr ? 0 : Slic3r::to_polygons(me)->size();
}

polygon_handle *polygons_at(polygon_collection_handle *me, uint32_t idx) {
    if (me == nullptr || idx >= Slic3r::to_polygons(me)->size())
        return NULL;
    return reinterpret_cast<polygon_handle*>(&(*Slic3r::to_polygons(me))[idx]);
}

const polygon_handle *polygons_at_const(const polygon_collection_handle *me, uint32_t idx) {
    if (me == nullptr || idx >= Slic3r::to_polygons(me)->size())
        return NULL;
    return reinterpret_cast<const polygon_handle*>(&(*Slic3r::to_polygons(me))[idx]);
}

int32_t polygons_equals(const polygon_collection_handle *me, const polygon_collection_handle *other) {
    if (me == other)
        return 1;
    if (me == nullptr || other == nullptr)
        return 0;
    return *Slic3r::to_polygons(me) == *Slic3r::to_polygons(other);
}

void polygons_resize(polygon_collection_handle *me, uint32_t new_size) {
    if (me != nullptr)
        Slic3r::to_polygons(me)->resize(new_size);
}

void polygons_emplace_back(polygon_collection_handle *me) {
    if (me != nullptr)
        Slic3r::to_polygons(me)->emplace_back();
}

void polygons_pop_back(polygon_collection_handle *me) {
    if (me != nullptr && !Slic3r::to_polygons(me)->empty())
        Slic3r::to_polygons(me)->pop_back();
}

void polygons_clear(polygon_collection_handle *me) {
    if (me != nullptr)
        Slic3r::to_polygons(me)->clear();
}

void polygons_erase(polygon_collection_handle *me, uint32_t begin_idx, uint32_t erase_size) {
    if (me == nullptr || erase_size == 0)
        return;
    Slic3r::Polygons *polys = Slic3r::to_polygons(me);
    if (begin_idx >= polys->size())
        return;
    const uint32_t end_idx = std::min(begin_idx + erase_size, static_cast<uint32_t>(polys->size()));
    polys->erase(polys->begin() + begin_idx, polys->begin() + end_idx);
}

void polygons_insert_copy(polygon_collection_handle *dst, uint32_t idx, const polygon_handle *src) {
    if (dst == nullptr || src == nullptr)
        return;
    Slic3r::Polygons *polys = Slic3r::to_polygons(dst);
    idx = std::min(idx, static_cast<uint32_t>(polys->size()));
    Slic3r::Polygon value = *Slic3r::to_polygon(src);
    polys->insert(polys->begin() + idx, std::move(value));
}

void polygons_append_copy(polygon_collection_handle *dst, const polygon_collection_handle *src) {
    if (dst == nullptr || src == nullptr)
        return;
    Slic3r::Polygons *dst_polys = Slic3r::to_polygons(dst);
    const Slic3r::Polygons *src_polys = Slic3r::to_polygons(src);
    dst_polys->insert(dst_polys->end(), src_polys->begin(), src_polys->end());
}

void polygons_insert_move(polygon_collection_handle *dst, uint32_t idx, polygon_handle *src) {
    if (dst == nullptr || src == nullptr)
        return;
    Slic3r::Polygons *polys = Slic3r::to_polygons(dst);
    idx = std::min(idx, static_cast<uint32_t>(polys->size()));
    Slic3r::Polygon value = std::move(*Slic3r::to_polygon(src));
    Slic3r::to_polygon(src)->clear();
    polys->insert(polys->begin() + idx, std::move(value));
}

void polygons_append_move(polygon_collection_handle *dst, polygon_collection_handle *src) {
    if (dst == nullptr || src == nullptr || dst == src)
        return;
    Slic3r::Polygons *dst_polys = Slic3r::to_polygons(dst);
    Slic3r::Polygons *src_polys = Slic3r::to_polygons(src);
    dst_polys->insert(dst_polys->end(),
                      std::make_move_iterator(src_polys->begin()),
                      std::make_move_iterator(src_polys->end()));
    src_polys->clear();
}

void polygons_copy(polygon_collection_handle *dst, const polygon_collection_handle *src) {
    if (dst != nullptr && src != nullptr)
        *Slic3r::to_polygons(dst) = *Slic3r::to_polygons(src);
}

void polygons_move(polygon_collection_handle *dst, polygon_collection_handle *src) {
    if (dst == nullptr || src == nullptr || dst == src)
        return;
    *Slic3r::to_polygons(dst) = std::move(*Slic3r::to_polygons(src));
    Slic3r::to_polygons(src)->clear();
}

int32_t polygons_on_boundary(const polygon_collection_handle *me, c_point point, coord_t max_distance) {
    if (me == nullptr)
        return 0;
    const Slic3r::Point search = Slic3r::to_point(point);
    const double max_distance_d = double(max_distance);
    for (const Slic3r::Polygon &poly : *Slic3r::to_polygons(me)) {
        if ((poly.point_projection(search).first - search).cast<double>().norm() <= max_distance_d)
            return 1;
    }
    return 0;
}

c_point polygons_point_projection(const polygon_collection_handle *me, c_point point) {
    c_point out = {};
    if (me == nullptr)
        return out;
    const Slic3r::Point search = Slic3r::to_point(point);
    bool found = false;
    double best = 0.;
    Slic3r::Point best_point;
    for (const Slic3r::Polygon &poly : *Slic3r::to_polygons(me)) {
        const Slic3r::Point projection = poly.point_projection(search).first;
        const double dist = (projection - search).cast<double>().norm();
        if (!found || dist < best) {
            found = true;
            best = dist;
            best_point = projection;
        }
    }
    if (found)
        out = Slic3r::to_c_point(best_point);
    return out;
}

/* ========================= polyline collection ========================= */

polyline_collection_handle *storage_new_polylines(storage_handle *me) {
    Slic3r::PluginStorage *storage = reinterpret_cast<Slic3r::PluginStorage *>(me);
    polyline_collection_handle *out_new_object =
        reinterpret_cast<polyline_collection_handle *>(&storage->polyline_collections.emplace_back());
    storage->generic_storage.insert(out_new_object);
    return out_new_object;
}

int32_t polylines_valid(const polyline_collection_handle *me) {
    if (me == nullptr)
        return 0;
    for (const Slic3r::Polyline &polyline : *Slic3r::to_polylines(me)) {
        if (!polyline.is_valid())
            return 0;
    }
    return 1;
}

uint32_t polylines_size(const polyline_collection_handle *me) {
    return me == nullptr ? 0 : Slic3r::to_polylines(me)->size();
}

polyline_handle *polylines_at(polyline_collection_handle *me, uint32_t idx) {
    if (me == nullptr || idx >= Slic3r::to_polylines(me)->size())
        return NULL;
    return reinterpret_cast<polyline_handle*>(&(*Slic3r::to_polylines(me))[idx]);
}

const polyline_handle *polylines_at_const(const polyline_collection_handle *me, uint32_t idx) {
    if (me == nullptr || idx >= Slic3r::to_polylines(me)->size())
        return NULL;
    return reinterpret_cast<const polyline_handle*>(&(*Slic3r::to_polylines(me))[idx]);
}

int32_t polylines_equals(const polyline_collection_handle *me, const polyline_collection_handle *other) {
    if (me == other)
        return 1;
    if (me == nullptr || other == nullptr)
        return 0;
    return *Slic3r::to_polylines(me) == *Slic3r::to_polylines(other);
}

void polylines_resize(polyline_collection_handle *me, uint32_t new_size) {
    if (me != nullptr)
        Slic3r::to_polylines(me)->resize(new_size);
}

void polylines_emplace_back(polyline_collection_handle *me) {
    if (me != nullptr)
        Slic3r::to_polylines(me)->emplace_back();
}

void polylines_pop_back(polyline_collection_handle *me) {
    if (me != nullptr && !Slic3r::to_polylines(me)->empty())
        Slic3r::to_polylines(me)->pop_back();
}

void polylines_clear(polyline_collection_handle *me) {
    if (me != nullptr)
        Slic3r::to_polylines(me)->clear();
}

void polylines_erase(polyline_collection_handle *me, uint32_t begin_idx, uint32_t erase_size) {
    if (me == nullptr || erase_size == 0)
        return;
    Slic3r::Polylines *polylines = Slic3r::to_polylines(me);
    if (begin_idx >= polylines->size())
        return;
    const uint32_t end_idx = std::min(begin_idx + erase_size, static_cast<uint32_t>(polylines->size()));
    polylines->erase(polylines->begin() + begin_idx, polylines->begin() + end_idx);
}

void polylines_insert_copy(polyline_collection_handle *dst, uint32_t idx, const polyline_handle *src) {
    if (dst == nullptr || src == nullptr)
        return;
    Slic3r::Polylines *polylines = Slic3r::to_polylines(dst);
    idx = std::min(idx, static_cast<uint32_t>(polylines->size()));
    Slic3r::Polyline value = *Slic3r::to_polyline(src);
    polylines->insert(polylines->begin() + idx, std::move(value));
}

void polylines_append_copy(polyline_collection_handle *dst, const polyline_collection_handle *src) {
    if (dst == nullptr || src == nullptr)
        return;
    Slic3r::Polylines *dst_polylines = Slic3r::to_polylines(dst);
    const Slic3r::Polylines *src_polylines = Slic3r::to_polylines(src);
    dst_polylines->insert(dst_polylines->end(), src_polylines->begin(), src_polylines->end());
}

void polylines_insert_move(polyline_collection_handle *dst, uint32_t idx, polyline_handle *src) {
    if (dst == nullptr || src == nullptr)
        return;
    Slic3r::Polylines *polylines = Slic3r::to_polylines(dst);
    idx = std::min(idx, static_cast<uint32_t>(polylines->size()));
    Slic3r::Polyline value = std::move(*Slic3r::to_polyline(src));
    Slic3r::to_polyline(src)->clear();
    polylines->insert(polylines->begin() + idx, std::move(value));
}

void polylines_append_move(polyline_collection_handle *dst, polyline_collection_handle *src) {
    if (dst == nullptr || src == nullptr || dst == src)
        return;
    Slic3r::Polylines *dst_polylines = Slic3r::to_polylines(dst);
    Slic3r::Polylines *src_polylines = Slic3r::to_polylines(src);
    dst_polylines->insert(dst_polylines->end(),
                          std::make_move_iterator(src_polylines->begin()),
                          std::make_move_iterator(src_polylines->end()));
    src_polylines->clear();
}

void polylines_copy(polyline_collection_handle *dst, const polyline_collection_handle *src) {
    if (dst != nullptr && src != nullptr)
        *Slic3r::to_polylines(dst) = *Slic3r::to_polylines(src);
}

void polylines_move(polyline_collection_handle *dst, polyline_collection_handle *src) {
    if (dst == nullptr || src == nullptr || dst == src)
        return;
    *Slic3r::to_polylines(dst) = std::move(*Slic3r::to_polylines(src));
    Slic3r::to_polylines(src)->clear();
}

/* ========================= expolygon ========================= */

expolygon_status expolygon_valid(const expolygon_handle *me) {
    if (me == nullptr || Slic3r::to_expolygon(me)->empty())
        return EXPOLYGON_STATUS_EMPTY;
    const Slic3r::ExPolygon *expoly = Slic3r::to_expolygon(me);
    if (!expoly->contour.is_valid())
        return EXPOLYGON_STATUS_INVALID_GEOMETRY;
    if (!expoly->contour.is_counter_clockwise())
        return EXPOLYGON_STATUS_INVALID_ORIENTATION;
    for (const Slic3r::Polygon &hole : expoly->holes) {
        if (!hole.is_valid())
            return EXPOLYGON_STATUS_INVALID_GEOMETRY;
        if (!hole.is_clockwise())
            return EXPOLYGON_STATUS_INVALID_ORIENTATION;
    }
    return EXPOLYGON_STATUS_OK;
}

void expolygon_ensure_valid(expolygon_handle *me, coord_t resolution) {
    if (me == nullptr)
        return;
    Slic3r::ExPolygons tmp;
    tmp.emplace_back(std::move(*Slic3r::to_expolygon(me)));
    Slic3r::ensure_valid(tmp, resolution);
    if (tmp.empty())
        Slic3r::to_expolygon(me)->clear();
    else
        *Slic3r::to_expolygon(me) = std::move(tmp.front());
}

expolygon_handle *storage_new_expolygon(storage_handle *me) {
    Slic3r::PluginStorage *storage = reinterpret_cast<Slic3r::PluginStorage *>(me);
    expolygon_handle *out_new_object = reinterpret_cast<expolygon_handle *>(&storage->expolygons.emplace_back());
    storage->generic_storage.insert(out_new_object);
    return out_new_object;
}

polygon_handle *expolygon_contour(expolygon_handle *me) {
    return me == nullptr ? NULL : reinterpret_cast<polygon_handle*>(&Slic3r::to_expolygon(me)->contour);
}

const polygon_handle *expolygon_contour_const(const expolygon_handle *me) {
    return me == nullptr ? NULL : reinterpret_cast<const polygon_handle*>(&Slic3r::to_expolygon(me)->contour);
}

uint32_t expolygon_hole_size(const expolygon_handle *me) {
    return me == nullptr ? 0 : Slic3r::to_expolygon(me)->holes.size();
}

polygon_handle *expolygon_hole_at(expolygon_handle *me, uint32_t hole_idx) {
    if (me == nullptr || hole_idx >= Slic3r::to_expolygon(me)->holes.size())
        return NULL;
    return reinterpret_cast<polygon_handle*>(&Slic3r::to_expolygon(me)->holes[hole_idx]);
}

const polygon_handle *expolygon_hole_at_const(const expolygon_handle *me, uint32_t hole_idx) {
    if (me == nullptr || hole_idx >= Slic3r::to_expolygon(me)->holes.size())
        return NULL;
    return reinterpret_cast<const polygon_handle*>(&Slic3r::to_expolygon(me)->holes[hole_idx]);
}

int32_t expolygon_equals(const expolygon_handle *me, const expolygon_handle *other) {
    if (me == other)
        return 1;
    if (me == nullptr || other == nullptr)
        return 0;
    return *Slic3r::to_expolygon(me) == *Slic3r::to_expolygon(other);
}

void expolygon_holes_resize(expolygon_handle *me, uint32_t new_size) {
    if (me != nullptr)
        Slic3r::to_expolygon(me)->holes.resize(new_size);
}

void expolygon_holes_emplace_back(expolygon_handle *me) {
    if (me != nullptr)
        Slic3r::to_expolygon(me)->holes.emplace_back();
}

void expolygon_holes_pop_back(expolygon_handle *me) {
    if (me != nullptr && !Slic3r::to_expolygon(me)->holes.empty())
        Slic3r::to_expolygon(me)->holes.pop_back();
}

void expolygon_holes_clear(expolygon_handle *me) {
    if (me != nullptr)
        Slic3r::to_expolygon(me)->holes.clear();
}

void expolygon_holes_erase(expolygon_handle *me, uint32_t begin_idx, uint32_t erase_size) {
    if (me == nullptr || erase_size == 0)
        return;
    Slic3r::Polygons &holes = Slic3r::to_expolygon(me)->holes;
    if (begin_idx >= holes.size())
        return;
    const uint32_t end_idx = std::min(begin_idx + erase_size, uint32_t(holes.size()));
    holes.erase(holes.begin() + begin_idx, holes.begin() + end_idx);
}

void expolygon_copy(expolygon_handle *dst, const expolygon_handle *src) {
    if (dst != nullptr && src != nullptr)
        *Slic3r::to_expolygon(dst) = *Slic3r::to_expolygon(src);
}

void expolygon_move(expolygon_handle *dst, expolygon_handle *src) {
    if (dst == nullptr || src == nullptr || dst == src)
        return;
    *Slic3r::to_expolygon(dst) = std::move(*Slic3r::to_expolygon(src));
    Slic3r::to_expolygon(src)->clear();
}

double expolygon_area(const expolygon_handle *me) {
    return me == nullptr ? 0.0 : Slic3r::to_expolygon(me)->area();
}

int32_t expolygon_contains(const expolygon_handle *me, c_point point) {
    return me == nullptr ? 0 : Slic3r::to_expolygon(me)->contains(Slic3r::to_point(point));
}

int32_t expolygon_overlaps(const expolygon_handle *me, const expolygon_handle *other) {
    return (me == nullptr || other == nullptr) ? 0 : Slic3r::to_expolygon(me)->overlaps(*Slic3r::to_expolygon(other));
}

void expolygon_to_polygons(const expolygon_handle *src, polygon_collection_handle *dst) {
    if (src == nullptr || dst == nullptr)
        return;
    Slic3r::Polygons &out = *Slic3r::to_polygons(dst);
    out.clear();
    out.reserve(Slic3r::to_expolygon(src)->holes.size() + 1);
    out.emplace_back(Slic3r::to_expolygon(src)->contour);
    for (const Slic3r::Polygon &hole : Slic3r::to_expolygon(src)->holes)
        out.emplace_back(hole);
}

expolygon_status polygons_to_expolygon(const polygon_collection_handle *src, expolygon_handle *dst) {
    if (src == nullptr || dst == nullptr || Slic3r::to_polygons(src)->empty())
        return EXPOLYGON_STATUS_EMPTY;
    Slic3r::ExPolygon &out = *Slic3r::to_expolygon(dst);
    out.clear();
    out.contour = (*Slic3r::to_polygons(src))[0];
    for (uint32_t i = 1; i < Slic3r::to_polygons(src)->size(); ++i)
        out.holes.emplace_back((*Slic3r::to_polygons(src))[i]);
    return expolygon_valid(dst);
}

/* ========================= expolygon collection ========================= */

expolygon_status expolygons_valid(const expolygon_collection_handle *me) {
    if (me == nullptr)
        return EXPOLYGON_STATUS_EMPTY;
    for (const Slic3r::ExPolygon &expoly : *Slic3r::to_expolygons(me)) {
        const expolygon_status status = expoly.empty() ? EXPOLYGON_STATUS_EMPTY :
            (!expoly.contour.is_valid() ? EXPOLYGON_STATUS_INVALID_GEOMETRY :
            (!expoly.contour.is_counter_clockwise() ? EXPOLYGON_STATUS_INVALID_ORIENTATION : EXPOLYGON_STATUS_OK));
        if (status != EXPOLYGON_STATUS_OK)
            return status;
        for (const Slic3r::Polygon &hole : expoly.holes) {
            if (!hole.is_valid())
                return EXPOLYGON_STATUS_INVALID_GEOMETRY;
            if (!hole.is_clockwise())
                return EXPOLYGON_STATUS_INVALID_ORIENTATION;
        }
    }
    return EXPOLYGON_STATUS_OK;
}

void expolygons_ensure_valid(expolygon_collection_handle *me, coord_t resolution) {
    if (me != nullptr)
        Slic3r::ensure_valid(*Slic3r::to_expolygons(me), resolution);
}

expolygon_collection_handle *storage_new_expolygons(storage_handle *me) {
    Slic3r::PluginStorage *storage = reinterpret_cast<Slic3r::PluginStorage *>(me);
    expolygon_collection_handle *out_new_object =
        reinterpret_cast<expolygon_collection_handle *>(&storage->expolygon_collections.emplace_back());
    storage->generic_storage.insert(out_new_object);
    return out_new_object;
}

uint32_t expolygons_size(const expolygon_collection_handle *me) {
    return me == nullptr ? 0 : Slic3r::to_expolygons(me)->size();
}

expolygon_handle *expolygons_at(expolygon_collection_handle *me, uint32_t i) {
    if (me == nullptr || i >= Slic3r::to_expolygons(me)->size())
        return NULL;
    return reinterpret_cast<expolygon_handle*>(&(*Slic3r::to_expolygons(me))[i]);
}

const expolygon_handle *expolygons_at_const(const expolygon_collection_handle *me, uint32_t i) {
    if (me == nullptr || i >= Slic3r::to_expolygons(me)->size())
        return NULL;
    return reinterpret_cast<const expolygon_handle*>(&(*Slic3r::to_expolygons(me))[i]);
}

int32_t expolygons_equals(const expolygon_collection_handle *me, const expolygon_collection_handle *other) {
    if (me == other)
        return 1;
    if (me == nullptr || other == nullptr)
        return 0;
    return *Slic3r::to_expolygons(me) == *Slic3r::to_expolygons(other);
}

void expolygons_resize(expolygon_collection_handle *me, uint32_t new_size) {
    if (me != nullptr)
        Slic3r::to_expolygons(me)->resize(new_size);
}

void expolygons_emplace_back(expolygon_collection_handle *me) {
    if (me != nullptr)
        Slic3r::to_expolygons(me)->emplace_back();
}

void expolygons_pop_back(expolygon_collection_handle *me) {
    if (me != nullptr && !Slic3r::to_expolygons(me)->empty())
        Slic3r::to_expolygons(me)->pop_back();
}

void expolygons_clear(expolygon_collection_handle *me) {
    if (me != nullptr)
        Slic3r::to_expolygons(me)->clear();
}

void expolygons_erase(expolygon_collection_handle *me, uint32_t begin_idx, uint32_t erase_size) {
    if (me == nullptr || erase_size == 0)
        return;
    Slic3r::ExPolygons *expolys = Slic3r::to_expolygons(me);
    if (begin_idx >= expolys->size())
        return;
    const uint32_t end_idx = std::min(begin_idx + erase_size, uint32_t(expolys->size()));
    expolys->erase(expolys->begin() + begin_idx, expolys->begin() + end_idx);
}

void expolygons_insert_copy(expolygon_collection_handle *dst, uint32_t idx, const expolygon_handle *src) {
    if (dst == nullptr || src == nullptr)
        return;
    Slic3r::ExPolygons *expolys = Slic3r::to_expolygons(dst);
    idx = std::min(idx, static_cast<uint32_t>(expolys->size()));
    Slic3r::ExPolygon value = *Slic3r::to_expolygon(src);
    expolys->insert(expolys->begin() + idx, std::move(value));
}

void expolygons_append_copy(expolygon_collection_handle *dst, const expolygon_collection_handle *src) {
    if (dst == nullptr || src == nullptr)
        return;
    Slic3r::ExPolygons *dst_expolys = Slic3r::to_expolygons(dst);
    const Slic3r::ExPolygons *src_expolys = Slic3r::to_expolygons(src);
    dst_expolys->insert(dst_expolys->end(), src_expolys->begin(), src_expolys->end());
}

void expolygons_insert_move(expolygon_collection_handle *dst, uint32_t idx, expolygon_handle *src) {
    if (dst == nullptr || src == nullptr)
        return;
    Slic3r::ExPolygons *expolys = Slic3r::to_expolygons(dst);
    idx = std::min(idx, static_cast<uint32_t>(expolys->size()));
    Slic3r::ExPolygon value = std::move(*Slic3r::to_expolygon(src));
    Slic3r::to_expolygon(src)->clear();
    expolys->insert(expolys->begin() + idx, std::move(value));
}

void expolygons_append_move(expolygon_collection_handle *dst, expolygon_collection_handle *src) {
    if (dst == nullptr || src == nullptr || dst == src)
        return;
    Slic3r::ExPolygons *dst_expolys = Slic3r::to_expolygons(dst);
    Slic3r::ExPolygons *src_expolys = Slic3r::to_expolygons(src);
    dst_expolys->insert(dst_expolys->end(),
                        std::make_move_iterator(src_expolys->begin()),
                        std::make_move_iterator(src_expolys->end()));
    src_expolys->clear();
}

void expolygons_copy(expolygon_collection_handle *dst, const expolygon_collection_handle *src) {
    if (dst != nullptr && src != nullptr)
        *Slic3r::to_expolygons(dst) = *Slic3r::to_expolygons(src);
}

void expolygons_move(expolygon_collection_handle *dst, expolygon_collection_handle *src) {
    if (dst == nullptr || src == nullptr || dst == src)
        return;
    *Slic3r::to_expolygons(dst) = std::move(*Slic3r::to_expolygons(src));
    Slic3r::to_expolygons(src)->clear();
}

void expolygons_to_polygons(const expolygon_collection_handle *src, polygon_collection_handle *dst) {
    if (src == nullptr || dst == nullptr)
        return;
    Slic3r::Polygons &out = *Slic3r::to_polygons(dst);
    out.clear();
    for (const Slic3r::ExPolygon &expoly : *Slic3r::to_expolygons(src)) {
        out.emplace_back(expoly.contour);
        for (const Slic3r::Polygon &hole : expoly.holes)
            out.emplace_back(hole);
    }
}

polyline_collection_handle *expolygons_to_polylines(storage_handle *storage,
                                                    const expolygon_collection_handle *src)
{
    if (storage == nullptr || src == nullptr)
        return nullptr;

    polyline_collection_handle *out_handle = storage_new_polylines(storage);
    *Slic3r::to_polylines(out_handle) = Slic3r::to_polylines(*Slic3r::to_expolygons(src));
    return out_handle;
}

expolygon_status polygons_to_expolygons(const polygon_collection_handle *src, expolygon_collection_handle *dst) {
    if (src == nullptr || dst == nullptr)
        return EXPOLYGON_STATUS_EMPTY;
    Slic3r::ExPolygons &out = *Slic3r::to_expolygons(dst);
    out.clear();
    if (Slic3r::to_polygons(src)->empty())
        return EXPOLYGON_STATUS_EMPTY;
    for (Slic3r::Polygon poly : *Slic3r::to_polygons(src)) {
        if (!poly.is_valid())
            return EXPOLYGON_STATUS_INVALID_GEOMETRY;
        if (poly.is_counter_clockwise()) {
            out.emplace_back(std::move(poly));
        } else {
            if (out.empty())
                return EXPOLYGON_STATUS_INVALID_ORIENTATION;
            out.back().holes.emplace_back(std::move(poly));
        }
    }
    return expolygons_valid(dst);
}

extrusion_entity_handle *expolygon_medial_axis_extrusion(
    storage_handle *storage,
    const expolygon_handle *src,
    const c_medial_axis_extrusion_params *params)
{
    if (storage == nullptr || src == nullptr || params == nullptr)
        return nullptr;

    const Slic3r::ExPolygon &source = *Slic3r::to_expolygon(src);
    if (!Slic3r::medial_axis_input_is_valid(source, *params))
        return nullptr;

    std::unique_ptr<Slic3r::ExtrusionEntity> root = Slic3r::build_medial_axis_tree(source, *params);
    if (root == nullptr)
        return nullptr;

    if (root->child_count() == 0 && (params->flags & MEDIAL_AXIS_EXTRUSION_KEEP_EMPTY_ROOT) == 0)
        return nullptr;

    return Slic3r::store_extrusion_entity(storage, std::move(root));
}

} // extern "C"
