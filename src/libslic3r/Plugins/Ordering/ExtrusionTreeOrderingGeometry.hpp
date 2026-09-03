///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_Plugins_Ordering_ExtrusionTreeOrderingGeometry_hpp_
#define slic3r_Plugins_Ordering_ExtrusionTreeOrderingGeometry_hpp_

#include "libslic3r/Api/plugin/cpp/ExtrusionViews.hpp"

namespace slic3r_api { namespace Ordering { namespace TreeOrderingGeometry {

/*
Shared geometry mutations used by built-in extrusion-tree ordering providers.

These operations preserve existing entity handles whenever possible. Loop
rotation accepts seams on lines and arcs, splits only the segment containing
the seam, and preserves segment Z offsets. The caller owns the policy deciding
which seam, direction, or ordering should be selected.
*/

/* Reverse every local path and child order so the complete tree runs backward. */
void reverse_tree(MutableExtrusionEntity entity);

/* Rotate a fixed closed loop so its first and last point equal seam. */
void rotate_loop_to_seam(storage_handle *scratch_storage,
                         MutableExtrusionEntity loop,
                         c_point seam);

/* Remove sorting and reversal permissions from an entity and all descendants. */
void disable_ordering_flags_recursively(MutableExtrusionEntity entity);

}}} // namespace slic3r_api::Ordering::TreeOrderingGeometry

#endif // slic3r_Plugins_Ordering_ExtrusionTreeOrderingGeometry_hpp_
