///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher

// RepositoryChangelogService selects comparison bases and downloads cached
// commit notes for both preset and plugin repositories. Commit and Compare are
// explicit request kinds: a future paginated Compare implementation can change
// this service without affecting RepositoryUpdater or its derived models.

#ifndef slic3r_Updater_RepositoryChangelogService_hpp_
#define slic3r_Updater_RepositoryChangelogService_hpp_

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>

#include "libslic3r/Updater/RepositoryUpdater.hpp"

namespace Slic3r {

class UpdaterHttpTransport;

namespace RepositoryUpdaterInternal {

class RepositoryChangelogService
{
public:
    using ReserveRequestSlotFn = std::function<bool(const std::string &)>;
    using SyncInProgressFn = std::function<bool()>;
    using FinishedFn = std::function<void(bool)>;

    explicit RepositoryChangelogService(UpdaterHttpTransport &http_transport);

    // Downloads or restores one batch. A tag synchronization blocks the batch
    // because request callbacks may refer to version records rebuilt by it.
    void download(std::vector<RepositoryChangelogRequest> requests,
                  FinishedFn finished,
                  bool force,
                  SyncInProgressFn sync_in_progress,
                  ReserveRequestSlotFn reserve_request_slot);

    // Converts neutral package versions into commit or compare requests. The
    // repository URL must already be normalized by RepositoryUpdater.
    void download_versions(std::vector<RepositoryChangelogVersion> versions,
                           const boost::filesystem::path &log_directory,
                           const std::string &repository_url,
                           FinishedFn finished,
                           bool force,
                           SyncInProgressFn sync_in_progress,
                           ReserveRequestSlotFn reserve_request_slot);

    bool download_in_progress() const { return m_pending_downloads != 0; }

private:
    UpdaterHttpTransport &m_http_transport;
    std::atomic_int m_pending_downloads = 0;
};

} // namespace RepositoryUpdaterInternal
} // namespace Slic3r

#endif // slic3r_Updater_RepositoryChangelogService_hpp_
