///|/ Copyright (c) Prusa Research 2018 - 2020 Vojtěch Bubník @bubnikv, Lukáš Matěna @lukasmatena
///|/ Copyright (c) SuperSlicer 2018 Remi Durand @supermerill
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_FillGyroid_hpp_
#define slic3r_FillGyroid_hpp_

#include "../libslic3r.h"
#include "../Geometry.hpp"

#include "FillBase.hpp"

namespace Slic3r {

class FillGyroid : public Fill
{
public:
    FillGyroid() {}
    Fill* clone() const override { return new FillGyroid(*this); }

    // Density adjustment to have a good %of weight.
    static constexpr double DENSITY_ADJUST = 2.44;


protected:
    // Correction applied to regular infill angle to maximize printing
    // speed in default configuration (45 degrees)
    float _layer_angle(size_t idx) const override { return this->can_angle_cross ? float(Geometry::deg2rad(-45.)) : 0.f; }

    void _fill_surface_single(
        const FillParams                &params, 
        unsigned int                     thickness_layers,
        const std::pair<float, Point>   &direction, 
        ExPolygon                        expolygon, 
        Polylines                       &polylines_out) const override;
};

} // namespace Slic3r

#endif // slic3r_FillGyroid_hpp_
