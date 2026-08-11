///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// PluginUpdater discovers repository descriptions, downloads GitHub tags and
// places selected archives in the core cache. Installation is deliberately
// scheduled for the next launch because the current process may hold plugin
// DLLs open.

#include "libslic3r/Updater/PluginUpdater.hpp"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <iterator>
#include <sstream>
#include <utility>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include "libslic3r/Semver.hpp"
#include "libslic3r/Updater/RepositoryPackageCache.hpp"
#include "libslic3r/Utils.hpp"

#include "libslic3r/Updater/UpdaterHttp.hpp"

namespace Slic3r {
namespace {

const char *const DESCRIPTION_FILENAME = "description.ini";

boost::filesystem::path resource_descriptions_directory();
bool read_plugin_description(const boost::filesystem::path &path,
                             RepositoryDescription &description,
                             std::string &error_message);

boost::filesystem::path resource_descriptions_directory()
{
    return boost::filesystem::path(resources_dir()) / "plugins/descriptions";
}

bool read_plugin_description(const boost::filesystem::path &path,
                             RepositoryDescription &description,
                             std::string &error_message)
{
    boost::nowide::ifstream stream(path.string());
    if (!stream) {
        error_message = "Cannot read plugin description '" + path.string() + "'.";
        return false;
    }
    const std::string contents((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return parse_repository_description(contents, RepositoryPackageType::Plugin, description, error_message);
}

} // namespace

bool PluginSync::parse_tags(const std::string &json, std::string &error_message)
{
    std::vector<RepositoryPackageVersion> parsed;
    if (!parse_repository_versions(json, parsed, error_message))
        return false;
    for (const RepositoryPackageVersion &version : parsed) {
        const std::vector<PluginAvailable>::iterator existing = std::find_if(
            available_packages.begin(), available_packages.end(),
            [&version](const PluginAvailable &candidate) { return candidate.tag == version.tag; });
        if (existing == available_packages.end()) {
            PluginAvailable available;
            static_cast<RepositoryPackageVersion &>(available) = version;
            available_packages.emplace_back(std::move(available));
        } else {
            // A local package already owns its cache path and notes. Refresh
            // only the network fields supplied by the repository tag.
            existing->url_zip = version.url_zip;
            existing->commit_sha = version.commit_sha;
            existing->commit_url = version.commit_url;
        }
    }
    sort_available();
    return true;
}

void PluginSync::sort_available()
{
    best = nullptr;
    const std::optional<Semver> current_slicer_version = Semver::parse(SLIC3R_VERSION_FULL);
    if (!current_slicer_version)
        return;
    std::sort(available_packages.begin(), available_packages.end(), [](const PluginAvailable &lhs, const PluginAvailable &rhs) {
        const std::optional<Semver> lhs_slicer = Semver::parse(lhs.slicer_version);
        const std::optional<Semver> rhs_slicer = Semver::parse(rhs.slicer_version);
        if (*lhs_slicer != *rhs_slicer)
            return *lhs_slicer > *rhs_slicer;
        return *Semver::parse(lhs.package_version) > *Semver::parse(rhs.package_version);
    });
    for (PluginAvailable &version : available_packages) {
        const std::optional<Semver> slicer_version = Semver::parse(version.slicer_version);
        if (*slicer_version <= *current_slicer_version) {
            best = &version;
            break;
        }
    }
    if (best != nullptr && is_installed) {
        const std::optional<Semver> installed = Semver::parse(installed_version.package_version);
        const std::optional<Semver> available = Semver::parse(best->package_version);
        can_upgrade = installed && available && *available > *installed;
    } else {
        can_upgrade = false;
    }
}

void PluginUpdater::reload_all_plugins()
{
    std::lock_guard<std::recursive_mutex> guard(m_plugins_mutex);
    // HTTP callbacks retain references to entries in m_plugins. Do not erase
    // them while a refresh is in flight; the next dialog refresh will reload
    // descriptions once the callbacks have completed.
    if (sync_in_progress() || changelog_download_in_progress())
        return;
    m_plugins.clear();

    const boost::filesystem::path configuration_directory(data_dir());
    std::string error_message;
    if (!prepare_plugin_bundle_cache(boost::filesystem::path(resources_dir()),
                                     configuration_directory, error_message))
        BOOST_LOG_TRIVIAL(warning) << error_message;

    RepositoryPackageCache cache(configuration_directory, plugin_repository_cache_adapter());
    const boost::filesystem::path descriptions = resource_descriptions_directory();
    if (boost::filesystem::is_directory(descriptions)) {
        for (boost::filesystem::directory_iterator it(descriptions), end; it != end; ++it) {
            if (!boost::filesystem::is_regular_file(it->path()) || it->path().extension() != ".ini")
                continue;
            RepositoryDescription description;
            if (!read_plugin_description(it->path(), description, error_message)) {
                BOOST_LOG_TRIVIAL(warning) << error_message;
                continue;
            }
            // A package manifest is the source for display metadata, while an
            // embedded repository descriptor may supply the URL omitted by a
            // local bundle. Preserve package metadata and fill only that
            // missing repository field.
            const boost::filesystem::path cached_description =
                cache.repository_description_path(description.id);
            RepositoryDescription merged = description;
            if (boost::filesystem::is_regular_file(cached_description)) {
                RepositoryDescription existing;
                if (!read_plugin_description(cached_description, existing, error_message)) {
                    BOOST_LOG_TRIVIAL(warning) << error_message;
                    continue;
                }
                merged = std::move(existing);
                if (merged.config_update_rest.empty())
                    merged.config_update_rest = description.config_update_rest;
            }
            if (!cache.save_repository_description(merged, error_message))
                BOOST_LOG_TRIVIAL(warning) << error_message;
        }
    }

    for (const RepositoryCachedEntry &repository : cache.scan()) {
        PluginSync &plugin = m_plugins[repository.description.id];
        plugin.description = repository.description;
        plugin.has_cache = !repository.versions.empty();
        for (const RepositoryCachedVersion &cached : repository.versions) {
            PluginAvailable available;
            available.package_version = cached.version.package_version;
            available.slicer_version = cached.version.slicer_version;
            available.tag = RepositoryPackageCache::version_directory_name(
                available.package_version, available.slicer_version);
            available.local_directory = cached.directory.string();
            plugin.available_packages.emplace_back(std::move(available));
        }
        plugin.sort_available();
    }

    PluginActivationConfig config;
    bool from_user_config = false;
    if (!ensure_plugin_activation_config(boost::filesystem::path(data_dir()), config, from_user_config, error_message)) {
        BOOST_LOG_TRIVIAL(warning) << error_message;
        return;
    }
    for (const auto &[id, version] : config.installed) {
        PluginSync &plugin = m_plugins[id];
        const boost::filesystem::path package_root = boost::filesystem::path(data_dir()) / "plugins" / id;
        RepositoryDescription description;
        if (plugin.description.id.empty() &&
            read_plugin_description(package_root / DESCRIPTION_FILENAME, description, error_message))
            plugin.description = std::move(description);
        if (plugin.description.id.empty()) {
            plugin.description.type = RepositoryPackageType::Plugin;
            plugin.description.id = id;
            plugin.description.name = id;
            plugin.description.full_name = id;
        }
        plugin.is_installed = true;
        plugin.installed_version = version;
        plugin.has_cache = plugin.has_cache || boost::filesystem::is_directory(repository_package_cache_path(
            boost::filesystem::path(data_dir()), RepositoryPackageType::Plugin, id,
            version.package_version, version.slicer_version));
        plugin.sort_available();
    }
}

void PluginUpdater::sync_async(std::function<void(int)> callback_result, bool force)
{
    std::lock_guard<std::recursive_mutex> guard(m_plugins_mutex);
    if (!begin_sync(m_plugins.size(), callback_result)) {
        callback_result(static_cast<int>(count_updates()));
        return;
    }
    if (m_plugins.empty())
        return;
    for (auto &[id, plugin] : m_plugins)
        update_plugin(plugin, force);
}

void PluginUpdater::update_plugin(PluginSync &plugin, bool force)
{
    const RepositoryPackageCache cache(boost::filesystem::path(data_dir()),
                                       plugin_repository_cache_adapter());
    const boost::filesystem::path cache_path = cache.repository_tags_path(plugin.description.id);
    plugin.sync_in_progress = true;
    refresh_repository_tags(
        plugin.description.id, plugin.description.config_update_rest, cache_path, force,
        [&plugin](const std::string &contents) {
            std::string error_message;
            const bool succeeded = plugin.parse_tags(contents, error_message);
            if (!succeeded)
                BOOST_LOG_TRIVIAL(warning) << error_message;
            return succeeded;
        },
        [&plugin](bool succeeded) {
            plugin.sync_failed = !succeeded;
            plugin.sync_in_progress = false;
        });
}

void PluginUpdater::download_changelogs(const std::string &plugin_id,
                                        std::function<void(bool)> callback_result,
                                        bool force)
{
    std::vector<RepositoryChangelogVersion> versions;
    std::lock_guard<std::recursive_mutex> guard(m_plugins_mutex);
    PluginSync *plugin = get_plugin(plugin_id);
    if (plugin == nullptr) {
        callback_result(false);
        return;
    }

    const RepositoryPackageCache cache(boost::filesystem::path(data_dir()),
                                       plugin_repository_cache_adapter());
    const boost::filesystem::path log_directory = cache.repository_logs_directory(plugin_id);
    for (PluginAvailable &version : plugin->available_packages) {
        const std::optional<Semver> package_version = Semver::parse(version.package_version);
        const std::optional<Semver> slicer_version = Semver::parse(version.slicer_version);
        if (!package_version || !slicer_version)
            continue;

        RepositoryChangelogVersion common_version;
        common_version.content_version = *package_version;
        common_version.slicer_version = *slicer_version;
        common_version.tag = version.tag;
        common_version.commit_sha = version.commit_sha;
        common_version.commit_url = version.commit_url;
        common_version.store_notes = [&version](std::string notes) { version.notes = std::move(notes); };
        versions.emplace_back(std::move(common_version));
    }
    download_repository_version_changelogs(std::move(versions), log_directory,
                                           plugin->description.config_update_rest,
                                           std::move(callback_result), force);
}

void PluginUpdater::download_new_repo(const std::string &rest_url, std::function<void(UpdaterError)> callback_result)
{
    download_repository_description(
        rest_url,
        [](const std::string &contents, const std::string &) {
            RepositoryDescription description;
            std::string error_message;
            if (!parse_repository_description(contents, RepositoryPackageType::Plugin, description, error_message)) {
                BOOST_LOG_TRIVIAL(warning) << error_message;
                return make_updater_error(UpdaterError::Code::InvalidArchive, std::move(error_message));
            }
            RepositoryPackageCache cache(boost::filesystem::path(data_dir()),
                                         plugin_repository_cache_adapter());
            if (!prepare_plugin_bundle_cache(boost::filesystem::path(resources_dir()),
                                             boost::filesystem::path(data_dir()), error_message) ||
                !cache.save_repository_description(description, error_message))
                return make_updater_error(UpdaterError::Code::Filesystem, std::move(error_message));
            return UpdaterError();
        },
        std::move(callback_result));
}

UpdaterError PluginUpdater::cache_plugin_directory(const boost::filesystem::path &package_directory)
{
    if (!boost::filesystem::is_directory(package_directory))
        return make_updater_error(UpdaterError::Code::ArchiveUnavailable,
                                  "The selected plugin package directory does not exist.");

    RepositoryPackageCache cache(boost::filesystem::path(data_dir()), plugin_repository_cache_adapter());
    std::string error_message;
    RepositoryCachedVersion cached;
    if (!prepare_plugin_bundle_cache(boost::filesystem::path(resources_dir()),
                                     boost::filesystem::path(data_dir()), error_message))
        return make_updater_error(UpdaterError::Code::Filesystem, std::move(error_message));
    if (!cache.cache_simple(package_directory, cached, error_message))
        return make_updater_error(UpdaterError::Code::InvalidArchive, std::move(error_message));
    return UpdaterError();
}

void PluginUpdater::install_plugin(const std::string &plugin_id,
                                   const PluginAvailable &version,
                                   std::function<void(UpdaterError)> callback_result)
{
    PluginSync *plugin = get_plugin(plugin_id);
    if (plugin == nullptr) {
        callback_result(make_updater_error(UpdaterError::Code::ArchiveUnavailable));
        return;
    }

    // A previously downloaded package can be scheduled immediately. Validate
    // it before skipping HTTP so a corrupt cache is repaired by a fresh ZIP.
    std::string cache_error_message;
    const PluginInstalledVersion cached_version{version.package_version, version.slicer_version};
    if (plugin_package_cache_is_valid(boost::filesystem::path(data_dir()), plugin_id,
                                      cached_version, cache_error_message)) {
        callback_result(schedule_cached_plugin_install(plugin_id, version));
        return;
    }

    const boost::filesystem::path archive_path = repository_cache_root_path(
        boost::filesystem::path(data_dir()), RepositoryPackageType::Plugin, plugin_id) /
        (RepositoryPackageCache::version_directory_name(
            version.package_version, version.slicer_version) + ".zip");
    download_repository_file_async(
        version.url_zip, archive_path, 130 * 1024 * 1024,
        [this, plugin_id, version, archive_path, callback_result](UpdaterError download_error) {
            if (!download_error.succeeded()) {
                callback_result(std::move(download_error));
                return;
            }
            std::string error_message;
            if (!cache_plugin_package_archive(boost::filesystem::path(data_dir()), archive_path, plugin_id,
                                              version.package_version, version.slicer_version, error_message)) {
                callback_result(make_updater_error(UpdaterError::Code::Cache, std::move(error_message)));
                return;
            }
            boost::system::error_code cleanup_error;
            boost::filesystem::remove(archive_path, cleanup_error);
            callback_result(schedule_cached_plugin_install(plugin_id, version));
        });
}

UpdaterError PluginUpdater::schedule_cached_plugin_install(const std::string &plugin_id,
                                                            const PluginAvailable &version)
{
    std::string error_message;
    if (!request_plugin_install(plugin_id, version.package_version, version.slicer_version, error_message))
        return make_updater_error(UpdaterError::Code::Cache, std::move(error_message));

    // The activation config names the package loaded on the next startup. The
    // current model mirrors that selection so the dialog updates immediately.
    std::lock_guard<std::recursive_mutex> guard(m_plugins_mutex);
    PluginSync *scheduled = get_plugin(plugin_id);
    if (scheduled != nullptr) {
        scheduled->is_installed = true;
        scheduled->installed_version = PluginInstalledVersion{
            version.package_version, version.slicer_version};
        scheduled->has_cache = true;
        scheduled->sort_available();
    }
    return UpdaterError();
}

void PluginUpdater::uninstall_plugin(const std::string &plugin_id,
                                     std::function<void(UpdaterError)> callback_result)
{
    PluginSync *plugin = get_plugin(plugin_id);
    if (plugin == nullptr || !plugin->is_installed) {
        callback_result(make_updater_error(UpdaterError::Code::ArchiveUnavailable));
        return;
    }

    std::string error_message;
    if (!request_plugin_uninstall(plugin_id, error_message)) {
        callback_result(make_updater_error(UpdaterError::Code::Filesystem, std::move(error_message)));
        return;
    }

    // The DLL remains loaded until restart, but the updater presents the
    // package state requested for that restart just as it does for installs.
    std::lock_guard<std::recursive_mutex> guard(m_plugins_mutex);
    PluginSync *scheduled = get_plugin(plugin_id);
    if (scheduled != nullptr) {
        scheduled->is_installed = false;
        scheduled->installed_version = {};
        scheduled->can_upgrade = false;
    }
    callback_result(UpdaterError());
}

void PluginUpdater::clear_cache_plugin(const std::string &plugin_id, std::function<void(UpdaterError)> callback_result)
{
    try {
        boost::filesystem::remove_all(repository_cache_root_path(
            boost::filesystem::path(data_dir()), RepositoryPackageType::Plugin, plugin_id));
        std::lock_guard<std::recursive_mutex> guard(m_plugins_mutex);
        PluginSync *plugin = get_plugin(plugin_id);
        if (plugin != nullptr) {
            plugin->has_cache = false;
            for (PluginAvailable &version : plugin->available_packages)
                version.local_directory.clear();
        }
        callback_result(UpdaterError());
    } catch (const boost::filesystem::filesystem_error &error) {
        BOOST_LOG_TRIVIAL(warning) << error.what();
        callback_result(make_updater_error(UpdaterError::Code::Filesystem, error.what()));
    }
}

size_t PluginUpdater::count_available() const
{
    return m_plugins.size();
}

size_t PluginUpdater::count_updates() const
{
    size_t count = 0;
    for (const auto &[id, plugin] : m_plugins)
        if (plugin.can_upgrade)
            ++count;
    return count;
}

int PluginUpdater::update_count()
{
    return static_cast<int>(count_updates());
}

std::vector<std::string> PluginUpdater::plugin_ids() const
{
    std::vector<std::string> ids;
    ids.reserve(m_plugins.size());
    for (const auto &[id, plugin] : m_plugins)
        ids.emplace_back(id);
    return ids;
}

PluginSync *PluginUpdater::get_plugin(const std::string &id)
{
    const std::map<std::string, PluginSync>::iterator it = m_plugins.find(id);
    return it == m_plugins.end() ? nullptr : &it->second;
}

} // namespace Slic3r
