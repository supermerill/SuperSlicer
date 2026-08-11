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
        const boost::filesystem::path temporary_ini = output_directory / (vendor_id + ".new.ini");
        boost::filesystem::copy_file(input_ini, temporary_ini, boost::filesystem::copy_option::overwrite_if_exists);
        boost::filesystem::rename(temporary_ini, output_ini);
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

        boost::filesystem::path extracted_root = staging;
        if (!boost::filesystem::is_directory(extracted_root / "profiles")) {
            boost::filesystem::directory_iterator entry(staging);
            const boost::filesystem::directory_iterator end;
            if (entry == end || !boost::filesystem::is_directory(entry->path())) {
                boost::filesystem::remove_all(staging);
                return make_updater_error(UpdaterError::Code::InvalidArchive, "The archive has no profiles directory.");
            }
            extracted_root = entry->path();
            ++entry;
            if (entry != end || !boost::filesystem::is_directory(extracted_root / "profiles")) {
                boost::filesystem::remove_all(staging);
                return make_updater_error(UpdaterError::Code::InvalidArchive, "The archive root directory is invalid.");
            }
        }

        boost::filesystem::rename(extracted_root, package_root);
        if (extracted_root != staging)
            boost::filesystem::remove_all(staging);
        return UpdaterError();
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

    // Built-in profiles are copied to their versioned cache location. This
    // gives built-in and downloaded bundles the same installation source.
    if (boost::filesystem::is_directory(profiles_directory)) {
        for (const boost::filesystem::directory_entry &entry : boost::filesystem::directory_iterator(profiles_directory)) {
            if (entry.path().extension() != ".ini")
                continue;
            try {
                const VendorProfile profile = VendorProfile::from_ini(entry.path(), false);
                const boost::filesystem::path package_root = repository_package_cache_path(
                    configuration_directory, RepositoryPackageType::Vendor, profile.usable_id(),
                    profile.config_version.to_string(), profile.slicer_version.to_string());
                if (!boost::filesystem::exists(package_root))
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
            if (boost::filesystem::is_directory(profiles)) {
                load_unused_vendors(vendor_ids, profiles, false);
                continue;
            }
            // Repository descriptions live one directory above downloaded
            // package versions, so inspect both possible cache layouts.
            for (const boost::filesystem::directory_entry &version_entry : boost::filesystem::directory_iterator(entry.path())) {
                if (boost::filesystem::is_directory(version_entry.path()) &&
                    boost::filesystem::is_directory(version_entry.path() / "profiles"))
                    load_unused_vendors(vendor_ids, version_entry.path() / "profiles", false);
            }
            if (!boost::filesystem::is_directory(profiles))
                load_unused_vendors(vendor_ids, entry.path(), false);
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
    const std::string rest_url = VendorProfile::get_http_url_rest(vendor.profile.config_update_rest);
    vendor.synch_in_progress = true;
    refresh_repository_tags(
        vendor.profile.id, rest_url, cache_file, force,
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
    std::vector<RepositoryChangelogRequest> requests;
    std::lock_guard<std::recursive_mutex> guard(m_vendors_mutex);
    VendorSync *vendor = get_vendor(vendor_id);
    if (vendor == nullptr) {
        callback_result(false);
        return;
    }

    const boost::filesystem::path log_directory = vendor_cache_directory(vendor->profile) / "logs";
    const std::string repository_url = VendorProfile::get_http_url_rest(vendor->profile.config_update_rest);
    for (VendorAvailable &version : vendor->available_profiles) {
        if (version.commit_sha.empty())
            continue;

        VendorAvailable *previous = nullptr;
        for (VendorAvailable &candidate : vendor->available_profiles) {
            if (candidate.commit_sha.empty() || candidate.config_version >= version.config_version)
                continue;
            if (candidate.slicer_version.no_patch() == version.slicer_version.no_patch() &&
                (previous == nullptr || candidate.config_version > previous->config_version))
                previous = &candidate;
        }
        if (previous == nullptr) {
            for (VendorAvailable &candidate : vendor->available_profiles) {
                if (candidate.commit_sha.empty() || candidate.config_version >= version.config_version ||
                    candidate.slicer_version.no_patch() > version.slicer_version.no_patch())
                    continue;
                if (previous == nullptr || candidate.config_version > previous->config_version)
                    previous = &candidate;
            }
        }

        RepositoryChangelogRequest request;
        request.compare = previous != nullptr;
        request.cache_file = log_directory /
            (request.compare ? previous->tag + "..." + version.tag + ".json" : version.tag + ".json");
        request.url = request.compare ? repository_url + "/compare/" + previous->tag + "..." + version.tag :
                                        version.commit_url;
        request.store_notes = [&version](std::string notes) { version.notes = std::move(notes); };
        requests.emplace_back(std::move(request));
    }
    download_repository_changelogs(std::move(requests), std::move(callback_result), force);
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
            if (!boost::filesystem::is_directory(package_root)) {
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
        if (!transfer_vendor_files(data_path() / "vendor", vendor_cache_directory(vendor.profile), vendor.profile.id, false))
            return make_updater_error(UpdaterError::Code::Filesystem);
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
