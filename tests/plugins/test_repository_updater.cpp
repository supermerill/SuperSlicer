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
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <deque>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/json_parser.hpp>

#include "libslic3r/ContainerUtils.hpp"
#include "libslic3r/FilesystemTransactionTest.hpp"
#include "libslic3r/Plugins/PluginRepository.hpp"
#include "libslic3r/Updater/PluginUpdater.hpp"
#include "libslic3r/Updater/PresetUpdater.hpp"
#include "libslic3r/Updater/RepositoryPackageCache.hpp"
#include "libslic3r/Updater/RepositoryCacheIO.hpp"
#include "libslic3r/Updater/UpdaterHttp.hpp"
#include "libslic3r/Updater/UpdaterError.hpp"
#include "libslic3r/Updater/UpdaterOperationExecutor.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/miniz_extension.hpp"

namespace {

#ifdef SLIC3R_FILESYSTEM_TRANSACTION_TESTING
class ScopedFilesystemTransactionHook {
public:
    explicit ScopedFilesystemTransactionHook(Slic3r::FilesystemTransactionTestHook hook)
    {
        Slic3r::set_filesystem_transaction_test_hook(std::move(hook));
    }

    ~ScopedFilesystemTransactionHook()
    {
        Slic3r::set_filesystem_transaction_test_hook({});
    }
};
#endif

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
    long sync_request_timeout(size_t idx) const { return m_sync_request_timeouts.at(idx); }

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
        m_sync_request_timeouts.emplace_back(request.total_timeout_seconds());
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
    std::vector<long> m_sync_request_timeouts;
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
    void prepare_vendor_change_async(
        Slic3r::VendorChange change,
        const std::vector<std::string> &vendor_ids,
        Slic3r::PresetUpdaterHost::PrepareCallback callback) override
    {
        std::function<void()> operation =
            [this, change, vendor_ids, callback = std::move(callback)]() mutable {
                prepared_changes.push_back({change, vendor_ids});
                if (!accept_changes) {
                    callback(Slic3r::make_updater_error(
                                 Slic3r::UpdaterError::Code::PreparationRejected),
                             std::string());
                    return;
                }

                capture_vendor_directory();
                callback(Slic3r::UpdaterError(),
                         "test-snapshot-" + std::to_string(prepared_changes.size()));
            };
        if (dispatch_immediately)
            operation();
        else
            dispatched_changes.emplace_back(std::move(operation));
    }

    void rollback_vendor_change_async(
        const std::string &token,
        Slic3r::PresetUpdaterHost::RollbackCallback callback) override
    {
        rollback_tokens.emplace_back(token);
        if (!rollback_succeeds) {
            callback(Slic3r::make_updater_error(Slic3r::UpdaterError::Code::Filesystem,
                                                "The fake snapshot restore failed."));
            return;
        }

        const boost::filesystem::path vendor_directory =
            boost::filesystem::path(Slic3r::data_dir()) / "vendor";
        boost::filesystem::remove_all(vendor_directory);
        if (!snapshot_vendor_existed) {
            callback(Slic3r::UpdaterError());
            return;
        }

        boost::filesystem::create_directories(vendor_directory);
        for (const std::string &directory : snapshot_directories)
            boost::filesystem::create_directories(vendor_directory / directory);
        for (const std::pair<const std::string, std::string> &file : snapshot_files) {
            const boost::filesystem::path destination = vendor_directory / file.first;
            boost::filesystem::create_directories(destination.parent_path());
            boost::nowide::ofstream stream(destination.string(), std::ios::binary | std::ios::trunc);
            stream.write(file.second.data(), static_cast<std::streamsize>(file.second.size()));
        }
        callback(Slic3r::UpdaterError());
    }

    void run_next_dispatched_change()
    {
        REQUIRE_FALSE(dispatched_changes.empty());
        std::function<void()> operation = std::move(dispatched_changes.front());
        dispatched_changes.pop_front();
        operation();
    }

    void vendor_files_changed(Slic3r::PresetUpdater &,
                              Slic3r::VendorChange change,
                              const std::vector<std::string> &vendor_ids) override
    {
        completed_changes.push_back({change, vendor_ids});
    }

    bool accept_changes = true;
    bool rollback_succeeds = true;
    bool dispatch_immediately = true;
    std::vector<VendorChangeCall> prepared_changes;
    std::vector<VendorChangeCall> completed_changes;
    std::vector<std::string> rollback_tokens;
    std::deque<std::function<void()>> dispatched_changes;

private:
    // Keep a small in-memory equivalent of the GUI configuration snapshot so
    // functional tests can verify physical rollback without linking wxWidgets.
    void capture_vendor_directory()
    {
        snapshot_files.clear();
        snapshot_directories.clear();
        const boost::filesystem::path vendor_directory =
            boost::filesystem::path(Slic3r::data_dir()) / "vendor";
        snapshot_vendor_existed = boost::filesystem::is_directory(vendor_directory);
        if (!snapshot_vendor_existed)
            return;

        for (boost::filesystem::recursive_directory_iterator it(vendor_directory), end; it != end; ++it) {
            const std::string relative = it->path().lexically_relative(vendor_directory).generic_string();
            if (boost::filesystem::is_directory(it->status())) {
                snapshot_directories.emplace(relative);
            } else if (boost::filesystem::is_regular_file(it->status())) {
                boost::nowide::ifstream stream(it->path().string(), std::ios::binary);
                snapshot_files.emplace(relative, std::string(std::istreambuf_iterator<char>(stream),
                                                             std::istreambuf_iterator<char>()));
            }
        }
    }

    bool snapshot_vendor_existed = false;
    std::map<std::string, std::string> snapshot_files;
    std::set<std::string> snapshot_directories;
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

    ~TestRepositoryUpdater() override
    {
        // Real derived updaters drain callback chains before their model
        // members disappear. Keep the focused test facade under that contract.
        shutdown_operation_executor();
    }

    using RepositoryUpdater::begin_sync;
    using RepositoryUpdater::begin_repository_change;
    using RepositoryUpdater::changelog_download_in_progress;
    using RepositoryUpdater::download_repository_changelogs;
    using RepositoryUpdater::download_repository_description;
    using RepositoryUpdater::download_repository_file_async;
    using RepositoryUpdater::download_repository_file_sync;
    using RepositoryUpdater::enqueue_operation;
    using RepositoryUpdater::finish_sync;
    using RepositoryUpdater::finish_repository_change;
    using RepositoryUpdater::has_api_request_slot;
    using RepositoryUpdater::refresh_repository_tags;
    using RepositoryUpdater::RepositoryChangelogKind;
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
std::string paginated_repository_tags(size_t first_index, size_t count,
                                      const std::string &prefix = "release-");
std::vector<std::string> repository_tag_names(const std::string &json);
void write_tag_pagination(const boost::filesystem::path &cache_file,
                          uint32_t next_page,
                          bool history_complete,
                          std::time_t last_request);
boost::property_tree::ptree read_tag_pagination(const boost::filesystem::path &cache_file);

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
    ~PluginUpdaterFunctionalFixture();

    void write_plugin_repository();
    void write_bundled_plugin(const std::string &package_version);
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

std::string paginated_repository_tags(size_t first_index, size_t count, const std::string &prefix)
{
    std::ostringstream json;
    json << '[';
    for (size_t offset = 0; offset < count; ++offset) {
        if (offset != 0)
            json << ',';
        json << "{\"name\":\"" << prefix << first_index + offset << "\","
             << "\"zipball_url\":\"archive-" << first_index + offset << "\","
             << "\"commit\":{\"sha\":\"sha-" << first_index + offset << "\"}}";
    }
    json << ']';
    return json.str();
}

std::vector<std::string> repository_tag_names(const std::string &json)
{
    boost::property_tree::ptree root;
    std::stringstream stream(json);
    boost::property_tree::read_json(stream, root);
    std::vector<std::string> names;
    for (const boost::property_tree::ptree::value_type &entry : root)
        names.emplace_back(entry.second.get<std::string>("name"));
    return names;
}

void write_tag_pagination(const boost::filesystem::path &cache_file,
                          uint32_t next_page,
                          bool history_complete,
                          std::time_t last_request)
{
    boost::property_tree::ptree root;
    root.put("pagination.next_page", next_page);
    root.put("pagination.history_complete", history_complete);
    root.put("pagination.last_request", static_cast<int64_t>(last_request));
    const boost::filesystem::path path =
        cache_file.parent_path() / (cache_file.stem().string() + ".pagination.ini");
    boost::filesystem::create_directories(path.parent_path());
    boost::property_tree::write_ini(path.string(), root);
}

boost::property_tree::ptree read_tag_pagination(const boost::filesystem::path &cache_file)
{
    boost::property_tree::ptree root;
    const boost::filesystem::path path =
        cache_file.parent_path() / (cache_file.stem().string() + ".pagination.ini");
    boost::property_tree::read_ini(path.string(), root);
    return root;
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
          "https://api.github.com/repos/example/vendor/tags?per_page=100&page=1");
    http.succeed_front(vendor_repository_tags(versions), 200);

    REQUIRE(update_count.has_value());
    CHECK(updater.is_synchronized());
    const std::optional<Slic3r::VendorSync> synchronized_vendor = updater.vendor(vendor_id);
    REQUIRE(synchronized_vendor.has_value());
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
    Slic3r::Orchestrator::instance().clear_plugin_package_load_reports();
    write_test_file(resources_directory / "plugins" / "default_activated.ini",
                    "[installed]\n\n[activated]\n");
    Slic3r::RepositoryPackageCache cache(data_directory, Slic3r::plugin_repository_cache_adapter());
    bool purged = false;
    std::string error_message;
    REQUIRE(cache.prepare_layout(purged, error_message));
}

PluginUpdaterFunctionalFixture::~PluginUpdaterFunctionalFixture()
{
    Slic3r::Orchestrator::instance().clear_plugin_package_load_reports();
}

void PluginUpdaterFunctionalFixture::write_plugin_repository()
{
    save_test_plugin_repository(data_directory, plugin_id, "example/plugin");
}

void PluginUpdaterFunctionalFixture::write_bundled_plugin(const std::string &package_version)
{
    const boost::filesystem::path archive = resources_directory / "plugins" /
        (plugin_id + "_" + package_version + "_" + slicer_version + ".zip");
    REQUIRE(write_test_zip(
        archive,
        {{"description.ini", plugin_description_contents(plugin_id, package_version, slicer_version, false)},
         {"version.ini", "[plugin]\npackage_version=" + package_version + "\nslicer_version=" + slicer_version +
             "\n[abi]\nslic3r_plugin_types.h=1.0\n"},
         {plugin_library_filename(), "bundled library " + package_version}}));
}

void PluginUpdaterFunctionalFixture::write_installed_plugin(const std::string &package_version)
{
    const boost::filesystem::path package_root = data_directory / "plugins" / plugin_id;
    write_test_file(package_root / "description.ini",
                    plugin_description_contents(plugin_id, package_version, slicer_version, false));
    write_test_file(package_root / "version.ini",
                    "[plugin]\npackage_version = " + package_version +
                    "\nslicer_version = " + slicer_version + "\n[abi]\nslic3r_plugin_types.h = 1.0\n");
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
                    "\nslicer_version = " + slicer_version + "\n[abi]\nslic3r_plugin_types.h = 1.0\n");
    write_test_file(package_root / plugin_library_filename(), "cached library " + package_version);
    return package_root;
}

std::string PluginUpdaterFunctionalFixture::make_plugin_archive(const std::string &package_version)
{
    const boost::filesystem::path archive_path = temporary.path() / (package_version + ".zip");
    REQUIRE(write_test_zip(
        archive_path,
        {{"description.ini", plugin_description_contents(plugin_id, package_version, slicer_version, false)},
         {"version.ini", "[plugin]\npackage_version=" + package_version + "\nslicer_version=" + slicer_version +
             "\n[abi]\nslic3r_plugin_types.h=1.0\n"},
         {plugin_library_filename(), "downloaded library " + package_version}}));
    return read_test_file(archive_path);
}

void PluginUpdaterFunctionalFixture::synchronize(const std::vector<TestPluginVersion> &versions)
{
    std::optional<int> update_count;
    updater.sync_async([&update_count](int count) { update_count = count; }, true);
    REQUIRE(http.pending_count() == 1);
    CHECK(http.pending_front().url() ==
          "https://api.github.com/repos/example/plugin/tags?per_page=100&page=1");
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

TEST_CASE("RepositoryUpdater mutation gates are independent per updater", "[plugins][updater]")
{
    FakeUpdaterHttpTransport first_http;
    FakeUpdaterHttpTransport second_http;
    TestRepositoryUpdater first(first_http);
    TestRepositoryUpdater second(second_http);

    // One updater rejects a second logical mutation while another updater has
    // its own independent reservation and may proceed concurrently.
    CHECK(first.begin_repository_change());
    CHECK(first.repository_change_in_progress());
    CHECK_FALSE(first.begin_repository_change());
    CHECK(second.begin_repository_change());
    CHECK(second.repository_change_in_progress());

    first.finish_repository_change();
    CHECK_FALSE(first.repository_change_in_progress());
    CHECK(first.begin_repository_change());
    first.finish_repository_change();
    second.finish_repository_change();
}

#ifdef SLIC3R_FILESYSTEM_TRANSACTION_TESTING
TEST_CASE("Repository metadata batch restores tags and pagination together",
          "[plugins][updater][repository-cache]")
{
    TemporaryDirectory temporary;
    const boost::filesystem::path tags = temporary.path() / "tags.json";
    const boost::filesystem::path pagination = temporary.path() / "tags.pagination.ini";
    write_test_file(tags, "old tags");
    write_test_file(pagination, "old pagination");

    Slic3r::UpdaterError result;
    {
        ScopedFilesystemTransactionHook hook(
            [pagination](Slic3r::FilesystemTransactionTestPoint point,
                         const boost::filesystem::path &,
                         const boost::filesystem::path &destination) {
                if (point == Slic3r::FilesystemTransactionTestPoint::BeforePublishStaging &&
                    destination == pagination)
                    throw std::runtime_error("injected pagination publication failure");
            });
        result = Slic3r::RepositoryUpdaterInternal::publish_repository_caches_atomically({
            {tags, "new tags"}, {pagination, "new pagination"}});
    }

    CHECK_FALSE(result.succeeded());
    CHECK(result.detail.find("injected pagination publication failure") != std::string::npos);
    CHECK(read_test_file(tags) == "old tags");
    CHECK(read_test_file(pagination) == "old pagination");
    for (boost::filesystem::directory_iterator it(temporary.path()), end; it != end; ++it) {
        const std::string filename = it->path().filename().string();
        CHECK(filename.find(".download-") == std::string::npos);
        CHECK(filename.find(".transaction-backup-") == std::string::npos);
    }
}
#endif

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
        const std::string cached_tags = paginated_repository_tags(0, 1);
        write_test_file(cache_file, cached_tags);

        REQUIRE(updater.begin_sync(1, [&sync_callback_count](int count) {
            CHECK(count == 7);
            ++sync_callback_count;
        }));
        updater.refresh_repository_tags(
            "cached", "https://api.github.com/repos/example/cached", cache_file, false,
            [&parsed_contents](const std::string &contents) {
                parsed_contents = contents;
                return Slic3r::UpdaterError();
            },
            [&refresh_result](Slic3r::UpdaterError error) { refresh_result = std::move(error); });

        CHECK(http.pending_count() == 0);
        CHECK(parsed_contents == cached_tags);
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
            "invalid", "https://api.github.com/repos/example/invalid", cache_file, false,
            [](const std::string &) {
                return Slic3r::make_updater_error(Slic3r::UpdaterError::Code::InvalidRepositoryMetadata,
                                                  "invalid tags");
            },
            [&refresh_result](Slic3r::UpdaterError error) { refresh_result = std::move(error); });

        REQUIRE(refresh_result.has_value());
        CHECK(refresh_result->code == Slic3r::UpdaterError::Code::InvalidRepositoryMetadata);
        CHECK_FALSE(refresh_result->detail.empty());
        CHECK(sync_callback_count == 1);
    }

    SECTION("force downloads and replaces the cache") {
        write_test_file(cache_file, paginated_repository_tags(0, 1, "old-"));

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
              "https://example.invalid/forced/tags?per_page=100&page=1");
        CHECK(http.pending_front().response_size_limit() == 64 * 1024);
        CHECK(http.pending_front().total_timeout_seconds() == 60);
        CHECK(sync_callback_count == 0);

        http.succeed_front(paginated_repository_tags(0, 1, "fresh-"), 200);

        CHECK(repository_tag_names(parsed_contents) == std::vector<std::string>{"fresh-0"});
        CHECK(read_test_file(cache_file) == parsed_contents);
        REQUIRE(refresh_result.has_value());
        CHECK(refresh_result->succeeded());
        CHECK(sync_callback_count == 1);

        bool temporary_file_found = false;
        for (boost::filesystem::directory_iterator it(cache_file.parent_path()), end; it != end; ++it) {
            const std::string filename = it->path().filename().string();
            temporary_file_found = temporary_file_found ||
                filename.find(".download-") != std::string::npos ||
                filename.find(".previous-") != std::string::npos;
        }
        CHECK_FALSE(temporary_file_found);
    }

    SECTION("an invalid download preserves and reuses the previous cache") {
        const std::string cached_tags = paginated_repository_tags(0, 1, "cached-");
        write_test_file(cache_file, cached_tags);
        const std::function<Slic3r::UpdaterError(const std::string &)> parse =
            [&parsed_contents, &cached_tags](const std::string &contents) {
                if (contents != cached_tags)
                    return Slic3r::make_updater_error(
                        Slic3r::UpdaterError::Code::InvalidRepositoryMetadata, "invalid downloaded tags");
                parsed_contents = contents;
                return Slic3r::UpdaterError();
            };

        REQUIRE(updater.begin_sync(1, [&sync_callback_count](int) { ++sync_callback_count; }));
        updater.refresh_repository_tags(
            "invalid", "https://api.github.com/repos/example/invalid", cache_file, true, parse,
            [&refresh_result](Slic3r::UpdaterError error) { refresh_result = std::move(error); });
        REQUIRE(http.pending_count() == 1);
        http.succeed_front("not JSON", 200);

        REQUIRE(refresh_result.has_value());
        CHECK(refresh_result->code == Slic3r::UpdaterError::Code::InvalidRepositoryMetadata);
        CHECK(read_test_file(cache_file) == cached_tags);
        CHECK(sync_callback_count == 1);

        // The failed body did not replace the aggregate, but the persisted
        // attempt prevents another normal GitHub request during this window.
        refresh_result.reset();
        REQUIRE(updater.begin_sync(1, [&sync_callback_count](int) { ++sync_callback_count; }));
        updater.refresh_repository_tags(
            "cached", "https://api.github.com/repos/example/invalid", cache_file, false, parse,
            [&refresh_result](Slic3r::UpdaterError error) { refresh_result = std::move(error); });
        CHECK(http.pending_count() == 0);
        REQUIRE(refresh_result.has_value());
        CHECK(refresh_result->succeeded());
        CHECK(parsed_contents == cached_tags);
        CHECK(sync_callback_count == 2);
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

TEST_CASE("RepositoryUpdater advances GitHub tag pagination once per day", "[plugins][updater]")
{
    FakeUpdaterHttpTransport http;
    TestRepositoryUpdater updater(http);
    TemporaryDirectory temporary;
    const boost::filesystem::path cache_file = temporary.path() / "tags.json";
    std::optional<Slic3r::UpdaterError> result;
    std::vector<std::string> parsed_names;
    const std::function<Slic3r::UpdaterError(const std::string &)> parse =
        [&parsed_names](const std::string &contents) {
            parsed_names = repository_tag_names(contents);
            return Slic3r::UpdaterError();
        };
    const std::function<void(Slic3r::UpdaterError)> finish =
        [&result](Slic3r::UpdaterError error) { result = std::move(error); };

    // The first page fills the GitHub page size, so the next daily request is
    // scheduled for page two and the legacy-compatible aggregate is cached.
    REQUIRE(updater.begin_sync(1, [](int) {}));
    updater.refresh_repository_tags("daily", "example/daily", cache_file, false, parse, finish);
    REQUIRE(http.pending_count() == 1);
    CHECK(http.pending_front().url() ==
          "https://api.github.com/repos/example/daily/tags?per_page=100&page=1");
    http.succeed_front(paginated_repository_tags(0, 100), 200);

    REQUIRE(result.has_value());
    CHECK(result->succeeded());
    REQUIRE(parsed_names.size() == 100);
    boost::property_tree::ptree pagination = read_tag_pagination(cache_file);
    CHECK(pagination.get<uint32_t>("pagination.next_page") == 2);
    CHECK_FALSE(pagination.get<bool>("pagination.history_complete"));
    CHECK(pagination.get<int64_t>("pagination.last_request") > 0);

    // A second normal synchronization inside the 24-hour window consumes the
    // aggregate without creating another HTTP request.
    result.reset();
    parsed_names.clear();
    REQUIRE(updater.begin_sync(1, [](int) {}));
    updater.refresh_repository_tags("daily", "example/daily", cache_file, false, parse, finish);
    CHECK(http.pending_count() == 0);
    REQUIRE(result.has_value());
    INFO(result->detail);
    CHECK(result->succeeded());
    CHECK(parsed_names.size() == 100);

    // Age only the request marker. The next normal synchronization advances
    // to page two and a short response marks the historical crawl complete.
    write_tag_pagination(cache_file, 2, false, 0);
    result.reset();
    REQUIRE(updater.begin_sync(1, [](int) {}));
    updater.refresh_repository_tags("daily", "example/daily", cache_file, false, parse, finish);
    REQUIRE(http.pending_count() == 1);
    CHECK(http.pending_front().url() ==
          "https://api.github.com/repos/example/daily/tags?per_page=100&page=2");
    http.succeed_front(paginated_repository_tags(100, 2), 200);

    REQUIRE(result.has_value());
    CHECK(result->succeeded());
    REQUIRE(parsed_names.size() == 102);
    pagination = read_tag_pagination(cache_file);
    CHECK(pagination.get<uint32_t>("pagination.next_page") == 1);
    CHECK(pagination.get<bool>("pagination.history_complete"));

    // Once complete, page one is merged in front instead of replacing the
    // collected history, so an old release remains selectable.
    write_tag_pagination(cache_file, 1, true, 0);
    result.reset();
    REQUIRE(updater.begin_sync(1, [](int) {}));
    updater.refresh_repository_tags("daily", "example/daily", cache_file, false, parse, finish);
    REQUIRE(http.pending_count() == 1);
    CHECK(http.pending_front().url() ==
          "https://api.github.com/repos/example/daily/tags?per_page=100&page=1");
    http.succeed_front(paginated_repository_tags(0, 1, "new-"), 200);

    REQUIRE(result.has_value());
    CHECK(result->succeeded());
    REQUIRE(parsed_names.size() == 103);
    CHECK(parsed_names.front() == "new-0");
    CHECK(std::find(parsed_names.begin(), parsed_names.end(), "release-101") != parsed_names.end());
}

TEST_CASE("RepositoryUpdater restarts GitHub pagination when its aggregate is missing", "[plugins][updater]")
{
    FakeUpdaterHttpTransport http;
    TestRepositoryUpdater updater(http);
    TemporaryDirectory temporary;
    const boost::filesystem::path cache_file = temporary.path() / "tags.json";
    write_tag_pagination(cache_file, 4, false, 0);

    std::optional<Slic3r::UpdaterError> result;
    std::vector<std::string> parsed_names;
    const std::function<Slic3r::UpdaterError(const std::string &)> parse =
        [&parsed_names](const std::string &contents) {
            parsed_names = repository_tag_names(contents);
            return Slic3r::UpdaterError();
        };

    // A cursor without its aggregate cannot identify already collected tags.
    // The next request therefore rebuilds the history from page one.
    REQUIRE(updater.begin_sync(1, [](int) {}));
    updater.refresh_repository_tags(
        "orphaned", "example/orphaned", cache_file, false, parse,
        [&result](Slic3r::UpdaterError error) { result = std::move(error); });
    REQUIRE(http.pending_count() == 1);
    CHECK(http.pending_front().url() ==
          "https://api.github.com/repos/example/orphaned/tags?per_page=100&page=1");
    http.succeed_front(paginated_repository_tags(0, 1), 200);

    REQUIRE(result.has_value());
    CHECK(result->succeeded());
    CHECK(parsed_names == std::vector<std::string>{"release-0"});
    const boost::property_tree::ptree pagination = read_tag_pagination(cache_file);
    CHECK(pagination.get<uint32_t>("pagination.next_page") == 1);
    CHECK(pagination.get<bool>("pagination.history_complete"));
}

TEST_CASE("RepositoryUpdater refreshes the GitHub front page after historical overlap", "[plugins][updater]")
{
    FakeUpdaterHttpTransport http;
    TestRepositoryUpdater updater(http);
    TemporaryDirectory temporary;
    const boost::filesystem::path cache_file = temporary.path() / "tags.json";
    write_test_file(cache_file, paginated_repository_tags(0, 100));
    write_tag_pagination(cache_file, 2, false, 0);

    std::optional<Slic3r::UpdaterError> result;
    std::vector<std::string> parsed_names;
    const std::function<Slic3r::UpdaterError(const std::string &)> parse =
        [&parsed_names](const std::string &contents) {
            parsed_names = repository_tag_names(contents);
            return Slic3r::UpdaterError();
        };
    const std::function<void(Slic3r::UpdaterError)> finish =
        [&result](Slic3r::UpdaterError error) { result = std::move(error); };

    REQUIRE(updater.begin_sync(1, [](int) {}));
    updater.refresh_repository_tags("overlap", "example/overlap", cache_file, false, parse, finish);
    REQUIRE(http.pending_count() == 1);
    CHECK(http.pending_front().url() ==
          "https://api.github.com/repos/example/overlap/tags?per_page=100&page=2");

    // release-99 appears on both sides of the old page boundary. The updater
    // detects the shift and spends its explicit exception on page one now.
    http.succeed_front(paginated_repository_tags(99, 100), 200);
    REQUIRE(http.pending_count() == 1);
    CHECK_FALSE(result.has_value());
    CHECK(http.pending_front().url() ==
          "https://api.github.com/repos/example/overlap/tags?per_page=100&page=1");
    std::string refreshed_front = paginated_repository_tags(0, 99);
    refreshed_front.insert(1, "{\"name\":\"new-release\"},");
    http.succeed_front(refreshed_front, 200);

    REQUIRE(result.has_value());
    CHECK(result->succeeded());
    REQUIRE(parsed_names.size() == 200);
    CHECK(parsed_names.front() == "new-release");
    CHECK(parsed_names.back() == "release-198");
    boost::property_tree::ptree pagination = read_tag_pagination(cache_file);
    CHECK(pagination.get<uint32_t>("pagination.next_page") == 3);
    CHECK_FALSE(pagination.get<bool>("pagination.history_complete"));

    // The next eligible synchronization resumes after the historical page
    // already validated before the immediate page-one refresh.
    write_tag_pagination(cache_file, 3, false, 0);
    result.reset();
    REQUIRE(updater.begin_sync(1, [](int) {}));
    updater.refresh_repository_tags("overlap", "example/overlap", cache_file, false, parse, finish);
    REQUIRE(http.pending_count() == 1);
    CHECK(http.pending_front().url() ==
          "https://api.github.com/repos/example/overlap/tags?per_page=100&page=3");
    http.succeed_front(paginated_repository_tags(199, 1), 200);
    REQUIRE(result.has_value());
    CHECK(result->succeeded());
}

TEST_CASE("RepositoryUpdater counts failed GitHub tag attempts for one day", "[plugins][updater]")
{
    FakeUpdaterHttpTransport http;
    TestRepositoryUpdater updater(http);
    TemporaryDirectory temporary;
    const boost::filesystem::path cache_file = temporary.path() / "tags.json";
    std::optional<Slic3r::UpdaterError> result;
    const std::function<Slic3r::UpdaterError(const std::string &)> parse =
        [](const std::string &) { return Slic3r::UpdaterError(); };
    const std::function<void(Slic3r::UpdaterError)> finish =
        [&result](Slic3r::UpdaterError error) { result = std::move(error); };

    REQUIRE(updater.begin_sync(1, [](int) {}));
    updater.refresh_repository_tags("failed", "example/failed", cache_file, false, parse, finish);
    REQUIRE(http.pending_count() == 1);
    http.fail_front(std::string(), "offline", 0);
    REQUIRE(result.has_value());
    CHECK(result->code == Slic3r::UpdaterError::Code::Network);

    result.reset();
    REQUIRE(updater.begin_sync(1, [](int) {}));
    updater.refresh_repository_tags("failed", "example/failed", cache_file, false, parse, finish);
    CHECK(http.pending_count() == 0);
    REQUIRE(result.has_value());
    CHECK(result->code == Slic3r::UpdaterError::Code::Cache);

    // A deliberate force bypasses the persisted attempt and may repair the
    // repository immediately.
    result.reset();
    REQUIRE(updater.begin_sync(1, [](int) {}));
    updater.refresh_repository_tags("failed", "example/failed", cache_file, true, parse, finish);
    REQUIRE(http.pending_count() == 1);
    http.succeed_front("[]", 200);
    REQUIRE(result.has_value());
    CHECK(result->succeeded());
}

TEST_CASE("RepositoryUpdater downloads every non-GitHub tag page transactionally", "[plugins][updater]")
{
    FakeUpdaterHttpTransport http;
    TestRepositoryUpdater updater(http);
    TemporaryDirectory temporary;
    const boost::filesystem::path cache_file = temporary.path() / "tags.json";
    std::optional<Slic3r::UpdaterError> result;
    const std::function<Slic3r::UpdaterError(const std::string &)> parse =
        [](const std::string &contents) {
            repository_tag_names(contents);
            return Slic3r::UpdaterError();
        };
    const std::function<void(Slic3r::UpdaterError)> finish =
        [&result](Slic3r::UpdaterError error) { result = std::move(error); };

    SECTION("all pages are collected in one synchronization") {
        REQUIRE(updater.begin_sync(1, [](int) {}));
        updater.refresh_repository_tags(
            "external", "https://updates.example.invalid/external", cache_file, false, parse, finish);
        REQUIRE(http.pending_count() == 1);
        CHECK(http.pending_front().url() ==
              "https://updates.example.invalid/external/tags?per_page=100&page=1");
        http.succeed_front(paginated_repository_tags(0, 100), 200);
        REQUIRE(http.pending_count() == 1);
        CHECK_FALSE(result.has_value());
        CHECK(http.pending_front().url() ==
              "https://updates.example.invalid/external/tags?per_page=100&page=2");
        http.succeed_front(paginated_repository_tags(100, 2), 200);

        REQUIRE(result.has_value());
        CHECK(result->succeeded());
        CHECK(repository_tag_names(read_test_file(cache_file)).size() == 102);
    }

    SECTION("a repeated full page is rejected without replacing the cache") {
        const std::string previous_cache = paginated_repository_tags(500, 1, "cached-");
        write_test_file(cache_file, previous_cache);
        REQUIRE(updater.begin_sync(1, [](int) {}));
        updater.refresh_repository_tags(
            "repeated", "https://updates.example.invalid/repeated", cache_file, false, parse, finish);
        REQUIRE(http.pending_count() == 1);
        const std::string repeated_page = paginated_repository_tags(0, 100);
        http.succeed_front(repeated_page, 200);
        REQUIRE(http.pending_count() == 1);
        http.succeed_front(repeated_page, 200);

        REQUIRE(result.has_value());
        CHECK(result->code == Slic3r::UpdaterError::Code::InvalidRepositoryMetadata);
        CHECK(read_test_file(cache_file) == previous_cache);
    }

    SECTION("an intermediate transport failure preserves the previous cache") {
        const std::string previous_cache = paginated_repository_tags(700, 1, "cached-");
        write_test_file(cache_file, previous_cache);
        REQUIRE(updater.begin_sync(1, [](int) {}));
        updater.refresh_repository_tags(
            "offline", "https://updates.example.invalid/offline", cache_file, false, parse, finish);
        REQUIRE(http.pending_count() == 1);
        http.succeed_front(paginated_repository_tags(0, 100), 200);
        REQUIRE(http.pending_count() == 1);
        http.fail_front(std::string(), "offline", 0);

        REQUIRE(result.has_value());
        CHECK(result->code == Slic3r::UpdaterError::Code::Network);
        CHECK(read_test_file(cache_file) == previous_cache);
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

TEST_CASE("RepositoryUpdater serializes the GitHub request limit across threads", "[plugins][updater]")
{
    FakeUpdaterHttpTransport http;
    TestRepositoryUpdater updater(http);
    constexpr size_t worker_count = 64;
    std::atomic_size_t ready_workers{0};
    std::atomic_size_t accepted_requests{0};
    std::atomic_bool start{false};
    std::vector<std::thread> workers;
    workers.reserve(worker_count);

    // Release every worker together so several request paths contend for the
    // same first-window initialization and remaining request budget.
    for (size_t worker_idx = 0; worker_idx < worker_count; ++worker_idx) {
        workers.emplace_back([&updater, &ready_workers, &accepted_requests, &start] {
            ready_workers.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            if (updater.has_api_request_slot("https://api.github.com/repos/example/repository"))
                accepted_requests.fetch_add(1, std::memory_order_relaxed);
        });
    }
    while (ready_workers.load(std::memory_order_acquire) != worker_count)
        std::this_thread::yield();
    start.store(true, std::memory_order_release);
    for (std::thread &worker : workers)
        worker.join();

    CHECK(accepted_requests.load(std::memory_order_relaxed) == 24);
    CHECK_FALSE(updater.has_api_request_slot("https://api.github.com/repos/example/repository"));
    CHECK(updater.has_api_request_slot("https://updates.example.invalid/repository"));
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
        const boost::filesystem::path invalid_cache = temporary.path() / "invalid.json";
        REQUIRE(updater.begin_sync(1, [](int) {}));
        updater.refresh_repository_tags(
            "invalid", "https://example.invalid/invalid", invalid_cache, true,
            [](const std::string &) {
                return Slic3r::make_updater_error(Slic3r::UpdaterError::Code::InvalidRepositoryMetadata,
                                                  "tag name has no slicer version");
            },
            store_result);
        REQUIRE(http.pending_count() == 1);
        http.succeed_front(paginated_repository_tags(0, 1), 200);

        REQUIRE(result.has_value());
        CHECK(result->code == Slic3r::UpdaterError::Code::InvalidRepositoryMetadata);
        CHECK(result->detail == "tag name has no slicer version");
        CHECK_FALSE(boost::filesystem::exists(invalid_cache));
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
    compare.kind = TestRepositoryUpdater::RepositoryChangelogKind::Compare;
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
    CHECK(http.pending_at(0).total_timeout_seconds() == 60);
    CHECK(http.pending_at(1).total_timeout_seconds() == 60);
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

    bool temporary_file_found = false;
    for (boost::filesystem::directory_iterator it(commit_cache.parent_path()), end; it != end; ++it) {
        const std::string filename = it->path().filename().string();
        temporary_file_found = temporary_file_found ||
            filename.find(".download-") != std::string::npos ||
            filename.find(".previous-") != std::string::npos;
    }
    CHECK_FALSE(temporary_file_found);

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
    CHECK_FALSE(boost::filesystem::exists(invalid.cache_file));
    http.fail_front(std::string(), "offline", 0);

    REQUIRE(batch_succeeded.has_value());
    CHECK_FALSE(*batch_succeeded);
    CHECK(callback_count == 1);
    CHECK_FALSE(updater.changelog_download_in_progress());
}

TEST_CASE("RepositoryUpdater invalid changelogs preserve and reuse recent caches", "[plugins][updater]")
{
    FakeUpdaterHttpTransport http;
    TestRepositoryUpdater updater(http);
    TemporaryDirectory temporary;
    const boost::filesystem::path cache_file = temporary.path() / "logs" / "commit.json";
    const std::string cached_json = R"({"commit":{"message":"cached notes"}})";
    write_test_file(cache_file, cached_json);

    std::string notes = "unchanged";
    TestRepositoryUpdater::RepositoryChangelogRequest request;
    request.cache_file = cache_file;
    request.url = "https://example.invalid/commit/one";
    request.store_notes = [&notes](std::string value) { notes = std::move(value); };

    std::optional<bool> result;
    int callback_count = 0;
    updater.download_repository_changelogs(
        {request},
        [&result, &callback_count](bool succeeded) {
            result = succeeded;
            ++callback_count;
        },
        true);
    REQUIRE(http.pending_count() == 1);
    http.succeed_front("not JSON", 200);

    REQUIRE(result.has_value());
    CHECK_FALSE(*result);
    CHECK(callback_count == 1);
    CHECK(notes == "unchanged");
    CHECK(read_test_file(cache_file) == cached_json);

    // A normal refresh now consumes the unmodified recent cache and does not
    // retry HTTP until that cache expires.
    result.reset();
    updater.download_repository_changelogs(
        {request},
        [&result, &callback_count](bool succeeded) {
            result = succeeded;
            ++callback_count;
        },
        false);
    CHECK(http.pending_count() == 0);
    REQUIRE(result.has_value());
    CHECK(*result);
    CHECK(callback_count == 2);
    CHECK(notes == "cached notes");
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

TEST_CASE("RepositoryUpdater consumes valid changelogs when cache publication fails", "[plugins][updater]")
{
    FakeUpdaterHttpTransport http;
    TestRepositoryUpdater updater(http);
    TemporaryDirectory temporary;
    const boost::filesystem::path blocking_file = temporary.path() / "not-a-directory";
    write_test_file(blocking_file, "file");

    std::string notes;
    TestRepositoryUpdater::RepositoryChangelogRequest request;
    request.cache_file = blocking_file / "commit.json";
    request.url = "https://example.invalid/commit/one";
    request.store_notes = [&notes](std::string value) { notes = std::move(value); };

    std::optional<bool> result;
    updater.download_repository_changelogs(
        {request}, [&result](bool succeeded) { result = succeeded; }, true);
    REQUIRE(http.pending_count() == 1);
    http.succeed_front(R"({"commit":{"message":"validated notes"}})", 200);

    REQUIRE(result.has_value());
    CHECK(*result);
    CHECK(notes == "validated notes");
    CHECK_FALSE(boost::filesystem::exists(request.cache_file));
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
          "https://raw.githubusercontent.com/example/repository/HEAD/description.ini");
    CHECK(http.pending_front().response_size_limit() == 64 * 1024);
    CHECK(http.pending_front().total_timeout_seconds() == 60);
    http.succeed_front("repository description", 200);

    REQUIRE(result.has_value());
    CHECK(result->succeeded());
    CHECK(consumed_contents == "repository description");
    CHECK(consumed_fallback_id == "repository");
}

TEST_CASE("RepositoryUpdater preserves description endpoint errors", "[plugins][updater]")
{
    FakeUpdaterHttpTransport http;
    TestRepositoryUpdater updater(http);
    size_t consumer_calls = 0;
    std::optional<Slic3r::UpdaterError> result;

    SECTION("non-GitHub repositories keep their direct description endpoint") {
        updater.download_repository_description(
            "https://packages.example.invalid/repository",
            [&consumer_calls](const std::string &contents, const std::string &fallback_id) {
                ++consumer_calls;
                CHECK(contents == "direct description");
                CHECK(fallback_id == "repository");
                return Slic3r::UpdaterError();
            },
            [&result](Slic3r::UpdaterError error) { result = std::move(error); });

        REQUIRE(http.pending_count() == 1);
        CHECK(http.pending_front().url() ==
              "https://packages.example.invalid/repository/description");
        http.succeed_front("direct description", 200);

        REQUIRE(result.has_value());
        CHECK(result->succeeded());
        CHECK(consumer_calls == 1);
    }

    SECTION("a missing GitHub description is a missing repository") {
        updater.download_repository_description(
            "example/repository",
            [&consumer_calls](const std::string &, const std::string &) {
                ++consumer_calls;
                return Slic3r::UpdaterError();
            },
            [&result](Slic3r::UpdaterError error) { result = std::move(error); });

        REQUIRE(http.pending_count() == 1);
        CHECK(http.pending_front().url() ==
              "https://raw.githubusercontent.com/example/repository/HEAD/description.ini");
        http.fail_front(std::string(), "description not found", 404);

        REQUIRE(result.has_value());
        CHECK(result->code == Slic3r::UpdaterError::Code::RepositoryNotFound);
        CHECK(result->detail == "description not found");
        CHECK(consumer_calls == 0);
    }

    SECTION("the consumer keeps ownership of description validation") {
        updater.download_repository_description(
            "example/repository",
            [&consumer_calls](const std::string &, const std::string &) {
                ++consumer_calls;
                return Slic3r::make_updater_error(Slic3r::UpdaterError::Code::InvalidArchive,
                                                  "invalid description");
            },
            [&result](Slic3r::UpdaterError error) { result = std::move(error); });

        REQUIRE(http.pending_count() == 1);
        http.succeed_front("invalid repository description", 200);

        REQUIRE(result.has_value());
        CHECK(result->code == Slic3r::UpdaterError::Code::InvalidArchive);
        CHECK(result->detail == "invalid description");
        CHECK(consumer_calls == 1);
    }
}

TEST_CASE("RepositoryUpdater writes asynchronous and synchronous repository files", "[plugins][updater]")
{
    FakeUpdaterHttpTransport http;
    TestRepositoryUpdater updater(http);
    TemporaryDirectory temporary;

    SECTION("asynchronous success") {
        const boost::filesystem::path destination = temporary.path() / "async" / "archive.zip";
        std::optional<Slic3r::UpdaterError> result;

        // Hold the serialized worker so the HTTP completion can only enqueue
        // the file write. This makes the asynchronous observation independent
        // from thread scheduling speed.
        std::promise<void> blocker_started;
        std::future<void> blocker_started_future = blocker_started.get_future();
        std::promise<void> release_blocker;
        std::future<void> release_blocker_future = release_blocker.get_future();
        REQUIRE(updater.enqueue_operation(
            [&blocker_started, &release_blocker_future] {
                blocker_started.set_value();
                release_blocker_future.wait();
                return Slic3r::UpdaterError();
            },
            [](Slic3r::UpdaterError) {}));
        blocker_started_future.wait();

        updater.download_repository_file_async(
            "https://example.invalid/async.zip", destination, 4096,
            [&result](Slic3r::UpdaterError error) { result = std::move(error); });

        REQUIRE(http.pending_count() == 1);
        CHECK(http.pending_front().response_size_limit() == 4096);
        CHECK(http.pending_front().total_timeout_seconds() == 5 * 60);
        http.succeed_front("async archive", 200);
        CHECK_FALSE(result.has_value());
        release_blocker.set_value();
        updater.wait_for_pending_operations();

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
        REQUIRE(http.sync_request_count() == 1);
        CHECK(http.sync_request_timeout(0) == 5 * 60);
        CHECK(read_test_file(destination) == "sync archive");
    }

    SECTION("a configured archive timeout applies to asynchronous and synchronous requests") {
        updater.set_archive_download_timeout(std::chrono::seconds(75));
        const boost::filesystem::path async_destination = temporary.path() / "configured-async.zip";
        std::optional<Slic3r::UpdaterError> async_result;
        updater.download_repository_file_async(
            "https://example.invalid/configured-async.zip", async_destination, 4096,
            [&async_result](Slic3r::UpdaterError error) { async_result = std::move(error); });

        REQUIRE(http.pending_count() == 1);
        CHECK(http.pending_front().total_timeout_seconds() == 75);
        http.fail_front(std::string(), "test complete", 500);
        REQUIRE(async_result.has_value());

        http.script_sync_failure("test complete");
        updater.download_repository_file_sync(
            "https://example.invalid/configured-sync.zip", temporary.path() / "configured-sync.zip", 4096);
        REQUIRE(http.sync_request_count() == 1);
        CHECK(http.sync_request_timeout(0) == 75);
    }

    SECTION("a non-positive archive timeout is rejected") {
        CHECK_THROWS_AS(updater.set_archive_download_timeout(std::chrono::seconds(0)),
                        std::invalid_argument);
        CHECK_THROWS_AS(updater.set_archive_download_timeout(std::chrono::seconds(-1)),
                        std::invalid_argument);
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
        updater.wait_for_pending_operations();
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

TEST_CASE("VendorSync rebuilds remote versions while preserving cached profiles", "[plugins][updater]")
{
    Slic3r::VendorSync vendor;
    vendor.is_installed = true;
    vendor.profile.config_version = *Slic3r::Semver::parse("1.2.3.4");

    Slic3r::VendorAvailable cached;
    cached.config_version = *Slic3r::Semver::parse("1.2.3.4");
    cached.slicer_version = *Slic3r::Semver::parse("2.7.62.0");
    cached.local_file = "cached.ini";
    cached.url_zip = "stale-local-zip";
    cached.commit_sha = "stale-local-sha";
    cached.commit_url = "stale-local-commit";
    cached.tag = "1.2.3.4=2.7.62.0";
    cached.notes = "cached notes";
    vendor.available_profiles.emplace_back(cached);

    Slic3r::VendorAvailable retained_remote;
    retained_remote.config_version = *Slic3r::Semver::parse("2.0.0.0");
    retained_remote.slicer_version = *Slic3r::Semver::parse("2.7.62.0");
    retained_remote.tag = "2.0.0.0=2.7.62.0";
    retained_remote.notes = "retained notes";
    vendor.available_profiles.emplace_back(retained_remote);

    Slic3r::VendorAvailable removed_remote;
    removed_remote.config_version = *Slic3r::Semver::parse("3.0.0.0");
    removed_remote.slicer_version = *Slic3r::Semver::parse("2.7.62.0");
    removed_remote.tag = "3.0.0.0=2.7.62.0";
    vendor.available_profiles.emplace_back(removed_remote);

    REQUIRE(vendor.parse_tags(
        "[{\"name\":\"1.2.3.4=2.7.62.0\",\"zipball_url\":\"local-zip\","
        "\"commit\":{\"sha\":\"local-sha\",\"url\":\"local-commit\"}},"
        "{\"name\":\"2.0.0.0=2.7.62.0\",\"zipball_url\":\"retained-zip\","
        "\"commit\":{\"sha\":\"retained-sha\",\"url\":\"retained-commit\"}},"
        "{\"name\":\"2.5.0.0=2.7.62.0\",\"zipball_url\":\"new-zip\","
        "\"commit\":{\"sha\":\"new-sha\",\"url\":\"new-commit\"}}]").succeeded());
    REQUIRE(vendor.available_profiles.size() == 3);
    CHECK(std::none_of(vendor.available_profiles.begin(), vendor.available_profiles.end(),
        [](const Slic3r::VendorAvailable &available) { return available.tag == "3.0.0.0=2.7.62.0"; }));

    const std::vector<Slic3r::VendorAvailable>::const_iterator local = std::find_if(
        vendor.available_profiles.begin(), vendor.available_profiles.end(),
        [](const Slic3r::VendorAvailable &available) { return available.tag == "1.2.3.4=2.7.62.0"; });
    REQUIRE(local != vendor.available_profiles.end());
    CHECK(local->local_file == "cached.ini");
    CHECK(local->notes == "cached notes");
    CHECK(local->url_zip == "local-zip");
    CHECK(local->commit_sha == "local-sha");
    CHECK(local->commit_url == "local-commit");

    const std::vector<Slic3r::VendorAvailable>::const_iterator retained = std::find_if(
        vendor.available_profiles.begin(), vendor.available_profiles.end(),
        [](const Slic3r::VendorAvailable &available) { return available.tag == "2.0.0.0=2.7.62.0"; });
    REQUIRE(retained != vendor.available_profiles.end());
    CHECK(retained->notes == "retained notes");
    CHECK(retained->url_zip == "retained-zip");
    const std::vector<Slic3r::VendorAvailable>::const_iterator added = std::find_if(
        vendor.available_profiles.begin(), vendor.available_profiles.end(),
        [](const Slic3r::VendorAvailable &available) { return available.tag == "2.5.0.0=2.7.62.0"; });
    REQUIRE(added != vendor.available_profiles.end());
    CHECK(added->local_file.empty());
    CHECK(added->url_zip == "new-zip");
    CHECK(vendor.can_upgrade);

    // An empty successful response removes every remote-only version. The
    // cached profile remains usable without retaining stale repository links.
    REQUIRE(vendor.parse_tags("[]").succeeded());
    REQUIRE(vendor.available_profiles.size() == 1);
    CHECK(vendor.available_profiles.front().local_file == "cached.ini");
    CHECK(vendor.available_profiles.front().notes == "cached notes");
    CHECK(vendor.available_profiles.front().url_zip.empty());
    CHECK(vendor.available_profiles.front().commit_sha.empty());
    CHECK(vendor.available_profiles.front().commit_url.empty());
    CHECK_FALSE(vendor.can_upgrade);

    const Slic3r::VendorAvailable preserved = vendor.available_profiles.front();
    CHECK_FALSE(vendor.parse_tags("not JSON").succeeded());
    REQUIRE(vendor.available_profiles.size() == 1);
    CHECK(vendor.available_profiles.front().tag == preserved.tag);
    CHECK(vendor.available_profiles.front().local_file == preserved.local_file);
    CHECK(vendor.available_profiles.front().notes == preserved.notes);
    CHECK_FALSE(vendor.can_upgrade);
}

TEST_CASE("PluginSync rebuilds remote versions while preserving cached packages", "[plugins][updater]")
{
    Slic3r::PluginSync plugin;
    plugin.is_installed = true;
    plugin.installed_version.package_version = "1.2.3.4";
    plugin.installed_version.slicer_version = "2.7.62.0";

    Slic3r::PluginAvailable cached;
    cached.package_version = "1.2.3.4";
    cached.slicer_version = "2.7.62.0";
    cached.local_directory = "cached-package";
    cached.url_zip = "stale-local-zip";
    cached.commit_sha = "stale-local-sha";
    cached.commit_url = "stale-local-commit";
    cached.tag = "1.2.3.4=2.7.62.0";
    cached.notes = "cached notes";
    plugin.available_packages.emplace_back(cached);

    Slic3r::PluginAvailable retained_remote;
    retained_remote.package_version = "2.0.0.0";
    retained_remote.slicer_version = "2.7.62.0";
    retained_remote.tag = "2.0.0.0=2.7.62.0";
    retained_remote.notes = "retained notes";
    plugin.available_packages.emplace_back(retained_remote);

    Slic3r::PluginAvailable removed_remote;
    removed_remote.package_version = "3.0.0.0";
    removed_remote.slicer_version = "2.7.62.0";
    removed_remote.tag = "3.0.0.0=2.7.62.0";
    plugin.available_packages.emplace_back(removed_remote);

    REQUIRE(plugin.parse_tags(
        "[{\"name\":\"1.2.3.4=2.7.62.0\",\"zipball_url\":\"local-zip\","
        "\"commit\":{\"sha\":\"local-sha\",\"url\":\"local-commit\"}},"
        "{\"name\":\"2.0.0.0=2.7.62.0\",\"zipball_url\":\"retained-zip\","
        "\"commit\":{\"sha\":\"retained-sha\",\"url\":\"retained-commit\"}},"
        "{\"name\":\"2.5.0.0=2.7.62.0\",\"zipball_url\":\"new-zip\","
        "\"commit\":{\"sha\":\"new-sha\",\"url\":\"new-commit\"}}]").succeeded());
    REQUIRE(plugin.available_packages.size() == 3);
    CHECK(std::none_of(plugin.available_packages.begin(), plugin.available_packages.end(),
        [](const Slic3r::PluginAvailable &available) { return available.tag == "3.0.0.0=2.7.62.0"; }));

    const std::vector<Slic3r::PluginAvailable>::const_iterator local = std::find_if(
        plugin.available_packages.begin(), plugin.available_packages.end(),
        [](const Slic3r::PluginAvailable &available) { return available.tag == "1.2.3.4=2.7.62.0"; });
    REQUIRE(local != plugin.available_packages.end());
    CHECK(local->local_directory == "cached-package");
    CHECK(local->notes == "cached notes");
    CHECK(local->url_zip == "local-zip");
    CHECK(local->commit_sha == "local-sha");
    CHECK(local->commit_url == "local-commit");

    const std::vector<Slic3r::PluginAvailable>::const_iterator retained = std::find_if(
        plugin.available_packages.begin(), plugin.available_packages.end(),
        [](const Slic3r::PluginAvailable &available) { return available.tag == "2.0.0.0=2.7.62.0"; });
    REQUIRE(retained != plugin.available_packages.end());
    CHECK(retained->notes == "retained notes");
    CHECK(retained->url_zip == "retained-zip");
    const std::vector<Slic3r::PluginAvailable>::const_iterator added = std::find_if(
        plugin.available_packages.begin(), plugin.available_packages.end(),
        [](const Slic3r::PluginAvailable &available) { return available.tag == "2.5.0.0=2.7.62.0"; });
    REQUIRE(added != plugin.available_packages.end());
    CHECK(added->local_directory.empty());
    CHECK(added->url_zip == "new-zip");
    CHECK_FALSE(plugin.can_upgrade);
    CHECK(added->metadata.compatibility.status == Slic3r::PluginApiCompatibilityStatus::NotChecked);

    REQUIRE(plugin.parse_tags("[]").succeeded());
    REQUIRE(plugin.available_packages.size() == 1);
    CHECK(plugin.available_packages.front().local_directory == "cached-package");
    CHECK(plugin.available_packages.front().notes == "cached notes");
    CHECK(plugin.available_packages.front().url_zip.empty());
    CHECK(plugin.available_packages.front().commit_sha.empty());
    CHECK(plugin.available_packages.front().commit_url.empty());
    CHECK_FALSE(plugin.can_upgrade);

    const Slic3r::PluginAvailable preserved = plugin.available_packages.front();
    CHECK_FALSE(plugin.parse_tags("not JSON").succeeded());
    REQUIRE(plugin.available_packages.size() == 1);
    CHECK(plugin.available_packages.front().tag == preserved.tag);
    CHECK(plugin.available_packages.front().local_directory == preserved.local_directory);
    CHECK(plugin.available_packages.front().notes == preserved.notes);
    CHECK_FALSE(plugin.can_upgrade);
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
          "https://raw.githubusercontent.com/example/plugin/HEAD/description.ini");
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
    std::optional<Slic3r::PluginSync> plugin = updater.plugin(plugin_id);
    REQUIRE(plugin.has_value());
    CHECK(plugin->sync_state == Slic3r::RepositorySyncState::Unchecked);
    CHECK(plugin->sync_error.succeeded());

    std::optional<int> first_count;
    updater.sync_async([&first_count](int count) { first_count = count; }, true);
    plugin = updater.plugin(plugin_id);
    REQUIRE(plugin.has_value());
    CHECK(plugin->sync_state == Slic3r::RepositorySyncState::InProgress);
    CHECK(plugin->sync_error.succeeded());
    REQUIRE(http.pending_count() == 1);
    http.fail_front(std::string(), "offline", 0);

    REQUIRE(first_count.has_value());
    plugin = updater.plugin(plugin_id);
    REQUIRE(plugin.has_value());
    CHECK(plugin->sync_state == Slic3r::RepositorySyncState::Failed);
    CHECK(plugin->sync_error.code == Slic3r::UpdaterError::Code::Network);
    CHECK(plugin->sync_error.detail == "offline");

    // Starting a retry immediately clears the stale error. Its successful
    // terminal callback then leaves one unambiguous completed state.
    std::optional<int> retry_count;
    updater.sync_async([&retry_count](int count) { retry_count = count; }, true);
    plugin = updater.plugin(plugin_id);
    REQUIRE(plugin.has_value());
    CHECK(plugin->sync_state == Slic3r::RepositorySyncState::InProgress);
    CHECK(plugin->sync_error.succeeded());
    REQUIRE(http.pending_count() == 1);
    http.succeed_front(plugin_repository_tags({
        {"1.0.0.0", slicer_version, "https://example.invalid/plugin.zip"}
    }), 200);

    REQUIRE(retry_count.has_value());
    plugin = updater.plugin(plugin_id);
    REQUIRE(plugin.has_value());
    CHECK(plugin->sync_state == Slic3r::RepositorySyncState::Succeeded);
    CHECK(plugin->sync_error.succeeded());
}

TEST_CASE("Updater snapshots calculate their best version after copy and move", "[plugins][updater][snapshot]")
{
    Slic3r::PluginSync plugin;
    Slic3r::PluginAvailable older_plugin;
    older_plugin.package_version = "1.0.0.0";
    older_plugin.slicer_version = "1.0.0.0";
    Slic3r::PluginAvailable newer_plugin = older_plugin;
    newer_plugin.package_version = "2.0.0.0";
    plugin.available_packages = {older_plugin, newer_plugin};
    plugin.sort_available();

    // Remote versions are not automatically eligible until their manifest is read.
    CHECK(plugin.best_available() == nullptr);
    for (Slic3r::PluginAvailable &version : plugin.available_packages)
        version.metadata.compatibility.status = Slic3r::PluginApiCompatibilityStatus::Compatible;
    Slic3r::PluginSync plugin_copy = plugin;
    Slic3r::PluginSync plugin_move = std::move(plugin_copy);
    const Slic3r::PluginAvailable *plugin_best = plugin_move.best_available();
    REQUIRE(plugin_best != nullptr);
    CHECK(plugin_best == &plugin_move.available_packages.front());
    CHECK(plugin_best->package_version == "2.0.0.0");

    Slic3r::VendorSync vendor;
    Slic3r::VendorAvailable older;
    older.config_version = *Slic3r::Semver::parse("1.0.0.0");
    older.slicer_version = *Slic3r::Semver::parse("1.0.0.0");
    Slic3r::VendorAvailable newer = older;
    newer.config_version = *Slic3r::Semver::parse("2.0.0.0");
    vendor.available_profiles = {older, newer};
    vendor.sort_available();

    Slic3r::VendorSync vendor_copy = vendor;
    Slic3r::VendorSync vendor_move = std::move(vendor_copy);
    const Slic3r::VendorAvailable *vendor_best = vendor_move.best_available();
    REQUIRE(vendor_best != nullptr);
    CHECK(vendor_best == &vendor_move.available_profiles.front());
    CHECK(vendor_best->config_version.to_string() == "2.0.0.0");
}

TEST_CASE_METHOD(PluginUpdaterFunctionalFixture,
                 "PluginUpdater publishes worker results while snapshots are read",
                 "[plugins][updater][plugin-functional][snapshot]")
{
    write_plugin_repository();
    updater.reload_all_plugins();
    const std::optional<Slic3r::PluginSync> old_snapshot = updater.plugin(plugin_id);
    REQUIRE(old_snapshot.has_value());
    REQUIRE(old_snapshot->sync_state == Slic3r::RepositorySyncState::Unchecked);

    bool callback_read_model = false;
    updater.sync_async([this, &callback_read_model](int) {
        // A terminal callback runs after the updater releases its model lock.
        // Reading a snapshot here would deadlock if that contract regressed.
        callback_read_model = updater.plugin(plugin_id).has_value();
    }, true);
    REQUIRE(http.pending_count() == 1);

    std::atomic_bool keep_reading{true};
    std::atomic_bool reader_started{false};
    std::atomic_bool observed_missing_model{false};
    std::thread reader([this, &keep_reading, &reader_started, &observed_missing_model] {
        reader_started = true;
        while (keep_reading) {
            if (!updater.plugin(plugin_id).has_value() || updater.plugins().empty() ||
                updater.plugin_ids().empty() || updater.count_available() == 0)
                observed_missing_model = true;
        }
    });
    while (!reader_started)
        std::this_thread::yield();

    http.succeed_front(plugin_repository_tags({
        {"2.0.0.0", slicer_version, "https://example.invalid/plugin-2.zip"}
    }), 200);
    keep_reading = false;
    reader.join();

    CHECK_FALSE(observed_missing_model);
    CHECK(callback_read_model);
    CHECK(old_snapshot->sync_state == Slic3r::RepositorySyncState::Unchecked);
    CHECK(old_snapshot->available_packages.empty());

    const std::optional<Slic3r::PluginSync> new_snapshot = updater.plugin(plugin_id);
    REQUIRE(new_snapshot.has_value());
    CHECK(new_snapshot->sync_state == Slic3r::RepositorySyncState::Succeeded);
    REQUIRE(new_snapshot->available_packages.size() == 1);
    CHECK(new_snapshot->available_packages.front().package_version == "2.0.0.0");
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

    std::string startup_error;
    REQUIRE(Slic3r::prepare_plugin_bundle_cache(resources_directory, data_directory, startup_error));
    Slic3r::PluginUpdater updater(http);
    updater.reload_all_plugins();
    REQUIRE(updater.count_available() == 1);
    std::optional<int> update_count;
    updater.sync_async([&update_count](int count) { update_count = count; }, true);
    REQUIRE(http.pending_count() == 1);
    CHECK(http.pending_front().url() ==
          "https://api.github.com/repos/example/repository/tags?per_page=100&page=1");

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

    std::optional<Slic3r::PluginSync> plugin = updater.plugin("example.plugin");
    REQUIRE(plugin.has_value());
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

    // Reload is deliberately ignored while changelog requests are active. The
    // detached snapshot remains valid either way and cannot alias the model.
    const Slic3r::PluginSync snapshot_before_reload = *plugin;
    updater.reload_all_plugins();
    CHECK(snapshot_before_reload.available_packages.size() == 4);
    REQUIRE(updater.plugin("example.plugin").has_value());

    const std::string compare_json =
        R"({"commits":[{"commit":{"message":"older"}},{"commit":{"message":"newer"}}]})";
    http.succeed_front(compare_json, 200);
    http.succeed_front(compare_json, 200);
    http.succeed_front(compare_json, 200);
    http.succeed_front(R"({"commit":{"message":"initial"}})", 200);

    REQUIRE(changelogs_succeeded.has_value());
    CHECK(*changelogs_succeeded);
    CHECK(callback_count == 1);
    CHECK(snapshot_before_reload.available_packages.front().notes.empty());
    plugin = updater.plugin("example.plugin");
    REQUIRE(plugin.has_value());
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
    plugin = updater.plugin("example.plugin");
    REQUIRE(plugin.has_value());
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

    std::optional<Slic3r::PluginSync> plugin = updater.plugin(plugin_id);
    REQUIRE(plugin.has_value());
    CHECK(plugin->best_available() == nullptr);
    REQUIRE(plugin->available_packages.size() == 3);
    CHECK(plugin->available_packages.front().package_version == "3.0.0.0");

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

    plugin = updater.plugin(plugin_id);
    REQUIRE(plugin.has_value());

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
    updater.wait_for_pending_operations();

    REQUIRE(install_result.has_value());
    CHECK(install_result->succeeded());
    CHECK_FALSE(boost::filesystem::exists(data_directory / "plugins" / plugin_id));
    CHECK(boost::filesystem::is_directory(Slic3r::repository_package_cache_path(
        data_directory, Slic3r::RepositoryPackageType::Plugin, plugin_id, "2.0.0.0", slicer_version)));

    const Slic3r::PluginActivationConfig config = read_activation_config();
    REQUIRE(config.installed.count(plugin_id) == 1);
    CHECK(config.installed.at(plugin_id).package_version == "2.0.0.0");
    CHECK(config.installed.at(plugin_id).slicer_version == slicer_version);
    const std::string activation_contents = read_test_file(Slic3r::plugin_activation_config_path(data_directory));
    CHECK(activation_contents.find(plugin_id + " = 2.0.0.0") != std::string::npos);
    CHECK(activation_contents.find(plugin_id + ".slicer_version = " + slicer_version) != std::string::npos);
}

TEST_CASE_METHOD(PluginUpdaterFunctionalFixture,
                 "Plugin ABI is verified after download before installation is scheduled",
                 "[plugins][updater][plugin-functional][abi]")
{
    const bool compatible = GENERATE(true, false);
    write_plugin_repository();
    updater.reload_all_plugins();
    synchronize({{"2.0.0.0", slicer_version, "https://example.invalid/plugin-2.zip"}});
    std::optional<Slic3r::PluginSync> plugin = updater.plugin(plugin_id);
    REQUIRE(plugin.has_value());
    REQUIRE(plugin->available_packages.size() == 1);
    CHECK(plugin->best_available() == nullptr);
    CHECK(plugin->available_packages.front().metadata.compatibility.status == Slic3r::PluginApiCompatibilityStatus::NotChecked);
    std::optional<Slic3r::UpdaterError> result;
    updater.install_plugin(plugin_id, plugin->available_packages.front(),
        [&result](Slic3r::UpdaterError error) { result = std::move(error); });
    REQUIRE(http.pending_count() == 1);
    const boost::filesystem::path archive = temporary.path() / "abi.zip";
    REQUIRE(write_test_zip(archive, {
        {"description.ini", plugin_description_contents(plugin_id, "2.0.0.0", slicer_version, false)},
        {"version.ini", "[plugin]\npackage_version=2.0.0.0\nslicer_version=" + slicer_version +
            "\n[abi]\nslic3r_plugin_types.h=" + (compatible ? "1.0" : "99.0") + "\n"},
        {plugin_library_filename(), "test library"}}));
    http.succeed_front(read_test_file(archive), 200);
    updater.wait_for_pending_operations();
    REQUIRE(result.has_value());
    CHECK(result->succeeded() == compatible);
    plugin = updater.plugin(plugin_id);
    REQUIRE(plugin.has_value());
    CHECK(plugin->available_packages.front().metadata.compatibility.compatible() == compatible);
    CHECK_FALSE(plugin->available_packages.front().local_directory.empty());
    CHECK_FALSE(boost::filesystem::exists(data_directory / "plugins" / plugin_id));
    CHECK((read_activation_config().installed.count(plugin_id) != 0) == compatible);
    CHECK((plugin->best_available() != nullptr) == compatible);
}

TEST_CASE_METHOD(PluginUpdaterFunctionalFixture,
                 "PluginUpdater reserves a remote installation across its HTTP wait",
                 "[plugins][updater][plugin-functional][concurrency]")
{
    write_plugin_repository();
    updater.reload_all_plugins();
    synchronize({{"2.0.0.0", slicer_version, "https://example.invalid/plugin-2.zip"}});

    const std::optional<Slic3r::PluginSync> plugin = updater.plugin(plugin_id);
    REQUIRE(plugin.has_value());
    REQUIRE_FALSE(plugin->available_packages.empty());
    const Slic3r::PluginAvailable *selected = &plugin->available_packages.front();
    REQUIRE(selected != nullptr);
    const Slic3r::PluginAvailable version = *selected;

    // Keep the archive request pending so every following call runs during the
    // gap which previously was not covered by the serialized filesystem worker.
    std::optional<Slic3r::UpdaterError> install_result;
    updater.install_plugin(plugin_id, version, [&install_result](Slic3r::UpdaterError error) {
        install_result = std::move(error);
    });
    REQUIRE(http.pending_count() == 1);
    CHECK(updater.repository_change_in_progress());

    std::vector<Slic3r::UpdaterError> rejected;
    const std::function<void(Slic3r::UpdaterError)> record_rejection =
        [&rejected](Slic3r::UpdaterError error) { rejected.emplace_back(std::move(error)); };
    updater.clear_cache_plugin(plugin_id, record_rejection);
    updater.uninstall_plugin(plugin_id, record_rejection);
    updater.install_plugin(plugin_id, version, record_rejection);
    updater.download_new_repo("https://example.invalid/other", record_rejection);

    const boost::filesystem::path other_package = temporary.path() / "another.plugin";
    write_test_file(other_package / plugin_library_filename(), "another plugin");
    updater.cache_plugin_directory(other_package, record_rejection);

    REQUIRE(rejected.size() == 5);
    for (const Slic3r::UpdaterError &error : rejected)
        CHECK(error.code == Slic3r::UpdaterError::Code::PreparationRejected);
    CHECK_FALSE(updater.plugin("another.plugin").has_value());

    // Completing the original operation publishes one coherent cache and
    // activation selection, then releases the mutation reservation.
    http.succeed_front(make_plugin_archive("2.0.0.0"), 200);
    updater.wait_for_pending_operations();
    REQUIRE(install_result.has_value());
    CHECK(install_result->succeeded());
    CHECK_FALSE(updater.repository_change_in_progress());
    const Slic3r::PluginActivationConfig config = read_activation_config();
    REQUIRE(config.installed.count(plugin_id) == 1);
    CHECK(config.installed.at(plugin_id).package_version == "2.0.0.0");
}

TEST_CASE_METHOD(PluginUpdaterFunctionalFixture,
                 "PluginUpdater releases its mutation gate before the terminal callback",
                 "[plugins][updater][plugin-functional][concurrency]")
{
    write_plugin_repository();
    write_cached_plugin("1.0.0.0");
    updater.reload_all_plugins();
    const std::optional<Slic3r::PluginSync> plugin = updater.plugin(plugin_id);
    REQUIRE(plugin.has_value());
    REQUIRE_FALSE(plugin->available_packages.empty());
    const Slic3r::PluginAvailable *selected = &plugin->available_packages.front();
    REQUIRE(selected != nullptr);

    // The completion callback immediately starts a second mutation. It can be
    // accepted only if the first operation releases the shared gate beforehand.
    std::optional<Slic3r::UpdaterError> install_result;
    std::optional<Slic3r::UpdaterError> uninstall_result;
    updater.install_plugin(
        plugin_id, *selected,
        [this, &install_result, &uninstall_result](Slic3r::UpdaterError error) {
            install_result = error;
            updater.uninstall_plugin(
                plugin_id,
                [&uninstall_result](Slic3r::UpdaterError uninstall_error) {
                    uninstall_result = std::move(uninstall_error);
                });
        });
    updater.wait_for_pending_operations();

    REQUIRE(install_result.has_value());
    CHECK(install_result->succeeded());
    REQUIRE(uninstall_result.has_value());
    CHECK(uninstall_result->succeeded());
    CHECK_FALSE(updater.repository_change_in_progress());
    CHECK(read_activation_config().installed.count(plugin_id) == 0);
}

TEST_CASE_METHOD(PluginUpdaterFunctionalFixture,
                 "PluginUpdater releases its mutation gate after a network error",
                 "[plugins][updater][plugin-functional][concurrency]")
{
    write_plugin_repository();
    updater.reload_all_plugins();
    synchronize({{"2.0.0.0", slicer_version, "https://example.invalid/plugin-2.zip"}});
    const std::optional<Slic3r::PluginSync> plugin = updater.plugin(plugin_id);
    REQUIRE(plugin.has_value());
    REQUIRE_FALSE(plugin->available_packages.empty());
    const Slic3r::PluginAvailable *selected = &plugin->available_packages.front();
    REQUIRE(selected != nullptr);

    std::optional<Slic3r::UpdaterError> install_result;
    updater.install_plugin(plugin_id, *selected, [&install_result](Slic3r::UpdaterError error) {
        install_result = std::move(error);
    });
    REQUIRE(http.pending_count() == 1);
    http.fail_front(std::string(), "The test transport timed out.", 0);

    REQUIRE(install_result.has_value());
    CHECK_FALSE(install_result->succeeded());
    CHECK_FALSE(updater.repository_change_in_progress());

    std::optional<Slic3r::UpdaterError> clear_result;
    updater.clear_cache_plugin(plugin_id, [&clear_result](Slic3r::UpdaterError error) {
        clear_result = std::move(error);
    });
    updater.wait_for_pending_operations();
    REQUIRE(clear_result.has_value());
    CHECK(clear_result->succeeded());
}

TEST_CASE_METHOD(PluginUpdaterFunctionalFixture,
                 "Desired plugin packages are reconciled safely at startup",
                 "[plugins][updater][plugin-functional]")
{
    SECTION("a cached request is installed") {
        write_cached_plugin("1.0.0.0");
        Slic3r::PluginActivationConfig config;
        config.installed[plugin_id] = {"1.0.0.0", slicer_version};
        std::string error_message;
        REQUIRE(Slic3r::write_plugin_activation_config(
            Slic3r::plugin_activation_config_path(data_directory), config, error_message));

        REQUIRE(Slic3r::reconcile_installed_plugin_packages(
            data_directory, config, error_message));
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

        CHECK_FALSE(Slic3r::reconcile_installed_plugin_packages(
            data_directory, config, error_message));
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
    const boost::filesystem::path cache_root = write_cached_plugin("1.0.0.0");
    Slic3r::PluginActivationConfig configured = read_activation_config();
    configured.activated["example.first"] = true;
    configured.activated["example.second"] = true;
    configured.plugin_packages["example.first"] = plugin_id;
    configured.plugin_packages["example.second"] = plugin_id;
    std::string configuration_error;
    REQUIRE(Slic3r::write_plugin_activation_config(
        Slic3r::plugin_activation_config_path(data_directory), configured, configuration_error));
    updater.reload_all_plugins();
    std::optional<Slic3r::PluginSync> plugin = updater.plugin(plugin_id);
    REQUIRE(plugin.has_value());
    REQUIRE(plugin->is_installed);

    std::optional<Slic3r::UpdaterError> uninstall_result;
    updater.uninstall_plugin(plugin_id, [&uninstall_result](Slic3r::UpdaterError error) {
        uninstall_result = std::move(error);
    });
    updater.wait_for_pending_operations();

    REQUIRE(uninstall_result.has_value());
    CHECK(uninstall_result->succeeded());
    CHECK(boost::filesystem::is_directory(data_directory / "plugins" / plugin_id));
    const Slic3r::PluginActivationConfig config = read_activation_config();
    CHECK(config.installed.count(plugin_id) == 0);
    CHECK(config.activated.count("example.first") == 0);
    CHECK(config.activated.count("example.second") == 0);
    CHECK(config.plugin_packages.count("example.first") == 0);
    CHECK(config.plugin_packages.count("example.second") == 0);
    plugin = updater.plugin(plugin_id);
    REQUIRE(plugin.has_value());
    CHECK_FALSE(plugin->is_installed);
    CHECK(plugin->has_cache);
    CHECK(boost::filesystem::is_directory(cache_root));
}

TEST_CASE_METHOD(PluginUpdaterFunctionalFixture,
                 "PluginUpdater exposes the startup package load report",
                 "[plugins][updater][plugin-functional][loader]")
{
    Slic3r::Orchestrator &orchestrator = Slic3r::Orchestrator::instance();
    orchestrator.begin_plugin_package_load(plugin_id,
        (data_directory / "plugins" / plugin_id).string());
    Slic3r::PluginPackageLoadIssue issue;
    issue.code = Slic3r::PluginPackageLoadErrorCode::DependencyMissing;
    issue.detail = "The dependent runtime library was not found.";
    issue.system_error = 126;
    orchestrator.report_plugin_package_load_issue(plugin_id, std::move(issue));
    orchestrator.finish_plugin_package_load(plugin_id);

    updater.reload_all_plugins();
    const std::optional<Slic3r::PluginSync> plugin = updater.plugin(plugin_id);
    REQUIRE(plugin.has_value());
    REQUIRE(plugin->load_report.has_value());
    REQUIRE(plugin->load_report->issues.size() == 1);
    CHECK(plugin->load_report->issues.front().code ==
          Slic3r::PluginPackageLoadErrorCode::DependencyMissing);
    CHECK(plugin->load_report->issues.front().system_error == 126);
}

TEST_CASE_METHOD(PluginUpdaterFunctionalFixture,
                 "PluginUpdater keeps activation diagnostics out of package state",
                 "[plugins][updater][plugin-functional][loader]")
{
    write_plugin_repository();
    write_cached_plugin("1.0.0.0");
    Slic3r::PluginActivationConfig config;
    config.activated["functional.plugin.instance"] = true;
    config.plugin_packages["functional.plugin.instance"] = plugin_id;
    std::string error_message;
    REQUIRE(Slic3r::write_plugin_activation_config(
        Slic3r::plugin_activation_config_path(data_directory), config, error_message));

    // A cache-only package remains installable even when activated.ini still
    // requests one of its plugin ids from an earlier installation.
    updater.reload_all_plugins();
    std::optional<Slic3r::PluginSync> plugin = updater.plugin(plugin_id);
    REQUIRE(plugin.has_value());
    CHECK_FALSE(plugin->is_installed);
    const Slic3r::PluginAvailable *best = plugin->best_available();
    REQUIRE(best != nullptr);
    CHECK_FALSE(best->local_directory.empty());
    CHECK_FALSE(plugin->load_report.has_value());

    Slic3r::Orchestrator &orchestrator = Slic3r::Orchestrator::instance();
    orchestrator.begin_plugin_package_load(plugin_id,
        (data_directory / "plugins" / plugin_id).string());
    Slic3r::PluginPackageLoadIssue issue;
    issue.code = Slic3r::PluginPackageLoadErrorCode::ConfiguredPluginMissing;
    issue.plugin_id = "functional.plugin.instance";
    issue.detail = "The configured plugin id was not registered.";
    orchestrator.report_plugin_package_load_issue(plugin_id, std::move(issue));
    Slic3r::PluginPackageLoadIssue legacy_missing_issue;
    legacy_missing_issue.code = Slic3r::PluginPackageLoadErrorCode::PackageMissing;
    legacy_missing_issue.plugin_id = "functional.plugin.instance";
    legacy_missing_issue.detail = "The configured plugin package is not present.";
    orchestrator.report_plugin_package_load_issue(plugin_id, std::move(legacy_missing_issue));
    orchestrator.finish_plugin_package_load(plugin_id);

    updater.reload_all_plugins();
    plugin = updater.plugin(plugin_id);
    REQUIRE(plugin.has_value());
    CHECK_FALSE(plugin->load_report.has_value());
}

TEST_CASE_METHOD(PluginUpdaterFunctionalFixture,
                 "PluginUpdater removes activation issues from mixed package reports",
                 "[plugins][updater][plugin-functional][loader]")
{
    write_plugin_repository();
    write_cached_plugin("1.0.0.0");
    Slic3r::Orchestrator &orchestrator = Slic3r::Orchestrator::instance();
    orchestrator.begin_plugin_package_load(plugin_id,
        (data_directory / "plugins" / plugin_id).string());

    Slic3r::PluginPackageLoadIssue activation_issue;
    activation_issue.code = Slic3r::PluginPackageLoadErrorCode::ConfiguredPluginMissing;
    activation_issue.plugin_id = "functional.plugin.instance";
    activation_issue.detail = "The configured plugin id was not registered.";
    orchestrator.report_plugin_package_load_issue(plugin_id, std::move(activation_issue));

    Slic3r::PluginPackageLoadIssue package_issue;
    package_issue.code = Slic3r::PluginPackageLoadErrorCode::DependencyMissing;
    package_issue.detail = "The dependent runtime library was not found.";
    package_issue.system_error = 126;
    orchestrator.report_plugin_package_load_issue(plugin_id, std::move(package_issue));
    orchestrator.finish_plugin_package_load(plugin_id);

    updater.reload_all_plugins();
    const std::optional<Slic3r::PluginSync> plugin = updater.plugin(plugin_id);
    REQUIRE(plugin.has_value());
    REQUIRE(plugin->load_report.has_value());
    REQUIRE(plugin->load_report->issues.size() == 1);
    CHECK(plugin->load_report->issues.front().code ==
          Slic3r::PluginPackageLoadErrorCode::DependencyMissing);
}

TEST_CASE_METHOD(PluginUpdaterFunctionalFixture,
                 "PluginUpdater reports a selected installed package with no live directory",
                 "[plugins][updater][plugin-functional][loader]")
{
    write_plugin_repository();
    Slic3r::PluginActivationConfig config;
    config.installed[plugin_id] = {"1.0.0.0", slicer_version};
    std::string error_message;
    REQUIRE(Slic3r::write_plugin_activation_config(
        Slic3r::plugin_activation_config_path(data_directory), config, error_message));

    updater.reload_all_plugins();
    const std::optional<Slic3r::PluginSync> plugin = updater.plugin(plugin_id);
    REQUIRE(plugin.has_value());
    CHECK(plugin->is_installed);
    REQUIRE(plugin->load_report.has_value());
    REQUIRE(plugin->load_report->issues.size() == 1);
    CHECK(plugin->load_report->issues.front().code ==
          Slic3r::PluginPackageLoadErrorCode::PackageMissing);
    CHECK(plugin->load_report->issues.front().plugin_id.empty());
}

TEST_CASE_METHOD(PluginUpdaterFunctionalFixture,
                 "Plugin reconciliation removes packages outside the desired set without deleting their cache",
                 "[plugins][updater][plugin-functional]")
{
    SECTION("an installed package is removed") {
        write_installed_plugin("1.0.0.0");
        const boost::filesystem::path cache_root = write_cached_plugin("1.0.0.0");
        Slic3r::PluginActivationConfig config;
        std::string error_message;
        REQUIRE(Slic3r::write_plugin_activation_config(
            Slic3r::plugin_activation_config_path(data_directory), config, error_message));

        REQUIRE(Slic3r::reconcile_installed_plugin_packages(
            data_directory, config, error_message));
        CHECK_FALSE(boost::filesystem::exists(data_directory / "plugins" / plugin_id));
        CHECK(boost::filesystem::is_directory(cache_root));
    }

    SECTION("an already absent package is already reconciled") {
        const boost::filesystem::path cache_root = write_cached_plugin("1.0.0.0");
        Slic3r::PluginActivationConfig config;
        std::string error_message;
        REQUIRE(Slic3r::write_plugin_activation_config(
            Slic3r::plugin_activation_config_path(data_directory), config, error_message));

        REQUIRE(Slic3r::reconcile_installed_plugin_packages(
            data_directory, config, error_message));
        CHECK(boost::filesystem::is_directory(cache_root));
    }
}

TEST_CASE_METHOD(PluginUpdaterFunctionalFixture,
                 "PluginUpdater schedules a valid cached package without HTTP",
                 "[plugins][updater][plugin-functional]")
{
    write_plugin_repository();
    write_cached_plugin("2.0.0.0");
    Slic3r::PluginActivationConfig activation_config;
    std::string config_error;
    REQUIRE(Slic3r::write_plugin_activation_config(
        Slic3r::plugin_activation_config_path(data_directory), activation_config, config_error));
    updater.reload_all_plugins();

    Slic3r::PluginAvailable version;
    version.package_version = "2.0.0.0";
    version.slicer_version = slicer_version;
    version.url_zip = "https://example.invalid/must-not-download.zip";
    std::optional<Slic3r::UpdaterError> install_result;
    updater.install_plugin(plugin_id, version, [&install_result](Slic3r::UpdaterError error) {
        install_result = std::move(error);
    });
    updater.wait_for_pending_operations();

    REQUIRE(install_result.has_value());
    CHECK(install_result->succeeded());
    CHECK(http.pending_count() == 0);
    CHECK(http.sync_request_count() == 0);
    const Slic3r::PluginActivationConfig config = read_activation_config();
    REQUIRE(config.installed.count(plugin_id) == 1);
    CHECK(config.installed.at(plugin_id).package_version == "2.0.0.0");
}

TEST_CASE_METHOD(PluginUpdaterFunctionalFixture,
                 "PluginUpdater does not leave an unusable install request after clearing its cache",
                 "[plugins][updater][plugin-functional][clear-cache]")
{
    write_plugin_repository();
    write_cached_plugin("2.0.0.0");
    updater.reload_all_plugins();

    const std::optional<Slic3r::PluginSync> plugin = updater.plugin(plugin_id);
    REQUIRE(plugin.has_value());
    const std::vector<Slic3r::PluginAvailable>::const_iterator selected = std::find_if(
        plugin->available_packages.begin(), plugin->available_packages.end(),
        [](const Slic3r::PluginAvailable &version) { return version.package_version == "2.0.0.0"; });
    REQUIRE(selected != plugin->available_packages.end());

    // The Install button records a request for the next startup because a
    // loaded plugin cannot be replaced safely in the current process.
    std::optional<Slic3r::UpdaterError> install_result;
    updater.install_plugin(plugin_id, *selected, [&install_result](Slic3r::UpdaterError error) {
        install_result = std::move(error);
    });
    updater.wait_for_pending_operations();
    REQUIRE(install_result.has_value());
    REQUIRE(install_result->succeeded());
    REQUIRE(read_activation_config().installed.count(plugin_id) == 1);

    // Reproduce the second GUI action before restarting. A successful clear
    // must not leave an installation request that refers to the deleted cache.
    std::optional<Slic3r::UpdaterError> clear_result;
    updater.clear_cache_plugin(plugin_id, [&clear_result](Slic3r::UpdaterError error) {
        clear_result = std::move(error);
    });
    updater.wait_for_pending_operations();
    REQUIRE(clear_result.has_value());
    REQUIRE(clear_result->succeeded());
    CHECK_FALSE(updater.plugin(plugin_id).has_value());
    const std::vector<std::string> remaining_plugin_ids = updater.plugin_ids();
    CHECK(std::find(remaining_plugin_ids.begin(), remaining_plugin_ids.end(), plugin_id) ==
          remaining_plugin_ids.end());

    const Slic3r::PluginActivationConfig config = read_activation_config();
    CHECK(config.installed.count(plugin_id) == 0);
    std::string cache_error;
    const bool requested_package_is_cached = Slic3r::plugin_package_cache_is_valid(
        data_directory, plugin_id, {"2.0.0.0", slicer_version}, cache_error);
    CHECK_FALSE(requested_package_is_cached);

    // Exercise the next-startup stage as well. Clearing the cache must leave a
    // self-contained activation file which does not refer to deleted content.
    std::string startup_error;
    const bool startup_succeeded = Slic3r::reconcile_installed_plugin_packages(
        data_directory, config, startup_error);
    INFO(startup_error);
    CHECK(startup_succeeded);
}

TEST_CASE_METHOD(PluginUpdaterFunctionalFixture,
                 "PluginUpdater keeps a cleared bundled plugin absent until startup preparation",
                 "[plugins][updater][plugin-functional][clear-cache]")
{
    write_bundled_plugin("1.0.0.0");
    std::string error_message;
    REQUIRE(Slic3r::prepare_plugin_bundle_cache(resources_directory, data_directory, error_message));
    updater.reload_all_plugins();
    const std::optional<Slic3r::PluginSync> bundled_plugin = updater.plugin(plugin_id);
    REQUIRE(bundled_plugin.has_value());
    CHECK(bundled_plugin->has_cache);

    std::optional<Slic3r::UpdaterError> clear_result;
    updater.clear_cache_plugin(plugin_id, [&clear_result](Slic3r::UpdaterError error) {
        clear_result = std::move(error);
    });
    updater.wait_for_pending_operations();
    REQUIRE(clear_result.has_value());
    REQUIRE(clear_result->succeeded());
    CHECK_FALSE(updater.plugin(plugin_id).has_value());
    CHECK_FALSE(boost::filesystem::exists(Slic3r::repository_cache_root_path(
        data_directory, Slic3r::RepositoryPackageType::Plugin, plugin_id)));

    // Reload and unrelated runtime imports prepare the cache layout but must
    // not republish archives shipped in resources/plugins.
    updater.reload_all_plugins();
    CHECK_FALSE(updater.plugin(plugin_id).has_value());
    const boost::filesystem::path other_package = temporary.path() / "another.plugin";
    write_test_file(other_package / plugin_library_filename(), "another plugin");
    std::optional<Slic3r::UpdaterError> import_result;
    updater.cache_plugin_directory(other_package, [&import_result](Slic3r::UpdaterError error) {
        import_result = std::move(error);
    });
    updater.wait_for_pending_operations();
    REQUIRE(import_result.has_value());
    REQUIRE(import_result->succeeded());
    updater.reload_all_plugins();
    CHECK_FALSE(updater.plugin(plugin_id).has_value());
    CHECK(updater.plugin("another.plugin").has_value());

    // The PluginLoader performs this startup-only preparation, making bundled
    // packages available again in the next application process.
    REQUIRE(Slic3r::prepare_plugin_bundle_cache(resources_directory, data_directory, error_message));
    updater.reload_all_plugins();
    REQUIRE(updater.plugin(plugin_id).has_value());
}

TEST_CASE_METHOD(PluginUpdaterFunctionalFixture,
                 "PluginUpdater refuses cache removal while synchronization is active",
                 "[plugins][updater][plugin-functional][clear-cache]")
{
    write_plugin_repository();
    write_cached_plugin("1.0.0.0");
    updater.reload_all_plugins();

    std::optional<int> sync_result;
    updater.sync_async([&sync_result](int count) { sync_result = count; }, true);
    REQUIRE(http.pending_count() == 1);

    std::optional<Slic3r::UpdaterError> clear_result;
    updater.clear_cache_plugin(plugin_id, [&clear_result](Slic3r::UpdaterError error) {
        clear_result = std::move(error);
    });
    REQUIRE(clear_result.has_value());
    CHECK(clear_result->code == Slic3r::UpdaterError::Code::PreparationRejected);
    CHECK(updater.plugin(plugin_id).has_value());
    CHECK(boost::filesystem::exists(Slic3r::repository_cache_root_path(
        data_directory, Slic3r::RepositoryPackageType::Plugin, plugin_id)));

    http.succeed_front("[]", 200);
    REQUIRE(sync_result.has_value());
}

TEST_CASE_METHOD(PluginUpdaterFunctionalFixture,
                 "PluginUpdater distinguishes current and newer installed plugin versions",
                 "[plugins][updater][plugin-functional]")
{
    write_installed_plugin("1.0.0.0");
    write_plugin_repository();
    updater.reload_all_plugins();

    synchronize({{"1.0.0.0", slicer_version, "https://example.invalid/plugin-1.zip"}});
    std::optional<Slic3r::PluginSync> plugin = updater.plugin(plugin_id);
    REQUIRE(plugin.has_value());
    CHECK(plugin->is_installed);
    CHECK(plugin->sync_state == Slic3r::RepositorySyncState::Succeeded);
    CHECK_FALSE(plugin->can_upgrade);

    write_cached_plugin("2.0.0.0");
    updater.reload_all_plugins();
    synchronize({{"2.0.0.0", slicer_version, "https://example.invalid/plugin-2.zip"}});
    plugin = updater.plugin(plugin_id);
    REQUIRE(plugin.has_value());
    const Slic3r::PluginAvailable *best = plugin->best_available();
    REQUIRE(best != nullptr);
    CHECK(best->package_version == "2.0.0.0");
    CHECK(plugin->can_upgrade);
}

TEST_CASE_METHOD(PluginUpdaterFunctionalFixture,
                 "PluginUpdater restores the live version when a pending update cache is cleared",
                 "[plugins][updater][plugin-functional][clear-cache]")
{
    write_installed_plugin("1.0.0.0");
    write_plugin_repository();
    write_cached_plugin("2.0.0.0");
    updater.reload_all_plugins();

    std::optional<Slic3r::PluginSync> plugin = updater.plugin(plugin_id);
    REQUIRE(plugin.has_value());
    const std::vector<Slic3r::PluginAvailable>::const_iterator update = std::find_if(
        plugin->available_packages.begin(), plugin->available_packages.end(),
        [](const Slic3r::PluginAvailable &version) { return version.package_version == "2.0.0.0"; });
    REQUIRE(update != plugin->available_packages.end());

    std::optional<Slic3r::UpdaterError> install_result;
    updater.install_plugin(plugin_id, *update, [&install_result](Slic3r::UpdaterError error) {
        install_result = std::move(error);
    });
    updater.wait_for_pending_operations();
    REQUIRE(install_result.has_value());
    REQUIRE(install_result->succeeded());
    REQUIRE(read_activation_config().installed.at(plugin_id).package_version == "2.0.0.0");

    std::optional<Slic3r::UpdaterError> clear_result;
    updater.clear_cache_plugin(plugin_id, [&clear_result](Slic3r::UpdaterError error) {
        clear_result = std::move(error);
    });
    updater.wait_for_pending_operations();
    REQUIRE(clear_result.has_value());
    REQUIRE(clear_result->succeeded());

    const Slic3r::PluginActivationConfig config = read_activation_config();
    REQUIRE(config.installed.count(plugin_id) == 1);
    CHECK(config.installed.at(plugin_id).package_version == "1.0.0.0");
    std::string cache_error;
    CHECK(Slic3r::plugin_package_cache_is_valid(
        data_directory, plugin_id, {"1.0.0.0", slicer_version}, cache_error));
    CHECK_FALSE(Slic3r::plugin_package_cache_is_valid(
        data_directory, plugin_id, {"2.0.0.0", slicer_version}, cache_error));
    plugin = updater.plugin(plugin_id);
    REQUIRE(plugin.has_value());
    CHECK(plugin->is_installed);
    CHECK(plugin->installed_version.package_version == "1.0.0.0");

    // The current live version remains a valid startup source, while the
    // downloaded update selected before Clear cache has disappeared.
    std::string startup_error;
    INFO(startup_error);
    CHECK(Slic3r::reconcile_installed_plugin_packages(
        data_directory, config, startup_error));
}

TEST_CASE_METHOD(PluginUpdaterFunctionalFixture,
                 "PluginUpdater imports an unpacked local package",
                 "[plugins][updater][plugin-functional]")
{
    const boost::filesystem::path package = temporary.path() / "local_only_plugin";
    write_test_file(package / plugin_library_filename(), "local library");

    std::optional<Slic3r::UpdaterError> import_error;
    updater.cache_plugin_directory(package, [&import_error](Slic3r::UpdaterError error) {
        import_error = std::move(error);
    });
    updater.wait_for_pending_operations();
    REQUIRE(import_error.has_value());
    REQUIRE(import_error->succeeded());

    const std::optional<Slic3r::PluginSync> plugin = updater.plugin("local_only_plugin");
    REQUIRE(plugin.has_value());
    CHECK(plugin->description.config_update_rest.empty());
    REQUIRE(plugin->available_packages.size() == 1);
    CHECK(plugin->available_packages.front().package_version == "1.0.0.0");
    CHECK(plugin->available_packages.front().slicer_version == "1.0.0.0");
    CHECK_FALSE(plugin->available_packages.front().local_directory.empty());
    CHECK(http.pending_count() == 0);
}

TEST_CASE("PluginUpdater lists and manages Python runtime infrastructure",
          "[plugins][updater][python]")
{
    TemporaryDirectory temporary;
    const boost::filesystem::path resources_directory = temporary.path() / "resources";
    const boost::filesystem::path data_directory = temporary.path() / "data";
    ScopedUpdaterDirectories directories(resources_directory, data_directory);
    write_test_file(resources_directory / "plugins" / "default_activated.ini",
                    "[installed]\n[activated]\n");

    Slic3r::RepositoryPackageCache cache(data_directory, Slic3r::plugin_repository_cache_adapter());
    bool purged = false;
    std::string error_message;
    REQUIRE(cache.prepare_layout(purged, error_message));

    // Both packages use the normal package cache. The internal marker allows
    // runtime infrastructure to register no plugin id, but does not change
    // whether users can install or remove its package.
    const boost::filesystem::path visible_package = temporary.path() / "python.visible";
    write_test_file(visible_package / "description.ini",
                    "[plugin]\nid = python.visible\nname = Visible Python plugin\ninternal = 0\n");
    write_test_file(visible_package / "version.ini",
                    "[plugin]\npackage_version = 1.0.0\nslicer_version = 2.7.0.0\n[abi]\nslic3r_plugin_types.h = 1.0\n");
    write_test_file(visible_package / "plugin.py", "def register_plugin(api):\n    return None\n");
    Slic3r::RepositoryCachedVersion visible_cached;
    REQUIRE(cache.cache_simple(visible_package, visible_cached, error_message));

    const boost::filesystem::path internal_package = temporary.path() / "python";
    write_test_file(internal_package / "description.ini",
                    "[plugin]\nid = python\nname = Python runtime\ninternal = 1\n");
    write_test_file(internal_package / "version.ini",
                    "[plugin]\npackage_version = 1.0.0\nslicer_version = 2.7.0.0\n[abi]\nslic3r_plugin_types.h = 1.0\n");
    write_test_file(internal_package / "plugin.py", "def register_plugin(api):\n    return None\n");
    Slic3r::RepositoryCachedVersion internal_cached;
    REQUIRE(cache.cache_simple(internal_package, internal_cached, error_message));
    CHECK(internal_cached.description.is_internal);

    FakeUpdaterHttpTransport http;
    Slic3r::PluginUpdater updater(http);
    updater.reload_all_plugins();
    const std::vector<std::string> plugin_ids = updater.plugin_ids();
    CHECK(std::find(plugin_ids.begin(), plugin_ids.end(), "python.visible") != plugin_ids.end());
    CHECK(std::find(plugin_ids.begin(), plugin_ids.end(), "python") != plugin_ids.end());

    std::optional<Slic3r::PluginSync> runtime = updater.plugin("python");
    REQUIRE(runtime.has_value());
    const Slic3r::PluginAvailable *runtime_best = runtime->best_available();
    REQUIRE(runtime_best != nullptr);
    std::optional<Slic3r::UpdaterError> install_result;
    updater.install_plugin("python", *runtime_best, [&install_result](Slic3r::UpdaterError error) {
        install_result = std::move(error);
    });
    updater.wait_for_pending_operations();
    REQUIRE(install_result.has_value());
    REQUIRE(install_result->succeeded());
    runtime = updater.plugin("python");
    REQUIRE(runtime.has_value());
    CHECK(runtime->is_installed);

    // Package installation must not create a fake activatable plugin id. The
    // runtime remains absent from Plugin configuration because that dialog
    // enumerates registered plugin instances, not package installation rows.
    Slic3r::PluginActivationConfig activation;
    REQUIRE(Slic3r::read_plugin_activation_config(
        Slic3r::plugin_activation_config_path(data_directory), activation, error_message));
    CHECK(activation.installed.count("python") == 1);
    CHECK(activation.activated.count("python") == 0);
    CHECK(activation.plugin_packages.count("python") == 0);

    std::optional<Slic3r::UpdaterError> uninstall_result;
    updater.uninstall_plugin("python", [&uninstall_result](Slic3r::UpdaterError error) {
        uninstall_result = std::move(error);
    });
    updater.wait_for_pending_operations();
    REQUIRE(uninstall_result.has_value());
    REQUIRE(uninstall_result->succeeded());
    runtime = updater.plugin("python");
    REQUIRE(runtime.has_value());
    CHECK_FALSE(runtime->is_installed);
    CHECK(runtime->has_cache);

    REQUIRE(Slic3r::read_plugin_activation_config(
        Slic3r::plugin_activation_config_path(data_directory), activation, error_message));
    CHECK(activation.installed.count("python") == 0);
    CHECK(activation.activated.count("python") == 0);
    CHECK(activation.plugin_packages.count("python") == 0);
}

TEST_CASE_METHOD(PluginUpdaterFunctionalFixture,
                 "PluginUpdater reuses recent changelogs and refreshes stale files",
                 "[plugins][updater][plugin-functional]")
{
    write_plugin_repository();
    updater.reload_all_plugins();
    synchronize({{"1.0.0.0", slicer_version, "https://example.invalid/plugin-1.zip"}});

    std::optional<Slic3r::PluginSync> plugin = updater.plugin(plugin_id);
    REQUIRE(plugin.has_value());
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
    plugin = updater.plugin(plugin_id);
    REQUIRE(plugin.has_value());
    CHECK(plugin->available_packages.front().notes == "cached notes");

    boost::filesystem::last_write_time(cache_file, std::time(nullptr) - 24 * 3600 - 1);
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
    plugin = updater.plugin(plugin_id);
    REQUIRE(plugin.has_value());
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

TEST_CASE("Updater worker contains operation and completion exceptions",
          "[plugins][updater][exceptions]")
{
    Slic3r::UpdaterOperationExecutor executor;
    std::vector<Slic3r::UpdaterError> results;

    REQUIRE(executor.enqueue(
        []() -> Slic3r::UpdaterError { throw std::runtime_error("worker failure"); },
        [&results](Slic3r::UpdaterError error) { results.emplace_back(std::move(error)); }));
    REQUIRE(executor.enqueue(
        []() -> Slic3r::UpdaterError { throw 42; },
        [&results](Slic3r::UpdaterError error) { results.emplace_back(std::move(error)); }));

    // A bad terminal callback must be contained by the worker boundary. The
    // following queued operation proves that the worker continues afterwards.
    REQUIRE(executor.enqueue(
        [] { return Slic3r::UpdaterError(); },
        [](Slic3r::UpdaterError) { throw std::runtime_error("completion failure"); }));
    REQUIRE(executor.enqueue(
        [] { return Slic3r::UpdaterError(); },
        [&results](Slic3r::UpdaterError error) { results.emplace_back(std::move(error)); }));

    executor.wait_until_idle();
    REQUIRE(results.size() == 3);
    CHECK(results[0].code == Slic3r::UpdaterError::Code::Unexpected);
    CHECK(results[0].detail.find("worker failure") != std::string::npos);
    CHECK(results[1].code == Slic3r::UpdaterError::Code::Unexpected);
    CHECK(results[1].detail == "Unknown updater exception.");
    CHECK(results[2].succeeded());
}

TEST_CASE("Updater worker shutdown waits for retained asynchronous operations",
          "[plugins][updater][lifetime]")
{
    Slic3r::UpdaterOperationExecutor executor;
    CHECK(executor.can_shutdown_from_current_thread());
    Slic3r::UpdaterOperationExecutor::AsyncOperation pending = executor.retain_async_operation();
    REQUIRE(pending);

    std::promise<void> shutdown_completed;
    std::future<void> shutdown_completed_future = shutdown_completed.get_future();
    std::promise<void> shutdown_started;
    std::future<void> shutdown_started_future = shutdown_started.get_future();
    std::thread shutdown_thread([&executor, &shutdown_completed, &shutdown_started] {
        shutdown_started.set_value();
        executor.shutdown_and_wait();
        shutdown_completed.set_value();
    });

    // Shutdown must not return while an external callback can still reference
    // the updater which owns this executor.
    shutdown_started_future.wait();
    CHECK(shutdown_completed_future.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
    pending.reset();
    CHECK(shutdown_completed_future.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    shutdown_thread.join();
    CHECK_FALSE(executor.retain_async_operation());
}

TEST_CASE("Updater worker identifies shutdown contexts which would wait on themselves",
          "[plugins][updater][lifetime]")
{
    Slic3r::UpdaterOperationExecutor executor;
    CHECK(executor.can_shutdown_from_current_thread());

    std::promise<bool> worker_result;
    std::future<bool> worker_result_future = worker_result.get_future();
    REQUIRE(executor.enqueue(
        [&executor, &worker_result] {
            worker_result.set_value(executor.can_shutdown_from_current_thread());
            return Slic3r::UpdaterError();
        },
        [](Slic3r::UpdaterError) {}));
    CHECK_FALSE(worker_result_future.get());
    executor.wait_until_idle();

    Slic3r::UpdaterOperationExecutor::AsyncOperation pending = executor.retain_async_operation();
    REQUIRE(pending);
    bool callback_context_is_safe = true;
    pending->run_callback([&executor, &callback_context_is_safe] {
        callback_context_is_safe = executor.can_shutdown_from_current_thread();
    });
    CHECK_FALSE(callback_context_is_safe);
    pending.reset();
    CHECK(executor.can_shutdown_from_current_thread());
}

TEST_CASE("Updater idle waits do not wait for external callback tokens",
          "[plugins][updater][lifetime]")
{
    Slic3r::UpdaterOperationExecutor executor;
    Slic3r::UpdaterOperationExecutor::AsyncOperation pending = executor.retain_async_operation();
    REQUIRE(pending);

    std::promise<void> idle_completed;
    std::future<void> idle_completed_future = idle_completed.get_future();
    std::thread idle_thread([&executor, &idle_completed] {
        executor.wait_until_idle();
        idle_completed.set_value();
    });

    CHECK(idle_completed_future.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    idle_thread.join();
    pending.reset();
}

TEST_CASE("RepositoryUpdater destruction waits for a pending HTTP callback",
          "[plugins][updater][lifetime]")
{
    FakeUpdaterHttpTransport http;
    TemporaryDirectory temporary;
    const boost::filesystem::path destination = temporary.path() / "late-response.zip";
    std::optional<Slic3r::UpdaterError> result;
    std::unique_ptr<TestRepositoryUpdater> updater = std::make_unique<TestRepositoryUpdater>(http);

    updater->download_repository_file_async(
        "https://example.invalid/late-response.zip", destination, 4096,
        [&result](Slic3r::UpdaterError error) { result = std::move(error); });
    REQUIRE(http.pending_count() == 1);

    std::promise<void> destruction_completed;
    std::future<void> destruction_completed_future = destruction_completed.get_future();
    std::promise<void> destruction_started;
    std::future<void> destruction_started_future = destruction_started.get_future();
    std::thread destruction_thread(
        [owned = std::move(updater), &destruction_completed, &destruction_started]() mutable {
            destruction_started.set_value();
            owned.reset();
            destruction_completed.set_value();
        });

    // The updater stays alive until the delayed transport callback has either
    // submitted its work or observed that shutdown rejects new worker tasks.
    destruction_started_future.wait();
    CHECK(destruction_completed_future.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
    REQUIRE_NOTHROW(http.succeed_front("late response", 200));
    CHECK(destruction_completed_future.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    destruction_thread.join();
    REQUIRE(result.has_value());
}

TEST_CASE("RepositoryUpdater destruction keeps changelog service state alive",
          "[plugins][updater][lifetime]")
{
    FakeUpdaterHttpTransport http;
    TemporaryDirectory temporary;
    bool callback_succeeded = false;
    std::unique_ptr<TestRepositoryUpdater> updater = std::make_unique<TestRepositoryUpdater>(http);

    TestRepositoryUpdater::RepositoryChangelogRequest request;
    request.cache_file = temporary.path() / "logs" / "late.json";
    request.url = "https://example.invalid/commit/late";
    request.kind = TestRepositoryUpdater::RepositoryChangelogKind::Commit;
    request.store_notes = [](std::string) {};
    updater->download_repository_changelogs(
        {request}, [&callback_succeeded](bool succeeded) { callback_succeeded = succeeded; }, false);
    REQUIRE(http.pending_count() == 1);

    std::promise<void> destruction_completed;
    std::future<void> destruction_completed_future = destruction_completed.get_future();
    std::promise<void> destruction_started;
    std::future<void> destruction_started_future = destruction_started.get_future();
    std::thread destruction_thread(
        [owned = std::move(updater), &destruction_completed, &destruction_started]() mutable {
            destruction_started.set_value();
            owned.reset();
            destruction_completed.set_value();
        });

    // RepositoryChangelogState contains a pointer into its service. The async
    // token keeps that service alive until the batch callback releases it.
    destruction_started_future.wait();
    CHECK(destruction_completed_future.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
    REQUIRE_NOTHROW(http.succeed_front(R"({"commit":{"message":"late notes"}})", 200));
    CHECK(destruction_completed_future.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    destruction_thread.join();
    CHECK(callback_succeeded);
}

TEST_CASE("Updater HTTP contains callback exceptions", "[plugins][updater][exceptions]")
{
    FakeUpdaterHttpTransport http;

    SECTION("a complete exception is delivered once to the error callback") {
        int error_count = 0;
        std::string diagnostic;
        http.get("https://example.invalid/complete")
            .on_complete([](std::string, unsigned) { throw std::runtime_error("complete failure"); })
            .on_error([&error_count, &diagnostic](std::string, std::string error, unsigned) {
                ++error_count;
                diagnostic = std::move(error);
            })
            .perform();

        REQUIRE_NOTHROW(http.succeed_front("body", 200));
        CHECK(error_count == 1);
        CHECK(diagnostic.find("complete failure") != std::string::npos);
    }

    SECTION("a progress exception cancels and consumes the terminal callbacks") {
        int error_count = 0;
        http.get("https://example.invalid/progress")
            .on_complete([](std::string, unsigned) { FAIL("The completed request was already cancelled."); })
            .on_error([&error_count](std::string, std::string, unsigned) { ++error_count; })
            .on_progress([](Slic3r::UpdaterHttpRequest::Progress, bool &) {
                throw std::runtime_error("progress failure");
            })
            .perform();

        CHECK(http.progress_front(10, 5, "partial"));
        CHECK(error_count == 1);
        REQUIRE_NOTHROW(http.succeed_front(std::string(), 200));
        CHECK(error_count == 1);
    }

    SECTION("an error callback exception never escapes the transport") {
        http.get("https://example.invalid/error")
            .on_error([](std::string, std::string, unsigned) {
                throw std::runtime_error("error callback failure");
            })
            .perform();

        REQUIRE_NOTHROW(http.fail_front(std::string(), "network failure", 0));
        CHECK(http.pending_count() == 0);
    }
}

TEST_CASE("Repository sync contains terminal callback exceptions",
          "[plugins][updater][exceptions]")
{
    FakeUpdaterHttpTransport http;
    TestRepositoryUpdater updater(http);
    int following_callback_count = 0;

    REQUIRE(updater.begin_sync(1, [](int) {
        throw std::runtime_error("sync callback failure");
    }));
    CHECK_FALSE(updater.begin_sync(1, [&following_callback_count](int count) {
        CHECK(count == 7);
        ++following_callback_count;
    }));

    REQUIRE_NOTHROW(updater.finish_sync());
    CHECK(following_callback_count == 1);

    // Completing the previous batch must release the shared sync state so a
    // later request starts normally after the throwing subscriber.
    int later_callback_count = 0;
    REQUIRE(updater.begin_sync(0, [&later_callback_count](int) { ++later_callback_count; }));
    CHECK(later_callback_count == 1);
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

    // vendors() is the detached model read by the selection dialog. Its best
    // entry is calculated from the copied vector, so the snapshot is autonomous.
    const Slic3r::VendorSync &dialog_vendor = dialog.vendors.front();
    CHECK_FALSE(dialog_vendor.is_installed);
    const Slic3r::VendorAvailable *dialog_best = dialog_vendor.best_available();
    REQUIRE(dialog_best != nullptr);
    CHECK(dialog_best->config_version.to_string() == "1.0.0.0");
    CHECK_FALSE(dialog_best->local_file.empty());

    std::optional<Slic3r::UpdaterError> install_result;
    updater.install_vendor(vendor_id, *dialog_best,
                           [&install_result](Slic3r::UpdaterError error) {
                               install_result = std::move(error);
                           });
    updater.wait_for_pending_operations();

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
    CHECK(host.rollback_tokens.empty());
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
    std::optional<Slic3r::UpdaterError> import_result;
    updater.cache_vendor_archive(archive_path, [&import_result](Slic3r::UpdaterError error) {
        import_result = std::move(error);
    });
    updater.wait_for_pending_operations();
    REQUIRE(import_result.has_value());
    INFO("Updater error code: " << static_cast<int>(import_result->code));
    INFO("Updater error detail: " << import_result->detail);
    REQUIRE(import_result->succeeded());

    const boost::filesystem::path package_root = Slic3r::repository_package_cache_path(
        data_directory, Slic3r::RepositoryPackageType::Vendor, vendor_id,
        config_version, slicer_version);
    const boost::filesystem::path cached_profile = package_root / "profiles" / (vendor_id + ".ini");
    REQUIRE(boost::filesystem::is_regular_file(cached_profile));
    CHECK(package_root.filename() == config_version + "=" + slicer_version);
    CHECK_FALSE(boost::filesystem::exists(
        data_directory / "cache" / "vendor" / archive_path.stem()));

    updater.reload_all_vendors();
    const std::optional<Slic3r::VendorSync> vendor = updater.vendor(vendor_id);
    REQUIRE(vendor.has_value());
    CHECK_FALSE(vendor->is_installed);
    const Slic3r::VendorAvailable *best = vendor->best_available();
    REQUIRE(best != nullptr);
    CHECK(best->config_version.to_string() == config_version);
    CHECK(best->slicer_version.to_string() == slicer_version);
    CHECK(boost::filesystem::equivalent(best->local_file, cached_profile));
    CHECK(updater.count_available() == 1);
}

TEST_CASE_METHOD(PresetUpdaterFunctionalFixture,
                 "PresetUpdater replaces a vendor INI loaded twice from the dialog",
                 "[plugins][updater][preset-functional]")
{
    const boost::filesystem::path local_profile = temporary.path() / (vendor_id + ".ini");
    write_test_file(local_profile, vendor_profile_contents(vendor_id, "1.0.0.0", slicer_version));

    std::optional<Slic3r::UpdaterError> first_import;
    updater.cache_vendor_ini(local_profile, [&first_import](Slic3r::UpdaterError error) {
        first_import = std::move(error);
    });
    updater.wait_for_pending_operations();
    REQUIRE(first_import.has_value());
    INFO("First import error: " << first_import->detail);
    REQUIRE(first_import->succeeded());

    // Loading the same vendor again must overwrite its cached INI. Changing
    // the source version proves that the second call did not merely ignore it.
    write_test_file(local_profile, vendor_profile_contents(vendor_id, "2.0.0.0", slicer_version));
    std::optional<Slic3r::UpdaterError> second_import;
    updater.cache_vendor_ini(local_profile, [&second_import](Slic3r::UpdaterError error) {
        second_import = std::move(error);
    });
    updater.wait_for_pending_operations();
    REQUIRE(second_import.has_value());
    INFO("Second import error: " << second_import->detail);
    REQUIRE(second_import->succeeded());

    const boost::filesystem::path cached_profile = Slic3r::repository_package_cache_path(
        data_directory, Slic3r::RepositoryPackageType::Vendor, vendor_id,
        "2.0.0.0", slicer_version) / "profiles" / (vendor_id + ".ini");
    REQUIRE(boost::filesystem::is_regular_file(cached_profile));
    CHECK(Slic3r::VendorProfile::from_ini(cached_profile, true).config_version.to_string() == "2.0.0.0");

    updater.reload_all_vendors();
    const std::optional<Slic3r::VendorSync> vendor = updater.vendor(vendor_id);
    REQUIRE(vendor.has_value());
    const Slic3r::VendorAvailable *best = vendor->best_available();
    REQUIRE(best != nullptr);
    CHECK(best->config_version.to_string() == "2.0.0.0");
    CHECK(vendor->available_profiles.size() == 2);
}

TEST_CASE_METHOD(PresetUpdaterFunctionalFixture,
                 "PresetUpdater functional dialog changes an installed vendor to a selected local version",
                 "[plugins][updater][preset-functional]")
{
    write_installed_vendor("2.0.0.0");
    write_test_file(data_directory / "vendor" / vendor_id / "icons" / "old.svg", "old icon");
    write_resource_vendor("1.5.0.0");
    write_test_file(resources_directory / "profiles" / vendor_id / "icons" / "new.svg", "new icon");
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
    updater.wait_for_pending_operations();

    REQUIRE(change_result.has_value());
    CHECK(change_result->succeeded());
    const boost::filesystem::path installed_file = data_directory / "vendor" / (vendor_id + ".ini");
    REQUIRE(boost::filesystem::is_regular_file(installed_file));
    CHECK(Slic3r::VendorProfile::from_ini(installed_file, true).config_version.to_string() == "1.5.0.0");
    CHECK(read_test_file(data_directory / "vendor" / vendor_id / "icons" / "new.svg") == "new icon");
    CHECK_FALSE(boost::filesystem::exists(data_directory / "vendor" / vendor_id / "icons" / "old.svg"));
    CHECK(http.sync_request_count() == 0);
    REQUIRE(host.completed_changes.size() == 1);
    CHECK(host.completed_changes.front().change == Slic3r::VendorChange::Install);
}

TEST_CASE_METHOD(PresetUpdaterFunctionalFixture,
                 "PresetUpdater restores vendor files and resources when publication fails",
                 "[plugins][updater][preset-functional][rollback]")
{
    write_installed_vendor("1.0.0.0");
    write_test_file(data_directory / "vendor" / vendor_id / "icons" / "old.svg", "old icon");
    write_resource_vendor("2.0.0.0");
    updater.reload_all_vendors();

    const std::optional<Slic3r::VendorSync> before = updater.vendor(vendor_id);
    REQUIRE(before.has_value());
    const Slic3r::VendorAvailable *best = before->best_available();
    REQUIRE(best != nullptr);
    Slic3r::VendorAvailable broken = *best;
    REQUIRE_FALSE(broken.local_file.empty());

    // Corrupt the already selected cache source after discovery. Publication
    // copies it and its replacement resources before profile parsing fails,
    // which exercises rollback after the live tree was partially changed.
    write_test_file(broken.local_file, "this is not a vendor profile");
    const boost::filesystem::path broken_resources =
        boost::filesystem::path(broken.local_file).parent_path() / vendor_id;
    write_test_file(broken_resources / "icons" / "new.svg", "new icon");

    std::optional<Slic3r::UpdaterError> result;
    updater.install_vendor(vendor_id, broken, [&result](Slic3r::UpdaterError error) {
        result = std::move(error);
    });
    updater.wait_for_pending_operations();

    REQUIRE(result.has_value());
    CHECK_FALSE(result->succeeded());
    REQUIRE(host.rollback_tokens.size() == 1);
    CHECK(host.completed_changes.empty());
    const boost::filesystem::path installed_profile = data_directory / "vendor" / (vendor_id + ".ini");
    REQUIRE(boost::filesystem::is_regular_file(installed_profile));
    CHECK(Slic3r::VendorProfile::from_ini(installed_profile, true).config_version.to_string() == "1.0.0.0");
    CHECK(read_test_file(data_directory / "vendor" / vendor_id / "icons" / "old.svg") == "old icon");
    CHECK_FALSE(boost::filesystem::exists(data_directory / "vendor" / vendor_id / "icons" / "new.svg"));

    const std::optional<Slic3r::VendorSync> after = updater.vendor(vendor_id);
    REQUIRE(after.has_value());
    CHECK(after->profile.config_version.to_string() == "1.0.0.0");
}

TEST_CASE_METHOD(PresetUpdaterFunctionalFixture,
                 "PresetUpdater removes obsolete vendor resources when the selected version has none",
                 "[plugins][updater][preset-functional][transaction]")
{
    write_installed_vendor("1.0.0.0");
    write_test_file(data_directory / "vendor" / vendor_id / "icons" / "old.svg", "old icon");
    write_resource_vendor("2.0.0.0");
    updater.reload_all_vendors();

    const std::optional<Slic3r::VendorSync> vendor = updater.vendor(vendor_id);
    REQUIRE(vendor.has_value());
    const Slic3r::VendorAvailable *best = vendor->best_available();
    REQUIRE(best != nullptr);

    std::optional<Slic3r::UpdaterError> result;
    updater.install_vendor(vendor_id, *best, [&result](Slic3r::UpdaterError error) {
        result = std::move(error);
    });
    updater.wait_for_pending_operations();

    REQUIRE(result.has_value());
    CHECK(result->succeeded());
    CHECK_FALSE(boost::filesystem::exists(data_directory / "vendor" / vendor_id));
    CHECK(Slic3r::VendorProfile::from_ini(
        data_directory / "vendor" / (vendor_id + ".ini"), true).config_version.to_string() == "2.0.0.0");
}

TEST_CASE_METHOD(PresetUpdaterFunctionalFixture,
                 "PresetUpdater reports both publication and snapshot restore failures",
                 "[plugins][updater][preset-functional][rollback]")
{
    write_installed_vendor("1.0.0.0");
    write_resource_vendor("2.0.0.0");
    updater.reload_all_vendors();

    const std::optional<Slic3r::VendorSync> vendor = updater.vendor(vendor_id);
    REQUIRE(vendor.has_value());
    const Slic3r::VendorAvailable *best = vendor->best_available();
    REQUIRE(best != nullptr);
    Slic3r::VendorAvailable broken = *best;
    write_test_file(broken.local_file, "this is not a vendor profile");
    host.rollback_succeeds = false;

    std::optional<Slic3r::UpdaterError> result;
    updater.install_vendor(vendor_id, broken, [&result](Slic3r::UpdaterError error) {
        result = std::move(error);
    });
    updater.wait_for_pending_operations();

    REQUIRE(result.has_value());
    CHECK_FALSE(result->succeeded());
    CHECK(result->detail.find("failed to restore") != std::string::npos);
    CHECK(result->detail.find("fake snapshot restore failed") != std::string::npos);
    REQUIRE(host.rollback_tokens.size() == 1);
    CHECK(host.completed_changes.empty());
}

TEST_CASE_METHOD(PresetUpdaterFunctionalFixture,
                 "PresetUpdater rolls back a complete install batch after one publication fails",
                 "[plugins][updater][preset-functional][rollback]")
{
    const std::string first_id = "batch_vendor_a";
    const std::string second_id = "batch_vendor_b";
    write_test_file(resources_directory / "profiles" / (first_id + ".ini"),
                    vendor_profile_contents(first_id, "1.0.0.0", slicer_version));
    write_test_file(resources_directory / "profiles" / (second_id + ".ini"),
                    vendor_profile_contents(second_id, "1.0.0.0", slicer_version));
    updater.reload_all_vendors();

    const std::optional<Slic3r::VendorSync> second = updater.vendor(second_id);
    REQUIRE(second.has_value());
    const Slic3r::VendorAvailable *second_best = second->best_available();
    REQUIRE(second_best != nullptr);
    REQUIRE_FALSE(second_best->local_file.empty());

    std::optional<Slic3r::UpdaterErrors> result;
    {
        ScopedFilesystemTransactionHook hook(
            [second_id](Slic3r::FilesystemTransactionTestPoint point,
                        const boost::filesystem::path &,
                        const boost::filesystem::path &destination) {
                if (point == Slic3r::FilesystemTransactionTestPoint::BeforePublishStaging &&
                    destination.filename() == second_id + ".ini")
                    throw std::runtime_error("injected second vendor publication failure");
            });
        updater.install_all_vendors([&result](Slic3r::UpdaterErrors errors) {
            result = std::move(errors);
        });
        updater.wait_for_pending_operations();
    }

    REQUIRE(result.has_value());
    REQUIRE(result->size() == 1);
    CHECK(result->front().detail.find("injected second vendor publication failure") != std::string::npos);
    REQUIRE(host.rollback_tokens.size() == 1);
    CHECK(host.completed_changes.empty());
    CHECK_FALSE(boost::filesystem::exists(data_directory / "vendor" / (first_id + ".ini")));
    CHECK_FALSE(boost::filesystem::exists(data_directory / "vendor" / (second_id + ".ini")));
    const std::optional<Slic3r::VendorSync> first_after = updater.vendor(first_id);
    const std::optional<Slic3r::VendorSync> second_after = updater.vendor(second_id);
    REQUIRE(first_after.has_value());
    REQUIRE(second_after.has_value());
    CHECK_FALSE(first_after->is_installed);
    CHECK_FALSE(second_after->is_installed);
}

TEST_CASE_METHOD(PresetUpdaterFunctionalFixture,
                 "PresetUpdater restores a complete uninstall batch when one removal fails",
                 "[plugins][updater][preset-functional][rollback]")
{
    const std::string first_id = "uninstall_vendor_a";
    const std::string second_id = "uninstall_vendor_b";
    write_test_file(data_directory / "vendor" / (first_id + ".ini"),
                    vendor_profile_contents(first_id, "1.0.0.0", slicer_version));
    write_test_file(data_directory / "vendor" / (second_id + ".ini"),
                    vendor_profile_contents(second_id, "1.0.0.0", slicer_version));
    updater.reload_all_vendors();

    std::optional<Slic3r::UpdaterError> result;
    {
        ScopedFilesystemTransactionHook hook(
            [second_id](Slic3r::FilesystemTransactionTestPoint point,
                        const boost::filesystem::path &source,
                        const boost::filesystem::path &) {
                if (point == Slic3r::FilesystemTransactionTestPoint::BeforePreserveDestination &&
                    source.filename() == second_id + ".ini")
                    throw std::runtime_error("injected second vendor removal failure");
            });
        updater.uninstall_all_vendors([&result](Slic3r::UpdaterError error) {
            result = std::move(error);
        });
        updater.wait_for_pending_operations();
    }

    REQUIRE(result.has_value());
    CHECK_FALSE(result->succeeded());
    CHECK(result->detail.find("injected second vendor removal failure") != std::string::npos);
    CHECK(boost::filesystem::is_regular_file(data_directory / "vendor" / (first_id + ".ini")));
    CHECK(boost::filesystem::is_regular_file(data_directory / "vendor" / (second_id + ".ini")));
    REQUIRE(host.rollback_tokens.size() == 1);
    CHECK(host.completed_changes.empty());
    REQUIRE(updater.vendor(first_id).has_value());
    REQUIRE(updater.vendor(second_id).has_value());
    CHECK(updater.vendor(first_id)->is_installed);
    CHECK(updater.vendor(second_id)->is_installed);
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
    updater.wait_for_pending_operations();

    REQUIRE(uninstall_result.has_value());
    CHECK(uninstall_result->succeeded());
    CHECK(updater.count_installed() == 0);
    CHECK_FALSE(boost::filesystem::exists(data_directory / "vendor" / (vendor_id + ".ini")));
    CHECK(boost::filesystem::is_regular_file(Slic3r::repository_package_cache_path(
        data_directory, Slic3r::RepositoryPackageType::Vendor, vendor_id,
        "1.0.0.0", slicer_version) / "profiles" / (vendor_id + ".ini")));

    const std::optional<Slic3r::VendorSync> remaining_vendor = updater.vendor(vendor_id);
    REQUIRE(remaining_vendor.has_value());
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
    const Slic3r::VendorAvailable *dialog_best = dialog_vendor.best_available();
    REQUIRE(dialog_best != nullptr);
    CHECK(dialog_best->config_version.to_string() == "2.0.0.0");
    CHECK(dialog_best->local_file.empty());

    // The remote branch downloads and validates the archive before dispatching
    // the live publication to the host-owned thread.
    const boost::filesystem::path archive_file = temporary.path() / "vendor-2.zip";
    REQUIRE(write_test_zip(
        archive_file,
        {{"profiles/" + vendor_id + ".ini",
          vendor_profile_contents(vendor_id, "2.0.0.0", slicer_version)}}));
    const std::string archive_contents = read_test_file(archive_file);
    host.dispatch_immediately = false;

    std::optional<Slic3r::UpdaterError> upgrade_result;
    updater.install_vendor(vendor_id, *dialog_best,
                           [&upgrade_result](Slic3r::UpdaterError error) {
                               upgrade_result = std::move(error);
                           });

    CHECK_FALSE(upgrade_result.has_value());
    REQUIRE(http.pending_count() == 1);
    CHECK(http.pending_front().url() == remote_archive_url);
    CHECK(host.prepared_changes.empty());
    CHECK(Slic3r::VendorProfile::from_ini(
        data_directory / "vendor" / (vendor_id + ".ini"), true).config_version.to_string() == "1.0.0.0");

    // A second mutation cannot overlap the download that owns the future
    // snapshot and live-file transaction.
    std::optional<Slic3r::UpdaterError> overlapping_result;
    updater.install_vendor(vendor_id, *dialog_best,
                           [&overlapping_result](Slic3r::UpdaterError error) {
                               overlapping_result = std::move(error);
                           });
    REQUIRE(overlapping_result.has_value());
    CHECK(overlapping_result->code == Slic3r::UpdaterError::Code::PreparationRejected);
    CHECK(http.pending_count() == 1);

    http.succeed_front(archive_contents, 200);
    updater.wait_for_pending_operations();
    CHECK_FALSE(upgrade_result.has_value());
    CHECK(host.prepared_changes.empty());
    REQUIRE(host.dispatched_changes.size() == 1);
    host.run_next_dispatched_change();
    updater.wait_for_pending_operations();

    REQUIRE(upgrade_result.has_value());
    INFO("Updater error code: " << static_cast<int>(upgrade_result->code));
    INFO("Updater error detail: " << upgrade_result->detail);
    CHECK(upgrade_result->succeeded());
    CHECK(http.sync_request_count() == 0);
    const boost::filesystem::path installed_file = data_directory / "vendor" / (vendor_id + ".ini");
    REQUIRE(boost::filesystem::is_regular_file(installed_file));
    CHECK(Slic3r::VendorProfile::from_ini(installed_file, true).config_version.to_string() == "2.0.0.0");

    const std::optional<Slic3r::VendorSync> updated_vendor = updater.vendor(vendor_id);
    REQUIRE(updated_vendor.has_value());
    CHECK(updated_vendor->is_installed);
    CHECK_FALSE(updated_vendor->can_upgrade);
    REQUIRE(host.completed_changes.size() == 1);
    CHECK(host.completed_changes.front().change == Slic3r::VendorChange::Install);
}

TEST_CASE_METHOD(PresetUpdaterFunctionalFixture,
                 "PresetUpdater releases an asynchronous vendor operation after preparation errors",
                 "[plugins][updater][preset-functional]")
{
    write_installed_vendor("1.0.0.0");
    updater.reload_all_vendors();

    const std::string remote_archive_url = "https://example.invalid/vendor-broken.zip";
    const PresetDialogSnapshot dialog = synchronize_and_open_dialog({
        {"2.0.0.0", slicer_version, remote_archive_url}
    });
    const Slic3r::VendorAvailable *best = dialog.vendors.front().best_available();
    REQUIRE(best != nullptr);

    SECTION("network failure") {
        std::optional<Slic3r::UpdaterError> result;
        updater.install_vendor(vendor_id, *best, [&result](Slic3r::UpdaterError error) {
            result = std::move(error);
        });
        REQUIRE(http.pending_count() == 1);
        http.fail_front(std::string(), "connection refused", 0);

        REQUIRE(result.has_value());
        CHECK(result->code == Slic3r::UpdaterError::Code::Network);
        CHECK(host.prepared_changes.empty());
        CHECK(host.dispatched_changes.empty());

        // The terminal error releases the operation gate, so a retry starts a
        // fresh HTTP request instead of being rejected as overlapping work.
        result.reset();
        updater.install_vendor(vendor_id, *best, [&result](Slic3r::UpdaterError error) {
            result = std::move(error);
        });
        CHECK_FALSE(result.has_value());
        REQUIRE(http.pending_count() == 1);
        http.fail_front(std::string(), "retry stopped", 0);
        REQUIRE(result.has_value());
    }

    SECTION("invalid archive") {
        std::optional<Slic3r::UpdaterError> result;
        updater.install_vendor(vendor_id, *best, [&result](Slic3r::UpdaterError error) {
            result = std::move(error);
        });
        REQUIRE(http.pending_count() == 1);
        http.succeed_front("not a zip archive", 200);
        updater.wait_for_pending_operations();

        REQUIRE(result.has_value());
        CHECK(result->code == Slic3r::UpdaterError::Code::InvalidArchive);
        CHECK(host.prepared_changes.empty());
        const boost::filesystem::path transfer_archive = Slic3r::repository_cache_root_path(
            data_directory, Slic3r::RepositoryPackageType::Vendor, vendor_id) /
            (Slic3r::RepositoryPackageCache::version_directory_name(
                best->config_version.to_string(), best->slicer_version.to_string()) + ".zip");
        CHECK_FALSE(boost::filesystem::exists(transfer_archive));
    }
}

TEST_CASE_METHOD(PresetUpdaterFunctionalFixture,
                 "PresetUpdater prepares remote install batches sequentially before one snapshot",
                 "[plugins][updater][preset-functional]")
{
    const std::string first_id = "async_vendor_a";
    const std::string second_id = "async_vendor_b";
    write_test_file(data_directory / "vendor" / (first_id + ".ini"),
                    vendor_profile_contents(first_id, "1.0.0.0", slicer_version));
    write_test_file(data_directory / "vendor" / (second_id + ".ini"),
                    vendor_profile_contents(second_id, "1.0.0.0", slicer_version));
    updater.reload_all_vendors();

    std::optional<int> sync_result;
    updater.sync_async([&sync_result](int count) { sync_result = count; }, true);
    REQUIRE(http.pending_count() == 2);
    http.succeed_front(vendor_repository_tags({
        {"2.0.0.0", slicer_version, "https://example.invalid/vendor-a.zip"}
    }), 200);
    http.succeed_front(vendor_repository_tags({
        {"2.0.0.0", slicer_version, "https://example.invalid/vendor-b.zip"}
    }), 200);
    REQUIRE(sync_result.has_value());

    const boost::filesystem::path first_archive = temporary.path() / "vendor-a.zip";
    const boost::filesystem::path second_archive = temporary.path() / "vendor-b.zip";
    REQUIRE(write_test_zip(
        first_archive,
        {{"profiles/" + first_id + ".ini",
          vendor_profile_contents(first_id, "2.0.0.0", slicer_version)}}));
    REQUIRE(write_test_zip(
        second_archive,
        {{"profiles/" + second_id + ".ini",
          vendor_profile_contents(second_id, "2.0.0.0", slicer_version)}}));

    host.dispatch_immediately = false;
    std::optional<Slic3r::UpdaterErrors> install_result;
    updater.upgrade_all_installed_vendors([&install_result](Slic3r::UpdaterErrors errors) {
        install_result = std::move(errors);
    });

    REQUIRE(http.pending_count() == 1);
    CHECK(host.prepared_changes.empty());
    http.succeed_front(read_test_file(first_archive), 200);
    updater.wait_for_pending_operations();
    REQUIRE(http.pending_count() == 1);
    CHECK(host.dispatched_changes.empty());
    http.succeed_front(read_test_file(second_archive), 200);
    updater.wait_for_pending_operations();

    CHECK_FALSE(install_result.has_value());
    REQUIRE(host.dispatched_changes.size() == 1);
    CHECK(host.prepared_changes.empty());
    host.run_next_dispatched_change();
    updater.wait_for_pending_operations();

    REQUIRE(install_result.has_value());
    CHECK(install_result->empty());
    REQUIRE(host.prepared_changes.size() == 1);
    CHECK(host.prepared_changes.front().change == Slic3r::VendorChange::UpgradeAll);
    CHECK(host.prepared_changes.front().vendor_ids == std::vector<std::string>{first_id, second_id});
    CHECK(Slic3r::VendorProfile::from_ini(
        data_directory / "vendor" / (first_id + ".ini"), true).config_version.to_string() == "2.0.0.0");
    CHECK(Slic3r::VendorProfile::from_ini(
        data_directory / "vendor" / (second_id + ".ini"), true).config_version.to_string() == "2.0.0.0");
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
    const Slic3r::VendorAvailable *main_best = main_dialog.vendors.front().best_available();
    REQUIRE(main_best != nullptr);
    CHECK(main_best->config_version.to_string() == "2.0.0.0");

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

    // ChooseVendorVersionDialog receives the now-enriched detached snapshot.
    const std::optional<Slic3r::VendorSync> enriched_vendor = updater.vendor(vendor_id);
    REQUIRE(enriched_vendor.has_value());
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

    // Selecting the changelog row starts the normal asynchronous archive
    // request after the changelog requests have completed.
    const boost::filesystem::path archive_file = temporary.path() / "vendor-2-with-logs.zip";
    REQUIRE(write_test_zip(
        archive_file,
        {{"profiles/" + vendor_id + ".ini",
          vendor_profile_contents(vendor_id, "2.0.0.0", slicer_version)}}));
    const std::string archive_contents = read_test_file(archive_file);

    std::optional<Slic3r::UpdaterError> install_result;
    updater.install_vendor(vendor_id, *selected,
                           [&install_result](Slic3r::UpdaterError error) {
                               install_result = std::move(error);
                           });

    CHECK_FALSE(install_result.has_value());
    REQUIRE(http.pending_count() == 1);
    CHECK(http.pending_front().url() == remote_archive_url);
    CHECK(host.prepared_changes.empty());
    http.succeed_front(archive_contents, 200);
    updater.wait_for_pending_operations();

    REQUIRE(install_result.has_value());
    INFO("Updater error code: " << static_cast<int>(install_result->code));
    INFO("Updater error detail: " << install_result->detail);
    CHECK(install_result->succeeded());
    CHECK(http.sync_request_count() == 0);

    const boost::filesystem::path installed_file = data_directory / "vendor" / (vendor_id + ".ini");
    REQUIRE(boost::filesystem::is_regular_file(installed_file));
    CHECK(Slic3r::VendorProfile::from_ini(installed_file, true).config_version.to_string() == "2.0.0.0");
    const std::optional<Slic3r::VendorSync> updated_vendor = updater.vendor(vendor_id);
    REQUIRE(updated_vendor.has_value());
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
          "https://raw.githubusercontent.com/example/vendor/HEAD/description.ini");
    CHECK_FALSE(result.has_value());

    // A successful transport response still has to pass the repository parser.
    // This body is intentionally malformed so the test remains filesystem-free.
    http.succeed_front("not a repository description", 200);

    REQUIRE(result.has_value());
    CHECK(result->code == Slic3r::UpdaterError::Code::InvalidArchive);
    CHECK(http.pending_count() == 0);
}
