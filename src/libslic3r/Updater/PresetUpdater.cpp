///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/ Copyright (c) Prusa Research 2018 - 2023 David Kocik @kocikdav, Lukas Matena @lukasmatena, Vojtech Bubnik @bubnikv, Tomas Meszaros @tamasmeszaros, Vojtech Kral @vojtechkral
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// The implementation below deliberately uses only filesystem, HTTP and preset
// types from libslic3r. A PresetUpdaterHost is notified around file changes so
// applications may snapshot and reload their own state without leaking GUI
// dependencies into this reusable repository engine.

#include "libslic3r/Updater/PresetUpdater.hpp"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <utility>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include "libslic3r/Plugins/PluginRepository.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Updater/RepositoryPackageCache.hpp"
#include "libslic3r/Updater/UpdaterHttp.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/libslic3r.h"

namespace Slic3r {
namespace {

boost::filesystem::path data_path();
boost::filesystem::path vendor_cache_directory(const VendorProfile &profile);
// Build the versionless profile used to expose a repository in the updater.
VendorProfile vendor_profile_from_repository_description(const RepositoryDescription &description);
bool prepare_vendor_cache(RepositoryPackageCache &cache, bool &purged, std::string &error_message);
bool transfer_vendor_files(const boost::filesystem::path &input_directory,
                           const boost::filesystem::path &output_directory,
                           const std::string &vendor_id);
UpdaterError save_vendor_description(const std::string &contents, const std::string &fallback_id);

// Terminal callbacks belong to application code and may run on the updater,
// HTTP or owner thread. Contain their exceptions so no execution boundary can
// terminate while reporting an otherwise completed operation.
template<class Result>
void invoke_vendor_callback(const std::function<void(Result)> &callback,
                            Result result,
                            const char *context) noexcept
{
    if (!callback)
        return;
    try {
        callback(std::move(result));
    } catch (const std::exception &error) {
        BOOST_LOG_TRIVIAL(error) << context << " callback failed: " << error.what();
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << context << " callback failed with an unknown exception.";
    }
}

// Host, HTTP and worker continuations may execute long after their initiating
// call returned. Route an unexpected exception to the operation's normal
// terminal path so its gate and GUI busy state are always released.
template<class Failure, class Operation>
auto guard_vendor_continuation(const char *context, Failure failure, Operation operation)
{
    return [context, failure = std::move(failure), operation = std::move(operation)](
               auto &&...args) mutable noexcept {
        try {
            operation(std::forward<decltype(args)>(args)...);
        } catch (...) {
            UpdaterError error = make_updater_error_from_exception(std::current_exception());
            BOOST_LOG_TRIVIAL(error) << context << " continuation failed: " << error.detail;
            try {
                failure(std::move(error));
            } catch (const std::exception &failure_error) {
                BOOST_LOG_TRIVIAL(error) << context << " failure handler failed: "
                                         << failure_error.what();
            } catch (...) {
                BOOST_LOG_TRIVIAL(error) << context
                                         << " failure handler failed with an unknown exception.";
            }
        }
    };
}

boost::filesystem::path data_path()
{
    return boost::filesystem::path(data_dir());
}

boost::filesystem::path vendor_cache_directory(const VendorProfile &profile)
{
    return repository_cache_root_path(data_path(), RepositoryPackageType::Vendor, profile.id);
}

// A repository descriptor identifies a vendor before any version has been
// downloaded. It deliberately has no profile version, printer models or
// presets; those are read only from a version's profiles directory.
VendorProfile vendor_profile_from_repository_description(const RepositoryDescription &description)
{
    VendorProfile profile(description.id);
    profile.name = description.name;
    profile.full_name = description.full_name;
    profile.description = description.description;
    profile.config_update_rest = description.config_update_rest;
    profile.slicer = description.slicer;
    return profile;
}

// Initialize the schema and restore durable vendor sources after a purge. This
// function is used by every public cache entry point because the first action
// in a process may be an import rather than reload_all_vendors().
bool prepare_vendor_cache(RepositoryPackageCache &cache, bool &purged, std::string &error_message)
{
    if (!cache.prepare_layout(purged, error_message))
        return false;
    if (!purged)
        return true;

    const boost::filesystem::path installed = data_path() / "vendor";
    if (boost::filesystem::is_directory(installed)) {
        for (const boost::filesystem::directory_entry &entry : boost::filesystem::directory_iterator(installed)) {
            if (!boost::filesystem::is_regular_file(entry.path()) || entry.path().extension() != ".ini")
                continue;
            RepositoryCachedVersion cached;
            if (!cache.cache_simple(entry.path(), cached, error_message)) {
                BOOST_LOG_TRIVIAL(warning) << "Cannot restore installed vendor profile '"
                                           << entry.path().string() << "': " << error_message;
                error_message.clear();
            }
        }
    }

    const boost::filesystem::path embedded = boost::filesystem::path(resources_dir()) / "profiles";
    if (boost::filesystem::is_directory(embedded)) {
        for (const boost::filesystem::directory_entry &entry : boost::filesystem::directory_iterator(embedded)) {
            if (!boost::filesystem::is_regular_file(entry.path()) || entry.path().extension() != ".ini")
                continue;
            RepositoryCachedVersion cached;
            if (!cache.cache_simple(entry.path(), cached, error_message)) {
                BOOST_LOG_TRIVIAL(warning) << "Cannot restore embedded vendor profile '"
                                           << entry.path().string() << "': " << error_message;
                error_message.clear();
            }
        }
    }
    return true;
}

// Publish one cached vendor profile and its complete resource directory. The
// caller owns rollback through its configuration snapshot, so this function
// writes directly to the live vendor directory.
bool transfer_vendor_files(const boost::filesystem::path &input_directory,
                           const boost::filesystem::path &output_directory,
                           const std::string &vendor_id)
{
    const boost::filesystem::path input_ini = input_directory / (vendor_id + ".ini");
    const boost::filesystem::path output_ini = output_directory / (vendor_id + ".ini");
    if (!boost::filesystem::is_regular_file(input_ini))
        return false;

    boost::filesystem::create_directories(output_directory);
    std::string copy_error;
    if (copy_file(input_ini.string(), output_ini.string(), copy_error, true) != CopyFileResult::SUCCESS)
        return false;

    const boost::filesystem::path input_icons = input_directory / vendor_id;
    const boost::filesystem::path output_icons = output_directory / vendor_id;
    // Removing this directory is also required when the new version has no
    // resources; otherwise images from an older package would survive.
    boost::filesystem::remove_all(output_icons);
    if (boost::filesystem::is_directory(input_icons))
        boost::filesystem::copy(input_icons, output_icons, boost::filesystem::copy_options::recursive);
    return true;
}

// Writes a repository description only after it has been parsed and checked.
// The cache is then a valid local source for the next repository reload.
UpdaterError save_vendor_description(const std::string &contents, const std::string &fallback_id)
{
    RepositoryDescription description;
    std::string error_message;
    if (!parse_repository_description(contents, RepositoryPackageType::Vendor, description, error_message))
        return make_updater_error(UpdaterError::Code::InvalidArchive, std::move(error_message));
    if (description.id.empty())
        description.id = fallback_id;

    RepositoryPackageCache cache(data_path(), vendor_repository_cache_adapter());
    bool purged = false;
    if (!prepare_vendor_cache(cache, purged, error_message) ||
        !cache.save_repository_description(description, error_message))
        return make_updater_error(UpdaterError::Code::Filesystem, std::move(error_message));
    return UpdaterError();
}

} // namespace

PresetUpdater::~PresetUpdater()
{
    // Filesystem jobs and HTTP callback chains retain VendorSync records and
    // this updater's helpers, so drain both before derived members disappear.
    shutdown_operation_executor();
}

PresetUpdater::PresetUpdater(PresetUpdaterHost *host)
    : m_host(host)
{
}

PresetUpdater::PresetUpdater(PresetUpdaterHost *host, UpdaterHttpTransport &http_transport)
    : RepositoryUpdater(http_transport), m_host(host)
{
}

VendorSync *PresetUpdater::find_vendor_unlocked(const std::string &id)
{
    const std::map<std::string, VendorSync>::iterator it = m_vendors.find(id);
    return it == m_vendors.end() ? nullptr : &it->second;
}

const VendorSync *PresetUpdater::find_vendor_unlocked(const std::string &id) const
{
    const std::map<std::string, VendorSync>::const_iterator it = m_vendors.find(id);
    return it == m_vendors.end() ? nullptr : &it->second;
}

std::vector<VendorSync> PresetUpdater::vendors() const
{
    std::lock_guard<std::mutex> guard(m_model_mutex);
    std::vector<VendorSync> result;
    result.reserve(m_vendors.size());
    for (const auto &[id, vendor] : m_vendors)
        result.emplace_back(vendor);
    return result;
}

std::optional<VendorSync> PresetUpdater::vendor(const std::string &id) const
{
    std::lock_guard<std::mutex> guard(m_model_mutex);
    const VendorSync *found = find_vendor_unlocked(id);
    return found == nullptr ? std::nullopt : std::optional<VendorSync>(*found);
}

void PresetUpdater::set_installed_vendors(const PresetBundle *preset_bundle)
{
    if (preset_bundle == nullptr)
        return;

    std::vector<std::pair<VendorProfile, bool>> installed_vendors;
    installed_vendors.reserve(preset_bundle->vendors.size());
    for (const auto &[id, installed_vendor] : preset_bundle->vendors) {
        const bool has_cache = boost::filesystem::is_directory(
            repository_cache_root_path(data_path(), RepositoryPackageType::Vendor, installed_vendor.id));
        installed_vendors.emplace_back(installed_vendor, has_cache);
    }

    std::lock_guard<std::mutex> guard(m_model_mutex);
    for (const std::pair<VendorProfile, bool> &installed_vendor : installed_vendors) {
        const VendorProfile &profile = installed_vendor.first;
        const bool has_cache = installed_vendor.second;
        VendorSync &vendor = m_vendors[profile.id];
        vendor.reset(profile, true, has_cache);
        if (!profile.config_update_rest.empty())
            m_is_synchronized = false;
    }
}

void PresetUpdater::load_unused_vendors(std::map<std::string, VendorSync> &vendors,
                                        bool &is_synchronized,
                                        std::set<std::string> &vendor_ids,
                                        const boost::filesystem::path &vendor_directory,
                                        bool is_installed)
{
    if (!boost::filesystem::is_directory(vendor_directory))
        return;

    for (const boost::filesystem::directory_entry &entry : boost::filesystem::directory_iterator(vendor_directory)) {
        if (entry.path().extension() != ".ini")
            continue;
        try {
            VendorProfile profile = VendorProfile::from_ini(entry.path(), false);
            VendorSync &vendor = vendors[profile.id];
            if (vendor.profile.id.empty())
                vendor.reset(profile, is_installed, boost::filesystem::is_directory(vendor_cache_directory(profile)));
            if (profile.config_version != Semver()) {
                VendorAvailable available{profile.config_version, profile.slicer_version, entry.path().string(), "", "", "",
                                          profile.config_version.to_string() + "=" + profile.slicer_version.to_string(), ""};
                const bool already_present = std::any_of(vendor.available_profiles.begin(), vendor.available_profiles.end(),
                    [&available](const VendorAvailable &candidate) { return candidate.tag == available.tag; });
                if (!already_present)
                    vendor.available_profiles.emplace_back(std::move(available));
                vendor.sort_available();
            }
            vendor_ids.insert(profile.id);
            if (!profile.config_update_rest.empty())
                is_synchronized = false;
        } catch (const std::exception &error) {
            BOOST_LOG_TRIVIAL(warning) << "Cannot read vendor profile '" << entry.path().string() << "': " << error.what();
        }
    }
}

void PresetUpdater::reload_all_vendors()
{
    const boost::filesystem::path configuration_directory = data_path();
    RepositoryPackageCache cache(configuration_directory, vendor_repository_cache_adapter());
    const boost::filesystem::path profiles_directory = boost::filesystem::path(resources_dir()) / "profiles";
    std::set<std::string> vendor_ids;
    std::map<std::string, VendorSync> vendors;
    bool is_synchronized = false;

    if (sync_in_progress() || changelog_download_in_progress())
        return;
    std::string cache_error;
    bool purged = false;
    if (!prepare_vendor_cache(cache, purged, cache_error)) {
        BOOST_LOG_TRIVIAL(warning) << cache_error;
        return;
    }
    const boost::filesystem::path installed_directory = configuration_directory / "vendor";

    // Embedded profiles are imported through the same adapter as user files.
    // Replacing their exact version is harmless and ensures newly shipped
    // bundles appear even when the cache marker was already current.
    if (boost::filesystem::is_directory(profiles_directory)) {
        for (const boost::filesystem::directory_entry &entry : boost::filesystem::directory_iterator(profiles_directory)) {
            if (!boost::filesystem::is_regular_file(entry.path()) || entry.path().extension() != ".ini")
                continue;
            RepositoryCachedVersion cached;
            if (!cache.cache_simple(entry.path(), cached, cache_error))
                BOOST_LOG_TRIVIAL(warning) << "Cannot cache built-in vendor profile '" << entry.path().string()
                                           << "': " << cache_error;
        }
    }

    load_unused_vendors(vendors, is_synchronized, vendor_ids, installed_directory, true);
    for (const RepositoryCachedEntry &repository : cache.scan()) {
        // The root descriptor makes a local-only or newly configured remote
        // repository visible before it has any downloaded version. Other INI
        // files at this level, such as tags.pagination.ini, are cache metadata
        // and must never be parsed as vendor profiles.
        // Use the descriptor already validated by RepositoryPackageCache.
        const RepositoryDescription &description = repository.description;
        // A descriptor without an id cannot identify a VendorSync entry.
        if (!description.id.empty()) {
            // Reuse an installed entry or create the repository-only entry.
            VendorSync &vendor = vendors[description.id];
            // Installed profiles remain authoritative when already present.
            if (vendor.profile.id.empty()) {
                // Expose repositories that do not yet contain a cached version.
                vendor.reset(vendor_profile_from_repository_description(description), false, true);
            } else {
                // The scanned root proves that this vendor has cache metadata.
                vendor.has_cache = true;
            }
            // Remember the id so later synchronization includes this vendor.
            vendor_ids.insert(description.id);
            // A remote endpoint means its tag list still needs synchronization.
            if (!description.config_update_rest.empty()) {
                // Mark the aggregate model as requiring a repository refresh.
                is_synchronized = false;
            }
        }

        // Version directories contain the actual vendor profiles and are the
        // only cache locations interpreted by VendorProfile::from_ini().
        // Visit every cached version without scanning root metadata files.
        for (const RepositoryCachedVersion &version : repository.versions) {
            // Parse only the profiles payload belonging to this exact version.
            load_unused_vendors(vendors, is_synchronized, vendor_ids, version.directory / "profiles", false);
        }
    }

    std::lock_guard<std::mutex> guard(m_model_mutex);
    m_vendors.swap(vendors);
    m_is_synchronized = is_synchronized;
}

void PresetUpdater::sync_async(std::function<void(int)> callback_result, bool force)
{
    std::vector<std::string> vendor_ids;
    {
        std::lock_guard<std::mutex> guard(m_model_mutex);
        vendor_ids.reserve(m_vendors.size());
        for (const auto &[vendor_id, vendor] : m_vendors)
            vendor_ids.emplace_back(vendor_id);
    }
    if (!begin_sync(vendor_ids.size(), std::move(callback_result)))
        return;
    for (const std::string &vendor_id : vendor_ids)
        update_vendor(vendor_id, force);
}

void PresetUpdater::update_vendor(const std::string &vendor_id, bool force)
{
    std::string rest_url;
    {
        std::lock_guard<std::mutex> guard(m_model_mutex);
        VendorSync *vendor = find_vendor_unlocked(vendor_id);
        if (vendor == nullptr) {
            finish_sync();
            return;
        }
        vendor->sync_state = RepositorySyncState::InProgress;
        vendor->sync_error = UpdaterError();
        rest_url = vendor->profile.config_update_rest;
    }

    const RepositoryPackageCache cache(data_path(), vendor_repository_cache_adapter());
    const boost::filesystem::path cache_file = cache.repository_tags_path(vendor_id);
    refresh_repository_tags(
        vendor_id, rest_url, cache_file, force,
        [this, vendor_id](const std::string &tags) {
            std::lock_guard<std::mutex> guard(m_model_mutex);
            VendorSync *vendor = find_vendor_unlocked(vendor_id);
            return vendor == nullptr ?
                make_updater_error(UpdaterError::Code::RepositoryNotFound) : vendor->parse_tags(tags);
        },
        [this, vendor_id](UpdaterError error) {
            std::lock_guard<std::mutex> guard(m_model_mutex);
            VendorSync *vendor = find_vendor_unlocked(vendor_id);
            if (vendor == nullptr)
                return;
            vendor->sync_state = error.succeeded() ? RepositorySyncState::Succeeded : RepositorySyncState::Failed;
            vendor->sync_error = std::move(error);
        });
}

void PresetUpdater::download_changelogs(const std::string &vendor_id,
                                        std::function<void(bool)> callback_result,
                                        bool force)
{
    std::vector<RepositoryChangelogVersion> versions;
    std::string rest_url;
    bool found = false;
    {
        std::lock_guard<std::mutex> guard(m_model_mutex);
        VendorSync *vendor = find_vendor_unlocked(vendor_id);
        if (vendor != nullptr) {
            found = true;
            rest_url = vendor->profile.config_update_rest;
            versions.reserve(vendor->available_profiles.size());
            for (const VendorAvailable &version : vendor->available_profiles) {
                RepositoryChangelogVersion common_version;
                common_version.content_version = version.config_version;
                common_version.slicer_version = version.slicer_version;
                common_version.tag = version.tag;
                common_version.commit_sha = version.commit_sha;
                common_version.commit_url = version.commit_url;
                common_version.store_notes = [this, vendor_id, tag = version.tag](std::string notes) {
                    std::lock_guard<std::mutex> notes_guard(m_model_mutex);
                    VendorSync *current = find_vendor_unlocked(vendor_id);
                    if (current == nullptr)
                        return;
                    const std::vector<VendorAvailable>::iterator matching = std::find_if(
                        current->available_profiles.begin(), current->available_profiles.end(),
                        [&tag](const VendorAvailable &candidate) { return candidate.tag == tag; });
                    if (matching != current->available_profiles.end())
                        matching->notes = std::move(notes);
                };
                versions.emplace_back(std::move(common_version));
            }
        }
    }

    if (!found) {
        callback_result(false);
        return;
    }

    const RepositoryPackageCache cache(data_path(), vendor_repository_cache_adapter());
    const boost::filesystem::path log_directory = cache.repository_logs_directory(vendor_id);
    download_repository_version_changelogs(std::move(versions), log_directory,
                                           rest_url,
                                           std::move(callback_result), force);
}

void PresetUpdater::download_new_repo(const std::string &rest_url, std::function<void(UpdaterError)> callback_result)
{
    download_repository_description(
        rest_url,
        [](const std::string &contents, const std::string &fallback_id) {
            return save_vendor_description(contents, fallback_id);
        },
        std::move(callback_result));
}

void PresetUpdater::cache_vendor_archive(const boost::filesystem::path &archive_path,
                                         std::function<void(UpdaterError)> callback_result)
{
    if (!begin_repository_change()) {
        invoke_vendor_callback(callback_result, make_updater_error(
            UpdaterError::Code::PreparationRejected,
            "Another vendor package change is already in progress."), "Vendor archive import");
        return;
    }
    const std::shared_ptr<std::atomic_bool> terminal = std::make_shared<std::atomic_bool>(false);
    const std::function<void(UpdaterError)> complete =
        [this, terminal, callback_result = std::move(callback_result)](UpdaterError error) {
            if (terminal->exchange(true))
                return;
            finish_repository_change();
            invoke_vendor_callback(callback_result, std::move(error), "Vendor archive import");
        };

    const bool accepted = enqueue_operation(
        [this, archive_path] {
            UpdaterError result = cache_vendor_archive_files(archive_path);
            if (result.succeeded())
                reload_all_vendors();
            return result;
        },
        complete);
    if (!accepted)
        complete(make_updater_error(
            UpdaterError::Code::PreparationRejected,
            "The preset updater is shutting down."));
}

UpdaterError PresetUpdater::cache_vendor_archive_files(const boost::filesystem::path &archive_path)
{
    UpdaterError result;
    try {
        if (!boost::filesystem::is_regular_file(archive_path))
            result = make_updater_error(UpdaterError::Code::ArchiveUnavailable,
                                        "The selected vendor archive does not exist.");
        else {
            RepositoryPackageCache cache(data_path(), vendor_repository_cache_adapter());
            std::string error_message;
            bool purged = false;
            RepositoryCachedVersion cached;
            if (!prepare_vendor_cache(cache, purged, error_message))
                result = make_updater_error(UpdaterError::Code::Filesystem, std::move(error_message));
            else if (!cache.cache_archive(archive_path, std::nullopt, cached, error_message))
                result = make_updater_error(UpdaterError::Code::InvalidArchive, std::move(error_message));
        }
    } catch (const boost::filesystem::filesystem_error &error) {
        result = make_updater_error(UpdaterError::Code::Filesystem, error.what());
    } catch (const std::exception &error) {
        result = make_updater_error(UpdaterError::Code::InvalidArchive, error.what());
    }
    return result;
}

void PresetUpdater::cache_vendor_ini(const boost::filesystem::path &profile_path,
                                     std::function<void(UpdaterError)> callback_result)
{
    if (!begin_repository_change()) {
        invoke_vendor_callback(callback_result, make_updater_error(
            UpdaterError::Code::PreparationRejected,
            "Another vendor package change is already in progress."), "Vendor profile import");
        return;
    }
    const std::shared_ptr<std::atomic_bool> terminal = std::make_shared<std::atomic_bool>(false);
    const std::function<void(UpdaterError)> complete =
        [this, terminal, callback_result = std::move(callback_result)](UpdaterError error) {
            if (terminal->exchange(true))
                return;
            finish_repository_change();
            invoke_vendor_callback(callback_result, std::move(error), "Vendor profile import");
        };

    const bool accepted = enqueue_operation(
        [this, profile_path] {
            UpdaterError result = cache_vendor_ini_files(profile_path);
            if (result.succeeded())
                reload_all_vendors();
            return result;
        },
        complete);
    if (!accepted)
        complete(make_updater_error(
            UpdaterError::Code::PreparationRejected,
            "The preset updater is shutting down."));
}

UpdaterError PresetUpdater::cache_vendor_ini_files(const boost::filesystem::path &profile_path)
{
    UpdaterError result;
    try {
        if (!boost::filesystem::is_regular_file(profile_path))
            result = make_updater_error(UpdaterError::Code::ArchiveUnavailable,
                                        "The selected vendor profile does not exist.");
        else {
            RepositoryPackageCache cache(data_path(), vendor_repository_cache_adapter());
            std::string error_message;
            bool purged = false;
            RepositoryCachedVersion cached;
            if (!prepare_vendor_cache(cache, purged, error_message))
                result = make_updater_error(UpdaterError::Code::Filesystem, std::move(error_message));
            else if (!cache.cache_simple(profile_path, cached, error_message))
                result = make_updater_error(UpdaterError::Code::InvalidArchive, std::move(error_message));
        }
    } catch (const boost::filesystem::filesystem_error &error) {
        result = make_updater_error(UpdaterError::Code::Filesystem, error.what());
    } catch (const std::exception &error) {
        result = make_updater_error(UpdaterError::Code::InvalidArchive, error.what());
    }
    return result;
}

void PresetUpdater::prepare_vendor_install_source_async(
    const VendorSync &vendor,
    const VendorAvailable &version,
    std::function<void(UpdaterError, boost::filesystem::path)> callback_result)
{
    const std::shared_ptr<std::atomic_bool> terminal = std::make_shared<std::atomic_bool>(false);
    const std::function<void(UpdaterError, boost::filesystem::path)> complete =
        [terminal, callback_result = std::move(callback_result)](
            UpdaterError error, boost::filesystem::path source_directory) mutable {
            if (!terminal->exchange(true))
                callback_result(std::move(error), std::move(source_directory));
        };

    boost::filesystem::path package_root = repository_package_cache_path(
        data_path(), RepositoryPackageType::Vendor, vendor.profile.id,
        version.config_version.to_string(), version.slicer_version.to_string());
    try {
        if (!version.local_file.empty()) {
            const boost::filesystem::path source_directory =
                boost::filesystem::path(version.local_file).parent_path();
            const UpdaterError result =
                boost::filesystem::is_regular_file(source_directory / (vendor.profile.id + ".ini")) ?
                    UpdaterError() : make_updater_error(
                        UpdaterError::Code::ArchiveUnavailable,
                        "The selected vendor profile is not available in the package cache.");
            complete(result, source_directory);
            return;
        }

        const boost::filesystem::path cached_profile =
            package_root / "profiles" / (vendor.profile.id + ".ini");
        if (boost::filesystem::is_regular_file(cached_profile)) {
            complete(UpdaterError(), package_root / "profiles");
            return;
        }

        // Download and validate the complete package before asking the host to
        // snapshot live configuration. The HTTP completion thread may safely
        // write the repository cache because no loaded preset references it.
        const boost::filesystem::path archive_path = vendor_cache_directory(vendor.profile) /
            (RepositoryPackageCache::version_directory_name(
                version.config_version.to_string(), version.slicer_version.to_string()) + ".zip");
        const std::string vendor_id = vendor.profile.id;
        const std::string config_version = version.config_version.to_string();
        const std::string slicer_version = version.slicer_version.to_string();
        download_repository_file_async(
            version.url_zip, archive_path, 130 * 1024 * 1024,
            [this, archive_path, vendor_id, config_version, slicer_version, complete](UpdaterError download_error) mutable {
                if (!download_error.succeeded()) {
                    boost::system::error_code cleanup_error;
                    boost::filesystem::remove(archive_path, cleanup_error);
                    complete(std::move(download_error), boost::filesystem::path());
                    return;
                }

                const std::shared_ptr<boost::filesystem::path> source_directory =
                    std::make_shared<boost::filesystem::path>();
                const bool accepted = enqueue_operation(
                    [archive_path, vendor_id, config_version, slicer_version, source_directory] {
                        UpdaterError result;
                        try {
                            RepositoryPackageExpectation expected;
                            expected.type = RepositoryPackageType::Vendor;
                            expected.id = vendor_id;
                            expected.version.package_version = config_version;
                            expected.version.slicer_version = slicer_version;
                            RepositoryPackageCache cache(data_path(), vendor_repository_cache_adapter());
                            RepositoryCachedVersion cached;
                            std::string error_message;
                            if (cache.cache_archive(archive_path, expected, cached, error_message)) {
                                *source_directory = cached.directory / "profiles";
                            } else {
                                result = make_updater_error(
                                    UpdaterError::Code::InvalidArchive, std::move(error_message));
                            }
                        } catch (const boost::filesystem::filesystem_error &error) {
                            result = make_updater_error(UpdaterError::Code::Filesystem, error.what());
                        } catch (const std::exception &error) {
                            result = make_updater_error(UpdaterError::Code::InvalidArchive, error.what());
                        } catch (...) {
                            result = make_updater_error_from_exception(std::current_exception());
                        }

                        // The ZIP is only a transfer artifact. The validated
                        // package directory is the durable publication source.
                        boost::system::error_code cleanup_error;
                        boost::filesystem::remove(archive_path, cleanup_error);
                        return result;
                    },
                    [source_directory, complete](UpdaterError result) mutable {
                        complete(std::move(result), std::move(*source_directory));
                    });
                if (!accepted) {
                    boost::system::error_code cleanup_error;
                    boost::filesystem::remove(archive_path, cleanup_error);
                    complete(make_updater_error(
                                 UpdaterError::Code::PreparationRejected,
                                 "The preset updater is shutting down."),
                             boost::filesystem::path());
                }
            });
    } catch (const boost::filesystem::filesystem_error &error) {
        complete(make_updater_error(UpdaterError::Code::Filesystem, error.what()),
                 boost::filesystem::path());
    } catch (const std::exception &error) {
        complete(make_updater_error(UpdaterError::Code::InvalidArchive, error.what()),
                 boost::filesystem::path());
    }
}

UpdaterError PresetUpdater::install_vendor_files(VendorSync &vendor,
                                                 const boost::filesystem::path &source_directory)
{
    const boost::filesystem::path vendor_directory = data_path() / "vendor";
    try {
        if (!transfer_vendor_files(source_directory, vendor_directory, vendor.profile.id))
            return make_updater_error(UpdaterError::Code::Filesystem,
                                      "Cannot copy the vendor profile from the package cache.");
        vendor.profile = VendorProfile::from_ini(vendor_directory / (vendor.profile.id + ".ini"), true);
        vendor.is_installed = true;
        vendor.has_cache = boost::filesystem::is_directory(vendor_cache_directory(vendor.profile));
        const VendorAvailable *best = vendor.best_available();
        vendor.can_upgrade = best != nullptr && best->config_version > vendor.profile.config_version;
        return UpdaterError();
    } catch (const boost::filesystem::filesystem_error &error) {
        return make_updater_error(UpdaterError::Code::Filesystem, error.what());
    } catch (const std::exception &error) {
        return make_updater_error(UpdaterError::Code::InvalidArchive, error.what());
    }
}

UpdaterError PresetUpdater::uninstall_vendor_files(VendorSync &vendor)
{
    try {
        RepositoryPackageCache cache(data_path(), vendor_repository_cache_adapter());
        RepositoryCachedVersion cached;
        std::string error_message;
        if (!cache.cache_simple(data_path() / "vendor" / (vendor.profile.id + ".ini"), cached, error_message))
            return make_updater_error(UpdaterError::Code::Filesystem, std::move(error_message));

        // The cache now owns a verified copy. Remove the installed files only
        // after publication so a failed copy cannot uninstall the vendor.
        boost::filesystem::remove(data_path() / "vendor" / (vendor.profile.id + ".ini"));
        boost::filesystem::remove_all(data_path() / "vendor" / vendor.profile.id);
        vendor.is_installed = false;
        vendor.has_cache = true;
        vendor.sync_state = RepositorySyncState::Unchecked;
        vendor.sync_error = UpdaterError();
        vendor.can_upgrade = false;
        return UpdaterError();
    } catch (const boost::filesystem::filesystem_error &error) {
        return make_updater_error(UpdaterError::Code::Filesystem, error.what());
    }
}

UpdaterError PresetUpdater::clear_cache_vendor_files(VendorSync &vendor)
{
    try {
        boost::filesystem::remove_all(vendor_cache_directory(vendor.profile));
        vendor.has_cache = false;
        return UpdaterError();
    } catch (const boost::filesystem::filesystem_error &error) {
        return make_updater_error(UpdaterError::Code::Filesystem, error.what());
    }
}

void PresetUpdater::prepare_vendor_change_async(
    VendorChange change,
    const std::vector<std::string> &vendor_ids,
    PresetUpdaterHost::PrepareCallback callback)
{
    if (m_host == nullptr) {
        callback(UpdaterError(), std::string());
        return;
    }

    try {
        m_host->prepare_vendor_change_async(change, vendor_ids, callback);
    } catch (...) {
        callback(make_updater_error_from_exception(std::current_exception()), std::string());
    }
}

void PresetUpdater::rollback_vendor_change_async(
    const std::string &token,
    UpdaterError operation_error,
    std::function<void(UpdaterError)> callback)
{
    if (m_host == nullptr || token.empty()) {
        callback(std::move(operation_error));
        return;
    }

    const std::shared_ptr<UpdaterError> original =
        std::make_shared<UpdaterError>(std::move(operation_error));
    try {
        m_host->rollback_vendor_change_async(
            token,
            [original, callback](UpdaterError rollback_error) mutable {
                if (!rollback_error.succeeded()) {
                    if (!original->detail.empty())
                        original->detail += '\n';
                    original->detail +=
                        "The vendor operation also failed to restore its configuration snapshot.";
                    if (!rollback_error.detail.empty())
                        original->detail += " " + rollback_error.detail;
                }
                callback(std::move(*original));
            });
    } catch (...) {
        UpdaterError rollback_error = make_updater_error_from_exception(std::current_exception());
        if (!original->detail.empty())
            original->detail += '\n';
        original->detail += "The vendor operation also failed to start its snapshot restoration. " +
                            rollback_error.detail;
        callback(std::move(*original));
    }
}

void PresetUpdater::notify_vendor_files_changed(VendorChange change, const std::vector<std::string> &vendor_ids)
{
    if (m_host == nullptr)
        return;
    try {
        m_host->vendor_files_changed(*this, change, vendor_ids);
    } catch (const std::exception &error) {
        BOOST_LOG_TRIVIAL(error) << "Vendor change notification failed: " << error.what();
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << "Vendor change notification failed with an unknown exception.";
    }
}

void PresetUpdater::uninstall_vendor(const std::string &vendor_id, std::function<void(UpdaterError)> callback_result)
{
    if (!begin_repository_change()) {
        invoke_vendor_callback(callback_result, make_updater_error(
            UpdaterError::Code::PreparationRejected,
            "Another vendor package change is already in progress."), "Vendor uninstall");
        return;
    }
    const std::shared_ptr<std::atomic_bool> terminal = std::make_shared<std::atomic_bool>(false);
    const std::function<void(UpdaterError)> complete =
        [this, terminal, callback_result = std::move(callback_result)](UpdaterError error) {
            if (terminal->exchange(true))
                return;
            finish_repository_change();
            invoke_vendor_callback(callback_result, std::move(error), "Vendor uninstall");
        };

    const std::vector<std::string> ids{vendor_id};
    const std::optional<VendorSync> model_snapshot = vendor(vendor_id);
    if (!model_snapshot) {
        complete(make_updater_error(UpdaterError::Code::RepositoryNotFound));
        return;
    }
    const std::shared_ptr<VendorSync> changed_vendor =
        std::make_shared<VendorSync>(*model_snapshot);

    // The live tree is touched only after the host confirms that a complete
    // snapshot is durable. The deletion itself then runs on the updater worker.
    prepare_vendor_change_async(
        VendorChange::Uninstall, ids,
        guard_vendor_continuation(
        "Vendor uninstall preparation", complete,
        [this, vendor_id, ids, changed_vendor, complete](UpdaterError prepare_error,
                                                         std::string rollback_token) mutable {
            if (!prepare_error.succeeded()) {
                complete(std::move(prepare_error));
                return;
            }

            const bool accepted = enqueue_operation(
                [this, changed_vendor] { return uninstall_vendor_files(*changed_vendor); },
                guard_vendor_continuation(
                "Vendor uninstall publication", complete,
                [this, vendor_id, ids, changed_vendor, rollback_token, complete](UpdaterError error) mutable {
                    if (!error.succeeded()) {
                        rollback_vendor_change_async(
                            rollback_token, std::move(error),
                            [complete](UpdaterError rollback_result) mutable {
                                complete(std::move(rollback_result));
                            });
                        return;
                    }

                    {
                        std::lock_guard<std::mutex> guard(m_model_mutex);
                        VendorSync *current = find_vendor_unlocked(vendor_id);
                        if (current != nullptr) {
                            current->is_installed = changed_vendor->is_installed;
                            current->has_cache = changed_vendor->has_cache;
                            current->sync_state = changed_vendor->sync_state;
                            current->sync_error = changed_vendor->sync_error;
                            current->can_upgrade = changed_vendor->can_upgrade;
                        }
                    }
                    notify_vendor_files_changed(VendorChange::Uninstall, ids);
                    complete(UpdaterError());
                }));
            if (!accepted)
                complete(make_updater_error(
                    UpdaterError::Code::PreparationRejected,
                    "The preset updater is shutting down."));
        }));
}

void PresetUpdater::install_vendor(const std::string &vendor_id,
                                   const VendorAvailable &version,
                                   std::function<void(UpdaterError)> callback_result)
{
    install_vendor_batch(
        VendorChange::Install, {{vendor_id, version}},
        [callback_result = std::move(callback_result)](UpdaterErrors errors) mutable {
            if (errors.empty())
                callback_result(UpdaterError());
            else
                callback_result(std::move(errors.front()));
        });
}

void PresetUpdater::clear_cache_vendor(const std::string &vendor_id, std::function<void(UpdaterError)> callback_result)
{
    if (!begin_repository_change()) {
        invoke_vendor_callback(callback_result, make_updater_error(
            UpdaterError::Code::PreparationRejected,
            "Another vendor package change is already in progress."), "Vendor cache removal");
        return;
    }
    const std::shared_ptr<std::atomic_bool> terminal = std::make_shared<std::atomic_bool>(false);
    const std::function<void(UpdaterError)> complete =
        [this, terminal, callback_result = std::move(callback_result)](UpdaterError error) {
            if (terminal->exchange(true))
                return;
            finish_repository_change();
            invoke_vendor_callback(callback_result, std::move(error), "Vendor cache removal");
        };

    const std::vector<std::string> ids{vendor_id};
    const std::optional<VendorSync> model_snapshot = vendor(vendor_id);
    if (!model_snapshot) {
        complete(make_updater_error(UpdaterError::Code::RepositoryNotFound));
        return;
    }
    const std::shared_ptr<VendorSync> changed_vendor =
        std::make_shared<VendorSync>(*model_snapshot);

    // Cache removal does not alter the installed vendor tree, so it needs no
    // configuration snapshot. Only its filesystem and model work is queued.
    const bool accepted = enqueue_operation(
        [this, changed_vendor] { return clear_cache_vendor_files(*changed_vendor); },
        guard_vendor_continuation(
        "Vendor cache removal", complete,
        [this, vendor_id, ids, changed_vendor, complete](UpdaterError error) mutable {
            if (error.succeeded()) {
                std::lock_guard<std::mutex> guard(m_model_mutex);
                VendorSync *current = find_vendor_unlocked(vendor_id);
                if (current != nullptr)
                    current->has_cache = changed_vendor->has_cache;
            }
            if (error.succeeded())
                notify_vendor_files_changed(VendorChange::ClearCache, ids);
            complete(std::move(error));
        }));
    if (!accepted)
        complete(make_updater_error(
            UpdaterError::Code::PreparationRejected,
            "The preset updater is shutting down."));
}

void PresetUpdater::uninstall_all_vendors(std::function<void(UpdaterError)> callback_result)
{
    if (!begin_repository_change()) {
        invoke_vendor_callback(callback_result, make_updater_error(
            UpdaterError::Code::PreparationRejected,
            "Another vendor package change is already in progress."), "Bulk vendor uninstall");
        return;
    }
    const std::shared_ptr<std::atomic_bool> terminal = std::make_shared<std::atomic_bool>(false);
    const std::function<void(UpdaterError)> complete =
        [this, terminal, callback_result = std::move(callback_result)](UpdaterError error) {
            if (terminal->exchange(true))
                return;
            finish_repository_change();
            invoke_vendor_callback(callback_result, std::move(error), "Bulk vendor uninstall");
        };

    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> guard(m_model_mutex);
        for (const auto &[id, vendor] : m_vendors)
            if (vendor.is_installed)
                ids.emplace_back(id);
    }
    const std::shared_ptr<std::vector<std::pair<std::string, VendorSync>>> changed_vendors =
        std::make_shared<std::vector<std::pair<std::string, VendorSync>>>();
    for (const std::string &id : ids) {
        const std::optional<VendorSync> model_snapshot = vendor(id);
        if (model_snapshot)
            changed_vendors->emplace_back(id, *model_snapshot);
    }

    prepare_vendor_change_async(
        VendorChange::Uninstall, ids,
        guard_vendor_continuation(
        "Bulk vendor uninstall preparation", complete,
        [this, ids, changed_vendors, complete](UpdaterError prepare_error,
                                              std::string rollback_token) mutable {
            if (!prepare_error.succeeded()) {
                complete(std::move(prepare_error));
                return;
            }

            const bool accepted = enqueue_operation(
                [this, changed_vendors] {
                    for (std::pair<std::string, VendorSync> &changed : *changed_vendors) {
                        UpdaterError error = uninstall_vendor_files(changed.second);
                        if (!error.succeeded())
                            return error;
                    }
                    return UpdaterError();
                },
                guard_vendor_continuation(
                "Bulk vendor uninstall publication", complete,
                [this, ids, changed_vendors, rollback_token, complete](UpdaterError error) mutable {
                    if (!error.succeeded()) {
                        rollback_vendor_change_async(
                            rollback_token, std::move(error),
                            [complete](UpdaterError rollback_result) mutable {
                                complete(std::move(rollback_result));
                            });
                        return;
                    }

                    // Publish every detached model only after all removals
                    // succeed, matching the all-or-nothing snapshot contract.
                    {
                        std::lock_guard<std::mutex> guard(m_model_mutex);
                        for (const std::pair<std::string, VendorSync> &changed : *changed_vendors) {
                            VendorSync *current = find_vendor_unlocked(changed.first);
                            if (current != nullptr) {
                                current->is_installed = changed.second.is_installed;
                                current->has_cache = changed.second.has_cache;
                                current->sync_state = changed.second.sync_state;
                                current->sync_error = changed.second.sync_error;
                                current->can_upgrade = changed.second.can_upgrade;
                            }
                        }
                    }
                    if (!ids.empty())
                        notify_vendor_files_changed(VendorChange::Uninstall, ids);
                    complete(UpdaterError());
                }));
            if (!accepted)
                complete(make_updater_error(
                    UpdaterError::Code::PreparationRejected,
                    "The preset updater is shutting down."));
        }));
}

void PresetUpdater::install_all_vendors(std::function<void(UpdaterErrors)> callback_result)
{
    std::vector<std::pair<std::string, VendorAvailable>> installs;
    {
        std::lock_guard<std::mutex> guard(m_model_mutex);
        for (const auto &[id, vendor] : m_vendors) {
            const VendorAvailable *best = vendor.best_available();
            if (!vendor.is_installed && best != nullptr)
                installs.emplace_back(id, *best);
        }
    }
    install_vendor_batch(VendorChange::InstallAll, installs, std::move(callback_result));
}

void PresetUpdater::upgrade_all_installed_vendors(std::function<void(UpdaterErrors)> callback_result)
{
    std::vector<std::pair<std::string, VendorAvailable>> upgrades;
    {
        std::lock_guard<std::mutex> guard(m_model_mutex);
        for (const auto &[id, vendor] : m_vendors) {
            const VendorAvailable *best = vendor.best_available();
            if (vendor.is_installed && vendor.can_upgrade && best != nullptr)
                upgrades.emplace_back(id, *best);
        }
    }
    install_vendor_batch(VendorChange::UpgradeAll, upgrades, std::move(callback_result));
}

void PresetUpdater::install_vendor_batch(
    VendorChange change,
    const std::vector<std::pair<std::string, VendorAvailable>> &installs,
    std::function<void(UpdaterErrors)> callback_result)
{
    if (installs.empty()) {
        invoke_vendor_callback(callback_result, UpdaterErrors(), "Vendor installation");
        return;
    }
    if (!begin_repository_change()) {
        invoke_vendor_callback(callback_result, UpdaterErrors{make_updater_error(
            UpdaterError::Code::PreparationRejected,
            "Another vendor package change is already in progress.")}, "Vendor installation");
        return;
    }

    const std::shared_ptr<std::atomic_bool> terminal = std::make_shared<std::atomic_bool>(false);
    const std::function<void(UpdaterErrors)> complete =
        [this, terminal, callback_result = std::move(callback_result)](UpdaterErrors errors) {
            if (terminal->exchange(true))
                return;
            finish_repository_change();
            invoke_vendor_callback(callback_result, std::move(errors), "Vendor installation");
        };

    const std::shared_ptr<std::vector<PendingVendorInstall>> pending =
        std::make_shared<std::vector<PendingVendorInstall>>();
    pending->reserve(installs.size());
    for (const std::pair<std::string, VendorAvailable> &install : installs) {
        std::optional<VendorSync> vendor_snapshot = vendor(install.first);
        if (!vendor_snapshot.has_value()) {
            complete({make_updater_error(UpdaterError::Code::RepositoryNotFound)});
            return;
        }
        PendingVendorInstall pending_install;
        pending_install.vendor_id = install.first;
        pending_install.version = install.second;
        pending_install.vendor = std::move(*vendor_snapshot);
        pending->emplace_back(std::move(pending_install));
    }

    // Prepare each source sequentially. This preserves the previous network
    // load while allowing the caller's event loop to run between responses.
    prepare_vendor_install_batch(change, pending, 0, complete);
}

void PresetUpdater::prepare_vendor_install_batch(
    VendorChange change,
    std::shared_ptr<std::vector<PendingVendorInstall>> installs,
    size_t install_idx,
    std::function<void(UpdaterErrors)> callback_result)
{
    if (install_idx == installs->size()) {
        publish_vendor_install_batch(change, installs, std::move(callback_result));
        return;
    }

    PendingVendorInstall &install = installs->at(install_idx);
    prepare_vendor_install_source_async(
        install.vendor, install.version,
        guard_vendor_continuation(
        "Vendor source preparation",
        [callback_result](UpdaterError error) mutable {
            callback_result({std::move(error)});
        },
        [this, change, installs, install_idx,
         callback_result = std::move(callback_result)](
            UpdaterError error, boost::filesystem::path source_directory) mutable {
            if (!error.succeeded()) {
                callback_result({std::move(error)});
                return;
            }

            installs->at(install_idx).source_directory = std::move(source_directory);
            prepare_vendor_install_batch(change, installs, install_idx + 1,
                                         std::move(callback_result));
        }));
}

void PresetUpdater::publish_vendor_install_batch(
    VendorChange change,
    std::shared_ptr<std::vector<PendingVendorInstall>> installs,
    std::function<void(UpdaterErrors)> callback_result)
{
    std::vector<std::string> ids;
    ids.reserve(installs->size());
    for (const PendingVendorInstall &install : *installs)
        ids.emplace_back(install.vendor_id);

    // Snapshot creation starts only after every source has been downloaded and
    // validated. The worker publishes the whole batch after that durable point.
    prepare_vendor_change_async(
        change, ids,
        guard_vendor_continuation(
        "Vendor installation snapshot",
        [callback_result](UpdaterError error) mutable {
            callback_result({std::move(error)});
        },
        [this, change, ids, installs, callback_result](UpdaterError prepare_error,
                                                       std::string rollback_token) mutable {
            if (!prepare_error.succeeded()) {
                callback_result({std::move(prepare_error)});
                return;
            }

            const bool accepted = enqueue_operation(
                [this, installs] {
                    for (PendingVendorInstall &install : *installs) {
                        UpdaterError error = install_vendor_files(
                            install.vendor, install.source_directory);
                        if (!error.succeeded())
                            return error;
                    }
                    return UpdaterError();
                },
                guard_vendor_continuation(
                "Vendor installation publication",
                [callback_result](UpdaterError continuation_error) mutable {
                    callback_result({std::move(continuation_error)});
                },
                [this, change, ids, installs, rollback_token, callback_result](UpdaterError error) mutable {
                    if (!error.succeeded()) {
                        rollback_vendor_change_async(
                            rollback_token, std::move(error),
                            [callback_result](UpdaterError rollback_result) mutable {
                                callback_result({std::move(rollback_result)});
                            });
                        return;
                    }

                    // Model readers see the new batch only after all live
                    // files have been published successfully.
                    {
                        std::lock_guard<std::mutex> guard(m_model_mutex);
                        for (const PendingVendorInstall &install : *installs) {
                            VendorSync *current = find_vendor_unlocked(install.vendor_id);
                            if (current != nullptr) {
                                current->profile = install.vendor.profile;
                                current->is_installed = install.vendor.is_installed;
                                current->has_cache = install.vendor.has_cache;
                                current->sort_available();
                            }
                        }
                    }
                    notify_vendor_files_changed(change, ids);
                    callback_result(UpdaterErrors());
                }));
            if (!accepted) {
                callback_result({make_updater_error(
                    UpdaterError::Code::PreparationRejected,
                    "The preset updater is shutting down.")});
            }
        }));
}

int PresetUpdater::get_profile_count_to_update() const
{
    int count = 0;
    std::lock_guard<std::mutex> guard(m_model_mutex);
    for (const auto &[id, vendor] : m_vendors)
        if (vendor.can_upgrade)
            ++count;
    return count;
}

size_t PresetUpdater::count_available() const
{
    std::lock_guard<std::mutex> guard(m_model_mutex);
    return m_vendors.size();
}

size_t PresetUpdater::count_installed() const
{
    std::lock_guard<std::mutex> guard(m_model_mutex);
    return static_cast<size_t>(std::count_if(m_vendors.begin(), m_vendors.end(),
        [](const std::pair<const std::string, VendorSync> &entry) { return entry.second.is_installed; }));
}

int PresetUpdater::update_count()
{
    return get_profile_count_to_update();
}

bool PresetUpdater::is_synchronized() const
{
    std::lock_guard<std::mutex> guard(m_model_mutex);
    return m_is_synchronized;
}

void PresetUpdater::on_sync_completed()
{
    std::lock_guard<std::mutex> guard(m_model_mutex);
    m_is_synchronized = true;
}

void VendorSync::reset(const VendorProfile &new_profile, bool installed, bool cache_present)
{
    profile = new_profile;
    is_installed = installed;
    has_cache = cache_present;
    sync_state = RepositorySyncState::Unchecked;
    sync_error = UpdaterError();
    const VendorAvailable *best = best_available();
    can_upgrade = best != nullptr && best->config_version > profile.config_version;
}

UpdaterError VendorSync::parse_tags(const std::string &json)
{
    std::vector<RepositoryPackageVersion> versions;
    std::string error_message;
    if (!parse_repository_versions(json, versions, error_message))
        return make_updater_error(UpdaterError::Code::InvalidRepositoryMetadata, std::move(error_message));

    // Local profiles remain selectable when their repository tag disappears.
    // Their network fields are reset before applying the new remote projection
    // so a cached profile never retains a stale archive or changelog URL.
    std::vector<VendorAvailable> refreshed;
    refreshed.reserve(available_profiles.size() + versions.size());
    for (const VendorAvailable &available : available_profiles) {
        if (available.local_file.empty())
            continue;
        VendorAvailable local = available;
        local.url_zip.clear();
        local.commit_sha.clear();
        local.commit_url.clear();
        refreshed.emplace_back(std::move(local));
    }

    // Recreate every remote-only entry from this response. A matching local
    // entry keeps its file and notes, while receiving all repository metadata
    // from the current tag rather than from a previous synchronization.
    for (const RepositoryPackageVersion &version : versions) {
        const std::optional<Semver> package_version = Semver::parse(version.package_version);
        const std::optional<Semver> slicer_version = Semver::parse(version.slicer_version);
        if (!package_version || !slicer_version)
            continue;
        const std::vector<VendorAvailable>::iterator existing = std::find_if(
            refreshed.begin(), refreshed.end(),
            [&version](const VendorAvailable &candidate) { return candidate.tag == version.tag; });
        if (existing == refreshed.end()) {
            VendorAvailable available{*package_version, *slicer_version, "", version.url_zip, version.commit_sha,
                                      version.commit_url, version.tag, ""};
            const std::vector<VendorAvailable>::const_iterator previous = std::find_if(
                available_profiles.begin(), available_profiles.end(),
                [&version](const VendorAvailable &candidate) { return candidate.tag == version.tag; });
            if (previous != available_profiles.end())
                available.notes = previous->notes;
            refreshed.emplace_back(std::move(available));
        } else {
            existing->config_version = *package_version;
            existing->slicer_version = *slicer_version;
            existing->url_zip = version.url_zip;
            existing->commit_sha = version.commit_sha;
            existing->commit_url = version.commit_url;
            existing->tag = version.tag;
        }
    }

    available_profiles.swap(refreshed);
    sort_available();
    return UpdaterError();
}

void VendorSync::sort_available()
{
    std::sort(available_profiles.begin(), available_profiles.end(), [](const VendorAvailable &left, const VendorAvailable &right) {
        if (left.slicer_version != right.slicer_version)
            return left.slicer_version > right.slicer_version;
        return left.config_version > right.config_version;
    });
    const VendorAvailable *best = best_available();
    can_upgrade = is_installed && best != nullptr && best->config_version > profile.config_version;
}

const VendorAvailable *VendorSync::best_available() const
{
    const std::optional<Semver> current_slicer_version = Semver::parse(SLIC3R_VERSION_FULL);
    if (!current_slicer_version)
        return nullptr;

    for (const VendorAvailable &available : available_profiles)
        if (available.slicer_version <= *current_slicer_version)
            return &available;
    return nullptr;
}

} // namespace Slic3r
