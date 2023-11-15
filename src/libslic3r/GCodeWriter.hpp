#ifndef slic3r_GCodeWriter_hpp_
#define slic3r_GCodeWriter_hpp_

#include "libslic3r.h"
#include <string>
#include "Extruder.hpp"
#include "Point.hpp"
#include "PrintConfig.hpp"
#include "GCode/CoolingBuffer.hpp"

namespace Slic3r {

class GCodeWriter {
public:
    static std::string PausePrintCode;
    GCodeConfig config;
    bool multiple_extruders;
    // override from region
    const PrintRegionConfig* config_region = nullptr;
    
    GCodeWriter() : 
        multiple_extruders(false), m_extrusion_axis("E"), m_tool(nullptr),
        m_single_extruder_multi_material(false),
        m_last_acceleration(0), m_current_acceleration(0), m_current_speed(0),
        m_last_bed_temperature(0), m_last_bed_temperature_reached(true), 
        m_lifted(0)
        {}
    Tool*               tool()             { return m_tool; }
    const Tool*         tool()     const   { return m_tool; }

    // Returns empty string for gcfNoExtrusion.
    std::string         extrusion_axis() const { return m_extrusion_axis; }
    void                apply_print_config(const PrintConfig &print_config);
    void                apply_print_region_config(const PrintRegionConfig& print_region_config);
    // Extruders are expected to be sorted in an increasing order.
    void                set_extruders(std::vector<uint16_t> extruder_ids);
    const std::vector<Extruder>& extruders() const { return m_extruders; }
    std::vector<uint16_t> extruder_ids() const;
    void                set_mills(std::vector<uint16_t> extruder_ids);
    const std::vector<Mill>& mills() const { return m_millers; }
    std::vector<uint16_t> mill_ids() const;
    //give the first mill id or an id after the last extruder. Can be used to see if an id is an extruder or a mill
    uint16_t first_mill() const;
    bool tool_is_extruder() const;
    const Tool* get_tool(uint16_t id) const;
    std::string preamble();
    std::string postamble() const;
    std::string set_temperature(int16_t temperature, bool wait = false, int tool = -1);
    std::string set_bed_temperature(uint32_t temperature, bool wait = false);
    void        set_acceleration(uint32_t acceleration);
    void        set_travel_acceleration(uint32_t acceleration);
    uint32_t    get_acceleration() const;
    std::string write_acceleration();
    std::string reset_e(bool force = false);
    std::string update_progress(uint32_t num, uint32_t tot, bool allow_100 = false) const;
    // return false if this extruder was already selected
    bool        need_toolchange(uint16_t tool_id) const 
        { return m_tool == nullptr || m_tool->id() != tool_id; }
    std::string set_tool(uint16_t tool_id)
        { return this->need_toolchange(tool_id) ? this->toolchange(tool_id) : ""; }
    // Prefix of the toolchange G-code line, to be used by the CoolingBuffer to separate sections of the G-code
    // printed with the same extruder.
    std::string toolchange_prefix() const;
    std::string toolchange(uint16_t tool_id);
    // in mm/s
    std::string set_speed(const double speed, const std::string &comment = std::string(), const std::string &cooling_marker = std::string());
    // in mm/s
    double      get_speed() const;
    std::string travel_to_xy(const Vec2d &point, const double speed = 0.0, const std::string &comment = std::string());
    std::string travel_to_xyz(const Vec3d &point, const double speed = 0.0, const std::string &comment = std::string());
    std::string travel_to_z(double z, const std::string &comment = std::string());
    bool        will_move_z(double z) const;
    std::string extrude_to_xy(const Vec2d &point, double dE, const std::string &comment = std::string());
    std::string extrude_to_xyz(const Vec3d &point, double dE, const std::string &comment = std::string());
    std::string retract(bool before_wipe = false);
    std::string retract_for_toolchange(bool before_wipe = false);
    std::string unretract();
    void        set_extra_lift(double extra_zlift) { this->m_extra_lift = extra_zlift; }
    double      get_extra_lift() { return this->m_extra_lift; }
    std::string lift(int layer_id);
    std::string unlift();
    Vec3d       get_position() const { return m_pos; }
    Vec3d       get_unlifted_position() const { return m_pos - Vec3d{0, 0, m_extra_lift + m_lifted}; }

    // To be called by the CoolingBuffer from another thread.
    static std::string set_fan(const GCodeFlavor gcode_flavor, bool gcode_comments, uint8_t speed, uint8_t tool_fan_offset, bool is_fan_percentage);
    // To be called by the main thread. It always emits the G-code, it does remember the previous state to be able to reset after the wipe tower (but remove that when the wipe tower will be extrusions and not string).
    // Keeping the state is left to the CoolingBuffer, which runs asynchronously on another thread.
    std::string set_fan(uint8_t speed, uint16_t default_tool = 0);
    uint8_t get_fan() { return m_last_fan_speed; }

private:
	// Extruders are sorted by their ID, so that binary search is possible.
    std::vector<Extruder> m_extruders;
    std::vector<Mill> m_millers;
    std::string     m_extrusion_axis;
    bool            m_single_extruder_multi_material;
    Tool*           m_tool;
    uint32_t        m_last_acceleration;
    uint32_t        m_current_acceleration;
    uint32_t        m_current_travel_acceleration;
    double          m_current_speed;
    uint8_t         m_last_fan_speed;
    int16_t         m_last_temperature;
    int16_t         m_last_temperature_with_offset;
    int16_t         m_last_bed_temperature;
    bool            m_last_bed_temperature_reached;
    // if positive, it's set, and the next lift wil have this extra lift
    double          m_extra_lift = 0;
    // current lift, to remove from m_pos to have the current height.
    double          m_lifted;
    Vec3d           m_pos = Vec3d::Zero();

    std::string _travel_to_z(double z, const std::string &comment);
    std::string _retract(double length, double restart_extra, double restart_extra_toolchange, const std::string &comment);

};
#if 0
// removed as charconv isn't here in older system like ubuntu 16.04 (cpp 17)
#include <charconv>
#define USE_GCODEFORMATTER
class GCodeFormatter {
public:
    GCodeFormatter() {
        this->buf_end = buf + buflen;
        this->ptr_err.ptr = this->buf;
    }

    GCodeFormatter(const GCodeFormatter&) = delete;
    GCodeFormatter& operator=(const GCodeFormatter&) = delete;

    // At layer height 0.15mm, extrusion width 0.2mm and filament diameter 1.75mm,
    // the crossection of extrusion is 0.4 * 0.15 = 0.06mm2
    // and the filament crossection is 1.75^2 = 3.063mm2
    // thus the filament moves 3.063 / 0.6 = 51x slower than the XY axes
    // and we need roughly two decimal digits more on extruder than on XY.
#if 1
    static constexpr const int XYZF_EXPORT_DIGITS = 3;
    static constexpr const int E_EXPORT_DIGITS    = 5;
#else
    // order of magnitude smaller extrusion rate erros
    static constexpr const int XYZF_EXPORT_DIGITS = 4;
    static constexpr const int E_EXPORT_DIGITS    = 6;
    // excessive accuracy
//    static constexpr const int XYZF_EXPORT_DIGITS = 6;
//    static constexpr const int E_EXPORT_DIGITS    = 9;
#endif

    void emit_axis(const char axis, const double v, size_t digits);

    void emit_xy(const Vec2d &point) {
        this->emit_axis('X', point.x(), XYZF_EXPORT_DIGITS);
        this->emit_axis('Y', point.y(), XYZF_EXPORT_DIGITS);
    }

    void emit_xyz(const Vec3d &point) {
        this->emit_axis('X', point.x(), XYZF_EXPORT_DIGITS);
        this->emit_axis('Y', point.y(), XYZF_EXPORT_DIGITS);
        this->emit_z(point.z());
    }

    void emit_z(const double z) {
        this->emit_axis('Z', z, XYZF_EXPORT_DIGITS);
    }

    void emit_e(const std::string &axis, double v) {
        if (! axis.empty()) {
            // not gcfNoExtrusion
            this->emit_axis(axis[0], v, E_EXPORT_DIGITS);
        }
    }

    void emit_f(double speed) {
        this->emit_axis('F', speed, XYZF_EXPORT_DIGITS);
    }

    void emit_string(const std::string &s) {
        strncpy(ptr_err.ptr, s.c_str(), s.size());
        ptr_err.ptr += s.size();
    }

    void emit_comment(bool allow_comments, const std::string &comment) {
        if (allow_comments && ! comment.empty()) {
            *ptr_err.ptr ++ = ' '; *ptr_err.ptr ++ = ';'; *ptr_err.ptr ++ = ' ';
            this->emit_string(comment);
        }
    }

    std::string string() {
        *ptr_err.ptr ++ = '\n';
        return std::string(this->buf, ptr_err.ptr - buf);
    }

protected:
    static constexpr const size_t   buflen = 256;
    char                            buf[buflen];
    char* buf_end;
    std::to_chars_result            ptr_err;
};

class GCodeG1Formatter : public GCodeFormatter {
public:
    GCodeG1Formatter() {
        this->buf[0] = 'G';
        this->buf[1] = '1';
        this->buf_end = buf + buflen;
        this->ptr_err.ptr = this->buf + 2;
    }

    GCodeG1Formatter(const GCodeG1Formatter&) = delete;
    GCodeG1Formatter& operator=(const GCodeG1Formatter&) = delete;
};
#endif

} /* namespace Slic3r */

#endif /* slic3r_GCodeWriter_hpp_ */
