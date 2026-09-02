///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_Plugins_Ordering_KDTreeOrderingEngine_hpp_
#define slic3r_Plugins_Ordering_KDTreeOrderingEngine_hpp_

#include "libslic3r/Api/plugin/cpp/OrderingEngine.hpp"

namespace slic3r_api { namespace Ordering { namespace DefaultOrderingPlugin {

/*
Scalable built-in implementation of the generic OrderingEngine interface.

Despite the class name, small requests are solved globally because bounded
dynamic programming gives a better route at modest cost. Larger requests use
an indexed greedy KD-tree search whose memory consumption grows linearly with
the number of candidates. Keeping this class in the built-in plugin module
lets another ordering plugin replace the strategy while sharing the stable
API-side request and result types.
*/
class KDTreeOrderingEngine final : public OrderingEngine
{
public:
    /* Validate the request, select the appropriate solver and return its route. */
    std::vector<OrderedItem> order(
        const std::vector<OrderingItem> &items,
        std::optional<c_point> initial_position = std::nullopt) const override;
};

}}} // namespace slic3r_api::Ordering::DefaultOrderingPlugin

#endif // slic3r_Plugins_Ordering_KDTreeOrderingEngine_hpp_
