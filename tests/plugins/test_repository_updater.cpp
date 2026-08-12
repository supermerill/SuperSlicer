///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// These tests keep the updater data model independent from wxWidgets. They
// verify the common success/error result, injectable HTTP behavior and the
// complete vendor workflow exposed to the preset-selection dialog. Functional
// scenarios use isolated resource/data directories and real package archives,
// so no test can modify the developer's installed presets.

#include <catch2/catch.hpp>

#include <algorithm>
#include <ctime>
#include <deque>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include "libslic3r/ContainerUtils.hpp"
#include "libslic3r/Plugins/PluginRepository.hpp"
#include "libslic3r/Updater/PluginUpdater.hpp"
#include "libslic3r/Updater/PresetUpdater.hpp"
#include "libslic3r/Updater/RepositoryPackageCache.hpp"
#include "libslic3r/Updater/UpdaterHttp.hpp"
#include "libslic3r/Updater/UpdaterError.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/miniz_extension.hpp"

namespace {

// FakeUpdaterHttpTransport retains asynchronous requests instead of starting a
// network thread. A test first inspects the pending request, then deliberately
// completes or fails it to exercise the updater callback at a deterministic
// point in the test.
class FakeUpdaterHttpTransport final : public Slic3r::UpdaterHttpTransport
{
public:
    size_t pending_count() const { return m_pending.size(); }
    const Slic3r::UpdaterHttpRequest &pending_front() const { return m_pending.front(); }
    const Slic3r::UpdaterHttpRequest &pending_at(size_t idx) const { return m_pending.at(idx); }
    size_t sync_request_count() const { return m_sync_request_urls.size(); }
    const std::string &sync_request_url(size_t idx) const { return m_sync_request_urls.at(idx); }

    // The common updater catches failures raised while an asynchronous request
    // is being started. This hook exercises that path without a network thread.
    void throw_on_next_async_request(std::string message)
    {
        m_async_exception = std::move(message);
    }

    void succeed_front(std::string body, unsigned http_status)
    {
        Slic3r::UpdaterHttpRequest request = std::move(m_pending.front());
        m_pending.pop_front();
        complete_request(request, std::move(body), http_status);
    }

    void fail_front(std::string body, std::string error, unsigned http_status)
    {
        Slic3r::UpdaterHttpRequest request = std::move(m_pending.front());
        m_pending.pop_front();
        fail_request(request, std::move(body), std::move(error), http_status);
    }

    bool progress_front(size_t total, size_t current, const std::string &buffer)
    {
        bool cancel = false;
        progress_request(m_pending.front(), total, current, 0, 0, buffer, cancel);
        return cancel;
    }

    void script_sync_success(std::string body, unsigned http_status = 200)
    {
        m_sync_response = SyncResponse{true, std::move(body), std::string(), http_status};
    }

    void script_sync_failure(std::string error, unsigned http_status = 0)
    {
        m_sync_response = SyncResponse{false, std::string(), std::move(error), http_status};
    }

private:
    struct SyncResponse {
        bool succeeded;
        std::string body;
        std::string error;
        unsigned http_status;
    };

    // Async requests remain pending until the test selects their outcome.
    void perform_request(Slic3r::UpdaterHttpRequest request) override
    {
        if (m_async_exception) {
            const std::string message = std::move(*m_async_exception);
            m_async_exception.reset();
            throw std::runtime_error(message);
        }
        m_pending.emplace_back(std::move(request));
    }

    // A synchronous request must consume a response before returning, matching
    // the production transport contract without starting a worker thread.
    void perform_request_sync(Slic3r::UpdaterHttpRequest request) override
    {
        m_sync_request_urls.emplace_back(request.url());
        if (!m_sync_response)
            throw std::logic_error("The fake updater transport has no scripted synchronous response.");

        SyncResponse response = std::move(*m_sync_response);
        m_sync_response.reset();
        if (response.succeeded)
            complete_request(request, std::move(response.body), response.http_status);
        else
            fail_request(request, std::move(response.body), std::move(response.error), response.http_status);
    }

    std::deque<Slic3r::UpdaterHttpRequest> m_pending;
    std::optional<SyncResponse> m_sync_response;
    std::optional<std::string> m_async_exception;
    std::vector<std::string> m_sync_request_urls;
};

struct VendorChangeCall {
    Slic3r::VendorChange change;
    std::vector<std::string> vendor_ids;
};

// The GUI host normally creates a configuration snapshot before a change and
// reloads presets afterwards. Recording both calls verifies that the core
// updater surrounds filesystem changes with the same application contract.
class FakePresetUpdaterHost final : public Slic3r::PresetUpdaterHost
{
public:
    bool prepare_vendor_change(Slic3r::VendorChange change,
                               const std::vector<std::string> &vendor_ids) override
    {
        prepared_changes.push_back({change, vendor_ids});
        return accept_changes;
    }

    void vendor_files_changed(Slic3r::PresetUpdater &,
                              Slic3r::VendorChange change,
                              const std::vector<std::string> &vendor_ids) override
    {
        completed_changes.push_back({change, vendor_ids});
    }

    bool accept_changes = true;
    std::vector<VendorChangeCall> prepared_changes;
    std::vector<VendorChangeCall> completed_changes;
};

// Exposes the common protocol to focused unit tests. Real updaters use the same
// protected methods but keep their repository maps and parsing domain-specific.
class TestRepositoryUpdater final : public Slic3r::RepositoryUpdater
{
public:
    explicit TestRepositoryUpdater(Slic3r::UpdaterHttpTransport &http_transport)
        : RepositoryUpdater(http_transport)
    {
    }

    using RepositoryUpdater::begin_sync;
    using RepositoryUpdater::changelog_download_in_progress;
    using RepositoryUpdater::download_repository_changelogs;
    using RepositoryUpdater::download_repository_description;
    using RepositoryUpdater::download_repository_file_async;
    using RepositoryUpdater::download_repository_file_sync;
    using RepositoryUpdater::has_api_request_slot;
    using RepositoryUpdater::refresh_repository_tags;
    using RepositoryUpdater::RepositoryChangelogRequest;

private:
    int update_count() override { return 7; }
};

// Each test receives an isolated directory outside the source tree. Removing it
// in the destructor also cleans files created before a failed assertion.
class TemporaryDirectory
{
public:
    TemporaryDirectory()
        : m_path(boost::filesystem::temp_directory_path() /
                 boost::filesystem::unique_path("slic3r-updater-%%%%-%%%%-%%%%"))
    {
        boost::filesystem::create_directories(m_path);
    }

    ~TemporaryDirectory()
    {
        boost::system::error_code ignored_error;
        boost::filesystem::remove_all(m_path, ignored_error);
    }

    const boost::filesystem::path &path() const { return m_path; }

private:
    boost::filesystem::path m_path;
};

// PluginUpdater discovers descriptions through the process data/resource
// directories. This guard lets one test provide a complete isolated repository
// and restores the application globals even if an assertion aborts the test.
class ScopedUpdaterDirectories
{
public:
    ScopedUpdaterDirectories(const boost::filesystem::path &resources_directory,
                             const boost::filesystem::path &data_directory)
        : m_previous_resources_directory(Slic3r::resources_dir())
        , m_previous_data_directory(Slic3r::has_data_dir() ? Slic3r::data_dir() : std::string())
    {
        Slic3r::set_resources_dir(resources_directory.string());
        Slic3r::set_data_dir(data_directory.string());
    }

    ~ScopedUpdaterDirectories()
    {
        Slic3r::set_resources_dir(m_previous_resources_directory);
        Slic3r::set_data_dir(m_previous_data_directory);
    }

private:
    std::string m_previous_resources_directory;
    std::string m_previous_data_directory;
};

std::string read_test_file(const boost::filesystem::path &path)
{
    boost::nowide::ifstream stream(path.string(), std::ios::in | std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

void write_test_file(const boost::filesystem::path &path, const std::string &contents)
{
    boost::filesystem::create_directories(path.parent_path());
    boost::nowide::ofstream stream(path.string(), std::ios::out | std::ios::trunc);
    stream << contents;
}

struct ZipEntry {
    std::string name;
    std::string contents;
};

struct TestVendorVersion {
    std::string config_version;
    std::string slicer_version;
    std::string archive_url;
};

struct TestPluginVersion {
    std::string package_version;
    std::string slicer_version;
    std::string archive_url;
};

struct PresetDialogSnapshot {
    std::vector<Slic3r::VendorSync> vendors;
    int profile_count_to_update = 0;
};

bool write_test_zip(const boost::filesystem::path &archive_path, const std::vector<ZipEntry> &entries);
std::string vendor_profile_contents(const std::string &vendor_id,
                                    const std::string &config_version,
                                    const std::string &slicer_version);
std::string vendor_repository_tags(const std::vector<TestVendorVersion> &versions);
const char *plugin_library_filename();
std::string plugin_description_contents(const std::string &plugin_id,
                                        const std::string &package_version,
                                        const std::string &slicer_version,
                                        bool include_repository,
                                        const std::string &repository = "example/plugin");
std::string plugin_repository_tags(const std::vector<TestPluginVersion> &versions);

// This fixture reproduces the core portion of the GUI pipeline: discover local
// vendors, synchronize the repository, copy the dialog model, then apply the
// operation selected from that copied model.
class PresetUpdaterFunctionalFixture
{
protected:
    PresetUpdaterFunctionalFixture();

    void write_resource_vendor(const std::string &config_version);
    void write_installed_vendor(const std::string &config_version);
    PresetDialogSnapshot synchronize_and_open_dialog(const std::vector<TestVendorVersion> &versions);

    TemporaryDirectory temporary;
    boost::filesystem::path resources_directory;
    boost::filesystem::path data_directory;
    ScopedUpdaterDirectories directories;
    FakePresetUpdaterHost host;
    FakeUpdaterHttpTransport http;
    Slic3r::PresetUpdater updater;
    const std::string vendor_id = "functional_vendor";
    const std::string slicer_version = "2.7.0.0";
};

// This fixture follows the plugin dialog and startup protocol using real
// activation files and package directories. It never loads the fake library;
// startup is represented by the package-application function called before
// PluginLoader opens any DLL.
class PluginUpdaterFunctionalFixture
{
protected:
    PluginUpdaterFunctionalFixture();

    void write_plugin_repository();
    void write_installed_plugin(const std::string &package_version);
    boost::filesystem::path write_cached_plugin(const std::string &package_version);
    std::string make_plugin_archive(const std::string &package_version);
    void synchronize(const std::vector<TestPluginVersion> &versions);
    Slic3r::PluginActivationConfig read_activation_config();

    TemporaryDirectory temporary;
    boost::filesystem::path resources_directory;
    boost::filesystem::path data_directory;
    ScopedUpdaterDirectories directories;
    FakeUpdaterHttpTransport http;
    Slic3r::PluginUpdater updater;
    const std::string plugin_id = "functional.plugin";
    const std::string slicer_version = "2.7.0.0";
};

bool write_test_zip(const boost::filesystem::path &archive_path, const std::vector<ZipEntry> &entries)
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
    // miniz writes the central directory only during finalization. Closing the
    // writer without this call produces bytes but not a readable ZIP package.
    const bool finalized = success && mz_zip_writer_finalize_archive(&archive);
    return Slic3r::close_zip_writer(&archive) && finalized;
}

std::string vendor_profile_contents(const std::string &vendor_id,
                                    const std::string &config_version,
                                    const std::string &slicer_version)
{
    return "[vendor]\n"
           "id = " + vendor_id + "\n"
           "name = Functional vendor\n"
           "full_name = Functional vendor\n"
           "config_version = " + config_version + "\n"
           "slicer_version = " + slicer_version + "\n"
           "config_update_rest = example/vendor\n";
}

std::string vendor_repository_tags(const std::vector<TestVendorVersion> &versions)
{
    std::ostringstream json;
    json << '[';
    for (size_t idx = 0; idx < versions.size(); ++idx) {
        if (idx != 0)
            json << ',';
        const TestVendorVersion &version = versions[idx];
        const std::string tag = version.config_version + '=' + version.slicer_version;
        json << "{\"name\":\"" << tag
             << "\",\"zipball_url\":\"" << version.archive_url
             << "\",\"commit\":{\"sha\":\"sha-" << version.config_version
             << "\",\"url\":\"https://api.github.com/repos/example/vendor/commits/"
             << version.config_version << "\"}}";
    }
    json << ']';
    return json.str();
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

std::string plugin_description_contents(const std::string &plugin_id,
                                        const std::string &package_version,
                                        const std::string &slicer_version,
                                        bool include_repository,
                                        const std::string &repository)
{
    (void) package_version;
    (void) slicer_version;
    std::string contents =
        "[plugin]\n"
        "id = " + plugin_id + "\n"
        "name = Functional plugin\n"
        "full_name = Functional plugin\n";
    if (include_repository)
        contents += "config_update_rest = " + repository + "\n";
    return contents;
}

void save_test_plugin_repository(const boost::filesystem::path &data_directory,
                                 const std::string &plugin_id,
                                 const std::string &config_update_rest)
{
    Slic3r::RepositoryPackageCache cache(data_directory, Slic3r::plugin_repository_cache_adapter());
    bool purged = false;
    std::string error_message;
    REQUIRE(cache.prepare_layout(purged, error_message));

    Slic3r::RepositoryDescription description;
    description.type = Slic3r::RepositoryPackageType::Plugin;
    description.id = plugin_id;
    description.name = plugin_id;
    description.full_name = "Functional plugin";
    description.config_update_rest = config_update_rest;
    description.slicer = "SuperSlicer";
    REQUIRE(cache.save_repository_description(description, error_message));
}

std::string plugin_repository_tags(const std::vector<TestPluginVersion> &versions)
{
    std::ostringstream json;
    json << '[';
    for (size_t idx = 0; idx < versions.size(); ++idx) {
        if (idx != 0)
            json << ',';
        const TestPluginVersion &version = versions[idx];
        const std::string tag = version.package_version + '=' + version.slicer_version;
        json << "{\"name\":\"" << tag
             << "\",\"zipball_url\":\"" << version.archive_url
             << "\",\"commit\":{\"sha\":\"sha-" << version.package_version
             << "\",\"url\":\"https://api.github.com/repos/example/plugin/commits/"
             << version.package_version << "\"}}";
    }
    json << ']';
    return json.str();
}

PresetUpdaterFunctionalFixture::PresetUpdaterFunctionalFixture()
    : resources_directory(temporary.path() / "resources")
    , data_directory(temporary.path() / "data")
    , directories(resources_directory, data_directory)
    , updater(&host, http)
{
}

void PresetUpdaterFunctionalFixture::write_resource_vendor(const std::string &config_version)
{
    write_test_file(resources_directory / "profiles" / (vendor_id + ".ini"),
                    vendor_profile_contents(vendor_id, config_version, slicer_version));
}

void PresetUpdaterFunctionalFixture::write_installed_vendor(const std::string &config_version)
{
    write_test_file(data_directory / "vendor" / (vendor_id + ".ini"),
                    vendor_profile_contents(vendor_id, config_version, slicer_version));
}

PresetDialogSnapshot PresetUpdaterFunctionalFixture::synchronize_and_open_dialog(
    const std::vector<TestVendorVersion> &versions)
{
    std::optional<int> update_count;
    updater.sync_async([&update_count](int count) { update_count = count; }, true);

    // The dialog is scheduled only after the repository request completes.
    // Completing the retained request here reproduces that asynchronous edge.
    REQUIRE(http.pending_count() == 1);
    CHECK(http.pending_front().url() ==
          "https://api.github.com/repos/example/vendor/tags?per_page=100;page=1");
    http.succeed_front(vendor_repository_tags(versions), 200);

    REQUIRE(update_count.has_value());
    CHECK(updater.is_synchronized());
    const Slic3r::VendorSync *synchronized_vendor = updater.get_vendor(vendor_id);
    REQUIRE(synchronized_vendor != nullptr);
    CHECK(synchronized_vendor->sync_state == Slic3r::RepositorySyncState::Succeeded);
    CHECK(synchronized_vendor->sync_error.succeeded());
    return {updater.vendors(), *update_count};
}

PluginUpdaterFunctionalFixture::PluginUpdaterFunctionalFixture()
    : resources_directory(temporary.path() / "resources")
    , data_directory(temporary.path() / "data")
    , directories(resources_directory, data_directory)
    , updater(http)
{
    write_test_file(resources_directory / "plugins" / "default_activated.ini",
                    "[installed]\n\n[removed]\n\n[activated]\n");
    Slic3r::RepositoryPackageCache cache(data_directory, Slic3r::plugin_repository_cache_adapter());
    bool purged = false;
    std::string error_message;
    REQUIRE(cache.prepare_layout(purged, error_message));
}

void PluginUpdaterFunctionalFixture::write_plugin_repository()
{
    save_test_plugin_repository(data_directory, plugin_id, "example/plugin");
}

void PluginUpdaterFunctionalFixture::write_installed_plugin(const std::string &package_version)
{
    const boost::filesystem::path package_root = data_directory / "plugins" / plugin_id;
    write_test_file(package_root / "description.ini",
                    plugin_description_contents(plugin_id, package_version, slicer_version, false));
    write_test_file(package_root / "version.ini",
                    "[plugin]\npackage_version = " + package_version +
                    "\nslicer_version = " + slicer_version + "\n");
    write_test_file(package_root / plugin_library_filename(), "installed library " + package_version);

    Slic3r::PluginActivationConfig config;
    config.installed[plugin_id] = {package_version, slicer_version};
    std::string error_message;
    REQUIRE(Slic3r::write_plugin_activation_config(
        Slic3r::plugin_activation_config_path(data_directory), config, error_message));
}

boost::filesystem::path PluginUpdaterFunctionalFixture::write_cached_plugin(const std::string &package_version)
{
    const boost::filesystem::path package_root = Slic3r::repository_package_cache_path(
        data_directory, Slic3r::RepositoryPackageType::Plugin, plugin_id, package_version, slicer_version);
    write_test_file(package_root / "description.ini",
                    plugin_description_contents(plugin_id, package_version, slicer_version, false));
    write_test_file(package_root / "version.ini",
                    "[plugin]\npackage_version = " + package_version +
                    "\nslicer_version = " + slicer_version + "\n");
    write_test_file(package_root / plugin_library_filename(), "cached library " + package_version);
    return package_root;
}

std::string PluginUpdaterFunctionalFixture::make_plugin_archive(const std::string &package_version)
{
    const boost::filesystem::path archive_path = temporary.path() / (package_version + ".zip");
    REQUIRE(write_test_zip(
        archive_path,
        {{"description.ini", plugin_description_contents(plugin_id, package_version, slicer_version, false)},
         {plugin_library_filename(), "downloaded library " + package_version}}));
    return read_test_file(archive_path);
}

void PluginUpdaterFunctionalFixture::synchronize(const std::vector<TestPluginVersion> &versions)
{
    std::optional<int> update_count;
    updater.sync_async([&update_count](int count) { update_count = count; }, true);
    REQUIRE(http.pending_count() == 1);
    CHECK(http.pending_front().url() ==
          "https://api.github.com/repos/example/plugin/tags?per_page=100;page=1");
    http.succeed_front(plugin_repository_tags(versions), 200);
    REQUIRE(update_count.has_value());
}

Slic3r::PluginActivationConfig PluginUpdaterFunctionalFixture::read_activation_config()
{
    Slic3r::PluginActivationConfig config;
    std::string error_message;
    REQUIRE(Slic3r::read_plugin_activation_config(
        Slic3r::plugin_activation_config_path(data_directory), config, error_message));
    return config;
}

} // namespace

TEST_CASE("UpdaterError explicitly represents success and failure", "[plugins][updater]")
{
    const Slic3r::UpdaterError success;
    CHECK(success.succeeded());

    Slic3r::UpdaterError failure;
    failure.code = Slic3r::UpdaterError::Code::Network;
    failure.detail = "connection refused";
    CHECK_FALSE(failure.succeeded());
    CHECK(failure.detail == "connection refused");

    const Slic3r::UpdaterError constructed =
        Slic3r::make_updater_error(Slic3r::UpdaterError::Code::Filesystem, "read only");
    CHECK(constructed.code == Slic3r::UpdaterError::Code::Filesystem);
    CHECK(constructed.detail == "read only");
}

TEST_CASE("RepositoryUpdater normalizes configured repository URLs", "[plugins][updater]")
{
    const std::string github_api = "https://api.github.com/repos/example/repository";
    CHECK(Slic3r::RepositoryUpdater::normalize_repository_rest_url("example/repository") == github_api);
    CHECK(Slic3r::RepositoryUpdater::normalize_repository_rest_url("github.com/example/repository") == github_api);
    CHECK(Slic3r::RepositoryUpdater::normalize_repository_rest_url(
              "https://github.com/example/repository.git/") == github_api);
    CHECK(Slic3r::RepositoryUpdater::normalize_repository_rest_url(
              "https://api.github.com/repos/example/repository/") == github_api);
    CHECK(Slic3r::RepositoryUpdater::normalize_repository_rest_url(
              "https://updates.example.com/repository/") ==
          "https://updates.example.com/repository");
    CHECK(Slic3r::RepositoryUpdater::normalize_repository_rest_url(std::string()).empty());
}

TEST_CASE("RepositoryUpdater refreshes tags through cache and transport", "[plugins][updater]")
{
    FakeUpdaterHttpTransport http;
    TestRepositoryUpdater updater(http);
    TemporaryDirectory temporary;
    const boost::filesystem::path cache_file = temporary.path() / "tags.json";
    std::string parsed_contents;
    std::optional<Slic3r::UpdaterError> refresh_result;
    int sync_callback_count = 0;

    SECTION("a recent cache avoids HTTP") {
        boost::nowide::ofstream stream(cache_file.string());
        stream << "cached tags";
        stream.close();

        REQUIRE(updater.begin_sync(1, [&sync_callback_count](int count) {
            CHECK(count == 7);
            ++sync_callback_count;
        }));
        updater.refresh_repository_tags(
            "cached", "https://example.invalid/cached", cache_file, false,
            [&parsed_contents](const std::string &contents) {
                parsed_contents = contents;
                return Slic3r::UpdaterError();
            },
            [&refresh_result](Slic3r::UpdaterError error) { refresh_result = std::move(error); });

        CHECK(http.pending_count() == 0);
        CHECK(parsed_contents == "cached tags");
        REQUIRE(refresh_result.has_value());
        CHECK(refresh_result->succeeded());
        CHECK(sync_callback_count == 1);
    }

    SECTION("an invalid cached body completes as a failure") {
        boost::nowide::ofstream stream(cache_file.string());
        stream << "invalid tags";
        stream.close();

        REQUIRE(updater.begin_sync(1, [&sync_callback_count](int) { ++sync_callback_count; }));
        updater.refresh_repository_tags(
            "invalid", "https://example.invalid/invalid", cache_file, false,
            [](const std::string &) {
                return Slic3r::make_updater_error(Slic3r::UpdaterError::Code::InvalidRepositoryMetadata,
                                                  "invalid tags");
            },
            [&refresh_result](Slic3r::UpdaterError error) { refresh_result = std::move(error); });

        REQUIRE(refresh_result.has_value());
        CHECK(refresh_result->code == Slic3r::UpdaterError::Code::InvalidRepositoryMetadata);
        CHECK(refresh_result->detail == "invalid tags");
        CHECK(sync_callback_count == 1);
    }

    SECTION("force downloads and replaces the cache") {
        boost::nowide::ofstream stream(cache_file.string());
        stream << "old tags";
        stream.close();

        REQUIRE(updater.begin_sync(1, [&sync_callback_count](int) { ++sync_callback_count; }));
        updater.refresh_repository_tags(
            "forced", "https://example.invalid/forced", cache_file, true,
            [&parsed_contents](const std::string &contents) {
                parsed_contents = contents;
                return Slic3r::UpdaterError();
            },
            [&refresh_result](Slic3r::UpdaterError error) { refresh_result = std::move(error); });

        REQUIRE(http.pending_count() == 1);
        CHECK(http.pending_front().url() ==
              "https://example.invalid/forced/tags?per_page=100;page=1");
        CHECK(http.pending_front().response_size_limit() == 64 * 1024);
        CHECK(sync_callback_count == 0);

        http.succeed_front("fresh tags", 200);

        CHECK(parsed_contents == "fresh tags");
        CHECK(read_test_file(cache_file) == "fresh tags");
        REQUIRE(refresh_result.has_value());
        CHECK(refresh_result->succeeded());
        CHECK(sync_callback_count == 1);
    }

    SECTION("a transport failure completes once") {
        REQUIRE(updater.begin_sync(1, [&sync_callback_count](int) { ++sync_callback_count; }));
        updater.refresh_repository_tags(
            "offline", "https://example.invalid/offline", cache_file, false,
            [](const std::string &) { return Slic3r::UpdaterError(); },
            [&refresh_result](Slic3r::UpdaterError error) { refresh_result = std::move(error); });

        REQUIRE(http.pending_count() == 1);
        http.fail_front(std::string(), "offline", 0);

        REQUIRE(refresh_result.has_value());
        CHECK(refresh_result->code == Slic3r::UpdaterError::Code::Network);
        CHECK(refresh_result->detail == "offline");
        CHECK(sync_callback_count == 1);
        CHECK(http.pending_count() == 0);
    }
}

TEST_CASE("RepositoryUpdater refuses tag refresh after the GitHub request limit", "[plugins][updater]")
{
    FakeUpdaterHttpTransport http;
    TestRepositoryUpdater updater(http);
    TemporaryDirectory temporary;
    std::optional<Slic3r::UpdaterError> refresh_result;
    int sync_callback_count = 0;

    for (size_t request = 0; request < 24; ++request)
        REQUIRE(updater.has_api_request_slot("https://api.github.com/repos/example/repository"));

    REQUIRE(updater.begin_sync(1, [&sync_callback_count](int) { ++sync_callback_count; }));
    updater.refresh_repository_tags(
        "limited", "https://api.github.com/repos/example/repository", temporary.path() / "tags.json", true,
        [](const std::string &) { return Slic3r::UpdaterError(); },
        [&refresh_result](Slic3r::UpdaterError error) { refresh_result = std::move(error); });

    REQUIRE(refresh_result.has_value());
    CHECK(refresh_result->code == Slic3r::UpdaterError::Code::RateLimited);
    CHECK(sync_callback_count == 1);
    CHECK(http.pending_count() == 0);
}

TEST_CASE("RepositoryUpdater notifies callers that join an active refresh", "[plugins][updater]")
{
    FakeUpdaterHttpTransport http;
    TestRepositoryUpdater updater(http);
    TemporaryDirectory temporary;
    int first_callback_count = 0;
    int joined_callback_count = 0;
    std::optional<Slic3r::UpdaterError> refresh_result;

    REQUIRE(updater.begin_sync(1, [&first_callback_count](int count) {
        CHECK(count == 7);
        ++first_callback_count;
    }));
    updater.refresh_repository_tags(
        "active", "https://example.invalid/active", temporary.path() / "tags.json", true,
        [](const std::string &) { return Slic3r::UpdaterError(); },
        [&refresh_result](Slic3r::UpdaterError error) { refresh_result = std::move(error); });
    REQUIRE(http.pending_count() == 1);

    // A dialog opened during this request must wait for the existing refresh;
    // it must not rebuild immediately from the still-InProgress model.
    CHECK_FALSE(updater.begin_sync(1, [&joined_callback_count](int count) {
        CHECK(count == 7);
        ++joined_callback_count;
    }));
    CHECK(http.pending_count() == 1);
    CHECK(first_callback_count == 0);
    CHECK(joined_callback_count == 0);

    http.fail_front(std::string(), "offline", 0);

    REQUIRE(refresh_result.has_value());
    CHECK(refresh_result->code == Slic3r::UpdaterError::Code::Network);
    CHECK(first_callback_count == 1);
    CHECK(joined_callback_count == 1);
}

TEST_CASE("RepositoryUpdater reports precise tag refresh failures", "[plugins][updater]")
{
    FakeUpdaterHttpTransport http;
    TestRepositoryUpdater updater(http);
    TemporaryDirectory temporary;
    std::optional<Slic3r::UpdaterError> result;

    // Each section starts one logical repository refresh. The terminal result
    // must preserve enough information for a GUI to distinguish configuration,
    // transport, parsing and local filesystem failures.
    const std::function<void(Slic3r::UpdaterError)> store_result =
        [&result](Slic3r::UpdaterError error) { result = std::move(error); };
    const std::function<Slic3r::UpdaterError(const std::string &)> accept_tags =
        [](const std::string &) { return Slic3r::UpdaterError(); };

    SECTION("an empty URL is a repository error") {
        REQUIRE(updater.begin_sync(1, [](int) {}));
        updater.refresh_repository_tags("missing", std::string(), temporary.path() / "missing.json", true,
                                        accept_tags, store_result);

        REQUIRE(result.has_value());
        CHECK(result->code == Slic3r::UpdaterError::Code::RepositoryNotFound);
        CHECK_FALSE(result->detail.empty());
        CHECK(http.pending_count() == 0);
    }

    SECTION("an HTTP 404 is a repository error") {
        REQUIRE(updater.begin_sync(1, [](int) {}));
        updater.refresh_repository_tags("missing", "https://example.invalid/missing",
                                        temporary.path() / "missing.json", true, accept_tags, store_result);
        REQUIRE(http.pending_count() == 1);
        http.fail_front(std::string(), "not found", 404);

        REQUIRE(result.has_value());
        CHECK(result->code == Slic3r::UpdaterError::Code::RepositoryNotFound);
        CHECK(result->detail == "not found");
    }

    SECTION("an exception while starting HTTP is a network error") {
        http.throw_on_next_async_request("transport unavailable");
        REQUIRE(updater.begin_sync(1, [](int) {}));
        updater.refresh_repository_tags("throwing", "https://example.invalid/throwing",
                                        temporary.path() / "throwing.json", true, accept_tags, store_result);

        REQUIRE(result.has_value());
        CHECK(result->code == Slic3r::UpdaterError::Code::Network);
        CHECK(result->detail == "transport unavailable");
        CHECK(http.pending_count() == 0);
    }

    SECTION("a cache directory failure is a filesystem error") {
        const boost::filesystem::path blocking_file = temporary.path() / "not-a-directory";
        write_test_file(blocking_file, "file");

        REQUIRE(updater.begin_sync(1, [](int) {}));
        updater.refresh_repository_tags("filesystem", "https://example.invalid/filesystem",
                                        blocking_file / "tags.json", true, accept_tags, store_result);

        REQUIRE(result.has_value());
        CHECK(result->code == Slic3r::UpdaterError::Code::Filesystem);
        CHECK_FALSE(result->detail.empty());
        CHECK(http.pending_count() == 0);
    }

    SECTION("invalid downloaded tags preserve the parser detail") {
        REQUIRE(updater.begin_sync(1, [](int) {}));
        updater.refresh_repository_tags(
            "invalid", "https://example.invalid/invalid", temporary.path() / "invalid.json", true,
            [](const std::string &) {
                return Slic3r::make_updater_error(Slic3r::UpdaterError::Code::InvalidRepositoryMetadata,
                                                  "tag name has no slicer version");
            },
            store_result);
        REQUIRE(http.pending_count() == 1);
        http.succeed_front("invalid tags", 200);

        REQUIRE(result.has_value());
        CHECK(result->code == Slic3r::UpdaterError::Code::InvalidRepositoryMetadata);
        CHECK(result->detail == "tag name has no slicer version");
    }
}

TEST_CASE("RepositoryUpdater downloads changelog batches through cache and transport", "[plugins][updater]")
{
    FakeUpdaterHttpTransport http;
    TestRepositoryUpdater updater(http);
    TemporaryDirectory temporary;
    const boost::filesystem::path commit_cache = temporary.path() / "logs" / "commit.json";
    const boost::filesystem::path compare_cache = temporary.path() / "logs" / "compare.json";
    std::string commit_notes;
    std::string compare_notes;
    std::optional<bool> batch_succeeded;
    int callback_count = 0;

    TestRepositoryUpdater::RepositoryChangelogRequest commit;
    commit.cache_file = commit_cache;
    commit.url = "https://example.invalid/commit/one";
    commit.store_notes = [&commit_notes](std::string notes) { commit_notes = std::move(notes); };

    TestRepositoryUpdater::RepositoryChangelogRequest compare;
    compare.cache_file = compare_cache;
    compare.url = "https://example.invalid/compare/one...two";
    compare.compare = true;
    compare.store_notes = [&compare_notes](std::string notes) { compare_notes = std::move(notes); };

    updater.download_repository_changelogs(
        {commit, compare},
        [&batch_succeeded, &callback_count](bool succeeded) {
            batch_succeeded = succeeded;
            ++callback_count;
        },
        false);

    REQUIRE(updater.changelog_download_in_progress());
    REQUIRE(http.pending_count() == 2);
    CHECK(http.pending_at(0).response_size_limit() == 4 * 1024 * 1024);
    CHECK(http.pending_at(1).response_size_limit() == 128 * 1024);
    int rejected_sync_callback_count = 0;
    CHECK_FALSE(updater.begin_sync(1, [&rejected_sync_callback_count](int count) {
        CHECK(count == 7);
        ++rejected_sync_callback_count;
    }));
    CHECK(rejected_sync_callback_count == 1);

    const std::string commit_json = R"({"commit":{"message":"single commit"}})";
    http.succeed_front(commit_json, 200);
    CHECK(callback_count == 0);
    CHECK(updater.changelog_download_in_progress());

    const std::string compare_json =
        R"({"commits":[{"commit":{"message":"older"}},{"commit":{"message":"newer"}}]})";
    http.succeed_front(compare_json, 200);

    REQUIRE(batch_succeeded.has_value());
    CHECK(*batch_succeeded);
    CHECK(callback_count == 1);
    CHECK_FALSE(updater.changelog_download_in_progress());
    CHECK(commit_notes == "single commit");
    CHECK(compare_notes == "newer\nolder");
    CHECK(read_test_file(commit_cache) == commit_json);
    CHECK(read_test_file(compare_cache) == compare_json);

    // Cached commit data is immutable. A second batch completes synchronously
    // and restores the notes without consuming another HTTP request.
    commit_notes.clear();
    compare_notes.clear();
    batch_succeeded.reset();
    updater.download_repository_changelogs(
        {commit, compare},
        [&batch_succeeded, &callback_count](bool succeeded) {
            batch_succeeded = succeeded;
            ++callback_count;
        },
        false);
    REQUIRE(batch_succeeded.has_value());
    CHECK(*batch_succeeded);
    CHECK(callback_count == 2);
    CHECK(http.pending_count() == 0);
    CHECK(commit_notes == "single commit");
    CHECK(compare_notes == "newer\nolder");
}

TEST_CASE("RepositoryUpdater changelog failures complete one aggregate callback", "[plugins][updater]")
{
    FakeUpdaterHttpTransport http;
    TestRepositoryUpdater updater(http);
    TemporaryDirectory temporary;
    std::optional<bool> batch_succeeded;
    int callback_count = 0;

    TestRepositoryUpdater::RepositoryChangelogRequest invalid;
    invalid.cache_file = temporary.path() / "invalid.json";
    invalid.url = "https://example.invalid/invalid";
    invalid.store_notes = [](std::string) {};

    TestRepositoryUpdater::RepositoryChangelogRequest offline;
    offline.cache_file = temporary.path() / "offline.json";
    offline.url = "https://example.invalid/offline";
    offline.store_notes = [](std::string) {};

    updater.download_repository_changelogs(
        {invalid, offline},
        [&batch_succeeded, &callback_count](bool succeeded) {
            batch_succeeded = succeeded;
            ++callback_count;
        },
        false);
    REQUIRE(http.pending_count() == 2);

    http.succeed_front("not JSON", 200);
    CHECK(callback_count == 0);
    http.fail_front(std::string(), "offline", 0);

    REQUIRE(batch_succeeded.has_value());
    CHECK_FALSE(*batch_succeeded);
    CHECK(callback_count == 1);
    CHECK_FALSE(updater.changelog_download_in_progress());
}

TEST_CASE("RepositoryUpdater force and rate limits apply to changelogs", "[plugins][updater]")
{
    FakeUpdaterHttpTransport http;
    TestRepositoryUpdater updater(http);
    TemporaryDirectory temporary;
    const boost::filesystem::path cache_file = temporary.path() / "cached.json";
    write_test_file(cache_file, R"({"commit":{"message":"cached"}})");

    TestRepositoryUpdater::RepositoryChangelogRequest request;
    request.cache_file = cache_file;
    request.url = "https://example.invalid/forced";
    request.store_notes = [](std::string) {};
    std::optional<bool> forced_result;
    updater.download_repository_changelogs(
        {request}, [&forced_result](bool succeeded) { forced_result = succeeded; }, true);
    REQUIRE(http.pending_count() == 1);
    CHECK_FALSE(forced_result.has_value());
    http.succeed_front(R"({"commit":{"message":"fresh"}})", 200);
    REQUIRE(forced_result.has_value());
    CHECK(*forced_result);

    for (size_t request_idx = 0; request_idx < 24; ++request_idx)
        REQUIRE(updater.has_api_request_slot("https://api.github.com/repos/example/repository"));

    request.cache_file = temporary.path() / "limited.json";
    request.url = "https://api.github.com/repos/example/repository/commits/one";
    std::optional<bool> limited_result;
    updater.download_repository_changelogs(
        {request}, [&limited_result](bool succeeded) { limited_result = succeeded; }, true);
    REQUIRE(limited_result.has_value());
    CHECK_FALSE(*limited_result);
    CHECK(http.pending_count() == 0);
}

TEST_CASE("RepositoryUpdater downloads repository descriptions", "[plugins][updater]")
{
    FakeUpdaterHttpTransport http;
    TestRepositoryUpdater updater(http);
    std::string consumed_contents;
    std::string consumed_fallback_id;
    std::optional<Slic3r::UpdaterError> result;

    updater.download_repository_description(
        "https://api.github.com/repos/example/repository",
        [&consumed_contents, &consumed_fallback_id](const std::string &contents, const std::string &fallback_id) {
            consumed_contents = contents;
            consumed_fallback_id = fallback_id;
            return Slic3r::UpdaterError();
        },
        [&result](Slic3r::UpdaterError error) { result = std::move(error); });

    REQUIRE(http.pending_count() == 1);
    CHECK(http.pending_front().url() ==
          "https://raw.githubusercontent.com/example/repository/refs/heads/main/description.ini");
    CHECK(http.pending_front().response_size_limit() == 64 * 1024);
    http.succeed_front("repository description", 200);

    REQUIRE(result.has_value());
    CHECK(result->succeeded());
    CHECK(consumed_contents == "repository description");
    CHECK(consumed_fallback_id == "repository");
}

TEST_CASE("RepositoryUpdater writes asynchronous and synchronous repository files", "[plugins][updater]")
{
    FakeUpdaterHttpTransport http;
    TestRepositoryUpdater updater(http);
    TemporaryDirectory temporary;

    SECTION("asynchronous success") {
        const boost::filesystem::path destination = temporary.path() / "async" / "archive.zip";
        std::optional<Slic3r::UpdaterError> result;
        updater.download_repository_file_async(
            "https://example.invalid/async.zip", destination, 4096,
            [&result](Slic3r::UpdaterError error) { result = std::move(error); });

        REQUIRE(http.pending_count() == 1);
        CHECK(http.pending_front().response_size_limit() == 4096);
        http.succeed_front("async archive", 200);

        REQUIRE(result.has_value());
        CHECK(result->succeeded());
        CHECK(read_test_file(destination) == "async archive");
    }

    SECTION("synchronous success") {
        const boost::filesystem::path destination = temporary.path() / "sync" / "archive.zip";
        http.script_sync_success("sync archive");

        const Slic3r::UpdaterError result = updater.download_repository_file_sync(
            "https://example.invalid/sync.zip", destination, 8192);

        CHECK(result.succeeded());
        CHECK(read_test_file(destination) == "sync archive");
    }

    SECTION("network and URL errors keep their categories") {
        http.script_sync_failure("connection refused");
        const Slic3r::UpdaterError network = updater.download_repository_file_sync(
            "https://example.invalid/failure.zip", temporary.path() / "failure.zip", 1024);
        CHECK(network.code == Slic3r::UpdaterError::Code::Network);
        CHECK(network.detail == "connection refused");

        std::optional<Slic3r::UpdaterError> missing;
        updater.download_repository_file_async(
            std::string(), temporary.path() / "missing.zip", 1024,
            [&missing](Slic3r::UpdaterError error) { missing = std::move(error); });
        REQUIRE(missing.has_value());
        CHECK(missing->code == Slic3r::UpdaterError::Code::ArchiveUnavailable);
        CHECK(http.pending_count() == 0);
    }

    SECTION("a local write failure is a filesystem error") {
        std::optional<Slic3r::UpdaterError> result;
        updater.download_repository_file_async(
            "https://example.invalid/archive.zip", temporary.path(), 1024,
            [&result](Slic3r::UpdaterError error) { result = std::move(error); });

        REQUIRE(http.pending_count() == 1);
        http.succeed_front("cannot be written over a directory", 200);
        REQUIRE(result.has_value());
        CHECK(result->code == Slic3r::UpdaterError::Code::Filesystem);

        http.script_sync_success("cannot be written either");
        const Slic3r::UpdaterError sync_result = updater.download_repository_file_sync(
            "https://example.invalid/sync-archive.zip", temporary.path(), 1024);
        CHECK(sync_result.code == Slic3r::UpdaterError::Code::Filesystem);
    }

    SECTION("the GitHub limit is reported without starting an archive request") {
        for (size_t request = 0; request < 24; ++request)
            REQUIRE(updater.has_api_request_slot("https://api.github.com/repos/example/repository"));

        std::optional<Slic3r::UpdaterError> result;
        updater.download_repository_file_async(
            "https://api.github.com/repos/example/repository/zipball/v1", temporary.path() / "limited.zip", 1024,
            [&result](Slic3r::UpdaterError error) { result = std::move(error); });

        REQUIRE(result.has_value());
        CHECK(result->code == Slic3r::UpdaterError::Code::RateLimited);
        CHECK(http.pending_count() == 0);
    }
}

TEST_CASE("VendorSync enriches matching cached versions from repository tags", "[plugins][updater]")
{
    Slic3r::VendorSync vendor;
    Slic3r::VendorAvailable cached;
    cached.config_version = *Slic3r::Semver::parse("1.2.3.4");
    cached.slicer_version = *Slic3r::Semver::parse("2.7.63.0");
    cached.local_file = "cached.ini";
    cached.tag = "1.2.3.4=2.7.63.0";
    vendor.available_profiles.emplace_back(cached);

    REQUIRE(vendor.parse_tags(
        "[{\"name\":\"1.2.3.4=2.7.63.0\",\"zipball_url\":\"zip\","
        "\"commit\":{\"sha\":\"sha\",\"url\":\"commit\"}}]").succeeded());
    REQUIRE(vendor.available_profiles.size() == 1);
    CHECK(vendor.available_profiles.front().local_file == "cached.ini");
    CHECK(vendor.available_profiles.front().url_zip == "zip");
    CHECK(vendor.available_profiles.front().commit_sha == "sha");
    CHECK(vendor.available_profiles.front().commit_url == "commit");
}

TEST_CASE("PluginUpdater reports a transport failure without a real HTTP request", "[plugins][updater]")
{
    FakeUpdaterHttpTransport http;
    Slic3r::PluginUpdater updater(http);
    std::optional<Slic3r::UpdaterError> result;

    updater.download_new_repo("example/plugin", [&result](Slic3r::UpdaterError error) {
        result = std::move(error);
    });

    REQUIRE(http.pending_count() == 1);
    CHECK(http.pending_front().url() ==
          "https://raw.githubusercontent.com/example/plugin/refs/heads/main/description.ini");
    CHECK(http.pending_front().response_size_limit() == 64 * 1024);
    CHECK_FALSE(result.has_value());

    http.fail_front(std::string(), "connection refused", 0);

    REQUIRE(result.has_value());
    CHECK(result->code == Slic3r::UpdaterError::Code::Network);
    CHECK(result->detail == "connection refused");
    CHECK(http.pending_count() == 0);
}

TEST_CASE_METHOD(PluginUpdaterFunctionalFixture,
                 "PluginUpdater replaces a synchronization error after a successful retry",
                 "[plugins][updater][plugin-functional]")
{
    write_plugin_repository();
    updater.reload_all_plugins();
    Slic3r::PluginSync *plugin = updater.get_plugin(plugin_id);
    REQUIRE(plugin != nullptr);
    CHECK(plugin->sync_state == Slic3r::RepositorySyncState::Unchecked);
    CHECK(plugin->sync_error.succeeded());

    std::optional<int> first_count;
    updater.sync_async([&first_count](int count) { first_count = count; }, true);
    CHECK(plugin->sync_state == Slic3r::RepositorySyncState::InProgress);
    CHECK(plugin->sync_error.succeeded());
    REQUIRE(http.pending_count() == 1);
    http.fail_front(std::string(), "offline", 0);

    REQUIRE(first_count.has_value());
    CHECK(plugin->sync_state == Slic3r::RepositorySyncState::Failed);
    CHECK(plugin->sync_error.code == Slic3r::UpdaterError::Code::Network);
    CHECK(plugin->sync_error.detail == "offline");

    // Starting a retry immediately clears the stale error. Its successful
    // terminal callback then leaves one unambiguous completed state.
    std::optional<int> retry_count;
    updater.sync_async([&retry_count](int count) { retry_count = count; }, true);
    CHECK(plugin->sync_state == Slic3r::RepositorySyncState::InProgress);
    CHECK(plugin->sync_error.succeeded());
    REQUIRE(http.pending_count() == 1);
    http.succeed_front(plugin_repository_tags({
        {"1.0.0.0", slicer_version, "https://example.invalid/plugin.zip"}
    }), 200);

    REQUIRE(retry_count.has_value());
    CHECK(plugin->sync_state == Slic3r::RepositorySyncState::Succeeded);
    CHECK(plugin->sync_error.succeeded());
}

TEST_CASE("PluginUpdater selects comparable versions and caches their changelogs", "[plugins][updater]")
{
    FakeUpdaterHttpTransport http;
    TemporaryDirectory temporary;
    const boost::filesystem::path resources_directory = temporary.path() / "resources";
    const boost::filesystem::path data_directory = temporary.path() / "data";
    write_test_file(resources_directory / "plugins" / "default_activated.ini",
                    "[installed]\n\n[activated]\n");
    REQUIRE(write_test_zip(
        resources_directory / "plugins" / "example.plugin_1.0.0.0_2.7.63.0.zip",
        {{"description.ini", plugin_description_contents(
                                 "example.plugin", "1.0.0.0", "2.7.63.0", true, "example/repository")},
         {plugin_library_filename(), "embedded library"}}));
    ScopedUpdaterDirectories directories(resources_directory, data_directory);

    Slic3r::PluginUpdater updater(http);
    updater.reload_all_plugins();
    REQUIRE(updater.count_available() == 1);
    std::optional<int> update_count;
    updater.sync_async([&update_count](int count) { update_count = count; }, true);
    REQUIRE(http.pending_count() == 1);
    CHECK(http.pending_front().url() ==
          "https://api.github.com/repos/example/repository/tags?per_page=100;page=1");

    const std::string tags =
        "[{\"name\":\"3.0.0.0=2.8.0.0\",\"zipball_url\":\"zip3\","
        "\"commit\":{\"sha\":\"sha3\",\"url\":\"commit3\"}},"
        "{\"name\":\"2.0.0.0=2.7.64.0\",\"zipball_url\":\"zip2\","
        "\"commit\":{\"sha\":\"sha2\",\"url\":\"commit2\"}},"
        "{\"name\":\"1.5.0.0=2.7.64.0\",\"zipball_url\":\"zip15\","
        "\"commit\":{\"sha\":\"sha15\",\"url\":\"commit15\"}},"
        "{\"name\":\"1.0.0.0=2.7.63.0\",\"zipball_url\":\"zip1\","
        "\"commit\":{\"sha\":\"sha1\",\"url\":\"commit1\"}}]";
    http.succeed_front(tags, 200);
    REQUIRE(update_count.has_value());

    Slic3r::PluginSync *plugin = updater.get_plugin("example.plugin");
    REQUIRE(plugin != nullptr);
    REQUIRE(plugin->available_packages.size() == 4);
    std::optional<bool> changelogs_succeeded;
    int callback_count = 0;
    updater.download_changelogs(
        "example.plugin",
        [&changelogs_succeeded, &callback_count](bool succeeded) {
            changelogs_succeeded = succeeded;
            ++callback_count;
        });

    REQUIRE(http.pending_count() == 4);
    CHECK(http.pending_at(0).url() ==
          "https://api.github.com/repos/example/repository/compare/2.0.0.0=2.7.64.0...3.0.0.0=2.8.0.0");
    CHECK(http.pending_at(1).url() ==
          "https://api.github.com/repos/example/repository/compare/1.5.0.0=2.7.64.0...2.0.0.0=2.7.64.0");
    CHECK(http.pending_at(2).url() ==
          "https://api.github.com/repos/example/repository/compare/1.0.0.0=2.7.63.0...1.5.0.0=2.7.64.0");
    CHECK(http.pending_at(3).url() == "commit1");

    // Reload is deliberately ignored while callbacks retain pointers into the
    // plugin map. This is the lifetime guarantee used by store_notes.
    updater.reload_all_plugins();
    CHECK(updater.get_plugin("example.plugin") == plugin);
    CHECK(plugin->available_packages.size() == 4);

    const std::string compare_json =
        R"({"commits":[{"commit":{"message":"older"}},{"commit":{"message":"newer"}}]})";
    http.succeed_front(compare_json, 200);
    http.succeed_front(compare_json, 200);
    http.succeed_front(compare_json, 200);
    http.succeed_front(R"({"commit":{"message":"initial"}})", 200);

    REQUIRE(changelogs_succeeded.has_value());
    CHECK(*changelogs_succeeded);
    CHECK(callback_count == 1);
    CHECK(plugin->available_packages[0].notes == "newer\nolder");
    CHECK(plugin->available_packages[1].notes == "newer\nolder");
    CHECK(plugin->available_packages[2].notes == "newer\nolder");
    CHECK(plugin->available_packages[3].notes == "initial");

    const boost::filesystem::path log_directory = Slic3r::repository_cache_root_path(
        data_directory, Slic3r::RepositoryPackageType::Plugin, "example.plugin") / "logs";
    CHECK(boost::filesystem::is_regular_file(
        log_directory / "2.0.0.0=2.7.64.0...3.0.0.0=2.8.0.0.json"));
    CHECK(boost::filesystem::is_regular_file(log_directory / "1.0.0.0=2.7.63.0.json"));

    // A second request restores every note from the plugin cache and performs
    // no HTTP work. This also verifies that cache filenames map back to the
    // same package entries after the first batch.
    for (Slic3r::PluginAvailable &version : plugin->available_packages)
        version.notes.clear();
    changelogs_succeeded.reset();
    updater.download_changelogs(
        "example.plugin",
        [&changelogs_succeeded, &callback_count](bool succeeded) {
            changelogs_succeeded = succeeded;
            ++callback_count;
        });
    REQUIRE(changelogs_succeeded.has_value());
    CHECK(*changelogs_succeeded);
    CHECK(callback_count == 2);
    CHECK(http.pending_count() == 0);
    CHECK(plugin->available_packages[3].notes == "initial");
}

TEST_CASE_METHOD(PluginUpdaterFunctionalFixture,
                 "PluginUpdater functional version dialog schedules a non-latest plugin",
                 "[plugins][updater][plugin-functional]")
{
    write_plugin_repository();
    updater.reload_all_plugins();
    synchronize({
        {"3.0.0.0", slicer_version, "https://example.invalid/plugin-3.zip"},
        {"2.0.0.0", slicer_version, "https://example.invalid/plugin-2.zip"},
        {"1.0.0.0", slicer_version, "https://example.invalid/plugin-1.zip"}
    });

    Slic3r::PluginSync *plugin = updater.get_plugin(plugin_id);
    REQUIRE(plugin != nullptr);
    REQUIRE(plugin->best != nullptr);
    CHECK(plugin->best->package_version == "3.0.0.0");

    std::optional<bool> changelogs_succeeded;
    int changelog_callback_count = 0;
    updater.download_changelogs(
        plugin_id,
        [&changelogs_succeeded, &changelog_callback_count](bool succeeded) {
            changelogs_succeeded = succeeded;
            ++changelog_callback_count;
        });
    REQUIRE(http.pending_count() == 3);
    while (http.pending_count() != 0) {
        const bool compare = http.pending_front().url().find("/compare/") != std::string::npos;
        http.succeed_front(compare ?
            R"({"commits":[{"commit":{"message":"selected change"}}]})" :
            R"({"commit":{"message":"initial release"}})", 200);
    }
    REQUIRE(changelogs_succeeded.has_value());
    CHECK(*changelogs_succeeded);
    CHECK(changelog_callback_count == 1);

    const std::vector<Slic3r::PluginAvailable>::const_iterator selected = std::find_if(
        plugin->available_packages.begin(), plugin->available_packages.end(),
        [](const Slic3r::PluginAvailable &version) { return version.package_version == "2.0.0.0"; });
    REQUIRE(selected != plugin->available_packages.end());
    CHECK_FALSE(selected->notes.empty());

    std::optional<Slic3r::UpdaterError> install_result;
    updater.install_plugin(plugin_id, *selected, [&install_result](Slic3r::UpdaterError error) {
        install_result = std::move(error);
    });
    REQUIRE(http.pending_count() == 1);
    CHECK(http.pending_front().url() == "https://example.invalid/plugin-2.zip");
    http.succeed_front(make_plugin_archive("2.0.0.0"), 200);

    REQUIRE(install_result.has_value());
    CHECK(install_result->succeeded());
    CHECK_FALSE(boost::filesystem::exists(data_directory / "plugins" / plugin_id));
    CHECK(boost::filesystem::is_directory(Slic3r::repository_package_cache_path(
        data_directory, Slic3r::RepositoryPackageType::Plugin, plugin_id, "2.0.0.0", slicer_version)));

    const Slic3r::PluginActivationConfig config = read_activation_config();
    REQUIRE(config.installed.count(plugin_id) == 1);
    CHECK(config.installed.at(plugin_id).package_version == "2.0.0.0");
    CHECK(config.installed.at(plugin_id).slicer_version == slicer_version);
    CHECK(config.removed.count(plugin_id) == 0);
    const std::string activation_contents = read_test_file(Slic3r::plugin_activation_config_path(data_directory));
    CHECK(activation_contents.find(plugin_id + " = 2.0.0.0") != std::string::npos);
    CHECK(activation_contents.find(plugin_id + ".slicer_version = " + slicer_version) != std::string::npos);
}

TEST_CASE_METHOD(PluginUpdaterFunctionalFixture,
                 "Plugin package requests are applied safely at startup",
                 "[plugins][updater][plugin-functional]")
{
    SECTION("a cached request is installed") {
        write_cached_plugin("1.0.0.0");
        Slic3r::PluginActivationConfig config;
        config.installed[plugin_id] = {"1.0.0.0", slicer_version};
        std::string error_message;
        REQUIRE(Slic3r::write_plugin_activation_config(
            Slic3r::plugin_activation_config_path(data_directory), config, error_message));

        std::vector<std::string> warnings;
        REQUIRE(Slic3r::apply_requested_plugin_package_changes(
            data_directory, config, warnings, error_message));
        CHECK(warnings.empty());
        CHECK(boost::filesystem::is_regular_file(data_directory / "plugins" / plugin_id / plugin_library_filename()));
        CHECK(read_test_file(data_directory / "plugins" / plugin_id / plugin_library_filename()) ==
              "cached library 1.0.0.0");
    }

    SECTION("a missing cache reports an error without replacing the live plugin") {
        write_installed_plugin("1.0.0.0");
        Slic3r::PluginActivationConfig config;
        config.installed[plugin_id] = {"9.9.9.9", slicer_version};
        std::string error_message;
        REQUIRE(Slic3r::write_plugin_activation_config(
            Slic3r::plugin_activation_config_path(data_directory), config, error_message));

        std::vector<std::string> warnings;
        CHECK_FALSE(Slic3r::apply_requested_plugin_package_changes(
            data_directory, config, warnings, error_message));
        CHECK(warnings.empty());
        CHECK(error_message.find("not cached") != std::string::npos);
        CHECK(read_test_file(data_directory / "plugins" / plugin_id / plugin_library_filename()) ==
              "installed library 1.0.0.0");
    }
}

TEST_CASE_METHOD(PluginUpdaterFunctionalFixture,
                 "PluginUpdater schedules removal without unloading the current plugin",
                 "[plugins][updater][plugin-functional]")
{
    write_installed_plugin("1.0.0.0");
    updater.reload_all_plugins();
    Slic3r::PluginSync *plugin = updater.get_plugin(plugin_id);
    REQUIRE(plugin != nullptr);
    REQUIRE(plugin->is_installed);

    std::optional<Slic3r::UpdaterError> uninstall_result;
    updater.uninstall_plugin(plugin_id, [&uninstall_result](Slic3r::UpdaterError error) {
        uninstall_result = std::move(error);
    });

    REQUIRE(uninstall_result.has_value());
    CHECK(uninstall_result->succeeded());
    CHECK(boost::filesystem::is_directory(data_directory / "plugins" / plugin_id));
    const Slic3r::PluginActivationConfig config = read_activation_config();
    CHECK(config.installed.count(plugin_id) == 0);
    CHECK(config.removed.count(plugin_id) == 1);
    CHECK_FALSE(plugin->is_installed);
}

TEST_CASE_METHOD(PluginUpdaterFunctionalFixture,
                 "Plugin removals are consumed at startup without deleting their cache",
                 "[plugins][updater][plugin-functional]")
{
    SECTION("an installed package is removed") {
        write_installed_plugin("1.0.0.0");
        const boost::filesystem::path cache_root = write_cached_plugin("1.0.0.0");
        Slic3r::PluginActivationConfig config;
        config.removed.insert(plugin_id);
        std::string error_message;
        REQUIRE(Slic3r::write_plugin_activation_config(
            Slic3r::plugin_activation_config_path(data_directory), config, error_message));

        std::vector<std::string> warnings;
        REQUIRE(Slic3r::apply_requested_plugin_package_changes(
            data_directory, config, warnings, error_message));
        CHECK(warnings.empty());
        CHECK_FALSE(boost::filesystem::exists(data_directory / "plugins" / plugin_id));
        CHECK(boost::filesystem::is_directory(cache_root));
        CHECK(read_activation_config().removed.count(plugin_id) == 0);
    }

    SECTION("an already absent package emits a non-fatal warning") {
        const boost::filesystem::path cache_root = write_cached_plugin("1.0.0.0");
        Slic3r::PluginActivationConfig config;
        config.removed.insert(plugin_id);
        std::string error_message;
        REQUIRE(Slic3r::write_plugin_activation_config(
            Slic3r::plugin_activation_config_path(data_directory), config, error_message));

        std::vector<std::string> warnings;
        REQUIRE(Slic3r::apply_requested_plugin_package_changes(
            data_directory, config, warnings, error_message));
        REQUIRE(warnings.size() == 1);
        CHECK(warnings.front().find(plugin_id) != std::string::npos);
        CHECK(boost::filesystem::is_directory(cache_root));
        CHECK(read_activation_config().removed.count(plugin_id) == 0);
    }
}

TEST_CASE_METHOD(PluginUpdaterFunctionalFixture,
                 "PluginUpdater schedules a valid cached package without HTTP",
                 "[plugins][updater][plugin-functional]")
{
    write_plugin_repository();
    write_cached_plugin("2.0.0.0");
    Slic3r::PluginActivationConfig removal_config;
    removal_config.removed.insert(plugin_id);
    std::string config_error;
    REQUIRE(Slic3r::write_plugin_activation_config(
        Slic3r::plugin_activation_config_path(data_directory), removal_config, config_error));
    updater.reload_all_plugins();

    Slic3r::PluginAvailable version;
    version.package_version = "2.0.0.0";
    version.slicer_version = slicer_version;
    version.url_zip = "https://example.invalid/must-not-download.zip";
    std::optional<Slic3r::UpdaterError> install_result;
    updater.install_plugin(plugin_id, version, [&install_result](Slic3r::UpdaterError error) {
        install_result = std::move(error);
    });

    REQUIRE(install_result.has_value());
    CHECK(install_result->succeeded());
    CHECK(http.pending_count() == 0);
    CHECK(http.sync_request_count() == 0);
    const Slic3r::PluginActivationConfig config = read_activation_config();
    REQUIRE(config.installed.count(plugin_id) == 1);
    CHECK(config.installed.at(plugin_id).package_version == "2.0.0.0");
    CHECK(config.removed.count(plugin_id) == 0);
}

TEST_CASE_METHOD(PluginUpdaterFunctionalFixture,
                 "PluginUpdater imports an unpacked local package",
                 "[plugins][updater][plugin-functional]")
{
    const boost::filesystem::path package = temporary.path() / "local_only_plugin";
    write_test_file(package / plugin_library_filename(), "local library");

    const Slic3r::UpdaterError import_error = updater.cache_plugin_directory(package);
    REQUIRE(import_error.succeeded());
    updater.reload_all_plugins();

    Slic3r::PluginSync *plugin = updater.get_plugin("local_only_plugin");
    REQUIRE(plugin != nullptr);
    CHECK(plugin->description.config_update_rest.empty());
    REQUIRE(plugin->available_packages.size() == 1);
    CHECK(plugin->available_packages.front().package_version == "1.0.0.0");
    CHECK(plugin->available_packages.front().slicer_version == "1.0.0.0");
    CHECK_FALSE(plugin->available_packages.front().local_directory.empty());
    CHECK(http.pending_count() == 0);
}

TEST_CASE_METHOD(PluginUpdaterFunctionalFixture,
                 "PluginUpdater reuses recent changelogs and refreshes stale files",
                 "[plugins][updater][plugin-functional]")
{
    write_plugin_repository();
    updater.reload_all_plugins();
    synchronize({{"1.0.0.0", slicer_version, "https://example.invalid/plugin-1.zip"}});

    Slic3r::PluginSync *plugin = updater.get_plugin(plugin_id);
    REQUIRE(plugin != nullptr);
    REQUIRE(plugin->available_packages.size() == 1);
    const std::string tag = "1.0.0.0=" + slicer_version;
    const boost::filesystem::path cache_file = Slic3r::repository_cache_root_path(
        data_directory, Slic3r::RepositoryPackageType::Plugin, plugin_id) / "logs" / (tag + ".json");
    write_test_file(cache_file, R"({"commit":{"message":"cached notes"}})");

    std::optional<bool> changelogs_succeeded;
    int callback_count = 0;
    updater.download_changelogs(plugin_id, [&changelogs_succeeded, &callback_count](bool succeeded) {
        changelogs_succeeded = succeeded;
        ++callback_count;
    });
    REQUIRE(changelogs_succeeded.has_value());
    CHECK(*changelogs_succeeded);
    CHECK(callback_count == 1);
    CHECK(http.pending_count() == 0);
    CHECK(plugin->available_packages.front().notes == "cached notes");

    boost::filesystem::last_write_time(cache_file, std::time(nullptr) - 24 * 3600 - 1);
    plugin->available_packages.front().notes.clear();
    changelogs_succeeded.reset();
    updater.download_changelogs(plugin_id, [&changelogs_succeeded, &callback_count](bool succeeded) {
        changelogs_succeeded = succeeded;
        ++callback_count;
    });
    CHECK_FALSE(changelogs_succeeded.has_value());
    REQUIRE(http.pending_count() == 1);
    http.succeed_front(R"({"commit":{"message":"refreshed notes"}})", 200);

    REQUIRE(changelogs_succeeded.has_value());
    CHECK(*changelogs_succeeded);
    CHECK(callback_count == 2);
    CHECK(plugin->available_packages.front().notes == "refreshed notes");
    CHECK(read_test_file(cache_file).find("refreshed notes") != std::string::npos);
}

TEST_CASE("Updater HTTP progress remains controllable from a fake transport", "[plugins][updater]")
{
    FakeUpdaterHttpTransport http;
    size_t observed_current = 0;
    std::string observed_buffer;

    http.get("https://example.invalid/archive")
        .on_progress([&observed_current, &observed_buffer](Slic3r::UpdaterHttpRequest::Progress progress, bool &cancel) {
            observed_current = progress.dlnow;
            observed_buffer = progress.buffer;
            cancel = progress.dlnow >= progress.dltotal;
        })
        .perform();

    REQUIRE(http.pending_count() == 1);
    CHECK(http.progress_front(10, 4, "part") == false);
    CHECK(observed_current == 4);
    CHECK(observed_buffer == "part");
    CHECK(http.progress_front(10, 10, "complete"));
    http.succeed_front(std::string(), 200);
    CHECK(http.pending_count() == 0);
}

TEST_CASE_METHOD(PresetUpdaterFunctionalFixture,
                 "PresetUpdater functional dialog installs an uninstalled bundled vendor",
                 "[plugins][updater][preset-functional]")
{
    write_resource_vendor("1.0.0.0");
    updater.reload_all_vendors();

    const PresetDialogSnapshot dialog = synchronize_and_open_dialog({
        {"1.0.0.0", slicer_version, "https://example.invalid/vendor-1.zip"}
    });
    REQUIRE(dialog.vendors.size() == 1);
    CHECK(dialog.profile_count_to_update == 0);

    // vendors() is the detached model read by the selection dialog. Installing
    // through its best pointer verifies that copied models rebuild that pointer
    // correctly instead of retaining an address from the updater's map.
    const Slic3r::VendorSync &dialog_vendor = dialog.vendors.front();
    CHECK_FALSE(dialog_vendor.is_installed);
    REQUIRE(dialog_vendor.best != nullptr);
    CHECK(dialog_vendor.best->config_version.to_string() == "1.0.0.0");
    CHECK_FALSE(dialog_vendor.best->local_file.empty());

    std::optional<Slic3r::UpdaterError> install_result;
    updater.install_vendor(vendor_id, *dialog_vendor.best,
                           [&install_result](Slic3r::UpdaterError error) {
                               install_result = std::move(error);
                           });

    REQUIRE(install_result.has_value());
    CHECK(install_result->succeeded());
    CHECK(updater.count_installed() == 1);
    const boost::filesystem::path installed_file = data_directory / "vendor" / (vendor_id + ".ini");
    REQUIRE(boost::filesystem::is_regular_file(installed_file));
    CHECK(Slic3r::VendorProfile::from_ini(installed_file, true).config_version.to_string() == "1.0.0.0");
    CHECK(http.sync_request_count() == 0);

    REQUIRE(host.prepared_changes.size() == 1);
    CHECK(host.prepared_changes.front().change == Slic3r::VendorChange::Install);
    CHECK(host.prepared_changes.front().vendor_ids == std::vector<std::string>{vendor_id});
    REQUIRE(host.completed_changes.size() == 1);
    CHECK(host.completed_changes.front().change == Slic3r::VendorChange::Install);
}

TEST_CASE_METHOD(PresetUpdaterFunctionalFixture,
                 "PresetUpdater caches a local vendor archive and exposes it to the dialog model",
                 "[plugins][updater][preset-functional]")
{
    const std::string config_version = "1.5.0.0";
    const boost::filesystem::path archive_path = temporary.path() /
        (vendor_id + "_" + config_version + "_" + slicer_version + ".zip");
    REQUIRE(write_test_zip(
        archive_path,
        {{"description.ini", "[vendor]\nid = " + vendor_id +
                             "\nname = Functional vendor\nfull_name = Functional vendor\n"
                             "config_update_rest = example/vendor\n"},
         {"profiles/" + vendor_id + ".ini",
          vendor_profile_contents(vendor_id, config_version, slicer_version)}}));

    // The cache operation validates and publishes the package. The GUI adapter
    // then reloads the model before asking UpdateConfigDialog to rebuild.
    const Slic3r::UpdaterError import_result = updater.cache_vendor_archive(archive_path);
    INFO("Updater error code: " << static_cast<int>(import_result.code));
    INFO("Updater error detail: " << import_result.detail);
    REQUIRE(import_result.succeeded());

    const boost::filesystem::path package_root = Slic3r::repository_package_cache_path(
        data_directory, Slic3r::RepositoryPackageType::Vendor, vendor_id,
        config_version, slicer_version);
    const boost::filesystem::path cached_profile = package_root / "profiles" / (vendor_id + ".ini");
    REQUIRE(boost::filesystem::is_regular_file(cached_profile));
    CHECK(package_root.filename() == config_version + "=" + slicer_version);
    CHECK_FALSE(boost::filesystem::exists(
        data_directory / "cache" / "vendor" / archive_path.stem()));

    updater.reload_all_vendors();
    const Slic3r::VendorSync *vendor = updater.get_vendor(vendor_id);
    REQUIRE(vendor != nullptr);
    CHECK_FALSE(vendor->is_installed);
    REQUIRE(vendor->best != nullptr);
    CHECK(vendor->best->config_version.to_string() == config_version);
    CHECK(vendor->best->slicer_version.to_string() == slicer_version);
    CHECK(boost::filesystem::equivalent(vendor->best->local_file, cached_profile));
    CHECK(updater.count_available() == 1);
}

TEST_CASE_METHOD(PresetUpdaterFunctionalFixture,
                 "PresetUpdater replaces a vendor INI loaded twice from the dialog",
                 "[plugins][updater][preset-functional]")
{
    const boost::filesystem::path local_profile = temporary.path() / (vendor_id + ".ini");
    write_test_file(local_profile, vendor_profile_contents(vendor_id, "1.0.0.0", slicer_version));

    const Slic3r::UpdaterError first_import = updater.cache_vendor_ini(local_profile);
    INFO("First import error: " << first_import.detail);
    REQUIRE(first_import.succeeded());

    // Loading the same vendor again must overwrite its cached INI. Changing
    // the source version proves that the second call did not merely ignore it.
    write_test_file(local_profile, vendor_profile_contents(vendor_id, "2.0.0.0", slicer_version));
    const Slic3r::UpdaterError second_import = updater.cache_vendor_ini(local_profile);
    INFO("Second import error: " << second_import.detail);
    REQUIRE(second_import.succeeded());

    const boost::filesystem::path cached_profile = Slic3r::repository_package_cache_path(
        data_directory, Slic3r::RepositoryPackageType::Vendor, vendor_id,
        "2.0.0.0", slicer_version) / "profiles" / (vendor_id + ".ini");
    REQUIRE(boost::filesystem::is_regular_file(cached_profile));
    CHECK(Slic3r::VendorProfile::from_ini(cached_profile, true).config_version.to_string() == "2.0.0.0");

    updater.reload_all_vendors();
    const Slic3r::VendorSync *vendor = updater.get_vendor(vendor_id);
    REQUIRE(vendor != nullptr);
    REQUIRE(vendor->best != nullptr);
    CHECK(vendor->best->config_version.to_string() == "2.0.0.0");
    CHECK(vendor->available_profiles.size() == 2);
}

TEST_CASE_METHOD(PresetUpdaterFunctionalFixture,
                 "PresetUpdater functional dialog changes an installed vendor to a selected local version",
                 "[plugins][updater][preset-functional]")
{
    write_installed_vendor("2.0.0.0");
    write_resource_vendor("1.5.0.0");
    updater.reload_all_vendors();

    const PresetDialogSnapshot dialog = synchronize_and_open_dialog({
        {"2.0.0.0", slicer_version, "https://example.invalid/vendor-2.zip"},
        {"1.5.0.0", slicer_version, "https://example.invalid/vendor-15.zip"}
    });
    REQUIRE(dialog.vendors.size() == 1);
    CHECK(dialog.profile_count_to_update == 0);
    const Slic3r::VendorSync &dialog_vendor = dialog.vendors.front();
    CHECK(dialog_vendor.is_installed);

    // The detailed selector may choose any compatible entry, not only best.
    // Choosing the bundled older version exercises a real version replacement
    // without involving the separate archive-download path.
    const std::vector<Slic3r::VendorAvailable>::const_iterator selected = std::find_if(
        dialog_vendor.available_profiles.begin(), dialog_vendor.available_profiles.end(),
        [](const Slic3r::VendorAvailable &version) {
            return version.config_version.to_string() == "1.5.0.0";
        });
    REQUIRE(selected != dialog_vendor.available_profiles.end());
    CHECK_FALSE(selected->local_file.empty());

    std::optional<Slic3r::UpdaterError> change_result;
    updater.install_vendor(vendor_id, *selected,
                           [&change_result](Slic3r::UpdaterError error) {
                               change_result = std::move(error);
                           });

    REQUIRE(change_result.has_value());
    CHECK(change_result->succeeded());
    const boost::filesystem::path installed_file = data_directory / "vendor" / (vendor_id + ".ini");
    REQUIRE(boost::filesystem::is_regular_file(installed_file));
    CHECK(Slic3r::VendorProfile::from_ini(installed_file, true).config_version.to_string() == "1.5.0.0");
    CHECK(http.sync_request_count() == 0);
    REQUIRE(host.completed_changes.size() == 1);
    CHECK(host.completed_changes.front().change == Slic3r::VendorChange::Install);
}

TEST_CASE_METHOD(PresetUpdaterFunctionalFixture,
                 "PresetUpdater functional dialog removes an installed vendor",
                 "[plugins][updater][preset-functional]")
{
    write_installed_vendor("1.0.0.0");
    updater.reload_all_vendors();

    const PresetDialogSnapshot dialog = synchronize_and_open_dialog({
        {"1.0.0.0", slicer_version, "https://example.invalid/vendor-1.zip"}
    });
    REQUIRE(dialog.vendors.size() == 1);
    CHECK(dialog.vendors.front().is_installed);

    std::optional<Slic3r::UpdaterError> uninstall_result;
    updater.uninstall_vendor(vendor_id,
                             [&uninstall_result](Slic3r::UpdaterError error) {
                                 uninstall_result = std::move(error);
                             });

    REQUIRE(uninstall_result.has_value());
    CHECK(uninstall_result->succeeded());
    CHECK(updater.count_installed() == 0);
    CHECK_FALSE(boost::filesystem::exists(data_directory / "vendor" / (vendor_id + ".ini")));
    CHECK(boost::filesystem::is_regular_file(Slic3r::repository_package_cache_path(
        data_directory, Slic3r::RepositoryPackageType::Vendor, vendor_id,
        "1.0.0.0", slicer_version) / "profiles" / (vendor_id + ".ini")));

    const Slic3r::VendorSync *remaining_vendor = updater.get_vendor(vendor_id);
    REQUIRE(remaining_vendor != nullptr);
    CHECK_FALSE(remaining_vendor->is_installed);
    CHECK(remaining_vendor->has_cache);
    REQUIRE(host.prepared_changes.size() == 1);
    CHECK(host.prepared_changes.front().change == Slic3r::VendorChange::Uninstall);
    REQUIRE(host.completed_changes.size() == 1);
    CHECK(host.completed_changes.front().change == Slic3r::VendorChange::Uninstall);
}

TEST_CASE_METHOD(PresetUpdaterFunctionalFixture,
                 "PresetUpdater functional dialog downloads and installs a newer remote vendor",
                 "[plugins][updater][preset-functional]")
{
    write_installed_vendor("1.0.0.0");
    updater.reload_all_vendors();

    const std::string remote_archive_url = "https://example.invalid/vendor-2.zip";
    const PresetDialogSnapshot dialog = synchronize_and_open_dialog({
        {"2.0.0.0", slicer_version, remote_archive_url},
        {"1.0.0.0", slicer_version, "https://example.invalid/vendor-1.zip"}
    });
    REQUIRE(dialog.vendors.size() == 1);
    CHECK(dialog.profile_count_to_update == 1);
    const Slic3r::VendorSync &dialog_vendor = dialog.vendors.front();
    CHECK(dialog_vendor.is_installed);
    CHECK(dialog_vendor.can_upgrade);
    REQUIRE(dialog_vendor.best != nullptr);
    CHECK(dialog_vendor.best->config_version.to_string() == "2.0.0.0");
    CHECK(dialog_vendor.best->local_file.empty());

    // The remote branch receives the archive through perform_sync(), extracts
    // its profiles directory, then atomically publishes the selected INI into
    // the normal installed-vendor directory.
    const boost::filesystem::path archive_file = temporary.path() / "vendor-2.zip";
    REQUIRE(write_test_zip(
        archive_file,
        {{"profiles/" + vendor_id + ".ini",
          vendor_profile_contents(vendor_id, "2.0.0.0", slicer_version)}}));
    http.script_sync_success(read_test_file(archive_file));

    std::optional<Slic3r::UpdaterError> upgrade_result;
    updater.install_vendor(vendor_id, *dialog_vendor.best,
                           [&upgrade_result](Slic3r::UpdaterError error) {
                               upgrade_result = std::move(error);
                           });

    REQUIRE(upgrade_result.has_value());
    INFO("Updater error code: " << static_cast<int>(upgrade_result->code));
    INFO("Updater error detail: " << upgrade_result->detail);
    CHECK(upgrade_result->succeeded());
    REQUIRE(http.sync_request_count() == 1);
    CHECK(http.sync_request_url(0) == remote_archive_url);
    const boost::filesystem::path installed_file = data_directory / "vendor" / (vendor_id + ".ini");
    REQUIRE(boost::filesystem::is_regular_file(installed_file));
    CHECK(Slic3r::VendorProfile::from_ini(installed_file, true).config_version.to_string() == "2.0.0.0");

    const Slic3r::VendorSync *updated_vendor = updater.get_vendor(vendor_id);
    REQUIRE(updated_vendor != nullptr);
    CHECK(updated_vendor->is_installed);
    CHECK_FALSE(updated_vendor->can_upgrade);
    REQUIRE(host.completed_changes.size() == 1);
    CHECK(host.completed_changes.front().change == Slic3r::VendorChange::Install);
}

TEST_CASE_METHOD(PresetUpdaterFunctionalFixture,
                 "PresetUpdater functional version dialog installs a newer vendor after loading changelogs",
                 "[plugins][updater][preset-functional]")
{
    write_installed_vendor("1.0.0.0");
    updater.reload_all_vendors();

    const std::string remote_archive_url = "https://example.invalid/vendor-2-with-logs.zip";
    const PresetDialogSnapshot main_dialog = synchronize_and_open_dialog({
        {"2.0.0.0", slicer_version, remote_archive_url},
        {"1.0.0.0", slicer_version, "https://example.invalid/vendor-1.zip"}
    });
    REQUIRE(main_dialog.vendors.size() == 1);
    CHECK(main_dialog.profile_count_to_update == 1);
    REQUIRE(main_dialog.vendors.front().best != nullptr);
    CHECK(main_dialog.vendors.front().best->config_version.to_string() == "2.0.0.0");

    // Clicking the installed-version button downloads all notes before the
    // detailed selector is created. The recent version uses a GitHub compare;
    // the first published version has no predecessor and uses its commit URL.
    std::optional<bool> changelogs_succeeded;
    int changelog_callback_count = 0;
    updater.download_changelogs(
        vendor_id,
        [&changelogs_succeeded, &changelog_callback_count](bool succeeded) {
            changelogs_succeeded = succeeded;
            ++changelog_callback_count;
        });

    REQUIRE(http.pending_count() == 2);
    CHECK(http.pending_at(0).url() ==
          "https://api.github.com/repos/example/vendor/compare/"
          "1.0.0.0=2.7.0.0...2.0.0.0=2.7.0.0");
    CHECK(http.pending_at(1).url() ==
          "https://api.github.com/repos/example/vendor/commits/1.0.0.0");
    http.succeed_front(
        R"({"commits":[{"commit":{"message":"prepare upgrade"}},{"commit":{"message":"add new profiles"}}]})",
        200);
    CHECK_FALSE(changelogs_succeeded.has_value());
    http.succeed_front(R"({"commit":{"message":"initial profiles"}})", 200);

    REQUIRE(changelogs_succeeded.has_value());
    CHECK(*changelogs_succeeded);
    CHECK(changelog_callback_count == 1);
    CHECK(http.pending_count() == 0);

    // ChooseVendorVersionDialog copies the now-enriched internal VendorSync.
    // sort_available() is required after that copy so best points into the
    // copied available_profiles vector rather than into the updater's model.
    const Slic3r::VendorSync *enriched_vendor = updater.get_vendor(vendor_id);
    REQUIRE(enriched_vendor != nullptr);
    Slic3r::VendorSync version_dialog = *enriched_vendor;
    version_dialog.sort_available();
    const std::vector<Slic3r::VendorAvailable>::const_iterator selected = std::find_if(
        version_dialog.available_profiles.begin(), version_dialog.available_profiles.end(),
        [](const Slic3r::VendorAvailable &version) {
            return version.config_version.to_string() == "2.0.0.0";
        });
    REQUIRE(selected != version_dialog.available_profiles.end());
    CHECK(selected->notes == "add new profiles\nprepare upgrade");
    CHECK(selected->url_zip == remote_archive_url);
    CHECK(selected->local_file.empty());

    const std::vector<Slic3r::VendorAvailable>::const_iterator installed_version = std::find_if(
        version_dialog.available_profiles.begin(), version_dialog.available_profiles.end(),
        [](const Slic3r::VendorAvailable &version) {
            return version.config_version.to_string() == "1.0.0.0";
        });
    REQUIRE(installed_version != version_dialog.available_profiles.end());
    CHECK(installed_version->notes == "initial profiles");

    // Selecting the changelog row starts the normal remote installation path.
    // The archive response therefore uses the synchronous side of the same
    // fake transport that supplied the asynchronous changelog responses.
    const boost::filesystem::path archive_file = temporary.path() / "vendor-2-with-logs.zip";
    REQUIRE(write_test_zip(
        archive_file,
        {{"profiles/" + vendor_id + ".ini",
          vendor_profile_contents(vendor_id, "2.0.0.0", slicer_version)}}));
    http.script_sync_success(read_test_file(archive_file));

    std::optional<Slic3r::UpdaterError> install_result;
    updater.install_vendor(vendor_id, *selected,
                           [&install_result](Slic3r::UpdaterError error) {
                               install_result = std::move(error);
                           });

    REQUIRE(install_result.has_value());
    INFO("Updater error code: " << static_cast<int>(install_result->code));
    INFO("Updater error detail: " << install_result->detail);
    CHECK(install_result->succeeded());
    REQUIRE(http.sync_request_count() == 1);
    CHECK(http.sync_request_url(0) == remote_archive_url);

    const boost::filesystem::path installed_file = data_directory / "vendor" / (vendor_id + ".ini");
    REQUIRE(boost::filesystem::is_regular_file(installed_file));
    CHECK(Slic3r::VendorProfile::from_ini(installed_file, true).config_version.to_string() == "2.0.0.0");
    const Slic3r::VendorSync *updated_vendor = updater.get_vendor(vendor_id);
    REQUIRE(updated_vendor != nullptr);
    CHECK(updated_vendor->is_installed);
    CHECK_FALSE(updated_vendor->can_upgrade);

    REQUIRE(host.prepared_changes.size() == 1);
    CHECK(host.prepared_changes.front().change == Slic3r::VendorChange::Install);
    REQUIRE(host.completed_changes.size() == 1);
    CHECK(host.completed_changes.front().change == Slic3r::VendorChange::Install);
}

TEST_CASE("PresetUpdater processes a controlled successful HTTP response", "[plugins][updater]")
{
    FakeUpdaterHttpTransport http;
    Slic3r::PresetUpdater updater(nullptr, http);
    std::optional<Slic3r::UpdaterError> result;

    updater.download_new_repo("https://api.github.com/repos/example/vendor",
                              [&result](Slic3r::UpdaterError error) { result = std::move(error); });

    REQUIRE(http.pending_count() == 1);
    CHECK(http.pending_front().url() ==
          "https://raw.githubusercontent.com/example/vendor/refs/heads/main/description.ini");
    CHECK_FALSE(result.has_value());

    // A successful transport response still has to pass the repository parser.
    // This body is intentionally malformed so the test remains filesystem-free.
    http.succeed_front("not a repository description", 200);

    REQUIRE(result.has_value());
    CHECK(result->code == Slic3r::UpdaterError::Code::InvalidArchive);
    CHECK(http.pending_count() == 0);
}
