///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// These tests exercise the package filesystem protocol without loading a DLL.
// Generic descriptions and package versions are validated independently;
// loading the platform payload remains the responsibility of PluginLoader.

#include <catch2/catch.hpp>

#include <string>
#include <utility>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include "libslic3r/Plugins/PluginBinaryMetadata.hpp"
#include "libslic3r/Plugins/PluginRepository.hpp"
#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/host/Plugin.hpp"
#include "libslic3r/Updater/RepositoryPackageCache.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "plugin_test_helpers.hpp"

#ifdef _WIN32
#include <Windows.h>
#endif

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
                                 const std::string &name = std::string());
std::string version_contents(const std::string &package_version,
                             const std::string &slicer_version);
void write_description(const boost::filesystem::path &package_root,
                       const std::string &package_name,
                       const std::string &package_version,
                       const std::string &slicer_version);
const char *plugin_library_filename();
std::string read_text_file(const boost::filesystem::path &path);
#ifdef _WIN32
boost::filesystem::path current_test_executable();
#endif

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
                                 const std::string &name)
{
    return "[plugin]\n"
           "id = " + package_name + "\n"
           "name = " + (name.empty() ? package_name : name) + "\n"
           "full_name = " + package_name + "\n"
           "type = local\n";
}

std::string version_contents(const std::string &package_version,
                             const std::string &slicer_version)
{
    return "[plugin]\n"
           "package_version = " + package_version + "\n"
           "slicer_version = " + slicer_version + "\n";
}

void write_description(const boost::filesystem::path &package_root,
                       const std::string &package_name,
                       const std::string &package_version,
                       const std::string &slicer_version)
{
    boost::filesystem::create_directories(package_root);
    boost::nowide::ofstream description((package_root / "description.ini").string());
    description << description_contents(package_name);
    boost::nowide::ofstream version((package_root / "version.ini").string());
    version << version_contents(package_version, slicer_version);
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

#ifdef _WIN32
boost::filesystem::path current_test_executable()
{
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
        if (length == 0)
            return {};
        if (length < buffer.size() - 1)
            return boost::filesystem::path(std::wstring(buffer.data(), length));
        buffer.resize(buffer.size() * 2);
    }
}
#endif

} // namespace

TEST_CASE("Plugin bundles separate generic description from generated version metadata", "[plugins][repository]")
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
                                     {"description.ini", description_contents(package_name)}}));

    std::string error_message;
    CHECK(Slic3r::prepare_plugin_bundle_cache(resources_directory, data_directory, error_message));
    const boost::filesystem::path cached_package = Slic3r::repository_package_cache_path(
        data_directory, Slic3r::RepositoryPackageType::Plugin,
        package_name, package_version, slicer_version);
    CHECK(boost::filesystem::exists(cached_package / plugin_library_filename()));
    CHECK(boost::filesystem::exists(cached_package / "description.ini"));
    CHECK(read_text_file(cached_package / "description.ini").find("package_version") == std::string::npos);
    CHECK(read_text_file(cached_package / "description.ini").find("slicer_version") == std::string::npos);
    const std::string cached_version_contents = read_text_file(cached_package / "version.ini");
    CHECK(cached_version_contents.find("package_version = " + package_version) != std::string::npos);
    CHECK(cached_version_contents.find("slicer_version = " + slicer_version) != std::string::npos);
    const boost::filesystem::path root_description = Slic3r::repository_cache_root_path(
        data_directory, Slic3r::RepositoryPackageType::Plugin, package_name) / "description.ini";
    CHECK(boost::filesystem::is_regular_file(root_description));
    CHECK(read_text_file(root_description).find("package_version") == std::string::npos);
    CHECK(read_text_file(root_description).find("slicer_version") == std::string::npos);

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
                       {"description.ini", description_contents("another.plugin")}}));

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
    std::string error_message;
    REQUIRE(Slic3r::reconcile_installed_plugin_packages(data_directory, config, error_message));
    CHECK(boost::filesystem::exists(data_directory / "plugins" / package_name / "description.ini"));
    const boost::filesystem::path installed_package = data_directory / "plugins" / package_name;
    {
        boost::nowide::ofstream stream((installed_package / "local-marker.txt").string());
        stream << "preserved";
    }
    REQUIRE(Slic3r::reconcile_installed_plugin_packages(data_directory, config, error_message));
    CHECK(boost::filesystem::exists(installed_package / "local-marker.txt"));

    config.installed[package_name] = {second_version, slicer_version};
    REQUIRE(Slic3r::reconcile_installed_plugin_packages(data_directory, config, error_message));
    const std::string installed_version = read_text_file(
        data_directory / "plugins" / package_name / "version.ini");
    CHECK(installed_version.find("package_version = " + second_version) != std::string::npos);
    CHECK(installed_version.find("slicer_version = " + slicer_version) != std::string::npos);

    // A failed validation must stop the complete reconciliation, including
    // removal of unrelated live directories outside the desired set.
    const boost::filesystem::path unmanaged_package = data_directory / "plugins" / "unmanaged.plugin";
    boost::filesystem::create_directories(unmanaged_package);
    config.installed[package_name] = {"9.9.9.9", slicer_version};
    CHECK_FALSE(Slic3r::reconcile_installed_plugin_packages(data_directory, config, error_message));
    CHECK(boost::filesystem::exists(installed_package / "description.ini"));
    CHECK(boost::filesystem::is_directory(unmanaged_package));
    const std::string preserved_version = read_text_file(installed_package / "version.ini");
    CHECK(preserved_version.find("package_version = " + second_version) != std::string::npos);
    CHECK(preserved_version.find("slicer_version = " + slicer_version) != std::string::npos);

    boost::filesystem::remove_all(root);
}

TEST_CASE("Plugin reconciliation removes unmanaged live package directories", "[plugins][repository]")
{
    const boost::filesystem::path root = boost::filesystem::temp_directory_path() /
                                         boost::filesystem::unique_path("slic3r-plugin-reconcile-%%%%-%%%%");
    const boost::filesystem::path data_directory = root / "data";
    const boost::filesystem::path plugin_directory = data_directory / "plugins";
    const boost::filesystem::path unmanaged_package = plugin_directory / "manually.copied";
    boost::filesystem::create_directories(unmanaged_package);
    {
        boost::nowide::ofstream marker((unmanaged_package / "plugin.dll").string());
        marker << "unmanaged";
        boost::nowide::ofstream activation((plugin_directory / "activated.ini").string());
        activation << "[installed]\n";
    }

    Slic3r::PluginActivationConfig config;
    std::string error_message;
    REQUIRE(Slic3r::reconcile_installed_plugin_packages(data_directory, config, error_message));
    CHECK_FALSE(boost::filesystem::exists(unmanaged_package));
    CHECK(boost::filesystem::is_regular_file(plugin_directory / "activated.ini"));

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
    CHECK(plugin.name == "truc");
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
        CHECK(cached.version.package_version == "1.0.0.0");
        CHECK(cached.version.slicer_version == "1.0.0.0");
        CHECK(cached.description.config_update_rest.empty());
        CHECK(boost::filesystem::is_regular_file(cached.directory / "description.ini"));
        CHECK(boost::filesystem::is_regular_file(cached.directory / "version.ini"));
        CHECK(boost::filesystem::is_regular_file(cached.directory / plugin_library_filename()));
    }

    boost::filesystem::remove_all(root);
}

TEST_CASE("Vendor archive takes versions only from its profile",
          "[plugins][repository][cache-layout]")
{
    const boost::filesystem::path root = boost::filesystem::temp_directory_path() /
                                         boost::filesystem::unique_path("slic3r-vendor-version-source-%%%%-%%%%");
    const boost::filesystem::path archive = root / "vendor.zip";
    const std::string vendor_id = "example_vendor";
    const std::string profile =
        "[vendor]\n"
        "id = " + vendor_id + "\n"
        "name = Example Vendor\n"
        "full_name = Example Vendor\n"
        "config_version = 1.0.0.0\n"
        "slicer_version = 2.7.63.0-alpha\n";
    const std::string generic_description =
        "[vendor]\n"
        "id = " + vendor_id + "\n"
        "name = Example Vendor\n"
        "full_name = Example Vendor\n"
        // These obsolete fields must not override the profile versions.
        "config_version = 9.0.0.0\n"
        "slicer_version = 1.0.0.0\n";
    boost::filesystem::create_directories(root);
    REQUIRE(write_zip(archive, {{"description.ini", generic_description},
                                {"profiles/" + vendor_id + ".ini", profile}}));

    Slic3r::RepositoryPackageCache cache(root / "data", Slic3r::vendor_repository_cache_adapter());
    bool purged = false;
    std::string error_message;
    REQUIRE(cache.prepare_layout(purged, error_message));
    Slic3r::RepositoryCachedVersion cached;
    REQUIRE(cache.cache_archive(archive, std::nullopt, cached, error_message));
    CHECK(cached.version.package_version == "1.0.0.0");
    CHECK(cached.version.slicer_version == "2.7.63.0-alpha");
    CHECK(cached.directory.filename() == "1.0.0.0=2.7.63.0-alpha");
    CHECK(read_text_file(cached.directory / "description.ini").find("config_version") == std::string::npos);
    CHECK(cache.repository_description_path(vendor_id) ==
          cache.repository_directory(vendor_id) / "description.ini");

    boost::filesystem::remove_all(root);
}

TEST_CASE("Python plugin metadata creates version.ini and conflicts are rejected",
          "[plugins][repository][cache-layout]")
{
    const boost::filesystem::path root = boost::filesystem::temp_directory_path() /
                                         boost::filesystem::unique_path("slic3r-python-package-%%%%-%%%%");
    const boost::filesystem::path package = root / "python.example";
    boost::filesystem::create_directories(package);
    {
        boost::nowide::ofstream description((package / "description.ini").string());
        description << description_contents("python.example")
                    << "package_version = 99.0.0.0\n"
                    << "slicer_version = 99.0.0.0\n";
        boost::nowide::ofstream script((package / "plugin.py").string());
        script << "__version__ = '1.4.0'\n"
                  "__slicer_version__ = \"2.7.63.0\"\n"
                  "def register_plugin(api):\n    return None\n";
    }

    Slic3r::RepositoryPackageCache cache(root / "data", Slic3r::plugin_repository_cache_adapter());
    bool purged = false;
    std::string error_message;
    REQUIRE(cache.prepare_layout(purged, error_message));
    Slic3r::RepositoryCachedVersion cached;
    REQUIRE(cache.cache_simple(package, cached, error_message));
    CHECK(cached.version.package_version == "1.4.0");
    CHECK(cached.version.slicer_version == "2.7.63.0");
    CHECK_FALSE(boost::filesystem::exists(package / "version.ini"));
    CHECK(boost::filesystem::is_regular_file(cached.directory / "version.ini"));
    CHECK(boost::filesystem::is_regular_file(cached.directory / "plugin.py"));
    CHECK(read_text_file(cached.directory / "description.ini").find("99.0.0.0") == std::string::npos);

    // An explicit version file has higher provenance priority, but it may not
    // disagree with metadata embedded in the same package.
    {
        boost::nowide::ofstream version((package / "version.ini").string());
        version << version_contents("1.5.0", "2.7.63.0");
    }
    CHECK_FALSE(cache.cache_simple(package, cached, error_message));
    CHECK(error_message.find("conflict") != std::string::npos);

    {
        boost::nowide::ofstream version((package / "version.ini").string(), std::ios::out | std::ios::trunc);
        version << version_contents("1.4.0", "2.7.63.0");
    }
    Slic3r::RepositoryPackageExpectation expected;
    expected.type = Slic3r::RepositoryPackageType::Plugin;
    expected.id = "python.example";
    expected.version.package_version = "2.0.0";
    expected.version.slicer_version = "2.7.63.0";
    CHECK_FALSE(cache.cache_package_directory(package, expected, cached, error_message));
    CHECK(error_message.find("repository tag") != std::string::npos);

    boost::filesystem::remove_all(root);
}

TEST_CASE("Python plugin entry selection is deterministic",
          "[plugins][repository][cache-layout]")
{
    const boost::filesystem::path root = boost::filesystem::temp_directory_path() /
                                         boost::filesystem::unique_path("slic3r-python-entry-%%%%-%%%%");
    const boost::filesystem::path package = root / "entry.example";
    boost::filesystem::create_directories(package);
    {
        boost::nowide::ofstream description((package / "description.ini").string());
        description << description_contents("entry.example");
        boost::nowide::ofstream named((package / "entry.example.py").string());
        named << "__version__ = '2.0.0.0'\n__slicer_version__ = '2.7.63.0'\n";
        boost::nowide::ofstream conventional((package / "plugin.py").string());
        conventional << "__version__ = '9.0.0.0'\n__slicer_version__ = '9.0.0.0'\n";
    }

    Slic3r::RepositoryPackageCache cache(root / "data", Slic3r::plugin_repository_cache_adapter());
    bool purged = false;
    std::string error_message;
    REQUIRE(cache.prepare_layout(purged, error_message));
    Slic3r::RepositoryCachedVersion cached;
    REQUIRE(cache.cache_simple(package, cached, error_message));
    CHECK(cached.version.package_version == "2.0.0.0");
    CHECK(cached.version.slicer_version == "2.7.63.0");

    boost::filesystem::remove(package / "entry.example.py");
    boost::filesystem::remove(package / "plugin.py");
    {
        boost::nowide::ofstream first((package / "first.py").string());
        first << "__version__ = '1.0.0.0'\n";
        boost::nowide::ofstream second((package / "second.py").string());
        second << "__version__ = '1.0.0.0'\n";
    }
    CHECK_FALSE(cache.cache_simple(package, cached, error_message));
    CHECK(error_message.find("unambiguous Python entry point") != std::string::npos);

    boost::filesystem::remove_all(root);
}

TEST_CASE("Pure Python plugin packages install and remove through the normal lifecycle",
          "[plugins][repository][python]")
{
    const boost::filesystem::path root = boost::filesystem::temp_directory_path() /
                                         boost::filesystem::unique_path("slic3r-python-lifecycle-%%%%-%%%%");
    const boost::filesystem::path data_directory = root / "data";
    const boost::filesystem::path package = root / "lifecycle.python";
    boost::filesystem::create_directories(package);
    {
        boost::nowide::ofstream description((package / "description.ini").string());
        description << description_contents("lifecycle.python");
        boost::nowide::ofstream script((package / "plugin.py").string());
        script << "__version__ = '1.0.0.0'\n__slicer_version__ = '2.7.63.0'\n";
    }

    Slic3r::RepositoryPackageCache cache(data_directory, Slic3r::plugin_repository_cache_adapter());
    bool purged = false;
    std::string error_message;
    REQUIRE(cache.prepare_layout(purged, error_message));
    Slic3r::RepositoryCachedVersion cached;
    REQUIRE(cache.cache_simple(package, cached, error_message));

    Slic3r::PluginActivationConfig config;
    config.installed["lifecycle.python"] = {"1.0.0.0", "2.7.63.0"};
    REQUIRE(Slic3r::reconcile_installed_plugin_packages(
        data_directory, config, error_message));
    const boost::filesystem::path installed = data_directory / "plugins" / "lifecycle.python";
    CHECK(boost::filesystem::is_regular_file(installed / "plugin.py"));
    CHECK(boost::filesystem::is_regular_file(installed / "description.ini"));
    CHECK(boost::filesystem::is_regular_file(installed / "version.ini"));

    config.installed.clear();
    REQUIRE(Slic3r::reconcile_installed_plugin_packages(
        data_directory, config, error_message));
    CHECK_FALSE(boost::filesystem::exists(installed));
    CHECK(boost::filesystem::is_directory(cached.directory));

    boost::filesystem::remove_all(root);
}

#ifdef SLIC3R_TEST_PYTHON_PLUGINS
TEST_CASE("Pure Python package is loaded with its own package root",
          "[plugins][repository][python]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    REQUIRE(Slic3r::Test::Plugins::python_plugin_test_runtime_available());
    CHECK(Slic3r::Orchestrator::instance().get_plugin("python") == nullptr);
    const Slic3r::Plugin *plugin =
        Slic3r::Orchestrator::instance().get_plugin("python.external.package_root");
    REQUIRE(plugin != nullptr);
    const boost::filesystem::path package_root(plugin->get_package_root());
    CHECK(package_root.filename() == "python.external.package_root");
    CHECK(boost::filesystem::is_regular_file(package_root / "description.ini"));
    CHECK(boost::filesystem::is_regular_file(package_root / "version.ini"));
}
#endif

TEST_CASE("Portable binary metadata supplies missing plugin package versions",
          "[plugins][repository][cache-layout]")
{
    const boost::filesystem::path root = boost::filesystem::temp_directory_path() /
                                         boost::filesystem::unique_path("slic3r-portable-version-%%%%-%%%%");
    const boost::filesystem::path package = root / "portable.example";
    boost::filesystem::create_directories(package);
    {
        boost::nowide::ofstream description((package / "description.ini").string());
        description << description_contents("portable.example");
    }
    {
        const Slic3r::PluginBinaryMetadata metadata = {
            Slic3r::PLUGIN_BINARY_METADATA_MAGIC,
            Slic3r::PLUGIN_BINARY_METADATA_FORMAT_VERSION,
            "4.5.6",
            "2.7.63.0"
        };
        boost::nowide::ofstream library(
            (package / plugin_library_filename()).string(), std::ios::out | std::ios::binary);
        const std::string prefix(65500, 'x');
        library.write(prefix.data(), std::streamsize(prefix.size()));
        library.write(reinterpret_cast<const char *>(&metadata), sizeof(metadata));
        library << "binary suffix";
    }

    Slic3r::RepositoryPackageCache cache(root / "data", Slic3r::plugin_repository_cache_adapter());
    bool purged = false;
    std::string error_message;
    REQUIRE(cache.prepare_layout(purged, error_message));
    Slic3r::RepositoryCachedVersion cached;
    REQUIRE(cache.cache_simple(package, cached, error_message));
    CHECK(cached.version.package_version == "4.5.6");
    CHECK(cached.version.slicer_version == "2.7.63.0");

    // The portable record remains a real provenance source: a contradictory
    // sidecar must be rejected instead of silently replacing its values.
    {
        boost::nowide::ofstream version((package / "version.ini").string());
        version << version_contents("4.5.7", "2.7.63.0");
    }
    CHECK_FALSE(cache.cache_simple(package, cached, error_message));
    CHECK(error_message.find("native metadata") != std::string::npos);

    boost::filesystem::remove_all(root);
}

#ifdef _WIN32
TEST_CASE("Windows plugin VERSIONINFO supplies missing package versions",
          "[plugins][repository][cache-layout]")
{
    const boost::filesystem::path root = boost::filesystem::temp_directory_path() /
                                         boost::filesystem::unique_path("slic3r-native-version-%%%%-%%%%");
    const boost::filesystem::path package = root / "native.example";
    boost::filesystem::create_directories(package);
    {
        boost::nowide::ofstream description((package / "description.ini").string());
        description << description_contents("native.example");
    }
    boost::filesystem::copy_file(current_test_executable(), package / plugin_library_filename());

    Slic3r::RepositoryPackageCache cache(root / "data", Slic3r::plugin_repository_cache_adapter());
    bool purged = false;
    std::string error_message;
    REQUIRE(cache.prepare_layout(purged, error_message));
    Slic3r::RepositoryCachedVersion cached;
    REQUIRE(cache.cache_simple(package, cached, error_message));
    CHECK(cached.version.package_version == "9.8.7.6");
    CHECK(cached.version.slicer_version == "2.7.63.0");

    {
        boost::nowide::ofstream version((package / "version.ini").string());
        version << version_contents("1.0.0.0", "2.7.63.0");
    }
    CHECK_FALSE(cache.cache_simple(package, cached, error_message));
    CHECK(error_message.find("native metadata") != std::string::npos);

    boost::filesystem::remove_all(root);
}
#endif

#ifdef SLIC3R_TEST_POLYHOLES_PLUGIN_DLL
TEST_CASE("Packaged C++ plugin DLL metadata matches its generated version file",
          "[plugins][repository][cache-layout]")
{
    // Polyholes owns its release number, while compatibility keeps the full
    // slicer SemVer, including prerelease and build metadata.
    CHECK(std::string(SLIC3R_TEST_POLYHOLES_PACKAGE_VERSION) == "1.0.0");
    CHECK(std::string(SLIC3R_TEST_POLYHOLES_SLICER_VERSION) == SLIC3R_TEST_BUILD_SLICER_VERSION);

    const boost::filesystem::path root = boost::filesystem::temp_directory_path() /
                                         boost::filesystem::unique_path("slic3r-packaged-native-version-%%%%-%%%%");
    const boost::filesystem::path package = root / "polyholes";
    boost::filesystem::create_directories(package);
    {
        boost::nowide::ofstream description((package / "description.ini").string());
        description << description_contents("polyholes", "Polyholes");
    }
    boost::filesystem::copy_file(
        boost::filesystem::path(SLIC3R_TEST_POLYHOLES_PLUGIN_DLL),
        package / plugin_library_filename());

    Slic3r::RepositoryPackageCache cache(root / "data", Slic3r::plugin_repository_cache_adapter());
    bool purged = false;
    std::string error_message;
    REQUIRE(cache.prepare_layout(purged, error_message));
    Slic3r::RepositoryCachedVersion cached;

    // With no sidecar file, the cache must recover both values from the DLL.
    REQUIRE(cache.cache_simple(package, cached, error_message));
    CHECK(cached.version.package_version == SLIC3R_TEST_POLYHOLES_PACKAGE_VERSION);
    CHECK(cached.version.slicer_version == SLIC3R_TEST_POLYHOLES_SLICER_VERSION);

    // The normal package contains the same values in version.ini. Importing it
    // again verifies that generated native and sidecar metadata agree.
    {
        boost::nowide::ofstream version((package / "version.ini").string());
        version << version_contents(SLIC3R_TEST_POLYHOLES_PACKAGE_VERSION,
                                    SLIC3R_TEST_POLYHOLES_SLICER_VERSION);
    }
    REQUIRE(cache.cache_simple(package, cached, error_message));

    // A plugin must never silently choose between two different package
    // versions because the selected cache directory would become ambiguous.
    {
        boost::nowide::ofstream version((package / "version.ini").string());
        version << version_contents("999.999.999.999", SLIC3R_TEST_POLYHOLES_SLICER_VERSION);
    }
    CHECK_FALSE(cache.cache_simple(package, cached, error_message));
    CHECK(error_message.find("native metadata") != std::string::npos);

    boost::filesystem::remove_all(root);
}
#endif

#if defined(SLIC3R_TEST_FLAT_AREA_PACKAGE_VERSION) && defined(SLIC3R_TEST_FLAT_AREA_SLICER_VERSION)
TEST_CASE("Packaged C++ plugin defaults to the numeric slicer package version",
          "[plugins][repository][cache-layout]")
{
    CHECK(std::string(SLIC3R_TEST_FLAT_AREA_PACKAGE_VERSION) == SLIC3R_TEST_BUILD_RC_VERSION);
    CHECK(std::string(SLIC3R_TEST_FLAT_AREA_SLICER_VERSION) == SLIC3R_TEST_BUILD_SLICER_VERSION);
}
#endif

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
                       "full_name = New full name\n";
        boost::nowide::ofstream version((second / "version.ini").string());
        version << version_contents("2.0.0-beta.1", "2.7.0.0");
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

TEST_CASE("Plugin cache purge restores desired live packages and preserves lost selections",
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
                   << package_name << ".slicer_version = 2.7.0.0\n\n[activated]\n";
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

    SECTION("a desired package unavailable after purge remains selected") {
        const std::string package_name = "lost.plugin";
        {
            boost::nowide::ofstream config(config_path.string());
            config << "[installed]\n" << package_name << " = 1.0.0.0\n"
                   << package_name << ".slicer_version = 2.7.0.0\n\n[activated]\n";
        }
        const boost::filesystem::path obsolete = data_directory / "cache" / "plugins" / "old-package";
        boost::filesystem::create_directories(obsolete);

        std::string error_message;
        REQUIRE(Slic3r::prepare_plugin_bundle_cache(resources_directory, data_directory, error_message));
        Slic3r::PluginActivationConfig config;
        REQUIRE(Slic3r::read_plugin_activation_config(config_path, config, error_message));
        CHECK(config.installed.count(package_name) == 1);
        CHECK_FALSE(boost::filesystem::exists(obsolete));
        CHECK_FALSE(Slic3r::reconcile_installed_plugin_packages(data_directory, config, error_message));
        CHECK(error_message.find("not cached") != std::string::npos);
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

    std::string error_message;
    REQUIRE(Slic3r::write_plugin_activation_config(config_path, written, error_message));
    Slic3r::PluginActivationConfig read;
    REQUIRE(Slic3r::read_plugin_activation_config(config_path, read, error_message));
    CHECK(read.activated == written.activated);
    CHECK(read.plugin_packages == written.plugin_packages);

    const std::string contents = read_text_file(config_path);
    CHECK(contents.find("[plugin_packages]") != std::string::npos);
    CHECK(contents.find("example.first = example.package") != std::string::npos);
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
        stream << "[activated]\nlegacy.plugin = 1\n";
    }

    Slic3r::PluginActivationConfig config;
    std::string error_message;
    REQUIRE(Slic3r::read_plugin_activation_config(config_path, config, error_message));
    CHECK(config.activated["legacy.plugin"]);
    CHECK(config.plugin_packages.empty());
    boost::filesystem::remove_all(root);
}

TEST_CASE("Plugin activation configuration writes no removal section", "[plugins][repository]")
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
