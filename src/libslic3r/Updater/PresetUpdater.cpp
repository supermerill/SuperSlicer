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
#include <boost/property_tree/ini_parser.hpp>

#include "libslic3r/Plugins/PluginRepository.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Updater/UpdaterHttp.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/libslic3r.h"

namespace Slic3r {
namespace {

const char *const k_vendor_cache_directory = "cache/vendor";

boost::filesystem::path data_path();
boost::filesystem::path vendor_cache_directory(const VendorProfile &profile);
bool transfer_vendor_files(const boost::filesystem::path &input_directory,
                           const boost::filesystem::path &output_directory,
                           const std::string &vendor_id,
                           bool copy);
bool resolve_vendor_archive_root(const boost::filesystem::path &staging,
                                 boost::filesystem::path &archive_root,
                                 std::string &error_message);
UpdaterError publish_vendor_profiles(const boost::filesystem::path &archive_root,
                                     const boost::filesystem::path &package_root,
                                     const std::string &vendor_id);
UpdaterError extract_vendor_package(const boost::filesystem::path &archive_path,
                                    const boost::filesystem::path &package_root,
                                    const std::string &vendor_id);
UpdaterError save_vendor_description(const std::string &contents, const std::string &fallback_id);

boost::filesystem::path data_path()
{
    return boost::filesystem::path(data_dir());
}

boost::filesystem::path vendor_cache_directory(const VendorProfile &profile)
{
    return data_path() / k_vendor_cache_directory / profile.usable_id();
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

// Accepts the two layouts produced by regular ZIP tools and GitHub source
// archives. Restricting the wrapper to one directory keeps profile discovery
// deterministic and prevents unrelated archive trees from entering the cache.
bool resolve_vendor_archive_root(const boost::filesystem::path &staging,
                                 boost::filesystem::path &archive_root,
                                 std::string &error_message)
{
    archive_root = staging;
    if (boost::filesystem::is_directory(archive_root / "profiles"))
        return true;

    boost::filesystem::directory_iterator entry(staging);
    const boost::filesystem::directory_iterator end;
    if (entry == end || !boost::filesystem::is_directory(entry->path())) {
        error_message = "The archive has no profiles directory.";
        return false;
    }

    archive_root = entry->path();
    ++entry;
    if (entry != end || !boost::filesystem::is_directory(archive_root / "profiles")) {
        error_message = "The archive root directory is invalid.";
        return false;
    }
    return true;
}

// Publish only the profiles tree below the permanent vendor directory. The
// surrounding directory also stores repository descriptions, tags and logs,
// which must survive a profile update.
UpdaterError publish_vendor_profiles(const boost::filesystem::path &archive_root,
                                     const boost::filesystem::path &package_root,
                                     const std::string &vendor_id)
{
    const boost::filesystem::path source_profiles = archive_root / "profiles";
    if (!boost::filesystem::is_regular_file(source_profiles / (vendor_id + ".ini")))
        return make_updater_error(UpdaterError::Code::InvalidArchive,
                                  "The archive does not contain the expected vendor profile.");

    const boost::filesystem::path destination_profiles = package_root / "profiles";
    const boost::filesystem::path previous_profiles = package_root /
        boost::filesystem::unique_path(".profiles.previous-%%%%-%%%%");
    const bool replace_existing = boost::filesystem::exists(destination_profiles);
    boost::filesystem::create_directories(package_root);

    // Keep the previous tree until the validated replacement is in place. Both
    // directories are siblings, so the publication uses atomic renames.
    if (replace_existing)
        boost::filesystem::rename(destination_profiles, previous_profiles);
    try {
        boost::filesystem::rename(source_profiles, destination_profiles);
    } catch (...) {
        if (replace_existing && !boost::filesystem::exists(destination_profiles))
            boost::filesystem::rename(previous_profiles, destination_profiles);
        throw;
    }

    boost::system::error_code ignored_error;
    if (replace_existing)
        boost::filesystem::remove_all(previous_profiles, ignored_error);
    return UpdaterError();
}

// The downloaded archive may either contain profiles/ directly or wrap it in
// one top-level directory, as GitHub source archives do. Only those two
// layouts are accepted so an archive cannot publish an unexpected tree.
UpdaterError extract_vendor_package(const boost::filesystem::path &archive_path,
                                    const boost::filesystem::path &package_root,
                                    const std::string &vendor_id)
{
    std::string error_message;
    const boost::filesystem::path staging = package_root.parent_path() /
        boost::filesystem::unique_path("." + vendor_id + ".extract-%%%%-%%%%");
    try {
        if (!extract_repository_archive(archive_path, staging, error_message)) {
            boost::filesystem::remove_all(staging);
            return make_updater_error(UpdaterError::Code::InvalidArchive, std::move(error_message));
        }

        boost::filesystem::path extracted_root;
        if (!resolve_vendor_archive_root(staging, extracted_root, error_message)) {
            boost::filesystem::remove_all(staging);
            return make_updater_error(UpdaterError::Code::InvalidArchive, std::move(error_message));
        }

        const UpdaterError publish_error = publish_vendor_profiles(extracted_root, package_root, vendor_id);
        boost::filesystem::remove_all(staging);
        return publish_error;
    } catch (const boost::filesystem::filesystem_error &error) {
        boost::filesystem::remove_all(staging);
        return make_updater_error(UpdaterError::Code::Filesystem, error.what());
    }
}

// Writes a repository description only after it has been parsed and checked.
// The cache is then a valid local source for the next repository reload.
UpdaterError save_vendor_description(const std::string &contents, const std::string &fallback_id)
{
    try {
        boost::property_tree::ptree root;
        std::stringstream stream(contents);
        boost::property_tree::read_ini(stream, root);
        const VendorProfile profile = VendorProfile::from_ini(root, fallback_id, false);

        RepositoryDescription description;
        std::string description_error;
        if (!parse_repository_description(contents, RepositoryPackageType::Vendor, description, description_error) ||
            description.id != profile.id)
            return make_updater_error(UpdaterError::Code::InvalidArchive, std::move(description_error));

        const boost::filesystem::path directory = vendor_cache_directory(profile);
        boost::filesystem::create_directories(directory);
        boost::nowide::ofstream output((directory / (profile.usable_id() + ".ini")).string(),
                                       std::ios::out | std::ios::trunc);
        if (!output)
            return make_updater_error(UpdaterError::Code::Filesystem, "Cannot create vendor description file.");
        output << contents;
        return UpdaterError();
    } catch (const boost::filesystem::filesystem_error &error) {
        return make_updater_error(UpdaterError::Code::Filesystem, error.what());
    } catch (const std::exception &error) {
        return make_updater_error(UpdaterError::Code::InvalidArchive, error.what());
    }
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
        vendor.reset(installed_vendor, true, boost::filesystem::is_directory(vendor_cache_directory(installed_vendor)));
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
    const boost::filesystem::path cache_directory = configuration_directory / k_vendor_cache_directory;
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
    load_unused_vendors(vendor_ids, configuration_directory / "vendor", true);

    // Built-in profiles are copied to the vendor's canonical profiles tree.
    // Built-in, imported and downloaded profiles therefore share one layout.
    if (boost::filesystem::is_directory(profiles_directory)) {
        for (const boost::filesystem::directory_entry &entry : boost::filesystem::directory_iterator(profiles_directory)) {
            if (entry.path().extension() != ".ini")
                continue;
            try {
                const VendorProfile profile = VendorProfile::from_ini(entry.path(), false);
                const boost::filesystem::path package_root = repository_package_cache_path(
                    configuration_directory, RepositoryPackageType::Vendor, profile.usable_id(),
                    profile.config_version.to_string(), profile.slicer_version.to_string());
                if (!boost::filesystem::is_regular_file(package_root / "profiles" / (profile.id + ".ini")))
                    transfer_vendor_files(entry.path().parent_path(), package_root / "profiles", profile.id, true);
            } catch (const std::exception &error) {
                BOOST_LOG_TRIVIAL(warning) << "Cannot cache built-in vendor profile '" << entry.path().string()
                                           << "': " << error.what();
            }
        }
    }

    if (boost::filesystem::is_directory(cache_directory)) {
        for (const boost::filesystem::directory_entry &entry : boost::filesystem::directory_iterator(cache_directory)) {
            if (!boost::filesystem::is_directory(entry.path()))
                continue;
            const boost::filesystem::path profiles = entry.path() / "profiles";
            if (boost::filesystem::is_directory(profiles))
                load_unused_vendors(vendor_ids, profiles, false);
        }
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
    const boost::filesystem::path cache_file = vendor_cache_directory(vendor.profile) /
        (vendor.profile.usable_id() + "_tags.json");
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

    const boost::filesystem::path log_directory = vendor_cache_directory(vendor->profile) / "logs";
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

    const boost::filesystem::path cache_directory = data_path() / k_vendor_cache_directory;
    const boost::filesystem::path staging = cache_directory /
        boost::filesystem::unique_path(".vendor-import-%%%%-%%%%");
    std::string error_message;

    try {
        boost::filesystem::create_directories(cache_directory);
        if (!extract_repository_archive(archive_path, staging, error_message)) {
            boost::filesystem::remove_all(staging);
            return make_updater_error(UpdaterError::Code::InvalidArchive, std::move(error_message));
        }

        // A local archive has no repository row telling us which vendor it
        // contains. Discover exactly one profile before choosing its cache key.
        boost::filesystem::path archive_root;
        if (!resolve_vendor_archive_root(staging, archive_root, error_message)) {
            boost::filesystem::remove_all(staging);
            return make_updater_error(UpdaterError::Code::InvalidArchive, std::move(error_message));
        }

        boost::filesystem::path profile_path;
        const boost::filesystem::path profiles_directory = archive_root / "profiles";
        for (const boost::filesystem::directory_entry &entry :
             boost::filesystem::directory_iterator(profiles_directory)) {
            if (!boost::filesystem::is_regular_file(entry.path()) || entry.path().extension() != ".ini")
                continue;
            if (!profile_path.empty()) {
                boost::filesystem::remove_all(staging);
                return make_updater_error(UpdaterError::Code::InvalidArchive,
                                          "A vendor archive must contain exactly one profile INI file.");
            }
            profile_path = entry.path();
        }
        if (profile_path.empty()) {
            boost::filesystem::remove_all(staging);
            return make_updater_error(UpdaterError::Code::InvalidArchive,
                                      "The vendor archive profiles directory contains no INI file.");
        }

        // Parsing with load_all=true checks the version and complete vendor
        // metadata now, before a malformed package can replace a cached one.
        const VendorProfile profile = VendorProfile::from_ini(profile_path, true);
        if (profile_path.filename() != profile.id + ".ini") {
            boost::filesystem::remove_all(staging);
            return make_updater_error(UpdaterError::Code::InvalidArchive,
                                      "The vendor profile filename must match its vendor id.");
        }

        const boost::filesystem::path package_root = repository_package_cache_path(
            data_path(), RepositoryPackageType::Vendor, profile.usable_id(),
            profile.config_version.to_string(), profile.slicer_version.to_string());
        const UpdaterError publish_error = publish_vendor_profiles(archive_root, package_root, profile.id);

        boost::system::error_code ignored_error;
        boost::filesystem::remove_all(staging, ignored_error);
        return publish_error;
    } catch (const boost::filesystem::filesystem_error &error) {
        boost::system::error_code ignored_error;
        boost::filesystem::remove_all(staging, ignored_error);
        return make_updater_error(UpdaterError::Code::Filesystem, error.what());
    } catch (const std::exception &error) {
        boost::system::error_code ignored_error;
        boost::filesystem::remove_all(staging, ignored_error);
        return make_updater_error(UpdaterError::Code::InvalidArchive, error.what());
    }
}

UpdaterError PresetUpdater::cache_vendor_ini(const boost::filesystem::path &profile_path)
{
    if (!boost::filesystem::is_regular_file(profile_path))
        return make_updater_error(UpdaterError::Code::ArchiveUnavailable,
                                  "The selected vendor profile does not exist.");

    try {
        // Parse before touching the cache so a malformed profile cannot replace
        // a previously usable version of the same vendor.
        const VendorProfile profile = VendorProfile::from_ini(profile_path, true);
        if (profile_path.filename().string() != profile.id + ".ini")
            return make_updater_error(UpdaterError::Code::InvalidArchive,
                                      "The vendor profile filename must match its vendor id.");

        // The profiles subdirectory is the same layout used by extracted
        // packages. transfer_vendor_files also replaces the matching icon set.
        const boost::filesystem::path package_root = repository_package_cache_path(
            data_path(), RepositoryPackageType::Vendor, profile.usable_id(),
            profile.config_version.to_string(), profile.slicer_version.to_string());
        const boost::filesystem::path profiles_directory = package_root / "profiles";
        if (!transfer_vendor_files(profile_path.parent_path(), profiles_directory, profile.id, true))
            return make_updater_error(UpdaterError::Code::Filesystem,
                                      "Cannot copy the vendor profile into the cache.");
        return UpdaterError();
    } catch (const boost::filesystem::filesystem_error &error) {
        return make_updater_error(UpdaterError::Code::Filesystem, error.what());
    } catch (const std::exception &error) {
        return make_updater_error(UpdaterError::Code::InvalidArchive, error.what());
    }
}

UpdaterError PresetUpdater::install_vendor_files(VendorSync &vendor, const VendorAvailable &version)
{
    const boost::filesystem::path vendor_directory = data_path() / "vendor";
    const boost::filesystem::path package_root = repository_package_cache_path(
        data_path(), RepositoryPackageType::Vendor, vendor.profile.usable_id(),
        version.config_version.to_string(), version.slicer_version.to_string());
    try {
        if (!version.local_file.empty()) {
            if (!transfer_vendor_files(boost::filesystem::path(version.local_file).parent_path(), vendor_directory,
                                       vendor.profile.id, true))
                return make_updater_error(UpdaterError::Code::Filesystem);
        } else {
            if (!boost::filesystem::is_regular_file(package_root / "profiles" / (vendor.profile.id + ".ini"))) {
                const boost::filesystem::path archive_path = vendor_cache_directory(vendor.profile) /
                    (vendor.profile.usable_id() + ".zip");
                UpdaterError download_error = download_repository_file_sync(
                    version.url_zip, archive_path, 130 * 1024 * 1024);
                if (!download_error.succeeded())
                    return download_error;

                UpdaterError extraction_error = extract_vendor_package(archive_path, package_root, vendor.profile.usable_id());
                if (!extraction_error.succeeded())
                    return extraction_error;
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
        const boost::filesystem::path package_root = repository_package_cache_path(
            data_path(), RepositoryPackageType::Vendor, vendor.profile.usable_id(),
            vendor.profile.config_version.to_string(), vendor.profile.slicer_version.to_string());
        if (!transfer_vendor_files(data_path() / "vendor", package_root / "profiles", vendor.profile.id, true))
            return make_updater_error(UpdaterError::Code::Filesystem);

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
