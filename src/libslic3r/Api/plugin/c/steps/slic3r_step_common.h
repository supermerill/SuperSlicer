///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_step_common_h_
#define slic3r_step_common_h_

#define SLIC3R_PLUGIN_API_STEP_COMMON_MAJOR 1u
#define SLIC3R_PLUGIN_API_STEP_COMMON_MINOR 0u

#include "../slic3r_data_tree.h"
#include "../slic3r_plugin_run_context.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
Borrow mutable raw slices for a LayerRegion during steps where the host allows
plugins to fill or edit slicing results. The returned handle is borrowed from
the data tree; do not free it through plugin storage.
*/
typedef expolygon_collection_handle *(*layer_region_borrow_mutable_slices_fn)(const layer_region_handle *me);

#ifdef __cplusplus
}
#endif

#endif // slic3r_step_common_h_
