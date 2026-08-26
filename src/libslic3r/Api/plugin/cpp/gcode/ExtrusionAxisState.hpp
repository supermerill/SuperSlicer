///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/ Copyright (c) Prusa Research 2017 - 2023 Vojtech Bubnik @bubnikv, Lukas Matena @lukasmatena
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_gcode_ExtrusionAxisState_hpp_
#define slic3r_Api_plugin_cpp_gcode_ExtrusionAxisState_hpp_

#include <cstdint>
#include <optional>

#include "GCodeFormatter.hpp"

/*
Firmware extrusion-axis state
==============================

ExtrusionAxisState converts deposited volume into E-axis coordinates and owns
both the exact requested position and the rounding remainder left by G-code
precision. Each call updates that state and returns only the optional,
quantized E number that a firmware encoder may display.
*/

namespace slic3r_api { namespace GCodeGeneration {

// Contains the configuration values needed by ExtrusionAxisState after setup.
// The owner extracts these values from Config so the reusable E-axis state does
// not know application setting keys or retain a borrowed configuration view.
struct ExtrusionAxisSettings
{
    bool use_relative_e_distances = false;
    bool use_volumetric_e = false;
    double extrusion_multiplier = 1.0;
    double filament_diameter = 0.0;
    double retract_speed = 0.0;
    double deretract_speed = 0.0;
    int32_t xyz_precision = 3;
    int32_t e_precision = 5;
};

// Owns the physical E-axis and accounting state for one logical extruder. It
// alone converts exact requested distances into quantized firmware numbers.
class ExtrusionAxisState
{
public:
    void setup(const ExtrusionAxisSettings &settings);
    void reset_runtime_state();
    void synchronize_runtime_from(const ExtrusionAxisState &source);

    // Update every E-axis counter and return the already-quantized number for
    // the firmware. An empty result means the delta is below output precision;
    // a present zero remains a valid absolute E0 command.
    std::optional<double> extrude(double delta_e);
    double retract(double retract_length,
                   std::optional<double> restart_extra,
                   std::optional<double> restart_extra_from_toolchange);
    double unretract();
    void reset_retract();
    bool need_unretract() const;
    double retract_to_go(double retract_length) const;

    bool reset_E();
    double e_per_mm(double mm3_per_mm) const { return mm3_per_mm * m_e_per_mm3; }
    double e_per_mm3() const { return m_e_per_mm3; }
    double extruded_volume() const;
    double used_filament() const;

    double position() const { return m_E; }
    double extruded_dE_left() const { return m_dE_left; }
    double retracted() const { return m_retracted; }
    double restart_extra() const { return m_restart_extra; }
    void set_position(double e);
    void set_retracted(double retracted, double restart_extra);

    // Import state reported after a complete external G-code block was
    // validated. The exact E position is omitted in relative mode. A genuine
    // external change invalidates the old formatting remainder because it was
    // calculated from a machine state that has been replaced.
    bool synchronize_after_external_gcode(std::optional<double> e_position,
                                          double retracted,
                                          double restart_extra);
    // Observe an E word which was already emitted by external G-code.
    void observe_external_move(double e_value, bool relative_mode);
    void set_relative_mode(bool relative_mode);

    double filament_diameter() const { return m_filament_diameter; }
    double filament_crossection() const;
    bool uses_relative_e_distances() const { return m_use_relative_e_distances; }
    double retract_speed() const { return m_retract_speed; }
    double deretract_speed() const { return m_deretract_speed; }

private:
    // --- Persistent physical/runtime state ---
    // Exact requested E-axis position since the last G92 E0 in absolute mode.
    // Relative mode keeps this at zero because every command is a delta.
    double m_E = 0.0;
    // Logical coordinate retained by the firmware when M82/M83 changes how E
    // words are interpreted. Unlike m_E, this also advances in relative mode.
    double m_machine_E = 0.0;
    // Difference between the exact request and the quantized value. In
    // relative mode this is the pending sub-precision delta for a later call.
    double m_dE_left = 0.0;
    // Extruder tachometer used for volume and filament-length statistics.
    double m_absolute_E = 0.0;
    // Current positive amount of filament retraction.
    double m_retracted = 0.0;
    // Extra priming amount scheduled for the next normal deretraction.
    double m_restart_extra = 0.0;
    // Extra priming amount scheduled after a tool-change retraction.
    double m_restart_extra_toolchange = 0.0;

    // --- Configuration cache populated by setup() ---
    // These values are copied from Config for fast use while serializing many
    // segments. reset_runtime_state() deliberately preserves all of them.
    // Addressing mode controls whether generated E values are deltas or an
    // absolute position measured from the latest reset.
    bool m_use_relative_e_distances = false;
    bool m_configured_use_relative_e_distances = false;
    // Volumetric mode changes how E and the accumulated statistics are scaled.
    bool m_use_volumetric_e = false;
    // Conversion factor from deposited volume in mm3 to encoded E units.
    double m_e_per_mm3 = 0.0;
    // Filament diameter used by the conversion and usage statistics.
    double m_filament_diameter = 0.0;
    // Configured E-only speed used while retracting filament.
    double m_retract_speed = 0.0;
    // Configured E-only speed used while restoring filament.
    double m_deretract_speed = 0.0;
    // Quantizes E state consistently with the text formatter used by firmware.
    GCodeFormatter m_formatter{0, 0};
};

}} // namespace slic3r_api::GCodeGeneration

#endif // slic3r_Api_plugin_cpp_gcode_ExtrusionAxisState_hpp_
