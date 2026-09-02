///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_Plugins_Ordering_NearestNeighborOrderingEngine_hpp_
#define slic3r_Plugins_Ordering_NearestNeighborOrderingEngine_hpp_

#include "libslic3r/Api/plugin/cpp/OrderingEngine.hpp"

namespace slic3r_api { namespace Ordering { namespace DefaultOrderingPlugin {

/*
Straightforward greedy implementation of OrderingEngine.

At every step, the engine examines every candidate of every remaining item and
selects the candidate with the lowest incoming transition cost. This direct
scan intentionally avoids spatial indices and global optimization. It makes
the implementation useful as a readable reference and for small inputs where
algorithmic simplicity matters more than asymptotic performance.

For N items and C total candidates, ordering takes O(N * C) time in the general
case and O(N) auxiliary memory. When items expose a similar number of
candidates, this is commonly described as quadratic in the item count.
*/
class NearestNeighborOrderingEngine final : public OrderingEngine
{
public:
    /* Validate the request and greedily select the nearest remaining candidate. */
    std::vector<OrderedItem> order(
        const std::vector<OrderingItem> &items,
        std::optional<c_point> initial_position = std::nullopt) const override;
};

}}} // namespace slic3r_api::Ordering::DefaultOrderingPlugin

#endif // slic3r_Plugins_Ordering_NearestNeighborOrderingEngine_hpp_
