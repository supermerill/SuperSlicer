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
    // Starts one logical refresh. Every repository must call finish_sync()
    // exactly once, including malformed descriptions and HTTP failures.
    bool begin_sync(size_t repository_count, std::function<void(int)> callback);

    // Completes one repository refresh and calls update_count() only after
    // the last asynchronous request has released its references.
    void finish_sync();

    // GitHub limits unauthenticated API traffic. Other hosts are deliberately
    // unrestricted because they may implement their own rate policy.
    bool has_api_request_slot(const std::string &url);

    bool sync_in_progress() const { return m_sync_in_progress; }

    // Derived updaters build every request through this accessor so tests can
    // observe and complete the operation without contacting the network.
    UpdaterHttpTransport &http() { return m_http_transport; }

private:
    virtual int update_count() = 0;
    virtual void on_sync_completed() {}

    std::atomic_int m_pending_syncs = 0;
    std::atomic_bool m_sync_in_progress = false;
    std::mutex m_callback_mutex;
    std::function<void(int)> m_callback;
    std::atomic_int m_max_api_requests = 25;
    std::time_t m_next_api_window = 0;
    UpdaterHttpTransport &m_http_transport;
};

} // namespace Slic3r

#endif // slic3r_Updater_RepositoryUpdater_hpp_
