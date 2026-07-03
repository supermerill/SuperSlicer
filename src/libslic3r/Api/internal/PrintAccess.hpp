///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_Api_internal_PrintAccess_hpp_
#define slic3r_Api_internal_PrintAccess_hpp_

#include "libslic3r/DataTreeFwd.hpp"
#include "libslic3r/Polygon.hpp"

namespace Slic3r {

class ExtrusionEntity;
class Print;

namespace ApiInternal {

/*
Internal accessors for host-owned print outputs.

Plugin steps should normally mutate the print through C callback tables. The
host side of those callbacks still needs a narrow way to reach private Print
fields. Keep that access here instead of making brim, hull or state internals
public on Print.
*/
struct PrintAccess
{
    static void clear_brim(Print &print);
    static void clear_skirt(Print &print);
    static void normalize_skirt_brim_direction(Print &print);
    static void rebuild_first_layer_convex_hull_after_skirt_brim(Print &print);
};

} // namespace ApiInternal

} // namespace Slic3r

#endif // slic3r_Api_internal_PrintAccess_hpp_
