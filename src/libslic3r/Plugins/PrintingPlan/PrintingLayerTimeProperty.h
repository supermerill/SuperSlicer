///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_Plugins_PrintingPlan_PrintingLayerTimeProperty_h_
#define slic3r_Plugins_PrintingPlan_PrintingLayerTimeProperty_h_

/*
Shared layer-time property for PrintingPlan plugins
===================================================

Plugins that produce or consume ordered-layer timing include this private
header and register the property in their own orchestrator. The stable name
lets compatible plugins obtain the same compact runtime id without making the
payload layout part of the public plugin API.

The orchestrator rejects a second registration when its size or alignment does
not match. This prevents plugins built against incompatible revisions of this
private contract from interpreting each other's bytes.
*/

#include "libslic3r/Api/plugin/c/slic3r_data_tree.h"
#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"

#define PRINTING_LAYER_TIME_PROPERTY_NAME "slic3r.printing_layer.time"

typedef enum raw_printing_layer_time_origin {
    RAW_PRINTING_LAYER_TIME_ORIGIN_ESTIMATED = 0,
    RAW_PRINTING_LAYER_TIME_ORIGIN_FINAL = 1
} raw_printing_layer_time_origin;

typedef struct c_printing_layer_time_property {
    double duration_seconds;
    raw_printing_layer_time_origin origin;
} c_printing_layer_time_property;

/*
Register this private payload for one orchestrator.

The returned id is meaningful only inside that orchestrator. Consumers must
retain it instead of storing it in process-global state because tests and host
embeddings may use independent orchestrators.
*/
static inline plugin_property_type register_printing_layer_time_property(orchestrator_handle *orchestrator)
{
#ifdef __cplusplus
    const uint32_t alignment = (uint32_t)alignof(c_printing_layer_time_property);
#else
    const uint32_t alignment = (uint32_t)_Alignof(c_printing_layer_time_property);
#endif
    return (plugin_property_type)orchestrator_register_property(
        orchestrator,
        PRINTING_LAYER_TIME_PROPERTY_NAME,
        (uint32_t)sizeof(c_printing_layer_time_property),
        alignment);
}

#ifdef __cplusplus
#include "libslic3r/Api/plugin/cpp/PluginPropertyKey.hpp"

namespace slic3r_api {

/* C++ view over the trivially copied C payload shared by timing plugins. */
struct PrintingLayerTimeProperty : c_printing_layer_time_property
{
    bool is_final() const { return origin == RAW_PRINTING_LAYER_TIME_ORIGIN_FINAL; }
};

/* Register the shared contract and retain its orchestrator with the returned key. */
inline PluginPropertyKey<PrintingLayerTimeProperty>
printing_layer_time_property_key(orchestrator_handle *orchestrator)
{
    return PluginPropertyKey<PrintingLayerTimeProperty>::register_dynamic(
        orchestrator, PRINTING_LAYER_TIME_PROPERTY_NAME);
}

} // namespace slic3r_api
#endif

#endif // slic3r_Plugins_PrintingPlan_PrintingLayerTimeProperty_h_
