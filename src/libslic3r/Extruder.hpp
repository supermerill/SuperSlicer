///|/ Copyright (c) Prusa Research 2017 - 2023 Vojtěch Bubník @bubnikv, Lukáš Matěna @lukasmatena
///|/ Copyright (c) 2017 Joseph Lenox @lordofhyphens
///|/ Copyright (c) Slic3r 2014 - 2015 Alessandro Ranellucci @alranel
///|/
///|/ ported from lib/Slic3r/Extruder.pm:
///|/ Copyright (c) Slic3r 2011 - 2014 Alessandro Ranellucci @alranel
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Extruder_hpp_
#define slic3r_Extruder_hpp_

#include <optional>

#include "libslic3r.h"
#include "GCode/GCodeFormatter.hpp"
#include "Point.hpp"

namespace Slic3r {

class GCodeConfig;

class Tool
{
public:
    Tool(uint16_t id, GCodeConfig &config);
    ~Tool() = default;

    /*void   reset() {
        m_E             = 0;
        m_absolute_E    = 0;
        m_retracted     = 0;
        m_restart_extra = 0;
        m_restart_extra_toolchange = 0; // note: can't call  retract_restart_extra_toolchange(); because virtual inheritance doesn't work when only the tool is build (constructor)
    }*/

    uint16_t id() const { return m_id; }

    // Following three methods emit:
    // first  - extrusion delta
    // second - number to emit to G-code: This may be delta for relative mode or a distance from last reset_E() for absolute mode.
    // They also quantize the E axis to G-code resolution.
    virtual std::pair<double, double> extrude(double dE);
    virtual std::pair<double, double> retract(double retract_length, std::optional<double> restart_extra, std::optional<double> restart_extra_from_toolchange);
    virtual std::pair<double, double> unretract();
    virtual void                      reset_retract();
    virtual bool                      need_unretract();
    // How much to retract yet before retract_length is reached?
    // The value is quantized to G-code resolution.
    virtual double                    retract_to_go(double retract_length) const;

    // Reset the current state of the E axis (this is only needed for relative extruder addressing mode anyways).
    // Returns true if the extruder was non-zero before reset.
    bool   reset_E() { bool modified = m_E != 0; m_E = 0.; return modified; }
    double e_per_mm(double mm3_per_mm) const { return mm3_per_mm * m_e_per_mm3; }
    double e_per_mm3() const { return m_e_per_mm3; }
    // Used filament volume in mm^3.
    virtual double extruded_volume() const;
    // Used filament length in mm.
    virtual double used_filament() const;

    // Getters for the PlaceholderParser.
    // Get current extruder position. Only applicable with absolute extruder addressing.
    double position() const { return m_E; }
    // Get current retraction value. Only non-negative values.
    double retracted() const { return m_retracted; }
    // Get extra retraction planned after
    double restart_extra() const { return m_restart_extra; }
    // Setters for the PlaceholderParser.
    // Set current extruder position. Only applicable with absolute extruder addressing.
    void   set_position(double e) { m_E = e; }
    // Sets current retraction value & restart extra filament amount if retracted > 0.
    void   set_retracted(double retracted, double restart_extra);
    
    virtual double filament_diameter() const;
    double filament_crossection() const { return this->filament_diameter() * this->filament_diameter() * 0.25 * PI; }
    virtual double filament_density() const;
    virtual double filament_cost() const;
    virtual double extrusion_multiplier() const;
    virtual double retract_before_wipe() const;
    virtual double retract_length() const;
    virtual double retract_lift() const;
    virtual int    retract_speed() const;
    virtual int    deretract_speed() const;
    virtual double retract_restart_extra() const;
    virtual double retract_length_toolchange() const;
    virtual double retract_restart_extra_toolchange() const;
    virtual Vec2d  xy_offset() const;
    virtual int16_t temp_offset() const;
    virtual int8_t fan_offset() const;

protected:
    // Private constructor to create a key for a search in std::set.
    Tool(uint16_t id) : m_id(id), m_formatter(1, 1) {}

    // Reference to GCodeWriter instance owned by GCodeWriter.
    GCodeConfig *m_config;
    // Print-wide global ID of this extruder.
    uint16_t    m_id;
    // Current state of the extruder axis.
    // For absolute extruder addressing, it is the current state since the last reset (G92 E0) issued at the end of the last retraction.
    // For relative extruder addressing, it is the E axis difference emitted into the G-code the last time.
    double       m_E { 0 };
    // Current state of the extruder tachometer, used to output the extruded_volume() and used_filament() statistics.
    double       m_absolute_E { 0 };
    // Current positive amount of retraction.
    double       m_retracted { 0 };
    // When retracted, this value stores the extra amount of priming on deretraction.
    double       m_restart_extra { 0 };
    double       m_restart_extra_toolchange;
    double       m_e_per_mm3;
    // to quantize E
    GCodeFormatter m_formatter;
};


class Mill : public Tool
{
public:
    Mill(uint16_t mill_id, GCodeConfig &config);
    ~Mill() = default;
    double retract_lift() const override;

    uint16_t mill_id() const { return m_mill_id; }

protected:
    // Private constructor to create a key for a search in std::set.
    Mill(uint16_t tool_id) : Tool(tool_id) {}
    uint16_t    m_mill_id;
}; 

class Extruder : public Tool
{
public:
    Extruder(uint16_t id, GCodeConfig &config);
    virtual ~Extruder() {}

    double filament_diameter() const override;
    double filament_density() const override;
    double filament_cost() const override;
    double extrusion_multiplier() const override;
    double retract_before_wipe() const override;
    double retract_length() const override;
    double retract_lift() const override;
    int    retract_speed() const override;
    int    deretract_speed() const override;
    double retract_restart_extra() const override;
    double retract_length_toolchange() const override;
    double retract_restart_extra_toolchange() const override;
    Vec2d  xy_offset() const override;
    int16_t temp_offset() const override;
    int8_t fan_offset() const override;

protected:
    // Private constructor to create a key for a search in std::set.
    Extruder(uint16_t id) : Tool(id) {}
};

// Sort Extruder objects by the extruder id by default.
inline bool operator==(const Tool& e1, const Tool& e2) { return e1.id() == e2.id(); }
inline bool operator!=(const Tool& e1, const Tool& e2) { return e1.id() != e2.id(); }
inline bool operator< (const Tool& e1, const Tool& e2) { return e1.id() < e2.id(); }
inline bool operator> (const Tool& e1, const Tool& e2) { return e1.id() > e2.id(); }
inline bool operator==(const Extruder& e1, const Extruder& e2) { return e1.id() == e2.id(); }
inline bool operator!=(const Extruder& e1, const Extruder& e2) { return e1.id() != e2.id(); }
inline bool operator< (const Extruder& e1, const Extruder& e2) { return e1.id() < e2.id(); }
inline bool operator> (const Extruder& e1, const Extruder& e2) { return e1.id() > e2.id(); }

}

#endif // slic3r_Extruder_hpp_
