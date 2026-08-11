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
                           const std::string &vendor_id,
                           bool copy);
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

// Copies or moves one vendor INI and its icon directory. The temporary INI
// avoids publishing a partially copied profile when an update is interrupted.
bool transfer_vendor_files(const boost::filesystem::path &input_directory,
                           const boost::filesystem::path &output_directory,
                           const std::string &vendor_id,
                           bool copy)
{
    const boost::filesystem::path input_ini = input_directory / (vendor_id + ".ini");
    const boost::filesystem::path output_ini = output_directory / (vendor_id + ".ini");
    if (!boost::filesystem::is_regular_file(input_ini))
        return false;

    boost::filesystem::create_directories(output_directory);
    if (copy) {
        std::string copy_error;
        if (copy_file(input_ini.string(), output_ini.string(), copy_error, true) != CopyFileResult::SUCCESS)
            return false;
    } else {
        boost::filesystem::rename(input_ini, output_ini);
    }

    const boost::filesystem::path input_icons = input_directory / vendor_id;
    const boost::filesystem::path output_icons = output_directory / vendor_id;
    if (!boost::filesystem::is_directory(input_icons))
        return true;

    boost::filesystem::remove_all(output_icons);
    boost::filesystem::create_directories(output_icons);
    for (const boost::filesystem::directory_entry &entry : boost::filesystem::directory_iterator(input_icons)) {
        const boost::filesystem::path output = output_icons / entry.path().filename();
        if (copy)
            boost::filesystem::copy_file(entry.path(), output, boost::filesystem::copy_option::overwrite_if_exists);
        else
            boost::filesystem::rename(entry.path(), output);
    }
    if (!copy)
        boost::filesystem::remove_all(input_icons);
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

VendorSync *PresetUpdater::get_vendor(const std::string &id)
{
    const std::map<std::string, VendorSync>::iterator it = m_vendors.find(id);
    return it == m_vendors.end() ? nullptr : &it->second;
}

const VendorSync *PresetUpdater::get_vendor(const std::string &id) const
{
    const std::map<std::string, VendorSync>::const_iterator it = m_vendors.find(id);
    return it == m_vendors.end() ? nullptr : &it->second;
}

std::vector<VendorSync> PresetUpdater::vendors() const
{
    std::lock_guard<std::recursive_mutex> guard(m_vendors_mutex);
    std::vector<VendorSync> result;
    result.reserve(m_vendors.size());
    for (const auto &[id, vendor] : m_vendors) {
        result.emplace_back(vendor);
        // best points into available_profiles, so each copied view needs to
        // rebuild that pointer for its own vector storage.
        result.back().sort_available();
    }
    return result;
}

void PresetUpdater::set_installed_vendors(const PresetBundle *preset_bundle)
{
    if (preset_bundle == nullptr)
        return;

    std::lock_guard<std::recursive_mutex> guard(m_vendors_mutex);
    for (const auto &[id, installed_vendor] : preset_bundle->vendors) {
        VendorSync &vendor = m_vendors[installed_vendor.id];
        vendor.reset(installed_vendor, true, boost::filesystem::is_directory(
            repository_cache_root_path(data_path(), RepositoryPackageType::Vendor, installed_vendor.id)));
        if (!installed_vendor.config_update_rest.empty())
            m_is_synchronized = false;
    }
}

void PresetUpdater::load_unused_vendors(std::set<std::string> &vendor_ids,
                                        const boost::filesystem::path &vendor_directory,
                                        bool is_installed)
{
    if (!boost::filesystem::is_directory(vendor_directory))
        return;

    std::lock_guard<std::recursive_mutex> guard(m_vendors_mutex);
    for (const boost::filesystem::directory_entry &entry : boost::filesystem::directory_iterator(vendor_directory)) {
        if (entry.path().extension() != ".ini")
            continue;
        try {
            VendorProfile profile = VendorProfile::from_ini(entry.path(), false);
            VendorSync &vendor = m_vendors[profile.id];
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
                m_is_synchronized = false;
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

    {
        std::lock_guard<std::recursive_mutex> guard(m_vendors_mutex);
        // HTTP callbacks retain references to map entries. A reload while a
        // refresh is pending would invalidate those references.
        if (sync_in_progress() || changelog_download_in_progress())
            return;
        m_vendors.clear();
        m_is_synchronized = false;
    }
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

    load_unused_vendors(vendor_ids, installed_directory, true);
    for (const RepositoryCachedEntry &repository : cache.scan()) {
        // The root descriptor makes a local-only or newly configured remote
        // repository visible before it has any downloaded version.
        load_unused_vendors(vendor_ids, repository.directory, false);
        for (const RepositoryCachedVersion &version : repository.versions)
            load_unused_vendors(vendor_ids, version.directory / "profiles", false);
    }
}

void PresetUpdater::sync_async(std::function<void(int)> callback_result, bool force)
{
    std::lock_guard<std::recursive_mutex> guard(m_vendors_mutex);
    if (!begin_sync(m_vendors.size(), callback_result)) {
        callback_result(get_profile_count_to_update());
        return;
    }
    for (auto &[id, vendor] : m_vendors)
        update_vendor(vendor, force);
}

void PresetUpdater::update_vendor(VendorSync &vendor, bool force)
{
    const RepositoryPackageCache cache(data_path(), vendor_repository_cache_adapter());
    const boost::filesystem::path cache_file = cache.repository_tags_path(vendor.profile.id);
    vendor.synch_in_progress = true;
    refresh_repository_tags(
        vendor.profile.id, vendor.profile.config_update_rest, cache_file, force,
        [&vendor](const std::string &tags) { return vendor.parse_tags(tags); },
        [&vendor](bool succeeded) {
            vendor.synch_failed = !succeeded;
            vendor.synch_in_progress = false;
        });
}

void PresetUpdater::download_changelogs(const std::string &vendor_id,
                                        std::function<void(bool)> callback_result,
                                        bool force)
{
    std::vector<RepositoryChangelogVersion> versions;
    std::lock_guard<std::recursive_mutex> guard(m_vendors_mutex);
    VendorSync *vendor = get_vendor(vendor_id);
    if (vendor == nullptr) {
        callback_result(false);
        return;
    }

    const RepositoryPackageCache cache(data_path(), vendor_repository_cache_adapter());
    const boost::filesystem::path log_directory = cache.repository_logs_directory(vendor->profile.id);
    for (VendorAvailable &version : vendor->available_profiles) {
        RepositoryChangelogVersion common_version;
        common_version.content_version = version.config_version;
        common_version.slicer_version = version.slicer_version;
        common_version.tag = version.tag;
        common_version.commit_sha = version.commit_sha;
        common_version.commit_url = version.commit_url;
        common_version.store_notes = [&version](std::string notes) { version.notes = std::move(notes); };
        versions.emplace_back(std::move(common_version));
    }
    download_repository_version_changelogs(std::move(versions), log_directory,
                                           vendor->profile.config_update_rest,
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

UpdaterError PresetUpdater::install_vendor_files(VendorSync &vendor, const VendorAvailable &version)
{
    const boost::filesystem::path vendor_directory = data_path() / "vendor";
    boost::filesystem::path package_root = repository_package_cache_path(
        data_path(), RepositoryPackageType::Vendor, vendor.profile.id,
        version.config_version.to_string(), version.slicer_version.to_string());
    try {
        if (!version.local_file.empty()) {
            if (!transfer_vendor_files(boost::filesystem::path(version.local_file).parent_path(), vendor_directory,
                                       vendor.profile.id, true))
                return make_updater_error(UpdaterError::Code::Filesystem);
        } else {
            if (!boost::filesystem::is_regular_file(package_root / "profiles" / (vendor.profile.id + ".ini"))) {
                const boost::filesystem::path archive_path = vendor_cache_directory(vendor.profile) /
                    (RepositoryPackageCache::version_directory_name(
                        version.config_version.to_string(), version.slicer_version.to_string()) + ".zip");
                UpdaterError download_error = download_repository_file_sync(
                    version.url_zip, archive_path, 130 * 1024 * 1024);
                if (!download_error.succeeded())
                    return download_error;

                RepositoryDescription expected;
                expected.type = RepositoryPackageType::Vendor;
                expected.id = vendor.profile.id;
                expected.package_version = version.config_version.to_string();
                expected.slicer_version = version.slicer_version.to_string();
                RepositoryPackageCache cache(data_path(), vendor_repository_cache_adapter());
                RepositoryCachedVersion cached;
                std::string error_message;
                if (!cache.cache_archive(archive_path, expected, cached, error_message))
                    return make_updater_error(UpdaterError::Code::InvalidArchive, std::move(error_message));
                boost::system::error_code cleanup_error;
                boost::filesystem::remove(archive_path, cleanup_error);
                package_root = cached.directory;
            }
            if (!transfer_vendor_files(package_root / "profiles", vendor_directory, vendor.profile.id, true))
                return make_updater_error(UpdaterError::Code::Filesystem, "Cannot copy the vendor profile from the package cache.");
        }
        vendor.profile = VendorProfile::from_ini(vendor_directory / (vendor.profile.id + ".ini"), true);
        vendor.is_installed = true;
        vendor.has_cache = boost::filesystem::is_directory(vendor_cache_directory(vendor.profile));
        vendor.can_upgrade = vendor.best != nullptr && vendor.best->config_version > vendor.profile.config_version;
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
        vendor.is_synch = false;
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

bool PresetUpdater::prepare_vendor_change(VendorChange change, const std::vector<std::string> &vendor_ids)
{
    return m_host == nullptr || m_host->prepare_vendor_change(change, vendor_ids);
}

void PresetUpdater::notify_vendor_files_changed(VendorChange change, const std::vector<std::string> &vendor_ids)
{
    if (m_host != nullptr)
        m_host->vendor_files_changed(*this, change, vendor_ids);
}

void PresetUpdater::uninstall_vendor(const std::string &vendor_id, std::function<void(UpdaterError)> callback_result)
{
    const std::vector<std::string> ids{vendor_id};
    {
        std::lock_guard<std::recursive_mutex> guard(m_vendors_mutex);
        if (get_vendor(vendor_id) == nullptr) {
            callback_result(make_updater_error(UpdaterError::Code::RepositoryNotFound));
            return;
        }
    }
    if (!prepare_vendor_change(VendorChange::Uninstall, ids)) {
        callback_result(make_updater_error(UpdaterError::Code::PreparationRejected));
        return;
    }

    UpdaterError error;
    {
        std::lock_guard<std::recursive_mutex> guard(m_vendors_mutex);
        VendorSync *vendor = get_vendor(vendor_id);
        if (vendor == nullptr) {
            callback_result(make_updater_error(UpdaterError::Code::RepositoryNotFound));
            return;
        }
        error = uninstall_vendor_files(*vendor);
    }
    if (error.succeeded())
        notify_vendor_files_changed(VendorChange::Uninstall, ids);
    callback_result(std::move(error));
}

void PresetUpdater::install_vendor(const std::string &vendor_id,
                                   const VendorAvailable &version,
                                   std::function<void(UpdaterError)> callback_result)
{
    const std::vector<std::string> ids{vendor_id};
    {
        std::lock_guard<std::recursive_mutex> guard(m_vendors_mutex);
        if (get_vendor(vendor_id) == nullptr) {
            callback_result(make_updater_error(UpdaterError::Code::RepositoryNotFound));
            return;
        }
    }
    if (!prepare_vendor_change(VendorChange::Install, ids)) {
        callback_result(make_updater_error(UpdaterError::Code::PreparationRejected));
        return;
    }

    UpdaterError error;
    {
        std::lock_guard<std::recursive_mutex> guard(m_vendors_mutex);
        VendorSync *vendor = get_vendor(vendor_id);
        if (vendor == nullptr) {
            callback_result(make_updater_error(UpdaterError::Code::RepositoryNotFound));
            return;
        }
        error = install_vendor_files(*vendor, version);
    }
    if (error.succeeded())
        notify_vendor_files_changed(VendorChange::Install, ids);
    callback_result(std::move(error));
}

void PresetUpdater::clear_cache_vendor(const std::string &vendor_id, std::function<void(UpdaterError)> callback_result)
{
    const std::vector<std::string> ids{vendor_id};
    {
        std::lock_guard<std::recursive_mutex> guard(m_vendors_mutex);
        if (get_vendor(vendor_id) == nullptr) {
            callback_result(make_updater_error(UpdaterError::Code::RepositoryNotFound));
            return;
        }
    }
    if (!prepare_vendor_change(VendorChange::ClearCache, ids)) {
        callback_result(make_updater_error(UpdaterError::Code::PreparationRejected));
        return;
    }

    UpdaterError error;
    {
        std::lock_guard<std::recursive_mutex> guard(m_vendors_mutex);
        VendorSync *vendor = get_vendor(vendor_id);
        if (vendor == nullptr) {
            callback_result(make_updater_error(UpdaterError::Code::RepositoryNotFound));
            return;
        }
        error = clear_cache_vendor_files(*vendor);
    }
    if (error.succeeded())
        notify_vendor_files_changed(VendorChange::ClearCache, ids);
    callback_result(std::move(error));
}

void PresetUpdater::uninstall_all_vendors(std::function<void(UpdaterError)> callback_result)
{
    std::vector<std::string> ids;
    {
        std::lock_guard<std::recursive_mutex> guard(m_vendors_mutex);
        for (const auto &[id, vendor] : m_vendors)
            if (vendor.is_installed)
                ids.emplace_back(id);
    }
    if (!prepare_vendor_change(VendorChange::Uninstall, ids)) {
        callback_result(make_updater_error(UpdaterError::Code::PreparationRejected));
        return;
    }

    UpdaterError error;
    std::vector<std::string> changed_ids;
    {
        std::lock_guard<std::recursive_mutex> guard(m_vendors_mutex);
        for (const std::string &id : ids) {
            const std::map<std::string, VendorSync>::iterator vendor = m_vendors.find(id);
            if (vendor == m_vendors.end())
                continue;
            error = uninstall_vendor_files(vendor->second);
            if (!error.succeeded())
                break;
            changed_ids.emplace_back(id);
        }
    }
    if (!changed_ids.empty())
        notify_vendor_files_changed(VendorChange::Uninstall, changed_ids);
    callback_result(std::move(error));
}

void PresetUpdater::install_all_vendors(std::function<void(UpdaterErrors)> callback_result)
{
    std::vector<std::string> ids;
    {
        std::lock_guard<std::recursive_mutex> guard(m_vendors_mutex);
        for (const auto &[id, vendor] : m_vendors)
            if (!vendor.is_installed && vendor.best != nullptr)
                ids.emplace_back(id);
    }
    if (!prepare_vendor_change(VendorChange::InstallAll, ids)) {
        callback_result({make_updater_error(UpdaterError::Code::PreparationRejected)});
        return;
    }

    UpdaterErrors errors;
    std::vector<std::string> changed_ids;
    {
        std::lock_guard<std::recursive_mutex> guard(m_vendors_mutex);
        for (const std::string &id : ids) {
            const std::map<std::string, VendorSync>::iterator vendor = m_vendors.find(id);
            if (vendor == m_vendors.end() || vendor->second.best == nullptr)
                continue;
            UpdaterError error = install_vendor_files(vendor->second, *vendor->second.best);
            if (error.succeeded())
                changed_ids.emplace_back(id);
            else
                errors.emplace_back(std::move(error));
        }
    }
    if (!changed_ids.empty())
        notify_vendor_files_changed(VendorChange::InstallAll, changed_ids);
    callback_result(std::move(errors));
}

void PresetUpdater::upgrade_all_installed_vendors(std::function<void(UpdaterErrors)> callback_result)
{
    std::vector<std::string> ids;
    {
        std::lock_guard<std::recursive_mutex> guard(m_vendors_mutex);
        for (const auto &[id, vendor] : m_vendors)
            if (vendor.is_installed && vendor.can_upgrade && vendor.best != nullptr)
                ids.emplace_back(id);
    }
    if (!prepare_vendor_change(VendorChange::UpgradeAll, ids)) {
        callback_result({make_updater_error(UpdaterError::Code::PreparationRejected)});
        return;
    }

    UpdaterErrors errors;
    std::vector<std::string> changed_ids;
    {
        std::lock_guard<std::recursive_mutex> guard(m_vendors_mutex);
        for (const std::string &id : ids) {
            const std::map<std::string, VendorSync>::iterator vendor = m_vendors.find(id);
            if (vendor == m_vendors.end() || vendor->second.best == nullptr)
                continue;
            UpdaterError error = install_vendor_files(vendor->second, *vendor->second.best);
            if (error.succeeded())
                changed_ids.emplace_back(id);
            else
                errors.emplace_back(std::move(error));
        }
    }
    if (!changed_ids.empty())
        notify_vendor_files_changed(VendorChange::UpgradeAll, changed_ids);
    callback_result(std::move(errors));
}

int PresetUpdater::get_profile_count_to_update() const
{
    int count = 0;
    std::lock_guard<std::recursive_mutex> guard(m_vendors_mutex);
    for (const auto &[id, vendor] : m_vendors)
        if (vendor.can_upgrade)
            ++count;
    return count;
}

size_t PresetUpdater::count_available() const
{
    std::lock_guard<std::recursive_mutex> guard(m_vendors_mutex);
    return m_vendors.size();
}

size_t PresetUpdater::count_installed() const
{
    std::lock_guard<std::recursive_mutex> guard(m_vendors_mutex);
    return static_cast<size_t>(std::count_if(m_vendors.begin(), m_vendors.end(),
        [](const std::pair<const std::string, VendorSync> &entry) { return entry.second.is_installed; }));
}

int PresetUpdater::update_count()
{
    return get_profile_count_to_update();
}

bool PresetUpdater::is_synchronized() const
{
    std::lock_guard<std::recursive_mutex> guard(m_vendors_mutex);
    return m_is_synchronized;
}

void PresetUpdater::on_sync_completed()
{
    std::lock_guard<std::recursive_mutex> guard(m_vendors_mutex);
    m_is_synchronized = true;
}

void VendorSync::reset(const VendorProfile &new_profile, bool installed, bool cache_present)
{
    profile = new_profile;
    is_installed = installed;
    has_cache = cache_present;
    synch_in_progress = false;
    synch_failed = false;
    can_upgrade = best != nullptr && best->config_version > profile.config_version;
}

bool VendorSync::parse_tags(const std::string &json)
{
    std::vector<RepositoryPackageVersion> versions;
    std::string error_message;
    if (!parse_repository_versions(json, versions, error_message)) {
        BOOST_LOG_TRIVIAL(warning) << error_message;
        return false;
    }
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
    is_synch = true;
    return true;
}

void VendorSync::sort_available()
{
    std::sort(available_profiles.begin(), available_profiles.end(), [](const VendorAvailable &left, const VendorAvailable &right) {
        if (left.slicer_version != right.slicer_version)
            return left.slicer_version > right.slicer_version;
        return left.config_version > right.config_version;
    });
    best = nullptr;
    const std::optional<Semver> current_slicer_version = Semver::parse(SLIC3R_VERSION_FULL);
    if (!current_slicer_version)
        return;
    for (VendorAvailable &available : available_profiles) {
        if (available.slicer_version <= *current_slicer_version) {
            best = &available;
            break;
        }
    }
    can_upgrade = is_installed && best != nullptr && best->config_version > profile.config_version;
}

} // namespace Slic3r
