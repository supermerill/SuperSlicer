#include <catch2/catch.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/Format/BBConfig.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "nlohmann/json.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace Slic3r;

namespace {

class TemporaryBambuConfig
{
public:
    explicit TemporaryBambuConfig(const nlohmann::json &contents)
    {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        m_path = std::filesystem::temp_directory_path() /
                 ("superslicer-bambu-config-" + std::to_string(suffix) + ".json");
        std::ofstream stream(m_path);
        stream << contents.dump();
    }

    ~TemporaryBambuConfig()
    {
        std::error_code error;
        std::filesystem::remove(m_path, error);
    }

    const std::filesystem::path &path() const { return m_path; }

private:
    std::filesystem::path m_path;
};

bool contains_custom_variable(const std::string &variables,
                              const std::string &name,
                              const std::string &value)
{
    return variables.find(name + "=" + value + "\n") != std::string::npos;
}

} // namespace

TEST_CASE("Bambu settings which cannot be converted are retained as custom variables", "[Config][Bambu]")
{
    nlohmann::json source = {
        {"name", "project_settings"},
        {"layer_height", "0.25"},
        {"unknown_wall_setting", "12"},
        {"unknown_filament_setting", "PLA"},
        {"unknown_machine_setting", "enabled"},
        {"gcode_flavor", "not-a-valid-flavor"},
        {"nozzle_diameter", {"0.4", "0.6"}},
        {"unknown_per_extruder", {"left", "right"}},
        {"unknown_wall_array", {"one", "two", "three"}}
    };
    TemporaryBambuConfig input(source);
    DynamicPrintConfig config;
    config.set_key_value("print_custom_variables",
                         new ConfigOptionString("unknown_wall_setting=explicit\n"));
    ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::Enable);

    REQUIRE(read_json_file_bambu(input.path(), config, substitutions, true));

    const ConfigOptionString *print_variables = config.opt<ConfigOptionString>("print_custom_variables");
    const ConfigOptionString *printer_variables = config.opt<ConfigOptionString>("printer_custom_variables");
    const ConfigOptionStrings *filament_variables = config.opt<ConfigOptionStrings>("filament_custom_variables");
    REQUIRE(print_variables != nullptr);
    REQUIRE(printer_variables != nullptr);
    REQUIRE(filament_variables != nullptr);

    // An explicitly supplied custom variable wins over a generated fallback.
    CHECK(contains_custom_variable(print_variables->value, "unknown_wall_setting", "explicit"));
    CHECK_FALSE(contains_custom_variable(print_variables->value, "unknown_wall_setting", "12"));
    CHECK(contains_custom_variable(print_variables->value, "unknown_filament_setting", "PLA"));
    CHECK(contains_custom_variable(print_variables->value, "unknown_wall_array", "[\"one\",\"two\",\"three\"]"));

    CHECK(contains_custom_variable(printer_variables->value, "unknown_machine_setting", "enabled"));
    CHECK(contains_custom_variable(printer_variables->value, "gcode_flavor", "not-a-valid-flavor"));
    CHECK(contains_custom_variable(printer_variables->value, "chamber_cooling_mode", "2"));
    CHECK(printer_variables->value.find("name=") == std::string::npos);

    REQUIRE(filament_variables->size() >= 2);
    CHECK(contains_custom_variable(filament_variables->get_at(0), "unknown_per_extruder", "left"));
    CHECK(contains_custom_variable(filament_variables->get_at(1), "unknown_per_extruder", "right"));

    CHECK(config.opt_float("layer_height") == Approx(0.25));
    CHECK(print_variables->value.find("layer_height=") == std::string::npos);
    CHECK(printer_variables->value.find("layer_height=") == std::string::npos);
}
