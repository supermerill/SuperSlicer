///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// See RepositoryUpdater.hpp. The implementation is intentionally independent
// from wx and package files so it can coordinate both vendor and plugin HTTP
// operations without knowing what their archives contain.

#include "RepositoryUpdater.hpp"

namespace Slic3r {

bool RepositoryUpdater::begin_sync(size_t repository_count, std::function<void(int)> callback)
{
    if (m_sync_in_progress.exchange(true))
        return false;

    {
        std::lock_guard<std::mutex> guard(m_callback_mutex);
        m_callback = std::move(callback);
    }
    m_pending_syncs = static_cast<int>(repository_count);
    if (repository_count == 0) {
        m_sync_in_progress = false;
        std::function<void(int)> empty_callback;
        {
            std::lock_guard<std::mutex> guard(m_callback_mutex);
            empty_callback = std::move(m_callback);
            m_callback = [](int) {};
        }
        if (empty_callback)
            empty_callback(update_count());
    }
    return true;
}

void RepositoryUpdater::finish_sync()
{
    if (--m_pending_syncs != 0)
        return;

    m_sync_in_progress = false;
    on_sync_completed();
    std::function<void(int)> callback;
    {
        std::lock_guard<std::mutex> guard(m_callback_mutex);
        callback = std::move(m_callback);
        m_callback = [](int) {};
    }
    if (callback)
        callback(update_count());
}

bool RepositoryUpdater::has_api_request_slot(const std::string &url)
{
    if (url.find("api.github.com") == std::string::npos)
        return true;

    const std::time_t now = std::time(nullptr);
    if (m_next_api_window + 3600 < now) {
        m_next_api_window = now;
        m_max_api_requests = 25;
    }
    return --m_max_api_requests > 0;
}

} // namespace Slic3r
