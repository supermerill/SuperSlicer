///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#include <cstring>

#include "libslic3r/Api/host/ApiHostUtils.hpp"
#include "libslic3r/Api/plugin/c/slic3r_volume.h"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintObject.hpp"
#include "libslic3r/PrintObjectRegion.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"

namespace Slic3r {

static const PrintObject *to_object(const object_handle *me)
{
    return reinterpret_cast<const PrintObject *>(me);
}

static const ModelVolume *to_volume(const volume_handle *me)
{
    return reinterpret_cast<const ModelVolume *>(me);
}

static const TriangleMesh *to_triangle_mesh(const triangle_mesh_handle *me)
{
    return reinterpret_cast<const TriangleMesh *>(me);
}

using SlicingLayerRange = PrintObjectRegions::LayerRangeRegions;
using SlicingVolumeRegion = PrintObjectRegions::VolumeRegion;

static const SlicingLayerRange *to_layer_range(const slicing_layer_range_handle *me)
{
    return reinterpret_cast<const SlicingLayerRange *>(me);
}

static const SlicingVolumeRegion *to_volume_region(const slicing_volume_region_handle *me)
{
    return reinterpret_cast<const SlicingVolumeRegion *>(me);
}

static Polygons *to_polygons(polygon_collection_handle *me)
{
    return reinterpret_cast<Polygons *>(me);
}

static ExPolygons *to_expolygons(expolygon_collection_handle *me)
{
    return reinterpret_cast<ExPolygons *>(me);
}

static c_matrix4d to_c_matrix4d(const Transform3d &matrix)
{
    c_matrix4d out = {};
    const auto &raw = matrix.matrix();
    for (uint32_t row = 0; row < 4; ++row)
        for (uint32_t col = 0; col < 4; ++col)
            out.value[row * 4 + col] = raw(row, col);
    return out;
}

static c_vec3f to_c_vec3f(const Vec3f &point)
{
    c_vec3f out = {};
    out.x = point.x();
    out.y = point.y();
    out.z = point.z();
    return out;
}

static c_bounding_box3f to_c_bounding_box3f(const PrintObjectRegions::BoundingAlignedBox3f &box)
{
    c_bounding_box3f out = {};
    out.min = to_c_vec3f(box.min());
    out.max = to_c_vec3f(box.max());
    return out;
}

static Transform3d to_transform3d(const c_matrix4d &matrix)
{
    Transform3d out = Transform3d::Identity();
    for (uint32_t row = 0; row < 4; ++row)
        for (uint32_t col = 0; col < 4; ++col)
            out.matrix()(row, col) = matrix.value[row * 4 + col];
    return out;
}

static raw_volume_type to_raw_volume_type(ModelVolumeType type)
{
    return static_cast<raw_volume_type>(static_cast<int>(type));
}

static EnforcerBlockerType to_enforcer_blocker_type(int32_t value)
{
    return static_cast<EnforcerBlockerType>(value);
}

static bool paint_key_equals(const char *paint_key, const char *known_key)
{
    return paint_key != nullptr && std::strcmp(paint_key, known_key) == 0;
}

static bool volume_has_painting(const ModelVolume *volume, const char *paint_key)
{
    if (volume == nullptr || paint_key == nullptr)
        return false;

    if (paint_key_equals(paint_key, RAW_FACET_PAINTING_FDM_SUPPORT))
        return volume->is_fdm_support_painted();
    if (paint_key_equals(paint_key, RAW_FACET_PAINTING_SEAM))
        return volume->is_seam_painted();
    if (paint_key_equals(paint_key, RAW_FACET_PAINTING_MMU_SEGMENTATION))
        return volume->is_mm_painted();

    // Non-builtin keys are generic seam-like plugin paintings. They all share
    // the same ModelVolume map lookup, so plugins can use the same public API
    // for built-in and registered paint keys.
    return volume->has_facets_annotation(paint_key);
}

static void append_projected_by_layer(std::vector<Polygons> &&src, std::vector<Polygons> &dst)
{
    if (src.empty())
        return;

    if (dst.empty()) {
        dst = std::move(src);
        return;
    }

    if (dst.size() < src.size())
        dst.resize(src.size());

    for (size_t layer_idx = 0; layer_idx < src.size(); ++layer_idx)
        for (Polygon &polygon : src[layer_idx])
            dst[layer_idx].emplace_back(std::move(polygon));
}

static std::vector<Polygons> project_mmu_painting_to_polygons(const PrintObject &object, EnforcerBlockerType type)
{
    std::vector<Polygons> out;
    const std::vector<float> zs = slice_z_from_layers(object.layers());
    const Transform3d object_trafo = object.trafo_centered();

    for (const ModelVolume *volume : object.model_object()->volumes) {
        if (!volume->is_model_part() || !volume->is_mm_painted())
            continue;

        const indexed_triangle_set painted = volume->mm_segmentation_facets.get_facets_strict(*volume, type);
        if (painted.indices.empty())
            continue;

        std::vector<Polygons> top;
        std::vector<Polygons> bottom;
        slice_mesh_slabs(painted, zs, object_trafo * volume->get_matrix(), &top, &bottom, [](){});
        append_projected_by_layer(std::move(top), out);
        append_projected_by_layer(std::move(bottom), out);
    }

    return out;
}

static void write_projected_polygons_to_c_handles(std::vector<Polygons> &&projected,
                                                  polygon_collection_handle **out_by_layer,
                                                  uint32_t layer_count)
{
    if (out_by_layer == nullptr)
        return;

    for (uint32_t layer_idx = 0; layer_idx < layer_count; ++layer_idx) {
        if (out_by_layer[layer_idx] == nullptr)
            continue;
        Polygons &dst = *to_polygons(out_by_layer[layer_idx]);
        dst = layer_idx < projected.size() ? std::move(projected[layer_idx]) : Polygons{};
    }
}

static MeshSlicingParams::SlicingMode to_mesh_slicing_mode(raw_mesh_slicing_mode mode)
{
    switch (mode) {
    case RAW_MESH_SLICING_MODE_EVEN_ODD:
        return MeshSlicingParams::SlicingMode::EvenOdd;
    case RAW_MESH_SLICING_MODE_POSITIVE:
        return MeshSlicingParams::SlicingMode::Positive;
    case RAW_MESH_SLICING_MODE_POSITIVE_LARGEST_CONTOUR:
        return MeshSlicingParams::SlicingMode::PositiveLargestContour;
    case RAW_MESH_SLICING_MODE_REGULAR:
    default:
        return MeshSlicingParams::SlicingMode::Regular;
    }
}

static MeshSlicingParamsEx to_mesh_slicing_params(const c_mesh_slicing_params *params)
{
    MeshSlicingParamsEx out;
    if (params == nullptr)
        return out;

    out.mode = to_mesh_slicing_mode(params->mode);
    out.slicing_mode_normal_below_layer = params->slicing_mode_normal_below_layer;
    out.mode_below = to_mesh_slicing_mode(params->mode_below);
    out.trafo = to_transform3d(params->transform);
    out.closing_radius = params->closing_radius;
    out.extra_offset = params->extra_offset;
    out.resolution = params->resolution;
    out.model_resolution = params->model_resolution;
    return out;
}

uint32_t object_volume_count(const object_handle *object)
{
    const PrintObject *object_native = to_object(object);
    return object_native == nullptr || object_native->model_object() == nullptr ?
        0u : uint32_t(object_native->model_object()->volumes.size());
}

const volume_handle *object_volume_at(const object_handle *object, uint32_t idx)
{
    const PrintObject *object_native = to_object(object);
    if (object_native == nullptr || object_native->model_object() == nullptr)
        return nullptr;
    const ModelVolumePtrs &volumes = object_native->model_object()->volumes;
    if (idx >= volumes.size())
        return nullptr;
    return reinterpret_cast<const volume_handle *>(volumes[idx]);
}

uint32_t object_count_slicing_layer_range(const object_handle *object)
{
    const PrintObject *native = to_object(object);
    return native == nullptr || native->shared_regions() == nullptr ?
        0u : uint32_t(native->shared_regions()->layer_ranges.size());
}

const slicing_layer_range_handle *object_get_slicing_layer_range(const object_handle *object, uint32_t idx)
{
    const PrintObject *native = to_object(object);
    if (native == nullptr || native->shared_regions() == nullptr)
        return nullptr;
    const std::vector<SlicingLayerRange> &ranges = native->shared_regions()->layer_ranges;
    return idx < ranges.size() ? reinterpret_cast<const slicing_layer_range_handle *>(&ranges[idx]) : nullptr;
}

coord_t slicing_layer_range_get_z_min(const slicing_layer_range_handle *range)
{
    return range == nullptr ? 0 : to_layer_range(range)->layer_height_range_.first;
}

coord_t slicing_layer_range_get_z_max(const slicing_layer_range_handle *range)
{
    return range == nullptr ? 0 : to_layer_range(range)->layer_height_range_.second;
}

const config_handle *slicing_layer_range_get_config(const slicing_layer_range_handle *range)
{
    return range == nullptr ? nullptr : ApiHost::to_config_handle(to_layer_range(range)->config);
}

uint32_t slicing_layer_range_count_volume_region(const slicing_layer_range_handle *range)
{
    return range == nullptr ? 0u : uint32_t(to_layer_range(range)->volume_regions.size());
}

const slicing_volume_region_handle *slicing_layer_range_get_volume_region(
    const slicing_layer_range_handle *range,
    uint32_t idx)
{
    if (range == nullptr)
        return nullptr;
    const std::vector<SlicingVolumeRegion> &volume_regions = to_layer_range(range)->volume_regions;
    return idx < volume_regions.size() ?
        reinterpret_cast<const slicing_volume_region_handle *>(&volume_regions[idx]) : nullptr;
}

const volume_handle *slicing_volume_region_get_volume(const slicing_volume_region_handle *volume_region)
{
    const SlicingVolumeRegion *native = to_volume_region(volume_region);
    return native == nullptr ? nullptr : reinterpret_cast<const volume_handle *>(native->model_volume);
}

int32_t slicing_volume_region_get_parent(const slicing_volume_region_handle *volume_region)
{
    const SlicingVolumeRegion *native = to_volume_region(volume_region);
    return native == nullptr ? -1 : native->parent;
}

int32_t slicing_volume_region_get_layer_region_idx(const slicing_volume_region_handle *volume_region)
{
    const SlicingVolumeRegion *native = to_volume_region(volume_region);
    return native == nullptr || native->region == nullptr ? -1 : native->region->print_object_region_id();
}

c_bounding_box3f slicing_volume_region_get_bbox(const slicing_volume_region_handle *volume_region)
{
    const SlicingVolumeRegion *native = to_volume_region(volume_region);
    return native == nullptr || native->bbox == nullptr ? c_bounding_box3f{} : to_c_bounding_box3f(*native->bbox);
}

raw_volume_type volume_get_type(const volume_handle *volume)
{
    const ModelVolume *native = to_volume(volume);
    return native == nullptr ? RAW_VOLUME_TYPE_INVALID : to_raw_volume_type(native->type());
}

uint64_t volume_get_id(const volume_handle *volume)
{
    const ModelVolume *native = to_volume(volume);
    return native == nullptr ? 0u : uint64_t(native->id().id);
}

const config_handle *volume_get_config(const volume_handle *volume)
{
    const ModelVolume *native = to_volume(volume);
    return native == nullptr ? nullptr : ApiHost::to_config_handle(&native->config.get());
}

int32_t volume_get_extruder_id(const volume_handle *volume)
{
    const ModelVolume *native = to_volume(volume);
    return native == nullptr ? -1 : int32_t(native->extruder_id());
}

c_matrix4d volume_get_matrix(const volume_handle *volume)
{
    const ModelVolume *native = to_volume(volume);
    return native == nullptr ? c_matrix4d{} : to_c_matrix4d(native->get_matrix());
}

c_matrix4d volume_get_matrix_no_offset(const volume_handle *volume)
{
    const ModelVolume *native = to_volume(volume);
    return native == nullptr ? c_matrix4d{} : to_c_matrix4d(native->get_matrix_no_offset());
}

int volume_has_painting(const volume_handle *volume, const char *paint_key)
{
    const ModelVolume *native = to_volume(volume);
    return Slic3r::volume_has_painting(native, paint_key) ? 1 : 0;
}

void object_project_painting_to_polygons(const object_handle *object,
                                         const char *paint_key,
                                         int32_t painting_value,
                                         polygon_collection_handle **out_by_layer,
                                         uint32_t layer_count)
{
    const PrintObject *native = to_object(object);
    if (native == nullptr || native->model_object() == nullptr || paint_key == nullptr || out_by_layer == nullptr) {
        write_projected_polygons_to_c_handles({}, out_by_layer, layer_count);
        return;
    }

    std::vector<Polygons> projected;
    if (paint_key_equals(paint_key, RAW_FACET_PAINTING_FDM_SUPPORT)) {
        projected = native->project_and_append_custom_facets(false, to_enforcer_blocker_type(painting_value));
    } else if (paint_key_equals(paint_key, RAW_FACET_PAINTING_SEAM)) {
        projected = native->project_and_append_custom_facets(true, to_enforcer_blocker_type(painting_value));
    } else if (paint_key_equals(paint_key, RAW_FACET_PAINTING_MMU_SEGMENTATION)) {
        projected = project_mmu_painting_to_polygons(*native, to_enforcer_blocker_type(painting_value));
    } else {
        // Any other key is a registered generic seam-like painting. Projection
        // is deliberately the same as seam projection so plugin slicers can ask
        // for polygons without caring about the underlying storage class.
        projected = native->project_and_append_custom_facets(std::string(paint_key), to_enforcer_blocker_type(painting_value));
    }

    write_projected_polygons_to_c_handles(std::move(projected), out_by_layer, layer_count);
}

const triangle_mesh_handle *volume_get_mesh(const volume_handle *volume)
{
    const ModelVolume *native = to_volume(volume);
    return native == nullptr ? nullptr : reinterpret_cast<const triangle_mesh_handle *>(&native->mesh());
}

uint32_t triangle_mesh_vertex_count(const triangle_mesh_handle *mesh)
{
    const TriangleMesh *native = to_triangle_mesh(mesh);
    return native == nullptr ? 0u : uint32_t(native->its.vertices.size());
}

uint32_t triangle_mesh_triangle_count(const triangle_mesh_handle *mesh)
{
    const TriangleMesh *native = to_triangle_mesh(mesh);
    return native == nullptr ? 0u : uint32_t(native->its.indices.size());
}

c_vec3f triangle_mesh_vertex_at(const triangle_mesh_handle *mesh, uint32_t idx)
{
    const TriangleMesh *native = to_triangle_mesh(mesh);
    if (native == nullptr || idx >= native->its.vertices.size())
        return {};
    return to_c_vec3f(native->its.vertices[idx]);
}

c_triangle_indices triangle_mesh_triangle_at(const triangle_mesh_handle *mesh, uint32_t idx)
{
    c_triangle_indices out = {};
    const TriangleMesh *native = to_triangle_mesh(mesh);
    if (native == nullptr || idx >= native->its.indices.size())
        return out;
    const stl_triangle_vertex_indices &triangle = native->its.indices[idx];
    out.a = uint32_t(triangle[0]);
    out.b = uint32_t(triangle[1]);
    out.c = uint32_t(triangle[2]);
    return out;
}

} // namespace Slic3r

extern "C" {

uint32_t object_volume_count(const object_handle *object)
{
    return Slic3r::object_volume_count(object);
}

const volume_handle *object_volume_at(const object_handle *object, uint32_t idx)
{
    return Slic3r::object_volume_at(object, idx);
}

uint32_t object_count_slicing_layer_range(const object_handle *object)
{
    return Slic3r::object_count_slicing_layer_range(object);
}

const slicing_layer_range_handle *object_get_slicing_layer_range(const object_handle *object, uint32_t idx)
{
    return Slic3r::object_get_slicing_layer_range(object, idx);
}

coord_t slicing_layer_range_get_z_min(const slicing_layer_range_handle *range)
{
    return Slic3r::slicing_layer_range_get_z_min(range);
}

coord_t slicing_layer_range_get_z_max(const slicing_layer_range_handle *range)
{
    return Slic3r::slicing_layer_range_get_z_max(range);
}

const config_handle *slicing_layer_range_get_config(const slicing_layer_range_handle *range)
{
    return Slic3r::slicing_layer_range_get_config(range);
}

uint32_t slicing_layer_range_count_volume_region(const slicing_layer_range_handle *range)
{
    return Slic3r::slicing_layer_range_count_volume_region(range);
}

const slicing_volume_region_handle *slicing_layer_range_get_volume_region(
    const slicing_layer_range_handle *range,
    uint32_t idx)
{
    return Slic3r::slicing_layer_range_get_volume_region(range, idx);
}

const volume_handle *slicing_volume_region_get_volume(const slicing_volume_region_handle *volume_region)
{
    return Slic3r::slicing_volume_region_get_volume(volume_region);
}

int32_t slicing_volume_region_get_parent(const slicing_volume_region_handle *volume_region)
{
    return Slic3r::slicing_volume_region_get_parent(volume_region);
}

int32_t slicing_volume_region_get_layer_region_idx(const slicing_volume_region_handle *volume_region)
{
    return Slic3r::slicing_volume_region_get_layer_region_idx(volume_region);
}

c_bounding_box3f slicing_volume_region_get_bbox(const slicing_volume_region_handle *volume_region)
{
    return Slic3r::slicing_volume_region_get_bbox(volume_region);
}

raw_volume_type volume_get_type(const volume_handle *volume)
{
    return Slic3r::volume_get_type(volume);
}

uint64_t volume_get_id(const volume_handle *volume)
{
    return Slic3r::volume_get_id(volume);
}

const config_handle *volume_get_config(const volume_handle *volume)
{
    return Slic3r::volume_get_config(volume);
}

int32_t volume_get_extruder_id(const volume_handle *volume)
{
    return Slic3r::volume_get_extruder_id(volume);
}

c_matrix4d volume_get_matrix(const volume_handle *volume)
{
    return Slic3r::volume_get_matrix(volume);
}

c_matrix4d volume_get_matrix_no_offset(const volume_handle *volume)
{
    return Slic3r::volume_get_matrix_no_offset(volume);
}

int volume_has_painting(const volume_handle *volume, const char *paint_key)
{
    return Slic3r::volume_has_painting(volume, paint_key);
}

void object_project_painting_to_polygons(const object_handle *object,
                                         const char *paint_key,
                                         int32_t painting_value,
                                         polygon_collection_handle **out_by_layer,
                                         uint32_t layer_count)
{
    Slic3r::object_project_painting_to_polygons(object, paint_key, painting_value, out_by_layer, layer_count);
}

const triangle_mesh_handle *volume_get_mesh(const volume_handle *volume)
{
    return Slic3r::volume_get_mesh(volume);
}

uint32_t triangle_mesh_vertex_count(const triangle_mesh_handle *mesh)
{
    return Slic3r::triangle_mesh_vertex_count(mesh);
}

uint32_t triangle_mesh_triangle_count(const triangle_mesh_handle *mesh)
{
    return Slic3r::triangle_mesh_triangle_count(mesh);
}

c_vec3f triangle_mesh_vertex_at(const triangle_mesh_handle *mesh, uint32_t idx)
{
    return Slic3r::triangle_mesh_vertex_at(mesh, idx);
}

c_triangle_indices triangle_mesh_triangle_at(const triangle_mesh_handle *mesh, uint32_t idx)
{
    return Slic3r::triangle_mesh_triangle_at(mesh, idx);
}

void triangle_mesh_slice_to_polygons(const triangle_mesh_handle *mesh,
                                     c_matrix4d transform,
                                     const float *z_mm_by_layer,
                                     polygon_collection_handle **slices_by_layer,
                                     uint32_t layer_count)
{
    c_mesh_slicing_params params = {};
    params.mode = RAW_MESH_SLICING_MODE_REGULAR;
    params.mode_below = RAW_MESH_SLICING_MODE_REGULAR;
    params.transform = transform;
    triangle_mesh_slice_to_polygons_with_params(mesh, &params, z_mm_by_layer, slices_by_layer, layer_count);
}

void triangle_mesh_slice_to_polygons_with_params(const triangle_mesh_handle *mesh,
                                                 const c_mesh_slicing_params *params,
                                                 const float *z_mm_by_layer,
                                                 polygon_collection_handle **slices_by_layer,
                                                 uint32_t layer_count)
{
    if (mesh == nullptr || z_mm_by_layer == nullptr || slices_by_layer == nullptr || layer_count == 0)
        return;

    std::vector<float> zs(z_mm_by_layer, z_mm_by_layer + layer_count);

    Slic3r::MeshSlicingParamsEx native_params = Slic3r::to_mesh_slicing_params(params);
    indexed_triangle_set its = Slic3r::to_triangle_mesh(mesh)->its;
    if (native_params.trafo.rotation().determinant() < 0.)
        Slic3r::its_flip_triangles(its);
    std::vector<Slic3r::Polygons> sliced = Slic3r::slice_mesh(its, zs, native_params);

    for (uint32_t idx = 0; idx < layer_count; ++idx) {
        assert(slices_by_layer != nullptr);
        if (slices_by_layer[idx] != nullptr)
            *Slic3r::to_polygons(slices_by_layer[idx]) = idx < sliced.size() ? std::move(sliced[idx]) : Slic3r::Polygons{};
    }
}

polygon_collection_handle *triangle_mesh_slice_to_polygon(storage_handle *storage,
                                                          const triangle_mesh_handle *mesh,
                                                          c_matrix4d transform,
                                                          float layer_z_mm) {
    if (storage == nullptr || mesh == nullptr)
        return nullptr;

    //MeshSlicingParams params;
    //params.trafo = to_transform3d(transform);
    //std::vector<Polygons> sliced = slice_mesh(to_triangle_mesh(mesh)->its, zs, params);


    return nullptr;
}

void triangle_mesh_slice_to_expolygons(const triangle_mesh_handle *mesh,
                                       c_matrix4d transform,
                                       const float *z_mm_by_layer,
                                       expolygon_collection_handle **slices_by_layer,
                                       uint32_t layer_count)
{
    c_mesh_slicing_params params = {};
    params.mode = RAW_MESH_SLICING_MODE_REGULAR;
    params.mode_below = RAW_MESH_SLICING_MODE_REGULAR;
    params.transform = transform;
    triangle_mesh_slice_to_expolygons_with_params(mesh, &params, z_mm_by_layer, slices_by_layer, layer_count);
}

void triangle_mesh_slice_to_expolygons_with_params(const triangle_mesh_handle *mesh,
                                                   const c_mesh_slicing_params *params,
                                                   const float *z_mm_by_layer,
                                                   expolygon_collection_handle **slices_by_layer,
                                                   uint32_t layer_count)
{
    if (mesh == nullptr || z_mm_by_layer == nullptr || slices_by_layer == nullptr || layer_count == 0)
        return;

    std::vector<float> zs(z_mm_by_layer, z_mm_by_layer + layer_count);

    Slic3r::MeshSlicingParamsEx native_params = Slic3r::to_mesh_slicing_params(params);
    indexed_triangle_set its = Slic3r::to_triangle_mesh(mesh)->its;
    if (native_params.trafo.rotation().determinant() < 0.)
        Slic3r::its_flip_triangles(its);
    std::vector<Slic3r::ExPolygons> sliced = Slic3r::slice_mesh_ex(its, zs, native_params);

    for (uint32_t idx = 0; idx < layer_count; ++idx) {
        if (slices_by_layer[idx] == nullptr)
            continue;
        Slic3r::ExPolygons &dst = *Slic3r::to_expolygons(slices_by_layer[idx]);
        dst = idx < sliced.size() ? std::move(sliced[idx]) : Slic3r::ExPolygons{};
        Slic3r::ensure_valid(dst, scale_i(native_params.resolution));
    }
}

} // extern "C"
