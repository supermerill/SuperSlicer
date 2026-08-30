///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Plugins_SkirtBrim_AdhesionLayerHelpers_hpp_
#define slic3r_Plugins_SkirtBrim_AdhesionLayerHelpers_hpp_

#include "libslic3r/Api/plugin/cpp/AuxiliaryLayerHelpers.hpp"

namespace slic3r_api { namespace SkirtBrim {

/*
Adhesion layer helpers
======================

The public auxiliary-layer API builds a region-aware Layer from a 2D subject,
but deliberately does not decide what that layer means. These helpers contain
the conventions shared specifically by the built-in skirt and brim plugins:
tag the layer as adhesion output, store its generated tree in the perimeter
bucket, and retrieve or remove that output during later skirt/brim passes.

Keeping these decisions beside their consumers prevents a future auxiliary
feature from accidentally inheriting skirt/brim storage rules merely because it
uses the same generic Layer construction primitive.
*/

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
    return layer_is_brim(layer);
}

inline bool layer_is_any_skirt_adhesion(const Layer &layer)
{
    return layer_has_adhesion_kind(layer, RAW_LAYER_ADHESION_KIND_SKIRT);
}

inline bool layer_is_normal_skirt_adhesion(const Layer &layer)
{
    return layer_is_normal_skirt(layer);
}

inline bool layer_is_first_layer_skirt_adhesion(const Layer &layer)
{
    return layer_is_skirt_first_layer_only(layer);
}

inline void append_auxiliary_layer_extrusions(storage_handle *storage,
                                              const Layer &layer,
                                              StoredExtrusionEntity &out)
{
    /*
    Adhesion plugins publish their generated tree in the perimeter bucket of
    each LayerRegionIsland. Copy the bucket root before appending it so the
    caller receives a storage-owned tree and the source layer remains unchanged.
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
Collect the perimeter-bucket trees from auxiliary layers selected by the
caller. The returned tree belongs to storage and never mutates the source
layers.
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
Remove selected adhesion layers from back to front so erasing one layer never
changes the indices that still need to be inspected.
*/
template<class MatchesLayer>
void remove_auxiliary_layers(const Object &object, MatchesLayer matches_layer)
{
    for (uint32_t idx = object.auxiliary_layer_count(); idx > 0; --idx) {
        const Layer layer = object.auxiliary_layer(idx - 1);
        if (matches_layer(layer))
            object.remove_auxiliary_layer(layer);
    }
}

}} // namespace slic3r_api::SkirtBrim

#endif // slic3r_Plugins_SkirtBrim_AdhesionLayerHelpers_hpp_
