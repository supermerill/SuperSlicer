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

After the helper returns, the Layer has raw LayerRegion slices, cached Layer
slices and LayerSliceIsland membership ready for later perimeter/surface/infill
code. The helper does not create extrusions.
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

} // namespace slic3r_api

#endif // slic3r_Api_plugin_cpp_AuxiliaryLayerHelpers_hpp_
