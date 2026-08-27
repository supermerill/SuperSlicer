///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

/*
Implementation of the SCFG configuration snapshot format.

Each record stores numeric metadata followed by length-delimited key and value
bytes. The framing lets string values contain punctuation and line breaks
without another escaping layer. Deserialization builds a separate configuration
and publishes it only after every record has been checked.
*/

#include "ConfigSnapshotSerialization.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <set>
#include <system_error>
#include <vector>

#include "ConfigDef.hpp"

namespace Slic3r {
namespace ConfigSnapshotSerialization {

namespace Detail {
std::unique_ptr<ConfigOption> create_empty_option(config_option_type type);
bool supports_option_type(config_option_type type);
} // namespace Detail

static bool serialize_option_value(const ConfigOption &option, std::string &value);
static bool deserialize_option_value(ConfigOption &option,
                                     ConfigOptionType type,
                                     uint32_t flags,
                                     const std::string &value);
static bool parse_record_header(const std::string &line,
                                ConfigOptionType &type,
                                uint32_t &flags,
                                size_t &key_size,
                                size_t &value_size);
static bool read_line(const std::string &serialized, size_t &position, std::string &line);
static bool parse_unsigned(const std::string &text, uint64_t &value);
static bool parse_signed_int32(const std::string &text, int32_t &value);
static bool valid_option_flags(ConfigOptionType type, uint32_t flags);

// Serialize keys in lexical order so equivalent ConfigBase implementations
// produce the same bytes even when their native keys() order differs.
bool serialize_all(const ConfigBase &source, std::string &output)
{
    std::vector<std::string> keys = source.keys();
    std::sort(keys.begin(), keys.end());
    if (std::adjacent_find(keys.begin(), keys.end()) != keys.end())
        return false;

    std::string serialized = "SCFG1\n" + std::to_string(keys.size()) + "\n";
    for (const std::string &key : keys) {
        if (key.empty() || key.find('\0') != std::string::npos)
            return false;

        const ConfigOption *option = source.option(key);
        if (option == nullptr)
            return false;

        const ConfigOptionType type = option->type();
        if (!Detail::supports_option_type(type) || !valid_option_flags(type, option->flags))
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

    output.swap(serialized);
    return true;
}

// Parse every record into an independent configuration. Swapping only after
// the final byte has been consumed preserves the caller's output on failure.
bool deserialize_all(const std::string &input, DynamicConfig &output)
{
    size_t position = 0;
    std::string line;
    if (!read_line(input, position, line) || line != "SCFG1")
        return false;

    if (!read_line(input, position, line))
        return false;
    uint64_t record_count = 0;
    if (!parse_unsigned(line, record_count) || record_count > input.size())
        return false;

    DynamicConfig parsed;
    std::set<std::string> keys;
    for (uint64_t record_idx = 0; record_idx < record_count; ++record_idx) {
        if (!read_line(input, position, line))
            return false;

        ConfigOptionType type = coNone;
        uint32_t flags = 0;
        size_t key_size = 0;
        size_t value_size = 0;
        if (!parse_record_header(line, type, flags, key_size, value_size))
            return false;
        if (key_size > input.size() - position)
            return false;
        const std::string key = input.substr(position, key_size);
        position += key_size;
        if (value_size > input.size() - position)
            return false;
        const std::string value = input.substr(position, value_size);
        position += value_size;

        if (key.empty() || key.find('\0') != std::string::npos || !keys.insert(key).second)
            return false;

        std::unique_ptr<ConfigOption> option = Detail::create_empty_option(type);
        if (option == nullptr || !deserialize_option_value(*option, type, flags, value))
            return false;
        parsed.set_key_value(key, option.release());
    }

    if (position != input.size())
        return false;
    output.swap(parsed);
    return true;
}

// Create the concrete option classes supported by SCFG. Generic enums have no
// label map because the transport stores their stable numeric value.
std::unique_ptr<ConfigOption> Detail::create_empty_option(config_option_type type)
{
    switch (type) {
    case coFloat:            return std::make_unique<ConfigOptionFloat>();
    case coFloats:           return std::make_unique<ConfigOptionFloats>();
    case coInt:              return std::make_unique<ConfigOptionInt>();
    case coInts:             return std::make_unique<ConfigOptionInts>();
    case coString:           return std::make_unique<ConfigOptionString>();
    case coStrings:          return std::make_unique<ConfigOptionStrings>();
    case coPercent:          return std::make_unique<ConfigOptionPercent>();
    case coPercents:         return std::make_unique<ConfigOptionPercents>();
    case coFloatOrPercent:   return std::make_unique<ConfigOptionFloatOrPercent>();
    case coFloatsOrPercents: return std::make_unique<ConfigOptionFloatsOrPercents>();
    case coPoint:            return std::make_unique<ConfigOptionPoint>();
    case coPoints:           return std::make_unique<ConfigOptionPoints>();
    case coPoint3:           return std::make_unique<ConfigOptionPoint3>();
    case coBool:             return std::make_unique<ConfigOptionBool>();
    case coBools:            return std::make_unique<ConfigOptionBools>();
    case coEnum:             return std::make_unique<ConfigOptionEnumGeneric>(nullptr);
    case coGraph:            return std::make_unique<ConfigOptionGraph>();
    case coGraphs:           return std::make_unique<ConfigOptionGraphs>();
    default:                 return nullptr;
    }
}

bool Detail::supports_option_type(config_option_type type)
{
    // Keep validation allocation-free because serialization checks every
    // option before encoding its value.
    switch (type) {
    case coFloat:
    case coFloats:
    case coInt:
    case coInts:
    case coString:
    case coStrings:
    case coPercent:
    case coPercents:
    case coFloatOrPercent:
    case coFloatsOrPercents:
    case coPoint:
    case coPoints:
    case coPoint3:
    case coBool:
    case coBools:
    case coEnum:
    case coGraph:
    case coGraphs:
        return true;
    default:
        return false;
    }
}

// Generic enum labels belong to a ConfigDef and may not exist in a temporary
// config. Numeric enum values therefore bypass label-based serialization.
static bool serialize_option_value(const ConfigOption &option, std::string &value)
{
    if (option.type() == coEnum) {
        value = std::to_string(option.get_int());
        return true;
    }
    value = option.serialize();
    return true;
}

// Install flags before parsing so disabled vector entries are accepted, then
// restore them because an option parser may update only part of the metadata.
static bool deserialize_option_value(ConfigOption &option,
                                     ConfigOptionType type,
                                     uint32_t flags,
                                     const std::string &value)
{
    if (!valid_option_flags(type, flags))
        return false;

    option.flags = flags;
    if (type == coEnum) {
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

// Split the numeric record header while leaving length-delimited key and value
// bytes untouched. Exactly four fields are required.
static bool parse_record_header(const std::string &line,
                                ConfigOptionType &type,
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

    type = static_cast<ConfigOptionType>(raw_type);
    flags = static_cast<uint32_t>(raw_flags);
    key_size = static_cast<size_t>(raw_key_size);
    value_size = static_cast<size_t>(raw_value_size);
    return Detail::supports_option_type(type) && valid_option_flags(type, flags);
}

// Metadata is line-based, while key and value bodies are consumed separately
// from their byte lengths and may therefore contain embedded newlines.
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

// Locale-independent integer parsing rejects both partial numbers and extra
// characters instead of silently accepting a valid prefix.
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

// Reject unknown flags so a future SCFG version can define their semantics
// instead of an older reader accepting and then losing information.
static bool valid_option_flags(ConfigOptionType type, uint32_t flags)
{
    const uint32_t known_flags = ConfigOption::FCO_PHONY | ConfigOption::FCO_EXTRUDER_ARRAY |
                                 ConfigOption::FCO_PLACEHOLDER_TEMP | ConfigOption::FCO_ENABLED |
                                 ConfigOption::FCO_CAN_DISABLED;
    if (flags == 0 || (flags & ~known_flags) != 0)
        return false;
    if ((flags & ConfigOption::FCO_EXTRUDER_ARRAY) != 0 &&
        (static_cast<uint32_t>(type) & static_cast<uint32_t>(coVectorType)) == 0)
        return false;
    if ((flags & ConfigOption::FCO_ENABLED) == 0 &&
        (flags & ConfigOption::FCO_CAN_DISABLED) == 0)
        return false;
    return true;
}

} // namespace ConfigSnapshotSerialization
} // namespace Slic3r
