///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "DefaultGCodeFirmwareSession.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>

#include "libslic3r/Api/plugin/cpp/ExtrusionTreeVisitors.hpp"

/*
Standard PrintingPlan firmware implementation
=============================================

The implementation has two layers. ExtrusionWriterVisitor resolves inherited
properties while it walks one extrusion tree. DefaultGCodeFirmwareSession then
compares those requested values with its persistent machine state and encodes
only the Marlin output required to represent the requested state.

Every output fragment is generated before its corresponding state is marked as
encoded. If an encoder throws, the caller receives an error and the session
does not record the failed output as successfully generated.
*/

namespace slic3r_api { namespace GCodeGeneration {
namespace {

uint32_t option_size_or_zero(const Config &config, const char *key)
{
    // Optional vector settings may be absent in focused tests. Treating them
    // as empty lets the mandatory nozzle vector determine the tool count.
    return config.has(key) ? config.get(key).size() : 0;
}

std::string compact_number(double value, uint16_t precision = 5)
{
    // Marlin accepts decimal text without trailing zeroes. A classic locale
    // prevents a system locale from replacing the decimal point with a comma.
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(precision) << value;
    std::string text = stream.str();
    while (text.size() > 1 && text.back() == '0')
        text.pop_back();
    if (!text.empty() && text.back() == '.')
        text.pop_back();
    return text == "-0" ? "0" : text;
}

bool coordinates_differ(const c_vec3d &lhs, const c_vec3d &rhs)
{
    return lhs.x != rhs.x || lhs.y != rhs.y || lhs.z != rhs.z;
}

} // namespace

// Holds the effective extrusion properties at the visitor's current position
// in an extrusion tree. This state follows lexical inheritance: entering a
// child may override fields and leaving it restores the parent values. It is
// deliberately separate from Gantry, Printer and DefaultExtruder, whose
// requested, encoded and physical states persist across sibling tree nodes.
struct DefaultGCodeFirmwareSession::RequestedState
{
    std::optional<float> speed_mm_per_s;
    std::optional<float> acceleration_mm_per_s2;
    std::optional<float> pressure_advance;
    std::optional<float> fan_speed_percent;
    std::optional<float> temperature_c;
    std::optional<EPropertyAttributes> attributes;
    coord_t z_offset = 0;
};

// Resolves inherited properties and visits leaves in geometric output order
// for one extrusion root. The visitor owns only the temporary inheritance
// stack and accumulated text. It asks the session to apply machine requests,
// encode events and serialize geometry, while the session remains responsible
// for all persistent machine and tool state across roots.
class DefaultGCodeFirmwareSession::ExtrusionWriterVisitor final :
    public ExtrusionTreeConstVisitor<>
{
public:
    explicit ExtrusionWriterVisitor(DefaultGCodeFirmwareSession &session) : m_session(session) {}

    std::string write(const ExtrusionEntity &root)
    {
        m_output.clear();
        m_state = RequestedState{};
        m_parent_states.clear();
        traverse(root);
        return m_output;
    }

protected:
    void enter_node(ExtrusionEntity entity) override
    {
        // Save the inherited request before applying direct properties. The
        // matching leave_node() restores this exact value for the next sibling.
        m_parent_states.push_back(m_state);

        if (const EPropertySpeed *speed = entity.property<EPropertySpeed>()) {
            if (speed->speed_mm_per_s > 0.f)
                m_state.speed_mm_per_s = speed->speed_mm_per_s;
            if (speed->accel_mm_per_s2 > 0.f)
                m_state.acceleration_mm_per_s2 = speed->accel_mm_per_s2;
            if (speed->pressure_adv >= 0.f)
                m_state.pressure_advance = speed->pressure_adv;
            if (speed->fan_speed_percent >= 0.f)
                m_state.fan_speed_percent = speed->fan_speed_percent;
            if (speed->temperature_C >= 0.f)
                m_state.temperature_c = speed->temperature_C;
        }
        if (const EPropertyAttributes *attributes = entity.property<EPropertyAttributes>())
            m_state.attributes = *attributes;
        if (const EPropertyZOffset *z_offset = entity.property<EPropertyZOffset>())
            m_state.z_offset = z_offset->get();

        // Direct event properties describe one position in the ordered tree.
        // Process them once on node entry instead of repeating them for every
        // descendant leaf.
        m_session.apply_requested_state(m_state);
        m_output += m_session.enter_extrusion_node(entity);
        if (const EPropertyCustomGcode *custom_gcode = entity.property<EPropertyCustomGcode>())
            m_output += m_session.write_custom_gcode(entity, *custom_gcode);
        if (const EPropertySpecialCommand *command = entity.property<EPropertySpecialCommand>())
            m_output += m_session.write_special_command(*command, m_state);
    }

    void visit_leaf(ExtrusionEntity entity) override
    {
        // Give derived firmware state a chance to react before this leaf emits
        // any standard process command or movement.
        m_output += m_session.visit_extrusion_leaf(entity);
        if (entity.segment_count() > 0)
            m_output += m_session.write_leaf_geometry(entity, m_state);
    }

    void leave_node(ExtrusionEntity entity) override
    {
        // Requested values are lexical tree state. Machine position, E and
        // already encoded values live in the session objects and are therefore
        // intentionally not restored here.
        m_output += m_session.leave_extrusion_node(entity);
        m_state = m_parent_states.back();
        m_parent_states.pop_back();
        m_session.apply_requested_state(m_state);
    }

private:
    DefaultGCodeFirmwareSession &m_session;
    RequestedState m_state;
    std::vector<RequestedState> m_parent_states;
    std::string m_output;
};

void DefaultGCodeFirmwareSession::setup(const Config &config)
{
    // The widest per-tool vector defines how many tool-state objects are
    // needed. At least one extruder is created for valid single-tool presets
    // and for compact test configurations.
    const uint32_t extruder_count = std::max<uint32_t>({
        1,
        option_size_or_zero(config, "nozzle_diameter"),
        option_size_or_zero(config, "filament_diameter"),
        option_size_or_zero(config, "extruder_offset"),
        option_size_or_zero(config, "extrusion_multiplier")
    });

    m_extruders.clear();
    m_extruders.reserve(extruder_count);
    for (uint32_t extruder_idx = 0; extruder_idx < extruder_count; ++extruder_idx) {
        m_extruders.emplace_back(uint16_t(extruder_idx));
        m_extruders.back().setup(config);
    }

    m_gantry.setup(config);
    m_printer.setup(config);
    const int32_t max_precision = int32_t(GCodeFormatter::pow_10.size()) - 1;
    const int32_t xyz_precision = std::clamp(config.int_or_default("gcode_precision_xyz", 3), 0, max_precision);
    const int32_t e_precision = std::clamp(config.int_or_default("gcode_precision_e", 5), 0, max_precision);
    m_formatter.reset(new GCodeFormatter(uint16_t(xyz_precision), uint16_t(e_precision)));
    m_current_extruder_idx.reset();
    m_layer_print_z = 0;
    setup_firmware(config);
    m_is_setup = true;
}

std::string DefaultGCodeFirmwareSession::begin_print(const Print &print)
{
    // Establish the complete export state before invoking a dialect encoder,
    // because that encoder may inspect configured tools or the formatter.
    setup(print.config());

    // MachineEnvelope is the neutral boundary between printer settings and
    // firmware syntax. A disabled envelope produces no startup command.
    const std::optional<MachineEnvelope> envelope =
        configured_machine_envelope(print.config());
    return envelope ? encode_machine_envelope(*envelope) : std::string();
}

std::string DefaultGCodeFirmwareSession::begin_group(const PrintingGroup &)
{
    if (!m_is_setup)
        throw std::logic_error("The firmware session was not initialized by begin_print().");
    return {};
}

std::string DefaultGCodeFirmwareSession::begin_layer(const PrintingLayerGroup &layer)
{
    if (!m_is_setup)
        throw std::logic_error("The firmware session was not initialized by begin_print().");
    m_layer_print_z = layer.print_z();
    return {};
}

std::string DefaultGCodeFirmwareSession::begin_tool_group(const PrintingToolGroup &tool_group)
{
    if (!m_is_setup)
        throw std::logic_error("The firmware session was not initialized by begin_print().");
    return select_extruder(tool_group.extruder_id());
}

std::string DefaultGCodeFirmwareSession::write_extrusion(const PrintingExtrusion &extrusion)
{
    if (!m_is_setup)
        throw std::logic_error("The firmware session was not initialized by begin_print().");
    const extrusion_entity_handle *root_handle = printing_extrusion_get_root(extrusion.handle());
    if (root_handle == nullptr)
        throw std::invalid_argument("The PrintingExtrusion has no extrusion root.");

    return write_extrusion_tree(ExtrusionEntity(root_handle));
}

std::string DefaultGCodeFirmwareSession::write_event(const ExtrusionEntity &event_root)
{
    if (!m_is_setup)
        throw std::logic_error("The firmware session was not initialized by begin_print().");
    return write_extrusion_tree(event_root);
}

std::string DefaultGCodeFirmwareSession::write_extrusion_tree(const ExtrusionEntity &root)
{
    // Scope events and printable roots share property inheritance and event
    // handling. A fresh visitor isolates that lexical state while the session
    // retains machine state across both kinds of tree.
    ExtrusionWriterVisitor visitor(*this);
    return visitor.write(root);
}

std::string DefaultGCodeFirmwareSession::end_tool_group()
{
    return {};
}

std::string DefaultGCodeFirmwareSession::end_layer()
{
    return {};
}

std::string DefaultGCodeFirmwareSession::end_group()
{
    return {};
}

std::string DefaultGCodeFirmwareSession::end_print()
{
    return {};
}

std::string DefaultGCodeFirmwareSession::enter_extrusion_node(const ExtrusionEntity &)
{
    // The base firmware has no state beyond the properties resolved by its
    // visitor. Derived firmwares override this hook for additional properties.
    return {};
}

std::string DefaultGCodeFirmwareSession::visit_extrusion_leaf(const ExtrusionEntity &)
{
    // Standard leaf serialization is performed immediately after this hook by
    // write_leaf_geometry(). Derived state may prepare itself here first.
    return {};
}

std::string DefaultGCodeFirmwareSession::leave_extrusion_node(const ExtrusionEntity &)
{
    // Derived firmwares may restore their own lexical state here. The standard
    // visitor restores its RequestedState immediately after this hook returns.
    return {};
}

void DefaultGCodeFirmwareSession::synchronize_selected_extruder_state(
    const DefaultExtruder *,
    DefaultExtruder &)
{
    // The reusable session assumes that every tool component is independently
    // addressable. A dialect which emits a command for shared hardware must
    // explicitly copy that component's encoding history in its override.
}

const GCodeFormatter &DefaultGCodeFirmwareSession::gcode_formatter() const
{
    if (!m_formatter)
        throw std::logic_error("The G-code formatter is not initialized.");
    return *m_formatter;
}

std::string DefaultGCodeFirmwareSession::format_number(double value, uint16_t precision)
{
    return compact_number(value, precision);
}

std::string DefaultGCodeFirmwareSession::encode_unsupported_operation(const char *operation)
{
    if (operation == nullptr || operation[0] == '\0')
        throw std::invalid_argument("An unsupported firmware operation needs a name.");
    return std::string("; Unsupported firmware operation: ") + operation + "\n";
}

void DefaultGCodeFirmwareSession::setup_firmware(const Config &)
{
    // The standard dialect has no setup cache beyond the generic machine
    // objects. Derived sessions use this hook for dialect-only configuration.
}

std::string DefaultGCodeFirmwareSession::encode_machine_envelope(
    const MachineEnvelope &) const
{
    // The reusable session does not select a firmware dialect. Concrete
    // providers opt into machine-limit commands by overriding this hook.
    return {};
}

DefaultExtruder &DefaultGCodeFirmwareSession::extruder(uint16_t tool_id)
{
    if (tool_id >= m_extruders.size())
        throw std::out_of_range("The PrintingPlan selected an extruder that is not configured.");
    return m_extruders[tool_id];
}

const DefaultExtruder &DefaultGCodeFirmwareSession::extruder(uint16_t tool_id) const
{
    if (tool_id >= m_extruders.size())
        throw std::out_of_range("The PrintingPlan selected an extruder that is not configured.");
    return m_extruders[tool_id];
}

DefaultExtruder &DefaultGCodeFirmwareSession::current_extruder()
{
    if (!m_current_extruder_idx)
        throw std::logic_error("A printable extrusion requires an active extruder.");
    return extruder(*m_current_extruder_idx);
}

const DefaultExtruder &DefaultGCodeFirmwareSession::current_extruder() const
{
    if (!m_current_extruder_idx)
        throw std::logic_error("A printable extrusion requires an active extruder.");
    return extruder(*m_current_extruder_idx);
}

std::string DefaultGCodeFirmwareSession::select_extruder(uint16_t tool_id)
{
    DefaultExtruder &selected = extruder(tool_id);
    if (m_current_extruder_idx && *m_current_extruder_idx == tool_id)
        return {};

    DefaultExtruder *previous = m_current_extruder_idx ?
        &extruder(*m_current_extruder_idx) : nullptr;

    // Generate the tool command before committing the selected index. A
    // derived encoder may throw without leaving the session on a fictive tool.
    const std::string output = encode_tool_change(selected.id());
    synchronize_selected_extruder_state(previous, selected);
    m_current_extruder_idx = tool_id;
    return output;
}

void DefaultGCodeFirmwareSession::apply_requested_state(const RequestedState &state)
{
    m_gantry.request_speed(state.speed_mm_per_s ?
        std::optional<double>(*state.speed_mm_per_s) : std::optional<double>());

    if (!m_current_extruder_idx)
        return;

    // Tool properties are requested on the currently selected physical tool.
    // A later tool change receives the same inherited state when the visitor
    // calls this method again before processing the next event or leaf.
    DefaultExtruder &tool = current_extruder();
    tool.heater().request_temperature(state.temperature_c ?
        std::optional<int16_t>(int16_t(std::lround(*state.temperature_c))) :
        std::optional<int16_t>());
    tool.fan().request_speed_percent(state.fan_speed_percent ?
        std::optional<double>(*state.fan_speed_percent) : std::optional<double>());
    tool.pressure_advance().request(state.pressure_advance ?
        std::optional<double>(*state.pressure_advance) : std::optional<double>());
}

c_vec3d DefaultGCodeFirmwareSession::machine_position(c_point point, coord_t z_offset) const
{
    const DefaultExtruder &tool = current_extruder();
    const c_vec2d tool_offset = tool.xy_offset();
    return {
        unscaled(point.x) - tool_offset.x,
        unscaled(point.y) - tool_offset.y,
        unscaled(m_layer_print_z + z_offset) + m_gantry.z_offset() - tool.z_offset()
    };
}

double DefaultGCodeFirmwareSession::segment_length_mm(const c_extrusion_segment &segment) const
{
    const double chord = c_point_distance_to(segment.point_a, segment.point_b);
    if (segment.radius == 0.f)
        return unscaled(chord);

    const double radius = std::abs(double(segment.radius));
    if (radius <= 0.0 || chord > radius * 2.0 + double(SCALED_EPSILON))
        throw std::invalid_argument("An extrusion arc has an invalid radius.");

    // A positive radius denotes the shorter circular arc. A negative radius
    // denotes the complementary long arc between the same endpoints.
    const double ratio = std::clamp(chord / (2.0 * radius), 0.0, 1.0);
    double angle = 2.0 * std::asin(ratio);
    if (segment.radius < 0.f)
        angle = 2.0 * PI - angle;
    return unscaled(radius * angle);
}

std::string DefaultGCodeFirmwareSession::write_leaf_geometry(const ExtrusionEntity &leaf,
                                                              const RequestedState &state)
{
    if (!m_current_extruder_idx)
        throw std::invalid_argument("An extrusion movement has no active tool.");
    if (!state.attributes)
        throw std::invalid_argument("An extrusion movement needs effective attributes.");
    const bool is_travel = RAW_EXTRUSION_ROLE_IS_TRAVEL(state.attributes->extrusion_role());
    if (!is_travel &&
        (state.attributes->c_extrusion_property_attributes::mm3_per_mm <= 0.0 ||
         !std::isfinite(state.attributes->c_extrusion_property_attributes::mm3_per_mm)))
        throw std::invalid_argument("A printable extrusion needs valid flow attributes.");
    if (!state.speed_mm_per_s || *state.speed_mm_per_s <= 0.f)
        throw std::invalid_argument("An extrusion movement needs a positive speed.");
    if (!state.acceleration_mm_per_s2 || *state.acceleration_mm_per_s2 <= 0.f)
        throw std::invalid_argument("An extrusion movement needs a positive acceleration.");

    // The property stores one effective acceleration. Its leaf role chooses
    // which independent request it updates; the other request remains pending
    // until another leaf explicitly changes it.
    const uint32_t acceleration = uint32_t(std::lround(*state.acceleration_mm_per_s2));
    if (is_travel)
        m_gantry.request_travel_acceleration(acceleration);
    else
        m_gantry.request_print_acceleration(acceleration);
    apply_requested_state(state);
    std::string output;
    const c_extrusion_segment first_segment = leaf.segment(0);
    const c_vec3d first_position = machine_position(
        first_segment.point_a, state.z_offset + first_segment.z_offset_a);
    const c_vec3d quantized_first = m_formatter->quantize(first_position);

    // Reach the first path point with travel speed. Acceleration deliberately
    // remains the effective value carried by EPropertySpeed: an earlier
    // pipeline step has already resolved every regional acceleration rule.
    if (!m_gantry.position() || coordinates_differ(*m_gantry.position(), quantized_first)) {
        const std::optional<double> print_speed = m_gantry.requested_speed();
        m_gantry.request_speed(m_gantry.travel_speed());
        PreparedMove travel;
        travel.kind = PreparedMove::Kind::Travel;
        travel.destination = first_position;
        output += write_lines(travel);
        m_gantry.request_speed(print_speed);
    }

    // Convert every geometric segment into one prepared machine move. A travel
    // uses the same resolved speed property but deliberately carries no E.
    // Printable paths compute E from true arc length and preserve the original
    // arc for G2/G3 output.
    for (uint32_t segment_idx = 0; segment_idx < leaf.segment_count(); ++segment_idx) {
        const c_extrusion_segment segment = leaf.segment(segment_idx);

        PreparedMove move;
        move.kind = is_travel ? PreparedMove::Kind::Travel : PreparedMove::Kind::Extrusion;
        move.destination = machine_position(segment.point_b, state.z_offset + segment.z_offset_b);
        if (!is_travel) {
            const double path_length = segment_length_mm(segment);
            const double delta_e = path_length * current_extruder().extrusion_axis().e_per_mm(
                state.attributes->c_extrusion_property_attributes::mm3_per_mm);
            move.extrusion = current_extruder().extrusion_axis().extrude(delta_e);
        }
        move.radius_mm = segment.radius == 0.f ? 0.f : float(unscaled(double(segment.radius)));
        move.arc_orientation = segment.orientation;
        output += write_lines(move);
    }
    return output;
}

std::string DefaultGCodeFirmwareSession::write_special_command(
    const EPropertySpecialCommand &command,
    const RequestedState &state)
{
    apply_requested_state(state);
    switch (command.code) {
    case C_EXTRUSION_SPECIAL_COMMAND_TOOLCHANGE: {
        if (!std::isfinite(command.extra_data) || command.extra_data < 0.0 ||
            command.extra_data > double(std::numeric_limits<uint16_t>::max()))
            throw std::invalid_argument("A tool-change command contains an invalid tool id.");
        const uint16_t tool_id = uint16_t(std::lround(command.extra_data));
        const std::string output = select_extruder(tool_id);
        apply_requested_state(state);
        return output;
    }
    case C_EXTRUSION_SPECIAL_COMMAND_SAVE_AND_RESET_SPEED_RATIO:
        return encode_save_speed_ratio(command.extra_data);
    case C_EXTRUSION_SPECIAL_COMMAND_RESTORE_SPEED_RATIO:
        return encode_restore_speed_ratio();
    case C_EXTRUSION_SPECIAL_COMMAND_FLUSH_PLANNER_QUEUE:
        return encode_flush_planner();
    case C_EXTRUSION_SPECIAL_COMMAND_EXTRUSION:
    case C_EXTRUSION_SPECIAL_COMMAND_RETRACT: {
        DefaultExtruder &tool = current_extruder();
        const std::optional<double> inherited_speed = m_gantry.requested_speed();
        if (state.acceleration_mm_per_s2 && *state.acceleration_mm_per_s2 > 0.f)
            m_gantry.request_print_acceleration(
                uint32_t(std::lround(*state.acceleration_mm_per_s2)));
        if (command.code == C_EXTRUSION_SPECIAL_COMMAND_RETRACT) {
            const double speed = command.extra_data < 0.0 ?
                tool.extrusion_axis().retract_speed() : tool.extrusion_axis().deretract_speed();
            if (speed > 0.0)
                m_gantry.request_speed(speed);
        }
        PreparedMove move;
        move.kind = PreparedMove::Kind::Extrusion;
        move.extrusion = tool.extrusion_axis().extrude(command.extra_data);
        const std::string output = write_lines(move);
        m_gantry.request_speed(inherited_speed);
        return output;
    }
    case C_EXTRUSION_SPECIAL_COMMAND_PAUSE:
        return encode_pause(command.extra_data);
    case C_EXTRUSION_SPECIAL_COMMAND_WAIT_FOR_TEMP: {
        DefaultExtruder &tool = current_extruder();
        if (!tool.heater().requested_temperature())
            throw std::invalid_argument("A temperature wait command has no requested tool temperature.");
        if (!tool.heater().needs_wait_encoding())
            return {};
        const std::string output = encode_tool_temperature(
            tool.id(), tool.heater().effective_temperature(), true);
        tool.heater().mark_encoded_with_wait();
        return output;
    }
    case C_EXTRUSION_SPECIAL_COMMAND_DISABLE_PREVIEW:
        m_printer.set_preview_enabled(false);
        return {};
    case C_EXTRUSION_SPECIAL_COMMAND_ENABLE_PREVIEW:
        m_printer.set_preview_enabled(true);
        return {};
    case C_EXTRUSION_SPECIAL_COMMAND_EXTRUDER_CURRENT:
        return encode_extruder_current(current_extruder().id(), command.extra_data);
    }
    throw std::invalid_argument("An extrusion tree contains an unknown special command.");
}

std::string DefaultGCodeFirmwareSession::write_custom_gcode(
    const ExtrusionEntity &entity,
    const EPropertyCustomGcode &custom_gcode)
{
    return encode_custom_gcode(custom_gcode.kind, entity.stored_string(custom_gcode.text_id));
}

std::string DefaultGCodeFirmwareSession::write_acceleration(PreparedMove::Kind kind)
{
    // Print and travel have independent firmware commands in the standard
    // model. Encoding one category therefore leaves the acknowledgement of
    // the other category untouched.
    if (kind == PreparedMove::Kind::Travel &&
        m_gantry.needs_travel_acceleration_encoding()) {
        const uint32_t acceleration = *m_gantry.requested_travel_acceleration();
        std::string output = encode_acceleration(acceleration, true);
        m_gantry.mark_travel_acceleration_encoded();
        return output;
    }
    if (kind == PreparedMove::Kind::Extrusion &&
        m_gantry.needs_print_acceleration_encoding()) {
        const uint32_t acceleration = *m_gantry.requested_print_acceleration();
        std::string output = encode_acceleration(acceleration, false);
        m_gantry.mark_print_acceleration_encoded();
        return output;
    }
    return {};
}

std::string DefaultGCodeFirmwareSession::write_lines(const PreparedMove &prepared_move)
{
    std::string output;

    // State commands precede motion in a stable order so derived encoders can
    // reason about the machine state observed by the movement command.
    if (m_current_extruder_idx) {
        DefaultExtruder &tool = current_extruder();
        if (tool.heater().needs_encoding()) {
            output += encode_tool_temperature(
                tool.id(), tool.heater().effective_temperature(), false);
            tool.heater().mark_encoded();
        }
    }
    if (m_printer.bed_heater().needs_encoding()) {
        output += encode_bed_temperature(
            m_printer.bed_heater().effective_temperature(), false);
        m_printer.bed_heater().mark_encoded();
    }
    if (m_printer.chamber_heater().needs_encoding()) {
        output += encode_chamber_temperature(
            m_printer.chamber_heater().effective_temperature(), false);
        m_printer.chamber_heater().mark_encoded();
    }
    if (m_current_extruder_idx) {
        DefaultExtruder &tool = current_extruder();
        if (tool.fan().needs_encoding()) {
            output += encode_fan(tool.id(), tool.fan().effective_speed_percent());
            tool.fan().mark_encoded();
        }
        if (tool.pressure_advance().needs_encoding()) {
            output += encode_pressure_advance(
                tool.id(), *tool.pressure_advance().requested());
            tool.pressure_advance().mark_encoded();
        }
    }

    // Acceleration state ownership is delegated before movement encoding so a
    // firmware specialization may model either separate or shared registers.
    output += write_acceleration(prepared_move.kind);

    PreparedMove quantized_move = prepared_move;
    bool xyz_changed = false;
    if (quantized_move.destination) {
        quantized_move.destination = m_formatter->quantize(*quantized_move.destination);
        xyz_changed = !m_gantry.position() || coordinates_differ(*m_gantry.position(), *quantized_move.destination);
        if (!xyz_changed)
            quantized_move.destination.reset();
    }
    const bool e_changed = quantized_move.extrusion.has_value();

    // A sub-precision E delta still updates the extruder rounding remainder,
    // but no empty G-code line is produced until a later delta becomes visible.
    if (xyz_changed || e_changed) {
        if (!m_gantry.requested_speed() || *m_gantry.requested_speed() <= 0.0)
            throw std::invalid_argument("A movement requires a positive requested speed.");
        const bool include_speed = m_gantry.needs_speed_encoding();
        output += encode_move(quantized_move, include_speed);
        if (xyz_changed)
            m_gantry.set_position(*quantized_move.destination);
        if (include_speed)
            m_gantry.mark_speed_encoded();
    }
    return output;
}

std::string DefaultGCodeFirmwareSession::encode_tool_change(uint16_t tool_id) const
{
    return "T" + std::to_string(tool_id) + "\n";
}

std::string DefaultGCodeFirmwareSession::encode_tool_temperature(uint16_t tool_id,
                                                                  int16_t temperature,
                                                                  bool wait) const
{
    return std::string(wait ? "M109" : "M104") + " S" + std::to_string(temperature) +
           " T" + std::to_string(tool_id) + "\n";
}

std::string DefaultGCodeFirmwareSession::encode_bed_temperature(int16_t temperature,
                                                                bool wait) const
{
    return std::string(wait ? "M190" : "M140") + " S" + std::to_string(temperature) + "\n";
}

std::string DefaultGCodeFirmwareSession::encode_chamber_temperature(int16_t temperature,
                                                                    bool wait) const
{
    return std::string(wait ? "M191" : "M141") + " S" + std::to_string(temperature) + "\n";
}

std::string DefaultGCodeFirmwareSession::encode_fan(uint16_t, double speed_percent) const
{
    if (speed_percent <= 0.0)
        return "M107\n";
    const int32_t pwm = int32_t(std::lround(
        std::clamp(speed_percent, 0.0, 100.0) * 255.0 / 100.0));
    return "M106 S" + std::to_string(pwm) + "\n";
}

std::string DefaultGCodeFirmwareSession::encode_pressure_advance(
    uint16_t,
    double pressure_advance) const
{
    return "M900 K" + compact_number(pressure_advance) + "\n";
}

std::string DefaultGCodeFirmwareSession::encode_acceleration(uint32_t acceleration,
                                                             bool travel) const
{
    return std::string("M204 ") + (travel ? "T" : "P") + std::to_string(acceleration) + "\n";
}

std::string DefaultGCodeFirmwareSession::encode_move(const PreparedMove &move,
                                                     bool include_speed) const
{
    if (!m_formatter)
        throw std::logic_error("The G-code formatter is not initialized.");

    const bool arc = move.destination && move.radius_mm != 0.f;
    GCodeFormatter formatter(*m_formatter);
    if (arc) {
        if (move.arc_orientation == RAW_EXTRUSION_ARC_ORIENTATION_UNKNOWN)
            throw std::invalid_argument("An arc movement has no orientation.");
        formatter.emit_string(
            move.arc_orientation == RAW_EXTRUSION_ARC_ORIENTATION_CCW ? "G3" : "G2");
    } else {
        formatter.emit_string(move.kind == PreparedMove::Kind::Travel ? "G0" : "G1");
    }

    // Only axes that differ from the known physical position are emitted. If
    // position is unknown, all destination axes establish it explicitly.
    if (move.destination) {
        const std::optional<c_vec3d> current = m_gantry.position();
        if (!current || current->x != move.destination->x)
            formatter.emit_axis('X', move.destination->x, formatter.m_gcode_precision_xyz);
        if (!current || current->y != move.destination->y)
            formatter.emit_axis('Y', move.destination->y, formatter.m_gcode_precision_xyz);
        if (!current || current->z != move.destination->z)
            formatter.emit_axis('Z', move.destination->z, formatter.m_gcode_precision_xyz);
        if (arc)
            formatter.emit_axis('R', move.radius_mm, formatter.m_gcode_precision_xyz);
    }
    if (move.extrusion)
        formatter.emit_axis('E', *move.extrusion, formatter.m_gcode_precision_e);
    if (include_speed)
        formatter.emit_f(*m_gantry.requested_speed() * 60.0);
    return formatter.string();
}

std::string DefaultGCodeFirmwareSession::encode_custom_gcode(
    c_extrusion_custom_gcode_kind kind,
    const std::string &text) const
{
    std::string output;
    if (kind == C_EXTRUSION_CUSTOM_GCODE_COMMENT) {
        // Prefix every logical line so a multiline comment cannot accidentally
        // turn its second line into an executable machine command.
        size_t begin = 0;
        while (begin <= text.size()) {
            const size_t end = text.find('\n', begin);
            output += "; ";
            output.append(text, begin, end == std::string::npos ? std::string::npos : end - begin);
            output += '\n';
            if (end == std::string::npos)
                break;
            begin = end + 1;
            if (begin == text.size())
                break;
        }
        return output;
    }

    output = text;
    if (!output.empty() && output.back() != '\n')
        output += '\n';
    return output;
}

std::string DefaultGCodeFirmwareSession::encode_save_speed_ratio(double ratio) const
{
    if (!std::isfinite(ratio) || ratio < 0.0)
        throw std::invalid_argument("A speed-ratio command contains an invalid ratio.");
    return "M220 B\nM220 S" + compact_number(ratio * 100.0, 2) + "\n";
}

std::string DefaultGCodeFirmwareSession::encode_restore_speed_ratio() const
{
    return "M220 R\n";
}

std::string DefaultGCodeFirmwareSession::encode_flush_planner() const
{
    return "G4 S0\n";
}

std::string DefaultGCodeFirmwareSession::encode_pause(double milliseconds) const
{
    if (!std::isfinite(milliseconds) || milliseconds < 0.0)
        throw std::invalid_argument("A pause command contains an invalid duration.");
    return "G4 P" + compact_number(milliseconds, 3) + "\n";
}

std::string DefaultGCodeFirmwareSession::encode_extruder_current(uint16_t tool_id,
                                                                 double current) const
{
    if (!std::isfinite(current) || current < 0.0)
        throw std::invalid_argument("An extruder-current command contains an invalid value.");
    return "M906 T" + std::to_string(tool_id) + " E" + compact_number(current, 3) + "\n";
}

}} // namespace slic3r_api::GCodeGeneration
