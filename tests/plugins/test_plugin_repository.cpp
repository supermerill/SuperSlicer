///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// These tests exercise the package filesystem protocol without loading a DLL.
// A package is valid as soon as its version description is valid; loading the
// platform library remains the responsibility of PluginLoader.

#include <catch2/catch.hpp>

#include <string>
#include <utility>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include "libslic3r/Plugins/PluginRepository.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/miniz_extension.hpp"

namespace {

struct ZipEntry {
    std::string name;
    std::string contents;
};

class ScopedPluginRepositoryDirectories {
public:
    ScopedPluginRepositoryDirectories(const boost::filesystem::path &resources_directory,
                                      const boost::filesystem::path &data_directory);
    ~ScopedPluginRepositoryDirectories();

private:
    std::string m_previous_resources_directory;
    std::string m_previous_data_directory;
};

bool write_zip(const boost::filesystem::path &archive_path, const std::vector<ZipEntry> &entries);
std::string description_contents(const std::string &package_name,
                                 const std::string &package_version,
                                 const std::string &slicer_version);
void write_description(const boost::filesystem::path &package_root,
                       const std::string &package_name,
                       const std::string &package_version,
                       const std::string &slicer_version);
const char *plugin_library_filename();

ScopedPluginRepositoryDirectories::ScopedPluginRepositoryDirectories(const boost::filesystem::path &resources_directory,
                                                                       const boost::filesystem::path &data_directory)
    : m_previous_resources_directory(Slic3r::resources_dir())
    , m_previous_data_directory(Slic3r::data_dir())
{
    Slic3r::set_resources_dir(resources_directory.string());
    Slic3r::set_data_dir(data_directory.string());
}

ScopedPluginRepositoryDirectories::~ScopedPluginRepositoryDirectories()
{
    Slic3r::set_resources_dir(m_previous_resources_directory);
    Slic3r::set_data_dir(m_previous_data_directory);
}

bool write_zip(const boost::filesystem::path &archive_path, const std::vector<ZipEntry> &entries)
{
    mz_zip_archive archive = {};
    if (!Slic3r::open_zip_writer(&archive, archive_path.string()))
        return false;

    bool success = true;
    for (const ZipEntry &entry : entries) {
        if (!mz_zip_writer_add_mem(&archive, entry.name.c_str(), entry.contents.data(), entry.contents.size(),
                                   MZ_BEST_COMPRESSION)) {
            success = false;
            break;
        }
    }
    return Slic3r::close_zip_writer(&archive) && success;
}

std::string description_contents(const std::string &package_name,
                                 const std::string &package_version,
                                 const std::string &slicer_version)
{
    return "[plugin]\n"
           "id = " + package_name + "\n"
           "name = " + package_name + "\n"
           "full_name = " + package_name + "\n"
           "type = local\n"
           "package_version = " + package_version + "\n"
           "slicer_version = " + slicer_version + "\n";
}

void write_description(const boost::filesystem::path &package_root,
                       const std::string &package_name,
                       const std::string &package_version,
                       const std::string &slicer_version)
{
    boost::filesystem::create_directories(package_root);
    boost::nowide::ofstream stream((package_root / "description.ini").string());
    stream << description_contents(package_name, package_version, slicer_version);
}

const char *plugin_library_filename()
{
#ifdef _WIN32
    return "plugin.dll";
#elif defined(__APPLE__)
    return "plugin.dylib";
#else
    return "plugin.so";
#endif
}

} // namespace

TEST_CASE("Plugin bundles are cached with their versioned description", "[plugins][repository]")
{
    const boost::filesystem::path root = boost::filesystem::temp_directory_path() /
                                         boost::filesystem::unique_path("slic3r-plugin-repository-%%%%-%%%%");
    const boost::filesystem::path resources_directory = root / "resources";
    const boost::filesystem::path data_directory = root / "data";
    const boost::filesystem::path archive_directory = resources_directory / "plugins";
    const std::string package_name = "example_plugin";
    const std::string package_version = "1.2.3.4";
    const std::string slicer_version = "2.7.63.0";
    const boost::filesystem::path archive_path = archive_directory / (package_name + "_" + package_version + "_" + slicer_version + ".zip");
    boost::filesystem::create_directories(archive_directory);
    REQUIRE(write_zip(archive_path, {{plugin_library_filename(), "library"},
                                     {"description.ini", description_contents(package_name, package_version, slicer_version)}}));

    std::string error_message;
    CHECK(Slic3r::prepare_plugin_bundle_cache(resources_directory, data_directory, error_message));
    const boost::filesystem::path cached_package = data_directory / "cache/plugins" /
        (package_name + "_" + package_version + "_" + slicer_version);
    CHECK(boost::filesystem::exists(cached_package / plugin_library_filename()));
    CHECK(boost::filesystem::exists(cached_package / "description.ini"));

    // A valid cache remains usable even if the source archive is no longer
    // available, which is how cached packages survive application updates.
    boost::filesystem::remove(archive_path);
    CHECK(Slic3r::prepare_plugin_bundle_cache(resources_directory, data_directory, error_message));

    boost::filesystem::remove_all(root);
}

TEST_CASE("Plugin bundle extraction rejects malformed archives and Zip Slip", "[plugins][repository]")
{
    const boost::filesystem::path root = boost::filesystem::temp_directory_path() /
                                         boost::filesystem::unique_path("slic3r-plugin-repository-%%%%-%%%%");
    const boost::filesystem::path resources_directory = root / "resources";
    const boost::filesystem::path data_directory = root / "data";
    const boost::filesystem::path archive_directory = resources_directory / "plugins";
    boost::filesystem::create_directories(archive_directory);

    REQUIRE(write_zip(archive_directory / "invalid-name.zip", {{plugin_library_filename(), "library"}}));
    REQUIRE(write_zip(archive_directory / "unsafe_1.2.3.4_2.7.63.0.zip", {{"../outside.txt", "unsafe"}}));

    std::string error_message;
    CHECK_FALSE(Slic3r::prepare_plugin_bundle_cache(resources_directory, data_directory, error_message));
    CHECK_FALSE(boost::filesystem::exists(root / "outside.txt"));
    CHECK_FALSE(boost::filesystem::exists(data_directory / "cache/plugins/unsafe_1.2.3.4_2.7.63.0"));

    boost::filesystem::remove_all(root);
}

TEST_CASE("Plugin bundle extraction rejects a mismatched supplied manifest", "[plugins][repository]")
{
    const boost::filesystem::path root = boost::filesystem::temp_directory_path() /
                                         boost::filesystem::unique_path("slic3r-plugin-repository-%%%%-%%%%");
    const boost::filesystem::path resources_directory = root / "resources";
    const boost::filesystem::path data_directory = root / "data";
    const boost::filesystem::path archive_directory = resources_directory / "plugins";
    const std::string package_name = "example.plugin";
    const std::string package_version = "1.2.3.4";
    const std::string slicer_version = "2.7.63.0";
    boost::filesystem::create_directories(archive_directory);
    REQUIRE(write_zip(archive_directory / (package_name + "_" + package_version + "_" + slicer_version + ".zip"),
                      {{plugin_library_filename(), "library"},
                       {"description.ini", description_contents("another.plugin", package_version, slicer_version)}}));

    std::string error_message;
    CHECK_FALSE(Slic3r::prepare_plugin_bundle_cache(resources_directory, data_directory, error_message));
    CHECK_FALSE(boost::filesystem::exists(data_directory / "cache/plugins" /
                                          (package_name + "_" + package_version + "_" + slicer_version)));

    boost::filesystem::remove_all(root);
}

TEST_CASE("Plugin installation is deferred and preserves the previous package on failure", "[plugins][repository]")
{
    const boost::filesystem::path root = boost::filesystem::temp_directory_path() /
                                         boost::filesystem::unique_path("slic3r-plugin-repository-%%%%-%%%%");
    const boost::filesystem::path data_directory = root / "data";
    const std::string package_name = "example.plugin";
    const std::string first_version = "1.2.3.4";
    const std::string second_version = "1.2.3.5";
    const std::string slicer_version = "2.7.63.0";
    const boost::filesystem::path first_root = data_directory / "cache/plugins" /
        (package_name + "_" + first_version + "_" + slicer_version);
    const boost::filesystem::path second_root = data_directory / "cache/plugins" /
        (package_name + "_" + second_version + "_" + slicer_version);
    write_description(first_root, package_name, first_version, slicer_version);
    write_description(second_root, package_name, second_version, slicer_version);
    {
        boost::nowide::ofstream stream(
            (first_root / plugin_library_filename()).string());
        stream << "library";
    }
    {
        boost::nowide::ofstream stream(
            (second_root / plugin_library_filename()).string());
        stream << "library";
    }

    Slic3r::PluginActivationConfig config;
    config.installed[package_name] = {first_version, slicer_version};
    std::string error_message;
    REQUIRE(Slic3r::install_requested_plugin_packages(data_directory, config, error_message));
    CHECK(boost::filesystem::exists(data_directory / "plugins" / package_name / "description.ini"));
    const boost::filesystem::path installed_package = data_directory / "plugins" / package_name;
    {
        boost::nowide::ofstream stream((installed_package / "local-marker.txt").string());
        stream << "preserved";
    }
    REQUIRE(Slic3r::install_requested_plugin_packages(data_directory, config, error_message));
    CHECK(boost::filesystem::exists(installed_package / "local-marker.txt"));

    config.installed[package_name] = {second_version, slicer_version};
    REQUIRE(Slic3r::install_requested_plugin_packages(data_directory, config, error_message));
    std::string installed_manifest;
    {
        boost::nowide::ifstream stream((data_directory / "plugins" / package_name / "description.ini").string());
        for (int line = 0; line < 6; ++line)
            std::getline(stream, installed_manifest);
    }
    CHECK(installed_manifest == "package_version = " + second_version);

    config.installed[package_name] = {"9.9.9.9", slicer_version};
    CHECK_FALSE(Slic3r::install_requested_plugin_packages(data_directory, config, error_message));
    CHECK(boost::filesystem::exists(installed_package / "description.ini"));
    std::string installed_manifest_after_failure;
    {
        boost::nowide::ifstream stream((installed_package / "description.ini").string());
        for (int line = 0; line < 6; ++line)
            std::getline(stream, installed_manifest_after_failure);
    }
    CHECK(installed_manifest_after_failure == "package_version = " + second_version);

    boost::filesystem::remove_all(root);
}

TEST_CASE("Requesting a cached plugin version preserves activation settings", "[plugins][repository]")
{
    const boost::filesystem::path root = boost::filesystem::temp_directory_path() /
                                         boost::filesystem::unique_path("slic3r-plugin-repository-%%%%-%%%%");
    const boost::filesystem::path resources_directory = root / "resources";
    const boost::filesystem::path data_directory = root / "data";
    const std::string package_name = "example.plugin";
    const std::string package_version = "1.2.3.4";
    const std::string slicer_version = "2.7.63.0";
    const boost::filesystem::path archive_directory = resources_directory / "plugins";
    boost::filesystem::create_directories(archive_directory);
    REQUIRE(write_zip(archive_directory / (package_name + "_" + package_version + "_" + slicer_version + ".zip"),
                      {{plugin_library_filename(), "library"},
                       {"description.ini", description_contents(package_name, package_version, slicer_version)}}));

    const boost::filesystem::path default_config = archive_directory / "default_activated.ini";
    {
        boost::nowide::ofstream stream(default_config.string());
        stream << "[activated]\nexample.plugin.id = 1\n";
    }

    ScopedPluginRepositoryDirectories directories(resources_directory, data_directory);
    std::string error_message;
    REQUIRE(Slic3r::request_plugin_install(package_name, package_version, slicer_version, error_message));

    Slic3r::PluginActivationConfig config;
    REQUIRE(Slic3r::read_plugin_activation_config(data_directory / "plugins" / "activated.ini", config, error_message));
    CHECK(config.installed[package_name].package_version == package_version);
    CHECK(config.installed[package_name].slicer_version == slicer_version);
    CHECK(config.activated["example.plugin.id"]);

    boost::filesystem::remove_all(root);
}

TEST_CASE("Repository descriptions and GitHub tags share one version protocol", "[plugins][repository]")
{
    Slic3r::RepositoryDescription vendor;
    Slic3r::RepositoryDescription plugin;
    std::string error_message;
    REQUIRE(Slic3r::parse_repository_description(
        "[vendor]\nid = Creality_Example\nname = Creality\nfull_name = Creality\n"
        "config_update_rest = SuperSlicer-org/Creality-Profile\nslicer = SuperSlicer\n",
        Slic3r::RepositoryPackageType::Vendor, vendor, error_message));
    CHECK(vendor.id == "Creality_Example");
    CHECK(vendor.config_update_rest == "SuperSlicer-org/Creality-Profile");
    CHECK(Slic3r::repository_package_cache_path("data", Slic3r::RepositoryPackageType::Vendor,
                                                 vendor.id, "1.2.3.4", "2.7.63.0") ==
          boost::filesystem::path("data/cache/vendor/Creality_Example_1.2.3.4_2.7.63.0"));

    REQUIRE(Slic3r::parse_repository_description(
        "[plugin]\nid = postprocess.truc\nname = truc\nfull_name = Truc\n"
        "package_version = 1.0.0.0\nslicer_version = 2.7.63.0\n",
        Slic3r::RepositoryPackageType::Plugin, plugin, error_message));
    CHECK(plugin.id == "postprocess.truc");
    CHECK(plugin.package_version == "1.0.0.0");

    CHECK_FALSE(Slic3r::parse_repository_description(
        "[vendor]\nid = vendor\n\n[plugin]\nid = plugin\n", Slic3r::RepositoryPackageType::Plugin,
        plugin, error_message));

    std::vector<Slic3r::RepositoryPackageVersion> versions;
    REQUIRE(Slic3r::parse_repository_versions(
        "[{\"name\":\"1.2.3.4=2.7.63.0\",\"zipball_url\":\"zip\","
        "\"commit\":{\"sha\":\"sha\",\"url\":\"commit\"}},"
        "{\"name\":\"not-a-version\"}]", versions, error_message));
    REQUIRE(versions.size() == 1);
    CHECK(versions.front().package_version == "1.2.3.4");
    CHECK(versions.front().slicer_version == "2.7.63.0");
}

TEST_CASE("Legacy installed plugin version is read as both package and slicer versions", "[plugins][repository]")
{
    const boost::filesystem::path root = boost::filesystem::temp_directory_path() /
                                         boost::filesystem::unique_path("slic3r-plugin-repository-%%%%-%%%%");
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
