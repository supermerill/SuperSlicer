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
#include <sstream>
#include <utility>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/property_tree/json_parser.hpp>

#include "libslic3r/Updater/UpdaterHttp.hpp"

namespace Slic3r {
namespace {

const size_t k_repository_metadata_size_limit = 64 * 1024;
const std::time_t k_repository_cache_lifetime = 24 * 3600;
const size_t k_repository_compare_size_limit = 128 * 1024;
const size_t k_repository_commit_size_limit = 4 * 1024 * 1024;

// Tracks one batch of changelog requests. Several batches may run at once,
// while the parent counter prevents derived updaters from replacing the data
// captured by their store_notes callbacks.
struct RepositoryChangelogState {
    std::atomic_size_t pending = 0;
    std::atomic_bool succeeded = true;
    std::atomic_int *active_downloads = nullptr;
    std::function<void(bool)> callback;
};

// Parsing belongs to the derived updater, but exceptions must not leave the
// parent sync waiting forever. A parse exception is therefore reported as a
// failed repository refresh and logged with the repository id.
bool parse_repository_tags_safely(const std::string &repository_id,
                                  const std::function<bool(const std::string &)> &parse_tags,
                                  const std::string &contents);

// Stores a fully downloaded response and reports local failures separately
// from transport errors. Both sync and async archive paths use this function.
UpdaterError write_repository_file(const boost::filesystem::path &destination, const std::string &contents);

// Extracts either one commit message or the messages from a GitHub comparison.
// Parsing into a temporary string prevents a malformed response from replacing
// notes that were already available to the caller.
bool parse_repository_changelog(const std::string &contents, bool compare, std::string &notes);

// Releases one request from its batch and invokes the aggregate callback only
// after the last cache read or network request has finished.
void complete_repository_changelog_request(const std::shared_ptr<RepositoryChangelogState> &state,
                                            bool succeeded);

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

bool parse_repository_changelog(const std::string &contents, bool compare, std::string &notes)
{
    try {
        boost::property_tree::ptree root;
        std::stringstream stream(contents);
        boost::property_tree::read_json(stream, root);
        if (!compare) {
            notes = root.get<std::string>("commit.message");
            return true;
        }

        notes.clear();
        for (const boost::property_tree::ptree::value_type &entry : root.get_child("commits")) {
            const std::string message = entry.second.get<std::string>("commit.message");
            notes = notes.empty() ? message : message + "\n" + notes;
        }
        return true;
    } catch (const std::exception &error) {
        BOOST_LOG_TRIVIAL(warning) << "Cannot parse repository changelog: " << error.what();
        return false;
    }
}

void complete_repository_changelog_request(const std::shared_ptr<RepositoryChangelogState> &state,
                                            bool succeeded)
{
    if (!succeeded)
        state->succeeded = false;
    if (--state->pending == 0) {
        --*state->active_downloads;
        state->callback(state->succeeded.load());
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
    // Tag parsing replaces the version vectors whose entries receive
    // changelog notes. Refuse a refresh until those callbacks have released
    // their references.
    if (m_pending_changelogs != 0)
        return false;
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

void RepositoryUpdater::download_repository_changelogs(std::vector<RepositoryChangelogRequest> requests,
                                                       std::function<void(bool)> callback,
                                                       bool force)
{
    if (requests.empty()) {
        callback(true);
        return;
    }
    // A tag refresh may clear and rebuild the version records captured by
    // store_notes. Let the caller retry after that refresh has completed.
    if (sync_in_progress()) {
        callback(false);
        return;
    }

    const std::shared_ptr<RepositoryChangelogState> state = std::make_shared<RepositoryChangelogState>();
    state->pending = requests.size();
    state->active_downloads = &m_pending_changelogs;
    state->callback = std::move(callback);
    ++m_pending_changelogs;

    // Each request stores its result through a callback supplied by the
    // derived updater. Keeping parsing here gives vendors and plugins exactly
    // the same JSON and failure semantics without sharing their public models.
    const std::function<bool(const RepositoryChangelogRequest &, const std::string &)> consume =
        [](const RepositoryChangelogRequest &request, const std::string &contents) {
            std::string notes;
            if (!parse_repository_changelog(contents, request.compare, notes))
                return false;
            try {
                request.store_notes(std::move(notes));
                return true;
            } catch (const std::exception &error) {
                BOOST_LOG_TRIVIAL(warning) << "Cannot store repository changelog: " << error.what();
                return false;
            }
        };

    for (const RepositoryChangelogRequest &request : requests) {
        const std::shared_ptr<std::atomic_bool> terminal = std::make_shared<std::atomic_bool>(false);
        const std::function<void(bool)> complete = [state, terminal](bool succeeded) {
            if (!terminal->exchange(true))
                complete_repository_changelog_request(state, succeeded);
        };

        // A cache entry represents immutable commit data, so it has no expiry.
        // force bypasses it when the caller wants to repair or refresh files.
        try {
            if (!force && boost::filesystem::is_regular_file(request.cache_file)) {
                boost::nowide::ifstream stream(request.cache_file.string());
                if (!stream) {
                    complete(false);
                    continue;
                }
                const std::string contents((std::istreambuf_iterator<char>(stream)),
                                           std::istreambuf_iterator<char>());
                complete(consume(request, contents));
                continue;
            }
        } catch (const std::exception &error) {
            BOOST_LOG_TRIVIAL(warning) << "Cannot read repository changelog cache '"
                                       << request.cache_file.string() << "': " << error.what();
            complete(false);
            continue;
        }

        if (request.url.empty() || !has_api_request_slot(request.url)) {
            complete(false);
            continue;
        }

        try {
            http().get(request.url)
                .size_limit(request.compare ? k_repository_compare_size_limit : k_repository_commit_size_limit)
                .on_error([request, complete](std::string, std::string error, unsigned) {
                    BOOST_LOG_TRIVIAL(warning) << "Cannot download repository changelog '"
                                               << request.url << "': " << error;
                    complete(false);
                })
                .on_complete([request, consume, complete](std::string contents, unsigned) {
                    const UpdaterError cache_error = write_repository_file(request.cache_file, contents);
                    if (!cache_error.succeeded())
                        BOOST_LOG_TRIVIAL(warning) << "Cannot cache repository changelog '"
                                                   << request.cache_file.string() << "': " << cache_error.detail;
                    complete(consume(request, contents));
                })
                .perform();
        } catch (const std::exception &error) {
            BOOST_LOG_TRIVIAL(warning) << "Cannot start repository changelog request '"
                                       << request.url << "': " << error.what();
            complete(false);
        }
    }
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
