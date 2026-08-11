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
#include "libslic3r/Updater/RepositoryPackageCache.hpp"
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
std::string read_text_file(const boost::filesystem::path &path);

ScopedPluginRepositoryDirectories::ScopedPluginRepositoryDirectories(const boost::filesystem::path &resources_directory,
                                                                       const boost::filesystem::path &data_directory)
    : m_previous_resources_directory(Slic3r::resources_dir())
    , m_previous_data_directory(Slic3r::has_data_dir() ? Slic3r::data_dir() : std::string())
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

    // Finalization writes the central directory required by ZIP readers.
    const bool finalized = success && mz_zip_writer_finalize_archive(&archive);
    const bool closed = Slic3r::close_zip_writer(&archive);
    return success && finalized && closed;
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

std::string read_text_file(const boost::filesystem::path &path)
{
    boost::nowide::ifstream stream(path.string(), std::ios::in | std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
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
    const boost::filesystem::path cached_package = Slic3r::repository_package_cache_path(
        data_directory, Slic3r::RepositoryPackageType::Plugin,
        package_name, package_version, slicer_version);
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
    CHECK_FALSE(boost::filesystem::exists(Slic3r::repository_package_cache_path(
        data_directory, Slic3r::RepositoryPackageType::Plugin,
        "unsafe", "1.2.3.4", "2.7.63.0")));

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
    CHECK_FALSE(boost::filesystem::exists(Slic3r::repository_package_cache_path(
        data_directory, Slic3r::RepositoryPackageType::Plugin,
        package_name, package_version, slicer_version)));

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
    const boost::filesystem::path first_root = Slic3r::repository_package_cache_path(
        data_directory, Slic3r::RepositoryPackageType::Plugin,
        package_name, first_version, slicer_version);
    const boost::filesystem::path second_root = Slic3r::repository_package_cache_path(
        data_directory, Slic3r::RepositoryPackageType::Plugin,
        package_name, second_version, slicer_version);
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
    std::vector<std::string> warnings;
    std::string error_message;
    REQUIRE(Slic3r::apply_requested_plugin_package_changes(data_directory, config, warnings, error_message));
    CHECK(boost::filesystem::exists(data_directory / "plugins" / package_name / "description.ini"));
    const boost::filesystem::path installed_package = data_directory / "plugins" / package_name;
    {
        boost::nowide::ofstream stream((installed_package / "local-marker.txt").string());
        stream << "preserved";
    }
    REQUIRE(Slic3r::apply_requested_plugin_package_changes(data_directory, config, warnings, error_message));
    CHECK(boost::filesystem::exists(installed_package / "local-marker.txt"));

    config.installed[package_name] = {second_version, slicer_version};
    REQUIRE(Slic3r::apply_requested_plugin_package_changes(data_directory, config, warnings, error_message));
    std::string installed_manifest;
    {
        boost::nowide::ifstream stream((data_directory / "plugins" / package_name / "description.ini").string());
        for (int line = 0; line < 6; ++line)
            std::getline(stream, installed_manifest);
    }
    CHECK(installed_manifest == "package_version = " + second_version);

    config.installed[package_name] = {"9.9.9.9", slicer_version};
    CHECK_FALSE(Slic3r::apply_requested_plugin_package_changes(data_directory, config, warnings, error_message));
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
    const boost::filesystem::path default_config = resources_directory / "plugins" / "default_activated.ini";
    boost::filesystem::create_directories(default_config.parent_path());
    {
        boost::nowide::ofstream stream(default_config.string());
        stream << "[activated]\nexample.plugin.id = 1\n";
    }

    // request_plugin_install() accepts only packages that were fully validated
    // before the current process loaded any DLL. Build that cache directly so
    // this test isolates scheduling from archive extraction.
    Slic3r::RepositoryPackageCache cache(data_directory, Slic3r::plugin_repository_cache_adapter());
    bool purged = false;
    std::string layout_error;
    REQUIRE(cache.prepare_layout(purged, layout_error));
    const boost::filesystem::path cached_package = Slic3r::repository_package_cache_path(
        data_directory, Slic3r::RepositoryPackageType::Plugin,
        package_name, package_version, slicer_version);
    write_description(cached_package, package_name, package_version, slicer_version);
    {
        boost::nowide::ofstream stream((cached_package / plugin_library_filename()).string(), std::ios::binary);
        stream << "library";
    }

    ScopedPluginRepositoryDirectories directories(resources_directory, data_directory);
    std::string error_message;
    REQUIRE(Slic3r::request_plugin_install(package_name, package_version, slicer_version, error_message));
    CHECK_FALSE(boost::filesystem::exists(data_directory / "plugins" / package_name));

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
          boost::filesystem::path("data/cache/vendor/Creality_Example/1.2.3.4=2.7.63.0"));

    REQUIRE(Slic3r::parse_repository_description(
        "[plugin]\nid = postprocess.truc\nname = truc\nfull_name = Truc\n"
        "package_version = 1.0.0.0\nslicer_version = 2.7.63.0\n",
        Slic3r::RepositoryPackageType::Plugin, plugin, error_message));
    CHECK(plugin.id == "postprocess.truc");
    CHECK(plugin.package_version == "1.0.0.0");
    CHECK(Slic3r::repository_package_cache_path("data", Slic3r::RepositoryPackageType::Plugin,
                                                 plugin.id, "1.0.0.0", "2.7.63.0") ==
          boost::filesystem::path("data/cache/plugins/postprocess.truc/1.0.0.0=2.7.63.0"));

    CHECK_FALSE(Slic3r::parse_repository_description(
        "[vendor]\nid = vendor\n\n[plugin]\nid = plugin\n", Slic3r::RepositoryPackageType::Plugin,
        plugin, error_message));

    std::vector<Slic3r::RepositoryPackageVersion> versions;
    REQUIRE(Slic3r::parse_repository_versions(
        "[{\"name\":\"1.2.3.4=2.7.63.0\",\"zipball_url\":\"zip\","
        "\"commit\":{\"sha\":\"sha\",\"url\":\"commit\"}},"
        "{\"name\":\"2.0.0-beta.1=2.8.0-rc.1\",\"zipball_url\":\"prerelease\"},"
        "{\"name\":\"not-a-version\"}]", versions, error_message));
    REQUIRE(versions.size() == 2);
    CHECK(versions.front().package_version == "1.2.3.4");
    CHECK(versions.front().slicer_version == "2.7.63.0");
    CHECK(versions.back().package_version == "2.0.0-beta.1");
    CHECK(versions.back().slicer_version == "2.8.0-rc.1");
}

TEST_CASE("Repository package cache imports incomplete local content with defaults",
          "[plugins][repository][cache-layout]")
{
    const boost::filesystem::path root = boost::filesystem::temp_directory_path() /
                                         boost::filesystem::unique_path("slic3r-package-defaults-%%%%-%%%%");
    const boost::filesystem::path data_directory = root / "data";
    std::string error_message;

    SECTION("a bare vendor INI becomes a complete versioned package") {
        const boost::filesystem::path profile_path = root / "source" / "local_vendor.ini";
        boost::filesystem::create_directories(profile_path.parent_path());
        {
            boost::nowide::ofstream stream(profile_path.string());
            stream << "[vendor]\nid = local_vendor\n";
        }

        Slic3r::RepositoryPackageCache cache(data_directory, Slic3r::vendor_repository_cache_adapter());
        bool purged = false;
        REQUIRE(cache.prepare_layout(purged, error_message));
        Slic3r::RepositoryCachedVersion cached;
        REQUIRE(cache.cache_simple(profile_path, cached, error_message));
        CHECK(cached.directory.filename() == "1.0.0.0=1.0.0.0");
        CHECK(boost::filesystem::is_regular_file(cached.directory / "profiles" / "local_vendor.ini"));

        const std::vector<Slic3r::RepositoryCachedEntry> repositories = cache.scan();
        REQUIRE(repositories.size() == 1);
        CHECK(repositories.front().description.id == "local_vendor");
        CHECK(repositories.front().description.name == "local_vendor");
        CHECK(repositories.front().description.full_name == "local_vendor");
        CHECK(repositories.front().description.config_update_rest.empty());
        REQUIRE(repositories.front().versions.size() == 1);
    }

    SECTION("a plugin folder without a description receives local defaults") {
        const boost::filesystem::path package = root / "source" / "local_plugin";
        boost::filesystem::create_directories(package);
        {
            boost::nowide::ofstream stream((package / plugin_library_filename()).string(), std::ios::binary);
            stream << "library";
        }

        Slic3r::RepositoryPackageCache cache(data_directory, Slic3r::plugin_repository_cache_adapter());
        bool purged = false;
        REQUIRE(cache.prepare_layout(purged, error_message));
        Slic3r::RepositoryCachedVersion cached;
        REQUIRE(cache.cache_simple(package, cached, error_message));
        CHECK(cached.description.id == "local_plugin");
        CHECK(cached.description.package_version == "1.0.0.0");
        CHECK(cached.description.slicer_version == "1.0.0.0");
        CHECK(cached.description.config_update_rest.empty());
        CHECK(boost::filesystem::is_regular_file(cached.directory / "description.ini"));
        CHECK(boost::filesystem::is_regular_file(cached.directory / plugin_library_filename()));
    }

    boost::filesystem::remove_all(root);
}

TEST_CASE("Repository package cache preserves versions and selects root metadata",
          "[plugins][repository][cache-layout]")
{
    const boost::filesystem::path root = boost::filesystem::temp_directory_path() /
                                         boost::filesystem::unique_path("slic3r-package-versions-%%%%-%%%%");
    const boost::filesystem::path data_directory = root / "data";
    Slic3r::RepositoryPackageCache cache(data_directory, Slic3r::plugin_repository_cache_adapter());
    bool purged = false;
    std::string error_message;
    REQUIRE(cache.prepare_layout(purged, error_message));

    const boost::filesystem::path first = root / "first";
    write_description(first, "versioned.plugin", "1.0.0.0", "2.7.0.0");
    {
        boost::nowide::ofstream description((first / "description.ini").string(), std::ios::app);
        description << "config_update_rest = example/repository\n";
        boost::nowide::ofstream library((first / plugin_library_filename()).string(), std::ios::binary);
        library << "first";
    }
    Slic3r::RepositoryCachedVersion cached;
    REQUIRE(cache.cache_simple(first, cached, error_message));

    const boost::filesystem::path second = root / "second";
    boost::filesystem::create_directories(second);
    {
        boost::nowide::ofstream description((second / "description.ini").string());
        description << "[plugin]\nid = versioned.plugin\nname = New name\n"
                       "full_name = New full name\npackage_version = 2.0.0-beta.1\n"
                       "slicer_version = 2.7.0.0\n";
        boost::nowide::ofstream library((second / plugin_library_filename()).string(), std::ios::binary);
        library << "second";
    }
    REQUIRE(cache.cache_simple(second, cached, error_message));
    CHECK(cached.directory.filename() == "2.0.0-beta.1=2.7.0.0");

    // Reimporting one exact version atomically replaces only that directory.
    {
        boost::nowide::ofstream library((second / plugin_library_filename()).string(),
                                        std::ios::out | std::ios::binary | std::ios::trunc);
        library << "second replacement";
    }
    REQUIRE(cache.cache_simple(second, cached, error_message));
    CHECK(read_text_file(cached.directory / plugin_library_filename()) == "second replacement");
    CHECK(boost::filesystem::is_directory(cache.version_directory(
        "versioned.plugin", "1.0.0.0", "2.7.0.0")));

    const std::vector<Slic3r::RepositoryCachedEntry> repositories = cache.scan();
    REQUIRE(repositories.size() == 1);
    REQUIRE(repositories.front().versions.size() == 2);
    CHECK(repositories.front().description.name == "New name");
    CHECK(repositories.front().description.full_name == "New full name");
    CHECK(repositories.front().description.config_update_rest == "example/repository");

    boost::filesystem::remove_all(root);
}

TEST_CASE("Repository package cache sanitizes ids and rejects collisions",
          "[plugins][repository][cache-layout]")
{
    const boost::filesystem::path root = boost::filesystem::temp_directory_path() /
                                         boost::filesystem::unique_path("slic3r-package-id-%%%%-%%%%");
    const boost::filesystem::path data_directory = root / "data";
    Slic3r::RepositoryPackageCache cache(data_directory, Slic3r::plugin_repository_cache_adapter());
    bool purged = false;
    std::string error_message;
    REQUIRE(cache.prepare_layout(purged, error_message));

    const boost::filesystem::path first = root / "first";
    write_description(first, "same id", "1.0.0.0", "2.7.0.0");
    {
        boost::nowide::ofstream stream((first / plugin_library_filename()).string(), std::ios::binary);
        stream << "first";
    }
    Slic3r::RepositoryCachedVersion cached;
    REQUIRE(cache.cache_simple(first, cached, error_message));
    CHECK(cache.repository_directory("same id").filename() == "same-id");

    const boost::filesystem::path colliding = root / "colliding";
    write_description(colliding, "same-id", "1.0.0.0", "2.7.0.0");
    {
        boost::nowide::ofstream stream((colliding / plugin_library_filename()).string(), std::ios::binary);
        stream << "collision";
    }
    CHECK_FALSE(cache.cache_simple(colliding, cached, error_message));
    CHECK(error_message.find("map to the same cache directory") != std::string::npos);
    CHECK(read_text_file(cache.version_directory("same id", "1.0.0.0", "2.7.0.0") /
                         plugin_library_filename()) == "first");

    boost::filesystem::remove_all(root);
}

TEST_CASE("Repository package cache purges an obsolete layout once",
          "[plugins][repository][cache-layout]")
{
    const boost::filesystem::path root = boost::filesystem::temp_directory_path() /
                                         boost::filesystem::unique_path("slic3r-package-purge-%%%%-%%%%");
    const boost::filesystem::path data_directory = root / "data";
    const boost::filesystem::path old_file = data_directory / "cache" / "plugins" / "old-package";
    boost::filesystem::create_directories(old_file.parent_path());
    {
        boost::nowide::ofstream stream(old_file.string());
        stream << "old";
    }

    Slic3r::RepositoryPackageCache cache(data_directory, Slic3r::plugin_repository_cache_adapter());
    bool purged = false;
    std::string error_message;
    REQUIRE(cache.prepare_layout(purged, error_message));
    CHECK(purged);
    CHECK_FALSE(boost::filesystem::exists(old_file));
    CHECK(boost::filesystem::is_regular_file(cache.type_directory() / ".layout_version"));

    purged = true;
    REQUIRE(cache.prepare_layout(purged, error_message));
    CHECK_FALSE(purged);
    boost::filesystem::remove_all(root);
}

TEST_CASE("Plugin cache purge restores live packages and cancels lost requests",
          "[plugins][repository][cache-layout]")
{
    const boost::filesystem::path root = boost::filesystem::temp_directory_path() /
                                         boost::filesystem::unique_path("slic3r-plugin-purge-%%%%-%%%%");
    const boost::filesystem::path resources_directory = root / "resources";
    const boost::filesystem::path data_directory = root / "data";
    const boost::filesystem::path config_path = Slic3r::plugin_activation_config_path(data_directory);
    boost::filesystem::create_directories(config_path.parent_path());

    SECTION("a requested live package is recached") {
        const std::string package_name = "live.plugin";
        const boost::filesystem::path live_package = data_directory / "plugins" / package_name;
        boost::filesystem::create_directories(live_package);
        {
            boost::nowide::ofstream library((live_package / plugin_library_filename()).string(), std::ios::binary);
            library << "live";
        }
        {
            boost::nowide::ofstream config(config_path.string());
            config << "[installed]\n" << package_name << " = 1.2.0-beta.1\n"
                   << package_name << ".slicer_version = 2.7.0.0\n\n[removed]\n\n[activated]\n";
        }

        std::string error_message;
        REQUIRE(Slic3r::prepare_plugin_bundle_cache(resources_directory, data_directory, error_message));
        CHECK(boost::filesystem::is_regular_file(Slic3r::repository_package_cache_path(
            data_directory, Slic3r::RepositoryPackageType::Plugin, package_name,
            "1.2.0-beta.1", "2.7.0.0") / plugin_library_filename()));

        Slic3r::PluginActivationConfig config;
        REQUIRE(Slic3r::read_plugin_activation_config(config_path, config, error_message));
        CHECK(config.installed.count(package_name) == 1);
    }

    SECTION("a request available only in the purged cache is removed") {
        const std::string package_name = "lost.plugin";
        {
            boost::nowide::ofstream config(config_path.string());
            config << "[installed]\n" << package_name << " = 1.0.0.0\n"
                   << package_name << ".slicer_version = 2.7.0.0\n\n[removed]\n\n[activated]\n";
        }
        const boost::filesystem::path obsolete = data_directory / "cache" / "plugins" / "old-package";
        boost::filesystem::create_directories(obsolete);

        std::string error_message;
        REQUIRE(Slic3r::prepare_plugin_bundle_cache(resources_directory, data_directory, error_message));
        Slic3r::PluginActivationConfig config;
        REQUIRE(Slic3r::read_plugin_activation_config(config_path, config, error_message));
        CHECK(config.installed.count(package_name) == 0);
        CHECK_FALSE(boost::filesystem::exists(obsolete));
    }

    boost::filesystem::remove_all(root);
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

TEST_CASE("Plugin removal requests override conflicting installation entries", "[plugins][repository]")
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
               << "[removed]\nexample.plugin = 1\n\n"
               << "[activated]\nexample.id = 1\n";
    }

    Slic3r::PluginActivationConfig config;
    std::string error_message;
    REQUIRE(Slic3r::read_plugin_activation_config(config_path, config, error_message));
    CHECK(config.installed.count("example.plugin") == 0);
    CHECK(config.removed.count("example.plugin") == 1);
    CHECK(config.activated["example.id"]);
    boost::filesystem::remove_all(root);
}
