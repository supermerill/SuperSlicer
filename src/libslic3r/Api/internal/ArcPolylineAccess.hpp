///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_internal_ArcPolylineAccess_hpp_
#define slic3r_Api_internal_ArcPolylineAccess_hpp_

#include <cstddef>

#include "libslic3r/Geometry/ArcWelder.hpp"
#include "libslic3r/Point.hpp"

namespace Slic3r {

class ArcPolyline;

namespace ApiInternal {

struct ArcPolylineAccess
{
    static bool set_point(ArcPolyline &polyline, size_t idx, const Point &point);
    static bool insert_point(ArcPolyline &polyline, size_t idx, const Point &point);
    static bool remove_point(ArcPolyline &polyline, size_t idx);
    static bool set_segment(ArcPolyline &polyline,
                            size_t segment_idx,
                            const Point &point_a,
                            const Point &point_b,
                            float radius,
                            Geometry::ArcWelder::Orientation orientation);
    static void clear_z_offsets(ArcPolyline &polyline);

    /* Recompute cached arc metadata after replacing the complete path. */
    static void refresh_after_bulk_replace(ArcPolyline &polyline);

private:
    static void refresh_after_edit(ArcPolyline &polyline);
};

} // namespace ApiInternal

} // namespace Slic3r

#endif // slic3r_Api_internal_ArcPolylineAccess_hpp_
