///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/ Copyright (c) Prusa Research 2017 - 2023 Enrico Turri @enricoturri1966, Vojtěch Bubník @bubnikv
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_GCodeReader_hpp_
#define slic3r_GCodeReader_hpp_

#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>

#include "Api/plugin/cpp/gcode/GCodeLineParser.hpp"
#include "libslic3r.h"
#include "Config/FFFPrintConfig.hpp"

namespace Slic3r {

class GCodeReader : protected slic3r_api::GCodeGeneration::GCodeLineParser {
public:
    class GCodeLine : public slic3r_api::GCodeGeneration::GCodeLineParser::GCodeLine {
    public:
        GCodeLine() { reset(); }
        void reset()
        {
            slic3r_api::GCodeGeneration::GCodeLineParser::GCodeLine::reset();
            m_mask = 0;
            memset(m_axis, 0, sizeof(m_axis));
            m_e_char = 0;
        }

        const std::string_view cmd() const { return this->command(); }

        // Return position in this->raw() string starting with the "axis" character.
        std::string_view axis_pos(char axis) const;
        bool  has(Axis axis) const { return (m_mask & (1 << int(axis))) != 0; }
        float value(Axis axis) const { return m_axis[axis]; }
        bool  has(char axis) const;
        bool  has_value(char axis, float &value) const;
        bool  has_value(char axis, int &value) const;
        // Parse value of an axis from raw string starting at axis_pos.
        static bool has_value(std::string_view axis_pos, float &value);
        static bool has_value(std::string_view axis_pos, int &value);
        float new_X(const GCodeReader &reader) const { return this->has(X) ? this->x() : reader.x(); }
        float new_Y(const GCodeReader &reader) const { return this->has(Y) ? this->y() : reader.y(); }
        float new_Z(const GCodeReader &reader) const { return this->has(Z) ? this->z() : reader.z(); }
        float new_E(const GCodeReader &reader) const { return this->has(E) ? this->e() : reader.e(); }
        float new_F(const GCodeReader &reader) const { return this->has(F) ? this->f() : reader.f(); }
        Point new_XY_scaled(const GCodeReader &reader) const 
            { return Point::new_scale(this->new_X(reader), this->new_Y(reader)); }
        float dist_X(const GCodeReader &reader) const { return this->has(X) ? (this->x() - reader.x()) : 0; }
        float dist_Y(const GCodeReader &reader) const { return this->has(Y) ? (this->y() - reader.y()) : 0; }
        float dist_Z(const GCodeReader &reader) const { return this->has(Z) ? (this->z() - reader.z()) : 0; }
        float dist_E(const GCodeReader &reader) const { return this->has(E) ? (this->e() - reader.e()) : 0; }
        float dist_XY(const GCodeReader &reader) const {
            float x = this->has(X) ? (this->x() - reader.x()) : 0;
            float y = this->has(Y) ? (this->y() - reader.y()) : 0;
            return sqrt(x*x + y*y);
        }
        bool cmd_is(const char *cmd_test)          const { return cmd_is(m_raw, cmd_test); }
        bool extruding(const GCodeReader &reader)  const { return this->cmd_is("G1") && this->dist_E(reader) > 0; }
        bool retracting(const GCodeReader &reader) const { return this->cmd_is("G1") && this->dist_E(reader) < 0; }
        bool travel()     const { return this->cmd_is("G1") && ! this->has(E); }
        void set(const GCodeReader &reader, const Axis axis, const float new_value, const int decimal_digits = 3);

        bool  has_x() const { return this->has(X); }
        bool  has_y() const { return this->has(Y); }
        bool  has_z() const { return this->has(Z); }
        bool  has_e() const { return this->has(E); }
        bool  has_f() const { return this->has(F); }
        bool  has_unknown_axis() const { return this->has(UNKNOWN_AXIS); }
        float x() const { return m_axis[X]; }
        float y() const { return m_axis[Y]; }
        float z() const { return m_axis[Z]; }
        float e() const { return m_axis[E]; }
        float f() const { return m_axis[F]; }

        char e_char() const { return m_e_char; }

        static bool cmd_is(const std::string &gcode_line, const char *cmd_test)
            { return command_is(gcode_line, cmd_test); }

        static bool cmd_starts_with(const std::string& gcode_line, const char* cmd_test)
            { return command_starts_with(gcode_line, cmd_test); }

        static std::string extract_cmd(const std::string& gcode_line)
            { return extract_command(gcode_line); }

    private:
        float            m_axis[NUM_AXES];
        uint32_t         m_mask;
        char             m_e_char;
        friend class GCodeReader;
    };
    class FakeGCodeLine : public GCodeLine {
    public:
        void set_x(float x) { m_axis[X] = x; m_mask = (m_mask | (1 << int(X))); }
        void set_y(float y) { m_axis[Y] = y; m_mask = (m_mask | (1 << int(Y))); }
        void set_z(float z) { m_axis[Z] = z; m_mask = (m_mask | (1 << int(Z))); }
        void set_e(float e) { m_axis[E] = e; m_mask = (m_mask | (1 << int(E))); }
        void set_f(float f) { m_axis[F] = f; m_mask = (m_mask | (1 << int(F))); }
    };

    typedef std::function<void(GCodeReader&, const GCodeLine&)> callback_t;
    typedef std::function<void(GCodeReader&, const char*, const char*)> raw_line_callback_t;
    
    GCodeReader() : m_verbose(false), m_extrusion_axis('E') { this->reset(); }
    void reset() { memset(m_position, 0, sizeof(m_position)); }
    void apply_config(const GCodeConfig &config);
    void apply_config(const DynamicPrintConfig &config);

    template<typename Callback>
    void parse_buffer(const std::string &buffer, Callback callback)
    {
        const char *ptr = buffer.c_str();
        const char *end = ptr + buffer.size();
        GCodeLine gline;
        m_parsing = true;
        while (m_parsing && *ptr != 0) {
            gline.reset();
            ptr = this->parse_line(ptr, end, gline, callback);
        }
    }

    void parse_buffer(const std::string &buffer)
        { this->parse_buffer(buffer, [](GCodeReader&, const GCodeReader::GCodeLine&){}); }

    template<typename Callback>
    const char* parse_line(const char *ptr, const char *end, GCodeLine &gline, Callback &callback)
    {
        const char *line_end = parse_line_internal(ptr, end, gline);
        callback(*this, gline);
        update_coordinates(gline);
        return line_end;
    }

    template<typename Callback>
    void parse_line(const std::string &line, Callback callback)
        { GCodeLine gline; this->parse_line(line.c_str(), line.c_str() + line.size(), gline, callback); }

    // Returns false if reading the file failed.
    bool parse_file(const std::string &file, callback_t callback);
    // Collect positions of line ends in the binary G-code to be used by the G-code viewer when memory mapping and displaying section of G-code
    // as an overlay in the 3D scene.
    bool parse_file(const std::string& file, callback_t callback, std::vector<std::vector<size_t>>& lines_ends);
    // Just read the G-code file line by line, calls callback (const char *begin, const char *end). Returns false if reading the file failed.
    bool parse_file_raw(const std::string &file, raw_line_callback_t callback);

    // To be called by the callback to stop parsing.
    void quit_parsing() { slic3r_api::GCodeGeneration::GCodeLineParser::quit_parsing(); }

    float& x()       { return m_position[X]; }
    float  x() const { return m_position[X]; }
    float& y()       { return m_position[Y]; }
    float  y() const { return m_position[Y]; }
    float& z()       { return m_position[Z]; }
    float  z() const { return m_position[Z]; }
    float& e()       { return m_position[E]; }
    float  e() const { return m_position[E]; }
    float& f()       { return m_position[F]; }
    float  f() const { return m_position[F]; }
    Point  xy_scaled() const { return Point::new_scale(this->x(), this->y()); }


    // Returns 0 for gcfNoExtrusion.
    char   extrusion_axis() const { return m_extrusion_axis; }
//  void   set_extrusion_axis(char axis) { m_extrusion_axis = axis; }

private:
    template<typename ParseLineCallback, typename LineEndCallback>
    bool        parse_file_raw_internal(const std::string &filename, ParseLineCallback parse_line_callback, LineEndCallback line_end_callback);
    template<typename ParseLineCallback, typename LineEndCallback>
    bool        parse_file_internal(const std::string &filename, ParseLineCallback parse_line_callback, LineEndCallback line_end_callback);

    const char* parse_line_internal(const char *ptr, const char *end, GCodeLine &gline);
    void        update_coordinates(GCodeLine &gline);

    GCodeConfig m_config;
    char        m_extrusion_axis;
    float       m_position[NUM_AXES];
    bool        m_verbose;
};

} /* namespace Slic3r */

#endif /* slic3r_GCodeReader_hpp_ */
