///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// RepositoryUpdater contains the asynchronous mechanics shared by all
// downloadable repositories. Derived classes own their content-specific
// records, but use this class for the one active refresh, GitHub rate limit
// and completion callback. This keeps plugin DLL lifetime and preset loading
// out of the common layer.

#ifndef slic3r_Updater_RepositoryUpdater_hpp_
#define slic3r_Updater_RepositoryUpdater_hpp_

#include <atomic>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <boost/filesystem/path.hpp>

#include "libslic3r/Semver.hpp"
#include "libslic3r/Updater/UpdaterError.hpp"

namespace Slic3r {

class UpdaterHttpTransport;

// A repository has one synchronization state at a time. Keeping progress and
// completion in one enum prevents stale success and failure flags from being
// visible together after a retry.
enum class RepositorySyncState {
    Unchecked,
    InProgress,
    Succeeded,
    Failed
};

class RepositoryUpdater {
public:
    // The default constructor uses the application's Http backend. Tests pass
    // a transport whose lifetime covers the updater and all pending callbacks.
    RepositoryUpdater();
    explicit RepositoryUpdater(UpdaterHttpTransport &http_transport);
    RepositoryUpdater(const RepositoryUpdater &) = delete;
    RepositoryUpdater(RepositoryUpdater &&) = delete;
    RepositoryUpdater &operator=(const RepositoryUpdater &) = delete;
    RepositoryUpdater &operator=(RepositoryUpdater &&) = delete;
    virtual ~RepositoryUpdater() = default;

    // Convert the repository spellings accepted by the GUI and configuration
    // files into one REST URL. Short "owner/repository" and github.com URLs
    // become the GitHub API form; other explicit HTTP(S) endpoints are kept.
    static std::string normalize_repository_rest_url(const std::string &configured_url);

protected:
    // Vendor and plugin models expose different version types. Each derived
    // updater converts one package into this common description, after which
    // RepositoryUpdater can select comparison bases and build cache paths.
    struct RepositoryChangelogVersion {
        Semver content_version;
        Semver slicer_version;
        std::string tag;
        std::string commit_sha;
        std::string commit_url;
        std::function<void(std::string)> store_notes;
    };

    // A derived updater creates one request per available package version.
    // store_notes is invoked only after the JSON has been parsed successfully;
    // every object captured by it must remain alive until the final callback.
    struct RepositoryChangelogRequest {
        boost::filesystem::path cache_file;
        std::string url;
        bool compare = false;
        std::function<void(std::string)> store_notes;
    };

    using ParseRepositoryTagsFn = std::function<UpdaterError(const std::string &)>;
    using RepositoryRefreshFinishedFn = std::function<void(UpdaterError)>;
    using RepositoryDescriptionConsumerFn =
        std::function<UpdaterError(const std::string &, const std::string &)>;
    using UpdaterErrorCallback = std::function<void(UpdaterError)>;

    // Starts one logical refresh. If another refresh is already running, the
    // callback joins that operation and is called with its final update count.
    // A false result means the caller must not start repository requests; the
    // callback was either queued or invoked immediately when changelogs block
    // synchronization.
    bool begin_sync(size_t repository_count, std::function<void(int)> callback);

    // Completes one repository refresh and calls update_count() only after
    // the last asynchronous request has released its references.
    void finish_sync();

    // GitHub limits unauthenticated API traffic. Other hosts are deliberately
    // unrestricted because they may implement their own rate policy.
    bool has_api_request_slot(const std::string &url);

    // Refreshes one repository's tags. A recent cache is parsed immediately;
    // otherwise the common GitHub tags endpoint is downloaded and cached. The
    // terminal callback receives a precise transport, filesystem or parsing
    // error and updates derived state before update_count() is evaluated.
    void refresh_repository_tags(const std::string &repository_id,
                                 const std::string &rest_url,
                                 const boost::filesystem::path &cache_file,
                                 bool force,
                                 ParseRepositoryTagsFn parse_tags,
                                 RepositoryRefreshFinishedFn finished);

    // Downloads description.ini and gives its contents plus the repository
    // name from the URL to the derived updater. The consumer owns parsing and
    // persistence; this helper owns URL conversion and network errors.
    void download_repository_description(const std::string &rest_url,
                                         RepositoryDescriptionConsumerFn consume,
                                         UpdaterErrorCallback callback);

    // Downloads a repository archive into a local file. Both forms validate
    // URL/rate limits and distinguish network failures from local filesystem
    // failures. The synchronous form invokes no callback and returns directly.
    void download_repository_file_async(const std::string &url,
                                        const boost::filesystem::path &destination,
                                        size_t size_limit,
                                        UpdaterErrorCallback callback);
    UpdaterError download_repository_file_sync(const std::string &url,
                                               const boost::filesystem::path &destination,
                                               size_t size_limit);

    // Loads all changelogs as one logical operation. Existing cache files are
    // reused unless force is true. The callback runs once, after every request
    // has either stored its notes or failed, and reports aggregate success.
    void download_repository_changelogs(std::vector<RepositoryChangelogRequest> requests,
                                        std::function<void(bool)> callback,
                                        bool force);

    // Build and download changelogs for domain-specific package versions. The
    // closest older package for the same slicer family is preferred; if none
    // exists, the closest package for an older compatible family is used.
    void download_repository_version_changelogs(std::vector<RepositoryChangelogVersion> versions,
                                                const boost::filesystem::path &log_directory,
                                                const std::string &configured_rest_url,
                                                std::function<void(bool)> callback,
                                                bool force);

    bool sync_in_progress() const { return m_sync_in_progress; }
    bool changelog_download_in_progress() const { return m_pending_changelogs != 0; }

    // Derived updaters build every request through this accessor so tests can
    // observe and complete the operation without contacting the network.
    UpdaterHttpTransport &http() { return m_http_transport; }

private:
    // Publishes one final model state to every caller that requested or joined
    // the refresh. Callbacks run outside the mutex and may start a new refresh.
    void complete_sync();

    // Completes the derived state transition and always releases this logical
    // repository from the enclosing sync, including when the callback throws.
    void finish_repository_refresh(UpdaterError error, const RepositoryRefreshFinishedFn &finished);

    virtual int update_count() = 0;
    virtual void on_sync_completed() {}

    std::atomic_int m_pending_syncs = 0;
    std::atomic_int m_pending_changelogs = 0;
    std::atomic_bool m_sync_in_progress = false;
    std::mutex m_callback_mutex;
    std::vector<std::function<void(int)>> m_sync_callbacks;
    std::atomic_int m_max_api_requests = 25;
    std::time_t m_next_api_window = 0;
    UpdaterHttpTransport &m_http_transport;
};

} // namespace Slic3r

#endif // slic3r_Updater_RepositoryUpdater_hpp_
