///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// See RepositoryUpdater.hpp. The implementation is intentionally independent
// from wx and package files so it can coordinate both vendor and plugin HTTP
// operations without knowing what their archives contain.

#include "libslic3r/Updater/RepositoryUpdater.hpp"

#include <algorithm>
#include <cctype>
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
UpdaterError parse_repository_tags_safely(const std::string &repository_id,
                                          const std::function<UpdaterError(const std::string &)> &parse_tags,
                                          const std::string &contents);

// Publishes validated metadata through a sibling temporary file. The previous
// cache remains available until the completed temporary file is renamed into
// place, and is restored if that rename fails.
UpdaterError publish_repository_cache_atomically(const boost::filesystem::path &destination,
                                                  const std::string &contents);

// Stores a fully downloaded response and reports local failures separately
// from transport errors. Both sync and async archive paths use this function.
UpdaterError write_repository_file(const boost::filesystem::path &destination, const std::string &contents);

// Extracts either one commit message or the messages from a GitHub comparison.
// Parsing into a temporary string prevents a malformed response from replacing
// notes that were already available to the caller.
bool parse_repository_changelog(const std::string &contents, bool compare, std::string &notes);

// Publishes validated notes through the callback owned by the derived updater.
// Callback failures belong to this changelog request and must not escape its
// caller or prevent the batch from completing.
bool store_repository_changelog_notes(const std::function<void(std::string)> &store_notes,
                                      std::string notes);

// Releases one request from its batch and invokes the aggregate callback only
// after the last cache read or network request has finished.
void complete_repository_changelog_request(const std::shared_ptr<RepositoryChangelogState> &state,
                                            bool succeeded);

UpdaterError parse_repository_tags_safely(const std::string &repository_id,
                                          const std::function<UpdaterError(const std::string &)> &parse_tags,
                                          const std::string &contents)
{
    try {
        UpdaterError error = parse_tags(contents);
        if (!error.succeeded())
            BOOST_LOG_TRIVIAL(warning) << "Cannot parse repository tags for '" << repository_id << "': "
                                       << error.detail;
        return error;
    } catch (const std::exception &error) {
        BOOST_LOG_TRIVIAL(warning) << "Cannot parse repository tags for '" << repository_id << "': " << error.what();
        return make_updater_error(UpdaterError::Code::InvalidRepositoryMetadata, error.what());
    }
}

UpdaterError publish_repository_cache_atomically(const boost::filesystem::path &destination,
                                                  const std::string &contents)
{
    const boost::filesystem::path parent = destination.parent_path();
    const std::string filename = destination.filename().string();
    const boost::filesystem::path staging = parent /
        boost::filesystem::unique_path("." + filename + ".download-%%%%-%%%%");
    const boost::filesystem::path backup = parent /
        boost::filesystem::unique_path("." + filename + ".previous-%%%%-%%%%");
    bool previous_moved = false;

    try {
        if (!parent.empty())
            boost::filesystem::create_directories(parent);

        const UpdaterError write_error = write_repository_file(staging, contents);
        if (!write_error.succeeded()) {
            boost::system::error_code cleanup_error;
            boost::filesystem::remove(staging, cleanup_error);
            return write_error;
        }

        // Windows cannot rename a file over an existing destination. Keep the
        // previous cache beside the staging file until publication succeeds.
        if (boost::filesystem::exists(destination)) {
            if (!boost::filesystem::is_regular_file(destination)) {
                boost::system::error_code cleanup_error;
                boost::filesystem::remove(staging, cleanup_error);
                return make_updater_error(UpdaterError::Code::Filesystem,
                                          "The repository cache destination is not a regular file.");
            }
            boost::filesystem::rename(destination, backup);
            previous_moved = true;
        }

        try {
            boost::filesystem::rename(staging, destination);
        } catch (const boost::filesystem::filesystem_error &error) {
            std::string detail = error.what();
            if (previous_moved && !boost::filesystem::exists(destination)) {
                boost::system::error_code restore_error;
                boost::filesystem::rename(backup, destination, restore_error);
                if (restore_error)
                    detail += "; restoring the previous cache also failed: " + restore_error.message();
            }
            boost::system::error_code cleanup_error;
            boost::filesystem::remove(staging, cleanup_error);
            return make_updater_error(UpdaterError::Code::Filesystem, std::move(detail));
        }

        if (previous_moved) {
            boost::system::error_code cleanup_error;
            boost::filesystem::remove(backup, cleanup_error);
            if (cleanup_error)
                BOOST_LOG_TRIVIAL(warning) << "Cannot remove previous repository cache '"
                                           << backup.string() << "': " << cleanup_error.message();
        }
        return UpdaterError();
    } catch (const std::exception &error) {
        boost::system::error_code cleanup_error;
        boost::filesystem::remove(staging, cleanup_error);
        if (previous_moved && !boost::filesystem::exists(destination)) {
            boost::system::error_code restore_error;
            boost::filesystem::rename(backup, destination, restore_error);
            if (restore_error)
                return make_updater_error(UpdaterError::Code::Filesystem,
                    std::string(error.what()) + "; restoring the previous cache also failed: " +
                    restore_error.message());
        }
        return make_updater_error(UpdaterError::Code::Filesystem, error.what());
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
        stream.flush();
        if (!stream)
            return make_updater_error(UpdaterError::Code::Filesystem,
                                      "Cannot flush the downloaded repository file.");
        stream.close();
        if (!stream)
            return make_updater_error(UpdaterError::Code::Filesystem,
                                      "Cannot close the downloaded repository file.");
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

bool store_repository_changelog_notes(const std::function<void(std::string)> &store_notes,
                                      std::string notes)
{
    try {
        store_notes(std::move(notes));
        return true;
    } catch (const std::exception &error) {
        BOOST_LOG_TRIVIAL(warning) << "Cannot store repository changelog: " << error.what();
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

std::string RepositoryUpdater::normalize_repository_rest_url(const std::string &configured_url)
{
    std::string normalized = configured_url;
    while (!normalized.empty() && normalized.back() == '/')
        normalized.pop_back();
    if (normalized.empty())
        return normalized;

    // A value without a scheme may be either the common GitHub owner/project
    // shorthand or a fully named host. Give both an explicit HTTPS scheme so
    // all later parsing follows the same path.
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

    std::string repository_path = path_start == std::string::npos ? std::string() : normalized.substr(path_start + 1);
    if (host == "api.github.com" && repository_path.rfind("repos/", 0) == 0)
        repository_path.erase(0, std::strlen("repos/"));
    if (repository_path.size() > 4 && repository_path.compare(repository_path.size() - 4, 4, ".git") == 0)
        repository_path.erase(repository_path.size() - 4);
    return repository_path.empty() ? std::string() : "https://api.github.com/repos/" + repository_path;
}

bool RepositoryUpdater::begin_sync(size_t repository_count, std::function<void(int)> callback)
{
    // Tag parsing replaces the version vectors whose entries receive
    // changelog notes. Refuse a refresh until those callbacks have released
    // their references, but still complete the caller's request immediately.
    if (m_pending_changelogs != 0) {
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
    // Derived models finalize their aggregate state before update_count() and
    // before any GUI subscriber rebuilds itself from that model.
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
    // Cache parsing, request startup and transport callbacks all converge on
    // this guard. A transport must not release the enclosing sync twice, even
    // if it reports a terminal callback and then throws while unwinding.
    const std::shared_ptr<std::atomic_bool> terminal = std::make_shared<std::atomic_bool>(false);
    const RepositoryRefreshFinishedFn complete = [this, terminal, finished](UpdaterError error) {
        if (!terminal->exchange(true))
            finish_repository_refresh(std::move(error), finished);
    };

    try {
        if (boost::filesystem::is_regular_file(cache_file) && !force &&
            boost::filesystem::last_write_time(cache_file) + k_repository_cache_lifetime > std::time(nullptr)) {
            boost::nowide::ifstream stream(cache_file.string());
            if (!stream) {
                complete(make_updater_error(UpdaterError::Code::Filesystem,
                                            "Cannot read repository cache '" + cache_file.string() + "'."));
                return;
            }
            const std::string contents((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
            complete(parse_repository_tags_safely(repository_id, parse_tags, contents));
            return;
        }
    } catch (const std::exception &error) {
        BOOST_LOG_TRIVIAL(warning) << "Cannot read repository cache for '" << repository_id << "': " << error.what();
        complete(make_updater_error(UpdaterError::Code::Filesystem, error.what()));
        return;
    }

    const std::string repository_url = normalize_repository_rest_url(rest_url);
    const std::string tags_url = repository_url + "/tags?per_page=100;page=1";
    if (repository_url.empty()) {
        complete(make_updater_error(UpdaterError::Code::RepositoryNotFound,
                                    "The repository URL is empty or malformed."));
        return;
    }
    if (!has_api_request_slot(tags_url)) {
        complete(make_updater_error(UpdaterError::Code::RateLimited,
                                    "The GitHub API request limit has been reached."));
        return;
    }

    try {
        if (!cache_file.parent_path().empty())
            boost::filesystem::create_directories(cache_file.parent_path());
    } catch (const std::exception &error) {
        BOOST_LOG_TRIVIAL(warning) << "Cannot create repository cache for '" << repository_id << "': " << error.what();
        complete(make_updater_error(UpdaterError::Code::Filesystem, error.what()));
        return;
    }

    try {
        http().get(tags_url)
            .size_limit(k_repository_metadata_size_limit)
            .on_error([repository_id, complete](std::string, std::string error, unsigned status) {
                BOOST_LOG_TRIVIAL(warning) << "Cannot update repository '" << repository_id << "': " << error;
                const UpdaterError::Code code = status == 404 ? UpdaterError::Code::RepositoryNotFound :
                                                               UpdaterError::Code::Network;
                complete(make_updater_error(code, std::move(error)));
            })
            .on_complete([repository_id, cache_file, parse_tags, complete](std::string contents, unsigned) {
                UpdaterError parse_error = parse_repository_tags_safely(repository_id, parse_tags, contents);
                if (!parse_error.succeeded()) {
                    complete(std::move(parse_error));
                    return;
                }

                const UpdaterError cache_error = publish_repository_cache_atomically(cache_file, contents);
                if (!cache_error.succeeded())
                    BOOST_LOG_TRIVIAL(warning) << "Cannot write repository cache for '" << repository_id
                                               << "': " << cache_error.detail;
                complete(std::move(parse_error));
            })
            .perform();
    } catch (const std::exception &error) {
        BOOST_LOG_TRIVIAL(warning) << "Cannot start repository refresh for '" << repository_id
                                   << "': " << error.what();
        complete(make_updater_error(UpdaterError::Code::Network, error.what()));
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

    for (const RepositoryChangelogRequest &request : requests) {
        const std::shared_ptr<std::atomic_bool> terminal = std::make_shared<std::atomic_bool>(false);
        const std::function<void(bool)> complete = [state, terminal](bool succeeded) {
            if (!terminal->exchange(true))
                complete_repository_changelog_request(state, succeeded);
        };

        // Reuse recent notes while still allowing repositories to repair tags
        // or commit metadata that was rewritten after an initial publication.
        try {
            if (!force && boost::filesystem::is_regular_file(request.cache_file) &&
                boost::filesystem::last_write_time(request.cache_file) + k_repository_cache_lifetime >
                    std::time(nullptr)) {
                boost::nowide::ifstream stream(request.cache_file.string());
                if (!stream) {
                    complete(false);
                    continue;
                }
                const std::string contents((std::istreambuf_iterator<char>(stream)),
                                           std::istreambuf_iterator<char>());

                // Cache contents still pass through the common parser before
                // their notes are exposed to the derived updater's model.
                std::string notes;
                if (!parse_repository_changelog(contents, request.compare, notes)) {
                    complete(false);
                    continue;
                }
                complete(store_repository_changelog_notes(request.store_notes, std::move(notes)));
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
                .on_complete([request, complete](std::string contents, unsigned) {
                    std::string notes;
                    if (!parse_repository_changelog(contents, request.compare, notes)) {
                        complete(false);
                        return;
                    }

                    // Keep the validated notes usable even if the optional
                    // local cache cannot be replaced on this machine.
                    const UpdaterError cache_error = publish_repository_cache_atomically(
                        request.cache_file, contents);
                    if (!cache_error.succeeded())
                        BOOST_LOG_TRIVIAL(warning) << "Cannot cache repository changelog '"
                                                   << request.cache_file.string() << "': " << cache_error.detail;
                    complete(store_repository_changelog_notes(request.store_notes, std::move(notes)));
                })
                .perform();
        } catch (const std::exception &error) {
            BOOST_LOG_TRIVIAL(warning) << "Cannot start repository changelog request '"
                                       << request.url << "': " << error.what();
            complete(false);
        }
    }
}

void RepositoryUpdater::download_repository_version_changelogs(
    std::vector<RepositoryChangelogVersion> versions,
    const boost::filesystem::path &log_directory,
    const std::string &configured_rest_url,
    std::function<void(bool)> callback,
    bool force)
{
    std::vector<RepositoryChangelogRequest> requests;
    const std::string repository_url = normalize_repository_rest_url(configured_rest_url);
    for (RepositoryChangelogVersion &version : versions) {
        if (version.commit_sha.empty())
            continue;

        // Prefer an older package produced for the same slicer family. This
        // keeps a comparison focused on package changes instead of mixing in
        // compatibility work for an unrelated slicer release.
        RepositoryChangelogVersion *previous = nullptr;
        for (RepositoryChangelogVersion &candidate : versions) {
            if (candidate.commit_sha.empty() || candidate.content_version >= version.content_version)
                continue;
            if (candidate.slicer_version.no_patch() == version.slicer_version.no_patch() &&
                (previous == nullptr || candidate.content_version > previous->content_version))
                previous = &candidate;
        }

        // A repository may publish its first package for a new slicer family
        // without a same-family predecessor. In that case, compare it with the
        // newest older package that did not require a newer slicer.
        if (previous == nullptr) {
            for (RepositoryChangelogVersion &candidate : versions) {
                if (candidate.commit_sha.empty() || candidate.content_version >= version.content_version ||
                    candidate.slicer_version.no_patch() > version.slicer_version.no_patch())
                    continue;
                if (previous == nullptr || candidate.content_version > previous->content_version)
                    previous = &candidate;
            }
        }

        RepositoryChangelogRequest request;
        request.compare = previous != nullptr && !repository_url.empty();
        request.cache_file = log_directory /
            (request.compare ? previous->tag + "..." + version.tag + ".json" : version.tag + ".json");
        request.url = request.compare ? repository_url + "/compare/" + previous->tag + "..." + version.tag :
                                        version.commit_url;
        request.store_notes = std::move(version.store_notes);
        requests.emplace_back(std::move(request));
    }
    download_repository_changelogs(std::move(requests), std::move(callback), force);
}

void RepositoryUpdater::finish_repository_refresh(UpdaterError error, const RepositoryRefreshFinishedFn &finished)
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
