#include <catch2/catch.hpp>

#include <cstdint>

#include "libslic3r/GCodeReader.hpp"
#include "plugin_test_helpers.hpp"

/*
Legacy GCodeReader compatibility tests
======================================

GCodeReader now delegates lexical work to the public GCodeLineParser, but its
callers still depend on richer machine-state semantics. These tests keep that
compatibility contract separate from the syntax-only parser tests.
*/

namespace {

using namespace Slic3r;
using Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized;

TEST_CASE("GCodeReader preserves its stateful callbacks after lexical extraction",
          "[plugins][gcode][line-parser][reader-compatibility]")
{
    ensure_plugin_test_runtime_initialized();

    GCodeConfig config;
    config.extrusion_axis.value = "A";
    config.use_relative_e_distances.value = true;

    GCodeReader reader;
    reader.apply_config(config);
    uint32_t callback_count = 0;
    reader.parse_buffer(
        "G1 X1 A2\nG1 X3 A4\n",
        [&callback_count](GCodeReader &active_reader, const GCodeReader::GCodeLine &line) {
            ++callback_count;
            CHECK(line.cmd() == "G1");
            CHECK(line.has_x());
            CHECK(line.has_e());
            CHECK(line.e_char() == 'A');
            CHECK(active_reader.e() == Approx(0.0));
            CHECK(active_reader.x() == Approx(callback_count == 1 ? 0.0 : 1.0));
        });

    CHECK(callback_count == 2);
    CHECK(reader.x() == Approx(3.0));
    CHECK(reader.e() == Approx(4.0));
}

TEST_CASE("GCodeReader keeps its historical line helpers",
          "[plugins][gcode][line-parser][reader-compatibility]")
{
    CHECK(GCodeReader::GCodeLine::cmd_is("N7 G1 X2", "G1"));
    CHECK_FALSE(GCodeReader::GCodeLine::cmd_is("G10 X2", "G1"));
    CHECK(GCodeReader::GCodeLine::cmd_starts_with("  M106 S255", "M10"));
    CHECK(GCodeReader::GCodeLine::extract_cmd("\tG92 E0") == "G92");
}

} // namespace
