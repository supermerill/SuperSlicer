///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// These tests keep the updater data model independent from wxWidgets. They
// verify the common success/error result, injectable HTTP behavior and that a
// vendor repository keeps local metadata while enriching it with tag data.

#include <catch2/catch.hpp>

#include <deque>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

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

private:
    // Async requests remain pending until the test selects their outcome.
    void perform_request(Slic3r::UpdaterHttpRequest request) override
    {
        m_pending.emplace_back(std::move(request));
    }

    // None of these updater tests executes an archive download synchronously.
    // Throwing makes an unexpected new sync operation visible instead of
    // silently giving it asynchronous semantics in the fake.
    void perform_request_sync(Slic3r::UpdaterHttpRequest) override
    {
        throw std::logic_error("The fake updater transport has no scripted synchronous response.");
    }

    std::deque<Slic3r::UpdaterHttpRequest> m_pending;
};

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
