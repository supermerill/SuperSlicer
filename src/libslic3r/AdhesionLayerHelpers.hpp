///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_AdhesionLayerHelpers_hpp_
#define slic3r_AdhesionLayerHelpers_hpp_

#include <algorithm>
#include <type_traits>
#include <utility>

#include "ExtrusionEntityCollection.hpp"
#include "Layer.hpp"
#include "PluginProperty.hpp"
#include "PrintObject.hpp"

namespace Slic3r {

/*
Adhesion layer helpers
======================

Brim and skirt are now stored as ordinary auxiliary Layer objects. The layer
metadata says whether a layer is brim, normal skirt, or first-layer-only skirt;
the extrusion tree still lives in the perimeter bucket of each
LayerRegionIsland.

The helpers in this file do only mechanical traversal:
- test an auxiliary layer with a small predicate;
- copy matched extrusion roots into a legacy compatibility collection;
- remove matched auxiliary layers;
- visit matched extrusion roots for post-processing;
- collect the physical layer slice points used by first-layer hull rebuilding.

Feature-specific meaning stays in the predicates below. This keeps the same
loops usable for brim, all skirt, normal skirt, and first-layer-only skirt.
*/

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

inline bool layer_is_any_adhesion(const Layer &layer)
{
    return layer_is_brim_adhesion(layer) || layer_is_any_skirt_adhesion(layer);
}

inline void append_auxiliary_layer_extrusions(const Layer &layer, ExtrusionEntityCollection &dst);

/*
Collect the perimeter-bucket extrusion roots from every auxiliary layer matched
by matches_layer. The predicate can be a named helper or a lambda with the shape
`bool(const Layer&)`; it should only inspect the layer and must not keep
references to the borrowed layer after the call.
*/
template<class MatchesLayer>
void collect_auxiliary_layer_extrusions(const PrintObject &object,
                                        MatchesLayer matches_layer,
                                        ExtrusionEntityCollection &dst);

/*
Remove every auxiliary layer matched by matches_layer. The predicate can be a
named helper or a lambda with the shape `bool(const Layer&)`; keep it side
effect free because this helper mutates the PrintObject layer list during the
walk.
*/
template<class MatchesLayer>
void remove_auxiliary_layers(PrintObject &object, MatchesLayer matches_layer);

template<class MatchesLayer>
bool has_auxiliary_layer(const PrintObject &object, MatchesLayer matches_layer);

template<class Visitor>
void visit_auxiliary_layer_extrusions(Layer &layer, Visitor &visitor);

template<class MatchesLayer, class Visitor>
void visit_matching_auxiliary_layer_extrusions(PrintObject &object,
                                               MatchesLayer matches_layer,
                                               Visitor &visitor);

void append_auxiliary_layer_points(const Layer &layer, Points &dst);

template<class MatchesLayer>
void append_matching_auxiliary_layer_points(const PrintObject &object,
                                            MatchesLayer matches_layer,
                                            Points &dst);

namespace detail {

inline void append_adhesion_extrusion_copy(ExtrusionEntityCollection &dst,
                                           const ExtrusionEntity &src)
{
    if (src.is_nop())
        return;

    if (const ExtrusionEntityCollection *collection = dynamic_cast<const ExtrusionEntityCollection *>(&src)) {
        /*
        Legacy callers used to receive direct m_brim/m_skirt collections. A
        plain transport collection is flattened by one level to preserve that
        observable shape. A property-bearing collection stays whole because its
        children inherit metadata from it.
        */
        if (collection->has_properties()) {
            dst.append(src);
            return;
        }
        for (const ExtrusionEntity *child : collection->entities())
            dst.append(*child);
        return;
    }

    if (src.is_leaf()) {
        dst.append(src);
        return;
    }

    for (const ExtrusionEntityUPtr &child : src.children())
        dst.append(*child);
}

template<class Visitor, class = void> struct HasTraverse : std::false_type {};

template<class Visitor>
struct HasTraverse<Visitor,
                   std::void_t<decltype(std::declval<Visitor &>().traverse(
                       std::declval<ExtrusionEntity &>()))>> : std::true_type {};

template<class Visitor>
void visit_extrusion_tree(ExtrusionEntity &entity, Visitor &visitor)
{
    /*
    The native codebase has two visitor families. New tree visitors own a
    traverse() method, while legacy visitors are driven through
    ExtrusionEntity::visit(). Support both so callers can use the traversal
    style they already have.
    */
    if constexpr (HasTraverse<Visitor>::value)
        visitor.traverse(entity);
    else
        entity.visit(visitor);
}

} // namespace detail

inline void append_auxiliary_layer_extrusions(const Layer &layer, ExtrusionEntityCollection &dst)
{
    for (const LayerSliceIsland &island : layer.islands()) {
        for (const LayerRegionIsland &region_island : island.regions_islands()) {
            if (region_island.has_extrusion(LayerRegionIsland::PERIMETERS))
                detail::append_adhesion_extrusion_copy(dst, region_island.extrusion(LayerRegionIsland::PERIMETERS));
        }
    }
}

template<class MatchesLayer>
void collect_auxiliary_layer_extrusions(const PrintObject &object,
                                        MatchesLayer matches_layer,
                                        ExtrusionEntityCollection &dst)
{
    for (const Layer &layer : object.auxiliary_layers())
        if (matches_layer(layer))
            append_auxiliary_layer_extrusions(layer, dst);
}

template<class MatchesLayer>
void remove_auxiliary_layers(PrintObject &object, MatchesLayer matches_layer)
{
    LayerUPtrs &layers = object.mutable_auxiliary_layers();
    layers.erase(std::remove_if(layers.begin(),
                                layers.end(),
                                [&matches_layer](const LayerUPtr &layer) {
                                    return layer != nullptr && matches_layer(*layer);
                                }),
                 layers.end());
}

template<class MatchesLayer>
bool has_auxiliary_layer(const PrintObject &object, MatchesLayer matches_layer)
{
    for (const Layer &layer : object.auxiliary_layers())
        if (matches_layer(layer))
            return true;
    return false;
}

template<class Visitor>
void visit_auxiliary_layer_extrusions(Layer &layer, Visitor &visitor)
{
    for (LayerSliceIsland &island : layer.islands()) {
        for (LayerRegionIsland &region_island : island.regions_islands()) {
            if (region_island.has_extrusion(LayerRegionIsland::PERIMETERS))
                detail::visit_extrusion_tree(region_island.mutable_extrusion(LayerRegionIsland::PERIMETERS), visitor);
        }
    }
}

template<class MatchesLayer, class Visitor>
void visit_matching_auxiliary_layer_extrusions(PrintObject &object,
                                               MatchesLayer matches_layer,
                                               Visitor &visitor)
{
    for (Layer &layer : object.auxiliary_layers())
        if (matches_layer(layer))
            visit_auxiliary_layer_extrusions(layer, visitor);
}

inline void append_auxiliary_layer_points(const Layer &layer, Points &dst)
{
    /*
    Auxiliary adhesion layer slices are built from extrusion coverage, not only
    centerlines. Their polygon points therefore describe the physical bed area
    that must be included in the first-layer convex hull.
    */
    for (const ExPolygon &slice : layer.lslices()) {
        dst.insert(dst.end(), slice.contour.points.begin(), slice.contour.points.end());
        for (const Polygon &hole : slice.holes)
            dst.insert(dst.end(), hole.points.begin(), hole.points.end());
    }
}

template<class MatchesLayer>
void append_matching_auxiliary_layer_points(const PrintObject &object,
                                            MatchesLayer matches_layer,
                                            Points &dst)
{
    for (const Layer &layer : object.auxiliary_layers())
        if (matches_layer(layer))
            append_auxiliary_layer_points(layer, dst);
}

} // namespace Slic3r

#endif // slic3r_AdhesionLayerHelpers_hpp_
