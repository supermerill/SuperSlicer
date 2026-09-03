///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
/*
Adhesion layer publication helper
=================================

This file contains the storage operation shared by the skirt and brim
plugins. The callers provide already-generated adhesion geometry; this helper
does not decide its shape, size, or placement. Its responsibility is to turn
that geometry into a valid auxiliary layer and to publish the extrusion with
the metadata expected by later consumers.

The normal execution flow is:

    publish_adhesion_extrusion_to_auxiliary_layer()
    |-- validate the handles, geometry, and extrusion
    |-- build an auxiliary layer from the subject polygons
    |-- attach the adhesion kind and flags to the layer
    |-- get the first layer island and its perimeter extrusion root
    |-- append the generated extrusion to that root
    `-- remove the auxiliary layer if any publication step fails

The operation is intentionally transactional from the caller's point of
view: a failed build or insertion must not leave a partially initialized
adhesion layer in the object. The helper currently publishes into the first
island and perimeter bucket created by the auxiliary-layer builder; it does
not distribute geometry across multiple islands or generate final machine
G-code.
*/
#include "AdhesionLayerHelpers.hpp"

namespace slic3r_api { namespace SkirtBrim {

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
    Build the neutral layer first so its region slices and island membership are
    complete before the skirt/brim metadata and extrusion tree become visible.
    */
    AuxiliaryLayerBuildResult result =
        build_auxiliary_layer_regions_from_subject(storage, print, object, subject, height, print_z, slice_z);
    if (!result.created)
        return false;

    // Mark the feature explicitly so later adhesion plugins do not have to
    // infer whether an auxiliary layer contains brim or one of the skirt forms.
    LayerAdhesionProperty &property = result.layer.properties().get_or_add(LayerAdhesionProperty::key);
    property.kind = kind;
    property.flags = flags;

    /*
    Publish the generated tree in the perimeter bucket used by all built-in
    adhesion consumers. Any failure removes the newly created layer, preventing
    a partially initialized feature from surviving this operation.
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

}} // namespace slic3r_api::SkirtBrim
