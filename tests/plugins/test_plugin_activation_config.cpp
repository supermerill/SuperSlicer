///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// These tests exercise plugins/activated.ini as an independent integrity
// boundary. Package installation and loader startup remain integration tests
// in test_plugin_repository.cpp and test_plugin_loader_diagnostics.cpp.

#include <catch2/catch.hpp>

#include <algorithm>
#include <iterator>
#include <string>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include "libslic3r/Plugins/PluginActivationConfig.hpp"
#include "libslic3r/Utils.hpp"

#ifdef _WIN32
#include <Windows.h>
#endif

namespace {

class ScopedActivationDirectories {
public:
    ScopedActivationDirectories(const boost::filesystem::path &resources_directory,
                                const boost::filesystem::path &data_directory);
    ~ScopedActivationDirectories();

private:
    std::string m_previous_resources_directory;
    std::string m_previous_data_directory;
};

std::string read_text_file(const boost::filesystem::path &path);

ScopedActivationDirectories::ScopedActivationDirectories(
    const boost::filesystem::path &resources_directory,
    const boost::filesystem::path &data_directory)
    : m_previous_resources_directory(Slic3r::resources_dir())
    , m_previous_data_directory(Slic3r::has_data_dir() ? Slic3r::data_dir() : std::string())
{
    Slic3r::set_resources_dir(resources_directory.string());
    Slic3r::set_data_dir(data_directory.string());
}

ScopedActivationDirectories::~ScopedActivationDirectories()
{
    Slic3r::set_resources_dir(m_previous_resources_directory);
    Slic3r::set_data_dir(m_previous_data_directory);
}

std::string read_text_file(const boost::filesystem::path &path)
{
    boost::nowide::ifstream stream(path.string(), std::ios::in | std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("Legacy installed plugin version is read as both package and slicer versions",
          "[plugins][repository][activation]")
{
    const boost::filesystem::path root = boost::filesystem::temp_directory_path() /
                                         boost::filesystem::unique_path("slic3r-plugin-activation-%%%%-%%%%");
    const boost::filesystem::path config_path = root / "activated.ini";
    boost::filesystem::create_directories(root);
    {
        boost::nowide::ofstream stream(config_path.string());
        stream << "[installed]\nexample.plugin = 1.2.3.4\n\n[activated]\nexample.id = 1\n";
    }

    Slic3r::PluginActivationConfig config;
    std::string error_message;
    REQUIRE(Slic3r::read_plugin_activation_config(config_path, config, error_message));
    CHECK(config.installed["example.plugin"].package_version == "1.2.3.4");
    CHECK(config.installed["example.plugin"].slicer_version == "1.2.3.4");
    CHECK(config.activated["example.id"]);
    boost::filesystem::remove_all(root);
}

TEST_CASE("Plugin activation tolerant reader keeps valid entries and reports every semantic error",
          "[plugins][repository][activation]")
{
    const boost::filesystem::path root = boost::filesystem::temp_directory_path() /
                                         boost::filesystem::unique_path("slic3r-plugin-activation-%%%%-%%%%");
    const boost::filesystem::path config_path = root / "activated.ini";
    boost::filesystem::create_directories(root);
    {
        boost::nowide::ofstream stream(config_path.string());
        stream << "[installed]\n"
               << "valid.package = 1.2.3\n"
               << "valid.package.slicer_version = 2.7.63.0\n"
               << "invalid.version = not-semver\n"
               << "invalid.version.slicer_version = 2.7.63.0\n"
               << "invalid.slicer = 1.0.0\n"
               << "invalid.slicer.slicer_version = not-semver\n"
               << "orphan.package.slicer_version = 2.7.63.0\n"
               << "duplicate.package = 1.0.0\n"
               << "duplicate.package = 2.0.0\n"
               << "../unsafe = 1.0.0\n\n"
               << "[activated]\n"
               << "valid.plugin = yes\n"
               << "disabled.plugin = off\n"
               << "invalid.activation = perhaps\n"
               << "rejected.plugin = 1\n"
               << "duplicate.activation = 1\n"
               << "duplicate.activation = 0\n\n"
               << "[plugin_packages]\n"
               << "valid.plugin = valid.package\n"
               << "rejected.plugin = invalid.version\n"
               << "invalid.association = ../unsafe\n"
               << "duplicate.provider = valid.package\n"
               << "duplicate.provider = other.package\n";
    }
    const std::string original_contents = read_text_file(config_path);

    const Slic3r::PluginActivationConfigReadResult result =
        Slic3r::read_plugin_activation_config_tolerant(config_path);
    CHECK(result.status == Slic3r::PluginActivationConfigStatus::PartiallyValid);
    REQUIRE(result.config.installed.size() == 1);
    CHECK(result.config.installed.count("valid.package") == 1);
    CHECK(result.config.activated.at("valid.plugin"));
    CHECK_FALSE(result.config.activated.at("disabled.plugin"));
    CHECK(result.config.activated.count("invalid.activation") == 0);
    CHECK(result.config.activated.count("rejected.plugin") == 0);
    CHECK(result.config.plugin_packages.at("valid.plugin") == "valid.package");
    CHECK(result.config.plugin_packages.count("rejected.plugin") == 0);
    CHECK(result.issues.size() >= 8);
    CHECK(std::find(result.rejected_packages.begin(), result.rejected_packages.end(),
                    "invalid.version") != result.rejected_packages.end());
    CHECK(std::find(result.rejected_packages.begin(), result.rejected_packages.end(),
                    "invalid.slicer") != result.rejected_packages.end());
    CHECK(read_text_file(config_path) == original_contents);

    // Mutation APIs use the strict facade and must not rewrite a value after
    // silently dropping entries from it.
    Slic3r::PluginActivationConfig strict_config;
    std::string error_message;
    CHECK_FALSE(Slic3r::read_plugin_activation_config(config_path, strict_config, error_message));
    CHECK(strict_config.installed.empty());
    CHECK_FALSE(error_message.empty());
    boost::filesystem::remove_all(root);
}

TEST_CASE("Plugin activation desired package section is mandatory but may be empty",
          "[plugins][repository][activation]")
{
    const boost::filesystem::path root = boost::filesystem::temp_directory_path() /
                                         boost::filesystem::unique_path("slic3r-plugin-activation-%%%%-%%%%");
    const boost::filesystem::path config_path = root / "activated.ini";
    boost::filesystem::create_directories(root);
    {
        boost::nowide::ofstream stream(config_path.string());
        stream << "[activated]\nexample.plugin = 1\n";
    }
    CHECK(Slic3r::read_plugin_activation_config_tolerant(config_path).status ==
          Slic3r::PluginActivationConfigStatus::Invalid);

    {
        boost::nowide::ofstream stream(config_path.string(), std::ios::out | std::ios::trunc);
        stream << "[installed]\n\n[activated]\nexample.plugin = 1\n";
    }
    const Slic3r::PluginActivationConfigReadResult empty_installed =
        Slic3r::read_plugin_activation_config_tolerant(config_path);
    CHECK(empty_installed.status == Slic3r::PluginActivationConfigStatus::Valid);
    CHECK(empty_installed.config.installed.empty());
    CHECK(empty_installed.config.activated.at("example.plugin"));
    boost::filesystem::remove_all(root);
}

TEST_CASE("Plugin activation configuration preserves package providers",
          "[plugins][repository][activation]")
{
    const boost::filesystem::path root = boost::filesystem::temp_directory_path() /
                                         boost::filesystem::unique_path("slic3r-plugin-activation-%%%%-%%%%");
    const boost::filesystem::path config_path = root / "activated.ini";
    Slic3r::PluginActivationConfig written;
    written.activated["example.first"] = true;
    written.activated["example.second"] = true;
    written.plugin_packages["example.first"] = "example.package";
    written.plugin_packages["example.second"] = "example.package";
    written.installed["example.package"] = {"1.2.3", "2.7.63.0"};

    boost::filesystem::create_directories(root);
    {
        boost::nowide::ofstream previous(config_path.string());
        previous << "[installed]\nobsolete.package = 0.1.0\n"
                 << "obsolete.package.slicer_version = 2.0.0\n\n"
                 << "[activated]\nobsolete.plugin = 1\n";
    }

    std::string error_message;
    REQUIRE(Slic3r::write_plugin_activation_config(config_path, written, error_message));
    Slic3r::PluginActivationConfig read;
    REQUIRE(Slic3r::read_plugin_activation_config(config_path, read, error_message));
    CHECK(read.activated == written.activated);
    CHECK(read.plugin_packages == written.plugin_packages);
    REQUIRE(read.installed.size() == 1);
    const Slic3r::PluginInstalledVersion &installed = read.installed.at("example.package");
    CHECK(installed.package_version == "1.2.3");
    CHECK(installed.slicer_version == "2.7.63.0");

    const std::string contents = read_text_file(config_path);
    CHECK(contents.find("[plugin_packages]") != std::string::npos);
    CHECK(contents.find("example.first = example.package") != std::string::npos);
    CHECK(contents.find("obsolete.package") == std::string::npos);

    // A completed replacement consumes both sibling work files.
    for (boost::filesystem::directory_iterator it(root), end; it != end; ++it) {
        const std::string filename = it->path().filename().string();
        CHECK(filename.find(".activated.ini.replacement-") == std::string::npos);
        CHECK(filename.find(".activated.ini.previous-") == std::string::npos);
        CHECK(filename.find(".transaction-backup-") == std::string::npos);
    }
    boost::filesystem::remove_all(root);
}

TEST_CASE("Plugin activation configuration rejects a non-file destination without leftovers",
          "[plugins][repository][activation]")
{
    const boost::filesystem::path root = boost::filesystem::temp_directory_path() /
                                         boost::filesystem::unique_path("slic3r-plugin-activation-%%%%-%%%%");
    const boost::filesystem::path config_path = root / "activated.ini";
    boost::filesystem::create_directories(config_path);

    Slic3r::PluginActivationConfig config;
    config.activated["example.plugin"] = true;
    std::string error_message;
    CHECK_FALSE(Slic3r::write_plugin_activation_config(config_path, config, error_message));
    CHECK_FALSE(error_message.empty());
    CHECK(boost::filesystem::is_directory(config_path));

    // Refusing the destination also removes the complete staging file without
    // disturbing the directory which made publication invalid.
    for (boost::filesystem::directory_iterator it(root), end; it != end; ++it) {
        const std::string filename = it->path().filename().string();
        CHECK(filename.find(".activated.ini.replacement-") == std::string::npos);
        CHECK(filename.find(".activated.ini.previous-") == std::string::npos);
        CHECK(filename.find(".transaction-backup-") == std::string::npos);
    }
    boost::filesystem::remove_all(root);
}

TEST_CASE("Plugin activation configuration accepts files without package providers",
          "[plugins][repository][activation]")
{
    const boost::filesystem::path root = boost::filesystem::temp_directory_path() /
                                         boost::filesystem::unique_path("slic3r-plugin-activation-%%%%-%%%%");
    const boost::filesystem::path config_path = root / "activated.ini";
    boost::filesystem::create_directories(root);
    {
        boost::nowide::ofstream stream(config_path.string());
        stream << "[installed]\n\n[activated]\nlegacy.plugin = 1\n";
    }

    Slic3r::PluginActivationConfig config;
    std::string error_message;
    REQUIRE(Slic3r::read_plugin_activation_config(config_path, config, error_message));
    CHECK(config.activated["legacy.plugin"]);
    CHECK(config.plugin_packages.empty());
    boost::filesystem::remove_all(root);
}

TEST_CASE("Plugin activation repair publishes the complete default configuration",
          "[plugins][repository][activation]")
{
    const boost::filesystem::path root = boost::filesystem::temp_directory_path() /
                                         boost::filesystem::unique_path("slic3r-plugin-repair-%%%%-%%%%");
    const boost::filesystem::path resources_directory = root / "resources";
    const boost::filesystem::path data_directory = root / "data";
    const boost::filesystem::path default_config = resources_directory / "plugins" / "default_activated.ini";
    const boost::filesystem::path user_config = Slic3r::plugin_activation_config_path(data_directory);
    const boost::filesystem::path live_marker = data_directory / "plugins" / "keep.package" / "marker.txt";
    boost::filesystem::create_directories(default_config.parent_path());
    boost::filesystem::create_directories(user_config.parent_path());
    {
        boost::nowide::ofstream stream(default_config.string());
        stream << "; complete resource configuration\n"
               << "[installed]\ndefault.package = 1.0.0\n"
               << "default.package.slicer_version = 2.7.63.0\n\n"
               << "[activated]\ndefault.plugin = 1\n\n"
               << "[plugin_packages]\ndefault.plugin = default.package\n";
    }
    {
        boost::nowide::ofstream stream(user_config.string());
        stream << "[activated\nbroken.plugin = 1\n";
    }
    boost::filesystem::create_directories(live_marker.parent_path());
    {
        boost::nowide::ofstream stream(live_marker.string());
        stream << "must remain";
    }
    ScopedActivationDirectories directories(resources_directory, data_directory);

    std::string error_message;
    REQUIRE(Slic3r::replace_plugin_activation_config_with_defaults(user_config, error_message));
    CHECK(error_message.empty());
    CHECK(read_text_file(user_config) == read_text_file(default_config));
    CHECK(read_text_file(live_marker) == "must remain");

    Slic3r::PluginActivationConfig repaired;
    REQUIRE(Slic3r::read_plugin_activation_config(user_config, repaired, error_message));
    CHECK(repaired.installed.count("default.package") == 1);
    CHECK(repaired.activated.count("default.plugin") == 1);
    CHECK(repaired.plugin_packages.at("default.plugin") == "default.package");

    // Successful publication consumes both sibling work files; only the final
    // activation file and unrelated package directories remain.
    for (boost::filesystem::directory_iterator it(user_config.parent_path()), end; it != end; ++it) {
        const std::string filename = it->path().filename().string();
        CHECK(filename.find(".activated.ini.replacement-") == std::string::npos);
        CHECK(filename.find(".activated.ini.previous-") == std::string::npos);
    }

    boost::filesystem::remove_all(root);
}

TEST_CASE("Plugin activation repair preserves the user file when defaults are invalid",
          "[plugins][repository][activation]")
{
    const boost::filesystem::path root = boost::filesystem::temp_directory_path() /
                                         boost::filesystem::unique_path("slic3r-plugin-repair-%%%%-%%%%");
    const boost::filesystem::path resources_directory = root / "resources";
    const boost::filesystem::path data_directory = root / "data";
    const boost::filesystem::path default_config = resources_directory / "plugins" / "default_activated.ini";
    const boost::filesystem::path user_config = Slic3r::plugin_activation_config_path(data_directory);
    boost::filesystem::create_directories(default_config.parent_path());
    boost::filesystem::create_directories(user_config.parent_path());
    {
        boost::nowide::ofstream stream(default_config.string());
        stream << "[activated\ndefault.plugin = 1\n";
    }
    {
        boost::nowide::ofstream stream(user_config.string());
        stream << "[activated\nbroken.plugin = 1\n";
    }
    const std::string original_user_contents = read_text_file(user_config);
    ScopedActivationDirectories directories(resources_directory, data_directory);

    std::string error_message;
    CHECK_FALSE(Slic3r::replace_plugin_activation_config_with_defaults(user_config, error_message));
    CHECK_FALSE(error_message.empty());
    CHECK(read_text_file(user_config) == original_user_contents);

    boost::filesystem::remove_all(root);
}

#ifdef _WIN32
TEST_CASE("Plugin activation repair restores the user file when publication fails",
          "[plugins][repository][activation]")
{
    const boost::filesystem::path root = boost::filesystem::temp_directory_path() /
                                         boost::filesystem::unique_path("slic3r-plugin-repair-%%%%-%%%%");
    const boost::filesystem::path resources_directory = root / "resources";
    const boost::filesystem::path data_directory = root / "data";
    const boost::filesystem::path default_config = resources_directory / "plugins" / "default_activated.ini";
    const boost::filesystem::path user_config = Slic3r::plugin_activation_config_path(data_directory);
    boost::filesystem::create_directories(default_config.parent_path());
    boost::filesystem::create_directories(user_config.parent_path());
    {
        boost::nowide::ofstream stream(default_config.string());
        stream << "[installed]\n\n[activated]\ndefault.plugin = 1\n";
    }
    {
        boost::nowide::ofstream stream(user_config.string());
        stream << "[activated\nbroken.plugin = 1\n";
    }
    const std::string original_user_contents = read_text_file(user_config);
    ScopedActivationDirectories directories(resources_directory, data_directory);

    // Denying FILE_SHARE_DELETE makes the publication rename fail after the
    // complete replacement has been staged, which exercises the rollback path.
    const HANDLE locked_file = ::CreateFileW(user_config.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ,
                                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(locked_file != INVALID_HANDLE_VALUE);
    std::string error_message;
    CHECK_FALSE(Slic3r::replace_plugin_activation_config_with_defaults(user_config, error_message));
    CHECK_FALSE(error_message.empty());
    ::CloseHandle(locked_file);
    CHECK(read_text_file(user_config) == original_user_contents);

    boost::filesystem::remove_all(root);
}
#endif

TEST_CASE("Plugin activation configuration writes no removal section",
          "[plugins][repository][activation]")
{
    const boost::filesystem::path root = boost::filesystem::temp_directory_path() /
                                         boost::filesystem::unique_path("slic3r-plugin-removal-%%%%-%%%%");
    const boost::filesystem::path config_path = root / "activated.ini";
    boost::filesystem::create_directories(root);
    {
        boost::nowide::ofstream stream(config_path.string());
        stream << "[installed]\n"
               << "example.plugin = 1.2.3.4\n"
               << "example.plugin.slicer_version = 2.7.0.0\n\n"
               << "[activated]\nexample.id = 1\n";
    }

    Slic3r::PluginActivationConfig config;
    std::string error_message;
    REQUIRE(Slic3r::read_plugin_activation_config(config_path, config, error_message));
    CHECK(config.installed.count("example.plugin") == 1);
    CHECK(config.activated["example.id"]);
    REQUIRE(Slic3r::write_plugin_activation_config(config_path, config, error_message));
    CHECK(read_text_file(config_path).find("[removed]") == std::string::npos);
    boost::filesystem::remove_all(root);
}

TEST_CASE("Plugin activation migration splits an obsolete plugin id",
          "[plugins][repository][activation][migration]")
{
    const std::string obsolete = "layer_extrusion_edit.speed_acceleration.default";
    const std::string speed = "layer_extrusion_edit.speed.default";
    const std::string acceleration = "layer_extrusion_edit.acceleration.default";

    SECTION("the old enabled state initializes both missing successors") {
        Slic3r::PluginActivationConfig config;
        config.activated[obsolete] = true;
        config.plugin_packages[obsolete] = "obsolete.package";

        REQUIRE(Slic3r::migrate_plugin_activation_id(config, obsolete, {speed, acceleration}));
        CHECK(config.activated.count(obsolete) == 0);
        CHECK(config.plugin_packages.count(obsolete) == 0);
        CHECK(config.activated.at(speed));
        CHECK(config.activated.at(acceleration));
    }

    SECTION("an old disabled state stays disabled before defaults are merged") {
        Slic3r::PluginActivationConfig config;
        config.activated[obsolete] = false;

        REQUIRE(Slic3r::migrate_plugin_activation_id(config, obsolete, {speed, acceleration}));
        CHECK_FALSE(config.activated.at(speed));
        CHECK_FALSE(config.activated.at(acceleration));
    }

    SECTION("explicit successor choices remain authoritative") {
        Slic3r::PluginActivationConfig config;
        config.activated[obsolete] = true;
        config.activated[speed] = false;

        REQUIRE(Slic3r::migrate_plugin_activation_id(config, obsolete, {speed, acceleration}));
        CHECK_FALSE(config.activated.at(speed));
        CHECK(config.activated.at(acceleration));
    }
}

TEST_CASE("Plugin activation migration replaces the generic firmware with Marlin 2",
          "[plugins][repository][activation][migration][gcode]")
{
    Slic3r::PluginActivationConfig config;
    config.activated["gcode.firmware.default"] = true;
    config.plugin_packages["gcode.firmware.default"] = "obsolete.package";

    REQUIRE(Slic3r::migrate_plugin_activation_id(
        config, "gcode.firmware.default", {"gcode.firmware.marlin2"}));
    CHECK(config.activated.count("gcode.firmware.default") == 0);
    CHECK(config.plugin_packages.count("gcode.firmware.default") == 0);
    CHECK(config.activated.at("gcode.firmware.marlin2"));
}
