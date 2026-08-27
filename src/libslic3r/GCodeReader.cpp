///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/ Copyright (c) Prusa Research 2017 - 2023 Lukáš Matěna @lukasmatena, Vojtěch Bubník @bubnikv, Enrico Turri @enricoturri1966, Lukáš Hejl @hejllukas, Vojtěch Král @vojtechkral
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "GCodeReader.hpp"

#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>

#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/fstream.hpp>

#include "LocalesUtils.hpp"
#include "Utils.hpp"

namespace Slic3r {

static inline char get_extrusion_axis_char(const GCodeConfig &config)
{
    std::string axis = get_extrusion_axis(config);
    assert(axis.size() <= 1);
    // Return 0 for gcfNoExtrusion
    return axis.empty() ? 0 : axis[0];
}

void GCodeReader::apply_config(const GCodeConfig &config)
{
    m_config = config;
    m_extrusion_axis = get_extrusion_axis_char(m_config);
}

void GCodeReader::apply_config(const DynamicPrintConfig &config)
{
    m_config.apply(config, true);
    m_extrusion_axis = get_extrusion_axis_char(m_config);
}

const char* GCodeReader::parse_line_internal(const char *ptr, const char *end, GCodeLine &gline)
{
    assert(is_decimal_separator_point());

    const char *next_line = this->parse_line_text(ptr, end, gline);

    // Rebuild the legacy XYZEF cache from the neutral lexical line. The public
    // parser deliberately does not know which letter this reader configured as
    // its extrusion axis.
    const std::string &raw = gline.raw();
    const char *cursor = skip_whitespaces(raw.c_str());
    cursor = skip_word(cursor);
    while (!is_end_of_gcode_line(*cursor)) {
        cursor = skip_whitespaces(cursor);
        if (is_end_of_gcode_line(*cursor))
            break;

        Axis axis = NUM_AXES_WITH_UNKNOWN;
        switch (*cursor) {
        case 'X': axis = X; break;
        case 'Y': axis = Y; break;
        case 'Z': axis = Z; break;
        case 'F': axis = F; break;
        default:
            if (*cursor == m_extrusion_axis && m_extrusion_axis != 0) {
                axis = E;
                gline.m_e_char = *cursor;
            } else if (*cursor > m_extrusion_axis && *cursor + 20 < m_extrusion_axis &&
                       m_extrusion_axis != 0) {
                // Preserve the historical alternate-letter extrusion rule.
                axis = E;
                gline.m_e_char = *cursor;
            } else if (*cursor >= 'A' && *cursor <= 'Z') {
                // Remember a valid numeric word even when it is not XYZEF.
                axis = UNKNOWN_AXIS;
            }
            break;
        }

        const char *word_end = skip_word(cursor);
        if (axis != NUM_AXES_WITH_UNKNOWN) {
            float value = 0.f;
            const std::string_view parameter(cursor, size_t(raw.c_str() + raw.size() - cursor));
            if (GCodeLineParser::GCodeLine::parameter_value(parameter, value)) {
                if (axis != UNKNOWN_AXIS)
                    gline.m_axis[int(axis)] = value;
                gline.m_mask |= 1 << int(axis);
            }
        }
        cursor = word_end;
    }
    
    if (gline.has(E) && m_config.use_relative_e_distances)
        m_position[E] = 0;

    if (m_verbose)
        std::cout << gline.m_raw << std::endl;

    return next_line;
}

void GCodeReader::update_coordinates(GCodeLine &gline)
{
    const std::string_view command = gline.command();
    if (!command.empty() && command.front() == 'G') {
        const size_t command_length = command.size();
        if ((command_length == 2 &&
             (command[1] == '0' || command[1] == '1' || command[1] == '2' || command[1] == '3')) ||
            (command_length == 3 && command[1] == '9' && command[2] == '2')) {
            for (size_t i = 0; i < NUM_AXES; ++ i)
                if (gline.has(Axis(i)))
                    m_position[i] = gline.value(Axis(i));
        }
    }
}

template<typename ParseLineCallback, typename LineEndCallback>
bool GCodeReader::parse_file_raw_internal(const std::string &filename, ParseLineCallback parse_line_callback, LineEndCallback line_end_callback)
{
    FilePtr in{ boost::nowide::fopen(filename.c_str(), "rb") };

    // Read the input stream 64kB at a time, extract lines and process them.
    std::vector<char> buffer(65536 * 10, 0);
    // Line buffer.
    std::string gcode_line;
    size_t file_pos = 0;
    m_parsing = true;
    for (;;) {
        size_t cnt_read = ::fread(buffer.data(), 1, buffer.size(), in.f);
        if (::ferror(in.f))
            return false;
        bool eof       = cnt_read == 0;
        auto it        = buffer.begin();
        auto it_bufend = buffer.begin() + cnt_read;
        while (it != it_bufend || (eof && ! gcode_line.empty())) {
            // Find end of line.
            bool eol    = false;
            auto it_end = it;
            for (; it_end != it_bufend && ! (eol = *it_end == '\r' || *it_end == '\n'); ++ it_end)
                if (*it_end == '\n')
                    line_end_callback(file_pos + (it_end - buffer.begin()) + 1);
            // End of line is indicated also if end of file was reached.
            eol |= eof && it_end == it_bufend;
            if (eol) {
                if (gcode_line.empty())
                    parse_line_callback(&(*it), &(*it_end));
                else {
                    gcode_line.insert(gcode_line.end(), it, it_end);
                    parse_line_callback(gcode_line.c_str(), gcode_line.c_str() + gcode_line.size());
                    gcode_line.clear();
                }
                if (! m_parsing)
                    // The callback wishes to exit.
                    return true;
            } else
                gcode_line.insert(gcode_line.end(), it, it_end);
            // Skip EOL.
            it = it_end; 
            if (it != it_bufend && *it == '\r')
                ++ it;
            if (it != it_bufend && *it == '\n') {
                line_end_callback(file_pos + (it - buffer.begin()) + 1);
                ++ it;
            }
        }
        if (eof)
            break;
        file_pos += cnt_read;
    }
    return true;
}

template<typename ParseLineCallback, typename LineEndCallback>
bool GCodeReader::parse_file_internal(const std::string &filename, ParseLineCallback parse_line_callback, LineEndCallback line_end_callback)
{
    GCodeLine gline;    
    return this->parse_file_raw_internal(filename, 
        [this, &gline, parse_line_callback](const char *begin, const char *end) {
            gline.reset();
            this->parse_line(begin, end, gline, parse_line_callback);
        }, 
        line_end_callback);
}

bool GCodeReader::parse_file(const std::string &file, callback_t callback)
{
    return this->parse_file_internal(file, callback, [](size_t){});
}

bool GCodeReader::parse_file(const std::string& file, callback_t callback, std::vector<std::vector<size_t>>& lines_ends)
{
    lines_ends.clear();
    lines_ends.push_back(std::vector<size_t>());
    return this->parse_file_internal(file, callback, [&lines_ends](size_t file_pos) { lines_ends.front().emplace_back(file_pos); });
}

bool GCodeReader::parse_file_raw(const std::string &filename, raw_line_callback_t line_callback)
{
    return this->parse_file_raw_internal(filename,
        [this, line_callback](const char *begin, const char *end) { line_callback(*this, begin, end); }, 
        [](size_t){});
}

bool GCodeReader::GCodeLine::has(char axis) const
{
    return this->has_parameter(axis);
}

std::string_view GCodeReader::GCodeLine::axis_pos(char axis) const
{
    return this->parameter_position(axis);
}

bool GCodeReader::GCodeLine::has_value(std::string_view axis_pos, float &value)
{
    if (axis_pos.size() > 1 && (axis_pos[1] == ' ' || axis_pos[1] == '\t'))
        return false;
    return GCodeLineParser::GCodeLine::parameter_value(axis_pos, value);
}

bool GCodeReader::GCodeLine::has_value(char axis, float &value) const
{
    assert(is_decimal_separator_point());
    return this->has_value(this->axis_pos(axis), value);
}

bool GCodeReader::GCodeLine::has_value(std::string_view axis_pos, int &value)
{
    if (axis_pos.empty() ||
        (axis_pos.size() > 1 && (axis_pos[1] == ' ' || axis_pos[1] == '\t')))
        return false;
    if (const char *cursor = axis_pos.data(); cursor != nullptr) {
        char *parsed_end = nullptr;
        const long parsed_value = std::strtol(++cursor, &parsed_end, 10);
        if (parsed_end != nullptr && is_end_of_word(*parsed_end)) {
            value = int(parsed_value);
            return true;
        }
    }
    return false;
}

bool GCodeReader::GCodeLine::has_value(char axis, int &value) const
{
    return this->has_value(this->axis_pos(axis), value);
}

void GCodeReader::GCodeLine::set(const GCodeReader &reader, const Axis axis, const float new_value, const int decimal_digits)
{
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(decimal_digits) << new_value;

    char match[3] = " X";
    if (int(axis) < 3)
        match[1] += int(axis);
    else if (axis == F)
        match[1] = 'F';
    else {
        assert(axis == E);
        // Extruder axis is set.
        assert(reader.extrusion_axis() != 0);
        match[1] = reader.extrusion_axis();
    }

    if (this->has(axis)) {
        size_t pos = m_raw.find(match)+2;
        size_t end = m_raw.find(' ', pos+1);
        m_raw = m_raw.replace(pos, end-pos, ss.str());
    } else {
        size_t pos = m_raw.find(' ');
        if (pos == std::string::npos)
            m_raw += std::string(match) + ss.str();
        else
            m_raw = m_raw.replace(pos, 0, std::string(match) + ss.str());
    }
    m_axis[axis] = new_value;
    m_mask |= 1 << int(axis);
}

}
