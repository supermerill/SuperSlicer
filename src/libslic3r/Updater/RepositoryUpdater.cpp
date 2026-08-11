///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// See RepositoryUpdater.hpp. The implementation is intentionally independent
// from wx and package files so it can coordinate both vendor and plugin HTTP
// operations without knowing what their archives contain.

#include "libslic3r/Updater/RepositoryUpdater.hpp"

#include <cstring>
#include <ctime>
#include <exception>
#include <iterator>
#include <memory>
#include <utility>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include "libslic3r/Updater/UpdaterHttp.hpp"

namespace Slic3r {
namespace {

const size_t k_repository_metadata_size_limit = 64 * 1024;
const std::time_t k_repository_cache_lifetime = 24 * 3600;

// Parsing belongs to the derived updater, but exceptions must not leave the
// parent sync waiting forever. A parse exception is therefore reported as a
// failed repository refresh and logged with the repository id.
bool parse_repository_tags_safely(const std::string &repository_id,
                                  const std::function<bool(const std::string &)> &parse_tags,
                                  const std::string &contents);

// Stores a fully downloaded response and reports local failures separately
// from transport errors. Both sync and async archive paths use this function.
UpdaterError write_repository_file(const boost::filesystem::path &destination, const std::string &contents);

bool parse_repository_tags_safely(const std::string &repository_id,
                                  const std::function<bool(const std::string &)> &parse_tags,
                                  const std::string &contents)
{
    try {
        return parse_tags(contents);
    } catch (const std::exception &error) {
        BOOST_LOG_TRIVIAL(warning) << "Cannot parse repository tags for '" << repository_id << "': " << error.what();
        return false;
    }
}

UpdaterError write_repository_file(const boost::filesystem::path &destination, const std::string &contents)
{
    try {
        if (!destination.parent_path().empty())
            boost::filesystem::create_directories(destination.parent_path());

        boost::nowide::ofstream stream(destination.string(), std::ios::out | std::ios::binary | std::ios::trunc);
        if (!stream)
            return make_updater_error(UpdaterError::Code::Filesystem,
                                      "Cannot create the downloaded repository file.");
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!stream)
            return make_updater_error(UpdaterError::Code::Filesystem,
                                      "Cannot write the downloaded repository file.");
        return UpdaterError();
    } catch (const std::exception &error) {
        return make_updater_error(UpdaterError::Code::Filesystem, error.what());
    }
}

} // namespace

RepositoryUpdater::RepositoryUpdater()
    : RepositoryUpdater(default_updater_http_transport())
{
}

RepositoryUpdater::RepositoryUpdater(UpdaterHttpTransport &http_transport)
    : m_http_transport(http_transport)
{
}

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

void RepositoryUpdater::refresh_repository_tags(const std::string &repository_id,
                                                const std::string &rest_url,
                                                const boost::filesystem::path &cache_file,
                                                bool force,
                                                ParseRepositoryTagsFn parse_tags,
                                                RepositoryRefreshFinishedFn finished)
{
    // Cache parsing, request startup and transport callbacks all converge on
    // this guard. A transport must not release the enclosing sync twice, even
    // if it reports a terminal callback and then throws while unwinding.
    const std::shared_ptr<std::atomic_bool> terminal = std::make_shared<std::atomic_bool>(false);
    const RepositoryRefreshFinishedFn complete = [this, terminal, finished](bool succeeded) {
        if (!terminal->exchange(true))
            finish_repository_refresh(succeeded, finished);
    };

    try {
        if (boost::filesystem::is_regular_file(cache_file) && !force &&
            boost::filesystem::last_write_time(cache_file) + k_repository_cache_lifetime > std::time(nullptr)) {
            boost::nowide::ifstream stream(cache_file.string());
            const std::string contents((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
            complete(parse_repository_tags_safely(repository_id, parse_tags, contents));
            return;
        }
    } catch (const std::exception &error) {
        BOOST_LOG_TRIVIAL(warning) << "Cannot read repository cache for '" << repository_id << "': " << error.what();
        complete(false);
        return;
    }

    const std::string tags_url = rest_url + "/tags?per_page=100;page=1";
    if (rest_url.empty() || !has_api_request_slot(tags_url)) {
        complete(false);
        return;
    }

    try {
        if (!cache_file.parent_path().empty())
            boost::filesystem::create_directories(cache_file.parent_path());
    } catch (const std::exception &error) {
        BOOST_LOG_TRIVIAL(warning) << "Cannot create repository cache for '" << repository_id << "': " << error.what();
        complete(false);
        return;
    }

    try {
        http().get(tags_url)
            .size_limit(k_repository_metadata_size_limit)
            .on_error([repository_id, complete](std::string, std::string error, unsigned) {
                BOOST_LOG_TRIVIAL(warning) << "Cannot update repository '" << repository_id << "': " << error;
                complete(false);
            })
            .on_complete([repository_id, cache_file, parse_tags, complete](std::string contents, unsigned) {
                try {
                    boost::nowide::ofstream stream(cache_file.string(), std::ios::out | std::ios::trunc);
                    stream << contents;
                    if (!stream)
                        BOOST_LOG_TRIVIAL(warning) << "Cannot write repository cache for '" << repository_id << "'.";
                } catch (const std::exception &error) {
                    BOOST_LOG_TRIVIAL(warning) << "Cannot write repository cache for '" << repository_id
                                               << "': " << error.what();
                }
                complete(parse_repository_tags_safely(repository_id, parse_tags, contents));
            })
            .perform();
    } catch (const std::exception &error) {
        BOOST_LOG_TRIVIAL(warning) << "Cannot start repository refresh for '" << repository_id
                                   << "': " << error.what();
        complete(false);
    }
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

    const size_t separator = rest_url.find_last_of('/');
    const std::string fallback_id = separator == std::string::npos ? rest_url : rest_url.substr(separator + 1);
    if (rest_url.empty() || fallback_id.empty()) {
        complete(make_updater_error(UpdaterError::Code::RepositoryNotFound,
                                    "The repository URL is empty or malformed."));
        return;
    }

    const size_t github_marker = rest_url.find("https://api.github.com/repos/");
    const std::string description_url = github_marker == std::string::npos ?
        rest_url + "/description" :
        "https://raw.githubusercontent.com/" +
            rest_url.substr(github_marker + std::strlen("https://api.github.com/repos/")) +
            "/refs/heads/main/description.ini";

    try {
        http().get(description_url)
            .size_limit(k_repository_metadata_size_limit)
            .on_error([complete](std::string, std::string error, unsigned) {
                complete(make_updater_error(UpdaterError::Code::Network, std::move(error)));
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
                complete(write_repository_file(destination, contents));
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
                result = write_repository_file(destination, contents);
                completed = true;
            })
            .perform_sync();
    } catch (const std::exception &error) {
        if (!completed)
            result = make_updater_error(UpdaterError::Code::Network, error.what());
    }
    return result;
}

void RepositoryUpdater::finish_repository_refresh(bool succeeded, const RepositoryRefreshFinishedFn &finished)
{
    try {
        finished(succeeded);
    } catch (...) {
        finish_sync();
        throw;
    }
    finish_sync();
}

} // namespace Slic3r
