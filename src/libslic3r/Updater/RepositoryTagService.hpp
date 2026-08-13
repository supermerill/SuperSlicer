///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher

// RepositoryTagService owns the asynchronous tag pagination state used by
// repository updaters. It accepts an already normalized repository URL and
// publishes one validated aggregate to the caller. Vendor and plugin parsing
// stays outside this service, so it has no knowledge of package models.

#ifndef slic3r_Updater_RepositoryTagService_hpp_
#define slic3r_Updater_RepositoryTagService_hpp_

#include <functional>
#include <string>

#include <boost/filesystem/path.hpp>

#include "libslic3r/Updater/UpdaterError.hpp"

namespace Slic3r {

class UpdaterHttpTransport;

namespace RepositoryUpdaterInternal {

class RepositoryTagService
{
public:
    using ParseTagsFn = std::function<UpdaterError(const std::string &)>;
    using ReserveRequestSlotFn = std::function<bool(const std::string &)>;
    using FinishedFn = std::function<void(UpdaterError)>;

    explicit RepositoryTagService(UpdaterHttpTransport &http_transport);

    // GitHub repositories advance by one historical page per 24 hours, with
    // one immediate page-one reread when overlap reveals shifted history.
    // Other hosts are fully paginated in this call. The parser sees only the
    // final aggregate and the callback is invoked once by the owning facade.
    void refresh(const std::string &repository_id,
                 const std::string &repository_url,
                 const boost::filesystem::path &cache_file,
                 bool force,
                 ReserveRequestSlotFn reserve_request_slot,
                 ParseTagsFn parse_tags,
                 FinishedFn finished);

private:
    UpdaterHttpTransport &m_http_transport;
};

} // namespace RepositoryUpdaterInternal
} // namespace Slic3r

#endif // slic3r_Updater_RepositoryTagService_hpp_
