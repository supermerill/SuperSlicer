///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher

// See RepositoryTagService.hpp. tags.json remains a GitHub-compatible array;
// tags.pagination.ini stores only the daily cursor. All pages of one logical
// update are kept in memory until the domain parser and cache publication have
// both accepted the aggregate.

#include "libslic3r/Updater/RepositoryTagService.hpp"

#include <cstdint>
#include <ctime>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/json_parser.hpp>

#include "libslic3r/Updater/RepositoryCacheIO.hpp"
#include "libslic3r/Updater/UpdaterHttp.hpp"

namespace Slic3r {
namespace RepositoryUpdaterInternal {
namespace {

const std::time_t k_repository_cache_lifetime = 24 * 3600;
const size_t k_repository_metadata_size_limit = 64 * 1024;
const size_t k_repository_tags_per_page = 100;

// Each entry keeps the complete response object because vendor and plugin
// parsers need archive and commit links after pages have been merged.
struct RepositoryTagEntry {
    std::string name;
    boost::property_tree::ptree value;
};

// The cursor is deliberately separate from tags.json so existing package
// parsers continue to consume a plain aggregate array.
struct RepositoryTagPagination {
    uint32_t next_page = 1;
    bool history_complete = false;
    std::time_t last_request = 0;
};

// Recursive HTTP callbacks share this transaction state. No page is written
// before the complete logical refresh has succeeded.
struct RepositoryTagRefreshState {
    UpdaterHttpTransport *http = nullptr;
    std::string repository_id;
    std::string repository_url;
    boost::filesystem::path cache_file;
    boost::filesystem::path pagination_file;
    RepositoryTagService::ParseTagsFn parse_tags;
    RepositoryTagService::FinishedFn complete;
    RepositoryTagService::ReserveRequestSlotFn reserve_request_slot;
    bool github = false;
    bool daily_attempt_recorded = false;
    bool refresh_front_after_overlap = false;
    RepositoryTagPagination pagination;
    RepositoryTagPagination pagination_after_history_page;
    std::vector<RepositoryTagEntry> working_tags;
};

// Protects the facade from exceptions raised by a vendor or plugin parser and
// converts them into the same metadata error as malformed JSON.
UpdaterError parse_repository_tags_safely(const std::string &repository_id,
                                          const RepositoryTagService::ParseTagsFn &parse_tags,
                                          const std::string &contents);

// Converts one JSON page into named entries used for deterministic overlap
// detection and deduplication.
UpdaterError parse_repository_tag_page(const std::string &contents,
                                       std::vector<RepositoryTagEntry> &entries);

// Serializes the aggregate to the array shape accepted by existing parsers.
std::string serialize_repository_tags(const std::vector<RepositoryTagEntry> &entries);

// Appends unseen historical entries and reports whether this page overlaps
// tags that were already cached from a preceding page.
bool merge_repository_tag_history(std::vector<RepositoryTagEntry> &aggregate,
                                  const std::vector<RepositoryTagEntry> &page,
                                  size_t &added_count);

// Places page one first while preserving all distinct historical entries.
void merge_repository_tag_front(std::vector<RepositoryTagEntry> &aggregate,
                                const std::vector<RepositoryTagEntry> &page);

// Derives tags.pagination.ini without extending the public cache API.
boost::filesystem::path repository_tag_pagination_path(const boost::filesystem::path &cache_file);

// Loads and validates the page cursor, reporting a missing file separately so
// a pre-pagination tags.json can be adopted in place.
UpdaterError read_repository_tag_pagination(const boost::filesystem::path &path,
                                            RepositoryTagPagination &pagination,
                                            bool &exists);

// Serialize the cursor independently so it can participate in a batch commit.
std::string serialize_repository_tag_pagination(const RepositoryTagPagination &pagination);

// Atomically writes the cursor and daily request timestamp when no tags file
// is part of the same logical publication.
UpdaterError publish_repository_tag_pagination(const boost::filesystem::path &path,
                                               const RepositoryTagPagination &pagination);

// Requests one page and passes only validated arrays to the merge policy.
void request_repository_tag_page(const std::shared_ptr<RepositoryTagRefreshState> &state,
                                 uint32_t page_number);

// Applies one page and either continues pagination or finalizes the aggregate.
void consume_repository_tag_page(const std::shared_ptr<RepositoryTagRefreshState> &state,
                                 uint32_t page_number,
                                 std::vector<RepositoryTagEntry> page);

// Runs the domain parser once, then publishes tags.json and finally its cursor.
void finalize_repository_tag_refresh(const std::shared_ptr<RepositoryTagRefreshState> &state);

UpdaterError parse_repository_tags_safely(const std::string &repository_id,
                                          const RepositoryTagService::ParseTagsFn &parse_tags,
                                          const std::string &contents)
{
    try {
        UpdaterError error = parse_tags(contents);
        if (!error.succeeded())
            BOOST_LOG_TRIVIAL(warning) << "Cannot parse repository tags for '" << repository_id << "': "
                                       << error.detail;
        return error;
    } catch (const std::exception &error) {
        BOOST_LOG_TRIVIAL(warning) << "Cannot parse repository tags for '" << repository_id << "': "
                                   << error.what();
        return make_updater_error(UpdaterError::Code::InvalidRepositoryMetadata, error.what());
    }
}

UpdaterError parse_repository_tag_page(const std::string &contents,
                                       std::vector<RepositoryTagEntry> &entries)
{
    try {
        const size_t first = contents.find_first_not_of(" \t\r\n");
        const size_t last = contents.find_last_not_of(" \t\r\n");
        if (first == std::string::npos || contents[first] != '[' || contents[last] != ']')
            return make_updater_error(UpdaterError::Code::InvalidRepositoryMetadata,
                                      "Repository tags must be a JSON array.");

        boost::property_tree::ptree root;
        std::stringstream stream(contents);
        boost::property_tree::read_json(stream, root);

        std::map<std::string, size_t> known_names;
        entries.clear();
        for (const boost::property_tree::ptree::value_type &item : root) {
            if (!item.first.empty())
                return make_updater_error(UpdaterError::Code::InvalidRepositoryMetadata,
                                          "Repository tags must be an array of objects.");
            const std::string name = item.second.get<std::string>("name", std::string());
            if (name.empty())
                return make_updater_error(UpdaterError::Code::InvalidRepositoryMetadata,
                                          "A repository tag has no name.");

            const std::map<std::string, size_t>::const_iterator known = known_names.find(name);
            if (known == known_names.end()) {
                known_names.emplace(name, entries.size());
                entries.push_back(RepositoryTagEntry{name, item.second});
            } else {
                entries[known->second].value = item.second;
            }
        }
        return UpdaterError();
    } catch (const std::exception &error) {
        return make_updater_error(UpdaterError::Code::InvalidRepositoryMetadata,
                                  "Cannot parse repository tags: " + std::string(error.what()));
    }
}

std::string serialize_repository_tags(const std::vector<RepositoryTagEntry> &entries)
{
    std::stringstream stream;
    stream << '[';
    for (size_t index = 0; index < entries.size(); ++index) {
        if (index != 0)
            stream << ',';
        std::stringstream entry_stream;
        boost::property_tree::write_json(entry_stream, entries[index].value, false);
        std::string entry_json = entry_stream.str();
        while (!entry_json.empty() && (entry_json.back() == '\r' || entry_json.back() == '\n'))
            entry_json.pop_back();
        stream << entry_json;
    }
    stream << ']';
    return stream.str();
}

bool merge_repository_tag_history(std::vector<RepositoryTagEntry> &aggregate,
                                  const std::vector<RepositoryTagEntry> &page,
                                  size_t &added_count)
{
    std::map<std::string, size_t> known_names;
    for (size_t index = 0; index < aggregate.size(); ++index)
        known_names.emplace(aggregate[index].name, index);

    bool overlap = false;
    added_count = 0;
    for (const RepositoryTagEntry &entry : page) {
        const std::map<std::string, size_t>::const_iterator known = known_names.find(entry.name);
        if (known != known_names.end()) {
            overlap = true;
            aggregate[known->second].value = entry.value;
            continue;
        }

        known_names.emplace(entry.name, aggregate.size());
        aggregate.emplace_back(entry);
        ++added_count;
    }
    return overlap;
}

void merge_repository_tag_front(std::vector<RepositoryTagEntry> &aggregate,
                                const std::vector<RepositoryTagEntry> &page)
{
    std::vector<RepositoryTagEntry> merged;
    merged.reserve(page.size() + aggregate.size());
    std::map<std::string, size_t> known_names;

    for (const RepositoryTagEntry &entry : page) {
        known_names.emplace(entry.name, merged.size());
        merged.emplace_back(entry);
    }
    for (const RepositoryTagEntry &entry : aggregate) {
        if (known_names.emplace(entry.name, merged.size()).second)
            merged.emplace_back(entry);
    }
    aggregate.swap(merged);
}

boost::filesystem::path repository_tag_pagination_path(const boost::filesystem::path &cache_file)
{
    return cache_file.parent_path() / (cache_file.stem().string() + ".pagination.ini");
}

UpdaterError read_repository_tag_pagination(const boost::filesystem::path &path,
                                            RepositoryTagPagination &pagination,
                                            bool &exists)
{
    try {
        exists = boost::filesystem::is_regular_file(path);
        if (!exists)
            return UpdaterError();

        boost::property_tree::ptree root;
        boost::property_tree::read_ini(path.string(), root);
        const uint64_t next_page = root.get<uint64_t>("pagination.next_page");
        const int64_t last_request = root.get<int64_t>("pagination.last_request", 0);
        if (next_page == 0 || next_page > std::numeric_limits<uint32_t>::max() || last_request < 0)
            return make_updater_error(UpdaterError::Code::Cache,
                                      "Repository tag pagination contains an invalid cursor.");

        pagination.next_page = static_cast<uint32_t>(next_page);
        pagination.history_complete = root.get<bool>("pagination.history_complete", false);
        pagination.last_request = static_cast<std::time_t>(last_request);
        return UpdaterError();
    } catch (const std::exception &error) {
        return make_updater_error(UpdaterError::Code::Cache,
                                  "Cannot parse repository tag pagination: " + std::string(error.what()));
    }
}

std::string serialize_repository_tag_pagination(const RepositoryTagPagination &pagination)
{
    boost::property_tree::ptree root;
    root.put("pagination.next_page", pagination.next_page);
    root.put("pagination.history_complete", pagination.history_complete);
    root.put("pagination.last_request", static_cast<int64_t>(pagination.last_request));

    std::stringstream stream;
    boost::property_tree::write_ini(stream, root);
    return stream.str();
}

UpdaterError publish_repository_tag_pagination(const boost::filesystem::path &path,
                                               const RepositoryTagPagination &pagination)
{
    return publish_repository_cache_atomically(path, serialize_repository_tag_pagination(pagination));
}

void request_repository_tag_page(const std::shared_ptr<RepositoryTagRefreshState> &state,
                                 uint32_t page_number)
{
    const std::string tags_url = state->repository_url + "/tags?per_page=" +
        std::to_string(k_repository_tags_per_page) + "&page=" + std::to_string(page_number);
    if (state->github && !state->reserve_request_slot(tags_url)) {
        state->complete(make_updater_error(UpdaterError::Code::RateLimited,
                                           "The GitHub API request limit has been reached."));
        return;
    }

    // Persist the first attempt before transport startup. Failed or malformed
    // responses therefore consume the normal daily request as intended.
    if (state->github && !state->daily_attempt_recorded) {
        state->pagination.last_request = std::time(nullptr);
        const UpdaterError state_error =
            publish_repository_tag_pagination(state->pagination_file, state->pagination);
        if (!state_error.succeeded()) {
            state->complete(state_error);
            return;
        }
        state->daily_attempt_recorded = true;
    }

    try {
        state->http->get(tags_url)
            .size_limit(k_repository_metadata_size_limit)
            .on_error([state](std::string, std::string error, unsigned status) {
                BOOST_LOG_TRIVIAL(warning) << "Cannot update repository '" << state->repository_id
                                           << "': " << error;
                const UpdaterError::Code code = status == 404 ? UpdaterError::Code::RepositoryNotFound :
                                                               UpdaterError::Code::Network;
                state->complete(make_updater_error(code, std::move(error)));
            })
            .on_complete([state, page_number](std::string contents, unsigned) {
                std::vector<RepositoryTagEntry> page;
                UpdaterError parse_error = parse_repository_tag_page(contents, page);
                if (!parse_error.succeeded()) {
                    state->complete(std::move(parse_error));
                    return;
                }
                consume_repository_tag_page(state, page_number, std::move(page));
            })
            .perform();
    } catch (const std::exception &error) {
        BOOST_LOG_TRIVIAL(warning) << "Cannot start repository refresh for '" << state->repository_id
                                   << "': " << error.what();
        state->complete(make_updater_error(UpdaterError::Code::Network, error.what()));
    }
}

void consume_repository_tag_page(const std::shared_ptr<RepositoryTagRefreshState> &state,
                                 uint32_t page_number,
                                 std::vector<RepositoryTagEntry> page)
{
    if (!state->github) {
        size_t added_count = 0;
        merge_repository_tag_history(state->working_tags, page, added_count);
        if (page.size() == k_repository_tags_per_page && added_count == 0) {
            state->complete(make_updater_error(
                UpdaterError::Code::InvalidRepositoryMetadata,
                "Repository tag pagination did not advance; the endpoint may ignore the page parameter."));
            return;
        }
        if (page.size() < k_repository_tags_per_page) {
            finalize_repository_tag_refresh(state);
            return;
        }
        if (page_number == std::numeric_limits<uint32_t>::max()) {
            state->complete(make_updater_error(UpdaterError::Code::InvalidRepositoryMetadata,
                                               "Repository tag pagination exceeded its page range."));
            return;
        }
        request_repository_tag_page(state, page_number + 1);
        return;
    }

    if (page_number == 1) {
        merge_repository_tag_front(state->working_tags, page);
        if (state->refresh_front_after_overlap) {
            state->pagination = state->pagination_after_history_page;
        } else if (!state->pagination.history_complete) {
            state->pagination.history_complete = page.size() < k_repository_tags_per_page;
            state->pagination.next_page = state->pagination.history_complete ? 1 : 2;
        }
        finalize_repository_tag_refresh(state);
        return;
    }

    size_t added_count = 0;
    const bool overlap = merge_repository_tag_history(state->working_tags, page, added_count);
    (void) added_count;
    if (page.size() == k_repository_tags_per_page &&
        page_number == std::numeric_limits<uint32_t>::max()) {
        state->complete(make_updater_error(UpdaterError::Code::InvalidRepositoryMetadata,
                                           "Repository tag pagination exceeded its page range."));
        return;
    }
    state->pagination_after_history_page = state->pagination;
    state->pagination_after_history_page.history_complete = page.size() < k_repository_tags_per_page;
    state->pagination_after_history_page.next_page =
        state->pagination_after_history_page.history_complete ? 1 : page_number + 1;

    // A duplicate at a page boundary indicates that new tags shifted the
    // history. Page one is the sole immediate exception to the daily request.
    if (overlap) {
        state->refresh_front_after_overlap = true;
        request_repository_tag_page(state, 1);
        return;
    }

    state->pagination = state->pagination_after_history_page;
    finalize_repository_tag_refresh(state);
}

void finalize_repository_tag_refresh(const std::shared_ptr<RepositoryTagRefreshState> &state)
{
    const std::string aggregate = serialize_repository_tags(state->working_tags);
    UpdaterError parse_error = parse_repository_tags_safely(state->repository_id, state->parse_tags, aggregate);
    if (!parse_error.succeeded()) {
        state->complete(std::move(parse_error));
        return;
    }

    std::vector<RepositoryCachePublication> publications;
    publications.push_back(RepositoryCachePublication{state->cache_file, aggregate});
    if (state->github)
        publications.push_back(RepositoryCachePublication{
            state->pagination_file, serialize_repository_tag_pagination(state->pagination)});

    // The aggregate and its cursor describe one state. Publishing them in the
    // same transaction prevents a newer cursor from referring to old tags or
    // vice versa after a second-file failure.
    const UpdaterError cache_error = publish_repository_caches_atomically(publications);
    if (!cache_error.succeeded()) {
        BOOST_LOG_TRIVIAL(warning) << "Cannot write repository cache for '" << state->repository_id
                                   << "': " << cache_error.detail;
        state->complete(std::move(parse_error));
        return;
    }
    state->complete(std::move(parse_error));
}

} // namespace

RepositoryTagService::RepositoryTagService(UpdaterHttpTransport &http_transport)
    : m_http_transport(http_transport)
{
}

void RepositoryTagService::refresh(const std::string &repository_id,
                                   const std::string &repository_url,
                                   const boost::filesystem::path &cache_file,
                                   bool force,
                                   ReserveRequestSlotFn reserve_request_slot,
                                   ParseTagsFn parse_tags,
                                   FinishedFn finished)
{
    try {
        if (!cache_file.parent_path().empty())
            boost::filesystem::create_directories(cache_file.parent_path());
    } catch (const std::exception &error) {
        BOOST_LOG_TRIVIAL(warning) << "Cannot create repository cache for '" << repository_id
                                   << "': " << error.what();
        finished(make_updater_error(UpdaterError::Code::Filesystem, error.what()));
        return;
    }

    std::string cached_contents;
    bool cache_exists = false;
    const UpdaterError cache_read_error =
        read_repository_cache_file(cache_file, cached_contents, cache_exists);
    if (!cache_read_error.succeeded()) {
        finished(cache_read_error);
        return;
    }

    std::vector<RepositoryTagEntry> cached_tags;
    UpdaterError cached_parse_error;
    if (cache_exists)
        cached_parse_error = parse_repository_tag_page(cached_contents, cached_tags);

    const bool github = repository_url.rfind("https://api.github.com/repos/", 0) == 0;
    RepositoryTagPagination pagination;
    const boost::filesystem::path pagination_file = repository_tag_pagination_path(cache_file);
    if (github) {
        bool pagination_exists = false;
        UpdaterError pagination_error =
            read_repository_tag_pagination(pagination_file, pagination, pagination_exists);
        if (!pagination_error.succeeded() && !force) {
            finished(std::move(pagination_error));
            return;
        }

        // Adopt an older one-page cache by deriving its cursor from size and
        // its request time from the cache modification timestamp.
        if (!pagination_exists || !pagination_error.succeeded()) {
            pagination = RepositoryTagPagination();
            if (cache_exists) {
                try {
                    pagination.last_request = boost::filesystem::last_write_time(cache_file);
                } catch (const std::exception &error) {
                    finished(make_updater_error(UpdaterError::Code::Filesystem, error.what()));
                    return;
                }
                if (cached_parse_error.succeeded()) {
                    pagination.history_complete = cached_tags.size() < k_repository_tags_per_page;
                    pagination.next_page = pagination.history_complete ? 1 : 2;
                }
            }
        }

        // A malformed aggregate is repairable only after the daily window or
        // by a forced request; neither path may continue an old page cursor.
        if (cache_exists && !cached_parse_error.succeeded()) {
            if (!force && pagination.last_request + k_repository_cache_lifetime > std::time(nullptr)) {
                finished(std::move(cached_parse_error));
                return;
            }
            cache_exists = false;
            cached_tags.clear();
            pagination.next_page = 1;
            pagination.history_complete = false;
        }

        // A cursor without its aggregate cannot identify collected history.
        // Preserve only the request timestamp and rebuild from page one.
        if (!cache_exists && (pagination.next_page != 1 || pagination.history_complete)) {
            const std::time_t last_request = pagination.last_request;
            pagination = RepositoryTagPagination();
            pagination.last_request = last_request;
        }

        if (!force && pagination.last_request + k_repository_cache_lifetime > std::time(nullptr)) {
            if (!cache_exists) {
                finished(make_updater_error(
                    UpdaterError::Code::Cache,
                    "This GitHub repository was already checked within the last 24 hours, but no valid tag cache "
                    "is available. Use Force check for updates to retry now."));
                return;
            }
            finished(parse_repository_tags_safely(repository_id, parse_tags, cached_contents));
            return;
        }
    }

    const std::shared_ptr<RepositoryTagRefreshState> state =
        std::make_shared<RepositoryTagRefreshState>();
    state->http = &m_http_transport;
    state->repository_id = repository_id;
    state->repository_url = repository_url;
    state->cache_file = cache_file;
    state->pagination_file = pagination_file;
    state->parse_tags = std::move(parse_tags);
    state->complete = std::move(finished);
    state->reserve_request_slot = std::move(reserve_request_slot);
    state->github = github;
    state->pagination = pagination;
    state->working_tags = github ? std::move(cached_tags) : std::vector<RepositoryTagEntry>();

    const uint32_t first_page = github && !pagination.history_complete ? pagination.next_page : 1;
    request_repository_tag_page(state, first_page);
}

} // namespace RepositoryUpdaterInternal
} // namespace Slic3r
