///|/ Copyright (c) Prusa Research 2020 - 2023 Enrico Turri @enricoturri1966
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_LibVGCodeWrapper_hpp_
#define slic3r_LibVGCodeWrapper_hpp_

#if ENABLE_NEW_GCODE_VIEWER
#include "libslic3r/Color.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "slic3r/GUI/GUI_Preview.hpp"

#include <libvgcode/include/Viewer.hpp>
#include <libvgcode/include/PathVertex.hpp>
#include <libvgcode/include/GCodeInputData.hpp>
#include <libvgcode/include/ColorRange.hpp>

namespace Slic3r {
class Print;
} // namespace Slic3r

namespace libvgcode {

// mapping from Slic3r::Vec3f to libvgcode::Vec3
extern Vec3 convert(const Slic3r::Vec3f& v);

// mapping from libvgcode::Vec3 to Slic3r::Vec3f
extern Slic3r::Vec3f convert(const Vec3& v);

// mapping from Slic3r::Matrix4f to libvgcode::Mat4x4
extern Mat4x4 convert(const Slic3r::Matrix4f& m);

// mapping from libvgcode::Color to Slic3r::ColorRGBA
extern Slic3r::ColorRGBA convert(const Color& c);

// mapping from Slic3r::ColorRGBA to libvgcode::Color
extern Color convert(const Slic3r::ColorRGBA& c);

// mapping from libvgcode::EGCodeExtrusionRole to Slic3r::GCodeExtrusionRole
extern Slic3r::GCodeExtrusionRole convert(EGCodeExtrusionRole role);

// mapping from Slic3r::GCodeExtrusionRole to libvgcode::EGCodeExtrusionRole
extern EGCodeExtrusionRole convert(Slic3r::GCodeExtrusionRole role);

// mapping from Slic3r::EMoveType to libvgcode::EMoveType
extern EMoveType convert(Slic3r::EMoveType type);

// mapping from Slic3r::GUI::Preview::OptionType to libvgcode::EOptionType
extern EOptionType convert(const Slic3r::GUI::Preview::OptionType& type);

// mapping from Slic3r::PrintEstimatedStatistics::ETimeMode to libvgcode::ETimeMode
extern ETimeMode convert(const Slic3r::PrintEstimatedStatistics::ETimeMode& mode);

// mapping from libvgcode::ETimeMode to Slic3r::PrintEstimatedStatistics::ETimeMode
extern Slic3r::PrintEstimatedStatistics::ETimeMode convert(const ETimeMode& mode);

// mapping from Slic3r::GCodeProcessorResult to libvgcode::GCodeInputData
extern GCodeInputData convert(const Slic3r::GCodeProcessorResult& result, float travels_radius = DEFAULT_TRAVELS_RADIUS_MM,
    float wipes_radius = DEFAULT_WIPES_RADIUS_MM);

// mapping from Slic3r::Print to libvgcode::GCodeInputData
extern GCodeInputData convert(const Slic3r::Print& print, const std::vector<std::string>& str_tool_colors,
    const std::vector<Slic3r::CustomGCode::Item>& color_print_values, size_t extruders_count);

} // namespace libvgcode

#endif // ENABLE_NEW_GCODE_VIEWER

#endif // slic3r_LibVGCodeWrapper_hpp_
