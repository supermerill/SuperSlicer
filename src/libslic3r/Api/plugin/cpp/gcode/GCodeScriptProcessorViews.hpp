///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_gcode_GCodeScriptProcessorViews_hpp_
#define slic3r_Api_plugin_cpp_gcode_GCodeScriptProcessorViews_hpp_

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_config_option.h"
#include "libslic3r/Api/plugin/c/slic3r_gcode_script.h"

/*
G-code script processor C++ views
=================================

These wrappers keep PlaceholderParser out of the plugin API. A firmware sees
one borrowed, strictly typed Config, fills values already declared by the host,
then asks the host to process the script. No allocation crosses the C ABI.
*/

namespace slic3r_api { namespace GCodeGeneration {

// Provides typed access to the temporary Config prepared for one script. It
// cannot create options: a missing key or a mismatched type is an immediate
// programming error in the firmware plugin.
class GCodeScriptConfig
{
public:
    explicit GCodeScriptConfig(config_handle *config) : m_config(config)
    {
        if (m_config == nullptr)
            throw std::invalid_argument("A G-code script Config cannot be null.");
    }

    config_handle *handle() const { return m_config; }
    bool has(const char *key) const { return config_get(m_config, key) != nullptr; }

    void set(const char *key, int32_t value) const
    {
        config_option_handle *option = mutable_option(key, SLIC3R_CONFIG_OPTION_INT);
        config_option_set_int(option, value, 0);
    }

    void set(const char *key, double value) const
    {
        config_option_handle *option = mutable_option(key, SLIC3R_CONFIG_OPTION_FLOAT);
        config_option_set_float(option, value, 0);
    }

    void set(const char *key, bool value) const
    {
        config_option_handle *option = mutable_option(key, SLIC3R_CONFIG_OPTION_BOOL);
        config_option_set_bool(option, value ? 1 : 0, 0);
    }

    void set(const char *key, const std::string &value) const
    {
        config_option_handle *option = mutable_option(key, SLIC3R_CONFIG_OPTION_STRING);
        config_option_set_string(option, value.c_str(), 0);
    }

    void set(const char *key, const std::vector<double> &values) const
    {
        config_option_handle *option = mutable_vector_option(
            key, SLIC3R_CONFIG_OPTION_FLOATS, values.size());
        for (uint32_t idx = 0; idx < values.size(); ++idx)
            config_option_set_float(option, values[idx], idx);
    }

    void set(const char *key, const std::vector<int32_t> &values) const
    {
        config_option_handle *option = mutable_vector_option(
            key, SLIC3R_CONFIG_OPTION_INTS, values.size());
        for (uint32_t idx = 0; idx < values.size(); ++idx)
            config_option_set_int(option, values[idx], idx);
    }

    int32_t get_int(const char *key) const
    {
        return config_option_get_int(const_option(key, SLIC3R_CONFIG_OPTION_INT), 0);
    }

    double get_float(const char *key) const
    {
        return config_option_get_float(const_option(key, SLIC3R_CONFIG_OPTION_FLOAT), 0);
    }

    bool get_bool(const char *key) const
    {
        return config_option_get_bool(const_option(key, SLIC3R_CONFIG_OPTION_BOOL), 0) != 0;
    }

    std::string get_string(const char *key) const
    {
        const config_option_handle *option = const_option(key, SLIC3R_CONFIG_OPTION_STRING);
        const uint32_t needed = config_option_get_string(option, 0, nullptr, 0);
        std::string value(needed + 1, '\0');
        if (needed > 0)
            config_option_get_string(option, 0, &value[0], needed + 1);
        value.resize(needed);
        return value;
    }

    std::vector<double> get_floats(const char *key) const
    {
        const config_option_handle *option = const_option(key, SLIC3R_CONFIG_OPTION_FLOATS);
        std::vector<double> values(config_option_size(option));
        for (uint32_t idx = 0; idx < values.size(); ++idx)
            values[idx] = config_option_get_float(option, idx);
        return values;
    }

private:
    const config_option_handle *const_option(const char *key,
                                              config_option_type expected) const
    {
        if (key == nullptr)
            throw std::invalid_argument("A G-code script option key cannot be null.");
        const config_option_handle *option = config_get(m_config, key);
        if (option == nullptr)
            throw std::invalid_argument(std::string("Unknown G-code script option: ") + key);
        if (config_option_type_get(option) != expected)
            throw std::invalid_argument(std::string("Wrong type for G-code script option: ") + key);
        return option;
    }

    config_option_handle *mutable_option(const char *key, config_option_type expected) const
    {
        (void)const_option(key, expected);
        config_option_handle *option = config_get_mutable(m_config, key);
        if (option == nullptr)
            throw std::invalid_argument(std::string("G-code script option is not mutable: ") + key);
        return option;
    }

    config_option_handle *mutable_vector_option(const char *key,
                                                config_option_type expected,
                                                size_t expected_size) const
    {
        config_option_handle *option = mutable_option(key, expected);
        if (config_option_size(option) != expected_size)
            throw std::invalid_argument(std::string("Wrong vector size for G-code script option: ") + key);
        return option;
    }

    config_handle *m_config = nullptr;
};

// Represents the interval between prepare() and process(). The Config and all
// of its options become invalid as soon as the same processor is prepared
// again, so callers should keep this value only while filling one script.
class GCodeScriptContext
{
public:
    GCodeScriptContext(const raw_gcode_script_processor *processor,
                       config_handle *config) :
        m_processor(processor),
        m_config(config)
    {}

    GCodeScriptConfig config() const { return GCodeScriptConfig(m_config); }

    template<class Value> void set(const char *key, const Value &value) const
    {
        config().set(key, value);
    }

    std::string process(const std::string &script, uint16_t current_extruder) const
    {
        raw_gcode_script_result result = {};
        result.struct_size = sizeof(result);
        result.status = RAW_GCODE_SCRIPT_STATUS_UNSET;
        m_processor->process(m_processor->context, script.c_str(), current_extruder, &result);

        if (result.status != RAW_GCODE_SCRIPT_STATUS_SUCCESS) {
            const char *message = result.error_message != nullptr ?
                result.error_message : "The G-code script processor failed.";
            throw std::runtime_error(message);
        }
        if (result.text == nullptr && result.text_size != 0)
            throw std::runtime_error("The G-code script processor returned invalid text.");
        return result.text != nullptr ?
            std::string(result.text, static_cast<size_t>(result.text_size)) : std::string();
    }

private:
    const raw_gcode_script_processor *m_processor = nullptr;
    config_handle *m_config = nullptr;
};

// Validates and borrows one host processor. A default-constructed view is the
// intentional no-processor state used by standalone firmware-session tests.
class GCodeScriptProcessorView
{
public:
    GCodeScriptProcessorView() = default;
    explicit GCodeScriptProcessorView(const raw_gcode_script_processor *processor) :
        m_processor(processor)
    {
        validate();
    }

    bool valid() const { return m_processor != nullptr; }

    GCodeScriptContext prepare(gcode_script_type script_type) const {
        validate();
        if (script_type == GCODE_SCRIPT_TYPE_INVALID)
            throw std::invalid_argument("A G-code script needs a valid type.");
        config_handle *config = m_processor->prepare(m_processor->context, script_type);
        if (config == nullptr)
            throw std::runtime_error("The host could not prepare the G-code script context.");
        return GCodeScriptContext(m_processor, config);
    }

private:
    void validate() const
    {
        if (m_processor == nullptr)
            throw std::logic_error("No G-code script processor is available.");
        if (m_processor->struct_size < sizeof(raw_gcode_script_processor) ||
            m_processor->context == nullptr || m_processor->prepare == nullptr ||
            m_processor->process == nullptr)
            throw std::invalid_argument("The G-code script processor is incomplete.");
    }

    const raw_gcode_script_processor *m_processor = nullptr;
};

}} // namespace slic3r_api::GCodeGeneration

#endif // slic3r_Api_plugin_cpp_gcode_GCodeScriptProcessorViews_hpp_
