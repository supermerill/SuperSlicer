///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

/*
Host implementation of temporary plugin configurations.

The SCFG format stores one self-describing record per option. Numeric metadata
is kept readable while byte lengths frame keys and serialized values, so string
options may contain punctuation and line breaks without an outer escape layer.
Deserialization always builds a complete temporary result before changing the
destination, which makes a malformed property payload harmless to live state.
*/

#include "libslic3r/Api/plugin/c/slic3r_config.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include "ApiHostUtils.hpp"
#include "Orchestrator.hpp"
#include "libslic3r/ConfigDef.hpp"
#include "libslic3r/ConfigOption.hpp"

namespace Slic3r {

static bool serialize_config(const ConfigBase &config, std::string &serialized);
static bool deserialize_config(const std::string &serialized, DynamicConfig &config);
static std::unique_ptr<ConfigOption> create_empty_option(config_option_type type);
static bool serialize_option_value(const ConfigOption &option, std::string &value);
static bool deserialize_option_value(ConfigOption &option,
                                     config_option_type type,
                                     uint32_t flags,
                                     const std::string &value);
static bool parse_record_header(const std::string &line,
                                config_option_type &type,
                                uint32_t &flags,
                                size_t &key_size,
                                size_t &value_size);
static bool read_line(const std::string &serialized, size_t &position, std::string &line);
static bool parse_unsigned(const std::string &text, uint64_t &value);
static bool parse_signed_int32(const std::string &text, int32_t &value);
static bool supported_option_type(config_option_type type);
static bool valid_option_flags(config_option_type type, uint32_t flags);
static uint32_t copy_string_out(const std::string &value, char *output, uint32_t capacity);

// Serialize keys in lexical order so equivalent ConfigBase implementations
// produce the same bytes even when their native keys() order differs.
static bool serialize_config(const ConfigBase &config, std::string &serialized)
{
    std::vector<std::string> keys = config.keys();
    std::sort(keys.begin(), keys.end());
    if (std::adjacent_find(keys.begin(), keys.end()) != keys.end())
        return false;

    serialized = "SCFG1\n" + std::to_string(keys.size()) + "\n";
    for (const std::string &key : keys) {
        if (key.empty() || key.find('\0') != std::string::npos)
            return false;

        const ConfigOption *option = config.option(key);
        if (option == nullptr)
            return false;

        const config_option_type type = static_cast<config_option_type>(option->type());
        if (!supported_option_type(type) || !valid_option_flags(type, option->flags))
            return false;

        std::string value;
        if (!serialize_option_value(*option, value))
            return false;

        serialized += std::to_string(static_cast<uint32_t>(type));
        serialized += ':';
        serialized += std::to_string(option->flags);
        serialized += ':';
        serialized += std::to_string(key.size());
        serialized += ':';
        serialized += std::to_string(value.size());
        serialized += '\n';
        serialized += key;
        serialized += value;
    }
    return true;
}

// Parse the whole document into an independent DynamicConfig. The caller can
// therefore merge or discard the result without exposing partial records.
static bool deserialize_config(const std::string &serialized, DynamicConfig &config)
{
    size_t position = 0;
    std::string line;
    if (!read_line(serialized, position, line) || line != "SCFG1")
        return false;

    if (!read_line(serialized, position, line))
        return false;
    uint64_t record_count = 0;
    if (!parse_unsigned(line, record_count) || record_count > serialized.size())
        return false;

    DynamicConfig parsed;
    std::set<std::string> keys;
    for (uint64_t record_idx = 0; record_idx < record_count; ++record_idx) {
        if (!read_line(serialized, position, line))
            return false;

        config_option_type type = SLIC3R_CONFIG_OPTION_NONE;
        uint32_t flags = 0;
        size_t key_size = 0;
        size_t value_size = 0;
        if (!parse_record_header(line, type, flags, key_size, value_size))
            return false;
        if (key_size > serialized.size() - position)
            return false;
        const std::string key = serialized.substr(position, key_size);
        position += key_size;
        if (value_size > serialized.size() - position)
            return false;
        const std::string value = serialized.substr(position, value_size);
        position += value_size;

        if (key.empty() || key.find('\0') != std::string::npos || !keys.insert(key).second)
            return false;

        std::unique_ptr<ConfigOption> option = create_empty_option(type);
        if (option == nullptr || !deserialize_option_value(*option, type, flags, value))
            return false;
        parsed.set_key_value(key, option.release());
    }

    if (position != serialized.size())
        return false;
    config.swap(parsed);
    return true;
}

// Create only the concrete option classes represented by the public C enum.
// Dynamic enum options deliberately have no label map because SCFG stores their
// stable numeric value instead of a preset-specific textual label.
static std::unique_ptr<ConfigOption> create_empty_option(config_option_type type)
{
    switch (type) {
    case SLIC3R_CONFIG_OPTION_FLOAT:              return std::make_unique<ConfigOptionFloat>();
    case SLIC3R_CONFIG_OPTION_FLOATS:             return std::make_unique<ConfigOptionFloats>();
    case SLIC3R_CONFIG_OPTION_INT:                return std::make_unique<ConfigOptionInt>();
    case SLIC3R_CONFIG_OPTION_INTS:               return std::make_unique<ConfigOptionInts>();
    case SLIC3R_CONFIG_OPTION_STRING:             return std::make_unique<ConfigOptionString>();
    case SLIC3R_CONFIG_OPTION_STRINGS:            return std::make_unique<ConfigOptionStrings>();
    case SLIC3R_CONFIG_OPTION_PERCENT:            return std::make_unique<ConfigOptionPercent>();
    case SLIC3R_CONFIG_OPTION_PERCENTS:           return std::make_unique<ConfigOptionPercents>();
    case SLIC3R_CONFIG_OPTION_FLOAT_OR_PERCENT:   return std::make_unique<ConfigOptionFloatOrPercent>();
    case SLIC3R_CONFIG_OPTION_FLOATS_OR_PERCENTS: return std::make_unique<ConfigOptionFloatsOrPercents>();
    case SLIC3R_CONFIG_OPTION_POINT:              return std::make_unique<ConfigOptionPoint>();
    case SLIC3R_CONFIG_OPTION_POINTS:             return std::make_unique<ConfigOptionPoints>();
    case SLIC3R_CONFIG_OPTION_POINT3:             return std::make_unique<ConfigOptionPoint3>();
    case SLIC3R_CONFIG_OPTION_BOOL:               return std::make_unique<ConfigOptionBool>();
    case SLIC3R_CONFIG_OPTION_BOOLS:              return std::make_unique<ConfigOptionBools>();
    case SLIC3R_CONFIG_OPTION_ENUM:               return std::make_unique<ConfigOptionEnumGeneric>(nullptr);
    case SLIC3R_CONFIG_OPTION_GRAPH:              return std::make_unique<ConfigOptionGraph>();
    case SLIC3R_CONFIG_OPTION_GRAPHS:             return std::make_unique<ConfigOptionGraphs>();
    default:                                      return nullptr;
    }
}

// Generic enum labels belong to a ConfigDef and may not exist in a temporary
// config, so enum values use their numeric representation. Other option classes
// keep their established serialization, including per-item enabled markers.
static bool serialize_option_value(const ConfigOption &option, std::string &value)
{
    if (option.type() == coEnum) {
        value = std::to_string(option.get_int());
        return true;
    }
    value = option.serialize();
    return true;
}

// Flags are installed before parsing so disabled markers are legal, then
// restored exactly because deserialize() may update only a subset of them.
static bool deserialize_option_value(ConfigOption &option,
                                     config_option_type type,
                                     uint32_t flags,
                                     const std::string &value)
{
    if (!valid_option_flags(type, flags))
        return false;

    option.flags = flags;
    if (type == SLIC3R_CONFIG_OPTION_ENUM) {
        int32_t enum_value = 0;
        if (!parse_signed_int32(value, enum_value))
            return false;
        option.set_int(enum_value);
    } else if (!option.deserialize(value, false)) {
        return false;
    }
    option.flags = flags;
    return true;
}

// Split the small numeric record header while leaving key and value bytes
// untouched. Exactly four fields are required to reject ambiguous documents.
static bool parse_record_header(const std::string &line,
                                config_option_type &type,
                                uint32_t &flags,
                                size_t &key_size,
                                size_t &value_size)
{
    const size_t first = line.find(':');
    const size_t second = first == std::string::npos ? std::string::npos : line.find(':', first + 1);
    const size_t third = second == std::string::npos ? std::string::npos : line.find(':', second + 1);
    if (first == std::string::npos || second == std::string::npos || third == std::string::npos ||
        line.find(':', third + 1) != std::string::npos)
        return false;

    uint64_t raw_type = 0;
    uint64_t raw_flags = 0;
    uint64_t raw_key_size = 0;
    uint64_t raw_value_size = 0;
    if (!parse_unsigned(line.substr(0, first), raw_type) ||
        !parse_unsigned(line.substr(first + 1, second - first - 1), raw_flags) ||
        !parse_unsigned(line.substr(second + 1, third - second - 1), raw_key_size) ||
        !parse_unsigned(line.substr(third + 1), raw_value_size) ||
        raw_type > std::numeric_limits<uint32_t>::max() ||
        raw_flags > std::numeric_limits<uint32_t>::max() ||
        raw_key_size > std::numeric_limits<size_t>::max() ||
        raw_value_size > std::numeric_limits<size_t>::max())
        return false;

    type = static_cast<config_option_type>(raw_type);
    flags = static_cast<uint32_t>(raw_flags);
    key_size = static_cast<size_t>(raw_key_size);
    value_size = static_cast<size_t>(raw_value_size);
    return supported_option_type(type) && valid_option_flags(type, flags);
}

// Lines frame only metadata. Keys and values are consumed separately from the
// byte lengths in that metadata, so embedded newlines never reach this helper.
static bool read_line(const std::string &serialized, size_t &position, std::string &line)
{
    if (position > serialized.size())
        return false;
    const size_t end = serialized.find('\n', position);
    if (end == std::string::npos)
        return false;
    line = serialized.substr(position, end - position);
    position = end + 1;
    return true;
}

// from_chars avoids locale-dependent parsing and requires every character to
// belong to the decimal number.
static bool parse_unsigned(const std::string &text, uint64_t &value)
{
    if (text.empty())
        return false;
    const char *begin = text.data();
    const char *end = begin + text.size();
    const std::from_chars_result result = std::from_chars(begin, end, value, 10);
    return result.ec == std::errc() && result.ptr == end;
}

static bool parse_signed_int32(const std::string &text, int32_t &value)
{
    if (text.empty())
        return false;
    const char *begin = text.data();
    const char *end = begin + text.size();
    const std::from_chars_result result = std::from_chars(begin, end, value, 10);
    return result.ec == std::errc() && result.ptr == end;
}

static bool supported_option_type(config_option_type type)
{
    return create_empty_option(type) != nullptr;
}

// SCFG1 understands exactly the public flags available when the format was
// introduced. Rejecting future bits lets a later format version define their
// semantics instead of silently losing them.
static bool valid_option_flags(config_option_type type, uint32_t flags)
{
    const uint32_t known_flags = FCO_PHONY | FCO_EXTRUDER_ARRAY | FCO_PLACEHOLDER_TEMP |
                                 FCO_ENABLED | FCO_CAN_DISABLED;
    if (flags == 0 || (flags & ~known_flags) != 0)
        return false;
    if ((flags & FCO_EXTRUDER_ARRAY) != 0 &&
        (static_cast<uint32_t>(type) & static_cast<uint32_t>(SLIC3R_CONFIG_OPTION_VECTOR_TYPE)) == 0)
        return false;
    if ((flags & FCO_ENABLED) == 0 && (flags & FCO_CAN_DISABLED) == 0)
        return false;
    return true;
}

static uint32_t copy_string_out(const std::string &value, char *output, uint32_t capacity)
{
    if (value.size() > std::numeric_limits<uint32_t>::max())
        return 0;
    if (output != nullptr && capacity > 0) {
        const uint32_t count = value.size() < capacity - 1 ? static_cast<uint32_t>(value.size()) : capacity - 1;
        if (count > 0)
            std::memcpy(output, value.data(), count);
        output[count] = '\0';
    }
    return static_cast<uint32_t>(value.size());
}

} // namespace Slic3r

extern "C" {

const_strings_t config_keys(const config_handle *config_handle_value)
{
    const_strings_t out = {};
    if (config_handle_value == nullptr)
        return out;

    // The C array points into thread-local storage so callers may consume it
    // after this function returns without allocating or transferring ownership.
    static thread_local std::vector<std::string> key_storage;
    static thread_local std::vector<const char *> key_ptrs;
    key_storage = Slic3r::ApiHost::to_config(config_handle_value)->keys();
    key_ptrs.clear();
    key_ptrs.reserve(key_storage.size());
    for (const std::string &key : key_storage)
        key_ptrs.push_back(key.c_str());

    out.items = key_ptrs.empty() ? nullptr : key_ptrs.data();
    out.size = key_ptrs.size();
    return out;
}

const config_option_handle *config_get(const config_handle *config_handle_value, const char *key)
{
    if (config_handle_value == nullptr || key == nullptr)
        return nullptr;
    const Slic3r::ConfigBase *config = Slic3r::ApiHost::to_config(config_handle_value);
    return reinterpret_cast<const config_option_handle *>(config->option(key));
}

config_option_handle *config_get_mutable(config_handle *config_handle_value, const char *key)
{
    if (config_handle_value == nullptr || key == nullptr)
        return nullptr;
    Slic3r::ConfigBase *config = Slic3r::ApiHost::to_config(config_handle_value);
    return reinterpret_cast<config_option_handle *>(config->optptr(key, false));
}

config_handle *storage_new_config(storage_handle *storage_handle_value)
{
    if (storage_handle_value == nullptr)
        return nullptr;
    try {
        Slic3r::PluginStorage *storage = reinterpret_cast<Slic3r::PluginStorage *>(storage_handle_value);
        Slic3r::DynamicConfig &config = storage->configs.emplace_back();
        config_handle *handle = Slic3r::ApiHost::to_config_handle(&config);
        storage->generic_storage.insert(handle);
        return handle;
    } catch (...) {
        return nullptr;
    }
}

config_option_handle *config_get_or_add_mutable(config_handle *config_handle_value,
                                                const char *key,
                                                config_option_type type)
{
    if (config_handle_value == nullptr || key == nullptr || key[0] == '\0' ||
        !Slic3r::supported_option_type(type))
        return nullptr;
    try {
        Slic3r::DynamicConfig *config =
            dynamic_cast<Slic3r::DynamicConfig *>(Slic3r::ApiHost::to_config(config_handle_value));
        if (config == nullptr)
            return nullptr;

        Slic3r::ConfigOption *existing = config->optptr(key, false);
        if (existing != nullptr)
            return existing->type() == static_cast<Slic3r::ConfigOptionType>(type) ?
                reinterpret_cast<config_option_handle *>(existing) : nullptr;

        std::unique_ptr<Slic3r::ConfigOption> option = Slic3r::create_empty_option(type);
        if (option == nullptr)
            return nullptr;
        Slic3r::ConfigOption *result = option.get();
        config->set_key_value(key, option.release());
        return reinterpret_cast<config_option_handle *>(result);
    } catch (...) {
        return nullptr;
    }
}

int32_t config_clear(config_handle *config_handle_value)
{
    if (config_handle_value == nullptr)
        return 0;
    try {
        Slic3r::DynamicConfig *config =
            dynamic_cast<Slic3r::DynamicConfig *>(Slic3r::ApiHost::to_config(config_handle_value));
        if (config == nullptr)
            return 0;
        config->clear();
        return 1;
    } catch (...) {
        return 0;
    }
}

uint32_t config_serialize_all(const config_handle *config_handle_value, char *output, uint32_t capacity)
{
    if (config_handle_value == nullptr)
        return 0;
    try {
        std::string serialized;
        if (!Slic3r::serialize_config(*Slic3r::ApiHost::to_config(config_handle_value), serialized))
            return 0;
        return Slic3r::copy_string_out(serialized, output, capacity);
    } catch (...) {
        return 0;
    }
}

int32_t config_deserialize_all(config_handle *destination_handle, const char *serialized_value)
{
    if (destination_handle == nullptr || serialized_value == nullptr)
        return 0;
    try {
        Slic3r::DynamicConfig *destination =
            dynamic_cast<Slic3r::DynamicConfig *>(Slic3r::ApiHost::to_config(destination_handle));
        if (destination == nullptr)
            return 0;

        Slic3r::DynamicConfig incoming;
        if (!Slic3r::deserialize_config(serialized_value, incoming))
            return 0;

        // Clone the destination and replace only incoming keys. A type change
        // is valid during a merge, so set_key_value() is used instead of the
        // type-preserving DynamicConfig operator+=.
        Slic3r::DynamicConfig merged(*destination);
        for (std::map<Slic3r::t_config_option_key,
                      std::unique_ptr<Slic3r::ConfigOption>>::const_iterator it = incoming.cbegin();
             it != incoming.cend(); ++it)
            merged.set_key_value(it->first, it->second->clone());
        destination->swap(merged);
        return 1;
    } catch (...) {
        return 0;
    }
}

} // extern "C"
