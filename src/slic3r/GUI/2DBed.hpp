///|/ Copyright (c) SuperSlicer 2026 Durand R?mi @supermerill
///|/ Copyright (c) Prusa Research 2018 - 2019 Enrico Turri @enricoturri1966, Oleksandra Iushchenko @YuSanka, Vojtěch Bubník @bubnikv
///|/
///|/ ported from lib/Slic3r/GUI/2DBed.pm:
///|/ Copyright (c) Prusa Research 2016 - 2018 Vojtěch Bubník @bubnikv
///|/ Copyright (c) Slic3r 2015 Alessandro Ranellucci @alranel
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_2DBed_hpp_
#define slic3r_2DBed_hpp_

#include <wx/wx.h>

#include "libslic3r/Config/ConfigOption.hpp"
namespace Slic3r {
namespace GUI {

class Bed_2D : public wxPanel
{
    static const int Border = 10;

	bool		m_user_drawn_background = true;

    double		m_scale_factor;
	Vec2d		m_shift = Vec2d::Zero();
	Vec2d		m_pos = Vec2d::Zero();

    Point		to_pixels(const Vec2d& point, int height);
    void		set_pos(const Vec2d& pos);

public:
    explicit Bed_2D(wxWindow* parent);

    void repaint(const std::vector<Vec2d>& shape);
};


} // GUI
} // Slic3r

#endif /* slic3r_2DBed_hpp_ */
