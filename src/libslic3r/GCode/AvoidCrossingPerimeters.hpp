///|/ Copyright (c) SuperSlicer 2026 Durand R?mi @supermerill
///|/ Copyright (c) Prusa Research 2020 - 2022 Lukáš Hejl @hejllukas, Vojtěch Bubník @bubnikv
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_AvoidCrossingPerimeters_hpp_
#define slic3r_AvoidCrossingPerimeters_hpp_

#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "libslic3r/EdgeGrid.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Point.hpp"

namespace Slic3r {

// Forward declarations.
class Layer;

class AvoidCrossingPerimeters
{
public:
    /*
    Immutable inputs needed to route one travel without depending on the caller
    that owns the current machine state. The referenced Layer must remain alive
    for the duration of travel_to(); the context never owns or modifies it.

    start and origin use scaled core coordinates. start is expressed in the
    active object's coordinate system; origin translates it into print space
    when external routing is selected. An absolute max_detour value is expressed
    in millimetres, while a relative value is a percentage of the direct travel.
    Values at or below zero disable the detour limit.
    */
    struct TravelContext
    {
        const Layer &layer;
        Point start;
        Point origin;
        uint16_t extruder_id;
        double max_detour;
        bool max_detour_is_percent;
    };

    // Routing around the objects vs. inside a single object.
    void        use_external_mp(bool use = true) { m_use_external_mp = use; };
    void        use_external_mp_once()  { m_use_external_mp_once = true; }
    bool        used_external_mp_once() { return m_use_external_mp_once; }
    void        disable_once()          { m_disabled_once = true; }
    bool        disabled_once() const   { return m_disabled_once; }
    void        reset_once_modifiers()  { m_use_external_mp_once = false; m_disabled_once = false; }

    void        init_layer(const Layer &layer);
    bool        is_init() { return m_init; }

    Polyline    travel_to(const TravelContext &context, const Point &point)
    {
        bool could_be_wipe_disabled = false;
        return this->travel_to(context, point, &could_be_wipe_disabled);
    }

    Polyline    travel_to(const TravelContext &context, const Point &point, bool *could_be_wipe_disabled);

    struct Boundary {
        // Collection of boundaries used for detection of crossing perimeters for travels
        Polygons                        boundaries;
        // this is empty if m_use_external_mp, or the size of boundaries if not.
        // each entry is the boundary's island id. the island id is the boundary index of the contour.
        std::vector<size_t>             islands;
        // Bounding box of boundaries
        BoundingBoxf                    bbox;
        std::vector<BoundingBox>        bboxes;
        // Precomputed distances of all points in boundaries
        std::vector<std::vector<float>> boundaries_params;
        // Used for detection of intersection between line and any polygon from boundaries
        EdgeGrid::Grid                  grid;
        // grid for searching in the contour of one island. the id of the island is the idx of the contour in boundaries
        std::map<int, EdgeGrid::Grid>   island_to_grid;
        //used to move the point inside the boundary
        std::vector<std::pair<ExPolygon, ExPolygon>> boundary_growth;
        // area (top) where you don't want to travel, even more so than over voids.
        ExPolygons to_avoid;
        // Used for detection of intersection between line and any polygon from to_avoid
        EdgeGrid::Grid to_avoid_grid;

        void clear()
        {
            boundaries.clear();
            islands.clear();
            bbox = BoundingBoxf();
            bboxes.clear();
            boundaries_params.clear();
            grid = EdgeGrid::Grid();
            island_to_grid.clear();
            boundary_growth.clear();
            to_avoid.clear();
            to_avoid_grid = EdgeGrid::Grid();
        }
    };

private:
    bool           m_use_external_mp { false };
    // just for the next travel move
    bool           m_use_external_mp_once { false };
    // this flag disables avoid_crossing_perimeters just for the next travel move
    // we enable it by default for the first travel move in print
    bool           m_disabled_once { true };

    bool m_init{ false };

    // for assert, to see if we are correctly initialized
    const Layer             *m_init_to;
    // Layer slices offset by half an external perimeter width. A non-empty
    // result tells travel_to() that internal perimeter routing is available.
    ExPolygons               m_lslices_offset;
    // Store all needed data for travels inside object
    Boundary m_internal;
    // Store all needed data for travels outside object
    Boundary m_external;
};

} // namespace Slic3r

#endif // slic3r_AvoidCrossingPerimeters_hpp_
