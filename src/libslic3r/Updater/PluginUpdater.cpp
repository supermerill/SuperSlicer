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
#include "libslic3r/Utils.hpp"

#include "libslic3r/Updater/UpdaterHttp.hpp"

namespace Slic3r {
namespace {

const char *const REPOSITORIES_DIRECTORY = "cache/plugins/repositories";
const char *const DESCRIPTION_FILENAME = "description.ini";
const char *const TAGS_FILENAME = "tags.json";

boost::filesystem::path repositories_directory();
boost::filesystem::path resource_descriptions_directory();
bool read_plugin_description(const boost::filesystem::path &path,
                             RepositoryDescription &description,
                             std::string &error_message);
void load_plugin_descriptions(const boost::filesystem::path &directory,
                              std::map<std::string, PluginSync> &plugins);
void save_plugin_description(const RepositoryDescription &description, const std::string &contents);
boost::filesystem::path repositories_directory()
{
    return boost::filesystem::path(data_dir()) / REPOSITORIES_DIRECTORY;
}

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

void load_plugin_descriptions(const boost::filesystem::path &directory,
                              std::map<std::string, PluginSync> &plugins)
{
    if (!boost::filesystem::is_directory(directory))
        return;
    for (boost::filesystem::directory_iterator it(directory), end; it != end; ++it) {
        const boost::filesystem::path description_path = boost::filesystem::is_directory(it->path()) ?
            it->path() / DESCRIPTION_FILENAME : it->path();
        if (!boost::filesystem::is_regular_file(description_path) || description_path.extension() != ".ini")
            continue;
        RepositoryDescription description;
        std::string error_message;
        if (!read_plugin_description(description_path, description, error_message)) {
            BOOST_LOG_TRIVIAL(warning) << error_message;
            continue;
        }
        plugins[description.id].description = std::move(description);
    }
}

void save_plugin_description(const RepositoryDescription &description, const std::string &contents)
{
    const boost::filesystem::path destination = repositories_directory() / description.id / DESCRIPTION_FILENAME;
    boost::filesystem::create_directories(destination.parent_path());
    boost::nowide::ofstream stream(destination.string(), std::ios::out | std::ios::trunc);
    stream << contents;
}

} // namespace

bool PluginSync::parse_tags(const std::string &json, std::string &error_message)
{
    std::vector<RepositoryPackageVersion> parsed;
    if (!parse_repository_versions(json, parsed, error_message))
        return false;
    available_packages.clear();
    for (const RepositoryPackageVersion &version : parsed) {
        PluginAvailable available;
        static_cast<RepositoryPackageVersion &>(available) = version;
        available_packages.emplace_back(std::move(available));
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
    load_plugin_descriptions(resource_descriptions_directory(), m_plugins);
    load_plugin_descriptions(repositories_directory(), m_plugins);

    PluginActivationConfig config;
    std::string error_message;
    bool from_user_config = false;
    if (!ensure_plugin_activation_config(boost::filesystem::path(data_dir()), config, from_user_config, error_message)) {
        BOOST_LOG_TRIVIAL(warning) << error_message;
        return;
    }
    for (const auto &[id, version] : config.installed) {
        const boost::filesystem::path package_root = boost::filesystem::path(data_dir()) / "plugins" / id;
        RepositoryDescription description;
        if (read_plugin_description(package_root / DESCRIPTION_FILENAME, description, error_message))
            m_plugins[id].description = std::move(description);
        PluginSync &plugin = m_plugins[id];
        if (plugin.description.id.empty()) {
            plugin.description.type = RepositoryPackageType::Plugin;
            plugin.description.id = id;
            plugin.description.name = id;
            plugin.description.full_name = id;
        }
        plugin.is_installed = true;
        plugin.installed_version = version;
        plugin.has_cache = boost::filesystem::is_directory(repository_package_cache_path(
            boost::filesystem::path(data_dir()), RepositoryPackageType::Plugin, id,
            version.package_version, version.slicer_version));
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
    const boost::filesystem::path cache_path = repositories_directory() / plugin.description.id / TAGS_FILENAME;
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

    const boost::filesystem::path log_directory = repositories_directory() / plugin_id / "logs";
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
            try {
                save_plugin_description(description, contents);
                return UpdaterError();
            } catch (const boost::filesystem::filesystem_error &error) {
                BOOST_LOG_TRIVIAL(warning) << error.what();
                return make_updater_error(UpdaterError::Code::Filesystem, error.what());
            }
        },
        std::move(callback_result));
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

    const boost::filesystem::path archive_path = repositories_directory() / plugin_id /
        (version.package_version + "=" + version.slicer_version + ".zip");
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
        const boost::filesystem::path cache_directory = boost::filesystem::path(data_dir()) / "cache/plugins";
        if (boost::filesystem::is_directory(cache_directory))
            for (boost::filesystem::directory_iterator it(cache_directory), end; it != end; ++it)
                if (it->path().filename().string().find(plugin_id + "_") == 0)
                    boost::filesystem::remove_all(it->path());
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
