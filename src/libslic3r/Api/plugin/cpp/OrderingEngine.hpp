///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_Api_plugin_cpp_OrderingEngine_hpp_
#define slic3r_Api_plugin_cpp_OrderingEngine_hpp_

#include <cstdint>
#include <optional>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_geometry.h"

namespace slic3r_api { namespace Ordering {

/*
Geometry-only interface for arranging independent printable items.

The engine deliberately knows nothing about PrintingExtrusion, extrusion
trees, seams or reversals. A caller describes every legal way to enter and
leave an item with OrderingCandidate values, asks an OrderingEngine for a
sequence, then applies that sequence to its own data structures.

Entry and exit positions are estimates. Later processing may move a seam, add
a seam gap or append a wipe without invalidating the ordering contract. The
engine only tries to reduce the estimated transition cost between items.
*/
struct OrderingCandidate
{
    // Approximate position at which printing this candidate begins. Coordinates
    // use the scaled slicer coordinate system carried by c_point.
    c_point estimated_entry = {};

    // Approximate position left after printing the candidate. A closed loop
    // normally uses the same point for entry and exit.
    c_point estimated_exit = {};

    // Dimensionless weight applied to the XY distance travelled to this
    // candidate. Zero makes the candidate's transition cost independent of
    // distance and leaves only penalty_mm.
    double distance_ratio = 1.0;

    // Intrinsic cost of selecting this candidate, expressed in millimetres so
    // it can be added to the weighted transition distance.
    double penalty_mm = 0.0;
};

/*
One independently reorderable item.

Multiple candidates describe alternatives for the same item, not duplicate
items. For example, a reversible open path exposes front-to-back and
back-to-front candidates, while a loop may expose one candidate per possible
seam. Every item must contain at least one candidate.
*/
struct OrderingItem
{
    std::vector<OrderingCandidate> candidates;
};

/*
One selected item in its new sequence position.

item_index always refers to the original input vector. selected_candidate is a
copy of the entry/exit estimate selected by the engine, allowing the caller to
sort nested content without retaining pointers into the request.
*/
struct OrderedItem
{
    uint32_t item_index = 0;
    OrderingCandidate selected_candidate;
};

/*
Replaceable ordering strategy used by PrintingPlan ordering plugins.

Implementations must return each input item exactly once. They must not mutate
the request. The transition cost for a destination candidate is:

    distance_xy_mm * distance_ratio + penalty_mm

When initial_position is absent, implementations choose a deterministic first
item without charging travel from an invented machine position.
*/
class OrderingEngine
{
public:
    /* Destroy an implementation through the public strategy interface. */
    virtual ~OrderingEngine() = default;

    /*
    Return a complete permutation of items and the candidate selected for each.

    Implementations should reject malformed requests with std::invalid_argument
    rather than returning a partial sequence. An empty request returns an empty
    result.
    */
    virtual std::vector<OrderedItem> order(
        const std::vector<OrderingItem> &items,
        std::optional<c_point> initial_position = std::nullopt) const = 0;
};

}} // namespace slic3r_api::Ordering

#endif // slic3r_Api_plugin_cpp_OrderingEngine_hpp_
