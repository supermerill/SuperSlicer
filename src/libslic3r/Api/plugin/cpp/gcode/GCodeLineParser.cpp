///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "GCodeLineParser.hpp"

#include <charconv>
#include <cmath>
#include <cstring>

#include <fast_float/fast_float.h>

/*
G-code lexical parsing implementation
=====================================

The parser keeps framing and number recognition independent from all machine
state. A caller may therefore interpret the same line differently for each
firmware while sharing the exact same lexical rules.
*/

namespace slic3r_api { namespace GCodeGeneration {

GCodeLineParser::GCodeLine::GCodeLine()
{
    this->reset();
}

void GCodeLineParser::GCodeLine::reset()
{
    m_raw.clear();
}

std::string_view GCodeLineParser::GCodeLine::command() const
{
    const char *command_begin = GCodeLineParser::skip_whitespaces(m_raw.c_str());
    return std::string_view(command_begin,
                            GCodeLineParser::skip_word(command_begin) - command_begin);
}

std::string_view GCodeLineParser::GCodeLine::comment() const
{
    const size_t position = m_raw.find(';');
    return position == std::string::npos ? std::string_view() :
                                           std::string_view(m_raw).substr(position + 1);
}

std::string_view GCodeLineParser::GCodeLine::parameter_position(char parameter) const
{
    const char *position = GCodeLineParser::parameter_position(m_raw.c_str(), parameter);
    return position == nullptr ? std::string_view() :
                                 std::string_view(position, m_raw.size() - size_t(position - m_raw.data()));
}

bool GCodeLineParser::GCodeLine::has_parameter(char parameter) const
{
    return GCodeLineParser::parameter_position(m_raw.c_str(), parameter) != nullptr;
}

std::optional<float> GCodeLineParser::GCodeLine::parameter_value(char parameter) const
{
    float value = 0.f;
    return this->parameter_value(parameter, value) ? std::optional<float>(value) : std::nullopt;
}

bool GCodeLineParser::GCodeLine::parameter_value(char parameter, float &value) const
{
    return GCodeLine::parameter_value(this->parameter_position(parameter), value);
}

bool GCodeLineParser::GCodeLine::parameter_value(char parameter, int32_t &value) const
{
    return GCodeLine::parameter_value(this->parameter_position(parameter), value);
}

bool GCodeLineParser::GCodeLine::parameter_value(std::string_view position, float &value)
{
    if (position.empty())
        return false;

    const char *begin = position.data() + 1;
    const char *end = position.data() + position.size();
    while (begin < end && GCodeLineParser::is_whitespace(*begin))
        ++begin;
    float parsed_value = 0.f;
    const fast_float::from_chars_result parsed = fast_float::from_chars(begin, end, parsed_value);
    if (parsed.ptr == begin || parsed.ec != std::errc() || !std::isfinite(parsed_value) ||
        (parsed.ptr != end && !GCodeLineParser::is_end_of_word(*parsed.ptr)))
        return false;
    value = parsed_value;
    return true;
}

bool GCodeLineParser::GCodeLine::parameter_value(std::string_view position, int32_t &value)
{
    if (position.empty())
        return false;

    const char *begin = position.data() + 1;
    const char *end = position.data() + position.size();
    while (begin < end && GCodeLineParser::is_whitespace(*begin))
        ++begin;
    if (begin < end && *begin == '+')
        ++begin;
    int32_t parsed_value = 0;
    const std::from_chars_result parsed = std::from_chars(begin, end, parsed_value);
    if (parsed.ptr == begin || parsed.ec != std::errc() ||
        (parsed.ptr != end && !GCodeLineParser::is_end_of_word(*parsed.ptr)))
        return false;
    value = parsed_value;
    return true;
}

bool GCodeLineParser::GCodeLine::command_is(const std::string &gcode_line,
                                             const char *expected_command)
{
    if (expected_command == nullptr)
        return false;
    const char *command = GCodeLineParser::skip_whitespaces(gcode_line.c_str());
    if (*command == 'N') {
        command = GCodeLineParser::skip_word(command);
        command = GCodeLineParser::skip_whitespaces(command);
    }
    const size_t length = std::strlen(expected_command);
    const size_t available = gcode_line.size() - size_t(command - gcode_line.c_str());
    if (length > available)
        return false;
    return std::strncmp(command, expected_command, length) == 0 &&
           GCodeLineParser::is_end_of_word(command[length]);
}

bool GCodeLineParser::GCodeLine::command_starts_with(const std::string &gcode_line,
                                                      const char *expected_command)
{
    if (expected_command == nullptr)
        return false;
    const char *command = GCodeLineParser::skip_whitespaces(gcode_line.c_str());
    const size_t expected_length = std::strlen(expected_command);
    const size_t available = gcode_line.size() - size_t(command - gcode_line.c_str());
    return expected_length <= available &&
           std::strncmp(command, expected_command, expected_length) == 0;
}

std::string GCodeLineParser::GCodeLine::extract_command(const std::string &gcode_line)
{
    GCodeLine line;
    line.m_raw = gcode_line;
    const std::string_view command = line.command();
    return std::string(command.begin(), command.end());
}

void GCodeLineParser::parse_buffer(const std::string &buffer)
{
    this->parse_buffer(buffer, [](GCodeLineParser &, const GCodeLine &) {});
}

const char *GCodeLineParser::parse_line_text(const char *begin,
                                              const char *end,
                                              GCodeLine &line)
{
    if (begin == nullptr || end == nullptr || begin > end)
        return end;

    // A caller may reuse a line object directly, without going through
    // parse_buffer(), so an empty source line must also replace prior text.
    line.m_raw.clear();
    const char *cursor = begin;
    while (cursor < end && !is_end_of_line(*cursor))
        ++cursor;
    if (cursor > begin)
        line.m_raw.assign(begin, cursor);

    if (cursor < end && *cursor == '\r')
        ++cursor;
    if (cursor < end && *cursor == '\n')
        ++cursor;
    return cursor;
}

bool GCodeLineParser::is_whitespace(char character)
{
    return character == ' ' || character == '\t';
}

bool GCodeLineParser::is_end_of_line(char character)
{
    return character == '\r' || character == '\n' || character == 0;
}

bool GCodeLineParser::is_end_of_gcode_line(char character)
{
    return character == ';' || is_end_of_line(character);
}

bool GCodeLineParser::is_end_of_word(char character)
{
    return is_whitespace(character) || is_end_of_gcode_line(character);
}

const char *GCodeLineParser::skip_whitespaces(const char *text)
{
    while (is_whitespace(*text))
        ++text;
    return text;
}

const char *GCodeLineParser::skip_word(const char *text)
{
    while (!is_end_of_word(*text))
        ++text;
    return text;
}

const char *GCodeLineParser::parameter_position(const char *raw_line, char parameter)
{
    const char *cursor = skip_whitespaces(raw_line);
    cursor = skip_word(cursor);
    while (!is_end_of_gcode_line(*cursor)) {
        cursor = skip_whitespaces(cursor);
        if (is_end_of_gcode_line(*cursor))
            break;
        if (*cursor == parameter)
            return cursor;
        cursor = skip_word(cursor);
    }
    return nullptr;
}

}} // namespace slic3r_api::GCodeGeneration
