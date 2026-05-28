#include <catch2/catch.hpp>

#include "plugin_test_helpers.hpp"

#ifdef SLIC3R_TEST_PYTHON_PLUGINS

TEST_CASE("Python HairyObject plugin is available to perimeter tests", "[plugins][perimeter][hairy-object][python]")
{
    // This smoke test keeps the Python plugin fixture honest: the CMake target
    // copies the source plugin into the runtime folder, then the test bootstrap
    // loads it beside the other Python examples.
    REQUIRE(Slic3r::Test::Plugins::python_plugin_test_runtime_available());
}

#endif
