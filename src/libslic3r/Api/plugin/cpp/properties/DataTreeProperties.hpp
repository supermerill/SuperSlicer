///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_properties_DataTreeProperties_hpp_
#define slic3r_Api_plugin_cpp_properties_DataTreeProperties_hpp_

/*
Built-in data-tree properties
=============================

This header gives the fixed C payloads stored on data-tree objects their typed
C++ keys and small interpretation helpers. The types add no data to their C ABI
payload, so pointers returned by PluginProperties may be viewed directly as the
matching C++ type.

The layer helpers are templates because they only require a view exposing
properties(). Their definitions therefore remain independent of DataTreeViews
and are instantiated only after the caller's concrete layer view is complete.
*/

#include "libslic3r/Api/plugin/c/slic3r_data_tree.h"
#include "libslic3r/Api/plugin/cpp/PluginPropertyKey.hpp"

namespace slic3r_api {

/* Identifies an auxiliary layer containing generated support geometry. */
struct LayerSupportProperty :
    BuiltInPluginPropertyPayload<LayerSupportProperty,
                                 c_layer_support_property,
                                 PLUGIN_PROPERTY_TYPE_LAYER_SUPPORT>
{
};

/* Legacy marker identifying an auxiliary layer containing brim geometry. */
struct LayerBrimProperty :
    BuiltInPluginPropertyPayload<LayerBrimProperty,
                                 c_layer_brim_property,
                                 PLUGIN_PROPERTY_TYPE_LAYER_BRIM>
{
};

/* Describes the kind and lifetime of generated skirt or brim geometry. */
struct LayerAdhesionProperty :
    BuiltInPluginPropertyPayload<LayerAdhesionProperty,
                                 c_layer_adhesion_property,
                                 PLUGIN_PROPERTY_TYPE_LAYER_ADHESION>
{
    bool has_kind(raw_layer_adhesion_kind expected_kind) const { return kind == expected_kind; }
    bool has_flag(raw_layer_adhesion_flag flag) const { return (flags & flag) != 0; }
    bool is_brim() const { return has_kind(RAW_LAYER_ADHESION_KIND_BRIM); }
    bool is_skirt() const { return has_kind(RAW_LAYER_ADHESION_KIND_SKIRT); }
    bool is_first_layer_only() const { return has_flag(RAW_LAYER_ADHESION_FLAG_FIRST_LAYER_ONLY); }
};

/* Return the adhesion payload physically stored on a layer, if present. */
template<class LayerView>
const LayerAdhesionProperty *layer_adhesion_property(const LayerView &layer)
{
    return layer.properties().get(LayerAdhesionProperty::key);
}

/*
Test the effective adhesion kind carried by a layer.

The legacy brim marker remains a valid fallback so older producers and newer
consumers can share the same auxiliary-layer representation.
*/
template<class LayerView>
bool layer_has_adhesion_kind(const LayerView &layer, raw_layer_adhesion_kind kind)
{
    const LayerAdhesionProperty *adhesion = layer_adhesion_property(layer);
    if (adhesion != nullptr)
        return adhesion->has_kind(kind);
    return kind == RAW_LAYER_ADHESION_KIND_BRIM &&
           layer.properties().get(LayerBrimProperty::key) != nullptr;
}

template<class LayerView>
bool layer_is_brim(const LayerView &layer)
{
    return layer_has_adhesion_kind(layer, RAW_LAYER_ADHESION_KIND_BRIM);
}

template<class LayerView>
bool layer_is_normal_skirt(const LayerView &layer)
{
    const LayerAdhesionProperty *adhesion = layer_adhesion_property(layer);
    return adhesion != nullptr && adhesion->is_skirt() && !adhesion->is_first_layer_only();
}

template<class LayerView>
bool layer_is_skirt_first_layer_only(const LayerView &layer)
{
    const LayerAdhesionProperty *adhesion = layer_adhesion_property(layer);
    return adhesion != nullptr && adhesion->is_skirt() && adhesion->is_first_layer_only();
}

} // namespace slic3r_api

#endif // slic3r_Api_plugin_cpp_properties_DataTreeProperties_hpp_
