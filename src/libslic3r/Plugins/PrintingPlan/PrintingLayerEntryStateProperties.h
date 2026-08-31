///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_Plugins_PrintingPlan_PrintingLayerEntryStateProperties_h_
#define slic3r_Plugins_PrintingPlan_PrintingLayerEntryStateProperties_h_

/*
Shared entry-state properties for PrintingPlan plugins
======================================================

STEP_LAYER_EXTRUSION_EDIT runs one worker per PrintingLayerGroup. Reading the
previous layer from such a worker would couple otherwise independent jobs and
would become unsafe as soon as another worker edits that layer. These two
properties instead publish the state that existed immediately before the
layer-group began.

The contracts are private to the coordinated plugins which include this file.
Their stable names let producers and consumers obtain the same runtime ids
without making the payloads part of the global plugin ABI. Position and tool
are intentionally separate so a consumer only depends on the state it needs.
*/

#include <stdint.h>

#include "libslic3r/Api/plugin/c/slic3r_geometry.h"
#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"

#define PRINTING_LAYER_ENTRY_POSITION_PROPERTY_NAME "slic3r.printing_layer.entry_position"
#define PRINTING_LAYER_ENTRY_TOOL_PROPERTY_NAME "slic3r.printing_layer.entry_tool"

typedef enum raw_printing_layer_entry_position_state {
    RAW_PRINTING_LAYER_ENTRY_POSITION_UNKNOWN = 0,
    RAW_PRINTING_LAYER_ENTRY_POSITION_KNOWN = 1
} raw_printing_layer_entry_position_state;

/* Exact PrintingPlan-space position before any content of one layer-group. */
typedef struct c_printing_layer_entry_position_property {
    coord_t x;
    coord_t y;
    coord_t z;
    raw_printing_layer_entry_position_state state;
} c_printing_layer_entry_position_property;

/* Tool selected before begin_tool_group() visits this layer-group. */
typedef struct c_printing_layer_entry_tool_property {
    uint16_t extruder_id;
} c_printing_layer_entry_tool_property;

/* Register the position payload for one orchestrator and return its runtime id. */
static inline plugin_property_type register_printing_layer_entry_position_property(
    orchestrator_handle *orchestrator)
{
#ifdef __cplusplus
    const uint32_t alignment = (uint32_t)alignof(c_printing_layer_entry_position_property);
#else
    const uint32_t alignment = (uint32_t)_Alignof(c_printing_layer_entry_position_property);
#endif
    return (plugin_property_type)orchestrator_register_property(
        orchestrator,
        PRINTING_LAYER_ENTRY_POSITION_PROPERTY_NAME,
        (uint32_t)sizeof(c_printing_layer_entry_position_property),
        alignment);
}

/* Register the active-tool payload for one orchestrator and return its runtime id. */
static inline plugin_property_type register_printing_layer_entry_tool_property(
    orchestrator_handle *orchestrator)
{
#ifdef __cplusplus
    const uint32_t alignment = (uint32_t)alignof(c_printing_layer_entry_tool_property);
#else
    const uint32_t alignment = (uint32_t)_Alignof(c_printing_layer_entry_tool_property);
#endif
    return (plugin_property_type)orchestrator_register_property(
        orchestrator,
        PRINTING_LAYER_ENTRY_TOOL_PROPERTY_NAME,
        (uint32_t)sizeof(c_printing_layer_entry_tool_property),
        alignment);
}

#ifdef __cplusplus
#include "libslic3r/Api/plugin/cpp/PluginPropertyKey.hpp"

namespace slic3r_api {

/* C++ view over the exact planned position carried by the C payload. */
struct PrintingLayerEntryPositionProperty : c_printing_layer_entry_position_property
{
    bool is_known() const { return state == RAW_PRINTING_LAYER_ENTRY_POSITION_KNOWN; }
};

/* C++ view over the tool which was active before the layer-group began. */
struct PrintingLayerEntryToolProperty : c_printing_layer_entry_tool_property
{
    bool has_active_tool() const { return extruder_id != UINT16_MAX; }
};

/* Register and return the typed position key owned by this orchestrator. */
inline PluginPropertyKey<PrintingLayerEntryPositionProperty>
printing_layer_entry_position_property_key(orchestrator_handle *orchestrator)
{
    return PluginPropertyKey<PrintingLayerEntryPositionProperty>::register_dynamic(
        orchestrator, PRINTING_LAYER_ENTRY_POSITION_PROPERTY_NAME);
}

/* Register and return the typed active-tool key owned by this orchestrator. */
inline PluginPropertyKey<PrintingLayerEntryToolProperty>
printing_layer_entry_tool_property_key(orchestrator_handle *orchestrator)
{
    return PluginPropertyKey<PrintingLayerEntryToolProperty>::register_dynamic(
        orchestrator, PRINTING_LAYER_ENTRY_TOOL_PROPERTY_NAME);
}

} // namespace slic3r_api
#endif

#endif // slic3r_Plugins_PrintingPlan_PrintingLayerEntryStateProperties_h_
