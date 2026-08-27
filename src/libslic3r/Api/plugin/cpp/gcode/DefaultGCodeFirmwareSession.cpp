///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "DefaultGCodeFirmwareSession.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>

#include "libslic3r/Api/plugin/cpp/ExtrusionTreeVisitors.hpp"
#include "libslic3r/ExtrusionRole.hpp"
#include "libslic3r/GCodeReader.hpp"

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

DefaultGCodeFirmwareSession::DefaultGCodeFirmwareSession(
    GCodeScriptProcessorView scripts,
    storage_handle *storage) :
    m_scripts(scripts)
{
    if (storage != nullptr)
        m_script_config.emplace(storage);
}
using Slic3r::Axis;
using Slic3r::GCodeReader;

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

bool command_code(std::string_view command, char prefix, int32_t *code_out)
{
    if (code_out == nullptr || command.size() < 2 || command.front() != prefix)
        return false;
    const char *begin = command.data() + 1;
    const char *end = command.data() + command.size();
    const std::from_chars_result parsed = std::from_chars(begin, end, *code_out);
    return parsed.ec == std::errc() && parsed.ptr == end;
}

// Return true when this event tree contains a non-empty toolchange script.
// Its target is reconstructed from the final tool-group boundary at runtime.
bool has_toolchange_script(const ExtrusionEntity &root)
{
    const EPropertyCustomGcode *custom_gcode = root.property<EPropertyCustomGcode>();
    if (custom_gcode != nullptr &&
        custom_gcode->kind == C_EXTRUSION_CUSTOM_GCODE_SCRIPT &&
        custom_gcode->script_type == GCODE_SCRIPT_TYPE_TOOLCHANGE_GCODE &&
        root.stored_string(custom_gcode->text_id).find_first_not_of(" \t\r\n") != std::string::npos)
        return true;

    for (uint32_t child_idx = 0; child_idx < root.child_count(); ++child_idx)
        if (has_toolchange_script(root.child(child_idx)))
            return true;
    return false;
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

/*
Observe state-changing commands emitted by a resolved user script.

The interpreter never rewrites the text and ignores commands it does not know.
Its only job is to make the next host-generated movement start from the state
the printer will actually have after executing that text.
*/
class DefaultGCodeFirmwareSession::GCodeStateInterpreter
{
public:
    explicit GCodeStateInterpreter(DefaultGCodeFirmwareSession &session) : m_session(session) {}

    void apply(const std::string &gcode)
    {
        GCodeReader reader;
        reader.parse_buffer(gcode, [this](GCodeReader &, const GCodeReader::GCodeLine &line) {
            apply_line(line);
        });
    }

private:
    void apply_line(const GCodeReader::GCodeLine &line)
    {
        const std::string_view command = line.cmd();
        int32_t code = 0;
        if (command_code(command, 'T', &code)) {
            if (code >= 0 && code <= int32_t(std::numeric_limits<uint16_t>::max()))
                (void)m_session.select_extruder(uint16_t(code), false);
            return;
        }

        if (command_code(command, 'G', &code)) {
            if (code == 20) {
                m_session.m_units_in_mm = false;
                return;
            }
            if (code == 21) {
                m_session.m_units_in_mm = true;
                return;
            }
            if (code == 90) {
                m_session.m_xyz_relative_mode = false;
                return;
            }
            if (code == 91) {
                m_session.m_xyz_relative_mode = true;
                return;
            }
            if (code == 92) {
                apply_coordinate_reset(line);
                return;
            }
            if (code >= 0 && code <= 3)
                apply_move(line);
            return;
        }

        if (!command_code(command, 'M', &code))
            return;
        if (code == 82 || code == 83) {
            m_session.m_e_relative_mode = code == 83;
            for (DefaultExtruder &tool : m_session.m_extruders)
                tool.extrusion_axis().set_relative_mode(m_session.m_e_relative_mode);
            return;
        }
        if (code == 104 || code == 109) {
            apply_tool_temperature(line, code == 109);
            return;
        }
        if (code == 140 || code == 190) {
            apply_heater(line, m_session.m_printer.bed_heater(), code == 190);
            return;
        }
        if (code == 141 || code == 191) {
            apply_heater(line, m_session.m_printer.chamber_heater(), code == 191);
            return;
        }
        if (code == 106 || code == 107)
            apply_fan(line, code == 107);
    }

    double unit_scale() const { return m_session.m_units_in_mm ? 1.0 : 25.4; }

    std::optional<uint16_t> addressed_tool(const GCodeReader::GCodeLine &line, char selector) const
    {
        int32_t tool_id = -1;
        if (line.has_value(selector, tool_id)) {
            if (tool_id < 0 || tool_id >= int32_t(m_session.m_extruders.size()))
                return std::nullopt;
            return uint16_t(tool_id);
        }
        if (m_session.m_current_extruder_idx)
            return m_session.m_current_extruder_idx;
        return m_session.m_script_processing_tool;
    }

    void apply_coordinate_reset(const GCodeReader::GCodeLine &line)
    {
        const double scale = unit_scale();
        if (line.has(Axis::E)) {
            const std::optional<uint16_t> tool_id = addressed_tool(line, 'T');
            if (tool_id)
                m_session.extruder(*tool_id).extrusion_axis().set_position(line.value(Axis::E) * scale);
        }

        const std::optional<c_vec3d> old_position = m_session.m_gantry.position();
        if (!old_position && !(line.has(Axis::X) && line.has(Axis::Y) && line.has(Axis::Z)))
            return;
        c_vec3d position = old_position.value_or(c_vec3d{});
        if (line.has(Axis::X)) position.x = line.value(Axis::X) * scale;
        if (line.has(Axis::Y)) position.y = line.value(Axis::Y) * scale;
        if (line.has(Axis::Z)) position.z = line.value(Axis::Z) * scale;
        m_session.m_gantry.set_position(position);
    }

    void apply_move(const GCodeReader::GCodeLine &line)
    {
        const double scale = unit_scale();
        if (line.has(Axis::F)) {
            m_session.m_gantry.request_speed(line.value(Axis::F) * scale / 60.0);
            m_session.m_gantry.mark_speed_encoded();
        }

        const std::optional<c_vec3d> old_position = m_session.m_gantry.position();
        if (old_position || (line.has(Axis::X) && line.has(Axis::Y) && line.has(Axis::Z))) {
            c_vec3d position = old_position.value_or(c_vec3d{});
            if (line.has(Axis::X))
                position.x = m_session.m_xyz_relative_mode ? position.x + line.value(Axis::X) * scale :
                                                            line.value(Axis::X) * scale;
            if (line.has(Axis::Y))
                position.y = m_session.m_xyz_relative_mode ? position.y + line.value(Axis::Y) * scale :
                                                            line.value(Axis::Y) * scale;
            if (line.has(Axis::Z))
                position.z = m_session.m_xyz_relative_mode ? position.z + line.value(Axis::Z) * scale :
                                                            line.value(Axis::Z) * scale;
            m_session.m_gantry.set_position(position);
        }

        if (line.has(Axis::E)) {
            const std::optional<uint16_t> tool_id = addressed_tool(line, 'T');
            if (tool_id)
                m_session.extruder(*tool_id).extrusion_axis().observe_external_move(
                    line.value(Axis::E) * scale, m_session.m_e_relative_mode);
        }
    }

    void apply_heater(const GCodeReader::GCodeLine &line, HeaterState &heater, bool waited)
    {
        float temperature = 0.f;
        const bool has_temperature = line.has_value('S', temperature) ||
                                     (waited && line.has_value('R', temperature));
        if (has_temperature && std::isfinite(temperature) &&
            temperature >= double(std::numeric_limits<int16_t>::min()) &&
            temperature <= double(std::numeric_limits<int16_t>::max()))
            heater.synchronize_after_external_gcode(int16_t(std::lround(temperature)), waited);
    }

    void apply_tool_temperature(const GCodeReader::GCodeLine &line, bool waited)
    {
        const std::optional<uint16_t> tool_id = addressed_tool(line, 'T');
        if (tool_id)
            apply_heater(line, m_session.extruder(*tool_id).heater(), waited);
    }

    void apply_fan(const GCodeReader::GCodeLine &line, bool stopped)
    {
        const std::optional<uint16_t> tool_id = addressed_tool(line, 'P');
        if (!tool_id)
            return;
        float pwm = stopped ? 0.f : 255.f;
        if (!stopped)
            (void)line.has_value('S', pwm);
        m_session.extruder(*tool_id).fan().synchronize_after_external_gcode(
            std::clamp(double(pwm) * 100.0 / 255.0, 0.0, 100.0));
    }

    DefaultGCodeFirmwareSession &m_session;
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
        if (entity.segment_count() > 0) {
            m_output += m_session.write_leaf_geometry(entity, m_state);
        }
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
    m_last_layer_used_filament.assign(m_extruders.size(), 0.0);
    m_script_processing_tool.reset();
    m_xyz_relative_mode = false;
    m_units_in_mm = true;
    m_e_relative_mode = config.bool_or_default("use_relative_e_distances", false);
    m_seen_object_group = false;
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

std::string DefaultGCodeFirmwareSession::begin_group(const PrintingGroup &group)
{
    if (!m_is_setup)
        throw std::logic_error("The firmware session was not initialized by begin_print().");
    // Ordering represents a sequential object as a group with exactly one
    // source instance. Auxiliary groups carry none, while layer-wise plans may
    // carry several. Skipping both keeps ordinary first-path travel unchanged.
    if (group.object_instance_count() != 1)
        return {};
    if (!m_seen_object_group) {
        m_seen_object_group = true;
        return {};
    }
    return move_to_group_start(group);
}

std::string DefaultGCodeFirmwareSession::begin_layer(const PrintingLayerGroup &layer)
{
    if (!m_is_setup)
        throw std::logic_error("The firmware session was not initialized by begin_print().");
    m_layer_print_z = layer.print_z();
    if (!m_current_extruder_idx || !m_gantry.position())
        return {};

    const c_vec3d current = *m_gantry.position();
    const double target_z = unscaled(m_layer_print_z) + m_gantry.z_offset() - current_extruder().z_offset();
    if (current.z == target_z)
        return {};
    const std::optional<double> inherited_speed = m_gantry.requested_speed();
    m_gantry.request_speed(m_gantry.travel_speed());
    PreparedMove move;
    move.kind = PreparedMove::Kind::Travel;
    move.destination = c_vec3d{current.x, current.y, target_z};
    const std::string output = write_lines(move);
    m_gantry.request_speed(inherited_speed);
    return output;
}

std::string DefaultGCodeFirmwareSession::begin_tool_group(const PrintingToolGroup &tool_group)
{
    if (!m_is_setup)
        throw std::logic_error("The firmware session was not initialized by begin_print().");
    const bool custom_toolchange = has_toolchange_script(tool_group.events().before());
    const std::string output = select_extruder(tool_group.extruder_id(), !custom_toolchange);
    return output;
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

std::string DefaultGCodeFirmwareSession::select_extruder(uint16_t tool_id, bool emit_command)
{
    DefaultExtruder &selected = extruder(tool_id);
    if (m_current_extruder_idx && *m_current_extruder_idx == tool_id)
        return {};

    DefaultExtruder *previous = m_current_extruder_idx ?
        &extruder(*m_current_extruder_idx) : nullptr;

    // Generate the tool command before committing the selected index. A
    // derived encoder may throw without leaving the session on a fictive tool.
    const std::string output = emit_command ? encode_tool_change(selected.id()) : std::string();
    synchronize_selected_extruder_state(previous, selected);
    m_current_extruder_idx = tool_id;
    return output;
}

std::string DefaultGCodeFirmwareSession::move_to_group_start(const PrintingGroup &group)
{
    // The caller has already established that this is a transition between
    // real object groups. A missing tool or position still makes the first
    // regular path responsible for establishing the machine position.
    if (!m_current_extruder_idx || !m_gantry.position())
        return {};
    for (uint32_t layer_idx = 0; layer_idx < group.layer_group_count(); ++layer_idx) {
        const PrintingLayerGroup layer = group.layer_group(layer_idx);
        for (uint32_t tool_idx = 0; tool_idx < layer.tool_group_count(); ++tool_idx) {
            const PrintingToolGroup tool_group = layer.tool_group(tool_idx);
            for (uint32_t extrusion_idx = 0; extrusion_idx < tool_group.extrusion_count(); ++extrusion_idx) {
                const ExtrusionEntity root = tool_group.extrusion(extrusion_idx).root();
                if (root.empty())
                    continue;
                const c_point first_point = root.front();
                const c_vec3d destination = {
                    unscaled(first_point.x) - current_extruder().xy_offset().x,
                    unscaled(first_point.y) - current_extruder().xy_offset().y,
                    m_gantry.position()->z
                };
                const std::optional<double> inherited_speed = m_gantry.requested_speed();
                m_gantry.request_speed(m_gantry.travel_speed());
                PreparedMove move;
                move.kind = PreparedMove::Kind::Travel;
                move.destination = destination;
                const std::string output = write_lines(move);
                m_gantry.request_speed(inherited_speed);
                return output;
            }
        }
    }
    return {};
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
    const std::string text = entity.stored_string(custom_gcode.text_id);
    if (custom_gcode.kind == C_EXTRUSION_CUSTOM_GCODE_SCRIPT) {
        if (custom_gcode.script_type == GCODE_SCRIPT_TYPE_INVALID)
            throw std::invalid_argument("A custom G-code script has no script type.");

        const Config *producer_config = nullptr;
        if (custom_gcode.config_id != EXTRUSION_DATA_ID_INVALID) {
            if (!m_script_config)
                throw std::logic_error(
                    "The firmware session has no storage for a scripted G-code Config.");

            // The property owns a complete NUL-terminated SCFG snapshot. Rebuild
            // it in reusable plugin storage immediately before host preparation.
            uint32_t byte_size = 0;
            const char *serialized = static_cast<const char *>(
                entity.stored_data(custom_gcode.config_id, &byte_size));
            if (serialized == nullptr || byte_size == 0 || serialized[byte_size - 1] != '\0')
                throw std::invalid_argument("A custom G-code script has an invalid stored Config.");
            m_script_config->clear();
            m_script_config->deserialize_all(std::string(serialized, serialized + byte_size - 1));
            producer_config = &*m_script_config;
        }
        std::string output = process_script(
            custom_gcode.script_type, text, producer_config, custom_gcode.processing_extruder_id);
        if (!output.empty() && output.back() != '\n')
            output += '\n';
        return output;
    }
    if (custom_gcode.script_type != GCODE_SCRIPT_TYPE_INVALID ||
        custom_gcode.config_id != EXTRUSION_DATA_ID_INVALID ||
        custom_gcode.processing_extruder_id != GCODE_SCRIPT_PROCESSING_EXTRUDER_INVALID)
        throw std::invalid_argument("Raw G-code and comments cannot carry script metadata.");
    return encode_custom_gcode(custom_gcode.kind, text);
}

std::string DefaultGCodeFirmwareSession::process_script(
    gcode_script_type script_type,
    const std::string &script,
    const Config *producer_config,
    uint16_t processing_extruder_id)
{
    if (!m_scripts.valid())
        throw std::invalid_argument(
            "Custom G-code scripts require a host script processor.");

    const std::string resolved_script = script.empty() ?
        resolve_empty_script(script_type, producer_config) : script;

    // The script producer has already frozen every structural placeholder in
    // its Config. The firmware adds only values that depend on the machine at
    // this exact execution point.
    GCodeScriptContext context = producer_config == nullptr ?
        m_scripts.prepare(script_type) : m_scripts.prepare(script_type, *producer_config);
    GCodeScriptConfig config = context.config();

    uint16_t processing_tool = m_current_extruder_idx.value_or(0);
    if (processing_extruder_id != GCODE_SCRIPT_PROCESSING_EXTRUDER_INVALID)
        processing_tool = processing_extruder_id;
    if (processing_tool >= m_extruders.size())
        throw std::invalid_argument("A custom G-code script references an unknown processing extruder.");
    complete_script_context(script_type, processing_tool, config);

    std::vector<double> used_filament_snapshot;
    if (script_type == GCODE_SCRIPT_TYPE_BEFORE_LAYER_GCODE &&
        config.has("layer_used_filament")) {
        used_filament_snapshot.reserve(m_extruders.size());
        std::vector<double> layer_used_filament;
        layer_used_filament.reserve(m_extruders.size());
        for (size_t tool_idx = 0; tool_idx < m_extruders.size(); ++tool_idx) {
            const double used = m_extruders[tool_idx].extrusion_axis().used_filament();
            used_filament_snapshot.push_back(used);
            layer_used_filament.push_back(used - m_last_layer_used_filament[tool_idx]);
        }
        config.set("layer_used_filament", layer_used_filament);
    }
    if (config.has("gcode_bed_temperature") && m_printer.bed_heater().requested_temperature())
        config.set("gcode_bed_temperature", int32_t(m_printer.bed_heater().effective_temperature()));

    // Export the complete current machine snapshot before the parser runs.
    // An unknown position uses a stable placeholder but remains unknown unless
    // the script or the generated G-code actually changes that vector.
    const std::optional<c_vec3d> old_position = m_gantry.position();
    const std::vector<double> position = old_position ?
        std::vector<double>{old_position->x, old_position->y, old_position->z} :
        std::vector<double>{0.0, 0.0, 0.0};
    config.set("position", position);

    std::vector<double> old_e_positions;
    std::vector<double> old_retracted;
    std::vector<double> old_restart_extra;
    old_e_positions.reserve(m_extruders.size());
    old_retracted.reserve(m_extruders.size());
    old_restart_extra.reserve(m_extruders.size());
    for (const DefaultExtruder &tool : m_extruders) {
        old_e_positions.push_back(tool.extrusion_axis().position());
        old_retracted.push_back(tool.extrusion_axis().retracted());
        old_restart_extra.push_back(tool.extrusion_axis().restart_extra());
    }
    config.set("e_retracted", old_retracted);
    config.set("e_restart_extra", old_restart_extra);
    if (config.has("e_position"))
        config.set("e_position", old_e_positions);

    m_script_processing_tool = processing_tool;
    std::string output;
    try {
        output = context.process(resolved_script, processing_tool);
        GCodeStateInterpreter(*this).apply(output);
        m_script_processing_tool.reset();
    } catch (...) {
        m_script_processing_tool.reset();
        throw;
    }

    // Read and validate every output before mutating any session component.
    // This keeps script application atomic even when the last vector is bad.
    const std::vector<double> new_position = config.get_floats("position");
    const std::vector<double> new_retracted = config.get_floats("e_retracted");
    const std::vector<double> new_restart_extra = config.get_floats("e_restart_extra");
    const std::vector<double> new_e_positions = config.has("e_position") ?
        config.get_floats("e_position") : std::vector<double>();
    if (new_position.size() != 3 || new_retracted.size() != m_extruders.size() ||
        new_restart_extra.size() != m_extruders.size() ||
        (!new_e_positions.empty() && new_e_positions.size() != m_extruders.size()))
        throw std::invalid_argument("A G-code script returned an invalid machine-state vector size.");

    for (double value : new_position) {
        if (!std::isfinite(value))
            throw std::invalid_argument("A G-code script returned a non-finite position.");
    }
    for (size_t tool_idx = 0; tool_idx < m_extruders.size(); ++tool_idx) {
        if (!std::isfinite(new_retracted[tool_idx]) ||
            !std::isfinite(new_restart_extra[tool_idx]) ||
            new_retracted[tool_idx] < -EPSILON || new_restart_extra[tool_idx] < -EPSILON ||
            (!new_e_positions.empty() && !std::isfinite(new_e_positions[tool_idx])))
            throw std::invalid_argument("A G-code script returned an invalid extrusion state.");
    }

    // A script which leaves the placeholder position untouched must not turn
    // an unknown Gantry position into an invented origin.
    if (new_position != position)
        m_gantry.set_position(c_vec3d{new_position[0], new_position[1], new_position[2]});
    for (size_t tool_idx = 0; tool_idx < m_extruders.size(); ++tool_idx) {
        const bool e_position_changed = !new_e_positions.empty() &&
                                        new_e_positions[tool_idx] != old_e_positions[tool_idx];
        const bool retraction_changed = new_retracted[tool_idx] != old_retracted[tool_idx] ||
                                        new_restart_extra[tool_idx] != old_restart_extra[tool_idx];
        if (e_position_changed || retraction_changed) {
            const std::optional<double> e_position = e_position_changed ?
                std::optional<double>(new_e_positions[tool_idx]) : std::optional<double>();
            m_extruders[tool_idx].extrusion_axis().synchronize_after_external_gcode(
                e_position, new_retracted[tool_idx], new_restart_extra[tool_idx]);
        }
    }
    if (!used_filament_snapshot.empty())
        m_last_layer_used_filament = std::move(used_filament_snapshot);
    // Event chunks are concatenated directly by the plan writer. Collapse all
    // trailing line breaks to one so an empty line cannot accumulate at every
    // scope boundary and the following command can never join this script.
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
        output.pop_back();
    if (!output.empty())
        output += '\n';
    return output;
}

std::string DefaultGCodeFirmwareSession::resolve_empty_script(
    gcode_script_type,
    const Config *) const
{
    return {};
}

void DefaultGCodeFirmwareSession::complete_script_context(
    gcode_script_type,
    uint16_t,
    GCodeScriptConfig &)
{
    // The standard script types are fully described by the final plan and the
    // generic machine state. Derived firmware sessions may fill extra options
    // declared by a custom script type.
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

    // Host-generated coordinates are always absolute millimetres. Scripts may
    // temporarily select inches or relative XYZ; restore the neutral mode only
    // when a generated movement actually needs it, leaving adjacent scripts to
    // observe the exact state established by their predecessor.
    if ((prepared_move.destination || prepared_move.extrusion) && !m_units_in_mm) {
        output += "G21\n";
        m_units_in_mm = true;
    }
    if (prepared_move.destination && m_xyz_relative_mode) {
        output += "G90\n";
        m_xyz_relative_mode = false;
    }

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
    switch (kind) {
    case C_EXTRUSION_CUSTOM_GCODE_COMMENT: {
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
    case C_EXTRUSION_CUSTOM_GCODE_GCODE:
        output = text;
        if (!output.empty() && output.back() != '\n')
            output += '\n';
        return output;
    case C_EXTRUSION_CUSTOM_GCODE_SCRIPT:
        // SCRIPT is routed through process_script() before syntax encoders are
        // entered. Direct calls cannot safely bypass the machine-state import.
        throw std::invalid_argument(
            "Custom G-code scripts must be processed through the host script processor.");
    default:
        throw std::invalid_argument("A custom G-code property contains an unknown kind.");
    }
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
