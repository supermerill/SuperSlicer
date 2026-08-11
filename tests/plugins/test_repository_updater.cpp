///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// These tests keep the updater data model independent from wxWidgets. They
// verify the common success/error result, injectable HTTP behavior and that a
// vendor repository keeps local metadata while enriching it with tag data.

#include <catch2/catch.hpp>

#include <deque>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include "libslic3r/ContainerUtils.hpp"
#include "libslic3r/Updater/PluginUpdater.hpp"
#include "libslic3r/Updater/PresetUpdater.hpp"
#include "libslic3r/Updater/UpdaterHttp.hpp"
#include "libslic3r/Updater/UpdaterError.hpp"

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
        m_pending.emplace_back(std::move(request));
    }

    // A synchronous request must consume a response before returning, matching
    // the production transport contract without starting a worker thread.
    void perform_request_sync(Slic3r::UpdaterHttpRequest request) override
    {
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
    using RepositoryUpdater::download_repository_description;
    using RepositoryUpdater::download_repository_file_async;
    using RepositoryUpdater::download_repository_file_sync;
    using RepositoryUpdater::has_api_request_slot;
    using RepositoryUpdater::refresh_repository_tags;

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

std::string read_test_file(const boost::filesystem::path &path)
{
    boost::nowide::ifstream stream(path.string(), std::ios::in | std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
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

TEST_CASE("RepositoryUpdater refreshes tags through cache and transport", "[plugins][updater]")
{
    FakeUpdaterHttpTransport http;
    TestRepositoryUpdater updater(http);
    TemporaryDirectory temporary;
    const boost::filesystem::path cache_file = temporary.path() / "tags.json";
    std::string parsed_contents;
    std::optional<bool> refresh_succeeded;
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
                return true;
            },
            [&refresh_succeeded](bool succeeded) { refresh_succeeded = succeeded; });

        CHECK(http.pending_count() == 0);
        CHECK(parsed_contents == "cached tags");
        REQUIRE(refresh_succeeded.has_value());
        CHECK(*refresh_succeeded);
        CHECK(sync_callback_count == 1);
    }

    SECTION("an invalid cached body completes as a failure") {
        boost::nowide::ofstream stream(cache_file.string());
        stream << "invalid tags";
        stream.close();

        REQUIRE(updater.begin_sync(1, [&sync_callback_count](int) { ++sync_callback_count; }));
        updater.refresh_repository_tags(
            "invalid", "https://example.invalid/invalid", cache_file, false,
            [](const std::string &) { return false; },
            [&refresh_succeeded](bool succeeded) { refresh_succeeded = succeeded; });

        REQUIRE(refresh_succeeded.has_value());
        CHECK_FALSE(*refresh_succeeded);
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
                return true;
            },
            [&refresh_succeeded](bool succeeded) { refresh_succeeded = succeeded; });

        REQUIRE(http.pending_count() == 1);
        CHECK(http.pending_front().url() ==
              "https://example.invalid/forced/tags?per_page=100;page=1");
        CHECK(http.pending_front().response_size_limit() == 64 * 1024);
        CHECK(sync_callback_count == 0);

        http.succeed_front("fresh tags", 200);

        CHECK(parsed_contents == "fresh tags");
        CHECK(read_test_file(cache_file) == "fresh tags");
        REQUIRE(refresh_succeeded.has_value());
        CHECK(*refresh_succeeded);
        CHECK(sync_callback_count == 1);
    }

    SECTION("a transport failure completes once") {
        REQUIRE(updater.begin_sync(1, [&sync_callback_count](int) { ++sync_callback_count; }));
        updater.refresh_repository_tags(
            "offline", "https://example.invalid/offline", cache_file, false,
            [](const std::string &) { return true; },
            [&refresh_succeeded](bool succeeded) { refresh_succeeded = succeeded; });

        REQUIRE(http.pending_count() == 1);
        http.fail_front(std::string(), "offline", 0);

        REQUIRE(refresh_succeeded.has_value());
        CHECK_FALSE(*refresh_succeeded);
        CHECK(sync_callback_count == 1);
        CHECK(http.pending_count() == 0);
    }
}

TEST_CASE("RepositoryUpdater refuses tag refresh after the GitHub request limit", "[plugins][updater]")
{
    FakeUpdaterHttpTransport http;
    TestRepositoryUpdater updater(http);
    TemporaryDirectory temporary;
    std::optional<bool> refresh_succeeded;
    int sync_callback_count = 0;

    for (size_t request = 0; request < 24; ++request)
        REQUIRE(updater.has_api_request_slot("https://api.github.com/repos/example/repository"));

    REQUIRE(updater.begin_sync(1, [&sync_callback_count](int) { ++sync_callback_count; }));
    updater.refresh_repository_tags(
        "limited", "https://api.github.com/repos/example/repository", temporary.path() / "tags.json", true,
        [](const std::string &) { return true; },
        [&refresh_succeeded](bool succeeded) { refresh_succeeded = succeeded; });

    REQUIRE(refresh_succeeded.has_value());
    CHECK_FALSE(*refresh_succeeded);
    CHECK(sync_callback_count == 1);
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
        "\"commit\":{\"sha\":\"sha\",\"url\":\"commit\"}}]"));
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
