///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "SliceVolume.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/cpp/Views.hpp"
#include "libslic3r/PrintRegion.hpp"

/*
SliceVolume
===========

This plugin performs the raw mesh-slicing stage for one Object. It converts
the model, negative, and modifier volumes into ExPolygons at the already
selected object-layer heights, assigns those polygons to print regions, and
writes them into LayerRegion slices for later surface generation.

The normal execution flow is:

    run_impl()
    |-- read the object layer Z positions
    |-- slice every printable, negative, or modifier volume with the object
    |   transform, resolution, slicing mode, and XY compensation
    |-- assign sliced polygons to the destination print regions
    |   |-- use the direct fast path when only one volume region is active
    |   |-- use direct assignment when multiple regions do not overlap in XY
    |   `-- use the complex clipping pass when active volume regions overlap
    `-- move the completed region/layer collections into LayerRegion slices

`VolumeSlices` keeps the result of one volume indexed by object layer. The
volume list is processed in stable ID order so overlapping regions have a
deterministic input order. Layer ranges filter which volumes are sliced and
which layers receive their polygons. Complex slices reproduce the native
region relationship rules by clipping each volume against the portions that
have already been assigned, then moving the resulting polygons into the
region/layer grid.

The plugin deliberately publishes raw LayerRegion slices only. It does not
create `Surface` objects, classify top/bottom/internal areas, or generate
extrusions; those responsibilities belong to later pipeline steps. Empty
volumes and volumes that are not printable model, negative, or modifier parts
are skipped.
*/

namespace slic3r_api { namespace SliceVolumePlugin {

namespace {

const char *k_slice_volume_id = "slice_volume";
const char *k_no_dependencies[] = {nullptr};

// Raw mesh slicing result for one ModelVolume. The slicer returns simple
// polygon loops per layer; region assignment and boolean clipping are handled
// later because they depend on layer ranges and volume relationships.
struct VolumeSlices
{
    Volume volume;
    std::vector<StoredExPolygonCollection> by_layer;
};

// A layer/range pair that cannot be assigned by the fast path because two or
// more printable volume regions overlap in XY at this Z. These slices need the
// clipping pass that mimics slices_to_regions().
struct ComplexSlice
{
    uint32_t layer_range_idx = 0;
    uint32_t layer_idx = 0;
    float z = 0.f;
};

bool contains_z(c_bounding_box3f bbox, float z)
{
    return bbox.min.z <= z && bbox.max.z >= z;
}

bool overlap_in_xy(c_bounding_box3f lhs, c_bounding_box3f rhs)
{
    return !(lhs.max.x < rhs.min.x || lhs.min.x > rhs.max.x ||
             lhs.max.y < rhs.min.y || lhs.min.y > rhs.max.y);
}

VolumeSlices *volume_slices_find_by_id(std::vector<VolumeSlices> &volume_slices, uint64_t volume_id)
{
    auto it = std::find_if(volume_slices.begin(), volume_slices.end(), [volume_id](const VolumeSlices &slices) {
        return slices.volume.id() == volume_id;
    });
    assert(it != volume_slices.end());
    return it == volume_slices.end() ? nullptr : &*it;
}

const VolumeSlices *volume_slices_find_by_id(const std::vector<VolumeSlices> &volume_slices, uint64_t volume_id)
{
    auto it = std::find_if(volume_slices.begin(), volume_slices.end(), [volume_id](const VolumeSlices &slices) {
        return slices.volume.id() == volume_id;
    });
    assert(it != volume_slices.end());
    return it == volume_slices.end() ? nullptr : &*it;
}

bool model_volume_needs_slicing(const Volume &volume)
{
    return volume.is_model_part() || volume.is_negative() || volume.is_modifier();
}

raw_mesh_slicing_mode mesh_slicing_mode_from_config(int32_t slicing_mode)
{
    switch (slicing_mode) {
    case 1:
        return RAW_MESH_SLICING_MODE_EVEN_ODD;
    case 2:
        return RAW_MESH_SLICING_MODE_POSITIVE;
    case 0:
    default:
        return RAW_MESH_SLICING_MODE_REGULAR;
    }
}

bool layer_range_has_volume(const run_ctx_slicing &ctx,
                            const slicing_layer_range_handle *layer_range,
                            uint64_t volume_id)
{
    const uint32_t count = ctx.layer_range_volume_region_count(layer_range);
    for (uint32_t idx = 0; idx < count; ++idx) {
        const slicing_volume_region_handle *volume_region = ctx.layer_range_volume_region_at(layer_range, idx);
        const volume_handle *volume_h = ctx.volume_region_volume(volume_region);
        if (volume_h != nullptr && Volume(volume_h).id() == volume_id)
            return true;
    }
    return false;
}

std::vector<std::pair<float, float>> slicing_ranges_for_volume(const run_ctx_slicing &ctx,
                                                               const Object &object,
                                                               uint64_t volume_id)
{
    std::vector<std::pair<float, float>> ranges;
    const uint32_t count = ctx.layer_range_count(object.handle());
    ranges.reserve(count);
    for (uint32_t idx = 0; idx < count; ++idx) {
        const slicing_layer_range_handle *layer_range = ctx.layer_range_at(object.handle(), idx);
        if (layer_range != nullptr && layer_range_has_volume(ctx, layer_range, volume_id))
            ranges.emplace_back(float(unscaled(ctx.layer_range_z_min(layer_range))),
                                float(unscaled(ctx.layer_range_z_max(layer_range))));
    }
    return ranges;
}

std::vector<StoredExPolygonCollection> make_empty_expolygon_layers(storage_handle *storage, size_t layer_count)
{
    std::vector<StoredExPolygonCollection> out;
    out.reserve(layer_count);
    for (size_t idx = 0; idx < layer_count; ++idx)
        out.emplace_back(storage);
    return out;
}

std::vector<StoredExPolygonCollection> slice_volume(const Volume &volume,
                                                    const std::vector<float> &slice_zs,
                                                    const c_mesh_slicing_params &params,
                                                    storage_handle *storage)
{
    if (slice_zs.empty() || volume.mesh().empty())
        return {};

    c_mesh_slicing_params volume_params = params;
    volume_params.transform = matrix4d_mul(params.transform, volume.matrix());
    std::vector<StoredExPolygonCollection> out =
        volume.mesh().slice_to_expolygons(storage, volume_params, slice_zs);
    for (StoredExPolygonCollection &expolygons : out)
        expolygons.ensure_valid(scale_i(params.resolution));
    return out;
}

std::vector<StoredExPolygonCollection> slice_volume(const Volume &volume,
                                                    const std::vector<float> &slice_zs,
                                                    const std::vector<std::pair<float, float>> &ranges,
                                                    const c_mesh_slicing_params &params,
                                                    storage_handle *storage)
{
    if (slice_zs.empty() || ranges.empty() || volume.mesh().empty())
        return {};

    if (ranges.size() == 1 && slice_zs.front() >= ranges.front().first && slice_zs.back() < ranges.front().second)
        return slice_volume(volume, slice_zs, params, storage);

    std::vector<float> filtered_zs;
    std::vector<std::pair<size_t, size_t>> filtered_spans;
    filtered_zs.reserve(slice_zs.size());
    filtered_spans.reserve(2 * ranges.size());

    size_t idx = 0;
    for (const std::pair<float, float> &range : ranges) {
        for (; idx < slice_zs.size() && slice_zs[idx] < range.first; ++idx) {}
        const size_t first = idx;
        for (; idx < slice_zs.size() && slice_zs[idx] < range.second; ++idx)
            filtered_zs.emplace_back(slice_zs[idx]);
        if (idx > first)
            filtered_spans.emplace_back(first, idx);
    }
    if (filtered_spans.empty())
        return {};

    std::vector<StoredExPolygonCollection> sliced = slice_volume(volume, filtered_zs, params, storage);
    std::vector<StoredExPolygonCollection> out = make_empty_expolygon_layers(storage, slice_zs.size());
    size_t sliced_idx = 0;
    for (const std::pair<size_t, size_t> &span : filtered_spans) {
        for (size_t layer_idx = span.first; layer_idx < span.second; ++layer_idx)
            out[layer_idx] = std::move(sliced[sliced_idx++]);
    }
    return out;
}

void process_complex_volume_regions_with_clipping(
    const run_ctx_slicing &ctx,
    storage_handle *storage,
    const std::vector<VolumeSlices> &volume_slices,
    const std::vector<ComplexSlice> &complex_slices,
    std::vector<std::vector<StoredExPolygonCollection>> &region_slices)
{
    ClipperContext clipper(storage);

    // Per-volume material for one complex layer. region_id is the destination
    // PrintRegion index; -1 means the volume is not printable for this layer.
    struct RegionSlice
    {
        StoredExPolygonCollection expolygons;
        int32_t region_id = -1;
        uint64_t volume_id = 0;
    };

    for (const ComplexSlice &complex_slice : complex_slices) {
        const slicing_layer_range_handle *layer_range =
            ctx.layer_range_at(ctx.object, complex_slice.layer_range_idx);
        if (layer_range == nullptr)
            continue;

        std::vector<RegionSlice> temp_slices;
        const uint32_t volume_region_count = ctx.layer_range_volume_region_count(layer_range);
        temp_slices.reserve(volume_region_count);

        for (uint32_t idx = 0; idx < volume_region_count; ++idx) {
            const slicing_volume_region_handle *volume_region =
                ctx.layer_range_volume_region_at(layer_range, idx);
            const volume_handle *volume_h = ctx.volume_region_volume(volume_region);
            if (volume_h == nullptr) {
                temp_slices.push_back({StoredExPolygonCollection(storage), -1, 0});
                continue;
            }

            Volume volume(volume_h);
            StoredExPolygonCollection expolygons(storage);
            if (contains_z(ctx.volume_region_bbox(volume_region), complex_slice.z)) {
                const VolumeSlices *slices = volume_slices_find_by_id(volume_slices, volume.id());
                if (slices != nullptr && complex_slice.layer_idx < slices->by_layer.size())
                    expolygons.copy_from(slices->by_layer[complex_slice.layer_idx].readonly());
            }

            temp_slices.push_back({
                std::move(expolygons),
                ctx.volume_region_layer_region_idx(volume_region),
                volume.id()
            });
        }

        // Apply modifier and negative-volume relationships in layer-range order.
        // This follows the structure of the native slices_to_regions() complex
        // branch: modifiers carve their parent, while model/negative volumes cut
        // previously processed non-negative material when their bounding boxes
        // overlap in XY.
        for (uint32_t idx = 0; idx < volume_region_count; ++idx) {
            const slicing_volume_region_handle *volume_region =
                ctx.layer_range_volume_region_at(layer_range, idx);
            const volume_handle *volume_h = ctx.volume_region_volume(volume_region);
            if (volume_h == nullptr || temp_slices[idx].expolygons.empty())
                continue;

            Volume volume(volume_h);
            if (volume.is_modifier()) {
                const int32_t parent_idx = ctx.volume_region_parent(volume_region);
                if (parent_idx < 0 || uint32_t(parent_idx) >= temp_slices.size())
                    continue;

                RegionSlice &parent_slice = temp_slices[uint32_t(parent_idx)];
                RegionSlice &this_slice = temp_slices[idx];
                StoredExPolygonCollection source = this_slice.expolygons.readonly().clone(storage);

                if (parent_slice.expolygons.empty()) {
                    this_slice.expolygons.clear();
                } else {
                    // A modifier receives only the area inside its parent. The
                    // parent then loses that same source area so following
                    // regions do not see duplicated material.
                    this_slice.expolygons = clipper_intersection(clipper(parent_slice.expolygons), clipper(source)).to_expolygon_collection();
                    parent_slice.expolygons = clipper_diff(clipper(parent_slice.expolygons), clipper(source)).to_expolygon_collection();
                }

                if (idx + 1 < volume_region_count) {
                    const slicing_volume_region_handle *next_volume_region =
                        ctx.layer_range_volume_region_at(layer_range, idx + 1);
                    const volume_handle *next_volume_h = ctx.volume_region_volume(next_volume_region);
                    if (next_volume_h != nullptr && Volume(next_volume_h).id() == volume.id())
                        temp_slices[idx + 1].expolygons.copy_from(source);
                }
            } else if (volume.is_model_part() || volume.is_negative()) {
                const c_bounding_box3f bbox = ctx.volume_region_bbox(volume_region);
                for (uint32_t worse_idx = 0; worse_idx < idx; ++worse_idx) {
                    if (temp_slices[worse_idx].expolygons.empty())
                        continue;

                    const slicing_volume_region_handle *worse_volume_region =
                        ctx.layer_range_volume_region_at(layer_range, worse_idx);
                    const volume_handle *worse_volume_h = ctx.volume_region_volume(worse_volume_region);
                    if (worse_volume_h == nullptr)
                        continue;

                    Volume worse_volume(worse_volume_h);
                    if (worse_volume.is_negative() || !overlap_in_xy(bbox, ctx.volume_region_bbox(worse_volume_region)))
                        continue;

                    // TODO: reproduce the native slice_merge_dent / slice_merge_min_width
                    // policy here. For now the complex path performs the core clipping:
                    // later higher-priority model/negative volumes remove material from
                    // previous non-negative regions.
                    temp_slices[worse_idx].expolygons =
                        clipper_diff(clipper(temp_slices[worse_idx].expolygons), clipper(temp_slices[idx].expolygons))
                            .to_expolygon_collection();
                }
            }
        }

        // Move the temporary region slices into the final [region][layer] grid.
        // A union is applied after appending because several volume regions can
        // target the same print region on the same layer.
        for (RegionSlice &slice : temp_slices) {
            if (slice.region_id < 0 || uint32_t(slice.region_id) >= region_slices.size() || slice.expolygons.empty())
                continue;

            StoredExPolygonCollection &dst = region_slices[uint32_t(slice.region_id)][complex_slice.layer_idx];
            dst.append_move_from(std::move(slice.expolygons));
            dst = clipper_union(clipper(dst)).to_expolygon_collection();
            dst.ensure_valid();
        }
    }
}

std::vector<float> slice_z_from_layers(const Object &object)
{
    // The low-level triangle slicer uses float coordinates, so keep the Z list
    // unscaled here. All comparisons against layer ranges are scaled explicitly
    // at the call sites.
    std::vector<float> slice_zs;
    slice_zs.reserve(object.layer_count());
    for (uint32_t layer_idx = 0; layer_idx < object.layer_count(); ++layer_idx) {
        const Layer layer = object.layer(layer_idx);
        slice_zs.push_back(static_cast<float>(unscaled(layer.slice_z())));
    }
    return slice_zs;
}

std::vector<VolumeSlices> slice_volumes_inner(const run_ctx_slicing &ctx,
                                              const Print &print,
                                              const Object &object,
                                              const std::vector<float> &slice_zs,
                                              storage_handle *storage)
{
    // This follows PrintObjectSlice.cpp::slice_volumes_inner(): prepare one
    // MeshSlicingParamsEx-equivalent, slice only printable/negative/modifier
    // volumes, filter by layer ranges, and apply the same XY compensation policy.
    std::vector<VolumeSlices> out;
    out.reserve(object.volume_count());

    std::vector<Volume> volumes;
    volumes.reserve(object.volume_count());
    for (uint32_t volume_idx = 0; volume_idx < object.volume_count(); ++volume_idx)
        volumes.push_back(object.volume(volume_idx));
    std::sort(volumes.begin(), volumes.end(), [](const Volume &lhs, const Volume &rhs) {
        return lhs.id() < rhs.id();
    });

    const Config print_config = print.config();
    const Config object_config = object.config();
    const size_t num_extruders = print.config().get("nozzle_diameter").size();
    const bool is_mm_painted = num_extruders > 1 && std::any_of(volumes.begin(), volumes.end(), [](const Volume &volume) {
        return volume.is_mm_painted();
    });

    const float outer_delta = float(object_config.get("xy_size_compensation").get_float());
    const float inner_delta = float(object_config.get("xy_inner_size_compensation").get_float());
    const float hole_delta = inner_delta + float(object_config.get("hole_size_compensation").get_float());
    const float min_delta = std::min(outer_delta, std::min(inner_delta, hole_delta));
    const float extra_offset = is_mm_painted ? 0.f : std::max(0.f, min_delta);

    c_mesh_slicing_params params_base = {};
    params_base.closing_radius = float(object_config.get("slice_closing_radius").get_float());
    params_base.extra_offset = 0.f;
    params_base.transform = object.transform_centered();
    params_base.resolution = std::max(EPSILON, print_config.get("resolution").get_float());
    params_base.model_resolution = object_config.get("model_precision").get_float();
    params_base.mode = mesh_slicing_mode_from_config(object_config.get("slicing_mode").get_int());
    params_base.mode_below = params_base.mode;

    const uint32_t layer_range_count = ctx.layer_range_count(object.handle());
    for (const Volume &volume : volumes) {
        if (!model_volume_needs_slicing(volume))
            continue;

        c_mesh_slicing_params params = params_base;
        if (!volume.is_negative())
            params.extra_offset = extra_offset;

        std::vector<StoredExPolygonCollection> by_layer;
        if (layer_range_count == 1) {
            const slicing_layer_range_handle *layer_range = ctx.layer_range_at(object.handle(), 0);
            if (layer_range == nullptr || !layer_range_has_volume(ctx, layer_range, volume.id()))
                continue;

            // Keep all model contours during slicing. Vase-mode cleanup is now
            // a post-slicing plugin, where it can connect close islands with
            // real bridge material before choosing which disconnected
            // component must survive.

            by_layer = slice_volume(volume, slice_zs, params, storage);
        } else {
            assert(!print_config.get("spiral_vase").get_bool());
            std::vector<std::pair<float, float>> ranges = slicing_ranges_for_volume(ctx, object, volume.id());
            by_layer = slice_volume(volume, slice_zs, ranges, params, storage);
        }

        if (!by_layer.empty())
            out.push_back({volume, std::move(by_layer)});
    }

    return out;
}

std::vector<std::vector<StoredExPolygonCollection>> slices_to_regions(const run_ctx_slicing &ctx,
                                                                      const Print &print,
                                                                      const Object &object,
                                                                      const std::vector<float> &slice_zs,
                                                                      std::vector<VolumeSlices> &&volume_slices,
                                                                      storage_handle *storage)
{
    (void)print;

    // Destination grid: one owned ExPolygon collection for every print region
    // and every already-created object layer.
    std::vector<std::vector<StoredExPolygonCollection>> region_slices;
    region_slices.reserve(object.print_region_count());
    for (uint32_t region_idx = 0; region_idx < object.print_region_count(); ++region_idx) {
        std::vector<StoredExPolygonCollection> by_layer;
        by_layer.reserve(object.layer_count());
        for (uint32_t layer_idx = 0; layer_idx < object.layer_count(); ++layer_idx)
            by_layer.emplace_back(storage);
        region_slices.push_back(std::move(by_layer));
    }

    std::vector<ComplexSlice> complex_slices;
    uint32_t layer_idx = 0;
    const uint32_t layer_range_count = ctx.layer_range_count(ctx.object);

    for (uint32_t layer_range_idx = 0; layer_range_idx < layer_range_count; ++layer_range_idx) {
        const slicing_layer_range_handle *layer_range = ctx.layer_range_at(ctx.object, layer_range_idx);
        if (layer_range == nullptr)
            continue;

        const coord_t range_z_min = ctx.layer_range_z_min(layer_range);
        const coord_t range_z_max = ctx.layer_range_z_max(layer_range);

        while (layer_idx < slice_zs.size() && scale_i(double(slice_zs[layer_idx])) < range_z_min)
            ++layer_idx;

        const uint32_t volume_region_count = ctx.layer_range_volume_region_count(layer_range);
        if (volume_region_count == 0)
            continue;

        // Fast path: when a layer range has a single printable model part, no
        // overlap test or boolean clipping is needed. We only rebuild ExPolygons
        // from raw volume polygons and append them to the mapped print region.
        if (volume_region_count == 1) {
            const slicing_volume_region_handle *volume_region = ctx.layer_range_volume_region_at(layer_range, 0);
            Volume volume(ctx.volume_region_volume(volume_region));
            const int32_t region_idx = ctx.volume_region_layer_region_idx(volume_region);

            if (volume.is_model_part() && region_idx >= 0 && uint32_t(region_idx) < region_slices.size()) {
                VolumeSlices *slices_src = volume_slices_find_by_id(volume_slices, volume.id());
                for (; layer_idx < slice_zs.size() && scale_i(double(slice_zs[layer_idx])) < range_z_max; ++layer_idx) {
                    region_slices[uint32_t(region_idx)][layer_idx].append_move_from(std::move(slices_src->by_layer[layer_idx]));
                }
            }
            continue;
        }

        // Slow-path classifier. A layer is complex only if another active volume
        // overlaps the first printable model volume in XY at the current Z.
        complex_slices.reserve(slice_zs.size());
        for (; layer_idx < slice_zs.size() && scale_i(double(slice_zs[layer_idx])) < range_z_max; ++layer_idx) {
            const float z = slice_zs[layer_idx];
            int32_t first_printable_region = -1;
            bool complex = false;

            for (uint32_t region_idx = 0; region_idx < volume_region_count; ++region_idx) {
                const slicing_volume_region_handle *volume_region =
                    ctx.layer_range_volume_region_at(layer_range, region_idx);
                const c_bounding_box3f bbox = ctx.volume_region_bbox(volume_region);
                if (!contains_z(bbox, z))
                    continue;

                Volume volume(ctx.volume_region_volume(volume_region));
                if (first_printable_region == -1 && volume.is_model_part()) {
                    first_printable_region = int32_t(region_idx);
                    continue;
                }

                if (first_printable_region != -1) {
                    for (int32_t prev_idx = first_printable_region; prev_idx < int32_t(region_idx); ++prev_idx) {
                        const slicing_volume_region_handle *prev_volume_region =
                            ctx.layer_range_volume_region_at(layer_range, uint32_t(prev_idx));
                        const c_bounding_box3f prev_bbox = ctx.volume_region_bbox(prev_volume_region);
                        if (contains_z(prev_bbox, z) && overlap_in_xy(bbox, prev_bbox)) {
                            complex = true;
                            break;
                        }
                    }
                }

                if (complex)
                    break;
            }

            if (complex) {
                complex_slices.push_back({layer_range_idx, layer_idx, z});
                continue;
            }

            if (first_printable_region >= 0) {
                // Non-complex multi-volume case: only one printable model part
                // contributes at this Z, so it can still be assigned directly.
                const slicing_volume_region_handle *volume_region =
                    ctx.layer_range_volume_region_at(layer_range, uint32_t(first_printable_region));
                Volume volume(ctx.volume_region_volume(volume_region));
                const int32_t region_idx = ctx.volume_region_layer_region_idx(volume_region);
                if (region_idx < 0 || uint32_t(region_idx) >= region_slices.size())
                    continue;

                VolumeSlices *slices_src = volume_slices_find_by_id(volume_slices, volume.id());
                region_slices[uint32_t(region_idx)][layer_idx].append_move_from(std::move(slices_src->by_layer[layer_idx]));
            }
        }
    }

    if (!complex_slices.empty()) {
        process_complex_volume_regions_with_clipping(ctx, storage, volume_slices, complex_slices, region_slices);
    }

    return region_slices;
}

void write_region_slices(const run_ctx_slicing &ctx,
                         Object object,
                         std::vector<std::vector<StoredExPolygonCollection>> &&region_slices)
{
    // STEP_SLICING owns raw slice generation only. Do not create Surface entries
    // here: surface generation is a later step and must see these raw slices as
    // its input.
    for (uint32_t region_idx = 0; region_idx < region_slices.size(); ++region_idx) {
        std::vector<StoredExPolygonCollection> &by_layer = region_slices[region_idx];
        for (uint32_t layer_idx = 0; layer_idx < by_layer.size(); ++layer_idx) {
            StoredExPolygonCollection &layer_region_slices = by_layer[layer_idx];

            expolygon_collection_handle *dst = ctx.layer_region_borrow_mutable_slices(
                object.layer(layer_idx).region(region_idx).handle());
            expolygons_move(dst, layer_region_slices.mutable_handle());
        }
    }
}

} // namespace

SliceVolume &SliceVolume::instance(orchestrator_handle *orch)
{
    static SliceVolume s_instance(orch);
    return s_instance;
}

const char *SliceVolume::id_impl() const noexcept
{
    return k_slice_volume_id;
}

const char *SliceVolume::name_impl() const noexcept
{
    return "Volume slicer";
}

const char *SliceVolume::description_impl() const noexcept
{
    return "Slice object volumes into raw layer-region slices.";
}

slicing_step_t SliceVolume::step_impl() const noexcept
{
    return STEP_SLICING;
}

const char *const *SliceVolume::dependencies_impl() const noexcept
{
    return k_no_dependencies;
}

int32_t SliceVolume::priority_impl() const noexcept
{
    return 0;
}

const char *SliceVolume::progress_message_format_impl() const noexcept
{
    return "Slicing volumes: %u / %u layers";
}

void SliceVolume::setup_run_impl(const plugin_run_context *run_ctx) const
{
    // Progress is counted per layer, per object. setup_run() is called once for
    // each object before run(), allowing PluginProgress to aggregate all workers.
    const run_ctx_slicing *ctx = plugin_ctx_as_slicing(run_ctx);
    if (ctx == nullptr || ctx->object == nullptr)
        return;

    progress().add_max(Object(ctx->object).layer_count());
}

void SliceVolume::run_impl(const plugin_run_context *run_ctx) const
{
    // This plugin is intentionally written through public ABI views. If a host
    // object is missing from the step context, fail quietly; PluginBase handles
    // exceptions and fatal error reporting for real failures.
    const run_ctx_slicing *ctx = plugin_ctx_as_slicing(run_ctx);
    if (ctx == nullptr || ctx->print == nullptr || ctx->object == nullptr)
        return;

    Print print(ctx->print);
    Object object(ctx->object);
    storage_handle *storage = run_ctx->plugin_storage;

    std::vector<float> slice_zs = slice_z_from_layers(object);
    std::vector<VolumeSlices> volume_slices = slice_volumes_inner(*ctx, print, object, slice_zs, storage);
    std::vector<std::vector<StoredExPolygonCollection>> region_slices =
        slices_to_regions(*ctx, print, object, slice_zs, std::move(volume_slices), storage);

    write_region_slices(*ctx, object, std::move(region_slices));

    progress().finish_run();
}

void register_slice_volume_plugin(orchestrator_handle *orch)
{
    orchestrator_register_plugin(orch, SliceVolume::instance(orch).c_instance());
}

}} // namespace slic3r_api::SliceVolumePlugin

#ifdef SLICE_VOLUME_PLUGIN_DLL
extern "C" void register_plugin(orchestrator_handle *orch)
{
    slic3r_api::SliceVolumePlugin::register_slice_volume_plugin(orch);
}
#endif // SLICE_VOLUME_PLUGIN_DLL
