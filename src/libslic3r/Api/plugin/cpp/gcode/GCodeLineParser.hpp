///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_gcode_GCodeLineParser_hpp_
#define slic3r_Api_plugin_cpp_gcode_GCodeLineParser_hpp_

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

/*
Lightweight G-code lexical parser
=================================

GCodeLineParser separates a G-code text into lines, commands, comments and
letter-prefixed numeric parameters. It deliberately stops at syntax: it does
not track a machine position, choose an extrusion axis or interpret what a
command means for a firmware.

This class belongs to the public C++ plugin API so firmware plugins can inspect
generated or user-provided G-code without including private libslic3r headers.
A parsed GCodeLine owns its original text and may therefore be copied outside
the callback when a consumer needs to retain it.
*/

namespace slic3r_api { namespace GCodeGeneration {

class GCodeLineParser
{
public:
    /*
    One lexically parsed line. Parameter access uses the literal letter found
    in the text; the class does not assign special meanings to X, E or any
    other parameter.
    */
    class GCodeLine
    {
    public:
        GCodeLine();

        /* Restore the reusable line object to its empty state. */
        void reset();

        /* Return the complete line without its trailing CR/LF characters. */
        const std::string &raw() const { return m_raw; }

        /* Return the first word before arguments or a comment. */
        std::string_view command() const;

        /* Return the text after the first semicolon, without the semicolon. */
        std::string_view comment() const;

        /* Locate a parameter word and return a view beginning at its letter. */
        std::string_view parameter_position(char parameter) const;

        /* Report whether a parameter word is present, even if its value is invalid. */
        bool has_parameter(char parameter) const;

        /* Return a parsed floating-point parameter, or no value when it is absent or invalid. */
        std::optional<float> parameter_value(char parameter) const;

        /* Parse a floating-point parameter into storage supplied by the caller. */
        bool parameter_value(char parameter, float &value) const;

        /* Parse an integer parameter when its complete value is representable. */
        bool parameter_value(char parameter, int32_t &value) const;

        /* Parse a parameter view previously returned by parameter_position(). */
        static bool parameter_value(std::string_view parameter_position, float &value);
        static bool parameter_value(std::string_view parameter_position, int32_t &value);

        /* Compare a raw line with one complete command word. */
        static bool command_is(const std::string &gcode_line, const char *command);

        /* Test only the beginning of a command, for consumers that accept suffixes. */
        static bool command_starts_with(const std::string &gcode_line, const char *command);

        /* Extract the command word from an otherwise unparsed line. */
        static std::string extract_command(const std::string &gcode_line);

    protected:
        std::string m_raw;

        friend class GCodeLineParser;
    };

    using callback_t = std::function<void(GCodeLineParser &, const GCodeLine &)>;

    GCodeLineParser() = default;

    /*
    Visit every line in a buffer in source order. The same temporary line
    object is reset and reused between callbacks.
    */
    template<typename Callback>
    void parse_buffer(const std::string &buffer, Callback callback)
    {
        const char *ptr = buffer.c_str();
        const char *end = ptr + buffer.size();
        GCodeLine line;
        m_parsing = true;
        while (m_parsing && ptr < end && *ptr != 0) {
            line.reset();
            ptr = this->parse_line(ptr, end, line, callback);
        }
    }

    /* Parse a buffer while discarding its lines. */
    void parse_buffer(const std::string &buffer);

    /*
    Parse one line from a character range, invoke the callback, and return the
    first character after its optional CR/LF terminator.
    */
    template<typename Callback>
    const char *parse_line(const char *begin,
                           const char *end,
                           GCodeLine &line,
                           Callback &callback)
    {
        const char *next = this->parse_line_text(begin, end, line);
        callback(*this, line);
        return next;
    }

    /* Parse one standalone line and invoke the callback once. */
    template<typename Callback>
    void parse_line(const std::string &line, Callback callback)
    {
        GCodeLine parsed_line;
        this->parse_line(line.c_str(), line.c_str() + line.size(), parsed_line, callback);
    }

    /* Stop the currently active parse_buffer() after its current callback. */
    void quit_parsing() { m_parsing = false; }

protected:
    /* Copy one source line into a caller-provided line object without interpreting it. */
    const char *parse_line_text(const char *begin, const char *end, GCodeLine &line);

    static bool is_whitespace(char character);
    static bool is_end_of_line(char character);
    static bool is_end_of_gcode_line(char character);
    static bool is_end_of_word(char character);
    static const char *skip_whitespaces(const char *text);
    static const char *skip_word(const char *text);
    static const char *parameter_position(const char *raw_line, char parameter);

    bool m_parsing {false};
};

}} // namespace slic3r_api::GCodeGeneration

#endif /* slic3r_Api_plugin_cpp_gcode_GCodeLineParser_hpp_ */
