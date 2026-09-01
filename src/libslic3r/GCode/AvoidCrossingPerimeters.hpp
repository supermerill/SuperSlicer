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
#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "libslic3r/EdgeGrid.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Point.hpp"

namespace Slic3r {

// Forward declarations.
class Layer;
class PrintObject;

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

    /*
    Immutable inputs used to build the cached geometry for perimeter-crossing
    tests. The referenced objects only need to remain alive during
    prepare_crossing_test(); all generated polygons are then owned by this
    AvoidCrossingPerimeters instance.

    already_printed_object_layers is needed only for support layers, where a
    travel may cross any object already printed at the current height.
    nozzle_radius uses scaled coordinates and extruder_id is zero based.
    */
    struct PerimeterCrossingContext
    {
        const Layer &layer;
        const std::vector<const Layer *> &already_printed_object_layers;
        uint64_t instance_id;
        uint16_t extruder_id;
        coord_t nozzle_radius;
        std::function<void()> throw_if_canceled;
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

    /*
    Prepare the simplified and offset layer slices used by the hot crossing
    test. Geometry is rebuilt only when the supplied context changes. The
    return value reports an object or instance transition so the legacy G-code
    generator can preserve its conservative behavior without making that
    policy part of the geometric query.
    */
    bool prepare_crossing_test(const PerimeterCrossingContext &context);

    /*
    Return true when the prepared travel leaves a contour or crosses one of
    its holes. offset selects the nozzle-clearance cache used by avoid-crossing
    travel planning. prepare_crossing_test() must be called first.
    */
    bool can_cross_perimeter(const Polyline &travel, bool offset);

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
    /* One simplified printable island used by the crossing hot path. */
    struct SliceIsland
    {
        ExPolygon expolygon;
        BoundingBox boundingbox;
        std::vector<BoundingBox> hole_boundingboxes;

        SliceIsland(ExPolygon &&expolygon, BoundingBox &&boundingbox) :
            expolygon(std::move(expolygon)), boundingbox(std::move(boundingbox))
        {}
#ifdef CAN_CROSS_PERIMETER_USE_GRID
        std::optional<EdgeGrid::Grid> grid;
        SliceIsland(ExPolygon &&expolygon, BoundingBox &&boundingbox, EdgeGrid::Grid &&grid) :
            expolygon(std::move(expolygon)), boundingbox(std::move(boundingbox)), grid(std::move(grid))
        {}
#endif
        void create_hole_bounding_boxes();
    };

    /* Cached normal and nozzle-offset slices for one execution context. */
    struct CrossingCache
    {
        std::vector<SliceIsland> slices;
        std::vector<SliceIsland> offset_slices;
        const Layer *layer = nullptr;
        const PrintObject *object = nullptr;
        uint64_t instance_id = uint64_t(-1);
        uint16_t extruder_id = uint16_t(-1);
        coord_t nozzle_radius = 0;
    };

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

    // The crossing cache belongs to the caller-owned router. A plugin creates
    // one router per worker, so no synchronization is needed around it.
    CrossingCache m_crossing_cache;
    std::function<void()> m_crossing_throw_if_canceled = []() {};
};

} // namespace Slic3r

#endif // slic3r_AvoidCrossingPerimeters_hpp_
