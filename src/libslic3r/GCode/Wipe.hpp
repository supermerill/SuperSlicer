#ifndef slic3r_GCode_Wipe_hpp_
#define slic3r_GCode_Wipe_hpp_

// #include "SmoothPath.hpp"

#include "../Geometry/ArcWelder.hpp"
#include "../Point.hpp"
#include "../PrintConfig.hpp"
#include "../ExtrusionEntity.hpp"

#include "GCodeWriter.hpp"

#include <cassert>
#include <optional>

namespace Slic3r {

class GCodeGenerator;

namespace GCode {

class Wipe {
public:
    using Path = Slic3r::Geometry::ArcWelder::Path;

    Wipe() = default;

    void            init(const PrintConfig &config, const GCodeWriter &writer, const std::vector<uint16_t> &extruders);
    void            enable(double wipe_len_max) { m_enabled = true; m_wipe_len_max = wipe_len_max; }
    void            disable() { m_enabled = false; }
    bool            is_enabled() const { return m_enabled; }

    const Path&     path() const { return m_path; }
    bool            has_path() const { assert(m_path.empty() || m_path.size() > 1); return ! m_path.empty(); }
    void            reset_path() { m_path.clear(); m_offset = Point::Zero(); }
    void            set_path(const Path &path) {
        assert(path.empty() || path.size() > 1);
        this->reset_path(); 
        if (this->is_enabled() && path.size() > 1) 
            m_path = path;
    }
    void            set_path(Path &&path) {
        assert(path.empty() || path.size() > 1);
        this->reset_path(); 
        if (this->is_enabled() && path.size() > 1)
            m_path = std::move(path);
    }
    void            set_path(const ExtrusionPaths &paths, bool reversed);
    void            offset_path(const Point &v) { m_offset += v; }

    std::string     wipe(GCodeGenerator &gcodegen, bool toolchange);

    // Reduce feedrate a bit; travel speed is often too high to move on existing material.
    // Too fast = ripping of existing material; too slow = short wipe path, thus more blob.
    static std::pair<double, bool>   calc_wipe_speed(const GCodeWriter &writer);
    // Reduce retraction length a bit to avoid effective retraction speed to be greater than the configured one
    // due to rounding (TODO: test and/or better math for this).
    static double calc_xy_to_e_ratio(const GCodeWriter &writer, unsigned int extruder_id) 
        { return 0.95 * floor(writer.gcode_config().retract_speed.get_at(extruder_id) + 0.5) / calc_wipe_speed(writer).first; }
    
    //superslicer method to update the wipe 
    //void            append(const Point &p);
    //void            append(const Polyline &p);
    //void            set(const Polyline &p);
    //void            reverse() { path.reverse(); }
    //void            clip_start(coord_t dist) { path.clip_start(dist); }
    //void            translate(const Point &trsl) { path.translate(trsl); } // replaced by offset_path

private:
    bool    m_enabled{ false };
    // Maximum length of a path to accumulate. Only wipes shorter than this threshold will be requested.
    double  m_wipe_len_max{ 0. };
    Path    m_path;
    // Offset from m_path to the current PrintObject active.
    Point   m_offset{ Point::Zero() };
};

// Make a little move inwards before leaving loop.
std::optional<Point> wipe_hide_seam(const ExtrusionPaths &paths, bool is_hole, double wipe_length);

} // namespace GCode
} // namespace Slic3r

#endif // slic3r_GCode_Wipe_hpp_
