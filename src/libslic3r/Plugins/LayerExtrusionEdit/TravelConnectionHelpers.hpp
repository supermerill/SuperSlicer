///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_Plugins_LayerExtrusionEdit_TravelConnectionHelpers_hpp_
#define slic3r_Plugins_LayerExtrusionEdit_TravelConnectionHelpers_hpp_

/*
Ordered travel connection helpers
=================================

Travel providers share the delicate tree mutation needed to connect compact
PrintingPlan scopes. This module owns endpoint tracking, epsilon snapping,
reserved-phase validation, Travel attributes, final materialization flags and
Z interpolation. A provider only chooses the two-dimensional path used for a
real gap.

ScopeTravelConnector mirrors the three callbacks of a layer-extrusion-edit
plugin. setup() allocates a small summary slot per PrintingLayerGroup,
setup_run() publishes each layer's first-boundary decision independently, and
run() connects local scopes through PrintingEntityPropertyTraversal. The host
barrier makes summaries immutable before any run reads another layer.

All tree mutations remain local to the current layer. In particular, a worker
never writes the previous layer's final scope: that scope's own worker updates
its outgoing flag from the next non-empty layer summary.
*/

#include <functional>
#include <vector>

#include "libslic3r/Api/plugin/cpp/PrintingPlanViews.hpp"
#include "libslic3r/Plugins/PrintingPlan/PrintingExtrusionScopeProperty.h"
#include "libslic3r/Plugins/PrintingPlan/PrintingLayerEntryStateProperties.h"

namespace slic3r_api {

class PluginProgress;

namespace LayerExtrusionEdit { namespace TravelConnection {

/* One absolute point in the coordinate system of the final PrintingPlan. */
struct PlannedPosition
{
    coord_t x = 0;
    coord_t y = 0;
    coord_t z = 0;
    bool known = false;
};

/*
Regional provenance for one side of a gap. interior_point lies up to
SCALED_EPSILON inside the geometric leaf and follows arcs; providers use it to
resolve settings when the endpoint itself lies on a region boundary.
*/
struct TravelEndpoint
{
    PlannedPosition position;
    LayerRegionIsland region_island;
    uint16_t object_instance_idx = uint16_t(-1);
    c_point interior_point{};
    bool has_interior_point = false;
};

/*
Return an ordered path including source and target. Invalid or shorter results
are rejected instead of being silently inserted into the extrusion tree.
*/
using TravelPathPlanner = std::function<std::vector<c_point>(
    const TravelEndpoint &source,
    const TravelEndpoint &target,
    uint16_t extruder_id)>;

/*
Shared lifecycle and tree editor used by one travel provider instance.

The object stores only runtime property keys, the stable flat layer mapping and
one scalar summary per layer. It never stores borrowed extrusion or property
pointers between callbacks.
*/
class ScopeTravelConnector
{
public:
    /* Register the private scope and layer-entry properties. */
    explicit ScopeTravelConnector(orchestrator_handle *orchestrator);

    /* Discard all summaries from a previous PrintingPlan execution. */
    void reset();

    /* Allocate one summary slot per final PrintingLayerGroup. */
    void setup(const PrintingPlan &plan, uint32_t run_count);

    /* Analyze one layer in its worker-owned slot before the host barrier. */
    void setup_run(const PrintingLayerGroup &layer,
                   uint32_t group_idx,
                   uint32_t layer_idx,
                   PluginProgress &progress);

    /* Connect consecutive scopes and update only properties owned by layer. */
    void run(const PrintingLayerGroup &layer,
             uint32_t group_idx,
             uint32_t layer_idx,
             const TravelPathPlanner &planner,
             PluginProgress &progress) const;

private:
    /* Information required by the previous layer's final scope. */
    struct LayerBoundarySummary
    {
        bool prepared = false;
        bool has_scope = false;
        bool first_has_incoming_transition = false;
        bool first_travel_materialized_after_run = false;
    };

    /* Convert final group/layer indexes into m_layers. */
    size_t flat_layer_index(uint32_t group_idx, uint32_t layer_idx) const;

    /* Find the next layer summary containing at least one scope. */
    const LayerBoundarySummary *next_nonempty_layer(size_t flat_idx) const;

    PluginPropertyKey<PrintingExtrusionScopeProperty> m_scope_property;
    PluginPropertyKey<PrintingLayerEntryPositionProperty> m_entry_position_property;
    bool m_setup_valid = false;
    std::vector<std::vector<size_t>> m_flat_indices;
    std::vector<LayerBoundarySummary> m_layers;
};

/* Build the direct two-point path used by the simple provider and fallbacks. */
std::vector<c_point> straight_path(const TravelEndpoint &source,
                                   const TravelEndpoint &target,
                                   uint16_t extruder_id);

}}} // namespace slic3r_api::LayerExtrusionEdit::TravelConnection

#endif // slic3r_Plugins_LayerExtrusionEdit_TravelConnectionHelpers_hpp_
