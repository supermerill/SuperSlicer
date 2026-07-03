///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_Api_internal_PrintObjectAccess_hpp_
#define slic3r_Api_internal_PrintObjectAccess_hpp_

#include <vector>
#include <optional>

#include "libslic3r/DataTreeFwd.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/libslic3r.h"

namespace Slic3r {

class Layer;
class ExtrusionEntity;
class ExtrusionEntityCollection;
class PrintObject;

namespace ApiInternal {

struct PrintObjectAccess
{
    static void set_layer_profile(PrintObject &object, std::vector<coord_t> &&layer_profile);
    static void replace_layers_by_moving_contents(PrintObject &object, LayerUPtrs &&new_layers);
    static PrintInstances &mutable_instances(PrintObject &object);
    static void clear_brim_auxiliary_layers(PrintObject &object);
    static void clear_skirt_auxiliary_layers(PrintObject &object);
#ifdef _DEBUG
    static void make_perimeters(PrintObject &object);
#endif
};

} // namespace ApiInternal

} // namespace Slic3r

#endif // slic3r_Api_internal_PrintObjectAccess_hpp_
