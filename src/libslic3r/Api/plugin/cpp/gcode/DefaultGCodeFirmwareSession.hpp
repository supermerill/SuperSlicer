///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_gcode_DefaultGCodeFirmwareSession_hpp_
#define slic3r_Api_plugin_cpp_gcode_DefaultGCodeFirmwareSession_hpp_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "libslic3r/Api/plugin/cpp/gcode/GCodeFirmwareViews.hpp"
#include "libslic3r/Api/plugin/cpp/gcode/GCodeFormatter.hpp"
#include "libslic3r/Api/plugin/cpp/gcode/DefaultExtruder.hpp"
#include "libslic3r/Api/plugin/cpp/gcode/Gantry.hpp"
#include "libslic3r/Api/plugin/cpp/gcode/MachineEnvelope.hpp"
#include "libslic3r/Api/plugin/cpp/gcode/Printer.hpp"

/*
Reusable standard PrintingPlan firmware session
================================================

DefaultGCodeFirmwareSession turns ordered PrintingPlan extrusions into a
modern Marlin-style command stream. The session owns all machine state for one
export: position, E coordinates, selected tool, requested process values and
the values already sent to the printer.

The traversal and state transition algorithm is deliberately non-virtual. A
derived firmware changes syntax by overriding one small protected encoder, for
example encode_fan(), without having to duplicate inheritance, extrusion
accounting or command de-duplication.

The class lives in the public C++ plugin API so another firmware provider may
derive from it and replace only the encoders whose syntax differs. It does not
register a plugin by itself. PrintingPlanFileWriter remains responsible for the
output artifact and writes each returned string in plan order.
*/

namespace slic3r_api { namespace GCodeGeneration {

// Owns every state needed to serialize one PrintingPlan export. The session
// interprets extrusion trees, updates the generic machine-state objects and
// delegates only command syntax to its protected encode_* methods. Derived
// firmware classes may replace those encoders, but must leave traversal,
// requested/encoded state transitions and extrusion accounting to this class.
class DefaultGCodeFirmwareSession : public GCodeFirmwareSession
{
public:
    DefaultGCodeFirmwareSession() = default;
    ~DefaultGCodeFirmwareSession() override = default;

    // Copy every setting needed during the export and reset all transient
    // machine state. The borrowed Config view is not retained.
    void setup(const Config &config);

    std::string begin_print(const Print &print) override;
    std::string begin_group(const PrintingGroup &group) override;
    std::string begin_layer(const PrintingLayerGroup &layer) override;
    std::string begin_tool_group(const PrintingToolGroup &tool_group) override;
    std::string write_extrusion(const PrintingExtrusion &extrusion) override;
    std::string end_tool_group() override;
    std::string end_layer() override;
    std::string end_group() override;
    std::string end_print() override;

protected:
    // Describes one movement after geometry and extrusion have been converted
    // to machine units. It is a short-lived value passed to write_lines() and
    // encode_move(); it does not own or mark any persistent machine state as encoded
    // state. An absent destination or extrusion means that axis did not change.
    struct PreparedMove
    {
        enum class Kind : uint8_t { None, Travel, Extrusion };

        Kind kind = Kind::None;
        std::optional<c_vec3d> destination;
        // Quantized E-axis value ready for the firmware encoder. ExtrusionAxisState
        // retains all exact-distance and rounding state internally.
        std::optional<double> extrusion;
        float radius_mm = 0.f;
        raw_extrusion_arc_orientation arc_orientation = RAW_EXTRUSION_ARC_ORIENTATION_UNKNOWN;
    };

    Gantry &gantry() { return m_gantry; }
    const Gantry &gantry() const { return m_gantry; }
    Printer &printer() { return m_printer; }
    const Printer &printer() const { return m_printer; }
    std::vector<DefaultExtruder> &extruders() { return m_extruders; }
    const std::vector<DefaultExtruder> &extruders() const { return m_extruders; }
    DefaultExtruder &current_extruder();
    const DefaultExtruder &current_extruder() const;
    std::optional<uint16_t> current_extruder_id() const { return m_current_extruder_idx; }

    // Give dialect encoders the same numeric formatting policy as the base
    // implementation without exposing ownership of the session formatter.
    const GCodeFormatter &gcode_formatter() const;
    static std::string format_number(double value, uint16_t precision = 5);

    // Unsupported operations remain visible in generated output. Returning a
    // valid comment also lets the caller mark the request as handled, instead
    // of retrying the same unavailable command before every movement.
    static std::string encode_unsupported_operation(const char *operation);

    // A dialect may copy additional setup values after the generic machine
    // state has been initialized. The borrowed Config view remains valid only
    // for the duration of this call.
    virtual void setup_firmware(const Config &config);

    // Convert validated printer limits into this dialect's startup commands.
    // The neutral base emits nothing; concrete firmware sessions choose both
    // the supported fields and their command syntax.
    virtual std::string encode_machine_envelope(const MachineEnvelope &envelope) const;

    // These hooks let a derived firmware observe additional property types
    // without replacing the extrusion-tree visitor. A derived implementation
    // may maintain its own inheritance stack: push or override state on entry,
    // consume it before a leaf is serialized, then restore it on exit. The
    // default hooks emit no text and keep no additional state.
    virtual std::string enter_extrusion_node(const ExtrusionEntity &entity);
    virtual std::string visit_extrusion_leaf(const ExtrusionEntity &entity);
    virtual std::string leave_extrusion_node(const ExtrusionEntity &entity);

    // Encode and acknowledge the acceleration required by this movement. The
    // standard implementation preserves independent print and travel
    // histories; specialized sessions may override their relationship without
    // changing the extrusion traversal performed by write_lines().
    virtual std::string write_acceleration(PreparedMove::Kind kind);

    // Apply the dialect's state-sharing policy when a different extruder is
    // selected. The generic implementation shares nothing: a concrete
    // firmware must copy only the components addressed as shared hardware.
    virtual void synchronize_selected_extruder_state(const DefaultExtruder *previous,
                                                      DefaultExtruder &selected);

    // The methods below encode one already-decided state transition. They must
    // not mutate session state; write_lines() validates state only after an
    // encoder returns successfully.
    virtual std::string encode_tool_change(uint16_t tool_id) const;
    virtual std::string encode_tool_temperature(uint16_t tool_id,
                                                int16_t temperature,
                                                bool wait) const;
    virtual std::string encode_bed_temperature(int16_t temperature, bool wait) const;
    virtual std::string encode_chamber_temperature(int16_t temperature, bool wait) const;
    virtual std::string encode_fan(uint16_t tool_id, double speed_percent) const;
    virtual std::string encode_pressure_advance(uint16_t tool_id,
                                                double pressure_advance) const;
    virtual std::string encode_acceleration(uint32_t acceleration, bool travel) const;
    virtual std::string encode_move(const PreparedMove &move, bool include_speed) const;
    virtual std::string encode_custom_gcode(c_extrusion_custom_gcode_kind kind,
                                            const std::string &text) const;
    virtual std::string encode_save_speed_ratio(double ratio) const;
    virtual std::string encode_restore_speed_ratio() const;
    virtual std::string encode_flush_planner() const;
    virtual std::string encode_pause(double milliseconds) const;
    virtual std::string encode_extruder_current(uint16_t tool_id, double current) const;

private:
    struct RequestedState;
    class ExtrusionWriterVisitor;

    std::string select_extruder(uint16_t tool_id);
    std::string write_lines(const PreparedMove &move);
    std::string write_leaf_geometry(const ExtrusionEntity &leaf, const RequestedState &state);
    std::string write_special_command(const EPropertySpecialCommand &command,
                                      const RequestedState &state);
    std::string write_custom_gcode(const ExtrusionEntity &entity,
                                   const EPropertyCustomGcode &custom_gcode);
    void apply_requested_state(const RequestedState &state);
    c_vec3d machine_position(c_point point, coord_t z_offset) const;
    double segment_length_mm(const c_extrusion_segment &segment) const;
    DefaultExtruder &extruder(uint16_t tool_id);
    const DefaultExtruder &extruder(uint16_t tool_id) const;

    Gantry m_gantry;
    Printer m_printer;
    std::vector<DefaultExtruder> m_extruders;
    std::optional<uint16_t> m_current_extruder_idx;
    std::unique_ptr<GCodeFormatter> m_formatter;
    coord_t m_layer_print_z = 0;
    bool m_is_setup = false;
};

}} // namespace slic3r_api::GCodeGeneration

#endif // slic3r_Api_plugin_cpp_gcode_DefaultGCodeFirmwareSession_hpp_
