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
bool prepare_vendor_cache(RepositoryPackageCache &cache, bool &purged, std::string &error_message);
bool transfer_vendor_files(const boost::filesystem::path &input_directory,
                           const boost::filesystem::path &output_directory,
                           const std::string &vendor_id);
UpdaterError save_vendor_description(const std::string &contents, const std::string &fallback_id);

boost::filesystem::path data_path()
{
    return boost::filesystem::path(data_dir());
}

boost::filesystem::path vendor_cache_directory(const VendorProfile &profile)
{
    return repository_cache_root_path(data_path(), RepositoryPackageType::Vendor, profile.id);
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
        // repository visible before it has any downloaded version.
        load_unused_vendors(vendors, is_synchronized, vendor_ids, repository.directory, false);
        for (const RepositoryCachedVersion &version : repository.versions)
            load_unused_vendors(vendors, is_synchronized, vendor_ids, version.directory / "profiles", false);
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

UpdaterError PresetUpdater::cache_vendor_archive(const boost::filesystem::path &archive_path)
{
    if (!boost::filesystem::is_regular_file(archive_path))
        return make_updater_error(UpdaterError::Code::ArchiveUnavailable,
                                  "The selected vendor archive does not exist.");

    RepositoryPackageCache cache(data_path(), vendor_repository_cache_adapter());
    std::string error_message;
    bool purged = false;
    RepositoryCachedVersion cached;
    if (!prepare_vendor_cache(cache, purged, error_message))
        return make_updater_error(UpdaterError::Code::Filesystem, std::move(error_message));
    if (!cache.cache_archive(archive_path, std::nullopt, cached, error_message))
        return make_updater_error(UpdaterError::Code::InvalidArchive, std::move(error_message));
    return UpdaterError();
}

UpdaterError PresetUpdater::cache_vendor_ini(const boost::filesystem::path &profile_path)
{
    if (!boost::filesystem::is_regular_file(profile_path))
        return make_updater_error(UpdaterError::Code::ArchiveUnavailable,
                                  "The selected vendor profile does not exist.");

    RepositoryPackageCache cache(data_path(), vendor_repository_cache_adapter());
    std::string error_message;
    bool purged = false;
    RepositoryCachedVersion cached;
    if (!prepare_vendor_cache(cache, purged, error_message))
        return make_updater_error(UpdaterError::Code::Filesystem, std::move(error_message));
    if (!cache.cache_simple(profile_path, cached, error_message))
        return make_updater_error(UpdaterError::Code::InvalidArchive, std::move(error_message));
    return UpdaterError();
}

UpdaterError PresetUpdater::prepare_vendor_install_source(const VendorSync &vendor,
                                                          const VendorAvailable &version,
                                                          boost::filesystem::path &source_directory)
{
    boost::filesystem::path package_root = repository_package_cache_path(
        data_path(), RepositoryPackageType::Vendor, vendor.profile.id,
        version.config_version.to_string(), version.slicer_version.to_string());
    try {
        if (!version.local_file.empty()) {
            source_directory = boost::filesystem::path(version.local_file).parent_path();
            return boost::filesystem::is_regular_file(source_directory / (vendor.profile.id + ".ini")) ?
                UpdaterError() : make_updater_error(UpdaterError::Code::ArchiveUnavailable,
                                                    "The selected vendor profile is not available in the package cache.");
        }

        // Network and archive validation finish before the application creates
        // its rollback snapshot. Once this function succeeds, publication is
        // only a copy from the versioned package cache.
        if (!boost::filesystem::is_regular_file(package_root / "profiles" / (vendor.profile.id + ".ini"))) {
            const boost::filesystem::path archive_path = vendor_cache_directory(vendor.profile) /
                (RepositoryPackageCache::version_directory_name(
                    version.config_version.to_string(), version.slicer_version.to_string()) + ".zip");
            UpdaterError download_error = download_repository_file_sync(
                version.url_zip, archive_path, 130 * 1024 * 1024);
            if (!download_error.succeeded())
                return download_error;

            RepositoryPackageExpectation expected;
            expected.type = RepositoryPackageType::Vendor;
            expected.id = vendor.profile.id;
            expected.version.package_version = version.config_version.to_string();
            expected.version.slicer_version = version.slicer_version.to_string();
            RepositoryPackageCache cache(data_path(), vendor_repository_cache_adapter());
            RepositoryCachedVersion cached;
            std::string error_message;
            if (!cache.cache_archive(archive_path, expected, cached, error_message))
                return make_updater_error(UpdaterError::Code::InvalidArchive, std::move(error_message));
            boost::system::error_code cleanup_error;
            boost::filesystem::remove(archive_path, cleanup_error);
            package_root = cached.directory;
        }
        source_directory = package_root / "profiles";
        return UpdaterError();
    } catch (const boost::filesystem::filesystem_error &error) {
        return make_updater_error(UpdaterError::Code::Filesystem, error.what());
    } catch (const std::exception &error) {
        return make_updater_error(UpdaterError::Code::InvalidArchive, error.what());
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

std::optional<std::string> PresetUpdater::prepare_vendor_change(
    VendorChange change, const std::vector<std::string> &vendor_ids)
{
    return m_host == nullptr ? std::optional<std::string>(std::string()) :
                               m_host->prepare_vendor_change(change, vendor_ids);
}

UpdaterError PresetUpdater::rollback_vendor_change(const std::string &token, UpdaterError operation_error)
{
    if (m_host == nullptr || token.empty())
        return operation_error;

    const UpdaterError rollback_error = m_host->rollback_vendor_change(token);
    if (!rollback_error.succeeded()) {
        if (!operation_error.detail.empty())
            operation_error.detail += '\n';
        operation_error.detail += "The vendor operation also failed to restore its configuration snapshot.";
        if (!rollback_error.detail.empty())
            operation_error.detail += " " + rollback_error.detail;
    }
    return operation_error;
}

void PresetUpdater::notify_vendor_files_changed(VendorChange change, const std::vector<std::string> &vendor_ids)
{
    if (m_host != nullptr)
        m_host->vendor_files_changed(*this, change, vendor_ids);
}

void PresetUpdater::uninstall_vendor(const std::string &vendor_id, std::function<void(UpdaterError)> callback_result)
{
    const std::vector<std::string> ids{vendor_id};
    std::optional<VendorSync> vendor_snapshot = vendor(vendor_id);
    if (!vendor_snapshot) {
        callback_result(make_updater_error(UpdaterError::Code::RepositoryNotFound));
        return;
    }
    const std::optional<std::string> rollback_token = prepare_vendor_change(VendorChange::Uninstall, ids);
    if (!rollback_token.has_value()) {
        callback_result(make_updater_error(UpdaterError::Code::PreparationRejected));
        return;
    }

    UpdaterError error = uninstall_vendor_files(*vendor_snapshot);
    if (!error.succeeded())
        error = rollback_vendor_change(*rollback_token, std::move(error));
    if (error.succeeded()) {
        {
            std::lock_guard<std::mutex> guard(m_model_mutex);
            VendorSync *current = find_vendor_unlocked(vendor_id);
            if (current != nullptr) {
                current->is_installed = vendor_snapshot->is_installed;
                current->has_cache = vendor_snapshot->has_cache;
                current->sync_state = vendor_snapshot->sync_state;
                current->sync_error = vendor_snapshot->sync_error;
                current->can_upgrade = vendor_snapshot->can_upgrade;
            }
        }
        notify_vendor_files_changed(VendorChange::Uninstall, ids);
    }
    callback_result(std::move(error));
}

void PresetUpdater::install_vendor(const std::string &vendor_id,
                                   const VendorAvailable &version,
                                   std::function<void(UpdaterError)> callback_result)
{
    const std::vector<std::string> ids{vendor_id};
    std::optional<VendorSync> vendor_snapshot = vendor(vendor_id);
    if (!vendor_snapshot) {
        callback_result(make_updater_error(UpdaterError::Code::RepositoryNotFound));
        return;
    }

    boost::filesystem::path source_directory;
    UpdaterError error = prepare_vendor_install_source(*vendor_snapshot, version, source_directory);
    if (!error.succeeded()) {
        callback_result(std::move(error));
        return;
    }

    const std::optional<std::string> rollback_token = prepare_vendor_change(VendorChange::Install, ids);
    if (!rollback_token.has_value()) {
        callback_result(make_updater_error(UpdaterError::Code::PreparationRejected));
        return;
    }

    error = install_vendor_files(*vendor_snapshot, source_directory);
    if (!error.succeeded())
        error = rollback_vendor_change(*rollback_token, std::move(error));
    if (error.succeeded()) {
        {
            std::lock_guard<std::mutex> guard(m_model_mutex);
            VendorSync *current = find_vendor_unlocked(vendor_id);
            if (current != nullptr) {
                current->profile = vendor_snapshot->profile;
                current->is_installed = vendor_snapshot->is_installed;
                current->has_cache = vendor_snapshot->has_cache;
                current->sort_available();
            }
        }
        notify_vendor_files_changed(VendorChange::Install, ids);
    }
    callback_result(std::move(error));
}

void PresetUpdater::clear_cache_vendor(const std::string &vendor_id, std::function<void(UpdaterError)> callback_result)
{
    const std::vector<std::string> ids{vendor_id};
    std::optional<VendorSync> vendor_snapshot = vendor(vendor_id);
    if (!vendor_snapshot) {
        callback_result(make_updater_error(UpdaterError::Code::RepositoryNotFound));
        return;
    }
    const std::optional<std::string> rollback_token = prepare_vendor_change(VendorChange::ClearCache, ids);
    if (!rollback_token.has_value()) {
        callback_result(make_updater_error(UpdaterError::Code::PreparationRejected));
        return;
    }
    (void) rollback_token;

    UpdaterError error = clear_cache_vendor_files(*vendor_snapshot);
    if (error.succeeded()) {
        {
            std::lock_guard<std::mutex> guard(m_model_mutex);
            VendorSync *current = find_vendor_unlocked(vendor_id);
            if (current != nullptr)
                current->has_cache = vendor_snapshot->has_cache;
        }
        notify_vendor_files_changed(VendorChange::ClearCache, ids);
    }
    callback_result(std::move(error));
}

void PresetUpdater::uninstall_all_vendors(std::function<void(UpdaterError)> callback_result)
{
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> guard(m_model_mutex);
        for (const auto &[id, vendor] : m_vendors)
            if (vendor.is_installed)
                ids.emplace_back(id);
    }
    const std::optional<std::string> rollback_token = prepare_vendor_change(VendorChange::Uninstall, ids);
    if (!rollback_token.has_value()) {
        callback_result(make_updater_error(UpdaterError::Code::PreparationRejected));
        return;
    }

    UpdaterError error;
    std::vector<std::pair<std::string, VendorSync>> changed_vendors;
    for (const std::string &id : ids) {
        std::optional<VendorSync> vendor_snapshot = vendor(id);
        if (!vendor_snapshot)
            continue;
        error = uninstall_vendor_files(*vendor_snapshot);
        if (!error.succeeded())
            break;
        changed_vendors.emplace_back(id, std::move(*vendor_snapshot));
    }
    if (!error.succeeded()) {
        error = rollback_vendor_change(*rollback_token, std::move(error));
        callback_result(std::move(error));
        return;
    }

    // Publish model changes only after every filesystem removal succeeds. A
    // failed batch therefore leaves both the model and live files at the
    // state represented by the snapshot.
    {
        std::lock_guard<std::mutex> guard(m_model_mutex);
        for (const std::pair<std::string, VendorSync> &changed : changed_vendors) {
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
    callback_result(std::move(error));
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
    std::vector<std::string> ids;
    std::vector<boost::filesystem::path> source_directories;
    std::vector<VendorSync> changed_vendors;
    ids.reserve(installs.size());
    source_directories.reserve(installs.size());
    changed_vendors.reserve(installs.size());

    // Resolve downloads and validate every cache source before taking the one
    // snapshot that protects this batch. Network failure cannot therefore
    // trigger a pointless restore of untouched live files.
    for (const std::pair<std::string, VendorAvailable> &install : installs) {
        std::optional<VendorSync> vendor_snapshot = vendor(install.first);
        if (!vendor_snapshot.has_value()) {
            callback_result({make_updater_error(UpdaterError::Code::RepositoryNotFound)});
            return;
        }
        boost::filesystem::path source_directory;
        UpdaterError source_error = prepare_vendor_install_source(
            *vendor_snapshot, install.second, source_directory);
        if (!source_error.succeeded()) {
            callback_result({std::move(source_error)});
            return;
        }
        ids.emplace_back(install.first);
        source_directories.emplace_back(std::move(source_directory));
        changed_vendors.emplace_back(std::move(*vendor_snapshot));
    }

    const std::optional<std::string> rollback_token = prepare_vendor_change(change, ids);
    if (!rollback_token.has_value()) {
        callback_result({make_updater_error(UpdaterError::Code::PreparationRejected)});
        return;
    }

    for (size_t idx = 0; idx < changed_vendors.size(); ++idx) {
        UpdaterError error = install_vendor_files(changed_vendors[idx], source_directories[idx]);
        if (!error.succeeded()) {
            error = rollback_vendor_change(*rollback_token, std::move(error));
            callback_result({std::move(error)});
            return;
        }
    }

    // No model entry is changed until every live package has been published.
    // Readers therefore observe either the complete old batch or the complete
    // new batch, matching the filesystem restored by the snapshot.
    {
        std::lock_guard<std::mutex> guard(m_model_mutex);
        for (size_t idx = 0; idx < changed_vendors.size(); ++idx) {
            VendorSync *current = find_vendor_unlocked(ids[idx]);
            if (current != nullptr) {
                current->profile = changed_vendors[idx].profile;
                current->is_installed = changed_vendors[idx].is_installed;
                current->has_cache = changed_vendors[idx].has_cache;
                current->sort_available();
            }
        }
    }
    if (!ids.empty())
        notify_vendor_files_changed(change, ids);
    callback_result(UpdaterErrors());
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
    for (const RepositoryPackageVersion &version : versions) {
        const std::optional<Semver> package_version = Semver::parse(version.package_version);
        const std::optional<Semver> slicer_version = Semver::parse(version.slicer_version);
        if (!package_version || !slicer_version)
            continue;
        const std::vector<VendorAvailable>::iterator existing = std::find_if(
            available_profiles.begin(), available_profiles.end(),
            [&version](const VendorAvailable &candidate) { return candidate.tag == version.tag; });
        if (existing == available_profiles.end()) {
            available_profiles.push_back({*package_version, *slicer_version, "", version.url_zip, version.commit_sha,
                                           version.commit_url, version.tag, ""});
        } else {
            // A local profile may already have created this version entry.
            // Enrich it with remote download and changelog information.
            existing->url_zip = version.url_zip;
            existing->commit_sha = version.commit_sha;
            existing->commit_url = version.commit_url;
        }
    }
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
