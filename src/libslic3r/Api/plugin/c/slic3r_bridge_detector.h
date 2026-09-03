///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_bridge_detector_h_
#define slic3r_bridge_detector_h_

#define SLIC3R_PLUGIN_API_BRIDGE_DETECTOR_MAJOR 1u
#define SLIC3R_PLUGIN_API_BRIDGE_DETECTOR_MINOR 0u

#include <stdint.h>

#include "slic3r_data_tree.h"
#include "slic3r_geometry.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================= BRIDGE DETECTOR ========================= */

typedef struct bridge_detector_vtable bridge_detector_vtable;

typedef struct bridge_detector_instance {
    void *ctx;
    const bridge_detector_vtable *vt;
} bridge_detector_instance;

typedef struct bridge_detector_create_input {
    /*
    Provide either one expolygon or a collection of expolygons. If both are set,
    expolygon is preferred to match the common one-region BridgeDetector path.
    */
    const expolygon_handle *expolygon;
    const expolygon_collection_handle *expolygons;
    const expolygon_collection_handle *lower_slices;
    coord_t spacing;
    coord_t precision;
    int32_t layer_id;
} bridge_detector_create_input;

struct bridge_detector_vtable {
    void (*destroy)(void *detector_ctx);
    int32_t (*detect_angle)(void *detector_ctx, double bridge_direction_override);
    uint32_t (*coverage)(void *detector_ctx, double angle, polygon_collection_handle *out_polygons);
    uint32_t (*unsupported_edges)(void *detector_ctx, double angle, polyline_collection_handle *out_polylines);

    double (*get_angle)(void *detector_ctx);
    void (*set_max_bridge_length)(void *detector_ctx, double max_bridge_length);
    double (*get_max_bridge_length)(void *detector_ctx);
    void (*set_layer_id)(void *detector_ctx, int32_t layer_id);
    int32_t (*get_layer_id)(void *detector_ctx);
};

SLIC3R_HOST_API void bridge_detector_destroy(bridge_detector_instance *detector);
SLIC3R_HOST_API int32_t bridge_detector_detect_angle(bridge_detector_instance *detector, double bridge_direction_override);
SLIC3R_HOST_API uint32_t bridge_detector_coverage(bridge_detector_instance *detector,
                                                  double angle,
                                                  polygon_collection_handle *out_polygons);
SLIC3R_HOST_API uint32_t bridge_detector_unsupported_edges(bridge_detector_instance *detector,
                                                           double angle,
                                                           polyline_collection_handle *out_polylines);
SLIC3R_HOST_API double bridge_detector_get_angle(bridge_detector_instance *detector);
SLIC3R_HOST_API void bridge_detector_set_max_bridge_length(bridge_detector_instance *detector, double max_bridge_length);
SLIC3R_HOST_API double bridge_detector_get_max_bridge_length(bridge_detector_instance *detector);
SLIC3R_HOST_API void bridge_detector_set_layer_id(bridge_detector_instance *detector, int32_t layer_id);
SLIC3R_HOST_API int32_t bridge_detector_get_layer_id(bridge_detector_instance *detector);

#ifdef __cplusplus
}
#endif

#endif // slic3r_bridge_detector_h_
