///|/ Copyright (c) SuperSlicer 2026 Durand R?mi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_GCode_TemperatureMover_hpp_
#define slic3r_GCode_TemperatureMover_hpp_

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <list>
#include <regex>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "libslic3r/ExtrusionRole.hpp"
#include "libslic3r/GCode/GCodeWriter.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Config/FFFPrintConfig.hpp"

namespace Slic3r {


class TemperatureMover
{
    class BufferData
    {
    public:
        // raw string, contains end position
        std::string raw;
        // time to go from start to end
        float   time;
        int16_t temperature = -1;
        uint16_t temperature_extruder_idx = uint16_t(0);
        // start position
        float x = 0, y = 0, z = 0, e = 0;
        // delta to go to end position
        float dx = 0, dy = 0, dz = 0, de = 0;
        char e_axis_char = 'E';
        BufferData(std::string line, char e_axis, float time, int16_t temperature, uint16_t extruder_idx)
            : raw(line), time(time), temperature(temperature), temperature_extruder_idx(extruder_idx), e_axis_char(e_axis)
        {
            // avoid double \n
            if (!line.empty() && line.back() == '\n')
                line.pop_back();
        }
    };

private:
    const std::regex regex_temperature_set;
    const std::vector<double> heating_speed; // in °C/s
    float m_max_seconds_delay;
    const bool relative_e;

    GCodeReader m_parser{};
    const GCodeWriter& m_writer;

    //current value (at the back of the buffer), when parsing a new line
    GCodeExtrusionRole current_role = GCodeExtrusionRole::Custom;
    // in unit/second
    double m_current_speed = 1000 / 60.0;
    bool m_is_custom_gcode = false;
    uint16_t m_current_extruder = 0;

    // variable for when you add a line (front of the buffer)
    int m_front_buffer_temperatures[256];
    int m_back_buffer_temperatures[256];

    //buffer
    std::list<BufferData> m_buffer;
    double m_buffer_time_size = 0;

    // The output of process_layer()
    std::string m_process_output;

public:
    TemperatureMover(const GCodeWriter &writer,
             const FullPrintConfig     &config,
             const std::vector<double>  heating_speed,
             const bool                 relative_e)
        : regex_temperature_set("S[0-9]+")
        , heating_speed(heating_speed)
        , relative_e(relative_e)
        , m_writer(writer)
    {
        m_parser.apply_config(config);
        m_max_seconds_delay = 0;
        for (double speed : heating_speed) {
            float delay = 300.f / float(speed);
            if (delay > m_max_seconds_delay) {
                m_max_seconds_delay = delay;
            }
        }
        std::fill(std::begin(m_front_buffer_temperatures),std::end(m_front_buffer_temperatures),int16_t(-1));
        std::fill(std::begin(m_back_buffer_temperatures),std::end(m_back_buffer_temperatures),int16_t(-1));
    }

    // Adds the gcode contained in the given string to the analysis and returns it after removing the workcodes
    const std::string& process_gcode(const std::string& gcode, bool flush);

private:
    BufferData& put_in_buffer(BufferData&& data) {
        assert(data.time >= 0 && data.time < 1000000 && !std::isnan(data.time));
         m_buffer_time_size += data.time;
        if (!m_buffer.empty() && data.temperature_extruder_idx == m_buffer.back().temperature_extruder_idx &&
            data.temperature >= 0 && m_buffer.back().temperature >= 0) {
            // erase last item
            m_buffer.back() = data;
        } else {
            m_buffer.emplace_back(data);
        }
        return m_buffer.back();
    }
    std::list<BufferData>::iterator remove_from_buffer(std::list<BufferData>::iterator data) {
        assert(data->time >= 0 && data->time < 1000000 && !std::isnan(data->time));
        m_buffer_time_size -= data->time;
        return m_buffer.erase(data);
    }
    // Processes the given gcode line
    void _process_gcode_line(GCodeReader& reader, const GCodeReader::GCodeLine& line);
    void _process_ACTIVATE_EXTRUDER(const std::string_view command);
    void _process_T(const std::string_view command);
    //void _print_in_middle_G1(BufferData& line_to_split, float nb_sec, const std::string& line_to_write);
    void _put_in_middle_G1(std::list<BufferData>::reverse_iterator item_to_split, float nb_sec_since_itemtosplit_start, BufferData &&line_to_write, float max_time);
    void _remove_low_heat(int16_t min_temp, uint16_t extr_idx, float past_sec);
    std::tuple<int16_t, uint16_t> _get_temperature_with_extruder_idx(const std::string &line, GCodeFlavor flavor);
    void write_buffer_data();
};

} // namespace Slic3r


#endif /* slic3r_GCode_TemperatureMover_hpp_ */
