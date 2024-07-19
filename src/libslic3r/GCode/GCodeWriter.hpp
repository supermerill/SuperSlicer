///|/ Copyright (c) Prusa Research 2016 - 2023 Vojtěch Bubník @bubnikv, Lukáš Matěna @lukasmatena, Oleksandra Iushchenko @YuSanka, David Kocík @kocikdav
///|/ Copyright (c) 2016 Chow Loong Jin @hyperair
///|/ Copyright (c) Slic3r 2014 - 2015 Alessandro Ranellucci @alranel
///|/ Copyright (c) 2015 Maksim Derbasov @ntfshard
///|/ Copyright (c) 2015 Alexander Rössler @machinekoder
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_GCodeWriter_hpp_
#define slic3r_GCodeWriter_hpp_

#include "../libslic3r.h"
#include "../Extruder.hpp"
#include "../Point.hpp"
#include "../PrintConfig.hpp"
#include "CoolingBuffer.hpp"
#include "GCodeFormatter.hpp"

#include <string>
#include <string_view>
#include <vector>
#include <charconv>

namespace Slic3r {

class GCodeWriter {
public:
    GCodeConfig config;
    bool multiple_extruders;
    // override from region
    const PrintRegionConfig* config_region = nullptr;
    
    GCodeWriter() {}
    Tool*               tool()             { return m_tool; }
    const Tool*         tool()     const   { return m_tool; }

    Vec2d               current_tool_offset() const;
    const GCodeConfig &      gcode_config() const { return config; }
    const PrintRegionConfig *print_region_config() const { return config_region; }

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
    std::string set_chamber_temperature(uint32_t temperature, bool wait = false);
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
    std::string set_speed(const double speed, const std::string_view comment = {}, const std::string_view cooling_marker = {});
    // in mm/s
    double      get_speed() const;
    std::string travel_to_xy(const Vec2d &point, const double speed = 0.0, const std::string_view comment = {});
    std::string travel_arc_to_xy(const Vec2d& point, const Vec2d& center_offset, const bool is_ccw, const double speed, const std::string_view comment);
    std::string travel_to_xyz(const Vec3d &point, const double speed = 0.0, const std::string_view comment = {});
    std::string travel_to_z(double z, const std::string_view comment = {});
    std::string get_travel_to_z_gcode(double z, const std::string_view comment = {});
    bool        will_move_z(double z) const;
    std::string extrude_to_xy(const Vec2d &point, double dE, const std::string_view comment = {});
    std::string extrude_arc_to_xy(const Vec2d& point, const Vec2d& center_offset, double dE, const bool is_ccw, const std::string_view comment = {}); //BBS: generate G2 or G3 extrude which moves by arc
    std::string extrude_to_xyz(const Vec3d &point, double dE, const std::string_view comment = {});
    std::string retract(bool before_wipe = false);
    std::string retract_for_toolchange(bool before_wipe = false);
    std::string unretract();
    void        set_extra_lift(double extra_zlift) { this->m_extra_lift = extra_zlift; }
    double      get_extra_lift() const { return this->m_extra_lift; }
    double      get_lift() const { return this->m_lifted; } // for placeholder
    std::string lift(int layer_id);
    std::string unlift();

    // Current position of the printer, in G-code coordinates.
    // Z coordinate of current position contains zhop. If zhop is applied (this->zhop() > 0),
    // then the print_z = this->get_position().z() - this->zhop().
    Vec3d       get_position() const { return m_pos; }
    Vec3d       get_unlifted_position() const { return m_pos - Vec3d{0, 0, m_extra_lift + m_lifted}; }
    // Update position of the print head based on the final position returned by a custom G-code block.
    // The new position Z coordinate contains the Z-hop.
    // GCodeWriter expects the custom script to NOT change print_z, only Z-hop, thus the print_z is maintained
    // by this function while the current Z-hop accumulator is updated.
    void        update_position(const Vec3d &new_pos);

    // Returns whether this flavor supports separate print and travel acceleration.
    static bool supports_separate_travel_acceleration(GCodeFlavor flavor);

    // To be called by the CoolingBuffer from another thread.
    static std::string set_fan(const GCodeFlavor gcode_flavor, bool gcode_comments, uint8_t speed, uint8_t tool_fan_offset, bool is_fan_percentage, const std::string_view comment = {});
    // To be called by the main thread. It always emits the G-code, it does remember the previous state to be able to reset after the wipe tower (but remove that when the wipe tower will be extrusions and not string).
    // Keeping the state is left to the CoolingBuffer, which runs asynchronously on another thread.
    std::string set_fan(uint8_t speed, uint16_t default_tool = 0);
    uint8_t get_fan() { return m_last_fan_speed; }

    GCodeFormatter get_default_gcode_formatter() const { return GCodeFormatter(config.gcode_precision_xyz, config.gcode_precision_e); }

    static std::string get_default_pause_gcode(const GCodeConfig &config);
    static std::string get_default_color_change_gcode(const GCodeConfig &config);

private:
	// Extruders are sorted by their ID, so that binary search is possible.
    std::vector<Extruder> m_extruders;
    std::vector<Mill> m_millers;
    std::string     m_extrusion_axis = "E";
    bool            m_single_extruder_multi_material = false;
    Tool*           m_tool = nullptr;
    uint32_t        m_last_acceleration = uint32_t(0);
    uint32_t        m_last_travel_acceleration = uint32_t(0);
    uint32_t        m_current_acceleration = 0;
    uint32_t        m_current_travel_acceleration = 0;
    //uint32_t        m_max_acceleration;
    //uint32_t        m_max_travel_acceleration;
    double          m_current_speed = 0;
    uint8_t         m_last_fan_speed = 0;
    int16_t         m_last_temperature = 0;
    int16_t         m_last_temperature_with_offset = 0;
    int16_t         m_last_bed_temperature = 0;
    bool            m_last_bed_temperature_reached = true;
    int16_t         m_last_chamber_temperature = 0;
    // if positive, it's set, and the next lift wil have this extra lift
    double          m_extra_lift = 0;
    // current lift, to remove from m_pos to have the current height.
    double          m_lifted = 0;
    Vec3d           m_pos = Vec3d::Zero();
    // cached string representation of x & y m_pos
    std::string     m_pos_str_x;
    std::string     m_pos_str_y;
    // stored de that wasn't written, because of the rounding
    double          m_de_left = 0;
    
    
    GCodeFormatter  m_formatter {0,0};
    
    std::string _retract(double length, std::optional<double> restart_extra, std::optional<double> restart_extra_toolchange, const std::string_view comment = {});

};

} /* namespace Slic3r */

#endif /* slic3r_GCodeWriter_hpp_ */
