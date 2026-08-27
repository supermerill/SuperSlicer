#include <catch2/catch.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "libslic3r/Api/plugin/cpp/gcode/GCodeLineParser.hpp"

/*
Public G-code lexical parser tests
==================================

This file intentionally includes only the public parser and standard-library
types. It verifies that plugins can inspect G-code syntax without importing
the private reader, configuration or geometry APIs from libslic3r.
*/

namespace {

using slic3r_api::GCodeGeneration::GCodeLineParser;

TEST_CASE("GCodeLineParser exposes commands comments and parameters",
          "[plugins][gcode][line-parser]")
{
    GCodeLineParser parser;
    std::vector<GCodeLineParser::GCodeLine> lines;
    parser.parse_buffer(
        "  G1 X1.25 Y-2 ; move\r\nM104 S210\n",
        [&lines](GCodeLineParser &, const GCodeLineParser::GCodeLine &line) {
            lines.push_back(line);
        });

    REQUIRE(lines.size() == 2);
    CHECK(lines[0].raw() == "  G1 X1.25 Y-2 ; move");
    CHECK(lines[0].command() == "G1");
    CHECK(lines[0].comment() == " move");
    CHECK(lines[0].has_parameter('X'));
    CHECK_FALSE(lines[0].has_parameter('Z'));

    const std::optional<float> x = lines[0].parameter_value('X');
    float y = 0.f;
    REQUIRE(x);
    REQUIRE(lines[0].parameter_value('Y', y));
    CHECK(*x == Approx(1.25));
    CHECK(y == Approx(-2.0));
    CHECK_FALSE(lines[0].parameter_value('Z'));

    int32_t temperature = 0;
    REQUIRE(lines[1].parameter_value('S', temperature));
    CHECK(temperature == 210);
}

TEST_CASE("GCodeLineParser validates complete numeric parameter words",
          "[plugins][gcode][line-parser]")
{
    GCodeLineParser parser;
    parser.parse_line(
        "M123 A+12 B-4.5 Cbad D7tail E 8 ; F99",
        [](GCodeLineParser &, const GCodeLineParser::GCodeLine &line) {
            int32_t integer = 0;
            float floating = 0.f;

            CHECK(line.has_parameter('C'));
            REQUIRE(line.parameter_value('A', integer));
            CHECK(integer == 12);
            REQUIRE(line.parameter_value('B', floating));
            CHECK(floating == Approx(-4.5));
            CHECK_FALSE(line.parameter_value('C'));
            CHECK_FALSE(line.parameter_value('C', floating));
            CHECK_FALSE(line.parameter_value('D', integer));
            REQUIRE(line.parameter_value('E', integer));
            CHECK(integer == 8);
            CHECK_FALSE(line.has_parameter('F'));
        });
}

TEST_CASE("GCodeLineParser command helpers preserve complete-word semantics",
          "[plugins][gcode][line-parser]")
{
    CHECK(GCodeLineParser::GCodeLine::command_is("N42 G1 X1", "G1"));
    CHECK_FALSE(GCodeLineParser::GCodeLine::command_is("G10 X1", "G1"));
    CHECK(GCodeLineParser::GCodeLine::command_starts_with("  G10 X1", "G1"));
    CHECK(GCodeLineParser::GCodeLine::extract_command("\tM106 S255") == "M106");
}

TEST_CASE("GCodeLineParser callbacks may stop a buffer traversal",
          "[plugins][gcode][line-parser]")
{
    GCodeLineParser parser;
    uint32_t callback_count = 0;
    parser.parse_buffer(
        "G1 X1\nG1 X2\nG1 X3\n",
        [&callback_count](GCodeLineParser &active_parser,
                          const GCodeLineParser::GCodeLine &) {
            ++callback_count;
            active_parser.quit_parsing();
        });
    CHECK(callback_count == 1);
}

} // namespace
