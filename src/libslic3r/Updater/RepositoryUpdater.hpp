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

#include "libslic3r/Updater/UpdaterError.hpp"

namespace Slic3r {

class UpdaterHttpTransport;

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

protected:
    // A derived updater creates one request per available package version.
    // store_notes is invoked only after the JSON has been parsed successfully;
    // every object captured by it must remain alive until the final callback.
    struct RepositoryChangelogRequest {
        boost::filesystem::path cache_file;
        std::string url;
        bool compare = false;
        std::function<void(std::string)> store_notes;
    };

    using ParseRepositoryTagsFn = std::function<bool(const std::string &)>;
    using RepositoryRefreshFinishedFn = std::function<void(bool)>;
    using RepositoryDescriptionConsumerFn =
        std::function<UpdaterError(const std::string &, const std::string &)>;
    using UpdaterErrorCallback = std::function<void(UpdaterError)>;

    // Starts one logical refresh. Every repository must call finish_sync()
    // exactly once, including malformed descriptions and HTTP failures.
    bool begin_sync(size_t repository_count, std::function<void(int)> callback);

    // Completes one repository refresh and calls update_count() only after
    // the last asynchronous request has released its references.
    void finish_sync();

    // GitHub limits unauthenticated API traffic. Other hosts are deliberately
    // unrestricted because they may implement their own rate policy.
    bool has_api_request_slot(const std::string &url);

    // Refreshes one repository's tags. A recent cache is parsed immediately;
    // otherwise the common GitHub tags endpoint is downloaded and cached. The
    // finished callback updates derived state before this method calls
    // finish_sync(), so update_count() observes the final result.
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

    bool sync_in_progress() const { return m_sync_in_progress; }
    bool changelog_download_in_progress() const { return m_pending_changelogs != 0; }

    // Derived updaters build every request through this accessor so tests can
    // observe and complete the operation without contacting the network.
    UpdaterHttpTransport &http() { return m_http_transport; }

private:
    // Completes the derived state transition and always releases this logical
    // repository from the enclosing sync, including when the callback throws.
    void finish_repository_refresh(bool succeeded, const RepositoryRefreshFinishedFn &finished);

    virtual int update_count() = 0;
    virtual void on_sync_completed() {}

    std::atomic_int m_pending_syncs = 0;
    std::atomic_int m_pending_changelogs = 0;
    std::atomic_bool m_sync_in_progress = false;
    std::mutex m_callback_mutex;
    std::function<void(int)> m_callback;
    std::atomic_int m_max_api_requests = 25;
    std::time_t m_next_api_window = 0;
    UpdaterHttpTransport &m_http_transport;
};

} // namespace Slic3r

#endif // slic3r_Updater_RepositoryUpdater_hpp_
