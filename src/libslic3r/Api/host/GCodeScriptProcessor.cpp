///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "GCodeScriptProcessor.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <map>
#include <optional>
#include <random>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <fast_float/fast_float.h>

#include "ApiHostUtils.hpp"
#include "Orchestrator.hpp"
#include "libslic3r/Config/ConfigDef.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PlaceholderParser.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Config/PrintConfig.hpp"
#include "libslic3r/PrintObject.hpp"

/*
Host G-code script processor implementation
===========================================

Every prepare() creates a fresh public Config. process() then splits that
Config into read-only script inputs and writable machine outputs before it
enters PlaceholderParser. Outputs are validated in temporary storage and are
published back only after the complete script succeeds.
*/

namespace Slic3r {
namespace {

const char *const k_machine_option_keys[] = {
    "position",
    "e_retracted",
    "e_restart_extra",
    "e_position"
};

void copy_option(DynamicConfig &destination,
                 const DynamicConfig &source,
                 const std::string &key);
void validate_finite_vector(const DynamicConfig &config,
                            const char *key,
                            size_t expected_size);
std::array<std::optional<double>, 3> position_after_gcode(
    const std::vector<double> &initial_position,
    const std::string &gcode);
bool is_machine_option_key(const std::string &key);

void copy_option(DynamicConfig &destination,
                 const DynamicConfig &source,
                 const std::string &key)
{
    const ConfigOption *option = source.option(key);
    if (option == nullptr)
        throw std::invalid_argument("Missing prepared G-code script option: " + key);
    destination.set_key_value(key, option->clone());
}

void validate_finite_vector(const DynamicConfig &config,
                            const char *key,
                            size_t expected_size)
{
    const ConfigOptionFloats *option = config.option<ConfigOptionFloats>(key);
    if (option == nullptr || option->size() != expected_size)
        throw std::invalid_argument(
            std::string("G-code script output variable has an invalid size: ") + key);
    for (double value : option->get_values()) {
        if (!std::isfinite(value))
            throw std::invalid_argument(
                std::string("G-code script output variable contains a non-finite value: ") + key);
    }
}

std::array<std::optional<double>, 3> position_after_gcode(
    const std::vector<double> &initial_position,
    const std::string &gcode)
{
    std::array<std::optional<double>, 3> position = {
        initial_position[0], initial_position[1], initial_position[2]
    };
    if (gcode.empty())
        return position;

    // Match the legacy parser's conservative policy: ordinary G1/G2/G3 moves
    // update mentioned axes, while an unrecognized command makes the inferred
    // position unusable instead of guessing what the script did.
    GCodeReader parser;
    parser.parse_buffer(gcode, [&position](GCodeReader &, const GCodeReader::GCodeLine &line) {
        const std::string_view command = line.cmd();
        if (command.empty() || (command[0] == 'T' && command.size() == 1))
            return;
        if (command[0] != 'G' && command[0] != 'M') {
            position = {};
            return;
        }

        double code = 0.0;
        const char *begin = command.data() + 1;
        const char *end = command.data() + command.size();
        const fast_float::from_chars_result parsed = fast_float::from_chars(begin, end, code);
        if (parsed.ptr != end) {
            position = {};
            return;
        }
        // Relative/unit changes need the firmware's persistent modal state.
        // The standalone processor deliberately stops inferring instead of
        // treating a following coordinate as an absolute millimetre value.
        if (command[0] == 'G' && (code == 20.0 || code == 21.0 || code == 90.0 || code == 91.0 || code == 92.0)) {
            position = {};
            return;
        }
        if (command[0] != 'G' || code < 0.0 || code > 3.0)
            return;
        if (line.has(Axis::X))
            position[0] = line.value(Axis::X);
        if (line.has(Axis::Y))
            position[1] = line.value(Axis::Y);
        if (line.has(Axis::Z))
            position[2] = line.value(Axis::Z);
    });
    return position;
}

// Machine values are supplied by the firmware at execution time and may not be frozen by a script producer.
bool is_machine_option_key(const std::string &key)
{
    for (const char *machine_key : k_machine_option_keys)
        if (key == machine_key)
            return true;
    return false;
}

} // namespace

class GCodeScriptProcessor::Impl
{
public:
    Impl(const Print &print, const Orchestrator &orchestrator);

    const raw_gcode_script_processor *c_processor() const { return &m_c_processor; }

private:
    static config_handle *prepare_thunk(void *context,
                                        gcode_script_type script_type,
                                        const config_handle *producer_config) noexcept;
    static void process_thunk(void *context,
                              const char *script,
                              uint16_t current_extruder,
                              raw_gcode_script_result *result) noexcept;

    config_handle *prepare(gcode_script_type script_type,
                           const ConfigBase *producer_config);
    std::string process(const std::string &script, uint16_t current_extruder);
    void create_machine_options();
    void create_specific_options(const std::string &script_name);
    void merge_producer_config(const ConfigBase *producer_config);
    void resolve_object_names();
    void validate_outputs(const DynamicConfig &outputs) const;
    void infer_position_from_gcode(const DynamicConfig &before,
                                   DynamicConfig &outputs,
                                   const std::string &gcode) const;
    void publish_outputs(const DynamicConfig &outputs);

    PlaceholderParser m_parser;
    PlaceholderParser::ContextData m_context;
    DynamicConfig m_public_config;
    std::vector<std::string> m_specific_keys;
    size_t m_extruder_count = 0;
    bool m_absolute_e = false;
    bool m_prepared = false;
    bool m_processing = false;
    const Orchestrator &m_orchestrator;
    int32_t m_first_layer_bed_temperature = 0;
    std::vector<std::string> m_object_names;
    std::string m_output;
    std::string m_error;
    raw_gcode_script_processor m_c_processor = {};
};

GCodeScriptProcessor::Impl::Impl(const Print &print,
                                 const Orchestrator &orchestrator)
    : m_parser(print.placeholder_parser().external_config())
    , m_extruder_count(
          std::max<size_t>({size_t(1), print.config().nozzle_diameter.size(), print.config().filament_diameter.size(),
                            print.config().extruder_offset.size(), print.config().extrusion_multiplier.size()}))
    , m_absolute_e(!print.config().use_relative_e_distances.value)
    , m_orchestrator(orchestrator)
    , m_first_layer_bed_temperature(print.first_layer_bed_temperature()) {
    // Reproduce the persistent part of the legacy export context once. The
    // parser, random generator and global variables then remain shared by all
    // scripts handled during this export only.
    const DynamicConfig &source_config = print.placeholder_parser().config();
    for (const std::string &key : source_config.keys()) {
        const ConfigOption *option = source_config.option(key);
        // Compact tests may retain empty vector placeholders. Cloning those
        // trips the vector invariant and contributes no usable script value.
        if (option != nullptr && (!option->is_vector() || option->size() != 0))
            m_parser.set(key, option->clone());
    }
    m_parser.update_timestamp();
    m_context.rng = std::mt19937(
        static_cast<uint32_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    m_context.global_config.reset(new DynamicConfig());
    // update_object_placeholders() constructs a non-empty vector placeholder
    // in normal exports. An intentionally empty PrintingPlan test has no model
    // object and therefore has no object placeholder to add.
    if (!print.model().objects().empty())
        print.update_object_placeholders(m_parser.config_writable(), ".gcode");
    m_parser.parse_custom_variables(print.config().print_custom_variables);
    m_parser.parse_custom_variables(print.config().printer_custom_variables);
    // Focused pipeline tests may omit every filament preset and therefore
    // expose an empty vector. There is no custom variable to import in that
    // case, and ConfigOptionVector::get_at(0) would be invalid.
    if (!print.config().filament_custom_variables.empty())
        m_parser.parse_custom_variables(print.config().filament_custom_variables);
    m_parser.apply_config(print.physical_printer_config());

    // Object script contexts store compact indices. Copy sanitized labels now
    // so processing later never retains model pointers from the Print view.
    const std::regex invalid_name("[^\\w]+", std::regex_constants::ECMAScript);
    m_object_names.reserve(print.objects().size());
    for (const PrintObject &object : print.objects())
        m_object_names.emplace_back(std::regex_replace(object.model_object()->name, invalid_name, std::string("_")));

    m_c_processor.struct_size = sizeof(m_c_processor);
    m_c_processor.context = this;
    m_c_processor.prepare = &prepare_thunk;
    m_c_processor.process = &process_thunk;
}

config_handle *GCodeScriptProcessor::Impl::prepare_thunk(
    void *context,
    gcode_script_type script_type,
    const config_handle *producer_config) noexcept
{
    if (context == nullptr || script_type == GCODE_SCRIPT_TYPE_INVALID)
        return nullptr;
    Impl *self = static_cast<Impl *>(context);
    try {
        return self->prepare(
            script_type,
            producer_config == nullptr ? nullptr : ApiHost::to_config(producer_config));
    } catch (const std::exception &exception) {
        self->m_error = exception.what();
    } catch (...) {
        self->m_error = "Unknown error while preparing a G-code script context.";
    }
    self->m_prepared = false;
    return nullptr;
}

void GCodeScriptProcessor::Impl::process_thunk(
    void *context,
    const char *script,
    uint16_t current_extruder,
    raw_gcode_script_result *result) noexcept
{
    if (result == nullptr || result->struct_size < sizeof(raw_gcode_script_result))
        return;

    Impl *self = static_cast<Impl *>(context);
    result->status = RAW_GCODE_SCRIPT_STATUS_UNSET;
    result->text = nullptr;
    result->text_size = 0;
    result->error_message = nullptr;
    if (self == nullptr || script == nullptr) {
        result->status = RAW_GCODE_SCRIPT_STATUS_INVALID_ARGUMENT;
        return;
    }

    try {
        self->m_error.clear();
        self->m_output = self->process(script, current_extruder);
        result->status = RAW_GCODE_SCRIPT_STATUS_SUCCESS;
        result->text = self->m_output.data();
        result->text_size = static_cast<uint64_t>(self->m_output.size());
    } catch (const std::invalid_argument &exception) {
        self->m_output.clear();
        self->m_error = exception.what();
        result->status = RAW_GCODE_SCRIPT_STATUS_INVALID_ARGUMENT;
        result->error_message = self->m_error.c_str();
    } catch (const std::exception &exception) {
        self->m_output.clear();
        self->m_error = exception.what();
        result->status = RAW_GCODE_SCRIPT_STATUS_ERROR;
        result->error_message = self->m_error.c_str();
    } catch (...) {
        self->m_output.clear();
        self->m_error = "Unknown error while processing a G-code script.";
        result->status = RAW_GCODE_SCRIPT_STATUS_ERROR;
        result->error_message = self->m_error.c_str();
    }
}

config_handle *GCodeScriptProcessor::Impl::prepare(
    gcode_script_type script_type,
    const ConfigBase *producer_config)
{
    if (m_processing)
        throw std::logic_error("The G-code script processor is not reentrant.");

    // A fresh Config prevents values supplied for one script kind from leaking
    // into the next call, while parser globals intentionally stay persistent.
    m_public_config.clear();
    m_specific_keys.clear();
    m_output.clear();
    m_error.clear();
    const char *script_name = gcode_script_type_name(reinterpret_cast<const orchestrator_handle *>(&m_orchestrator),
                                                     script_type);
    if (script_name == nullptr)
        throw std::invalid_argument("Unknown G-code script type.");
    create_machine_options();
    create_specific_options(script_name);
    merge_producer_config(producer_config);
    if (script_type == GCODE_SCRIPT_TYPE_START_GCODE)
        m_public_config.option<ConfigOptionInt>("start_gcode_bed_temperature")->value = m_first_layer_bed_temperature;
    if (script_type == GCODE_SCRIPT_TYPE_BEFORE_LAYER_GCODE || script_type == GCODE_SCRIPT_TYPE_LAYER_GCODE)
        m_public_config.option<ConfigOptionInt>("gcode_bed_temperature")->value = m_first_layer_bed_temperature;
    m_prepared = true;
    return ApiHost::to_config_handle(&m_public_config);
}

void GCodeScriptProcessor::Impl::resolve_object_names()
{
    // Object IDs are inexpensive scalar ABI values. Resolve their display
    // names inside the host so plugins never need access to Model objects or
    // the PlaceholderParser naming rules.
    const char *const id_keys[] = {"previous_object_id", "next_object_id"};
    const char *const name_keys[] = {"previous_object_name", "next_object_name"};
    for (size_t idx = 0; idx < 2; ++idx) {
        ConfigOptionInt *object_id = m_public_config.option<ConfigOptionInt>(id_keys[idx]);
        ConfigOptionString *object_name = m_public_config.option<ConfigOptionString>(name_keys[idx]);
        if (object_id == nullptr || object_name == nullptr)
            continue;
        if (object_id->value < 0 || static_cast<size_t>(object_id->value) >= m_object_names.size())
            throw std::invalid_argument(std::string("A G-code script references an unknown object in ") +
                                        id_keys[idx] + ".");
        object_name->value = m_object_names[static_cast<size_t>(object_id->value)];
    }
}

std::string GCodeScriptProcessor::Impl::process(
    const std::string &script,
    uint16_t current_extruder)
{
    if (!m_prepared)
        throw std::logic_error("A G-code script must be prepared before it is processed.");
    if (m_processing)
        throw std::logic_error("The G-code script processor is not reentrant.");

    m_processing = true;
    try {
        // The producer Config was merged during prepare(), then the firmware
        // supplied current machine values. Resolve host-owned derived values
        // only after both parts of the execution context are complete.
        resolve_object_names();
        DynamicConfig specific;
        for (const std::string &key : m_specific_keys)
            copy_option(specific, m_public_config, key);

        DynamicConfig outputs;
        for (const char *key : k_machine_option_keys) {
            if (m_public_config.option(key) != nullptr)
                copy_option(outputs, m_public_config, key);
        }
        const DynamicConfig before(outputs);

        // current_position is a read-only legacy alias. The writable position
        // remains in config_outputs, where PlaceholderParser permits assignment.
        const ConfigOptionFloats *position = outputs.option<ConfigOptionFloats>("position");
        m_parser.set("current_position", new ConfigOptionFloats(position->get_values()));
        m_parser.set("zhop", new ConfigOptionFloat(0.0));

        // PlaceholderParser uses the numeric process argument to select vector
        // settings, while [current_extruder] is a regular named option. Keep
        // both representations synchronized for every script invocation.
        m_parser.set("current_extruder", new ConfigOptionInt(current_extruder));
        std::string result = m_parser.process(script, current_extruder, &specific, &outputs, &m_context);
        validate_outputs(outputs);
        infer_position_from_gcode(before, outputs, result);
        validate_outputs(outputs);
        publish_outputs(outputs);

        m_processing = false;
        m_prepared = false;
        return result;
    } catch (...) {
        m_processing = false;
        m_prepared = false;
        throw;
    }
}

void GCodeScriptProcessor::Impl::create_machine_options()
{
    m_public_config.set_key_value("position", new ConfigOptionFloats(3, 0.0));
    m_public_config.set_key_value(
        "e_retracted", new ConfigOptionFloats(m_extruder_count, 0.0));
    m_public_config.set_key_value(
        "e_restart_extra", new ConfigOptionFloats(m_extruder_count, 0.0));
    if (m_absolute_e)
        m_public_config.set_key_value(
            "e_position", new ConfigOptionFloats(m_extruder_count, 0.0));
}

void GCodeScriptProcessor::Impl::create_specific_options(const std::string &script_name)
{
    const std::map<t_custom_gcode_key, t_config_option_keys> &all_placeholders =
        custom_gcode_specific_placeholders();
    const std::map<t_custom_gcode_key, t_config_option_keys>::const_iterator found =
        all_placeholders.find(script_name);
    if (found == all_placeholders.end())
        return;

    for (const std::string &key : found->second) {
        const ConfigOptionDef *definition = custom_gcode_specific_config_def.get(key);
        if (definition == nullptr)
            throw std::logic_error("Missing custom G-code option definition: " + key);

        ConfigOption *option = definition->create_default_option();
        // This placeholder is consumed as one value per extruder by the legacy
        // generator, so its prepared vector uses the same cardinality.
        if (key == "layer_used_filament") {
            delete option;
            option = new ConfigOptionFloats(m_extruder_count, 0.0);
        }
        m_public_config.set_key_value(key, option);
        m_specific_keys.push_back(key);
    }
}

void GCodeScriptProcessor::Impl::merge_producer_config(
    const ConfigBase *producer_config)
{
    if (producer_config == nullptr)
        return;

    // The producer Config is borrowed only for this call. Clone every option
    // immediately so the firmware may reuse its scratch Config after prepare().
    for (const std::string &key : producer_config->keys()) {
        if (is_machine_option_key(key))
            throw std::invalid_argument("A script producer cannot replace runtime machine option: " + key);

        const ConfigOption *producer_option = producer_config->option(key);
        if (producer_option == nullptr)
            throw std::invalid_argument("Invalid G-code script Config option: " + key);
        const ConfigOption *existing = m_public_config.option(key);
        if (existing != nullptr && existing->type() != producer_option->type())
            throw std::invalid_argument("Wrong type for G-code script argument: " + key);
        m_public_config.set_key_value(key, producer_option->clone());
        if (std::find(m_specific_keys.begin(), m_specific_keys.end(), key) == m_specific_keys.end())
            m_specific_keys.push_back(key);
    }
}

void GCodeScriptProcessor::Impl::validate_outputs(const DynamicConfig &outputs) const
{
    validate_finite_vector(outputs, "position", 3);
    validate_finite_vector(outputs, "e_retracted", m_extruder_count);
    validate_finite_vector(outputs, "e_restart_extra", m_extruder_count);
    if (m_absolute_e)
        validate_finite_vector(outputs, "e_position", m_extruder_count);
}

void GCodeScriptProcessor::Impl::infer_position_from_gcode(
    const DynamicConfig &before,
    DynamicConfig &outputs,
    const std::string &gcode) const
{
    const ConfigOptionFloats *old_position = before.option<ConfigOptionFloats>("position");
    ConfigOptionFloats *new_position = outputs.option<ConfigOptionFloats>("position");
    if (old_position->get_values() != new_position->get_values())
        return;

    const std::array<std::optional<double>, 3> inferred =
        position_after_gcode(old_position->get_values(), gcode);
    if (inferred[0] && inferred[1] && inferred[2])
        new_position->set({*inferred[0], *inferred[1], *inferred[2]});
}

void GCodeScriptProcessor::Impl::publish_outputs(const DynamicConfig &outputs)
{
    // Copy all validated values only now. A parser or validation exception
    // therefore leaves the public in/out Config byte-for-byte unchanged.
    for (const char *key : k_machine_option_keys) {
        const ConfigOption *source = outputs.option(key);
        ConfigOption *destination = m_public_config.option(key);
        if (source != nullptr && destination != nullptr)
            destination->set(*source);
    }
}

GCodeScriptProcessor::GCodeScriptProcessor(const Print &print,
                                           const Orchestrator &orchestrator)
    : m_impl(new Impl(print, orchestrator)) {}

GCodeScriptProcessor::~GCodeScriptProcessor() = default;

const raw_gcode_script_processor *GCodeScriptProcessor::c_processor() const
{
    return m_impl->c_processor();
}

} // namespace Slic3r
