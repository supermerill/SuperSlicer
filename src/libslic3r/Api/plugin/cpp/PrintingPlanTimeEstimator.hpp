///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_Api_plugin_cpp_PrintingPlanTimeEstimator_hpp_
#define slic3r_Api_plugin_cpp_PrintingPlanTimeEstimator_hpp_

#include <cstdint>
#include <vector>

#include "libslic3r/Api/plugin/cpp/ConfigViews.hpp"
#include "libslic3r/Api/plugin/cpp/PrintingPlanViews.hpp"

namespace slic3r_api {

/*
Fallback duration estimator for an ordered PrintingPlan.

The estimator measures only movements whose duration can be inferred from the
plan: extrusion/travel geometry and synthetic travels between separate leaves.
It deliberately ignores acceleration, scripts, retractions, and machine-side
delays. The result is therefore an estimate, not an authoritative G-code time.

One result is returned for every PrintingLayerGroup in group/layer order. The
machine position is retained between results so the travel entering a layer is
charged to that destination layer.
*/
struct PrintingLayerTimeEstimate
{
    uint32_t group_idx = 0;
    uint32_t layer_group_idx = 0;
    double duration_seconds = 0.0;
};

class PrintingPlanTimeEstimator
{
public:
    explicit PrintingPlanTimeEstimator(const Config &print_config);

    std::vector<PrintingLayerTimeEstimate> estimate(const PrintingPlan &plan) const;

private:
    double m_travel_speed_mm_per_s = 0.0;
};

} // namespace slic3r_api

#endif // slic3r_Api_plugin_cpp_PrintingPlanTimeEstimator_hpp_
