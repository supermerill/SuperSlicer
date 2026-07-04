///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
/*
Auxiliary layer helpers build normal Layer/LayerRegion/LayerIsland structures
from geometry that is already 2D. Unlike object slicing, the input subject is
the printable material: model parts and modifiers only decide which PrintRegion
settings apply to each part of that subject. Negative volumes are ignored here
because auxiliary geometry such as brim, skirt, support, or plugin-owned paths
should opt into subtractive behavior explicitly.
*/
#include "AuxiliaryLayerHelpers.hpp"

#include <cassert>
#include <vector>

#include "libslic3r/Api/plugin/cpp/ClipperViews.hpp"

namespace slic3r_api {
namespace {

// Return true when a layer-range or volume bbox is active at the requested
// object-local Z. The C ABI bbox is unscaled because it comes from mesh space.
bool contains_z(const c_bounding_box3f &bbox, double z_mm);

// Choose the same layer-range boundary convention as STEP_SLICING: when Z is
// exactly on a shared boundary, the upper range owns the slice.
const slicing_layer_range_handle *layer_range_for_z(const Object &object, coord_t slice_z);

// Build the low-level mesh slicing parameters used to slice region masks. The
// extra XY offset is deliberately zero because masks select settings, not
// printable object material.
c_mesh_slicing_params mask_slicing_params(const Print &print, const Object &object);

// Slice one volume at one Z and clip it to the auxiliary subject. The returned
// collection is empty when the volume is not active at this layer.
StoredExPolygonCollection slice_volume_mask(storage_handle *storage,
                                            const Print &print,
                                            const Object &object,
                                            const SlicingVolumeRegion &volume_region,
                                            const ExPolygonCollection &subject,
                                            coord_t slice_z);

// Fill one auxiliary layer's regions from the subject and the applicable
// model-part/modifier masks. Region zero starts with the whole subject; later
// masks steal their area from every previous region before being added to their
// target region.
void assign_regions_from_masks(storage_handle *storage,
                               const Print &print,
                               const Object &object,
                               layer_handle *layer,
                               const ExPolygonCollection &subject,
                               coord_t slice_z);

bool contains_z(const c_bounding_box3f &bbox, double z_mm)
{
    return bbox.min.z <= z_mm + EPSILON && bbox.max.z >= z_mm - EPSILON;
}

const slicing_layer_range_handle *layer_range_for_z(const Object &object, coord_t slice_z)
{
    const uint32_t range_count = object_slicing_layer_range_count(object);
    const slicing_layer_range_handle *selected = nullptr;

    // Scan the object ranges in their stored order and keep the range whose Z
    // interval contains the requested slice. Shared boundaries belong to the
    // upper range, matching the object slicing step.
    for (uint32_t range_idx = 0; range_idx < range_count; ++range_idx) {
        const slicing_layer_range_handle *range = object_get_slicing_layer_range(object.handle(), range_idx);
        if (range == nullptr)
            continue;
        const coord_t z_min = slicing_layer_range_get_z_min(range);
        const coord_t z_max = slicing_layer_range_get_z_max(range);
        if (z_min <= slice_z && slice_z <= z_max) {
            selected = range;
            if (slice_z < z_max)
                break;
        }
    }
    return selected;
}

c_mesh_slicing_params mask_slicing_params(const Print &print, const Object &object)
{
    const Config print_config = print.config();
    const Config object_config = object.config();

    // Mirror the object's slicing mode so modifier masks follow the same mesh
    // interpretation as the real object slice.
    c_mesh_slicing_params params = {};
    const int32_t slicing_mode = object_config.enum_or_default("slicing_mode", 0);
    if (slicing_mode == 1)
        params.mode = RAW_MESH_SLICING_MODE_EVEN_ODD;
    else if (slicing_mode == 2)
        params.mode = RAW_MESH_SLICING_MODE_POSITIVE;
    else
        params.mode = RAW_MESH_SLICING_MODE_REGULAR;

    // Auxiliary masks select settings only. They use the object transform and
    // resolution rules, but no XY expansion because the subject is already the
    // printable area to classify.
    params.mode_below = params.mode;
    params.transform = object.transform_centered();
    params.closing_radius = float(object_config.float_or_default("slice_closing_radius", 0.0));
    params.extra_offset = 0.0f;
    params.resolution = print_config.float_or_default("resolution", EPSILON);
    params.model_resolution = object_config.float_or_default("model_precision", 0.0);
    return params;
}

StoredExPolygonCollection slice_volume_mask(storage_handle *storage,
                                            const Print &print,
                                            const Object &object,
                                            const SlicingVolumeRegion &volume_region,
                                            const ExPolygonCollection &subject,
                                            coord_t slice_z)
{
    StoredExPolygonCollection empty(storage);
    if (subject.empty())
        return empty;

    // Avoid slicing meshes whose cached bounding box cannot touch this Z. This
    // is only a fast rejection; the real geometry is still clipped below.
    const double z_mm = unscaled(slice_z);
    if (!contains_z(volume_region.bbox(), z_mm))
        return empty;

    // Only model parts and modifiers provide settings masks for auxiliary
    // layers. Negative and special volumes are deliberately ignored here.
    const volume_handle *volume_handle = slicing_volume_region_get_volume(volume_region.handle());
    if (volume_handle == nullptr)
        return empty;

    const Volume volume(volume_handle);
    if (!volume.is_model_part() && !volume.is_modifier())
        return empty;

    // Slice this one volume at the requested Z. The mesh slicer returns the
    // volume's mask in object coordinates, not yet limited to the auxiliary
    // subject.
    const c_mesh_slicing_params params = mask_slicing_params(print, object);
    const std::vector<float> zs{float(z_mm)};
    std::vector<StoredExPolygonCollection> slices =
        volume.mesh().slice_to_expolygons(storage, params, zs);
    if (slices.empty() || slices.front().empty())
        return empty;

    // The mask may extend beyond the helper geometry. Clip it to the subject so
    // every returned polygon belongs to the auxiliary layer being built.
    ClipperContext clipper(storage);
    return clipper_intersection(clipper(slices.front()), clipper(subject)).to_expolygon_collection();
}

void assign_regions_from_masks(storage_handle *storage,
                               const Print &print,
                               const Object &object,
                               layer_handle *layer,
                               const ExPolygonCollection &subject,
                               coord_t slice_z)
{
    const uint32_t region_count = object.print_region_count();
    if (region_count == 0 || layer == nullptr)
        return;

    // Region zero is the fallback: until a part or modifier mask steals an
    // area, the whole subject uses the object's default PrintRegion.
    std::vector<StoredExPolygonCollection> region_slices;
    region_slices.reserve(region_count);
    for (uint32_t region_idx = 0; region_idx < region_count; ++region_idx)
        region_slices.emplace_back(storage);
    region_slices.front().copy_from(subject);

    // Find the region table active at this Z and allocate one cached mask per
    // volume-region entry. Modifier entries can then clip themselves to the
    // already computed mask of their parent entry.
    const slicing_layer_range_handle *range = layer_range_for_z(object, slice_z);
    const uint32_t volume_region_count = slicing_layer_range_count_volume_region(range);
    std::vector<StoredExPolygonCollection> effective_masks;
    effective_masks.reserve(volume_region_count);
    for (uint32_t volume_region_idx = 0; volume_region_idx < volume_region_count; ++volume_region_idx)
        effective_masks.emplace_back(storage);

    ClipperContext clipper(storage);
    for (uint32_t volume_region_idx = 0; volume_region_idx < volume_region_count; ++volume_region_idx) {
        const slicing_volume_region_handle *volume_region_handle =
            slicing_layer_range_get_volume_region(range, volume_region_idx);
        if (volume_region_handle == nullptr)
            continue;

        // Discard entries that cannot publish into one of the layer regions
        // already created from the object's PrintRegion table.
        const SlicingVolumeRegion volume_region(volume_region_handle);
        const int32_t target_region_idx = volume_region.layer_region_idx();
        if (target_region_idx < 0 || uint32_t(target_region_idx) >= region_slices.size())
            continue;

        // Convert this volume-region entry into a mask over the auxiliary
        // subject. Empty masks simply mean that the entry is inactive here.
        StoredExPolygonCollection mask =
            slice_volume_mask(storage, print, object, volume_region, subject, slice_z);
        if (mask.empty())
            continue;

        // A modifier inherits geometry from its resolved parent entry. Clipping
        // to the parent mask keeps chained modifiers inside the exact area they
        // are allowed to override.
        const int32_t parent_idx = volume_region.parent();
        if (parent_idx >= 0 && uint32_t(parent_idx) < effective_masks.size()) {
            mask = clipper_intersection(clipper(mask), clipper(effective_masks[uint32_t(parent_idx)]))
                       .to_expolygon_collection();
            if (mask.empty())
                continue;
        }

        effective_masks[volume_region_idx].copy_from(mask);

        /*
        A later mask owns its pixels. Removing it from every existing region
        before adding it to the target region guarantees the final raw slices do
        not overlap, even when model parts and modifiers overlap in XY.
        */
        for (StoredExPolygonCollection &region_slice : region_slices)
            region_slice = clipper_diff(clipper(region_slice), clipper(mask)).to_expolygon_collection();

        region_slices[uint32_t(target_region_idx)].append_move_from(std::move(mask));
        region_slices[uint32_t(target_region_idx)] =
            clipper_union(clipper(region_slices[uint32_t(target_region_idx)])).to_expolygon_collection();
    }

    // Publish the classified raw slices back into the real LayerRegion objects.
    // Empty regions stay empty; the recompute step below will build the layer
    // geometry from the union of the non-empty regions.
    for (uint32_t region_idx = 0; region_idx < region_slices.size(); ++region_idx) {
        layer_region_handle *region = layer_get_region_mutable(layer, region_idx);
        expolygon_collection_handle *dst = layer_region_borrow_mutable_slices(region);
        if (dst != nullptr)
            expolygons_move(dst, region_slices[region_idx].mutable_handle());
    }

    // Rebuild the derived layer geometry exactly as post-slicing expects it:
    // first from region raw slices to layer islands, then attach each region to
    // the islands it intersects.
    layer_recompute_slices_and_islands_from_layer_regions(layer);
    layer_add_regions_to_islands(layer);
}

} // namespace

AuxiliaryLayerBuildResult build_auxiliary_layer_regions_from_subject(storage_handle *storage,
                                                                     const Print &print,
                                                                     const Object &object,
                                                                     const ExPolygonCollection &subject,
                                                                     coord_t height,
                                                                     coord_t print_z,
                                                                     coord_t slice_z)
{
    AuxiliaryLayerBuildResult result;
    if (storage == nullptr || object.print_region_count() == 0 || height <= 0 || print_z <= 0)
        return result;

    // Create the mutable auxiliary layer first. The host initializes its
    // LayerRegion list from the object's PrintRegions, but the regions are
    // empty until assign_regions_from_masks() publishes raw slices.
    layer_handle *layer = object_add_auxiliary_layer(object.handle(), height, print_z, slice_z);
    if (layer == nullptr)
        return result;

    // Classify the input subject by region settings and expose the finished
    // Layer view to the caller only after islands/region-islands were rebuilt.
    assign_regions_from_masks(storage, print, object, layer, subject, slice_z);
    result.layer = Layer(layer);
    result.created = true;
    return result;
}

bool publish_adhesion_extrusion_to_auxiliary_layer(storage_handle *storage,
                                                   orchestrator_handle *orchestrator,
                                                   const Print &print,
                                                   const Object &object,
                                                   const ExPolygonCollection &subject,
                                                   coord_t height,
                                                   coord_t print_z,
                                                   coord_t slice_z,
                                                   raw_layer_adhesion_kind kind,
                                                   raw_layer_adhesion_flag flags,
                                                   MutableExtrusionEntity extrusion)
{
    if (storage == nullptr || orchestrator == nullptr || !print.valid() || !object.valid() ||
        !subject.valid() || !extrusion.valid() || subject.empty() || extrusion.empty())
        return false;

    /*
    First build a normal auxiliary layer from the footprint supplied by the
    plugin. This gives the layer coherent LayerRegion raw slices and island
    membership before an extrusion tree is attached to it.
    */
    AuxiliaryLayerBuildResult result =
        build_auxiliary_layer_regions_from_subject(storage, print, object, subject, height, print_z, slice_z);
    if (!result.created)
        return false;

    /*
    Mark the layer before publishing the extrusion. Consumers can classify the
    layer by metadata without inspecting the extrusion tree itself.
    */
    LayerAdhesionProperty &property = result.layer.properties().get_or_add<LayerAdhesionProperty>(orchestrator);
    property.kind = kind;
    property.flags = flags;

    /*
    Adhesion output is stored in the full region-island for the generated
    auxiliary layer. If any step fails after the layer was created, remove the
    layer so callers never leave an empty or half-published auxiliary layer.
    */
    if (result.layer.island_count() == 0) {
        object.remove_auxiliary_layer(result.layer);
        return false;
    }

    layer_region_island_handle *region_island =
        layer_island_get_or_create_region_island(
            const_cast<layer_island_handle *>(result.layer.island(0).handle()),
            nullptr,
            0,
            -1);
    if (region_island == nullptr) {
        object.remove_auxiliary_layer(result.layer);
        return false;
    }

    extrusion_entity_handle *root =
        layer_region_island_get_mutable_extrusion(region_island, RAW_EXTRUSION_ROLE_PERIMETER);
    if (root == nullptr) {
        object.remove_auxiliary_layer(result.layer);
        return false;
    }

    const uint32_t inserted = MutableExtrusionEntity(root).append_child_move(extrusion);
    if (is_invalid_index(inserted)) {
        object.remove_auxiliary_layer(result.layer);
        return false;
    }
    return true;
}

} // namespace slic3r_api
