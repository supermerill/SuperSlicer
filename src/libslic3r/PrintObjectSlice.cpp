///|/ Copyright (c) Prusa Research 2021 - 2023 Oleksandra Iushchenko @YuSanka, Vojtěch Bubník @bubnikv, Pavel Mikuš @Godrak, Lukáš Hejl @hejllukas
///|/
///|/ Copyright (c) SuperSlicer 2020 - 2024 Durand Remi @supermerill
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "BridgeDetector.hpp"
#include "ClipperUtils.hpp"
#include "ElephantFootCompensation.hpp"
#include "I18N.hpp"
#include "Layer.hpp"
#include "MultiMaterialSegmentation.hpp"
#include "Print.hpp"
#include "ShortestPath.hpp"
#include "Thread.hpp"

#include <boost/log/trivial.hpp>

#include <oneapi/tbb/parallel_for.h>


namespace Slic3r {

LayerPtrs new_layers(
    PrintObject                 *print_object,
    // Object layers (pairs of bottom/top Z coordinate), without the raft.
    const std::vector<coordf_t> &object_layers)
{
    LayerPtrs out;
    out.reserve(object_layers.size());
    auto     id   = int(print_object->slicing_parameters().raft_layers());
    coordf_t zmin = print_object->slicing_parameters().object_print_z_min;
    Layer   *prev = nullptr;
    for (size_t i_layer = 0; i_layer < object_layers.size(); i_layer += 2) {
        coordf_t lo = object_layers[i_layer];
        coordf_t hi = object_layers[i_layer + 1];
        coordf_t slice_z = 0.5 * (lo + hi);
        Layer *layer = new Layer(id ++, print_object, hi - lo, hi + zmin, slice_z);
        out.emplace_back(layer);
        if (prev != nullptr) {
            prev->upper_layer = layer;
            layer->lower_layer = prev;
        }
        prev = layer;
    }
    return out;
}

// Slice single triangle mesh.
static std::vector<ExPolygons> slice_volume(
    const ModelVolume             &volume,
    const std::vector<float>      &zs, 
    const MeshSlicingParamsEx     &params,
    const std::function<void()>   &throw_on_cancel_callback)
{
    std::vector<ExPolygons> layers;
    if (! zs.empty()) {
        indexed_triangle_set its = volume.mesh().its;
        if (its.indices.size() > 0) {
            MeshSlicingParamsEx params2 { params };
            params2.trafo = params2.trafo * volume.get_matrix();
            if (params2.trafo.rotation().determinant() < 0.)
                its_flip_triangles(its);
            layers = slice_mesh_ex(its, zs, params2, throw_on_cancel_callback);
            throw_on_cancel_callback();
        }
    }

    return layers;
}

// Slice single triangle mesh.
// Filter the zs not inside the ranges. The ranges are closed at the bottom and open at the top, they are sorted lexicographically and non overlapping.
static std::vector<ExPolygons> slice_volume(
    const ModelVolume                           &volume,
    const std::vector<float>                    &z,
    const std::vector<t_layer_height_range>     &ranges,
    const MeshSlicingParamsEx                   &params,
    const std::function<void()>                 &throw_on_cancel_callback)
{
    std::vector<ExPolygons> out;
    if (! z.empty() && ! ranges.empty()) {
        if (ranges.size() == 1 && z.front() >= ranges.front().first && z.back() < ranges.front().second) {
            // All layers fit into a single range.
            out = slice_volume(volume, z, params, throw_on_cancel_callback);
        } else {
            std::vector<float>                     z_filtered;
            std::vector<std::pair<size_t, size_t>> n_filtered;
            z_filtered.reserve(z.size());
            n_filtered.reserve(2 * ranges.size());
            size_t i = 0;
            for (const t_layer_height_range &range : ranges) {
                for (; i < z.size() && z[i] < range.first; ++ i) ;
                size_t first = i;
                for (; i < z.size() && z[i] < range.second; ++ i)
                    z_filtered.emplace_back(z[i]);
                if (i > first)
                    n_filtered.emplace_back(std::make_pair(first, i));
            }
            if (! n_filtered.empty()) {
                std::vector<ExPolygons> layers = slice_volume(volume, z_filtered, params, throw_on_cancel_callback);
                out.assign(z.size(), ExPolygons());
                i = 0;
                for (const std::pair<size_t, size_t> &span : n_filtered)
                    for (size_t j = span.first; j < span.second; ++ j)
                        out[j] = std::move(layers[i ++]);
            }
        }
    }
    for(ExPolygons &expolys : out)
        ensure_valid(expolys, scale_t(params.resolution));
    return out;
}


struct VolumeSlices
{
    ObjectID                volume_id;
    std::vector<ExPolygons> slices;
};

static inline bool model_volume_needs_slicing(const ModelVolume &mv)
{
    ModelVolumeType type = mv.type();
    return type == ModelVolumeType::MODEL_PART || type == ModelVolumeType::NEGATIVE_VOLUME || type == ModelVolumeType::PARAMETER_MODIFIER;
}

// Slice printable volumes, negative volumes and modifier volumes, sorted by ModelVolume::id().
// Apply closing radius.
// Apply positive XY compensation to ModelVolumeType::MODEL_PART and ModelVolumeType::PARAMETER_MODIFIER, not to ModelVolumeType::NEGATIVE_VOLUME.
// Apply contour simplification.
static std::vector<VolumeSlices> slice_volumes_inner(
    const PrintConfig                                        &print_config,
    const PrintObjectConfig                                  &print_object_config,
    const Transform3d                                        &object_trafo,
    ModelVolumePtrs                                           model_volumes,
    const std::vector<PrintObjectRegions::LayerRangeRegions> &layer_ranges,
    const std::vector<float>                                 &zs,
    const std::function<void()>                              &throw_on_cancel_callback)
{
    model_volumes_sort_by_id(model_volumes);

    std::vector<VolumeSlices> out;
    out.reserve(model_volumes.size());

    std::vector<t_layer_height_range> slicing_ranges;
    if (layer_ranges.size() > 1)
        slicing_ranges.reserve(layer_ranges.size());

    MeshSlicingParamsEx params_base;
    params_base.closing_radius = print_object_config.slice_closing_radius.value;
    params_base.extra_offset   = 0;
    params_base.trafo          = object_trafo;
    params_base.resolution     = std::max(EPSILON, print_config.resolution.value);
    params_base.model_resolution = print_object_config.model_precision.value;

    switch (print_object_config.slicing_mode.value) {
    case SlicingMode::Regular:    params_base.mode = MeshSlicingParams::SlicingMode::Regular; break;
    case SlicingMode::EvenOdd:    params_base.mode = MeshSlicingParams::SlicingMode::EvenOdd; break;
    case SlicingMode::CloseHoles: params_base.mode = MeshSlicingParams::SlicingMode::Positive; break;
    }

    params_base.mode_below     = params_base.mode;

    const size_t num_extruders = print_config.nozzle_diameter.size();
    const bool   is_mm_painted = num_extruders > 1 && std::any_of(model_volumes.cbegin(), model_volumes.cend(), [](const ModelVolume *mv) { return mv->is_mm_painted(); });
    // Apply size compensation and perform clipping of multi-part objects.
    float outter_delta = print_object_config.xy_size_compensation.value;
    float inner_delta = print_object_config.xy_inner_size_compensation.value;
    float hole_delta = inner_delta + (print_object_config.hole_size_compensation.value);
    float min_delta = std::min(outter_delta, std::min(inner_delta, hole_delta));
    const float extra_offset = is_mm_painted ? 0.f : std::max(0.f, min_delta);

    for (const ModelVolume *model_volume : model_volumes)
        if (model_volume_needs_slicing(*model_volume)) {
            MeshSlicingParamsEx params { params_base };
            if (! model_volume->is_negative_volume())
                params.extra_offset = extra_offset;
            if (layer_ranges.size() == 1) {
                if (const PrintObjectRegions::LayerRangeRegions &layer_range = layer_ranges.front(); layer_range.has_volume(model_volume->id())) {
                    if (model_volume->is_model_part() && print_config.spiral_vase) {
                        auto it = std::find_if(layer_range.volume_regions.begin(), layer_range.volume_regions.end(), 
                            [model_volume](const auto &slice){ return model_volume == slice.model_volume; });
                        params.mode = MeshSlicingParams::SlicingMode::PositiveLargestContour;
                        // Slice the bottom layers with SlicingMode::Regular.
                        // This needs to be in sync with LayerRegion::make_perimeters() spiral_vase!
                        const PrintRegionConfig &region_config = it->region->config();
                        params.slicing_mode_normal_below_layer = size_t(region_config.bottom_solid_layers.value);
                        for (; params.slicing_mode_normal_below_layer < zs.size() && zs[params.slicing_mode_normal_below_layer] < region_config.bottom_solid_min_thickness - EPSILON;
                            ++ params.slicing_mode_normal_below_layer);
                    }
                    out.push_back({
                        model_volume->id(), 
                        slice_volume(*model_volume, zs, params, throw_on_cancel_callback)
                    });
                }
            } else {
                assert(! print_config.spiral_vase);
                slicing_ranges.clear();
                for (const PrintObjectRegions::LayerRangeRegions &layer_range : layer_ranges)
                    if (layer_range.has_volume(model_volume->id()))
                        slicing_ranges.emplace_back(layer_range.layer_height_range);
                if (! slicing_ranges.empty())
                    out.push_back({ 
                        model_volume->id(), 
                        slice_volume(*model_volume, zs, slicing_ranges, params, throw_on_cancel_callback)
                    });
            }
            if (! out.empty() && out.back().slices.empty())
                out.pop_back();
        }

    return out;
}

static inline VolumeSlices& volume_slices_find_by_id(std::vector<VolumeSlices> &volume_slices, const ObjectID id)
{
    auto it = lower_bound_by_predicate(volume_slices.begin(), volume_slices.end(), [id](const VolumeSlices &vs) { return vs.volume_id < id; });
    assert(it != volume_slices.end() && it->volume_id == id);
    return *it;
}

static inline bool overlap_in_xy(const PrintObjectRegions::BoundingBox &l, const PrintObjectRegions::BoundingBox &r)
{
    return ! (l.max().x() < r.min().x() || l.min().x() > r.max().x() ||
              l.max().y() < r.min().y() || l.min().y() > r.max().y());
}

static std::vector<PrintObjectRegions::LayerRangeRegions>::const_iterator layer_range_first(const std::vector<PrintObjectRegions::LayerRangeRegions> &layer_ranges, double z)
{
    auto  it = lower_bound_by_predicate(layer_ranges.begin(), layer_ranges.end(),
        [z](const PrintObjectRegions::LayerRangeRegions &lr) { return lr.layer_height_range.second < z; });
    assert(it != layer_ranges.end() && it->layer_height_range.first <= z && z <= it->layer_height_range.second);
    if (z == it->layer_height_range.second)
        if (auto it_next = it; ++ it_next != layer_ranges.end() && it_next->layer_height_range.first == z)
            it = it_next;
    assert(it != layer_ranges.end() && it->layer_height_range.first <= z && z <= it->layer_height_range.second);
    return it;
}

static std::vector<PrintObjectRegions::LayerRangeRegions>::const_iterator layer_range_next(
    const std::vector<PrintObjectRegions::LayerRangeRegions>            &layer_ranges, 
    std::vector<PrintObjectRegions::LayerRangeRegions>::const_iterator   it,
    double                                                               z)
{
    for (; it->layer_height_range.second <= z; ++ it)
        assert(it != layer_ranges.end());
    assert(it != layer_ranges.end() && it->layer_height_range.first <= z && z < it->layer_height_range.second);
    return it;
}

static std::vector<std::vector<ExPolygons>> slices_to_regions(
    const PrintConfig                                        &print_config,
    const PrintObject                                        &print_object,
    ModelVolumePtrs                                           model_volumes,
    const PrintObjectRegions                                 &print_object_regions,
    const std::vector<float>                                 &zs,
    std::vector<VolumeSlices>                               &&volume_slices,
    const std::function<void()>                              &throw_on_cancel_callback)
{
    model_volumes_sort_by_id(model_volumes);

    std::vector<std::vector<ExPolygons>> slices_by_region(print_object_regions.all_regions.size(), std::vector<ExPolygons>(zs.size(), ExPolygons()));

    // First shuffle slices into regions if there is no overlap with another region possible, collect zs of the complex cases.
    std::vector<std::pair<size_t, float>> zs_complex;
    {
        size_t z_idx = 0;
        for (const PrintObjectRegions::LayerRangeRegions &layer_range : print_object_regions.layer_ranges) {
            for (; z_idx < zs.size() && zs[z_idx] < layer_range.layer_height_range.first; ++ z_idx) ;
            if (layer_range.volume_regions.empty()) {
            } else if (layer_range.volume_regions.size() == 1) {
                const ModelVolume *model_volume = layer_range.volume_regions.front().model_volume;
                assert(model_volume != nullptr);
                if (model_volume->is_model_part()) {
                    VolumeSlices &slices_src = volume_slices_find_by_id(volume_slices, model_volume->id());
                    auto         &slices_dst = slices_by_region[layer_range.volume_regions.front().region->print_object_region_id()];
                    for (; z_idx < zs.size() && zs[z_idx] < layer_range.layer_height_range.second; ++z_idx) {
                        slices_dst[z_idx] = std::move(slices_src.slices[z_idx]);
                        ensure_valid(slices_dst[z_idx], SCALED_EPSILON);
                    }
                }
            } else {
                zs_complex.reserve(zs.size());
                for (; z_idx < zs.size() && zs[z_idx] < layer_range.layer_height_range.second; ++ z_idx) {
                    float z                          = zs[z_idx];
                    int   idx_first_printable_region = -1;
                    bool  complex                    = false;
                    for (int idx_region = 0; idx_region < int(layer_range.volume_regions.size()); ++ idx_region) {
                        const PrintObjectRegions::VolumeRegion &region = layer_range.volume_regions[idx_region];
                        if (region.bbox->min().z() <= z && region.bbox->max().z() >= z) {
                            if (idx_first_printable_region == -1 && region.model_volume->is_model_part())
                                idx_first_printable_region = idx_region;
                            else if (idx_first_printable_region != -1) {
                                // Test for overlap with some other region.
                                for (int idx_region2 = idx_first_printable_region; idx_region2 < idx_region; ++ idx_region2) {
                                    const PrintObjectRegions::VolumeRegion &region2 = layer_range.volume_regions[idx_region2];
                                    if (region2.bbox->min().z() <= z && region2.bbox->max().z() >= z && overlap_in_xy(*region.bbox, *region2.bbox)) {
                                        complex = true;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    if (complex)
                        zs_complex.push_back({ z_idx, z });
                    else if (idx_first_printable_region >= 0) {
                        const PrintObjectRegions::VolumeRegion &region = layer_range.volume_regions[idx_first_printable_region];
                        assert_valid(volume_slices_find_by_id(volume_slices, region.model_volume->id()).slices[z_idx]);
                        slices_by_region[region.region->print_object_region_id()][z_idx] = std::move(volume_slices_find_by_id(volume_slices, region.model_volume->id()).slices[z_idx]);
                    }
                }
            }
            throw_on_cancel_callback();
        }
    }
    for(auto &slices : slices_by_region) for(auto &expolys : slices) assert_valid(expolys);

    // Second perform region clipping and assignment in parallel.
    if (! zs_complex.empty()) {
        std::vector<std::vector<VolumeSlices*>> layer_ranges_regions_to_slices(print_object_regions.layer_ranges.size(), std::vector<VolumeSlices*>());
        for (const PrintObjectRegions::LayerRangeRegions &layer_range : print_object_regions.layer_ranges) {
            std::vector<VolumeSlices*> &layer_range_regions_to_slices = layer_ranges_regions_to_slices[&layer_range - print_object_regions.layer_ranges.data()];
            layer_range_regions_to_slices.reserve(layer_range.volume_regions.size());
            for (const PrintObjectRegions::VolumeRegion &region : layer_range.volume_regions)
                layer_range_regions_to_slices.push_back(&volume_slices_find_by_id(volume_slices, region.model_volume->id()));
        }
        tbb::parallel_for(
            tbb::blocked_range<size_t>(0, zs_complex.size()),
            [&slices_by_region, &print_object_regions, &zs_complex, &layer_ranges_regions_to_slices, &print_config, &print_object, &throw_on_cancel_callback]
                (const tbb::blocked_range<size_t> &range) {
                float z              = zs_complex[range.begin()].second;
                auto  it_layer_range = layer_range_first(print_object_regions.layer_ranges, z);
                // Per volume_regions slices at this Z height.
                struct RegionSlice { 
                    ExPolygons  expolygons;
                    // Identifier of this region in PrintObjectRegions::all_regions
                    int         region_id;
                    ObjectID    volume_id;
                    bool operator<(const RegionSlice &rhs) const {
                        bool this_empty = this->region_id < 0 || this->expolygons.empty();
                        bool rhs_empty  = rhs.region_id < 0 || rhs.expolygons.empty();
                        // Sort the empty items to the end of the list.
                        // Sort by region_id & volume_id lexicographically.
                        return ! this_empty && (rhs_empty || (this->region_id < rhs.region_id || (this->region_id == rhs.region_id && volume_id < volume_id)));
                    }
                };
                std::vector<RegionSlice> temp_slices;
                for (size_t zs_complex_idx = range.begin(); zs_complex_idx < range.end(); ++ zs_complex_idx) {
                    auto [z_idx, z] = zs_complex[zs_complex_idx];
                    it_layer_range = layer_range_next(print_object_regions.layer_ranges, it_layer_range, z);
                    const PrintObjectRegions::LayerRangeRegions &layer_range = *it_layer_range;
                    {
                        std::vector<VolumeSlices*> &layer_range_regions_to_slices = layer_ranges_regions_to_slices[it_layer_range - print_object_regions.layer_ranges.begin()];
                        // Per volume_regions slices at thiz Z height.
                        temp_slices.clear();
                        temp_slices.reserve(layer_range.volume_regions.size());
                        for (VolumeSlices* &slices : layer_range_regions_to_slices) {
                            const PrintObjectRegions::VolumeRegion &volume_region = layer_range.volume_regions[&slices - layer_range_regions_to_slices.data()];
                            temp_slices.push_back({ std::move(slices->slices[z_idx]), volume_region.region ? volume_region.region->print_object_region_id() : -1, volume_region.model_volume->id() });
                        }
                    }

                    double max_slice_closing_radius = print_object.config().slice_closing_radius; //0;
                    // get slice for each region
                    for (int idx_region = 0; idx_region < int(layer_range.volume_regions.size()); ++ idx_region) {
                        //if(layer_range.volume_regions[idx_region].region)max_slice_closing_radius = std::max(max_slice_closing_radius, layer_range.volume_regions[idx_region].region->->config().slice_closing_radius.value; //for when slice_closing_radius will be in region
                        if (! temp_slices[idx_region].expolygons.empty()) {
                            const PrintObjectRegions::VolumeRegion &region = layer_range.volume_regions[idx_region];
                            if (region.model_volume->is_modifier()) {
                                assert(region.parent > -1);
                                bool next_region_same_modifier = idx_region + 1 < int(temp_slices.size()) && layer_range.volume_regions[idx_region + 1].model_volume == region.model_volume;
                                RegionSlice &parent_slice = temp_slices[region.parent];
                                RegionSlice &this_slice   = temp_slices[idx_region];
                                ExPolygons   source       = this_slice.expolygons;
                                if (parent_slice.expolygons.empty()) {
                                    this_slice  .expolygons.clear();
                                } else {
                                    this_slice  .expolygons = intersection_ex(parent_slice.expolygons, source);
                                    parent_slice.expolygons = diff_ex        (parent_slice.expolygons, source);
                                }
                                if (next_region_same_modifier)
                                    // To be used in the following iteration.
                                    temp_slices[idx_region + 1].expolygons = std::move(source);
                            } else if (region.model_volume->is_model_part() || region.model_volume->is_negative_volume()) {
                                assert(region.model_volume->is_negative_volume() || region.region);
                                coord_t best_half_min_width = (!region.region) ? 0 :
                                    scale_t(region.region->config().slice_merge_min_width.get_abs_value(
                                        region.region->width(frExternalPerimeter, false, print_object))) / 2;
                                // Clip every non-zero region preceding it.
                                for (int idx_worse_region = 0; idx_worse_region < idx_region; ++ idx_worse_region) {
                                    if (! temp_slices[idx_worse_region].expolygons.empty()) {
                                        if (const PrintObjectRegions::VolumeRegion &worse_region = layer_range.volume_regions[idx_worse_region];
                                            ! worse_region.model_volume->is_negative_volume() && overlap_in_xy(*region.bbox, *worse_region.bbox)) {
                                            // if not same extruder : make a dent for adhesion (unless it's a negative volume, no need to dent a negative volume).
                                            assert(!region.model_volume->is_model_part() || worse_region.region);
                                            if (region.model_volume->is_model_part() && worse_region.model_volume->extruder_id() != region.model_volume->extruder_id()) {
                                                double his_unscaled_ext_peri_width = worse_region.region->width(frExternalPerimeter, false, print_object);
                                                coord_t his_half_min_width = scale_t(worse_region.region->config().slice_merge_min_width.get_abs_value(his_unscaled_ext_peri_width)) / 2;
                                                coord_t his_expansion = scale_t(worse_region.region->config().slice_merge_dent.get_abs_value(his_unscaled_ext_peri_width));
                                                bool is_modified = false;
                                                ExPolygons collapsed_best_polys = temp_slices[idx_region].expolygons;
                                                if (best_half_min_width > 0) {
                                                    // compute collapsed region of it.
                                                    collapsed_best_polys = offset2_ex(collapsed_best_polys, -best_half_min_width, best_half_min_width);
                                                }
                                                if (his_expansion > 0) {
                                                    for (const ExPolygon &best_poly : collapsed_best_polys) {
                                                        for (size_t idx_worse_expoly = 0; idx_worse_expoly < temp_slices[idx_worse_region].expolygons.size(); ++idx_worse_expoly) {
                                                            ExPolygon &worse_poly = temp_slices[idx_worse_region].expolygons[idx_worse_expoly];
                                                            if (!intersection_ex(worse_poly, best_poly).empty()) {
                                                                // intersect! make a diff
                                                                ExPolygons smaller_worse_expolys = diff_ex(worse_poly, best_poly);
                                                                if (!smaller_worse_expolys.empty()) {
                                                                    {
                                                                        // make a dent
                                                                        ExPolygons medium_size_worse_expolys = offset_ex(smaller_worse_expolys, his_expansion, Slic3r::ClipperLib::jtSquare);
                                                                        // only go where our old big poly allow us
                                                                        medium_size_worse_expolys = intersection_ex(medium_size_worse_expolys, {worse_poly});
                                                                        // collapse small areas
                                                                        if (his_half_min_width > 0) {
                                                                            medium_size_worse_expolys = offset2_ex(medium_size_worse_expolys,
                                                                                -his_half_min_width, his_half_min_width,
                                                                                // jtSquare to avoid putting an unprintable needle into my best polygon. 
                                                                                Slic3r::ClipperLib::jtSquare);
                                                                        }
                                                                        // union with the smaller polygons, to avoid offset2_ex artifacts
                                                                        smaller_worse_expolys = union_ex(medium_size_worse_expolys, smaller_worse_expolys);
                                                                        ensure_valid(smaller_worse_expolys);
                                                                        // TODO: now, be sure 'best_poly' is wide enough.
                                                                        // If not, then remove a bit more material from smaller_worse_expolys
                                                                    }
                                                                    is_modified = true;
                                                                    // assign the result
                                                                    if (!smaller_worse_expolys.empty()) {
                                                                        worse_poly = smaller_worse_expolys.front();
                                                                    }
                                                                    if (smaller_worse_expolys.size() > 1) {
                                                                        // insert new expolygons
                                                                        temp_slices[idx_worse_region].expolygons.insert(
                                                                            temp_slices[idx_worse_region].expolygons.begin() + idx_worse_expoly,
                                                                            smaller_worse_expolys.begin()+1, smaller_worse_expolys.end());
                                                                        // don't process them a second time.
                                                                        idx_worse_expoly += smaller_worse_expolys.size() - 1;
                                                                    }
                                                                }
                                                                if (smaller_worse_expolys.empty()) {
                                                                    // remove if nothing is left
                                                                    temp_slices[idx_worse_region].expolygons.erase(
                                                                        temp_slices[idx_worse_region].expolygons.begin() + idx_worse_expoly);
                                                                    --idx_worse_expoly;
                                                                }
                                                            }
                                                        }
                                                    }
                                                } else {
                                                    // no slice_merge_dent, so just remove collapsed areas
                                                    temp_slices[idx_worse_region].expolygons = diff_ex(temp_slices[idx_worse_region].expolygons, collapsed_best_polys);
                                                }
                                                // if the collapsed have an intersection with this part, tag to remove intersection afterward
                                                // this test is done at the end, to void the intersection_ex if is_modified is already set.
                                                if (!is_modified && best_half_min_width > 0 && 
                                                    !intersection_ex(temp_slices[idx_region].expolygons, temp_slices[idx_worse_region].expolygons).empty()) {
                                                    is_modified = true;
                                                }
                                                if (is_modified) {
                                                    temp_slices[idx_region].expolygons = diff_ex(temp_slices[idx_region].expolygons, temp_slices[idx_worse_region].expolygons);
                                                }
                                            } else {
                                                // just a modifier or something like that.
                                                // Clip every non-zero region preceding it.
                                                temp_slices[idx_worse_region].expolygons = diff_ex(temp_slices[idx_worse_region].expolygons, temp_slices[idx_region].expolygons);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    //gap closing: also between volumes
                    // note: it needs to already have good clipped expolygons for each region, so nobody 'steal' area from another.
                    if (layer_range.volume_regions.size() > 1 && max_slice_closing_radius > 0) {
                        // if slice gap closing raduis & multiple region, then we need a clipping master
                        ExPolygons clip_master;
                        for (size_t idx_region = 0; idx_region < layer_range.volume_regions.size(); ++idx_region) {
                            append(clip_master, temp_slices[idx_region].expolygons);
                        }
                        clip_master = union_ex(clip_master);
                        // do the gap closing for the master (it's a simple one, use the max_slice_closing_radius
                        // instead of doing it separatly for each region, as this would be very complicated).
                        clip_master = offset2_ex(clip_master, scale_d(max_slice_closing_radius), -scale_d(max_slice_closing_radius));
                        ensure_valid(clip_master);

                        // // for when slice_closing_radius will be in region
                        // // can redo the grow to allow a region to go into its neighbor without the neighbor growing.
                        //ExPolygons unclip_master = clip_master;
                        //int iter = 0;
                        //while (!unclip_master.empty() && iter < 2) {
                            // now grow & clip the polygons of each region (the biggest region id has the priority, so
                            // we began by it)
                            for (size_t idx_region = layer_range.volume_regions.size() - 1; idx_region < layer_range.volume_regions.size(); --idx_region) {
                                if (!temp_slices[idx_region].expolygons.empty()) {
                                    ExPolygons &region_expolys = temp_slices[idx_region].expolygons;
                                    //if(if(layer_range.volume_regions[idx_region].region)) region_expolys = offset_ex(region_expolys, scale_d(layer_range.volume_regions[idx_region].region->config().slice_closing_radius.value)); // for when slice_closing_radius will be in region
                                    region_expolys = offset_ex(region_expolys, scale_d(print_object.config().slice_closing_radius));
                                    // now clip it by clip_master
                                    region_expolys = intersection_ex(region_expolys, clip_master);
                                    // now remove parts from the other regions
                                    for (size_t idx_region2 = 0; idx_region2 < layer_range.volume_regions.size(); ++idx_region2) {
                                        if (!temp_slices[idx_region2].expolygons.empty() && idx_region != idx_region2) {
                                            region_expolys = diff_ex(region_expolys, temp_slices[idx_region2].expolygons);
                                        }
                                    }
                                    // we now have our final poly for this region
                                    // FIXME: this may have make it grows outside of a modifier box, but parent_slice.expolygons is already modified.
                                    //        test if you can create the issue & then resolve it.
                                    ensure_valid(union_safety_offset_ex(region_expolys));
                                    // // for when slice_closing_radius will be in region
                                    // // to verify there is no holes from different max_slice_closing_radius
                                    //unclip_master = diff_ex(unclip_master, region_expolys);
                                }
                            }
                        // // for when slice_closing_radius will be in region
                        //    // remove artifacts
                        //    unclip_master = offset2_ex(unclip_master, -SACLED_EPSILON * 5, SCALED_EPSILON * 5);
                        //    iter++
                        //}
                    }

                    // Sort by region_id, push empty slices to the end.
                    std::sort(temp_slices.begin(), temp_slices.end());
                    // Remove the empty slices.
                    temp_slices.erase(std::find_if(temp_slices.begin(), temp_slices.end(), [](const auto &slice) { return slice.region_id == -1 || slice.expolygons.empty(); }), temp_slices.end());
                    // Merge slices and store them to the output.
                    for (int i = 0; i < int(temp_slices.size());) {
                        // Find a range of temp_slices with the same region_id.
                        int j = i;
                        bool merged = false;
                        ExPolygons &expolygons = temp_slices[i].expolygons;
                        for (++ j; j < int(temp_slices.size()) && temp_slices[i].region_id == temp_slices[j].region_id; ++ j)
                            if (ExPolygons &expolygons2 = temp_slices[j].expolygons; ! expolygons2.empty()) {
                                if (expolygons.empty()) {
                                    expolygons = std::move(expolygons2);
                                } else {
                                    append(expolygons, std::move(expolygons2));
                                    merged = true;
                                }
                            }
                        if (merged)
                            expolygons = closing_ex(expolygons, float(scale_(EPSILON)));
                        ensure_valid(expolygons);
                        slices_by_region[temp_slices[i].region_id][z_idx] = std::move(expolygons);
                        i = j;
                    }
                    throw_on_cancel_callback();
                }
            });
    }
    for(auto &slices : slices_by_region) for(auto &expolys : slices) assert_valid(expolys);

    // filament shrink
    for (const std::shared_ptr<PrintRegion>& pr : print_object_regions.all_regions) {
        if (pr.get()) {
            std::vector<ExPolygons>& region_polys = slices_by_region[pr->print_object_region_id()];
            const size_t extruder_id = pr->extruder(FlowRole::frPerimeter, print_object) - 1;
            double scale = print_config.filament_shrink.get_abs_value(extruder_id, 1);
            if (scale != 1) {
                scale = 1 / scale;
                for (ExPolygons& polys : region_polys)
                    for (ExPolygon& poly : polys)
                        poly.scale(scale);
                //FIXME: merge regions overlapping, don't shrink inside
            }
        }
    }
    for(auto &slices : slices_by_region) for(auto &expolys : slices) assert_valid(expolys);
    return slices_by_region;
}

// Layer::slicing_errors is no more set since 1.41.1 or possibly earlier, thus this code
// was not really functional for a long day and nobody missed it.
// Could we reuse this fixing code one day?
/*
std::string fix_slicing_errors(LayerPtrs &layers, const std::function<void()> &throw_if_canceled)
{
    // Collect layers with slicing errors.
    // These layers will be fixed in parallel.
    std::vector<size_t> buggy_layers;
    buggy_layers.reserve(layers.size());
    for (size_t idx_layer = 0; idx_layer < layers.size(); ++ idx_layer)
        if (layers[idx_layer]->slicing_errors)
            buggy_layers.push_back(idx_layer);

    BOOST_LOG_TRIVIAL(debug) << "Slicing objects - fixing slicing errors in parallel - begin";
    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, buggy_layers.size()),
        [&layers, &throw_if_canceled, &buggy_layers](const tbb::blocked_range<size_t>& range) {
            for (size_t buggy_layer_idx = range.begin(); buggy_layer_idx < range.end(); ++ buggy_layer_idx) {
                throw_if_canceled();
                size_t idx_layer = buggy_layers[buggy_layer_idx];
                Layer *layer     = layers[idx_layer];
                assert(layer->slicing_errors);
                // Try to repair the layer surfaces by merging all contours and all holes from neighbor layers.
                // BOOST_LOG_TRIVIAL(trace) << "Attempting to repair layer" << idx_layer;
                for (size_t region_id = 0; region_id < layer->region_count(); ++ region_id) {
                    LayerRegion *layerm = layer->get_region(region_id);
                    // Find the first valid layer below / above the current layer.
                    const Surfaces *upper_surfaces = nullptr;
                    const Surfaces *lower_surfaces = nullptr;
                    for (size_t j = idx_layer + 1; j < layers.size(); ++ j)
                        if (! layers[j]->slicing_errors) {
                            upper_surfaces = &layers[j]->regions()[region_id]->slices().surfaces;
                            break;
                        }
                    for (int j = int(idx_layer) - 1; j >= 0; -- j)
                        if (! layers[j]->slicing_errors) {
                            lower_surfaces = &layers[j]->regions()[region_id]->slices().surfaces;
                            break;
                        }
                    // Collect outer contours and holes from the valid layers above & below.
                    Polygons outer;
                    outer.reserve(
                        ((upper_surfaces == nullptr) ? 0 : upper_surfaces->size()) + 
                        ((lower_surfaces == nullptr) ? 0 : lower_surfaces->size()));
                    size_t num_holes = 0;
                    if (upper_surfaces)
                        for (const auto &surface : *upper_surfaces) {
                            outer.push_back(surface.expolygon.contour);
                            num_holes += surface.expolygon.holes.size();
                        }
                    if (lower_surfaces)
                        for (const auto &surface : *lower_surfaces) {
                            outer.push_back(surface.expolygon.contour);
                            num_holes += surface.expolygon.holes.size();
                        }
                    Polygons holes;
                    holes.reserve(num_holes);
                    if (upper_surfaces)
                        for (const auto &surface : *upper_surfaces)
                            polygons_append(holes, surface.expolygon.holes);
                    if (lower_surfaces)
                        for (const auto &surface : *lower_surfaces)
                            polygons_append(holes, surface.expolygon.holes);
                    layerm->m_slices.set(diff_ex(union_(outer), holes), stPosInternal | stDensSparse);
                }
                // Update layer slices after repairing the single regions.
                layer->make_slices();
            }
        });
    throw_if_canceled();
    BOOST_LOG_TRIVIAL(debug) << "Slicing objects - fixing slicing errors in parallel - end";

    // remove empty layers from bottom
    while (! layers.empty() && (layers.front()->lslices().empty() || layers.front()->empty())) {
        delete layers.front();
        layers.erase(layers.begin());
        if(!layers.empty())
            layers.front()->lower_layer = nullptr;
        for (size_t i = 0; i < layers.size(); ++ i)
            layers[i]->set_id(layers[i]->id() - 1);
    }

    return buggy_layers.empty() ? "" :
        "The model has overlapping or self-intersecting facets. I tried to repair it, "
        "however you might want to check the results or repair the input file and retry.\n";
}
*/

// Called by make_perimeters()
// 1) Decides Z positions of the layers,
// 2) Initializes layers and their regions
// 3) Slices the object meshes
// 4) Slices the modifier meshes and reclassifies the slices of the object meshes by the slices of the modifier meshes
// 5) Applies size compensation (offsets the slices in XY plane)
// 6) Replaces bad slices by the slices reconstructed from the upper/lower layer
// Resulting expolygons of layer regions are marked as Internal.
void PrintObject::slice()
{
    if (! this->set_started(posSlice))
        return;
    m_print->set_status(0, L("Processing triangulated mesh"));
    std::vector<coordf_t> layer_height_profile;
    this->update_layer_height_profile(*this->model_object(), *m_slicing_params, layer_height_profile);
    m_print->throw_if_canceled();
    m_typed_slices = false;
    this->clear_layers();
    m_layers = new_layers(this, generate_object_layers(*m_slicing_params, layer_height_profile));
    this->slice_volumes();
    m_print->throw_if_canceled();
#if 0
    // Layer::slicing_errors is no more set since 1.41.1 or possibly earlier, thus this code
    // was not really functional for a long day and nobody missed it.
    // Could we reuse this fixing code one day?

    // Fix the model.
    //FIXME is this the right place to do? It is done repeateadly at the UI and now here at the backend.
    std::string warning = fix_slicing_errors(m_layers, [this](){ m_print->throw_if_canceled(); });
    m_print->throw_if_canceled();
    if (! warning.empty())
        BOOST_LOG_TRIVIAL(info) << warning;
#endif

    //create polyholes
    this->_transform_hole_to_polyholes();

    this->_max_overhang_threshold();

    // Update bounding boxes, back up raw slices of complex models.
    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, m_layers.size()),
        [this](const tbb::blocked_range<size_t> &range) {
            for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++ layer_idx) {
                m_print->throw_if_canceled();
                Layer &layer = *m_layers[layer_idx];
                layer.lslices_ex.clear();
                layer.lslices_ex.reserve(layer.lslices().size());
                for (const ExPolygon &expoly : layer.lslices())
                	layer.lslices_ex.push_back({ get_extents(expoly) });
                layer.backup_untyped_slices();
            }
        });
    // Interlink the lslices into a Z graph.
    tbb::parallel_for(
        tbb::blocked_range<size_t>(1, m_layers.size()),
        [this](const tbb::blocked_range<size_t> &range) {
            for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++ layer_idx) {
                m_print->throw_if_canceled();
                Layer::build_up_down_graph(*m_layers[layer_idx - 1], *m_layers[layer_idx]);
            }
        });
    if (m_layers.empty())
        throw Slic3r::SlicingError("No layers were detected. You might want to repair your STL file(s) or check their size or thickness and retry.\n");    
    this->set_done(posSlice);
}

// modify the polygon so it doesn't have any concave angle spiker than 90°
// used by _max_overhang_threshold
// note: don't work well for min_angle != 90°
void only_convex_or_gt(Polygon &poly, double min_angle = PI/2) {
    const bool ccw = poly.is_counter_clockwise();
    std::vector<size_t> concave = ccw ? poly.concave_points_idx(0, min_angle - 0.001) : poly.convex_points_idx(0, min_angle - 0.001);
    size_t iter = 0;
    while (!concave.empty()) {
        assert(std::is_sorted(concave.begin(), concave.end()));
        Points new_pts;
        bool previous_modified = false;
        for (size_t idx = 0; idx < poly.points.size(); ++idx) {
            if (previous_modified || std::find(concave.begin(), concave.end(), idx) == concave.end()) {
                // convex: keep
                new_pts.push_back(poly.points[idx]);
                previous_modified = false;
            } else {
                previous_modified = true;
                // concave: create new points to have a min_angle angle
                // get smallest side
                Point small_side_point = idx == 0 ? poly.back() : poly[idx - 1];
                Point big_side_point   = idx == poly.size() - 1 ? poly.front() : poly[idx + 1];
                if (poly[idx].distance_to_square(small_side_point) > poly[idx].distance_to_square(big_side_point)) {
                    big_side_point   = idx == 0 ? poly.back() : poly[idx - 1];
                    small_side_point = idx == poly.size() - 1 ? poly.front() : poly[idx + 1];
                }
                // then get the distance to move in the big side
                Point previous_point = ccw? (idx == 0 ? poly.back() : poly[idx - 1]) : (idx == poly.size() - 1 ? poly.front() : poly[idx + 1]);
                Point next_point = ccw? (idx == poly.size() - 1 ? poly.front() : poly[idx + 1]) : (idx == 0 ? poly.back() : poly[idx - 1]);
#if _DEBUG
                assert(is_approx(ccw_angle_old_test(poly[idx], previous_point, next_point), abs_angle(angle_ccw(previous_point - poly[idx], next_point - poly[idx])), 0.00000001));
#endif
                double angle = abs_angle(angle_ccw(previous_point - poly[idx], next_point - poly[idx]));
                if (angle < min_angle + 0.001 && angle > min_angle) {
                    angle = min_angle;
                }
                assert(angle <= min_angle + EPSILON && angle >= 0);
                coordf_t dist_to_move = std::cos(angle) * poly[idx].distance_to(small_side_point) + SCALED_EPSILON / 2;
                // increase dist if big number of iteration (means it's too slow to pull 2 points one after another)
                dist_to_move *= (0.95 + ((iter + 2) * (iter + 1) / 40)); // 0->1; 1->1.1; 3->1.45; 5->2; 10->4.2; 20->12.5
                // if distance to move too big, just deleted point (don't add it)
                if (dist_to_move < poly[idx].distance_to(big_side_point)) {
                    Line l(poly[idx], big_side_point);
                    l.extend_start(-dist_to_move);
                    new_pts.push_back(l.a);
                    double angle_new = abs_angle(angle_ccw(previous_point - l.a, next_point - l.a));
                    assert(angle_new != angle);
                }
            }
        };
        poly.points = new_pts;
        concave = ccw ? poly.concave_points_idx(0, min_angle - 0.001) : poly.convex_points_idx(0, min_angle - 0.001);

        if (iter > 20) {
            // abord where we are.
            return;
        }
        iter++;
    }
}

// try to remove zig-zags
void smoothen(Polygon &poly, distf_t max_length = scale_d(1), double threshold = PI / 2) {
    // for each short-enough segment
    distsqrf_t max_d2 = max_length * max_length;
    distsqrf_t min_d2 = (max_length / 20) * (max_length / 20);
    bool previous_culled = false;
    bool prev_angle_culled = 0;
    for (size_t i = 0; i < poly.size(); i++) {
        //remove too small sections
        size_t im1 = std::min(i - 1, poly.size() - 1);
        distsqrf_t my_d2 = poly[im1].distance_to_square(poly[i]);
        if (min_d2 > my_d2) {
            poly[im1] = Line(poly[im1], poly[i]).midpoint();
            poly.points.erase(poly.points.begin() + i);
            i--;
        }
    }
    min_d2 = (max_length / 5) * (max_length / 5);
    for (size_t i = 0; i < poly.size(); i++) {
        bool next_culled = false;
        if (poly.size() <= 3) {
            // polygon too small
            return;
        }
        size_t im1 = std::min(i - 1, poly.size() - 1);
        distsqrf_t my_d2 = poly[im1].distance_to_square(poly[i]);
        if (max_d2 > my_d2) {
            bool too_short = min_d2 > my_d2;
            // if previous angle is sharp convex/concave
            size_t im2 = std::min(i - 2, poly.size() - 2);
            double prev_angle = previous_culled ? prev_angle_culled:
                angle_ccw(poly[im2] - poly[im1], poly[i] - poly[im1]);
            // if next angle is sharp concave/convex
            size_t ip1 = ((i + 1) < poly.size() ? i + 1 : 0);
            double next_angle = angle_ccw(poly[im1] - poly[i], poly[ip1] - poly[i]);
            //if (poly[im2].distance_to_square(poly[im1]) > max_d2 || poly[i].distance_to_square(poly[ip1]) > max_d2) {
            //    //if previous or next is very long, don't delete its point.
            //    continue;
            //}
            if ((prev_angle < 0 && next_angle > 0) || (prev_angle > 0 && next_angle < 0)) {
                // then merge the segment into a point
                if (too_short || (std::abs(prev_angle) < threshold && std::abs(next_angle) < threshold)) {
                    Point midpoint = Line(poly[im1], poly[i]).midpoint();
                    // smoothen
                    if (poly[im2].distance_to_square(poly[im1]) > max_d2) {
                        poly[im1] = Line(poly[im1], poly[im2]).point_at(max_length);
                        if (poly[i].distance_to_square(poly[ip1]) > max_d2) {
                            poly[i] = Line(poly[i], poly[ip1]).point_at(max_length);
                        } else {
                            poly[i] = midpoint;
                        }
                    } else {
                        poly[im1] = midpoint;
                        if (poly[i].distance_to_square(poly[ip1]) > max_d2) {
                            poly[i] = Line(poly[i], poly[ip1]).point_at(max_length);
                        } else {
                            poly.points.erase(poly.points.begin() + i);
                            i--;
                        }
                    }
                     next_culled = true;
                     prev_angle_culled = next_angle;
                }
            }
        }
        prev_angle_culled = next_culled;
    }
}

void PrintObject::_max_overhang_threshold() {
    bool has_enlargment = false;

    coord_t max_nz_diam = 0;
    for (int16_t extr_id : this->object_extruders()) {
        max_nz_diam = std::max(max_nz_diam, scale_t(print()->config().nozzle_diameter.get_at(extr_id)));
    }
    if (max_nz_diam == 0)
        max_nz_diam = scale_t(0.4);
    
    for (size_t region_idx = 0; region_idx < this->num_printing_regions(); ++region_idx) {
        coord_t enlargement = scale_t(this->printing_region(region_idx).config().overhangs_max_slope.get_abs_value(unscaled(max_nz_diam)));
        if (enlargement > 0) {
            has_enlargment = true;
            break;
        }
    }
    if (!has_enlargment)
        return;

    const coord_t resolution = std::max(scale_t(m_print->config().resolution / 2), SCALED_EPSILON);

    for (size_t layer_idx = 1; layer_idx < this->layers().size(); layer_idx++) {
        // get supported area
        Layer* my_layer = this->get_layer(layer_idx);
        const Layer* lower_layer = this->get_layer(layer_idx - 1);
        assert(lower_layer == my_layer->lower_layer);

        // check if it's activated somewhere for this layer
        bool has_modifications = false;
        for (size_t region_idx = 0; region_idx < my_layer->m_regions.size(); ++region_idx) {
            if (my_layer->get_region(region_idx)->region().config().overhangs_max_slope.get_abs_value(1.) > 0) {
                has_modifications = true;
                break;
            }
        }
        if (!has_modifications) {
            continue;
        }
        
        ExPolygons all_region_modified;
        const ExPolygons supported_area = ensure_valid(intersection_ex(my_layer->lslices(), lower_layer->lslices()), resolution);

        // get bridgeable area
        for (size_t region_idx = 0; region_idx < my_layer->m_regions.size(); ++region_idx) {
            LayerRegion* lregion = my_layer->get_region(region_idx);
            Flow bridge_flow = lregion->bridging_flow(FlowRole::frSolidInfill);
            ExPolygons bridged_area;
            ExPolygons bridged_other_layers_areas;

            // do we check for our bridge (and maybe the one above?)
            // yes if overhangs_bridge_threshold isn't enabled to 0
            if (lregion->region().config().overhangs_bridge_threshold.value != 0 ||
                    !lregion->region().config().overhangs_bridge_threshold.is_enabled()) {
                const Surfaces & my_surfaces = lregion->m_slices.surfaces;
                ExPolygons unsupported = to_expolygons(my_surfaces);
                unsupported            = diff_ex(unsupported, lower_layer->lslices(), ApplySafetyOffset::Yes);

                if (!unsupported.empty()) {
                    ExPolygons unsupported_filtered;
                    // remove small overhangs (but also good bridges to a cylinder)
                    unsupported_filtered = offset2_ex(unsupported, double(-max_nz_diam/2), double(max_nz_diam), Slic3r::ClipperLib::jtMiter, 5);
                    unsupported_filtered = intersection_ex(unsupported, unsupported_filtered);
                    for (const ExPolygon &to_bridge : unsupported_filtered) {
                        BridgeDetector detector(to_bridge,
                                                lower_layer->lslices(),
                                                bridge_flow.scaled_spacing(),
                                                scale_t(this->print()->config().bridge_precision.get_abs_value(bridge_flow.spacing())),
                                                layer_idx);
                        if (lregion->region().config().overhangs_bridge_threshold.is_enabled()) {
                            detector.max_bridge_length = scale_d(std::max(0., lregion->region().config().overhangs_bridge_threshold.value));
                        } else {
                            detector.max_bridge_length = -1;
                        }
                        if (detector.detect_angle()) {
                            append(bridged_area, union_ex(detector.coverage()));
                        }
                    }
                    // then, check other layers
                    if (lregion->region().config().overhangs_bridge_upper_layers.is_enabled()) { // disabled -> don't check other layers
                        size_t max_layer_idx = lregion->region().config().overhangs_bridge_upper_layers.value;
                        if (max_layer_idx == 0) // 0 -> all layers
                            max_layer_idx = this->layers().size();
                        max_layer_idx += layer_idx;
                        max_layer_idx = std::min(max_layer_idx, this->layers().size());
                        // compute the area still unsupported
                        ExPolygons still_unsupported = diff_ex(unsupported, bridged_area);
                        still_unsupported = intersection_ex(still_unsupported, offset2_ex(still_unsupported, double(-bridge_flow.scaled_spacing()/2), double(bridge_flow.scaled_spacing()),  Slic3r::ClipperLib::jtMiter, 5));
                        // compute the support (without the enlarged part,as we don't know yet where it will be) 
                        ExPolygons previous_supported = supported_area;
                        append(previous_supported, bridged_area);
                        previous_supported = union_safety_offset_ex(previous_supported);
                        for (size_t other_layer_bridge_idx = layer_idx + 1; other_layer_bridge_idx < max_layer_idx; other_layer_bridge_idx++) {
                            // remove new voids
                            still_unsupported = intersection_ex(still_unsupported, this->get_layer(other_layer_bridge_idx)->lslices());
                            //compute bridges
                            ExPolygons new_bridged_area;
                            for (size_t other_region_idx = 0; other_region_idx < my_layer->m_regions.size(); ++other_region_idx) {
                                const LayerRegion *other_lregion = my_layer->get_region(other_region_idx);
                                if ( (other_lregion->region().config().overhangs_bridge_threshold.value != 0 ||
                                        !lregion->region().config().overhangs_bridge_threshold.is_enabled())
                                    && other_lregion->region().config().overhangs_max_slope > 0) {
                                    coord_t enlargement = scale_t(lregion->region().config().overhangs_max_slope.get_abs_value(unscaled(max_nz_diam))); // me or other?
                                    enlargement = std::max(enlargement, max_nz_diam);
                                    const Surfaces &other_surfaces = other_lregion->m_slices.surfaces;
                                    for (const ExPolygon &other_to_bridge : intersection_ex(still_unsupported, to_expolygons(other_surfaces))) {
                                        //collapse too small area
                                        if(offset(other_to_bridge, -enlargement).empty())
                                            continue;
                                        BridgeDetector detector(other_to_bridge, previous_supported, bridge_flow.scaled_spacing(),
                                                     scale_t(this->print()->config().bridge_precision.get_abs_value(bridge_flow.spacing())),
                                                     other_layer_bridge_idx);
                                        detector.layer_id = other_layer_bridge_idx;
                                        if (lregion->region().config().overhangs_bridge_threshold.is_enabled()) {
                                            detector.max_bridge_length = scale_d(std::max(0., other_lregion->region().config().overhangs_bridge_threshold.value));
                                        } else {
                                            detector.max_bridge_length = -1;
                                        }
                                        if (detector.detect_angle()) {
                                            // note: can use to_expolygons on polygons because it's just after a clipper operation, we didn't messed with the order.
                                            new_bridged_area = union_ex(new_bridged_area, detector.coverage());
                                        }
                                    }
                                }
                                // TODO: if overhangs_bridge_upper_layers goes from 2+ to 0, detect that you can't go higher inside the region.
                            }
                            if (!new_bridged_area.empty()) {
                                append(bridged_other_layers_areas, new_bridged_area);
                                // update the area still unsupported
                                still_unsupported = diff_ex(still_unsupported, new_bridged_area);
                                still_unsupported = offset2_ex(still_unsupported, 
                                    double(-bridge_flow.scaled_spacing()/2), double(bridge_flow.scaled_spacing()/2));
                                // update support area from this layer
                                if (other_layer_bridge_idx + 1 < max_layer_idx) {
                                    //previous_supported = diff_ex(this->get_layer(other_layer_bridge_idx)->lslices(), still_unsupported);
                                    previous_supported = union_ex(previous_supported, new_bridged_area);
                                }
                            }
                        }
                    }
                }
            }

            // enlarge supported area & intersect it with full area
            //also modify region surfaces
            //std::map<coord_t, ExPolygons> enlargement_2_support_area;
            // TODO: fuse region with same enlargement
            coord_t enlargement = scale_t(lregion->region().config().overhangs_max_slope.get_abs_value(unscaled(max_nz_diam)));
            if (enlargement > 0) {
                ExPolygons enlarged_support = offset_ex(supported_area, double(enlargement));
                if (!bridged_other_layers_areas.empty()) {
                    bridged_other_layers_areas = union_ex(bridged_other_layers_areas);
                    // only remove  expolygons that are not enclosed inside enlarged_support, because else there's no point.
                    for (ExPolygon &bridged_other_layers_area : bridged_other_layers_areas) {
                        ExPolygons check = diff_ex(bridged_other_layers_area, enlarged_support);
                        if (!check.empty() && (check.size() > 1 || check.front() != bridged_other_layers_area)) {
                            enlarged_support = diff_ex(enlarged_support, bridged_other_layers_area);
                        }
                    }
                }
                enlarged_support = union_safety_offset_ex(enlarged_support);
                ExPolygons max_enlarged_support = offset_ex(enlarged_support, double(enlargement * 0.5));
                ExPolygons min_enlarged_support = union_ex(supported_area,
                                                           offset_ex(enlarged_support, double(-enlargement * 0.5)));
                
                // put bridgeable into supported area (bridges are not enlarged)
                max_enlarged_support = union_ex(max_enlarged_support, bridged_area);
                min_enlarged_support = union_ex(min_enlarged_support, bridged_area);

                // if possible, be sure to not have concave points in unsupported area
                for (ExPolygon &expoly : enlarged_support) {
                    assert(expoly.contour.is_counter_clockwise());
                    smoothen(expoly.contour, std::max(double(enlargement) * 2, double(max_nz_diam * 2)),
                             PI * 5. / 6.);
                    // same with holes (concave as they are in reverse order, this is taken care inside only_convex_or_90deg)
                    for (Polygon &hole : expoly.holes) {
                        assert(hole.is_clockwise());
                        smoothen(expoly.contour, std::max(double(enlargement) * 2, double(max_nz_diam * 2)),
                                 PI * 5. / 6.);
                    }
                }
                enlarged_support = intersection_ex(enlarged_support, max_enlarged_support);
                enlarged_support = union_ex(enlarged_support, min_enlarged_support);
                // modify geometry
                Surfaces to_add;
                Surfaces &my_surfaces = lregion->m_slices.surfaces;
                ExPolygons expolys_final;
                for (size_t surf_idx = 0; surf_idx < my_surfaces.size(); surf_idx++) {
                    ExPolygons expolys = intersection_ex(enlarged_support, my_surfaces[surf_idx].expolygon);
                    // if bridge, smooth enlargment so there won't be spikes near bridges.
                    if (!bridged_other_layers_areas.empty()) {
                        expolys = offset2_ex(expolys, double(-enlargement / 2), double(enlargement / 2));
                    }
                    expolys_final = union_ex(expolys_final, expolys);
                    if (expolys.empty()) {
                        my_surfaces.erase(my_surfaces.begin() + surf_idx);
                        surf_idx--;
                    } else {
                        my_surfaces[surf_idx].expolygon = expolys[0];
                        for (size_t i = 1; i < expolys.size(); i++) {
                            to_add.emplace_back(my_surfaces[surf_idx], expolys[i]);
                        }
                        append(all_region_modified, std::move(expolys));
                    }
                }
                append(my_surfaces, std::move(to_add));
                ensure_valid(my_surfaces, resolution);
                for(auto &srf : my_surfaces) srf.expolygon.assert_valid();
            } else {
                Surfaces &my_surfaces = lregion->m_slices.surfaces;
                for (size_t surf_idx = 0; surf_idx < my_surfaces.size(); surf_idx++) {
                    all_region_modified.push_back(my_surfaces[surf_idx].expolygon);
                }
            }
            all_region_modified = union_ex(all_region_modified);
            ensure_valid(all_region_modified, resolution);
        }
        //also lslices
        all_region_modified = offset_ex(all_region_modified, resolution + SCALED_EPSILON);
        ExPolygons new_lslices = intersection_ex(my_layer->lslices(), all_region_modified);
        ensure_valid(new_lslices, resolution);
        assert_valid(new_lslices);
        // lslices are sorted by topological order from outside to inside from the clipper intersection_ex used above
#ifdef _DEBUG
        if (new_lslices.size() > 1) {
            std::vector<BoundingBox> bboxes;
            bboxes.emplace_back(new_lslices[0].contour.points);
            for (size_t check_idx = 1; check_idx < new_lslices.size(); ++check_idx) {
                assert(bboxes.size() == check_idx);
                bboxes.emplace_back(new_lslices[check_idx].contour.points);
                for (size_t bigger_idx = 0; bigger_idx < check_idx; ++bigger_idx) {
                    // higher idx can be inside holes, but not the opposite!
                    if (bboxes[check_idx].contains(bboxes[bigger_idx])) {
                        assert(!new_lslices[check_idx].contour.contains(new_lslices[bigger_idx].contour.first_point()));
                    }
                }
            }
        }
#endif
        my_layer->set_lslices() = std::move(new_lslices);
        my_layer->lslice_indices_sorted_by_print_order = chain_expolygons(my_layer->lslices());
        assert(my_layer->lslices().size() == my_layer->lslice_indices_sorted_by_print_order.size());
    }
}

Polygons create_polyholes(const Point center, const coord_t radius, const coord_t nozzle_diameter, bool multiple)
{
    // n = max(round(2 * d), 3); // for 0.4mm nozzle
    size_t nb_edges = (int)std::max(3, (int)std::round(4.0 * unscaled(radius) * 0.4 / unscaled(nozzle_diameter)));
    // cylinder(h = h, r = d / cos (180 / n), $fn = n);
    //create x polyholes by rotation if multiple
    int nb_polyhole = 1;
    float rotation = 0;
    if (multiple) {
        nb_polyhole = 5;
        rotation = 2 * float(PI) / (nb_edges * nb_polyhole);
    }
    Polygons list;
    for (int i_poly = 0; i_poly < nb_polyhole; i_poly++)
        list.emplace_back();
    for (int i_poly = 0; i_poly < nb_polyhole; i_poly++) {
        Polygon& pts = (((i_poly % 2) == 0) ? list[i_poly / 2] : list[(nb_polyhole + 1) / 2 + i_poly / 2]);
        const float new_radius = radius / float(std::cos(PI / nb_edges));
        for (size_t i_edge = 0; i_edge < nb_edges; ++i_edge) {
            float angle = rotation * i_poly + (float(PI) * 2 * (float)i_edge) / nb_edges;
            pts.points.emplace_back(center.x() + new_radius * cos(angle), center.y() + new_radius * sin(angle));
        }
        pts.make_clockwise();
    }
    //alternate
    return list;
}

void PrintObject::_transform_hole_to_polyholes()
{
    struct LayerData
    {
        Point center;
        double max_diameter;
        int16_t extruder_id;
        coord_t max_deviation;
        bool is_twist;

        bool operator<(const LayerData &other) {
            return this->is_twist != other.is_twist ? this->is_twist < other.is_twist :
                this->extruder_id != other.extruder_id ? this->extruder_id < other.extruder_id :
                !this->center.coincides_with_epsilon(other.center) ? this->center < other.center :
                this->max_diameter < max_diameter;
        }
    };
    // get all circular holes for each layer
    // the id is center-diameter-extruderid
    //the tuple is Point center; float diameter_max; int extruder_id; coord_t max_variation; bool twist;
    std::vector<std::vector<std::pair<LayerData, Polygon*>>> layerid2center;
    for (size_t i = 0; i < this->m_layers.size(); i++) layerid2center.emplace_back();
    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, m_layers.size()),
        [this, &layerid2center](const tbb::blocked_range<size_t>& range) {
        for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++layer_idx) {
            m_print->throw_if_canceled();
            Layer* layer = m_layers[layer_idx];
            for (size_t region_idx = 0; region_idx < layer->m_regions.size(); ++region_idx)
            {
                if (layer->m_regions[region_idx]->region().config().hole_to_polyhole) {
                    for (Surface& surf : layer->m_regions[region_idx]->m_slices.surfaces) {
                        for (Polygon& hole : surf.expolygon.holes) {
                            //test if convex (as it's clockwise bc it's a hole, we have to do the opposite)
                            if (hole.convex_points(0, PI).empty() && hole.points.size() > 8) {
                                // Computing circle center
                                Point center = hole.centroid();
                                double diameter_min = std::numeric_limits<float>::max(), diameter_max = 0;
                                double diameter_sum = 0;
                                for (int i = 0; i < hole.points.size(); ++i) {
                                    double dist = hole.points[i].distance_to(center);
                                    diameter_min = std::min(diameter_min, dist);
                                    diameter_max = std::max(diameter_max, dist);
                                    diameter_sum += dist;
                                }
                                //also use center of lines to check it's not a rectangle
                                double diameter_line_min = std::numeric_limits<float>::max(), diameter_line_max = 0;
                                Lines hole_lines = hole.lines();
                                for (Line l : hole_lines) {
                                    Point midline = (l.a + l.b) / 2;
                                    double dist = center.distance_to(midline);
                                    diameter_line_min = std::min(diameter_line_min, dist);
                                    diameter_line_max = std::max(diameter_line_max, dist);
                                }


                                // SCALED_EPSILON was a bit too harsh. Now using a config, as some may want some harsh setting and some don't.
                                coord_t max_variation = std::max(SCALED_EPSILON, scale_(this->m_layers[layer_idx]->m_regions[region_idx]->region().config().hole_to_polyhole_threshold.get_abs_value(unscaled(diameter_sum / hole.points.size()))));
                                bool twist = this->m_layers[layer_idx]->m_regions[region_idx]->region().config().hole_to_polyhole_twisted.value;
                                if (diameter_max - diameter_min < max_variation * 2 && diameter_line_max - diameter_line_min < max_variation * 2) {
                                    layerid2center[layer_idx].emplace_back(
                                        LayerData{center, diameter_max, int16_t(layer->m_regions[region_idx]->region().config().perimeter_extruder.value - 1), max_variation, twist}, &hole);
                                }
                            }
                        }
                    }
                }
            }
            // for layer->slices, it will be also replaced later.
        }
    });
    //sort holes per center-diameter
    std::vector<std::pair<LayerData, std::vector<std::pair<Polygon*, int>>>> id2layers2hole;

    //search & find hole that span at least X layers
    const size_t min_nb_layers = 2;
    for (size_t layer_idx = 0; layer_idx < this->m_layers.size(); ++layer_idx) {
        for (size_t hole_idx = 0; hole_idx < layerid2center[layer_idx].size(); ++hole_idx) {
            //get all other same polygons
            const LayerData& id = layerid2center[layer_idx][hole_idx].first;
            float max_z = layers()[layer_idx]->print_z;
            std::vector<std::pair<Polygon*, int>> holes;
            holes.emplace_back(layerid2center[layer_idx][hole_idx].second, layer_idx);
            for (size_t search_layer_idx = layer_idx + 1; search_layer_idx < this->m_layers.size(); ++search_layer_idx) {
                if (layers()[search_layer_idx]->print_z - layers()[search_layer_idx]->height - max_z > EPSILON) break;
                //search an other polygon with same id
                for (size_t search_hole_idx = 0; search_hole_idx < layerid2center[search_layer_idx].size(); ++search_hole_idx) {
                    const LayerData& search_id = layerid2center[search_layer_idx][search_hole_idx].first;
                    if (id.extruder_id == search_id.extruder_id
                        && id.center.distance_to(search_id.center) < id.max_deviation
                        && std::abs(id.max_diameter - search_id.max_diameter) < id.max_deviation
                        ) {
                        max_z = layers()[search_layer_idx]->print_z;
                        holes.emplace_back(layerid2center[search_layer_idx][search_hole_idx].second, search_layer_idx);
                        layerid2center[search_layer_idx].erase(layerid2center[search_layer_idx].begin() + search_hole_idx);
                        search_hole_idx--;
                        break;
                    }
                }
            }
            //check if strait hole or first layer hole (cause of first layer compensation)
            if (holes.size() >= min_nb_layers || (holes.size() == 1 && holes[0].second == 0)) {
                id2layers2hole.emplace_back(std::move(id),std::move(holes));
            }
        }
    }
    //create a polyhole per id and replace holes points by it.
    for (auto &entry : id2layers2hole) {
        Polygons polyholes = create_polyholes(entry.first.center, entry.first.max_diameter, scale_t(print()->config().nozzle_diameter.get_at(entry.first.extruder_id)), entry.first.is_twist);
        for (auto& poly_to_replace : entry.second) {
            Polygon polyhole = polyholes[poly_to_replace.second % polyholes.size()];
            //search the clone in layers->slices
            for (ExPolygon& explo_slice : m_layers[poly_to_replace.second]->set_lslices()) {
                for (Polygon& poly_slice : explo_slice.holes) {
                    if (poly_slice.points == poly_to_replace.first->points) {
                        poly_slice.points = polyhole.points;
                        poly_slice.assert_valid();
                    }
                }
            }
            // copy
            poly_to_replace.first->points = polyhole.points;
        }
    }
    for(auto *layer : m_layers)
        for(auto &region : layer->regions())
            for(auto &srf : region->m_slices)
                srf.expolygon.assert_valid();
}

template<typename ThrowOnCancel>
void apply_mm_segmentation(PrintObject &print_object, ThrowOnCancel throw_on_cancel)
{
    // Returns MMU segmentation based on painting in MMU segmentation gizmo
    std::vector<std::vector<ExPolygons>> segmentation = multi_material_segmentation_by_painting(print_object, throw_on_cancel);
    assert(segmentation.size() == print_object.layer_count());
    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, segmentation.size(), std::max(segmentation.size() / 128, size_t(1))),
        [&print_object, &segmentation, throw_on_cancel](const tbb::blocked_range<size_t> &range) {
            const auto  &layer_ranges   = print_object.shared_regions()->layer_ranges;
            double       z              = print_object.get_layer(range.begin())->slice_z;
            auto         it_layer_range = layer_range_first(layer_ranges, z);
            const size_t num_extruders = print_object.print()->config().nozzle_diameter.size();
            struct ByExtruder {
                ExPolygons  expolygons;
                BoundingBox bbox;
            };
            std::vector<ByExtruder> by_extruder;
            struct ByRegion {
                ExPolygons  expolygons;
                bool        needs_merge { false };
            };
            std::vector<ByRegion> by_region;
            for (size_t layer_id = range.begin(); layer_id < range.end(); ++ layer_id) {
                throw_on_cancel();
                Layer *layer = print_object.get_layer(layer_id);
                it_layer_range = layer_range_next(layer_ranges, it_layer_range, layer->slice_z);
                const PrintObjectRegions::LayerRangeRegions &layer_range = *it_layer_range;
                // Gather per extruder expolygons.
                by_extruder.assign(num_extruders, ByExtruder());
                by_region.assign(layer->region_count(), ByRegion());
                bool layer_split = false;
                for (size_t extruder_id = 0; extruder_id < num_extruders; ++ extruder_id) {
                    ByExtruder &region = by_extruder[extruder_id];
                    append(region.expolygons, std::move(segmentation[layer_id][extruder_id]));
                    if (! region.expolygons.empty()) {
                        region.bbox = get_extents(region.expolygons);
                        layer_split = true;
                    }
                }
                if (! layer_split)
                    continue;
                // Split LayerRegions by by_extruder regions.
                // layer_range.painted_regions are sorted by extruder ID and parent PrintObject region ID.
                auto it_painted_region = layer_range.painted_regions.begin();
                for (int region_id = 0; region_id < int(layer->region_count()); ++ region_id)
                    if (LayerRegion &layerm = *layer->get_region(region_id); ! layerm.slices().empty()) {
                        assert(layerm.region().print_object_region_id() == region_id);
                        const BoundingBox bbox = get_extents(layerm.slices().surfaces);
                        assert(it_painted_region < layer_range.painted_regions.end());
                        // Find the first it_painted_region which overrides this region.
                        for (; layer_range.volume_regions[it_painted_region->parent].region->print_object_region_id() < region_id; ++ it_painted_region)
                            assert(it_painted_region != layer_range.painted_regions.end());
                        assert(it_painted_region != layer_range.painted_regions.end());
                        assert(layer_range.volume_regions[it_painted_region->parent].region == &layerm.region());
                        // 1-based extruder ID
                        bool   self_trimmed = false;
                        int    self_extruder_id = -1;
                        for (int extruder_id = 1; extruder_id <= int(by_extruder.size()); ++ extruder_id)
                            if (ByExtruder &segmented = by_extruder[extruder_id - 1]; segmented.bbox.defined && bbox.overlap(segmented.bbox)) {
                                // Find the target region.
                                for (; int(it_painted_region->extruder_id) < extruder_id; ++ it_painted_region)
                                    assert(it_painted_region != layer_range.painted_regions.end());
                                assert(layer_range.volume_regions[it_painted_region->parent].region == &layerm.region() && int(it_painted_region->extruder_id) == extruder_id);
                                //FIXME Don't trim by self, it is not reliable.
                                if (&layerm.region() == it_painted_region->region) {
                                    self_extruder_id = extruder_id;
                                    continue;
                                }
                                // Steal from this region.
                                int         target_region_id = it_painted_region->region->print_object_region_id();
                                ExPolygons  stolen           = intersection_ex(layerm.slices().surfaces, segmented.expolygons);
                                if (! stolen.empty()) {
                                    ByRegion &dst = by_region[target_region_id];
                                    if (dst.expolygons.empty()) {
                                        dst.expolygons = std::move(stolen);
                                    } else {
                                        append(dst.expolygons, std::move(stolen));
                                        dst.needs_merge = true;
                                    }
                                }
#if 0
                                if (&layerm.region() == it_painted_region->region)
                                    // Slices of this LayerRegion were trimmed by a MMU region of the same PrintRegion.
                                    self_trimmed = true;
#endif
                            }
                        if (! self_trimmed) {
                            // Trim slices of this LayerRegion with all the MMU regions.
                            // prusa has a move() here. I find that dangerous, I'm not sure i won't reuse it afterwards. It's a well hidden silent deletion. That's why i copy here.
                            Polygons mine = to_polygons(layerm.slices().surfaces);
                            for (auto &segmented : by_extruder)
                                if (&segmented - by_extruder.data() + 1 != self_extruder_id && segmented.bbox.defined && bbox.overlap(segmented.bbox)) {
                                    mine = diff(mine, segmented.expolygons);
                                    if (mine.empty())
                                        break;
                                }
                            // Filter out unprintable polygons produced by subtraction multi-material painted regions from layerm.region().
                            // ExPolygon returned from multi-material segmentation does not precisely match ExPolygons in layerm.region()
                            // (because of preprocessing of the input regions in multi-material segmentation). Therefore, subtraction from
                            // layerm.region() could produce a huge number of small unprintable regions for the model's base extruder.
                            // This could, on some models, produce bulges with the model's base color (#7109).
                            if (! mine.empty())
                                mine = opening(union_ex(mine), float(scale_(5 * EPSILON)), float(scale_(5 * EPSILON)));
                            if (! mine.empty()) {
                                ByRegion &dst = by_region[layerm.region().print_object_region_id()];
                                if (dst.expolygons.empty()) {
                                    dst.expolygons = union_ex(mine);
                                } else {
                                    append(dst.expolygons, union_ex(mine));
                                    dst.needs_merge = true;
                                }
                            }
                        }
                    }
                // Re-create Surfaces of LayerRegions.
                for (size_t region_id = 0; region_id < layer->region_count(); ++ region_id) {
                    ByRegion &src = by_region[region_id];
                    if (src.needs_merge)
                        // Multiple regions were merged into one.
                        src.expolygons = closing_ex(src.expolygons, float(scale_(10 * EPSILON)));
                    ensure_valid(src.expolygons);
                    layer->get_region(region_id)->m_slices.set(std::move(src.expolygons), stPosInternal | stDensSparse);
                    for(auto &srf : layer->get_region(region_id)->m_slices) srf.expolygon.assert_valid();
                }
            }
        });
}



ExPolygons PrintObject::_shrink_contour_holes(double contour_delta, double not_convex_delta, double convex_delta, const ExPolygons& polys) const {
    ExPolygons new_ex_polys;
    double max_hole_area = scale_d(scale_d(m_config.hole_size_threshold.value));
    for (const ExPolygon& ex_poly : polys) {
        Polygons contours;
        ExPolygons holes;
        for (const Polygon& hole : ex_poly.holes) {
            assert(hole.points.size() >= 3);
            //check if convex to reduce it
            // check whether first point forms a convex angle
            //note: we allow a deviation of 5.7° (0.01rad = 0.57°)
            bool is_convex = true;
            //ok = (hole.points.front().ccw_angle(hole.points.back(), *(hole.points.begin() + 1)) <= PI + 0.1);
#if _DEBUG
            assert(is_approx(ccw_angle_old_test(hole.points.front(), hole.points.back(), *(hole.points.begin() + 1)), 
                abs_angle(angle_ccw( hole.points.back() - hole.points.front(),*(hole.points.begin() + 1) - hole.points.front())), 0.000000001));
#endif
            is_convex = (abs_angle(angle_ccw( hole.points.back() - hole.points.front(),*(hole.points.begin() + 1) - hole.points.front())) <= PI + 0.1);
            // check whether points 1..(n-1) form convex angles
            if (is_convex)
                for (Points::const_iterator p = hole.points.begin() + 1; p != hole.points.end() - 1; ++p) {
                    //ok = (p->ccw_angle(*(p - 1), *(p + 1)) <= PI + 0.1);
#if _DEBUG
                    assert(is_approx(ccw_angle_old_test(*p, *(p - 1), *(p + 1)), abs_angle(angle_ccw((*(p - 1)) - *p, (*(p + 1)) - *p)), 0.000000001));
#endif
                    is_convex = (abs_angle(angle_ccw((*(p - 1)) - *p, (*(p + 1)) - *p)) <= PI + 0.1);
                    if (!is_convex) break;
                }

            // check whether last point forms a convex angle
            //ok &= (hole.points.back().ccw_angle(*(hole.points.end() - 2), hole.points.front()) <= PI + 0.1);
#if _DEBUG
            assert(is_approx(ccw_angle_old_test(hole.points.back(), *(hole.points.end() - 2), hole.points.front()),
                abs_angle(angle_ccw(*(hole.points.end() - 2) - hole.points.back(), hole.points.front() - hole.points.back())), 0.000000001));
#endif
            is_convex &= (abs_angle(angle_ccw(*(hole.points.end() - 2) - hole.points.back(), \
                            hole.points.front() - hole.points.back())) <= PI + 0.1);

            if (is_convex && not_convex_delta != convex_delta) {
                if (convex_delta != 0) {
                    //apply hole threshold cutoff
                    double convex_delta_adapted = convex_delta;
                    double area = -hole.area();
                    if (area > max_hole_area * 4 && max_hole_area > 0) {
                        convex_delta_adapted = not_convex_delta;
                    } else if (area > max_hole_area && max_hole_area > 0) {
                        // not a hard threshold, to avoid artefacts on slopped holes.
                        double percent = (max_hole_area * 4 - area) / (max_hole_area * 3);
                        convex_delta_adapted = convex_delta * percent + (1 - percent) * not_convex_delta;
                    }
                    if (convex_delta_adapted != 0) {
                        Polygon hole_as_contour = hole;
                        hole_as_contour.make_counter_clockwise();
                        for (ExPolygon& newHole : offset_ex(ExPolygon{ hole_as_contour }, -convex_delta_adapted)) {
                            holes.push_back(std::move(newHole));
                        }
                    } else {
                        holes.push_back(ExPolygon{ hole });
                        holes.back().contour.make_counter_clockwise();
                    }
                } else {
                    holes.push_back(ExPolygon{ hole });
                    holes.back().contour.make_counter_clockwise();
                }
            } else {
                if (not_convex_delta != 0) {
                    Polygon hole_as_contour = hole;
                    hole_as_contour.make_counter_clockwise();
                    for (ExPolygon& newHole : offset_ex(ExPolygon{ hole_as_contour }, -not_convex_delta)) {
                        holes.push_back(std::move(newHole));
                    }
                } else {
                    holes.push_back(ExPolygon{ hole });
                    holes.back().contour.make_counter_clockwise();
                }
            }
        }
        //modify contour
        if (contour_delta != 0) {
            Polygons new_contours = offset(ex_poly.contour, contour_delta);
            if (new_contours.size() == 0)
                continue;
            contours.insert(contours.end(), std::make_move_iterator(new_contours.begin()), std::make_move_iterator(new_contours.end()));
        } else {
            contours.push_back(ex_poly.contour);
        }
        ExPolygons temp = diff_ex(union_ex(contours), union_ex(holes));
        new_ex_polys.insert(new_ex_polys.end(), std::make_move_iterator(temp.begin()), std::make_move_iterator(temp.end()));
    }
    return union_ex(new_ex_polys);
}

/// max angle: you ahve to be lwer than that to divide it. PI => all accepted
/// min angle: don't smooth sharp angles! 0  => all accepted
/// cutoff_dist: maximum dist between two point to add new points
/// max dist : maximum distance between two pointsd, where we add new points
Polygon _smooth_curve(Polygon& p, double max_angle, double min_angle_convex, double min_angle_concave, coord_t cutoff_dist, coord_t max_dist) {
    if (p.size() < 4) return p;
    Polygon pout;
    //duplicate points to simplify the loop
    p.points.insert(p.points.end(), p.points.begin(), p.points.begin() + 3);
    for (size_t idx = 1; idx < p.size() - 2; idx++) {
        //put first point
        pout.points.push_back(p[idx]);
        //get angles
        //double angle1 = p[idx].ccw_angle(p.points[idx - 1], p.points[idx + 1]);
#if _DEBUG
        assert(is_approx(ccw_angle_old_test(p[idx], p.points[idx - 1], p.points[idx + 1]), abs_angle(angle_ccw( p.points[idx - 1] - p[idx],p.points[idx + 1] - p[idx])),EPSILON));
#endif
        double angle1 = abs_angle(angle_ccw(p.points[idx - 1] - p[idx], p.points[idx + 1] - p[idx]));
        bool angle1_concave = true;
        if (angle1 > PI) {
            angle1 = 2 * PI - angle1;
            angle1_concave = false;
        }
        //double angle2 = p[idx + 1].ccw_angle(p.points[idx], p.points[idx + 2]);
#if _DEBUG
        assert(is_approx(ccw_angle_old_test(p[idx + 1], p.points[idx], p.points[idx + 2]), abs_angle(angle_ccw( p.points[idx] - p[idx + 1],p.points[idx + 2] - p[idx + 1])),EPSILON));
#endif
        double angle2 = abs_angle(angle_ccw(p.points[idx] - p[idx + 1], p.points[idx + 2] - p[idx + 1]));
        bool angle2_concave = true;
        if (angle2 > PI) {
            angle2 = 2 * PI - angle2;
            angle2_concave = false;
        }
        //filters
        bool angle1_ok = angle1_concave ? angle1 >= min_angle_concave : angle1 >= min_angle_convex;
        bool angle2_ok = angle2_concave ? angle2 >= min_angle_concave : angle2 >= min_angle_convex;
        if (!angle1_ok && !angle2_ok) continue;
        if (angle1 > max_angle && angle2 > max_angle) continue;
        if (cutoff_dist > 0 && p.points[idx].distance_to(p.points[idx + 1]) > cutoff_dist) continue;
        // add points, but how many?
        coordf_t dist = p[idx].distance_to(p[idx + 1]);
        int nb_add = dist / max_dist;
        if (max_angle < PI) {
            int nb_add_per_angle = std::max((PI - angle1) / (PI - max_angle), (PI - angle2) / (PI - max_angle));
            nb_add = std::min(nb_add, nb_add_per_angle);
        }
        if (nb_add == 0) continue;

        //cr�ation des points de controles
        Vec2d vec_ab = (p[idx] - p[idx - 1]).cast<double>();
        Vec2d vec_bc = (p[idx + 1] - p[idx]).cast<double>();
        Vec2d vec_cb = (p[idx] - p[idx + 1]).cast<double>();
        Vec2d vec_dc = (p[idx + 1] - p[idx + 2]).cast<double>();
        vec_ab.normalize();
        vec_bc.normalize();
        vec_cb.normalize();
        vec_dc.normalize();
        Vec2d vec_b_tang = vec_ab + vec_bc;
        vec_b_tang.normalize();
        //should be 0.55 / 1.414 = ~0.39 to create a true circle from a square (90°)
        // it's ~0.36 for exagon (120°)
        // it's ~0.34 for octogon (135°)
        vec_b_tang *= dist * (0.31 + 0.12 * (1 - (angle1 / PI)));
        Vec2d vec_c_tang = vec_dc + vec_cb;
        vec_c_tang.normalize();
        vec_c_tang *= dist * (0.31 + 0.12 * (1 - (angle2 / PI)));
        Point bp = p[idx] + ((!angle1_ok) ? vec_bc.cast<coord_t>() : vec_b_tang.cast<coord_t>());
        Point cp = p[idx + 1] + ((!angle2_ok) ? vec_cb.cast<coord_t>() : vec_c_tang.cast<coord_t>());
        for (int idx_np = 0; idx_np < nb_add; idx_np++) {
            const float percent_np = (idx_np + 1) / (float)(nb_add + 1);
            const float inv_percent_np = 1 - percent_np;
            pout.points.emplace_back();
            Point& new_p = pout.points.back();
            const float coeff0 = inv_percent_np * inv_percent_np * inv_percent_np;
            const float coeff1 = percent_np * inv_percent_np * inv_percent_np;
            const float coeff2 = percent_np * percent_np * inv_percent_np;
            const float coeff3 = percent_np * percent_np * percent_np;
            new_p.x() = (p[idx].x() * coeff0)
                + (3 * bp.x() * coeff1)
                + (3 * cp.x() * coeff2)
                + (p[idx + 1].x() * coeff3);
            new_p.y() = (p[idx].y() * coeff0)
                + (3 * bp.y() * coeff1)
                + (3 * cp.y() * coeff2)
                + (p[idx + 1].y() * coeff3);
        }

    }
    return pout;
}

ExPolygons PrintObject::_smooth_curves(const ExPolygons& input, const PrintRegionConfig& conf) const {
    ExPolygons new_polys;
    for (const ExPolygon& ex_poly : input) {
        ExPolygon new_ex_poly(ex_poly);
        new_ex_poly.contour.remove_collinear(SCALED_EPSILON * 10);
        new_ex_poly.contour = _smooth_curve(new_ex_poly.contour, PI,
            conf.curve_smoothing_angle_convex.value * PI / 180.0,
            conf.curve_smoothing_angle_concave.value * PI / 180.0,
            scale_(conf.curve_smoothing_cutoff_dist.value),
            scale_(conf.curve_smoothing_precision.value));
        for (Polygon& phole : new_ex_poly.holes) {
            phole.reverse(); // make_counter_clockwise();
            phole.remove_collinear(SCALED_EPSILON * 10);
            phole = _smooth_curve(phole, PI,
                conf.curve_smoothing_angle_convex.value * PI / 180.0,
                conf.curve_smoothing_angle_concave.value * PI / 180.0,
                scale_(conf.curve_smoothing_cutoff_dist.value),
                scale_(conf.curve_smoothing_precision.value));
            phole.reverse(); // make_clockwise();
        }
        new_polys.push_back(new_ex_poly);
    }
    return new_polys;
}

// 1) Decides Z positions of the layers,
// 2) Initializes layers and their regions
// 3) Slices the object meshes
// 4) Slices the modifier meshes and reclassifies the slices of the object meshes by the slices of the modifier meshes
// 5) Applies size compensation (offsets the slices in XY plane)
// 6) Replaces bad slices by the slices reconstructed from the upper/lower layer
// Resulting expolygons of layer regions are marked as Internal.
//
// this should be independent
void PrintObject::slice_volumes()
{
    BOOST_LOG_TRIVIAL(info) << "Slicing volumes..." << log_memory_info();
    const Print *print                      = this->print();
    const auto   throw_on_cancel_callback   = std::function<void()>([print](){ print->throw_if_canceled(); });

    // Clear old LayerRegions, allocate for new PrintRegions.
    for (Layer* layer : m_layers) {
        layer->m_regions.clear();
        layer->m_regions.reserve(m_shared_regions->all_regions.size());
        for (const std::shared_ptr<PrintRegion> &pr : m_shared_regions->all_regions)
            layer->m_regions.emplace_back(new LayerRegion(layer, pr.get()));
    }

    std::vector<float>                   slice_zs      = zs_from_layers(m_layers);
    std::vector<VolumeSlices> volume_slices = slice_volumes_inner(
        print->config(),
        this->config(),
        this->trafo_centered(),
        this->model_object()->volumes,
        m_shared_regions->layer_ranges,
        slice_zs,
        throw_on_cancel_callback);

    std::vector<std::vector<ExPolygons>> region_slices = slices_to_regions(
        print->config(),
        *this,
        this->model_object()->volumes, 
        *m_shared_regions, 
        slice_zs,
        std::move(volume_slices),
        throw_on_cancel_callback);

    for (size_t region_id = 0; region_id < region_slices.size(); ++ region_id) {
        std::vector<ExPolygons> &by_layer = region_slices[region_id];
        for (size_t layer_id = 0; layer_id < by_layer.size(); ++ layer_id) {
            ensure_valid(by_layer[layer_id]);
            m_layers[layer_id]->regions()[region_id]->m_slices.append(std::move(by_layer[layer_id]), stPosInternal | stDensSparse);
            for(auto &srf : m_layers[layer_id]->regions()[region_id]->m_slices) srf.expolygon.assert_valid();
        }
    }
    region_slices.clear();

    BOOST_LOG_TRIVIAL(debug) << "Slicing volumes - removing top empty layers";
    while (! m_layers.empty()) {
        const Layer *layer = m_layers.back();
        if (! layer->empty())
            break;
        delete layer;
        m_layers.pop_back();
    }
    if (! m_layers.empty())
        m_layers.back()->upper_layer = nullptr;
    m_print->throw_if_canceled();

    // Is any ModelVolume MMU painted?
    if (const auto& volumes = this->model_object()->volumes;
        m_print->config().nozzle_diameter.size() > 1 &&
        std::find_if(volumes.begin(), volumes.end(), [](const ModelVolume* v) { return !v->mm_segmentation_facets.empty(); }) != volumes.end()) {

        // If XY Size compensation is also enabled, notify the user that XY Size compensation
        // would not be used because the object is multi-material painted.
        if (m_config.xy_size_compensation.value != 0.f || m_config.xy_inner_size_compensation.value != 0.f || m_config.hole_size_compensation.value != 0.f) {
            this->active_step_add_warning(
                PrintStateBase::WarningLevel::CRITICAL,
                _u8L("An object has enabled XY Size compensation which will not be used because it is also multi-material painted.\nXY Size "
                  "compensation cannot be combined with multi-material painting.") +
                    "\n" + (_u8L("Object name")) + ": " + this->model_object()->name);
        }

        BOOST_LOG_TRIVIAL(debug) << "Slicing volumes - MMU segmentation";
        apply_mm_segmentation(*this, [print]() { print->throw_if_canceled(); });
    }


    BOOST_LOG_TRIVIAL(debug) << "Slicing volumes - make_slices in parallel - begin";
    {
        // Compensation value, scaled. Only applying the negative scaling here, as the positive scaling has already been applied during slicing.
        ////const size_t num_extruders = print->config().nozzle_diameter.size();
        ////const auto   xy_compensation_scaled            = (num_extruders > 1 && this->is_mm_painted()) ? scaled<float>(0.f) : scaled<float>(std::min(m_config.xy_size_compensation.value, 0.));
        ////const float  elephant_foot_compensation_scaled = (m_config.raft_layers == 0) ?
        ////	// Only enable Elephant foot compensation if printing directly on the print bed.
        ////    float(scale_(m_config.elefant_foot_compensation.value)) :
        ////	0.f;
        // Uncompensated slices for the first layer in case the Elephant foot compensation is applied.
	    //ExPolygons  lslices_1st_layer;
        Slic3r::parallel_for(size_t(0), m_layers.size(),
            [this](const size_t layer_id) {
	                m_print->throw_if_canceled();
	                Layer *layer = m_layers[layer_id];
	                // Apply size compensation and perform clipping of multi-part objects.
                    coord_t outter_delta = scale_t(m_config.xy_size_compensation.value);
                    coord_t inner_delta = scale_t(m_config.xy_inner_size_compensation.value);
                    coord_t hole_delta = inner_delta + scale_t(m_config.hole_size_compensation.value);
                    //FIXME only apply the compensation if no raft is enabled.
                    coord_t first_layer_compensation = 0;
                    const int first_layers = m_config.first_layer_size_compensation_layers.value;
                    if (layer_id < first_layers && layer_id >= m_config.raft_layers && m_config.first_layer_size_compensation.value != 0) {
                        // Only enable Elephant foot compensation if printing directly on the print bed.
                        first_layer_compensation = scale_t(m_config.first_layer_size_compensation.value);
                        // reduce first_layer_compensation for every layer over the first one.
                        first_layer_compensation = coord_t( (coordf_t(first_layers - layer_id) * first_layer_compensation) / coordf_t(first_layers));
                        // simplify compensations if possible
                        outter_delta += first_layer_compensation;
                        inner_delta += first_layer_compensation;
                        hole_delta += first_layer_compensation;
                        first_layer_compensation = 0;
                    }
                    // preparation to apply the elephant foot compensation: grow what will be shrunk
                    if (layer_id < first_layers && layer_id >= m_config.raft_layers && m_config.first_layer_size_compensation_no_collapse.value) {
                        first_layer_compensation = std::min(coord_t(0), std::min(hole_delta, std::min(inner_delta, outter_delta)));
                        assert(first_layer_compensation <= 0);
                        //hole_delta -= first_layer_compensation;
                        //inner_delta -= first_layer_compensation;
                        //outter_delta -= first_layer_compensation;
                    } else {
                        first_layer_compensation = 0;
                    }
                    
                    //remove the upscaling done by the slicing
                    const coord_t aleady_done_delta = is_mm_painted() ? coord_t(0) : std::max(coord_t(0), std::min(outter_delta, std::min(inner_delta, hole_delta)));
                    outter_delta -= aleady_done_delta;
                    inner_delta -= aleady_done_delta;
                    hole_delta -= aleady_done_delta;


                    coord_t scaled_resolution = std::max(scale_t(m_print->config().resolution), SCALED_EPSILON);
                    //TODO: test it's done for multi-region and not
	                if (layer->regions().size() == 1) {
	                    // Optimized version for a single region layer.
	                    // Single region, growing or shrinking.
                        LayerRegion* layerm = layer->regions().front();
                        // we can move here because we'll fill it again below.
                        ExPolygons expolygons = to_expolygons(std::move(layerm->m_slices.surfaces));
                        // Apply all three main XY compensation.
                        if (hole_delta > 0 || inner_delta > 0 || outter_delta > 0) {
                            expolygons = _shrink_contour_holes(std::max(coord_t(0), outter_delta), std::max(coord_t(0), inner_delta), std::max(coord_t(0), hole_delta), expolygons);
                        }
                        // Apply the elephant foot compensation.
                        if (first_layer_compensation < 0) {
                            ExPolygons trim_first_layer = expolygons;
                            expolygons = union_ex(Slic3r::elephant_foot_compensation(expolygons, layerm->flow(frExternalPerimeter), unscaled(-first_layer_compensation)));
                            // regrow
                            assert(first_layer_compensation <= std::min(hole_delta, std::min(inner_delta, outter_delta)));
                            expolygons = _shrink_contour_holes(std::min(coord_t(0), outter_delta) - first_layer_compensation, std::min(coord_t(0), inner_delta) - first_layer_compensation, std::min(coord_t(0), hole_delta) - first_layer_compensation, expolygons);
                            //trim
                            expolygons = intersection_ex(expolygons, trim_first_layer);
                        } else if (hole_delta < 0 || inner_delta < 0 || outter_delta < 0) {
                            // Apply all three main negative XY compensation normally.
                            expolygons = _shrink_contour_holes(std::min(coord_t(0), outter_delta), std::min(coord_t(0), inner_delta), std::min(coord_t(0), hole_delta), expolygons);
                        }
                        if (layer->regions().front()->region().config().curve_smoothing_precision > 0.f) {
                            //smoothing
                            expolygons = _smooth_curves(expolygons, layer->regions().front()->region().config());
                        }
                        ensure_valid(expolygons);
                        layerm->m_slices.set(std::move(expolygons), stPosInternal | stDensSparse);
                    } else {
                        bool same_curve_smoothing = true;
                        for (size_t region_id = 1; same_curve_smoothing && region_id < layer->regions().size(); ++region_id) {
                            same_curve_smoothing = layer->regions()[region_id -1]->region().config().curve_smoothing_precision.value == layer->regions()[region_id -1]->region().config().curve_smoothing_precision.value
                                && layer->regions()[region_id -1]->region().config().curve_smoothing_angle_concave.value == layer->regions()[region_id -1]->region().config().curve_smoothing_angle_concave.value
                                && layer->regions()[region_id -1]->region().config().curve_smoothing_angle_convex.value == layer->regions()[region_id -1]->region().config().curve_smoothing_angle_convex.value
                                && layer->regions()[region_id -1]->region().config().curve_smoothing_cutoff_dist.value == layer->regions()[region_id -1]->region().config().curve_smoothing_cutoff_dist.value;
                        }

                        //growth
                        if (hole_delta > 0 || inner_delta > 0 || outter_delta > 0) {
                            //merge polygons because region can cut "holes".
                            //then, cut them to give them again later to their region
                            ExPolygons merged_poly = layer->merged();
                            ExPolygons trimming = merged_poly;
                            trimming = _shrink_contour_holes(std::max(coord_t(0), outter_delta), std::max(coord_t(0), inner_delta), std::max(coord_t(0), hole_delta), union_ex(trimming));
                            // Multiple regions, growing or just clipping one region by the other.
                            // When clipping the regions, priority is given to the first regions.
                            if (same_curve_smoothing && layer->regions().front()->region().config().curve_smoothing_precision > 0.) {
                                trimming = _smooth_curves(trimming, layer->regions().front()->region().config());
                            }
                            // trim surfaces
                            for (size_t region_id = 0; region_id < layer->regions().size(); ++region_id) {
                                LayerRegion* layerm = layer->regions()[region_id];
                                // get
                                ExPolygons slices = to_expolygons(std::move(layerm->slices().surfaces));
                                ExPolygons other_base_slices = diff_ex(merged_poly, slices);
                                // clip with current growth
                                coord_t max_growth = 0;
                                max_growth += std::max(hole_delta, std::max(inner_delta, outter_delta));
                                if (layerm->region().config().curve_smoothing_precision == 0. || same_curve_smoothing) {
                                    slices = intersection_ex(offset_ex(slices, max_growth), trimming);
                                } else {
                                    //smooth curve
                                    ExPolygons trimming_smooth_curve = _smooth_curves(trimming, layerm->region().config());
                                    slices = intersection_ex(offset_ex(slices, max_growth + layerm->region().config().curve_smoothing_precision), trimming_smooth_curve);
                                }
                                // clip with other region areas
                                slices = diff_ex(slices, other_base_slices);
                                //store
                                ensure_valid(slices);
                                layerm->m_slices.set(std::move(slices), stPosInternal | stDensSparse);
                            }
                        }
                        //shrink
                        if (hole_delta < 0 || inner_delta < 0 || outter_delta < 0 || first_layer_compensation < 0) {
                            // Apply the negative XY compensation. (the ones that is <0)
                            ExPolygons merged_poly = layer->merged();
                            ExPolygons trimming;
                            if (first_layer_compensation < 0) {
                                const coord_t eps = scale_t(m_config.slice_closing_radius.value * 1.5);
                                trimming = offset_ex(layer->merged(eps), -eps);
                            } else {
                                trimming = merged_poly;
                            }
                            if (first_layer_compensation < 0) {
                                Flow min_ext_peri_flow = layer->regions().front()->flow(frExternalPerimeter);
                                for (size_t region_id = 1; region_id < layer->regions().size(); ++region_id) {
                                    Flow ext_peri_flow = layer->regions()[region_id]->flow(frExternalPerimeter);
                                    if (ext_peri_flow.width() < min_ext_peri_flow.width()) {
                                        min_ext_peri_flow = ext_peri_flow;
                                    }
                                }
                                ExPolygons trim_first_layer = trimming;
                                trimming = union_ex(Slic3r::elephant_foot_compensation(trimming, min_ext_peri_flow, unscaled(-first_layer_compensation)));
                                // regrow
                                assert(first_layer_compensation <= std::min(hole_delta, std::min(inner_delta, outter_delta)));
                                trimming = _shrink_contour_holes(std::min(coord_t(0), outter_delta) - first_layer_compensation, std::min(coord_t(0), inner_delta) - first_layer_compensation, std::min(coord_t(0), hole_delta) - first_layer_compensation, trimming);
                                //trim
                                trimming = intersection_ex(trimming, trim_first_layer);
                            } else if (hole_delta < 0 || inner_delta < 0 || outter_delta < 0) {
                                trimming = _shrink_contour_holes(std::min(coord_t(0), outter_delta), std::min(coord_t(0), inner_delta), std::min(coord_t(0), hole_delta), trimming);
                            }
                            if (same_curve_smoothing && layer->regions().front()->region().config().curve_smoothing_precision > 0.) {
                                trimming = _smooth_curves(trimming, layer->regions().front()->region().config());
                            }
                            // trim surfaces
                            for (size_t region_id = 0; region_id < layer->regions().size(); ++region_id) {
                                LayerRegion* layerm = layer->regions()[region_id];
                                // get
                                ExPolygons slices = to_expolygons(std::move(layerm->slices().surfaces));
                                ExPolygons other_base_slices = diff_ex(merged_poly, slices);
                                // clip
                                if (layerm->region().config().curve_smoothing_precision == 0. || same_curve_smoothing) {
                                    slices = intersection_ex(slices, trimming);
                                }else{
                                    //smooth curve
                                    ExPolygons trimming_smooth_curve = _smooth_curves(trimming, layerm->region().config());
                                    slices = intersection_ex(offset_ex(slices, layerm->region().config().curve_smoothing_precision), trimming_smooth_curve);
                                }
                                // clip with other region areas
                                slices = diff_ex(slices, other_base_slices);
                                //store
                                ensure_valid(slices);
                                layerm->m_slices.set(std::move(slices), stPosInternal | stDensSparse);
                            }
                        }
                    }
	                // Merge all regions' slices to get islands, sorted topologically, chain them by a shortest path in separate index list
	                layer->make_slices();
                    //FIXME: can't make it work in multi-region object, it seems useful to avoid bridge on top of first layer compensation
                    //so it's disable, if you want an offset, use the offset field.
                    //if (layer->regions().size() == 1 && ! m_layers.empty() && layer_id == 0 && first_layer_compensation < 0 && m_config.raft_layers == 0) {
                    //    // The Elephant foot has been compensated, therefore the 1st layer's lslices() are shrank with the Elephant foot compensation value.
                    //    // Store the uncompensated value there.
                    //    assert(! m_layers.empty());
                    //    assert(m_layers.front()->id() == 0);
                    //    m_layers.front()->set_lslices() = offset_ex(std::move(m_layers.front()->lslices()), -first_layer_compensation);
                    //    m_layers.front()->lslice_indices_sorted_by_print_order = chain_expolygons(layer.lslices());
                    //}
                    for(auto &layerm : layer->regions()) for(auto &srf : layerm->slices().surfaces) srf.expolygon.assert_valid();
                }
            );
    }

    m_print->throw_if_canceled();
    BOOST_LOG_TRIVIAL(debug) << "Slicing volumes - make_slices in parallel - end";
}

std::vector<ExPolygons> PrintObject::slice_support_volumes(const ModelVolumeType model_volume_type) const
{
    auto it_volume     = this->model_object()->volumes.begin();
    auto it_volume_end = this->model_object()->volumes.end();
    for (; it_volume != it_volume_end && (*it_volume)->type() != model_volume_type; ++ it_volume) ;
    std::vector<ExPolygons> slices;
    if (it_volume != it_volume_end) {
        // Found at least a single support volume of model_volume_type.
        std::vector<float> zs = zs_from_layers(this->layers());
        std::vector<char>  merge_layers;
        bool               merge = false;
        const Print       *print = this->print();
        auto               throw_on_cancel_callback = std::function<void()>([print](){ print->throw_if_canceled(); });
        MeshSlicingParamsEx params;
        params.trafo = this->trafo_centered();
        for (; it_volume != it_volume_end; ++ it_volume)
            if ((*it_volume)->type() == model_volume_type) {
                std::vector<ExPolygons> slices2 = slice_volume(*(*it_volume), zs, params, throw_on_cancel_callback);
                if (slices.empty()) {
                    slices = std::move(slices2);
                } else if (!slices2.empty()) {
                    if (merge_layers.empty())
                        merge_layers.assign(zs.size(), false);
                    for (size_t i = 0; i < zs.size(); ++ i) {
                        if (slices[i].empty())
                            slices[i] = std::move(slices2[i]);
                        else if (! slices2[i].empty()) {
                            append(slices[i], std::move(slices2[i]));
                            merge_layers[i] = true;
                            merge = true;
                        }
                    }
                }
            }
        if (merge) {
            std::vector<ExPolygons*> to_merge;
            to_merge.reserve(zs.size());
            for (size_t i = 0; i < zs.size(); ++ i) {
                if (merge_layers[i]) {
                    to_merge.emplace_back(&slices[i]);
                }
            }
            Slic3r::parallel_for(size_t(0), to_merge.size(),
                [&to_merge](const size_t i) {
                        *to_merge[i] = union_ex(*to_merge[i]);
            });
        }
    }
    return slices;
}

} // namespace Slic3r
