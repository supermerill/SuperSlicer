///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// PluginUpdater discovers repository descriptions, downloads GitHub tags and
// places selected archives in the core cache. Installation is deliberately
// scheduled for the next launch because the current process may hold plugin
// DLLs open.

#include "PluginUpdater.hpp"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <sstream>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include "libslic3r/Semver.hpp"
#include "libslic3r/Utils.hpp"

#include "slic3r/Utils/Http.hpp"

namespace Slic3r {
namespace {

const char *const REPOSITORIES_DIRECTORY = "cache/plugins/repositories";
const char *const DESCRIPTION_FILENAME = "description.ini";
const char *const TAGS_FILENAME = "tags.json";

boost::filesystem::path repositories_directory();
boost::filesystem::path resource_descriptions_directory();
std::string repository_rest_url(const std::string &configured_url);
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

std::string repository_rest_url(const std::string &configured_url)
{
    if (configured_url.empty())
        return std::string();
    if (configured_url.find("://") != std::string::npos)
        return configured_url;
    return "https://api.github.com/repos/" + configured_url;
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
    if (m_sync_in_progress)
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
    if (m_sync_in_progress.exchange(true)) {
        callback_result(static_cast<int>(count_updates()));
        return;
    }
    {
        std::lock_guard<std::mutex> callback_guard(m_callback_mutex);
        m_callback_result = std::move(callback_result);
    }
    m_plugins_sync = static_cast<int>(m_plugins.size());
    if (m_plugins.empty()) {
        m_sync_in_progress = false;
        callback_result(0);
        return;
    }
    for (auto &[id, plugin] : m_plugins)
        update_plugin(plugin, force);
}

void PluginUpdater::update_plugin(PluginSync &plugin, bool force)
{
    if (plugin.description.config_update_rest.empty()) {
        plugin.sync_failed = true;
        end_updating();
        return;
    }
    const boost::filesystem::path cache_path = repositories_directory() / plugin.description.id / TAGS_FILENAME;
    if (boost::filesystem::exists(cache_path) && !force &&
        boost::filesystem::last_write_time(cache_path) + 24 * 3600 > std::time(nullptr)) {
        boost::nowide::ifstream stream(cache_path.string());
        const std::string contents((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        std::string error_message;
        plugin.sync_failed = !plugin.parse_tags(contents, error_message);
        if (plugin.sync_failed)
            BOOST_LOG_TRIVIAL(warning) << error_message;
        end_updating();
        return;
    }
    const std::string rest_url = repository_rest_url(plugin.description.config_update_rest);
    const std::string url = rest_url + "/tags?per_page=100;page=1";
    if (!has_api_request_slot(url)) {
        plugin.sync_failed = true;
        end_updating();
        return;
    }
    plugin.sync_in_progress = true;
    boost::filesystem::create_directories(cache_path.parent_path());
    Http::get(url)
        .size_limit(1024 * 64)
        .on_error([this, &plugin](std::string, std::string error, unsigned) {
            BOOST_LOG_TRIVIAL(warning) << "Cannot update plugin repository '" << plugin.description.id << "': " << error;
            plugin.sync_failed = true;
            plugin.sync_in_progress = false;
            end_updating();
        })
        .on_complete([this, cache_path, &plugin](std::string contents, unsigned) {
            boost::nowide::ofstream stream(cache_path.string(), std::ios::out | std::ios::trunc);
            stream << contents;
            std::string error_message;
            plugin.sync_failed = !plugin.parse_tags(contents, error_message);
            if (plugin.sync_failed)
                BOOST_LOG_TRIVIAL(warning) << error_message;
            plugin.sync_in_progress = false;
            end_updating();
        })
        .perform();
}

void PluginUpdater::end_updating()
{
    if (--m_plugins_sync != 0)
        return;
    m_sync_in_progress = false;
    std::function<void(int)> callback;
    {
        std::lock_guard<std::mutex> guard(m_callback_mutex);
        callback = m_callback_result;
        m_callback_result = [](int) {};
    }
    callback(static_cast<int>(count_updates()));
}

bool PluginUpdater::has_api_request_slot(const std::string &url)
{
    if (url.find("api.github.com") == std::string::npos)
        return true;
    if (m_next_time_slot + 3600 < std::time(nullptr)) {
        m_next_time_slot = std::time(nullptr);
        m_max_request = 25;
    }
    return --m_max_request > 0;
}

void PluginUpdater::download_new_repo(const std::string &rest_url, std::function<void(bool)> callback_result)
{
    const std::string normalized_rest_url = repository_rest_url(rest_url);
    const size_t marker = normalized_rest_url.find("https://api.github.com/repos/");
    const std::string description_url = marker != std::string::npos ?
        "https://raw.githubusercontent.com/" + normalized_rest_url.substr(marker + strlen("https://api.github.com/repos/")) +
            "/refs/heads/main/description.ini" : normalized_rest_url + "/description";
    Http::get(description_url)
        .size_limit(1024 * 64)
        .on_error([callback_result](std::string, std::string, unsigned) { callback_result(false); })
        .on_complete([callback_result](std::string contents, unsigned) {
            RepositoryDescription description;
            std::string error_message;
            if (!parse_repository_description(contents, RepositoryPackageType::Plugin, description, error_message)) {
                BOOST_LOG_TRIVIAL(warning) << error_message;
                callback_result(false);
                return;
            }
            try {
                save_plugin_description(description, contents);
                callback_result(true);
            } catch (const boost::filesystem::filesystem_error &error) {
                BOOST_LOG_TRIVIAL(warning) << error.what();
                callback_result(false);
            }
        })
        .perform();
}

void PluginUpdater::install_plugin(const std::string &plugin_id,
                                   const PluginAvailable &version,
                                   std::function<void(const std::string &)> callback_result)
{
    PluginSync *plugin = get_plugin(plugin_id);
    if (plugin == nullptr || version.url_zip.empty()) {
        callback_result("The selected plugin version has no downloadable archive.");
        return;
    }
    if (!has_api_request_slot(version.url_zip)) {
        callback_result("Too many requests to GitHub. Please try again later.");
        return;
    }
    const boost::filesystem::path archive_path = repositories_directory() / plugin_id /
        (version.package_version + "=" + version.slicer_version + ".zip");
    boost::filesystem::create_directories(archive_path.parent_path());
    Http::get(version.url_zip)
        .size_limit(130 * 1024 * 1024)
        .on_error([callback_result](std::string, std::string error, unsigned) { callback_result(error); })
        .on_complete([plugin_id, version, archive_path, callback_result](std::string contents, unsigned) {
            boost::nowide::ofstream stream(archive_path.string(), std::ios::out | std::ios::binary | std::ios::trunc);
            stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
            stream.close();
            std::string error_message;
            if (!cache_plugin_package_archive(boost::filesystem::path(data_dir()), archive_path, plugin_id,
                                              version.package_version, version.slicer_version, error_message) ||
                !request_plugin_install(plugin_id, version.package_version, version.slicer_version, error_message)) {
                callback_result(error_message);
                return;
            }
            callback_result(std::string());
        })
        .perform();
}

void PluginUpdater::clear_cache_plugin(const std::string &plugin_id, std::function<void(bool)> callback_result)
{
    try {
        const boost::filesystem::path cache_directory = boost::filesystem::path(data_dir()) / "cache/plugins";
        if (boost::filesystem::is_directory(cache_directory))
            for (boost::filesystem::directory_iterator it(cache_directory), end; it != end; ++it)
                if (it->path().filename().string().find(plugin_id + "_") == 0)
                    boost::filesystem::remove_all(it->path());
        callback_result(true);
    } catch (const boost::filesystem::filesystem_error &error) {
        BOOST_LOG_TRIVIAL(warning) << error.what();
        callback_result(false);
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
