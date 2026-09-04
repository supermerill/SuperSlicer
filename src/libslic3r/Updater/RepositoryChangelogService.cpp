///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher

// See RepositoryChangelogService.hpp. Every request validates JSON before it
// replaces a cache or writes notes into a model. A batch owns one aggregate
// completion state so partial network failures still produce one callback.

#include "libslic3r/Updater/RepositoryChangelogService.hpp"

#include <ctime>
#include <exception>
#include <memory>
#include <sstream>
#include <utility>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/json_parser.hpp>

#include "libslic3r/Updater/RepositoryCacheIO.hpp"
#include "libslic3r/Updater/UpdaterHttp.hpp"

namespace Slic3r {
namespace RepositoryUpdaterInternal {
namespace {

const std::time_t k_repository_cache_lifetime = 24 * 3600;
const size_t k_repository_compare_size_limit = 128 * 1024;
const size_t k_repository_commit_size_limit = 4 * 1024 * 1024;

// Several asynchronous requests contribute to one user-visible changelog
// operation. The final callback runs only after the last request completes.
struct RepositoryChangelogState {
    std::atomic_size_t pending = 0;
    std::atomic_bool succeeded = true;
    std::atomic_int *active_downloads = nullptr;
    RepositoryChangelogService::FinishedFn callback;
};

// Parses a single commit response into one note.
bool parse_repository_commit_changelog(const std::string &contents, std::string &notes);

// Parses one unpaginated GitHub compare response. GitHub limits this form to
// 250 commits, so a changelog spanning more commits will be incomplete. This
// is intentional: plugin and vendor releases are not expected to exceed that
// range, and the extra requests and state needed for pagination are not
// justified for this unlikely case.
bool parse_repository_compare_changelog(const std::string &contents, std::string &notes);

// Selects the parser that belongs to the explicit request kind.
bool parse_repository_changelog(const std::string &contents,
                                RepositoryChangelogKind kind,
                                std::string &notes);

// Publishes validated notes through the callback owned by a derived updater.
bool store_repository_changelog_notes(const std::function<void(std::string)> &store_notes,
                                      std::string notes);

// Releases one request and invokes the batch callback after the final result.
void complete_repository_changelog_request(const std::shared_ptr<RepositoryChangelogState> &state,
                                            bool succeeded);

// Returns the response limit appropriate for one endpoint kind.
size_t repository_changelog_size_limit(RepositoryChangelogKind kind);

bool parse_repository_commit_changelog(const std::string &contents, std::string &notes)
{
    try {
        boost::property_tree::ptree root;
        std::stringstream stream(contents);
        boost::property_tree::read_json(stream, root);
        notes = root.get<std::string>("commit.message");
        return true;
    } catch (const std::exception &error) {
        BOOST_LOG_TRIVIAL(warning) << "Cannot parse repository commit changelog: " << error.what();
        return false;
    }
}

bool parse_repository_compare_changelog(const std::string &contents, std::string &notes)
{
    try {
        boost::property_tree::ptree root;
        std::stringstream stream(contents);
        boost::property_tree::read_json(stream, root);

        notes.clear();
        for (const boost::property_tree::ptree::value_type &entry : root.get_child("commits")) {
            const std::string message = entry.second.get<std::string>("commit.message");
            notes = notes.empty() ? message : message + "\n" + notes;
        }
        return true;
    } catch (const std::exception &error) {
        BOOST_LOG_TRIVIAL(warning) << "Cannot parse repository comparison changelog: " << error.what();
        return false;
    }
}

bool parse_repository_changelog(const std::string &contents,
                                RepositoryChangelogKind kind,
                                std::string &notes)
{
    return kind == RepositoryChangelogKind::Compare ?
        parse_repository_compare_changelog(contents, notes) :
        parse_repository_commit_changelog(contents, notes);
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

size_t repository_changelog_size_limit(RepositoryChangelogKind kind)
{
    return kind == RepositoryChangelogKind::Compare ?
        k_repository_compare_size_limit : k_repository_commit_size_limit;
}

} // namespace

RepositoryChangelogService::RepositoryChangelogService(UpdaterHttpTransport &http_transport)
    : m_http_transport(http_transport)
{
}

void RepositoryChangelogService::download(std::vector<RepositoryChangelogRequest> requests,
                                          FinishedFn finished,
                                          bool force,
                                          SyncInProgressFn sync_in_progress,
                                          ReserveRequestSlotFn reserve_request_slot)
{
    if (requests.empty()) {
        finished(true);
        return;
    }
    // Tag parsing may rebuild records captured by store_notes. Refuse this
    // batch until that model update has completed.
    if (sync_in_progress()) {
        finished(false);
        return;
    }

    const std::shared_ptr<RepositoryChangelogState> state = std::make_shared<RepositoryChangelogState>();
    state->pending = requests.size();
    state->active_downloads = &m_pending_downloads;
    state->callback = std::move(finished);
    ++m_pending_downloads;

    for (const RepositoryChangelogRequest &request : requests) {
        const std::shared_ptr<std::atomic_bool> terminal = std::make_shared<std::atomic_bool>(false);
        const std::function<void(bool)> complete = [state, terminal](bool succeeded) {
            if (!terminal->exchange(true))
                complete_repository_changelog_request(state, succeeded);
        };

        // Recent validated JSON is restored without consuming another request.
        try {
            if (!force && boost::filesystem::is_regular_file(request.cache_file) &&
                boost::filesystem::last_write_time(request.cache_file) + k_repository_cache_lifetime >
                    std::time(nullptr)) {
                std::string contents;
                bool exists = false;
                const UpdaterError read_error = read_repository_cache_file(request.cache_file, contents, exists);
                if (!read_error.succeeded() || !exists) {
                    complete(false);
                    continue;
                }

                std::string notes;
                if (!parse_repository_changelog(contents, request.kind, notes)) {
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

        if (request.url.empty() || !reserve_request_slot(request.url)) {
            complete(false);
            continue;
        }

        try {
            m_http_transport.get(request.url)
                .size_limit(repository_changelog_size_limit(request.kind))
                .on_error([request, complete](std::string, std::string error, unsigned) {
                    BOOST_LOG_TRIVIAL(warning) << "Cannot download repository changelog '"
                                               << request.url << "': " << error;
                    complete(false);
                })
                .on_complete([request, complete](std::string contents, unsigned) {
                    std::string notes;
                    if (!parse_repository_changelog(contents, request.kind, notes)) {
                        complete(false);
                        return;
                    }

                    // Notes are useful even if this optional cache cannot be
                    // published on the current machine.
                    const UpdaterError cache_error =
                        publish_repository_cache_atomically(request.cache_file, contents);
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

void RepositoryChangelogService::download_versions(std::vector<RepositoryChangelogVersion> versions,
                                                   const boost::filesystem::path &log_directory,
                                                   const std::string &repository_url,
                                                   FinishedFn finished,
                                                   bool force,
                                                   SyncInProgressFn sync_in_progress,
                                                   ReserveRequestSlotFn reserve_request_slot)
{
    std::vector<RepositoryChangelogRequest> requests;
    for (RepositoryChangelogVersion &version : versions) {
        // Releases with local notes remain comparison bases, but need no
        // request of their own. Preset callers always supply a destination.
        if (version.commit_sha.empty() || !version.store_notes)
            continue;

        // Prefer an older package from the same slicer family so changelog
        // notes describe package changes rather than compatibility changes.
        RepositoryChangelogVersion *previous = nullptr;
        for (RepositoryChangelogVersion &candidate : versions) {
            if (candidate.commit_sha.empty() || candidate.content_version >= version.content_version)
                continue;
            if (candidate.slicer_version.no_patch() == version.slicer_version.no_patch() &&
                (previous == nullptr || candidate.content_version > previous->content_version))
                previous = &candidate;
        }

        // If that family has no predecessor, use the newest older package that
        // did not require a newer slicer family.
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
        request.kind = previous != nullptr && !repository_url.empty() ?
            RepositoryChangelogKind::Compare : RepositoryChangelogKind::Commit;
        const bool compare = request.kind == RepositoryChangelogKind::Compare;
        request.cache_file = log_directory /
            (compare ? previous->tag + "..." + version.tag + ".json" : version.tag + ".json");
        request.url = compare ? repository_url + "/compare/" + previous->tag + "..." + version.tag :
                                version.commit_url;
        request.store_notes = std::move(version.store_notes);
        requests.emplace_back(std::move(request));
    }

    download(std::move(requests), std::move(finished), force,
             std::move(sync_in_progress), std::move(reserve_request_slot));
}

} // namespace RepositoryUpdaterInternal
} // namespace Slic3r
