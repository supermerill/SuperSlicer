///|/ Copyright (c) Prusa Research 2016 - 2023 Vojtěch Bubník @bubnikv, Lukáš Matěna @lukasmatena, Lukáš Hejl @hejllukas, Oleksandra Iushchenko @YuSanka
///|/ Copyright (c) 2021 Raphael Sobik
///|/ Copyright (c) 2021 Martin Budden
///|/ Copyright (c) 2020 Paul Arden @ardenpm
///|/ Copyright (c) 2016 Joseph Lenox @lordofhyphens
///|/ Copyright (c) 2016 Chow Loong Jin @hyperair
///|/ Copyright (c) Slic3r 2014 - 2015 Alessandro Ranellucci @alranel
///|/ Copyright (c) 2015 Maksim Derbasov @ntfshard
///|/ Copyright (c) 2015 Maciej Dębski
///|/ Copyright (c) 2015 Alexander Rössler @machinekoder
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "GCodeWriter.hpp"
#include "../CustomGCode.hpp"

#include "LocalesUtils.hpp"

#include <boost/lexical_cast.hpp>

#include <algorithm>
#include <assert.h>
#include <iomanip>
#include <iostream>
#include <map>
#include <string_view>
#include <boost/math/special_functions/pow.hpp>
#ifdef __APPLE__
    #include <boost/spirit/include/karma.hpp>
#endif

#define FLAVOR_IS(val) this->config.gcode_flavor.value == val
#define FLAVOR_IS_NOT(val) this->config.gcode_flavor.value != val
// TODO: switch to gcodeformatter classes
#define COMMENT(comment) if (this->config.gcode_comments.value && !comment.empty()) gcode << " ; " << comment;
#define PRECISION(val, precision) to_string_nozero(val, precision)
#define XYZ_NUM(val) PRECISION(val, this->config.gcode_precision_xyz.value)
#define FLOAT_PRECISION(val, precision) std::defaultfloat << std::setprecision(precision) << (val)
#define F_NUM(val) FLOAT_PRECISION(val, 8)
#define E_NUM(val) PRECISION(val, this->config.gcode_precision_e.value)
using namespace std::string_view_literals;

namespace Slic3r {
    
// static
bool GCodeWriter::supports_separate_travel_acceleration(GCodeFlavor flavor)
{
    return (flavor == gcfRepetier || flavor == gcfMarlinFirmware ||  flavor == gcfRepRap);
}

std::string GCodeWriter::get_default_pause_gcode(const GCodeConfig &config)
{
    if (config.pause_print_gcode.value.empty()) {
        if (config.gcode_flavor.value == GCodeFlavor::gcfKlipper) {
            return "PAUSE";
        } else if (config.gcode_flavor.value == GCodeFlavor::gcfRepRap || config.gcode_flavor.value == GCodeFlavor::gcfMarlinLegacy /*prusa only*/) {
            return "M601";
        } else {
            // no pause command for other firmware, for what i am aware. Please submit a pullrequest or issue if they change.
            return "";
        }
    } else {
        return config.pause_print_gcode.value;
    }
}

std::string GCodeWriter::get_default_color_change_gcode(const GCodeConfig &config)
{
    if (config.color_change_gcode.value.empty()) {
        if (config.gcode_flavor.value == GCodeFlavor::gcfRepRap || config.gcode_flavor.value == GCodeFlavor::gcfMarlinLegacy ||
            config.gcode_flavor.value == GCodeFlavor::gcfMarlinFirmware || config.gcode_flavor.value == GCodeFlavor::gcfSmoothie) {
            return "M600";
        } else {
            // no pause command for other firmware, for what i am aware. Please submit a pullrequest or issue if they change.
            return "";
        }
    } else {
        return config.color_change_gcode.value;
    }
}

void GCodeWriter::apply_print_config(const PrintConfig &print_config)
{
    this->multiple_extruders = false;
    this->config.apply(print_config, true);
    m_extrusion_axis = get_extrusion_axis(this->config);
    m_single_extruder_multi_material = print_config.single_extruder_multi_material.value;
    m_formatter = GCodeFormatter(this->config.gcode_precision_xyz.value, this->config.gcode_precision_e.value);
}

void GCodeWriter::apply_print_region_config(const PrintRegionConfig& print_region_config)
{
    config_region = &print_region_config;
}

Vec2d GCodeWriter::current_tool_offset() const {
    if (this->tool()) {
        return this->tool()->xy_offset();
    }
    return Vec2d { 0, 0 };
}

std::vector<uint16_t> GCodeWriter::extruder_ids() const {
    std::vector<uint16_t> out;
    out.reserve(m_extruders.size());
    for (const Extruder& e : m_extruders)
        out.push_back(e.id());
    return out;
}

std::vector<uint16_t> GCodeWriter::mill_ids() const {
    std::vector<uint16_t> out;
    out.reserve(m_millers.size());
    for (const Tool& e : m_millers)
        out.push_back(e.id());
    return out;
}

uint16_t GCodeWriter::first_mill() const {
    if (m_millers.empty()) {
        uint16_t max = 0;
        for (const Extruder& e : m_extruders)
            max = std::max(max, e.id());
        max++;
        return (uint16_t)max;
    } else return m_millers.front().id();
}
bool GCodeWriter::tool_is_extruder() const {
    return m_tool && m_tool->id() < first_mill();
}
const Tool* GCodeWriter::get_tool(uint16_t id) const{
    for (const Extruder& e : m_extruders)
        if (id == e.id())
            return &e;
    for (const Tool& e : m_millers)
        if (id == e.id())
            return &e;
    return nullptr;
}

void GCodeWriter::set_extruders(std::vector<uint16_t> extruder_ids)
{
    std::sort(extruder_ids.begin(), extruder_ids.end());
    m_extruders.clear();
    m_extruders.reserve(extruder_ids.size());
    for (uint16_t extruder_id : extruder_ids)
        m_extruders.emplace_back(Extruder(extruder_id, this->config));

    /*  we enable support for multiple extruder if any extruder greater than 0 is used
        (even if prints only uses that one) since we need to output Tx commands
        first extruder has index 0 */
    if(!extruder_ids.empty() && !this->multiple_extruders)
        this->multiple_extruders = (*std::max_element(extruder_ids.begin(), extruder_ids.end())) > 0;
}

void GCodeWriter::set_mills(std::vector<uint16_t> mill_ids)
{
    std::sort(mill_ids.begin(), mill_ids.end());
    m_millers.clear();
    m_millers.reserve(mill_ids.size());
    for (uint16_t mill_id : mill_ids) {
        m_millers.emplace_back(Mill(mill_id, this->config));
    }

    /*  we enable support for multiple extruder */
    this->multiple_extruders = this->multiple_extruders || !mill_ids.empty();
}

std::string GCodeWriter::preamble()
{
    std::ostringstream gcode;
    
    if (FLAVOR_IS_NOT(gcfMakerWare)) {
        gcode << "G21 ; set units to millimeters\n";
        gcode << "G90 ; use absolute coordinates\n";
    }
    if (FLAVOR_IS(gcfSprinter) ||
        FLAVOR_IS(gcfRepRap) ||
        FLAVOR_IS(gcfMarlinLegacy) ||
        FLAVOR_IS(gcfMarlinFirmware) ||
        FLAVOR_IS(gcfKlipper) ||
        FLAVOR_IS(gcfTeacup) ||
        FLAVOR_IS(gcfRepetier) ||
        FLAVOR_IS(gcfSmoothie)
    )
    {
        if (this->config.use_relative_e_distances) {
            gcode << "M83 ; use relative distances for extrusion\n";
        } else {
            gcode << "M82 ; use absolute distances for extrusion\n";
        }
        gcode << this->reset_e(true);
    }
    
    return gcode.str();
}

std::string GCodeWriter::postamble() const
{
    std::ostringstream gcode;
    if (FLAVOR_IS(gcfMachinekit))
          gcode << "M2 ; end of program\n";
    return gcode.str();
}

std::string GCodeWriter::set_temperature(const int16_t temperature, bool wait, int tool)
{
    //use m_tool if tool isn't set
    if (tool < 0 && m_tool != nullptr)
        tool = m_tool->id();

    //add offset
    int16_t temp_w_offset = temperature;
    temp_w_offset += int16_t(get_tool(tool)->temp_offset());
    temp_w_offset = std::max(int16_t(0), std::min(int16_t(2000), temp_w_offset));

    // temp_w_offset has an effective minimum value of 0, so this cast is safe.
    if (m_last_temperature_with_offset == temp_w_offset && !wait)
        return "";
    if (wait && (FLAVOR_IS(gcfMakerWare) || FLAVOR_IS(gcfSailfish)))
        return {};
    
    std::string_view code, comment;
    if (wait && FLAVOR_IS_NOT(gcfTeacup) && FLAVOR_IS_NOT(gcfRepRap)) {
        code = "M109"sv;
        comment = "set temperature and wait for it to be reached"sv;
    } else {
        if (FLAVOR_IS(gcfRepRap)) { // M104 is deprecated on RepRapFirmware
            code = "G10"sv;
        } else {
            code = "M104"sv;
        }
        comment = "set temperature"sv;
    }
    
    std::ostringstream gcode;
    gcode << code << " ";
    if (FLAVOR_IS(gcfMach3) || FLAVOR_IS(gcfMachinekit)) {
        gcode << "P";
    } else if (FLAVOR_IS(gcfRepRap)) {
        gcode << "P" << tool << " S";
    } else if (wait && (FLAVOR_IS(gcfMarlinFirmware) || FLAVOR_IS(gcfMarlinLegacy)) && temp_w_offset < m_last_temperature_with_offset) {
        gcode << "R"; //marlin doesn't wait with S if it's a cooling change, it needs a R
    } else {
        gcode << "S";
    }
    gcode << temp_w_offset;
    bool multiple_tools = this->multiple_extruders && ! m_single_extruder_multi_material;
    if (tool != -1 && (multiple_tools || FLAVOR_IS(gcfMakerWare) || FLAVOR_IS(gcfSailfish)) && FLAVOR_IS_NOT(gcfRepRap)) {
        gcode << " T" << tool;
    }
    gcode << " ; " << comment << "\n";
    
    if ((FLAVOR_IS(gcfTeacup) || FLAVOR_IS(gcfRepRap)) && wait)
        gcode << "M116 ; wait for temperature to be reached\n";
    
    m_last_temperature = temperature;
    m_last_temperature_with_offset = temp_w_offset;

    return gcode.str();
}

std::string GCodeWriter::set_bed_temperature(uint32_t temperature, bool wait)
{
    if (temperature == m_last_bed_temperature && (! wait || m_last_bed_temperature_reached))
        return {};

    m_last_bed_temperature = temperature;
    m_last_bed_temperature_reached = wait;

    std::string_view code, comment;
    if (wait && FLAVOR_IS_NOT(gcfTeacup)) {
        if (FLAVOR_IS(gcfMakerWare) || FLAVOR_IS(gcfSailfish)) {
            code = "M109"sv;
        } else {
            code = "M190"sv;
        }
        comment = "set bed temperature and wait for it to be reached"sv;
    } else {
        code = "M140"sv;
        comment = "set bed temperature"sv;
    }
    
    std::ostringstream gcode;
    gcode << code << " ";
    if (FLAVOR_IS(gcfMach3) || FLAVOR_IS(gcfMachinekit)) {
        gcode << "P";
    } else {
        gcode << "S";
    }
    gcode << temperature << " ; " << comment << "\n";
    
    if (FLAVOR_IS(gcfTeacup) && wait)
        gcode << "M116 ; wait for bed temperature to be reached\n";
    
    return gcode.str();
}

std::string GCodeWriter::set_chamber_temperature(uint32_t temperature, bool wait)
{
    if (temperature == m_last_chamber_temperature && !wait)
        return std::string();

    if (FLAVOR_IS(gcfMarlinFirmware) || FLAVOR_IS(gcfRepRap) || FLAVOR_IS(gcfMachinekit)) {
        // ok
    } else {
        return std::string();
    }

    m_last_chamber_temperature = temperature;

    std::string code, comment;
    if (wait) {
        code = "M191";
        comment = "set chamber temperature and wait for it to be reached";
    } else {
        code = "M141";
        comment = "set chamber temperature";
    }
    
    std::ostringstream gcode;
    gcode << code << " " << "S";
    gcode << temperature << " ; " << comment << "\n";
    
    return gcode.str();
}

void GCodeWriter::set_acceleration(uint32_t acceleration)
{
    if (acceleration == 0 || acceleration == m_current_acceleration)
        return;

    m_current_acceleration = acceleration;
}

void GCodeWriter::set_travel_acceleration(uint32_t acceleration)
{
    //only gcfMarlinFirmware and gcfRepRap can use the travel accel
    // so for the other, override the current accel
    if (FLAVOR_IS_NOT(gcfMarlinFirmware) && FLAVOR_IS_NOT(gcfRepRap))
        set_acceleration(acceleration);

    if (acceleration == m_current_travel_acceleration)
        return;

    m_current_travel_acceleration = acceleration;
}

uint32_t GCodeWriter::get_acceleration() const
{
    return m_current_acceleration;
}

std::string GCodeWriter::write_acceleration(){
    bool need_write_travel_accel = (FLAVOR_IS(gcfMarlinFirmware) || FLAVOR_IS(gcfRepRap)) &&
                                   m_current_travel_acceleration != m_last_travel_acceleration;
    bool need_write_main_accel = m_current_acceleration != m_last_acceleration &&
                                 m_current_acceleration != 0;
    if (!need_write_main_accel && !need_write_travel_accel)
        return "";

    m_last_acceleration = m_current_acceleration;
    m_last_travel_acceleration = m_current_travel_acceleration;

    std::ostringstream gcode;
	//try to set only printing acceleration, travel should be untouched if possible
    if (FLAVOR_IS(gcfRepetier)) {
        // M201: Set max printing acceleration
        if (m_current_acceleration > 0)
            gcode << "M201 X" << m_current_acceleration << " Y" << m_current_acceleration;
    } else if(FLAVOR_IS(gcfSprinter)){
        // M204: Set printing acceleration
        // This is new MarlinFirmware with separated print/retraction/travel acceleration.
        // Use M204 P, we don't want to override travel acc by M204 S (which is deprecated anyway).
        if (m_current_acceleration > 0)
            gcode << "M204 P" << m_current_acceleration;
    } else if (FLAVOR_IS(gcfMarlinFirmware) || FLAVOR_IS(gcfRepRap)) {
        // M204: Set printing & travel acceleration
        if (m_current_acceleration > 0)
            gcode << "M204 P" << m_current_acceleration << " T" << (m_current_travel_acceleration > 0 ? m_current_travel_acceleration : m_current_acceleration);
        else if(m_current_travel_acceleration > 0)
            gcode << "M204 T" << m_current_travel_acceleration;
    } else { // gcfMarlinLegacy
        // M204: Set default acceleration
        if (m_current_acceleration > 0)
            gcode << "M204 S" << m_current_acceleration;
    }
    //if at least something, add comment and line return
    if (gcode.tellp() != std::streampos(0)) {
        if (this->config.gcode_comments)
            gcode << " ; adjust acceleration";
        gcode << "\n";
        return gcode.str();
    }
    assert(gcode.str().empty());
    return "";
}

std::string GCodeWriter::reset_e(bool force)
{
    this->m_de_left = 0;

    if (FLAVOR_IS(gcfMach3)
        || FLAVOR_IS(gcfMakerWare)
        || FLAVOR_IS(gcfSailfish))
        return "";
    
    if (m_tool != nullptr) {
        // if it was already at 0 (not modified with reset) and we don't force a G92 -> return
        if (!m_tool->reset_E() && !force)
            return "";
    }

    if (! m_extrusion_axis.empty() && ! this->config.use_relative_e_distances) {
        std::ostringstream gcode;
        gcode << "G92 " << m_extrusion_axis << "0";
        if (this->config.gcode_comments) gcode << " ; reset extrusion distance";
        gcode << "\n";
        return gcode.str();
    } else {
        return "";
    }
}

std::string GCodeWriter::update_progress(uint32_t num, uint32_t tot, bool allow_100) const
{
    if (FLAVOR_IS_NOT(gcfMakerWare) && FLAVOR_IS_NOT(gcfSailfish))
        return {};
    
    uint8_t percent = (uint32_t)floor(100.0 * num / tot + 0.5);
    if (!allow_100) percent = std::min(percent, (uint8_t)99);
    
    std::ostringstream gcode;
    gcode << "M73 P" << int(percent);
    if (this->config.gcode_comments) gcode << " ; update progress";
    gcode << "\n";
    return gcode.str();
}

std::string GCodeWriter::toolchange_prefix() const
{
    return FLAVOR_IS(gcfMakerWare) ? "M135 T" :
           FLAVOR_IS(gcfSailfish) ? "M108 T" :
           FLAVOR_IS(gcfKlipper) ? "ACTIVATE_EXTRUDER EXTRUDER=" :
           "T";
}

std::string GCodeWriter::toolchange(uint16_t tool_id)
{
    // set the new extruder
	/*auto it_extruder = Slic3r::lower_bound_by_predicate(m_extruders.begin(), m_extruders.end(), [tool_id](const Extruder &e) { return e.id() < tool_id; });
    assert(it_extruder != m_extruders.end() && it_extruder->id() == extruder_id);*/
    //less optimized but it's easier to modify and it's not needed, as it's not called often.
    bool found = false;
    for (Extruder& extruder : m_extruders) {
        if (tool_id == extruder.id()) {
            m_tool = &extruder;
            found = true;
            break;
        }
    }
    if (!found) {
        for (Tool& mill : m_millers) {
            if (tool_id == mill.id()) {
                m_tool = &mill;
                found = true;
                break;
            }
        }
    }

    // return the toolchange command
    // if we are running a single-extruder setup, just set the extruder and return nothing
    std::ostringstream gcode;
    if (this->multiple_extruders) {
        if (FLAVOR_IS(gcfKlipper)) {
            //check if we can use the tool_name field or not
            if (tool_id > 0 && tool_id < this->config.tool_name.size() && !this->config.tool_name.get_at(tool_id).empty()
                // NOTE: this will probably break if there's more than 10 tools, as it's relying on the
                // ASCII character table.
                && this->config.tool_name.get_at(tool_id)[0] != static_cast<char>(('0' + tool_id))) {
                gcode << this->toolchange_prefix() << this->config.tool_name.get_at(tool_id);
            } else {
                gcode << this->toolchange_prefix() << "extruder";
                if (tool_id > 0)
                    gcode << tool_id;
            }
        } else {
            gcode << this->toolchange_prefix() << tool_id;
        }
        if (this->config.gcode_comments)
            gcode << " ; change extruder";
        gcode << "\n";
        gcode << this->reset_e(true);
    }
    return gcode.str();
}

// !! be careful, prusa pass the F here (mm/min) as parameter, not the speed (mm/s)
std::string GCodeWriter::set_speed(const double speed, const std::string_view comment, const std::string_view cooling_marker)
{
    const double F = speed * 60;
    m_current_speed = speed;
    assert(F > 0.);
    assert(F < 10000000.);
    GCodeG1Formatter w(this->get_default_gcode_formatter());
    w.emit_f(F);
    w.emit_comment(this->config.gcode_comments, comment);
    w.emit_string(cooling_marker);
    return w.string();
}

double GCodeWriter::get_speed() const
{
    return m_current_speed;
}

std::string GCodeWriter::travel_to_xy(const Vec2d &point, const double speed, const std::string_view comment)
{
    assert(std::abs(point.x()) < 120000.);
    assert(std::abs(point.y()) < 120000.);

    double travel_speed = this->config.travel_speed.value;
    if ((speed > 0) & (speed < travel_speed))
        travel_speed = speed;

    m_pos.x() = point.x();
    m_pos.y() = point.y();
    GCodeG1Formatter w(this->get_default_gcode_formatter());
    if (!w.emit_xy(point, m_pos_str_x, m_pos_str_y)) {
        //if point too close to the other, then do not write it, it's useless.
        return "";
    }
    w.emit_f(travel_speed * 60);
    w.emit_comment(this->config.gcode_comments, comment);
    return write_acceleration() + w.string();
}

std::string GCodeWriter::travel_arc_to_xy(const Vec2d& point, const Vec2d& center_offset, const bool is_ccw, const double speed, const std::string_view comment)
{
    assert(std::abs(point.x()) < 120000.);
    assert(std::abs(point.y()) < 120000.);
    assert(std::abs(center_offset.x()) < 12000000.);
    assert(std::abs(center_offset.y()) < 12000000.);
    assert(std::abs(center_offset.x()) >= EPSILON * 10 || std::abs(center_offset.y()) >= EPSILON * 10);
    

    double travel_speed = this->config.travel_speed.value;
    if ((speed > 0) & (speed < travel_speed))
        travel_speed = speed;

    m_pos.x()             = point.x();
    m_pos.y()             = point.y();

    GCodeG2G3Formatter w(this->config.gcode_precision_xyz.value, this->config.gcode_precision_e.value, is_ccw);
    w.emit_xy(point);
    w.emit_ij(center_offset);
    w.emit_f(travel_speed * 60);
    w.emit_comment(this->config.gcode_comments, comment);
    return write_acceleration() + w.string();
}

std::string GCodeWriter::travel_to_xyz(const Vec3d &point, const double speed, const std::string_view comment)
{
    assert(std::abs(point.x()) < 120000.);
    assert(std::abs(point.y()) < 120000.);
 
    /*  If target Z is lower than current Z but higher than nominal Z we
        don't perform the Z move but we only move in the XY plane and
        adjust the nominal Z by reducing the lift amount that will be 
        used for unlift. */
    if (!this->will_move_z(point.z())) {
        double nominal_z = m_pos.z() - m_lifted;
        m_lifted -= (point.z() - nominal_z);
        // In case that retract_lift == layer_height we could end up with almost zero in_m_lifted
        // and a retract could be skipped (https://github.com/prusa3d/PrusaSlicer/issues/2154
        if (std::abs(m_lifted) < EPSILON)
            m_lifted = 0.;
        return this->travel_to_xy(to_2d(point), speed, comment);
    }
    

    //compute the speed
    double travel_speed = this->config.travel_speed.value;
    assert(travel_speed > 0);
    // check if travel_speed_z won't limit the speed
    if (this->config.travel_speed_z.value > 0) {
        const double dist_xyz        = (m_pos - point).norm();
        double       duration_xyz = dist_xyz / travel_speed;
        double       min_duration_z  = std::abs(m_pos.z() - point.z()) / this->config.travel_speed_z.value;
        if (min_duration_z > duration_xyz) {
            // too fast for travel_speed_z, reduce speed to be in line with expectation.
            travel_speed = travel_speed * duration_xyz / min_duration_z;
        }
    }
    // check if speed from caller won't limit the speed
    if ((speed > 0) & (speed < travel_speed))
        travel_speed = speed;

    /*  In all the other cases, we perform an actual XYZ move and cancel
        the lift. */
    m_lifted = 0;

    m_pos = point;
    
    GCodeG1Formatter w(this->get_default_gcode_formatter());
    if (!w.emit_xy(point.head<2>(), m_pos_str_x, m_pos_str_y)) {
        //if point too close to the other, then whatever, as long as the z is different.
        // //if point too close to the other, then do not write it, it's useless.
        // w = GCodeG1Formatter(this->get_default_gcode_formatter());
    }
    if (config.z_step > SCALING_FACTOR)
        w.emit_axis('Z', point.z(), 6);
    else
        w.emit_z(point.z());
    w.emit_f(travel_speed * 60);
    w.emit_comment(this->config.gcode_comments, comment);
    return write_acceleration() + w.string();
}

std::string GCodeWriter::travel_to_z(double z, const std::string_view comment)
{
    /*  If target Z is lower than current Z but higher than nominal Z
        we don't perform the move but we only adjust the nominal Z by
        reducing the lift amount that will be used for unlift. */
    // note that if we move but it's lower and we are lifted, we can wait a bit for unlifting, to avoid possible dance on layer change.
    if (!this->will_move_z(z) || z < m_pos.z() && m_lifted > EPSILON) {
        double nominal_z = m_pos.z() - m_lifted;
        m_lifted -= (z - nominal_z);
        if (std::abs(m_lifted) < EPSILON)
            m_lifted = 0.;
        return "";
    }
    /*  In all the other cases, we perform an actual Z move and cancel
        the lift. */
    m_lifted = 0;
    return this->get_travel_to_z_gcode(z, comment);
}


std::string GCodeWriter::get_travel_to_z_gcode(double z, const std::string_view comment)
{
    m_pos.z() = z;
    double speed = this->config.travel_speed_z.value;
    if (speed == 0.)
        speed = this->config.travel_speed.value;

    GCodeG1Formatter w(this->get_default_gcode_formatter());
    if (config.z_step > SCALING_FACTOR)
        w.emit_axis('Z', z, 6);
    else
        w.emit_z(z);
    w.emit_f(speed * 60.0);
    w.emit_comment(this->config.gcode_comments, comment);
    return write_acceleration() + w.string();
}

bool GCodeWriter::will_move_z(double z) const
{
    /* If target Z is lower than current Z but higher than nominal Z
        we don't perform an actual Z move. */
    if (m_lifted > 0) {
        double nominal_z = m_pos.z() - m_lifted;
        if (z >= nominal_z - EPSILON && z <= m_pos.z() + EPSILON)
            return false;
    }
    return true;
}

std::string GCodeWriter::extrude_to_xy(const Vec2d &point, double dE, const std::string_view comment)
{
    assert(dE == dE);
    assert(m_pos.x() != point.x() || m_pos.y() != point.y());

    m_pos.head<2>() = point.head<2>();
     auto [/*double*/ delta_e, /*double*/ e_to_write]  = this->m_tool->extrude(dE + this->m_de_left);
    bool is_extrude  = std::abs(delta_e) > 0.00000001;

    GCodeG1Formatter w(this->get_default_gcode_formatter());
    if (!w.emit_xy(point, m_pos_str_x, m_pos_str_y)) {
        //if point too close to the other, then do not write it, it's useless.
        this->m_de_left += dE;
        return "";
    }
    this->m_de_left += dE - delta_e;
    if (is_extrude) {
        double delta = w.emit_e(m_extrusion_axis, e_to_write);
        this->m_de_left += delta;
    }
    w.emit_comment(this->config.gcode_comments, comment);
    return write_acceleration() + w.string();
}

//BBS: generate G2 or G3 extrude which moves by arc
//point is end point which means X and Y axis
//center_offset is I and J axis
std::string GCodeWriter::extrude_arc_to_xy(const Vec2d& point, const Vec2d& center_offset, double dE, const bool is_ccw, const std::string_view comment)
{
    assert(std::abs(point.x()) < 120000.);
    assert(std::abs(point.y()) < 120000.);
    assert(std::abs(center_offset.x()) < 12000000.);
    assert(std::abs(center_offset.y()) < 12000000.);
    assert(std::abs(center_offset.x()) >= EPSILON * 10 || std::abs(center_offset.y()) >= EPSILON * 10);

    m_pos.x()             = point.x();
    m_pos.y()             = point.y();
     auto [/*double*/ delta_e, /*double*/ e_to_write]  = this->m_tool->extrude(dE + this->m_de_left);
    bool is_extrude  = std::abs(delta_e) > 0.00000001;

    GCodeG2G3Formatter w(this->config.gcode_precision_xyz.value, this->config.gcode_precision_e.value, is_ccw);
    bool has_x_y = w.emit_xy(point, m_pos_str_x, m_pos_str_y);
    assert(has_x_y);
    w.emit_ij(center_offset);
    this->m_de_left += dE - delta_e;
    if (is_extrude) {
        double delta = w.emit_e(m_extrusion_axis, e_to_write);
        this->m_de_left += delta;
    }
    w.emit_comment(this->config.gcode_comments, comment);
    return write_acceleration() + w.string();
}

std::string GCodeWriter::extrude_to_xyz(const Vec3d &point, double dE, const std::string_view comment)
{
    assert(std::abs(point.x()) < 120000.);
    assert(std::abs(point.y()) < 120000.);
    assert(std::abs(point.z()) < 120000.);
    assert(dE == dE);
    m_pos.x()                = point.x();
    m_pos.y()                = point.y();
    m_lifted                 = 0;
     auto [/*double*/ delta_e, /*double*/ e_to_write]  = this->m_tool->extrude(dE + this->m_de_left);
    bool is_extrude  = std::abs(delta_e) > 0.00000001;

    GCodeG1Formatter w(this->get_default_gcode_formatter());
    w.emit_xy(Vec2d(point.x(), point.y()), m_pos_str_x, m_pos_str_y);
    w.emit_z(point.z() + m_pos.z());
    this->m_de_left += dE - delta_e;
    if (is_extrude) {
        double delta = w.emit_e(m_extrusion_axis, e_to_write);
        assert(delta == 0 ); // shoulde be already taken into account by m_tool->extrude
        this->m_de_left += delta;
    }
    w.emit_comment(this->config.gcode_comments, comment);
    return write_acceleration() + w.string();
}

std::string GCodeWriter::retract(bool before_wipe)
{
    double factor = before_wipe ? m_tool->retract_before_wipe() : 1.;
    assert((factor >= 0. || before_wipe) && factor <= 1. + EPSILON);
    // if before_wipe but no retract_before_wipe, then no retract
    if (factor == 0)
        return "";
    //check for override
    if (config_region && config_region->print_retract_length >= 0) {
        return this->_retract(
            factor * config_region->print_retract_length,
            factor * m_tool->retract_restart_extra(),
            std::nullopt,
            "retract"
        );
    }
    return this->_retract(
        factor * m_tool->retract_length(),
        factor * m_tool->retract_restart_extra(),
        std::nullopt,
        "retract"
    );
}

std::string GCodeWriter::retract_for_toolchange(bool before_wipe)
{
    double factor = before_wipe ? m_tool->retract_before_wipe() : 1.;
    assert(factor >= 0. && factor <= 1. + EPSILON);
    return this->_retract(
        factor * m_tool->retract_length_toolchange(),
        std::nullopt,
        factor * m_tool->retract_restart_extra_toolchange(),
        "retract for toolchange"
    );
}

std::string GCodeWriter::_retract(double length, std::optional<double> restart_extra, std::optional<double> restart_extra_toolchange, const std::string_view comment)
{
    assert(std::abs(length) < 10000.0);
    assert(!restart_extra || std::abs(*restart_extra) < 10000.0);
    assert(!restart_extra_toolchange || std::abs(*restart_extra_toolchange) < 10000.0);

    std::string gcode;
    
    /*  If firmware retraction is enabled, we use a fake value of 1
        since we ignore the actual configured retract_length which 
        might be 0, in which case the retraction logic gets skipped. */
    if (this->config.use_firmware_retraction)
        length = 1;
    
    // If we use volumetric E values we turn lengths into volumes */
    if (this->config.use_volumetric_e) {
        double d = m_tool->filament_diameter();
        double area = d * d * PI/4;
        assert(length * area < std::numeric_limits<int32_t>::max());
        assert(length * area > 0);
        assert(!restart_extra || *restart_extra * area < std::numeric_limits<int32_t>::max());
        assert(!restart_extra || *restart_extra * area > -std::numeric_limits<int32_t>::max());
        assert(!restart_extra_toolchange || *restart_extra_toolchange * area < std::numeric_limits<int32_t>::max());
        assert(!restart_extra_toolchange || *restart_extra_toolchange * area > -std::numeric_limits<int32_t>::max());
        length = length * area;
        if(restart_extra)
            restart_extra = *restart_extra * area;
        if(restart_extra_toolchange)
            restart_extra_toolchange = *restart_extra_toolchange * area;
    }
    
    auto [dE, emit_E] = m_tool->retract(length, restart_extra, restart_extra_toolchange);
    assert(dE >= 0);
    assert(dE < 10000000);
    if (dE != 0) {
        if (this->config.use_firmware_retraction) {
            if (FLAVOR_IS(gcfMachinekit))
                gcode += "G22 ; retract\n";
            else
                gcode += "G10 ; retract\n";
        } else if (!m_extrusion_axis.empty()) {
            GCodeG1Formatter w(this->get_default_gcode_formatter());
            w.emit_e(m_extrusion_axis, emit_E);
            w.emit_f(m_tool->retract_speed() * 60.);
            w.emit_comment(this->config.gcode_comments, comment);
            gcode += w.string();
        }
    }

    // extrusion pressure is reset
    m_de_left = 0;
    
    if (FLAVOR_IS(gcfMakerWare))
        gcode += "M103 ; extruder off\n";
    
    return gcode;
}

std::string GCodeWriter::unretract()
{
    //assert(m_de_left == 0); //TOSO 2.7 test it
    std::string gcode;
    
    if (FLAVOR_IS(gcfMakerWare))
        gcode += "M101 ; extruder on\n";
    
    auto [dE, emit_E] = m_tool->unretract();
    assert(dE >= 0);
    assert(dE < 10000000);
    if (dE != 0) {
        if (this->config.use_firmware_retraction) {
            gcode += (FLAVOR_IS(gcfMachinekit) ? "G23 ; unretract\n" : "G11 ; unretract\n");
            gcode += this->reset_e();
        } else if (! m_extrusion_axis.empty()) {
            // use G1 instead of G0 because G0 will blend the restart with the previous travel move
            GCodeG1Formatter w(this->get_default_gcode_formatter());
            w.emit_e(m_extrusion_axis, emit_E);
            w.emit_f(m_tool->deretract_speed() * 60.);
            w.emit_comment(this->config.gcode_comments, " ; unretract");
            gcode += w.string();
        }
    }
    
    return gcode;
}

void GCodeWriter::update_position(const Vec3d &new_pos)
{
    // move z ==> update lift
    m_lifted = m_lifted + new_pos.z() - m_pos.z();
    //update position
    m_pos = new_pos;
}

/*  If this method is called more than once before calling unlift(),
    it will not perform subsequent lifts, even if Z was raised manually
    (i.e. with travel_to_z()) and thus _lifted was reduced. */
std::string GCodeWriter::lift(int layer_id)
{
    // check whether the above/below conditions are met
    double target_lift = 0;
    if(this->tool_is_extruder()){
        bool can_lift = this->config.retract_lift_first_layer.get_at(m_tool->id()) && layer_id == 0;
        if (!can_lift) {
            //these two should be in the Tool class methods....
            double above = this->config.retract_lift_above.get_at(m_tool->id());
            double below = this->config.retract_lift_below.get_at(m_tool->id());
            can_lift = (m_pos.z() >= above - EPSILON && (below == 0 || m_pos.z() <= below + EPSILON));
        }
        if(can_lift)
            target_lift = m_tool->retract_lift();
    } else {
        target_lift = m_tool->retract_lift();
    }

    // use the override if set
    if (target_lift > 0 && config_region && config_region->print_retract_lift.value >= 0) {
        target_lift = config_region->print_retract_lift.value;
    }

    // one-time extra lift (often for dangerous travels)
    if (this->m_extra_lift > 0) {
        target_lift += this->m_extra_lift;
        this->m_extra_lift = 0;
    }

    // compare against epsilon because travel_to_z() does math on it
    // and subtracting layer_height from retract_lift might not give
    // exactly zero
    if (std::abs(m_lifted) < target_lift - EPSILON && target_lift > 0) {
        std::string str =  this->get_travel_to_z_gcode(m_pos.z() + target_lift - m_lifted, "lift Z");
        m_lifted = target_lift;
        return str;
    }
    return "";
}

std::string GCodeWriter::unlift()
{
    std::string gcode;
    if (m_lifted > 0) {
        gcode += this->get_travel_to_z_gcode(m_pos.z() - m_lifted, "restore layer Z");
    }
    m_lifted = 0;
    return gcode;
}

std::string GCodeWriter::set_fan(const GCodeFlavor gcode_flavor, bool gcode_comments, uint8_t speed, uint8_t tool_fan_offset, bool is_fan_percentage, const std::string_view comment/*=""*/)
{
/*
    std::ostringstream gcode;
    if (speed == 0) {
        switch (gcode_flavor) {
        case gcfTeacup:
            gcode << "M106 S0"; break;
        case gcfMakerWare:
        case gcfSailfish:
            gcode << "M127";    break;
        default:
            gcode << "M107";    break;
        }
        if (gcode_comments)
            gcode << " ; disable fan";
        gcode << "\n";
    } else {
        switch (gcode_flavor) {
        case gcfMakerWare:
        case gcfSailfish:
            gcode << "M126";    break;
        case gcfMach3:
        case gcfMachinekit:
            gcode << "M106 P" << 255.0 * speed / 100.0; break;
        default:
            gcode << "M106 S" << 255.0 * speed / 100.0; break;
        }
        if (gcode_comments) 
            gcode << " ; enable fan";
        gcode << "\n";
    }
    return gcode.str();*/

    std::ostringstream gcode;

    //add fan_offset
    int16_t fan_speed = int8_t(std::min(uint8_t(100), speed));
    fan_speed += tool_fan_offset;
    fan_speed = std::max(int16_t(0), std::min(int16_t(100), fan_speed));
    const double fan_baseline = (is_fan_percentage ? 100.0 : 255.0);

    // write it
    if (fan_speed == 0) {
        if ((gcfTeacup == gcode_flavor || gcfRepRap == gcode_flavor)) {
            gcode << "M106 S0";
        } else if ((gcfMakerWare == gcode_flavor) || (gcfSailfish == gcode_flavor)) {
            gcode << "M127";
        } else {
            gcode << "M107";
        }
        if (gcode_comments)
            gcode << " ; " << (comment.empty() ? "disable fan" : comment);
        gcode << "\n";
    } else {
        if ((gcfMakerWare == gcode_flavor) || (gcfSailfish == gcode_flavor)) {
            gcode << "M126 T";
        } else {
            gcode << "M106 ";
            if ((gcfMach3 == gcode_flavor) || (gcfMachinekit == gcode_flavor)) {
                gcode << "P";
            } else {
                gcode << "S";
            }
            gcode << (fan_baseline * (fan_speed / 100.0));
        }
        if (gcode_comments)
            gcode << " ; " << (comment.empty() ? "enable fan" : comment);
        gcode << "\n";
    }
    return gcode.str();
}

std::string GCodeWriter::set_fan(const uint8_t speed, uint16_t default_tool)
{
    const Tool *tool = m_tool == nullptr ? get_tool(default_tool) : m_tool;
    m_last_fan_speed = speed;
    return GCodeWriter::set_fan(this->config.gcode_flavor.value, this->config.gcode_comments.value, speed, tool ? tool->fan_offset() : 0, this->config.fan_percentage.value);
}

} // namespace Slic3r
