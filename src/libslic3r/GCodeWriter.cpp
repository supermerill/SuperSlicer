#include "GCodeWriter.hpp"
#include "CustomGCode.hpp"
#include "LocalesUtils.hpp"

#include <boost/lexical_cast.hpp>

#include <algorithm>
#include <assert.h>
#include <iomanip>
#include <iostream>
#include <map>

#ifdef DONT_USE_CHARCONV
    #include <boost/spirit/include/karma.hpp>
#else
#ifdef __APPLE__
    #include <boost/spirit/include/karma.hpp>
#endif
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
namespace Slic3r {


std::string GCodeWriter::PausePrintCode = "M601";

void GCodeWriter::apply_print_config(const PrintConfig &print_config)
{
    this->config.apply(print_config, true);
    m_extrusion_axis = get_extrusion_axis(this->config);
    m_single_extruder_multi_material = print_config.single_extruder_multi_material.value;
}

void GCodeWriter::apply_print_region_config(const PrintRegionConfig& print_region_config)
{
    config_region = &print_region_config;
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
    return m_tool->id() < first_mill();
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
        m_extruders.emplace_back(Extruder(extruder_id, &this->config));

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
        m_millers.emplace_back(Mill(mill_id, &this->config));
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
        FLAVOR_IS(gcfLerdge) ||
        FLAVOR_IS(gcfTeacup) ||
        FLAVOR_IS(gcfRepetier) ||
        FLAVOR_IS(gcfSmoothie) ||
        FLAVOR_IS(gcfKlipper))
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
        return "";
    
    std::string code, comment;
    if (wait && FLAVOR_IS_NOT(gcfTeacup) && FLAVOR_IS_NOT(gcfRepRap)) {
        code = "M109";
        comment = "set temperature and wait for it to be reached";
    } else {
        if (FLAVOR_IS(gcfRepRap)) { // M104 is deprecated on RepRapFirmware
            code = "G10";
        } else {
            code = "M104";
        }
        comment = "set temperature";
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
        return std::string();

    m_last_bed_temperature = temperature;
    m_last_bed_temperature_reached = wait;

    std::string code, comment;
    if (wait && FLAVOR_IS_NOT(gcfTeacup)) {
        if (FLAVOR_IS(gcfMakerWare) || FLAVOR_IS(gcfSailfish)) {
            code = "M109";
        } else {
            code = "M190";
        }
        comment = "set bed temperature and wait for it to be reached";
    } else {
        code = "M140";
        comment = "set bed temperature";
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
    if (m_current_acceleration == m_last_acceleration || m_current_acceleration == 0)
        return "";

    m_last_acceleration = m_current_acceleration;

    std::ostringstream gcode;
	//try to set only printing acceleration, travel should be untouched if possible
    if (FLAVOR_IS(gcfRepetier)) {
        // M201: Set max printing acceleration
        gcode << "M201 X" << m_current_acceleration << " Y" << m_current_acceleration;
    } else if(FLAVOR_IS(gcfLerdge) || FLAVOR_IS(gcfSprinter)){
        // M204: Set printing acceleration
        // This is new MarlinFirmware with separated print/retraction/travel acceleration.
        // Use M204 P, we don't want to override travel acc by M204 S (which is deprecated anyway).
        gcode << "M204 P" << m_current_acceleration;
    } else if (FLAVOR_IS(gcfMarlinFirmware) || FLAVOR_IS(gcfRepRap)) {
        // M204: Set printing & travel acceleration
        gcode << "M204 P" << m_current_acceleration << " T" << (m_current_travel_acceleration > 0 ? m_current_travel_acceleration : m_current_acceleration);
    } else { // gcfMarlinLegacy
        // M204: Set default acceleration
        gcode << "M204 S" << m_current_acceleration;
    }
    if (this->config.gcode_comments) gcode << " ; adjust acceleration";
    gcode << "\n";
    
    return gcode.str();
}

std::string GCodeWriter::reset_e(bool force)
{
    if (FLAVOR_IS(gcfMach3)
        || FLAVOR_IS(gcfMakerWare)
        || FLAVOR_IS(gcfSailfish))
        return "";
    
    if (m_tool != nullptr) {
        if (m_tool->E() == 0. && ! force)
            return "";
        m_tool->reset_E();
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
        return "";
    
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
            if (tool_id > 0 && tool_id < this->config.tool_name.values.size() && !this->config.tool_name.values[tool_id].empty()
                // NOTE: this will probably break if there's more than 10 tools, as it's relying on the
                // ASCII character table.
                && this->config.tool_name.values[tool_id][0] != static_cast<char>(('0' + tool_id))) {
                gcode << this->toolchange_prefix() << this->config.tool_name.values[tool_id];
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

std::string GCodeWriter::set_speed(const double speed, const std::string &comment, const std::string &cooling_marker)
{
    const double F = speed * 60;
    m_current_speed = speed;
    assert(F > 0.);
    assert(F < 100000.);
//    GCodeG1Formatter w;
//    w.emit_f(F);
//    w.emit_comment(this->config.gcode_comments, comment);
//    w.emit_string(cooling_marker);
//    return w.string();
    std::ostringstream gcode;
    gcode << "G1 F" << F_NUM(F);
    COMMENT(comment);
    gcode << cooling_marker;
    gcode << "\n";
    return gcode.str();
}

double GCodeWriter::get_speed() const
{
    return m_current_speed;
}

std::string GCodeWriter::travel_to_xy(const Vec2d &point, const double speed, const std::string &comment)
{
    std::ostringstream gcode;
    gcode << write_acceleration();

    double travel_speed = this->config.travel_speed.value;
    if ((speed > 0) & (speed < travel_speed))
        travel_speed = speed;

    m_pos.x() = point.x();
    m_pos.y() = point.y();
    
//    GCodeG1Formatter w;
//    w.emit_xy(point);
//    w.emit_f(this->config.travel_speed.value * 60.0);
//    w.emit_comment(this->config.gcode_comments, comment);
//    return w.string();
    gcode << "G1 X" << XYZ_NUM(point.x())
          <<   " Y" << XYZ_NUM(point.y())
          <<   " F" << F_NUM(travel_speed * 60);
    COMMENT(comment);
    gcode << "\n";
    return gcode.str();
}

std::string GCodeWriter::travel_to_xyz(const Vec3d &point, const double speed, const std::string &comment)
{
    // FIXME: This function was not being used when travel_speed_z was separated (bd6badf).
    // Calculation of feedrate was not updated accordingly. If you want to use
    // this function, fix it first.
    std::terminate();

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
    
    /*  In all the other cases, we perform an actual XYZ move and cancel
        the lift. */
    m_lifted = 0;
    m_pos = point;

    double travel_speed = this->config.travel_speed.value;
    if ((speed > 0) & (speed < travel_speed))
        travel_speed = speed;

    std::ostringstream gcode;
    gcode << write_acceleration();
    gcode << "G1 X" << XYZ_NUM(point.x())
          << " Y" << XYZ_NUM(point.y());
    if (config.z_step > SCALING_FACTOR)
        gcode << " Z" << PRECISION(point.z(), 6);
    else
        gcode << " Z" << XYZ_NUM(point.z());
    gcode <<   " F" << F_NUM(travel_speed * 60);

    COMMENT(comment);
    gcode << "\n";
    return gcode.str();
}

std::string GCodeWriter::travel_to_z(double z, const std::string &comment)
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
    return this->_travel_to_z(z, comment);
}

std::string GCodeWriter::_travel_to_z(double z, const std::string &comment)
{
    m_pos.z() = z;

    std::ostringstream gcode;

    gcode << write_acceleration();    if (config.z_step > SCALING_FACTOR)
        gcode << "G1 Z" << PRECISION(z, 6);
    else
        gcode << "G1 Z" << XYZ_NUM(z);

    const double speed = this->config.travel_speed_z.value == 0.0 ? this->config.travel_speed.value : this->config.travel_speed_z.value;
    gcode <<   " F" << F_NUM(speed * 60.0);
    COMMENT(comment);
    gcode << "\n";
    return gcode.str();
}

bool GCodeWriter::will_move_z(double z) const
{
    /* If target Z is lower than current Z but higher than nominal Z
        we don't perform an actual Z move. */
    if (m_lifted > 0) {
        double nominal_z = m_pos.z() - m_lifted;
        if (z >= nominal_z + EPSILON && z <= m_pos.z() - EPSILON)
            return false;
    }
    return true;
}

std::string GCodeWriter::extrude_to_xy(const Vec2d &point, double dE, const std::string &comment)
{
    assert(dE == dE);
    m_pos.x() = point.x();
    m_pos.y() = point.y();
    bool is_extrude = m_tool->extrude(dE) != 0;

    std::ostringstream gcode;
    gcode << write_acceleration();
    gcode << "G1 X" << XYZ_NUM(point.x())
        << " Y" << XYZ_NUM(point.y());
    if(is_extrude)
        gcode <<    " " << m_extrusion_axis << E_NUM(m_tool->E());
    COMMENT(comment);
    gcode << "\n";
    return gcode.str();
}

//BBS: generate G2 or G3 extrude which moves by arc
//point is end point which means X and Y axis
//center_offset is I and J axis
std::string GCodeWriter::extrude_arc_to_xy(const Vec2d& point, const Vec2d& center_offset, double dE, const bool is_ccw, const std::string& comment)
{
    m_pos.x() = point.x();
    m_pos.y() = point.y();
    bool is_extrude = m_tool->extrude(dE) != 0;

    GCodeG2G3Formatter w(this->config.gcode_precision_xyz.value, this->config.gcode_precision_e.value, is_ccw);
    w.emit_xy(point);
    w.emit_ij(center_offset);
    if (is_extrude)
        w.emit_e(m_extrusion_axis, m_tool->E());
    //BBS
    w.emit_comment(this->config.gcode_comments, comment);
    return w.string();
}

std::string GCodeWriter::extrude_to_xyz(const Vec3d &point, double dE, const std::string &comment)
{
    assert(dE == dE);
    m_pos.x() = point.x();
    m_pos.y() = point.y();
    m_lifted = 0;
    bool is_extrude = m_tool->extrude(dE) != 0;

//    GCodeG1Formatter w;
//    w.emit_xyz(point);
//    w.emit_e(m_extrusion_axis, m_extruder->E());
//    w.emit_comment(this->config.gcode_comments, comment);
//    return w.string();
    std::ostringstream gcode;
    gcode << write_acceleration();
    gcode << "G1 X" << XYZ_NUM(point.x())
        << " Y" << XYZ_NUM(point.y())
        << " Z" << XYZ_NUM(point.z() + m_pos.z());
    if (is_extrude)
            gcode <<    " " << m_extrusion_axis << E_NUM(m_tool->E());
    COMMENT(comment);
    gcode << "\n";
    return gcode.str();
}

std::string GCodeWriter::retract(bool before_wipe)
{
    double factor = before_wipe ? m_tool->retract_before_wipe() : 1.;
    assert(factor >= 0. && factor <= 1. + EPSILON);
    //check for override
    if (config_region && config_region->print_retract_length >= 0) {
        return this->_retract(
            factor * config_region->print_retract_length,
            factor * m_tool->retract_restart_extra(),
            NAN,
            "retract"
        );
    }
    return this->_retract(
        factor * m_tool->retract_length(),
        factor * m_tool->retract_restart_extra(),
        NAN,
        "retract"
    );
}

std::string GCodeWriter::retract_for_toolchange(bool before_wipe)
{
    double factor = before_wipe ? m_tool->retract_before_wipe() : 1.;
    assert(factor >= 0. && factor <= 1. + EPSILON);
    return this->_retract(
        factor * m_tool->retract_length_toolchange(),
        NAN,
        factor * m_tool->retract_restart_extra_toolchange(),
        "retract for toolchange"
    );
}

std::string GCodeWriter::_retract(double length, double restart_extra, double restart_extra_toolchange, const std::string &comment)
{
    std::ostringstream gcode;
    
    /*  If firmware retraction is enabled, we use a fake value of 1
        since we ignore the actual configured retract_length which 
        might be 0, in which case the retraction logic gets skipped. */
    if (this->config.use_firmware_retraction)
        length = 1;
    
    // If we use volumetric E values we turn lengths into volumes */
    if (this->config.use_volumetric_e) {
        double d = m_tool->filament_diameter();
        double area = d * d * PI/4;
        length = length * area;
        restart_extra = restart_extra * area;
        restart_extra_toolchange = restart_extra_toolchange * area;
    }
    
    double dE = m_tool->retract(length, restart_extra, restart_extra_toolchange);
    assert(dE >= 0);
    assert(dE < 10000000);
    if (dE != 0) {
        if (this->config.use_firmware_retraction) {
            if (FLAVOR_IS(gcfMachinekit))
                gcode << "G22 ; retract\n";
            else
                gcode << "G10 ; retract\n";
        } else if (! m_extrusion_axis.empty()) {
//            GCodeG1Formatter w;
//            w.emit_e(m_extrusion_axis, m_extruder->E());
//            w.emit_f(m_extruder->retract_speed() * 60.);
//            w.emit_comment(this->config.gcode_comments, comment);
//            gcode = w.string();
            gcode << "G1 " << m_extrusion_axis << E_NUM(m_tool->E())
                           << " F" << F_NUM(m_tool->retract_speed() * 60.);
            COMMENT(comment);
            gcode << "\n";
        }
    }
    
    if (FLAVOR_IS(gcfMakerWare))
        gcode << "M103 ; extruder off\n";
    
    return gcode.str();
}

std::string GCodeWriter::unretract()
{
    std::ostringstream gcode;
//    std::string gcode;
    
    if (FLAVOR_IS(gcfMakerWare))
        gcode << "M101 ; extruder on\n";
    
    double dE = m_tool->unretract();
    assert(dE >= 0);
    assert(dE < 10000000);
    if (dE != 0) {
        if (this->config.use_firmware_retraction) {
            gcode << (FLAVOR_IS(gcfMachinekit) ? "G23 ; unretract\n" : "G11 ; unretract\n");
            gcode << this->reset_e();
        } else if (! m_extrusion_axis.empty()) {
            // use G1 instead of G0 because G0 will blend the restart with the previous travel move
//            GCodeG1Formatter w;
//            w.emit_e(m_extrusion_axis, m_tool->E());
//            w.emit_f(m_tool->deretract_speed() * 60.);
//            w.emit_comment(this->config.gcode_comments, " ; unretract");
//            gcode += w.string();
            gcode << "G1 " << m_extrusion_axis << E_NUM(m_tool->E())
                           << " F" << F_NUM(m_tool->deretract_speed() * 60.);
            if (this->config.gcode_comments) gcode << " ; unretract";
            gcode << "\n";
        }
    }
    
    return gcode.str();
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
        std::string str =  this->_travel_to_z(m_pos.z() + target_lift - m_lifted, "lift Z");
        m_lifted = target_lift;
        return str;
    }
    return "";
}

std::string GCodeWriter::unlift()
{
    std::string gcode;
    if (m_lifted > 0) {
        gcode += this->_travel_to_z(m_pos.z() - m_lifted, "restore layer Z");
    }
    m_lifted = 0;
    return gcode;
}

std::string GCodeWriter::set_fan(const GCodeFlavor gcode_flavor, bool gcode_comments, uint8_t speed, uint8_t tool_fan_offset, bool is_fan_percentage)
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
    int8_t fan_speed = int8_t(std::min(uint8_t(100), speed));
    fan_speed += tool_fan_offset;
    fan_speed = std::max(int8_t(0), std::min(int8_t(100), fan_speed));
    const double fan_baseline = (is_fan_percentage ? 100.0 : 255.0);

    // write it
    if (fan_speed == 0) {
        if ((gcfTeacup == gcode_flavor)) {
            gcode << "M106 S0";
        } else if ((gcfMakerWare == gcode_flavor) || (gcfSailfish == gcode_flavor)) {
            gcode << "M127";
        } else {
            gcode << "M107";
        }
        if (gcode_comments) gcode << " ; disable fan";
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
        if (gcode_comments) gcode << " ; enable fan";
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

#ifdef USE_GCODEFORMATTER
void GCodeFormatter::emit_axis(const char axis, const double v, size_t digits) {
    assert(digits <= 9);
    static constexpr const std::array<int, 10> pow_10{1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000};
#ifndef DONT_USE_CHARCONV
    *ptr_err.ptr++ = ' '; *ptr_err.ptr++ = axis;

    char *base_ptr = this->ptr_err.ptr;
    auto  v_int    = int64_t(std::round(v * pow_10[digits]));
    // Older stdlib on macOS doesn't support std::from_chars at all, so it is used boost::spirit::karma::generate instead of it.
    // That is a little bit slower than std::to_chars but not much.
#ifdef __APPLE__
    boost::spirit::karma::generate(this->ptr_err.ptr, boost::spirit::karma::int_generator<int64_t>(), v_int);
#else
    // this->buf_end minus 1 because we need space for adding the extra decimal point.
    this->ptr_err = std::to_chars(this->ptr_err.ptr, this->buf_end - 1, v_int);
#endif
    size_t writen_digits = (this->ptr_err.ptr - base_ptr) - (v_int < 0 ? 1 : 0);
    if (writen_digits < digits) {
        // Number is smaller than 10^digits, so that we will pad it with zeros.
        size_t remaining_digits = digits - writen_digits;
        // Move all newly inserted chars by remaining_digits to allocate space for padding with zeros.
        for (char *from_ptr = this->ptr_err.ptr - 1, *to_ptr = from_ptr + remaining_digits; from_ptr >= this->ptr_err.ptr - writen_digits; --to_ptr, --from_ptr)
            *to_ptr = *from_ptr;

        memset(this->ptr_err.ptr - writen_digits, '0', remaining_digits);
        this->ptr_err.ptr += remaining_digits;
    }

    // Move all newly inserted chars by one to allocate space for a decimal point.
    for (char *to_ptr = this->ptr_err.ptr, *from_ptr = to_ptr - 1; from_ptr >= this->ptr_err.ptr - digits; --to_ptr, --from_ptr)
        *to_ptr = *from_ptr;

    *(this->ptr_err.ptr - digits) = '.';
    for (size_t i = 0; i < digits; ++i) {
        if (*this->ptr_err.ptr != '0')
            break;
        this->ptr_err.ptr--;
    }
    if (*this->ptr_err.ptr == '.')
        this->ptr_err.ptr--;
    if ((this->ptr_err.ptr + 1) == base_ptr || *this->ptr_err.ptr == '-')
        *(++this->ptr_err.ptr) = '0';
    this->ptr_err.ptr++;
#else
    * ptr_err_ptr++ = ' '; *ptr_err_ptr++ = axis;

    char* base_ptr = this->ptr_err_ptr;
    auto  v_int = int64_t(std::round(v * pow_10[digits]));
    // Older stdlib doesn't support std::from_chars at all, so it is used boost::spirit::karma::generate instead of it.
    // That is a little bit slower than std::to_chars but not much.
    boost::spirit::karma::generate(this->ptr_err_ptr, boost::spirit::karma::int_generator<int64_t>(), v_int);
    size_t writen_digits = (this->ptr_err_ptr - base_ptr) - (v_int < 0 ? 1 : 0);
    if (writen_digits < digits) {
        // Number is smaller than 10^digits, so that we will pad it with zeros.
        size_t remaining_digits = digits - writen_digits;
        // Move all newly inserted chars by remaining_digits to allocate space for padding with zeros.
        for (char* from_ptr = this->ptr_err_ptr - 1, *to_ptr = from_ptr + remaining_digits; from_ptr >= this->ptr_err_ptr - writen_digits; --to_ptr, --from_ptr)
            *to_ptr = *from_ptr;

        memset(this->ptr_err_ptr - writen_digits, '0', remaining_digits);
        this->ptr_err_ptr += remaining_digits;
    }

    // Move all newly inserted chars by one to allocate space for a decimal point.
    for (char* to_ptr = this->ptr_err_ptr, *from_ptr = to_ptr - 1; from_ptr >= this->ptr_err_ptr - digits; --to_ptr, --from_ptr)
        *to_ptr = *from_ptr;

    *(this->ptr_err_ptr - digits) = '.';
    for (size_t i = 0; i < digits; ++i) {
        if (*this->ptr_err_ptr != '0')
            break;
        this->ptr_err_ptr--;
    }
    if (*this->ptr_err_ptr == '.')
        this->ptr_err_ptr--;
    if ((this->ptr_err_ptr + 1) == base_ptr || *this->ptr_err_ptr == '-')
        *(++this->ptr_err_ptr) = '0';
    this->ptr_err_ptr++;
#endif

#ifndef NDEBUG
    {
        // Verify that the optimized formatter produces the same result as the standard sprintf().
#ifndef DONT_USE_CHARCONV
        double v1 = atof(std::string(base_ptr, this->ptr_err.ptr).c_str());
#else
        double v1 = atof(std::string(base_ptr, this->ptr_err_ptr).c_str());
#endif
        char buf[2048];
        sprintf(buf, "%.*lf", int(digits), v);
        double v2 = atof(buf);
        // Numbers may differ when rounding at exactly or very close to 0.5 due to numerical issues when scaling the double to an integer.
        // Thus the complex assert.
//        assert(v1 == v2);
        assert(std::abs(v1 - v) * pow_10[digits] < 0.50001);
        assert(std::abs(v2 - v) * pow_10[digits] < 0.50001);
    }
#endif // NDEBUG
}
#endif

} // namespace Slic3r
