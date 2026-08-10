///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_AuxiliaryLayerHelpers_hpp_
#define slic3r_Api_plugin_cpp_AuxiliaryLayerHelpers_hpp_

#include "libslic3r/Api/plugin/cpp/Views.hpp"
#include "libslic3r/Api/plugin/cpp/VolumeViews.hpp"

namespace slic3r_api {

/*
Auxiliary layer helpers
=======================

Auxiliary layers are normal Layer objects that do not come from slicing the
object mesh. Support, skirt, brim and future helper geometry can publish there
without pretending to be an object layer.

The helper in this file solves the common first step: given an already-known 2D
subject area, create an auxiliary layer and split that subject into LayerRegion
raw slices. Region zero is the fallback object region. Model-part and modifier
volumes are sliced locally at the requested Z only to decide where alternate
settings apply. Negative volumes are ignored because the subject is already the
geometry the plugin wants to print.

After the build helper returns, the Layer has raw LayerRegion slices, cached
Layer slices and LayerSliceIsland membership ready for later
perimeter/surface/infill code. The publication helper below performs the next
common step for adhesion plugins: attach a brim/skirt tag and move one already
generated extrusion tree into the new layer. It does not generate the subject
area or the extrusion tree; callers are still responsible for those algorithms.

The small traversal helpers at the end of the file do one mechanical job:
walk auxiliary layers, test each layer with a caller-provided predicate, and
copy or remove the matched layer output. The predicate keeps the feature rule
outside of the traversal, so brim, normal skirt and first-layer skirt can all
reuse the same loops.
*/

struct AuxiliaryLayerBuildResult
{
    Layer layer;
    bool created = false;
};

AuxiliaryLayerBuildResult build_auxiliary_layer_regions_from_subject(storage_handle *storage,
                                                                     const Print &print,
                                                                     const Object &object,
                                                                     const ExPolygonCollection &subject,
                                                                     coord_t height,
                                                                     coord_t print_z,
                                                                     coord_t slice_z);

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
                                                   MutableExtrusionEntity extrusion);

inline bool layer_is_brim_adhesion(const Layer &layer)
{
    return LayerAdhesionProperty::layer_is_brim(layer);
}

inline bool layer_is_any_skirt_adhesion(const Layer &layer)
{
    return LayerAdhesionProperty::layer_has_kind(layer, RAW_LAYER_ADHESION_KIND_SKIRT);
}

inline bool layer_is_normal_skirt_adhesion(const Layer &layer)
{
    return LayerAdhesionProperty::layer_is_normal_skirt(layer);
}

inline bool layer_is_first_layer_skirt_adhesion(const Layer &layer)
{
    return LayerAdhesionProperty::layer_is_skirt_first_layer_only(layer);
}

inline void append_auxiliary_layer_extrusions(storage_handle *storage,
                                              const Layer &layer,
                                              StoredExtrusionEntity &out)
{
    /*
    Adhesion plugins publish their generated tree in the perimeter bucket of
    each LayerRegionIsland. Copy the bucket root before appending it so the
    caller receives a storage-owned tree and the source auxiliary layer remains
    unchanged.
    */
    for (uint32_t island_idx = 0; island_idx < layer.island_count(); ++island_idx) {
        const LayerIsland island = layer.island(island_idx);
        for (uint32_t region_island_idx = 0; region_island_idx < island.region_island_count();
             ++region_island_idx) {
            const LayerRegionIsland region_island = island.region_island(region_island_idx);
            if (!region_island.has_extrusion(RAW_EXTRUSION_ROLE_PERIMETER))
                continue;
            const ExtrusionEntity extrusion(region_island.extrusion(RAW_EXTRUSION_ROLE_PERIMETER));
            StoredExtrusionEntity copy(storage, extrusion);
            out.append_child_move(copy.mutable_view());
        }
    }
}

/*
Collect the perimeter-bucket extrusion roots from every auxiliary layer matched
by matches_layer. The predicate can be a named helper or a lambda with the shape
`bool(const Layer&)`; it should only inspect the layer and must not store the
borrowed Layer view past the call.
*/
template<class MatchesLayer>
StoredExtrusionEntity collect_auxiliary_layer_extrusions(storage_handle *storage,
                                                        const Object &object,
                                                        MatchesLayer matches_layer)
{
    StoredExtrusionEntity out(storage);
    out.disable_sort().disable_reverse();
    for (uint32_t layer_idx = 0; layer_idx < object.auxiliary_layer_count(); ++layer_idx) {
        const Layer layer = object.auxiliary_layer(layer_idx);
        if (matches_layer(layer))
            append_auxiliary_layer_extrusions(storage, layer, out);
    }
    out.disable_sort().disable_reverse();
    return out;
}

/*
Remove every auxiliary layer matched by matches_layer. The predicate can be a
named helper or a lambda with the shape `bool(const Layer&)`; keep it side
effect free because this helper mutates the Object while it walks the layer
list.
*/
template<class MatchesLayer>
void remove_auxiliary_layers(const Object &object, MatchesLayer matches_layer)
{
    /*
    Remove from the back so deleting a layer cannot change the index of layers
    that still need to be inspected.
    */
    for (uint32_t idx = object.auxiliary_layer_count(); idx > 0; --idx) {
        const Layer layer = object.auxiliary_layer(idx - 1);
        if (matches_layer(layer))
            object.remove_auxiliary_layer(layer);
    }
}

} // namespace slic3r_api

#endif // slic3r_Api_plugin_cpp_AuxiliaryLayerHelpers_hpp_
