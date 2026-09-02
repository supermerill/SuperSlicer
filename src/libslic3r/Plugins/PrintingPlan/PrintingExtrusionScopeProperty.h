///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_Plugins_PrintingPlan_PrintingExtrusionScopeProperty_h_
#define slic3r_Plugins_PrintingPlan_PrintingExtrusionScopeProperty_h_

/*
Compact extrusion-scope metadata shared by PrintingPlan plugins
================================================================

Transition plugins coordinate through a marker stored on the root of each
maximal continuous printable scope. The marker describes only which ordered
phases exist around that content. Owner information, endpoints and effective
extrusion settings are recovered from the normal PrintingPlan traversal.

This is a private contract between built-in plugins. Its stable registration
name lets several providers share one runtime property id without publishing
the payload as part of the global plugin ABI.

Typical use
-----------

1. CreateTransitionScope registers the key and attaches this payload to every
   maximal continuous printable scope.
2. OrderedExtrusionScope reads the flags to expose the optional travel,
   before and after phases without making callers depend on child indexes.
3. Later plugins traverse marked roots with PrintingEntityPropertyTraversal
   and write only into the phases announced by these flags.

The payload intentionally stores facts, not adjacency or owner data. A caller
recovers the owning PrintingLayerGroup, PrintingToolGroup and
PrintingExtrusion from the traversal which found the marked entity.
*/

#include <cstdint>

#include "libslic3r/Api/plugin/cpp/PluginPropertyKey.hpp"

#define PRINTING_EXTRUSION_SCOPE_PROPERTY_NAME \
    "slic3r.printing_plan.extrusion_scope"

namespace slic3r_api {

/*
Positive facts describing one compact ordered scope.

Transition flags describe which phase children physically exist. Tool-change
flags record a real tool selection inside that transition, including selections
made by empty tool groups. Materialized flags describe an already present
process movement between two scopes; they do not create additional children.
START and TERMINAL identify the global ends of the ordered plan independently
from transition phases.
*/
enum PrintingExtrusionScopeFlag : uint8_t
{
    /* The root owns travel and before children preceding its content. */
    PRINTING_EXTRUSION_SCOPE_INCOMING_TRANSITION = uint8_t(1u << 0),

    /* The root owns an after child following its content. */
    PRINTING_EXTRUSION_SCOPE_OUTGOING_TRANSITION = uint8_t(1u << 1),

    /* A Travel leaf already connects the preceding scope to this scope. */
    PRINTING_EXTRUSION_SCOPE_INCOMING_TRAVEL_MATERIALIZED = uint8_t(1u << 2),

    /* A Travel leaf already connects this scope to the following scope. */
    PRINTING_EXTRUSION_SCOPE_OUTGOING_TRAVEL_MATERIALIZED = uint8_t(1u << 3),

    /* This is the first printable scope in the complete PrintingPlan. */
    PRINTING_EXTRUSION_SCOPE_START = uint8_t(1u << 4),

    /* This is the last printable scope in the complete PrintingPlan. */
    PRINTING_EXTRUSION_SCOPE_TERMINAL = uint8_t(1u << 5),

    /* At least one real tool selection occurs in the incoming transition. */
    PRINTING_EXTRUSION_SCOPE_INCOMING_TOOLCHANGE = uint8_t(1u << 6),

    /* At least one real tool selection occurs in the outgoing transition. */
    PRINTING_EXTRUSION_SCOPE_OUTGOING_TOOLCHANGE = uint8_t(1u << 7)
};

/*
Marker attached directly to a scope root.

The one-byte payload deliberately contains no pointers. Scope adjacency comes
from PrintingEntityPropertyTraversal, while the flags determine whether the
root has reserved travel, before or after children.
*/
struct PrintingExtrusionScopeProperty
{
    /* Bitwise combination of PrintingExtrusionScopeFlag values. */
    uint8_t flags = 0;
};

/* Return whether one compact fact is present. */
inline bool printing_extrusion_scope_has_flag(
    const PrintingExtrusionScopeProperty &property,
    const PrintingExtrusionScopeFlag flag)
{
    return (property.flags & uint8_t(flag)) != 0;
}

/* Set or clear one fact without disturbing the other scope flags. */
inline void printing_extrusion_scope_set_flag(
    PrintingExtrusionScopeProperty &property,
    const PrintingExtrusionScopeFlag flag,
    const bool enabled)
{
    if (enabled)
        property.flags = uint8_t(property.flags | uint8_t(flag));
    else
        property.flags = uint8_t(property.flags & ~uint8_t(flag));
}

/*
Register and return this private payload's typed runtime key.

Every producer and consumer must call this helper with its own orchestrator
instead of persisting the numeric property id. Compatible registrations share
the same id inside that orchestrator; ids are not stable across orchestrators.
*/
inline PluginPropertyKey<PrintingExtrusionScopeProperty>
printing_extrusion_scope_property_key(orchestrator_handle *orchestrator)
{
    return PluginPropertyKey<PrintingExtrusionScopeProperty>::register_dynamic(
        orchestrator, PRINTING_EXTRUSION_SCOPE_PROPERTY_NAME);
}

} // namespace slic3r_api

#endif // slic3r_Plugins_PrintingPlan_PrintingExtrusionScopeProperty_h_
