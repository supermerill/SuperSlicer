///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_Plugins_LayerExtrusionEdit_TravelConnectionHelpers_hpp_
#define slic3r_Plugins_LayerExtrusionEdit_TravelConnectionHelpers_hpp_

/*
Ordered travel connection helpers
=================================

Travel providers share the delicate tree mutation needed to connect ordered
PrintingPlan leaves. This module owns endpoint tracking, epsilon snapping,
ordered-leaf insertion, Travel attributes, and Z interpolation. A provider
only chooses the two-dimensional path used for a real gap.

The helper processes one PrintingLayerGroup at a time and keeps no shared
state, so separate layer groups may be handled concurrently.
*/

#include <functional>
#include <vector>

#include "libslic3r/Api/plugin/cpp/PrintingPlanViews.hpp"

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

/* Add the amount of work represented by one layer group to plugin progress. */
void add_layer_progress(const PrintingLayerGroup &layer, PluginProgress &progress);

/*
Connect every geometric leaf in execution order. position is initialized from
the layer entry-state property and is advanced to the last processed endpoint.
*/
void connect_layer(const PrintingLayerGroup &layer,
                   PlannedPosition &position,
                   const TravelPathPlanner &planner,
                   PluginProgress &progress);

/* Build the direct two-point path used by the simple provider and fallbacks. */
std::vector<c_point> straight_path(const TravelEndpoint &source,
                                   const TravelEndpoint &target,
                                   uint16_t extruder_id);

}}} // namespace slic3r_api::LayerExtrusionEdit::TravelConnection

#endif // slic3r_Plugins_LayerExtrusionEdit_TravelConnectionHelpers_hpp_
