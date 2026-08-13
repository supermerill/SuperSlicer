///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher

// RepositoryUpdater is the facade shared by preset and plugin updaters. It
// coordinates one logical synchronization, owns the GitHub request budget and
// handles single-resource downloads. Stateful tag pagination and changelog
// processing live in dedicated services and report through this facade.

#include "libslic3r/Updater/RepositoryUpdater.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <ctime>
#include <exception>
#include <memory>
#include <utility>

#include "libslic3r/Updater/RepositoryCacheIO.hpp"
#include "libslic3r/Updater/RepositoryChangelogService.hpp"
#include "libslic3r/Updater/RepositoryTagService.hpp"
#include "libslic3r/Updater/UpdaterHttp.hpp"

namespace Slic3r {
namespace {

const size_t k_repository_metadata_size_limit = 64 * 1024;

} // namespace

RepositoryUpdater::RepositoryUpdater()
    : RepositoryUpdater(default_updater_http_transport())
{
}

RepositoryUpdater::RepositoryUpdater(UpdaterHttpTransport &http_transport)
    : m_http_transport(http_transport)
    , m_tag_service(std::make_unique<RepositoryUpdaterInternal::RepositoryTagService>(http_transport))
    , m_changelog_service(
          std::make_unique<RepositoryUpdaterInternal::RepositoryChangelogService>(http_transport))
{
}

RepositoryUpdater::~RepositoryUpdater() = default;

std::string RepositoryUpdater::normalize_repository_rest_url(const std::string &configured_url)
{
    std::string normalized = configured_url;
    while (!normalized.empty() && normalized.back() == '/')
        normalized.pop_back();
    if (normalized.empty())
        return normalized;

    // Accept both the common owner/repository shorthand and explicit hosts.
    // Every GitHub spelling is converted to the REST form used by services.
    if (normalized.find("://") == std::string::npos) {
        const size_t first_slash = normalized.find('/');
        const std::string first_component = normalized.substr(0, first_slash);
        normalized = first_component.find('.') == std::string::npos ?
            "https://github.com/" + normalized : "https://" + normalized;
    }

    const size_t scheme_end = normalized.find("://");
    const size_t host_start = scheme_end == std::string::npos ? 0 : scheme_end + 3;
    const size_t path_start = normalized.find('/', host_start);
    std::string host = normalized.substr(host_start, path_start - host_start);
    std::transform(host.begin(), host.end(), host.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });

    if (host != "github.com" && host != "www.github.com" && host != "api.github.com")
        return normalized;

    std::string repository_path = path_start == std::string::npos ?
        std::string() : normalized.substr(path_start + 1);
    if (host == "api.github.com" && repository_path.rfind("repos/", 0) == 0)
        repository_path.erase(0, std::strlen("repos/"));
    if (repository_path.size() > 4 && repository_path.compare(repository_path.size() - 4, 4, ".git") == 0)
        repository_path.erase(repository_path.size() - 4);
    return repository_path.empty() ? std::string() : "https://api.github.com/repos/" + repository_path;
}

bool RepositoryUpdater::begin_sync(size_t repository_count, std::function<void(int)> callback)
{
    // Tag parsing replaces records that changelog callbacks may reference. A
    // caller can retry after the active changelog batch has released them.
    if (changelog_download_in_progress()) {
        if (callback)
            callback(update_count());
        return false;
    }

    {
        std::lock_guard<std::mutex> guard(m_callback_mutex);
        if (m_sync_in_progress) {
            if (callback)
                m_sync_callbacks.emplace_back(std::move(callback));
            return false;
        }
        m_sync_in_progress = true;
        if (callback)
            m_sync_callbacks.emplace_back(std::move(callback));
    }
    m_pending_syncs = static_cast<int>(repository_count);
    if (repository_count == 0)
        complete_sync();
    return true;
}

void RepositoryUpdater::finish_sync()
{
    if (--m_pending_syncs != 0)
        return;

    complete_sync();
}

void RepositoryUpdater::complete_sync()
{
    // Derived models finalize aggregate state before subscribers rebuild from
    // update_count(). User callbacks run outside the callback mutex.
    on_sync_completed();
    const int final_update_count = update_count();

    std::vector<std::function<void(int)>> callbacks;
    {
        std::lock_guard<std::mutex> guard(m_callback_mutex);
        m_sync_in_progress = false;
        callbacks.swap(m_sync_callbacks);
    }
    for (const std::function<void(int)> &callback : callbacks)
        callback(final_update_count);
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

void RepositoryUpdater::refresh_repository_tags(const std::string &repository_id,
                                                const std::string &rest_url,
                                                const boost::filesystem::path &cache_file,
                                                bool force,
                                                ParseRepositoryTagsFn parse_tags,
                                                RepositoryRefreshFinishedFn finished)
{
    // Every cache, startup and transport path converges on this guard. The
    // enclosing synchronization is released exactly once even if a transport
    // reports twice or throws while unwinding a callback.
    const std::shared_ptr<std::atomic_bool> terminal = std::make_shared<std::atomic_bool>(false);
    const RepositoryRefreshFinishedFn complete = [this, terminal, finished](UpdaterError error) {
        if (!terminal->exchange(true))
            finish_repository_refresh(std::move(error), finished);
    };

    const std::string repository_url = normalize_repository_rest_url(rest_url);
    if (repository_url.empty()) {
        complete(make_updater_error(UpdaterError::Code::RepositoryNotFound,
                                    "The repository URL is empty or malformed."));
        return;
    }

    m_tag_service->refresh(
        repository_id, repository_url, cache_file, force,
        [this](const std::string &url) { return has_api_request_slot(url); },
        std::move(parse_tags), complete);
}

void RepositoryUpdater::download_repository_description(const std::string &rest_url,
                                                        RepositoryDescriptionConsumerFn consume,
                                                        UpdaterErrorCallback callback)
{
    const std::shared_ptr<std::atomic_bool> terminal = std::make_shared<std::atomic_bool>(false);
    const UpdaterErrorCallback complete = [terminal, callback](UpdaterError error) {
        if (!terminal->exchange(true))
            callback(std::move(error));
    };

    const std::string repository_url = normalize_repository_rest_url(rest_url);
    const size_t separator = repository_url.find_last_of('/');
    const std::string fallback_id = separator == std::string::npos ?
        repository_url : repository_url.substr(separator + 1);
    if (repository_url.empty() || fallback_id.empty()) {
        complete(make_updater_error(UpdaterError::Code::RepositoryNotFound,
                                    "The repository URL is empty or malformed."));
        return;
    }

    const size_t github_marker = repository_url.find("https://api.github.com/repos/");
    const std::string description_url = github_marker == std::string::npos ?
        repository_url + "/description" :
        "https://raw.githubusercontent.com/" +
            repository_url.substr(github_marker + std::strlen("https://api.github.com/repos/")) +
            "/HEAD/description.ini";

    try {
        http().get(description_url)
            .size_limit(k_repository_metadata_size_limit)
            .on_error([complete](std::string, std::string error, unsigned status) {
                const UpdaterError::Code code = status == 404 ? UpdaterError::Code::RepositoryNotFound :
                                                               UpdaterError::Code::Network;
                complete(make_updater_error(code, std::move(error)));
            })
            .on_complete([consume, complete, fallback_id](std::string contents, unsigned) {
                complete(consume(contents, fallback_id));
            })
            .perform();
    } catch (const std::exception &error) {
        complete(make_updater_error(UpdaterError::Code::Network, error.what()));
    }
}

void RepositoryUpdater::download_repository_file_async(const std::string &url,
                                                       const boost::filesystem::path &destination,
                                                       size_t size_limit,
                                                       UpdaterErrorCallback callback)
{
    const std::shared_ptr<std::atomic_bool> terminal = std::make_shared<std::atomic_bool>(false);
    const UpdaterErrorCallback complete = [terminal, callback](UpdaterError error) {
        if (!terminal->exchange(true))
            callback(std::move(error));
    };

    if (url.empty()) {
        complete(make_updater_error(UpdaterError::Code::ArchiveUnavailable));
        return;
    }
    if (!has_api_request_slot(url)) {
        complete(make_updater_error(UpdaterError::Code::RateLimited));
        return;
    }

    try {
        http().get(url)
            .size_limit(size_limit)
            .on_error([complete](std::string, std::string error, unsigned) {
                complete(make_updater_error(UpdaterError::Code::Network, std::move(error)));
            })
            .on_complete([destination, complete](std::string contents, unsigned) {
                complete(RepositoryUpdaterInternal::write_repository_file(destination, contents));
            })
            .perform();
    } catch (const std::exception &error) {
        complete(make_updater_error(UpdaterError::Code::Network, error.what()));
    }
}

UpdaterError RepositoryUpdater::download_repository_file_sync(const std::string &url,
                                                              const boost::filesystem::path &destination,
                                                              size_t size_limit)
{
    if (url.empty())
        return make_updater_error(UpdaterError::Code::ArchiveUnavailable);
    if (!has_api_request_slot(url))
        return make_updater_error(UpdaterError::Code::RateLimited);

    bool completed = false;
    UpdaterError result = make_updater_error(UpdaterError::Code::Network,
                                             "The repository request did not complete.");
    try {
        http().get(url)
            .size_limit(size_limit)
            .on_error([&result, &completed](std::string, std::string error, unsigned) {
                result = make_updater_error(UpdaterError::Code::Network, std::move(error));
                completed = true;
            })
            .on_complete([&result, &completed, destination](std::string contents, unsigned) {
                result = RepositoryUpdaterInternal::write_repository_file(destination, contents);
                completed = true;
            })
            .perform_sync();
    } catch (const std::exception &error) {
        if (!completed)
            result = make_updater_error(UpdaterError::Code::Network, error.what());
    }
    return result;
}

void RepositoryUpdater::download_repository_changelogs(std::vector<RepositoryChangelogRequest> requests,
                                                       std::function<void(bool)> callback,
                                                       bool force)
{
    m_changelog_service->download(
        std::move(requests), std::move(callback), force,
        [this]() { return sync_in_progress(); },
        [this](const std::string &url) { return has_api_request_slot(url); });
}

void RepositoryUpdater::download_repository_version_changelogs(
    std::vector<RepositoryChangelogVersion> versions,
    const boost::filesystem::path &log_directory,
    const std::string &configured_rest_url,
    std::function<void(bool)> callback,
    bool force)
{
    m_changelog_service->download_versions(
        std::move(versions), log_directory, normalize_repository_rest_url(configured_rest_url),
        std::move(callback), force,
        [this]() { return sync_in_progress(); },
        [this](const std::string &url) { return has_api_request_slot(url); });
}

bool RepositoryUpdater::changelog_download_in_progress() const
{
    return m_changelog_service->download_in_progress();
}

void RepositoryUpdater::finish_repository_refresh(UpdaterError error,
                                                  const RepositoryRefreshFinishedFn &finished)
{
    try {
        finished(std::move(error));
    } catch (...) {
        finish_sync();
        throw;
    }
    finish_sync();
}

} // namespace Slic3r
