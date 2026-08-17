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
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <boost/filesystem/path.hpp>

#include "libslic3r/Semver.hpp"
#include "libslic3r/Updater/UpdaterError.hpp"
#include "libslic3r/Updater/UpdaterOperationExecutor.hpp"

namespace Slic3r {

class UpdaterHttpTransport;

namespace RepositoryUpdaterInternal {

class RepositoryTagService;
class RepositoryChangelogService;

// The kind controls both JSON parsing and request limits. Compare is kept
// explicit so its future multi-page implementation remains isolated inside
// RepositoryChangelogService instead of leaking pagination into the facade.
enum class RepositoryChangelogKind {
    Commit,
    Compare
};

// Vendor and plugin models convert their version records into this neutral
// input before asking the changelog service to choose comparison bases.
struct RepositoryChangelogVersion {
    Semver content_version;
    Semver slicer_version;
    std::string tag;
    std::string commit_sha;
    std::string commit_url;
    std::function<void(std::string)> store_notes;
};

// One request identifies its cache, endpoint and parser kind. store_notes is
// called only with validated content and must remain valid until the aggregate
// completion callback runs.
struct RepositoryChangelogRequest {
    boost::filesystem::path cache_file;
    std::string url;
    RepositoryChangelogKind kind = RepositoryChangelogKind::Commit;
    std::function<void(std::string)> store_notes;
};

} // namespace RepositoryUpdaterInternal

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
    virtual ~RepositoryUpdater();

    // Establish a synchronization point after local filesystem operations.
    // Network requests may still be active and enqueue the next operation in a
    // multi-download sequence after this function returns.
    void wait_for_pending_operations();

    // Convert the repository spellings accepted by the GUI and configuration
    // files into one REST URL. Short "owner/repository" and github.com URLs
    // become the GitHub API form; other explicit HTTP(S) endpoints are kept.
    static std::string normalize_repository_rest_url(const std::string &configured_url);

protected:
    // Repository callbacks run on Http worker threads while callers may read
    // the model from the GUI or another application thread. Derived updaters
    // use this mutex for their complete vendor or plugin model; network,
    // filesystem work and user callbacks must run after releasing it.
    mutable std::mutex m_model_mutex;

    using RepositoryChangelogKind = RepositoryUpdaterInternal::RepositoryChangelogKind;
    using RepositoryChangelogVersion = RepositoryUpdaterInternal::RepositoryChangelogVersion;
    using RepositoryChangelogRequest = RepositoryUpdaterInternal::RepositoryChangelogRequest;

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

    // Refreshes one repository's tags. GitHub advances by one historical page
    // per 24 hours and rereads page one immediately when an overlap indicates
    // new releases; force bypasses only the daily delay. Other hosts fetch all
    // pages in one operation. tags.json remains one deduplicated array and a
    // sibling pagination INI stores the GitHub cursor. Invalid metadata never
    // replaces the previous cache. The terminal callback updates derived state
    // before update_count() is evaluated.
    void refresh_repository_tags(const std::string &repository_id,
                                 const std::string &rest_url,
                                 const boost::filesystem::path &cache_file,
                                 bool force,
                                 ParseRepositoryTagsFn parse_tags,
                                 RepositoryRefreshFinishedFn finished);

    // Downloads description.ini and gives its contents plus the repository
    // name from the URL to the derived updater. GitHub repositories use the
    // raw HEAD ref so the download follows their default branch without using
    // the rate-limited REST API. Other hosts keep their /description endpoint.
    // The consumer owns parsing and persistence; this helper owns URL
    // conversion and network errors.
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
    // reused unless force is true. Downloaded JSON is parsed before its cache
    // is atomically replaced, then the validated notes are stored. The callback
    // runs once after every request and reports aggregate success.
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
    bool changelog_download_in_progress() const;

    // Filesystem mutations use one serialized worker per updater, and network
    // chains retain asynchronous-operation tokens from the same executor.
    // Derived destructors call shutdown_operation_executor() before their model
    // members are destroyed because both kinds of callbacks may reference them.
    bool enqueue_operation(UpdaterOperationExecutor::Operation operation,
                           UpdaterOperationExecutor::Completion completion);
    void shutdown_operation_executor();

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
    std::atomic_bool m_sync_in_progress = false;
    std::mutex m_callback_mutex;
    std::vector<std::function<void(int)>> m_sync_callbacks;
    // The request window and its remaining budget form one state transition.
    // Pagination and download callbacks may reserve slots concurrently.
    std::mutex m_api_request_mutex;
    int m_max_api_requests = 25;
    std::time_t m_next_api_window = 0;
    UpdaterHttpTransport &m_http_transport;
    std::unique_ptr<RepositoryUpdaterInternal::RepositoryTagService> m_tag_service;
    std::unique_ptr<RepositoryUpdaterInternal::RepositoryChangelogService> m_changelog_service;
    UpdaterOperationExecutor m_operation_executor;
};

} // namespace Slic3r

#endif // slic3r_Updater_RepositoryUpdater_hpp_
