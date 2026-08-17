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
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include "libslic3r/Semver.hpp"
#include "libslic3r/Updater/RepositoryPackageCache.hpp"
#include "libslic3r/Utils.hpp"

#include "libslic3r/Updater/UpdaterHttp.hpp"

namespace Slic3r {
namespace {

const char *const DESCRIPTION_FILENAME = "description.ini";

bool read_plugin_description(const boost::filesystem::path &path,
                             RepositoryDescription &description,
                             std::string &error_message);
bool read_live_plugin_version(const boost::filesystem::path &package_root,
                              std::optional<PluginInstalledVersion> &version,
                              std::string &error_message);
// Keep reports shown by the package manager limited to failures caused while
// loading package contents. Missing configured plugin ids belong to the
// activation dialog, which can reason about them using activated.ini.
std::optional<PluginPackageLoadReport> package_manager_load_report(
    const PluginPackageLoadReport &report);
// Build the package-level diagnostic used when activated.ini selects an
// installed package whose live directory is no longer present.
PluginPackageLoadReport missing_live_package_report(
    const std::string &package_id, const boost::filesystem::path &package_root);

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

bool read_live_plugin_version(const boost::filesystem::path &package_root,
                              std::optional<PluginInstalledVersion> &version,
                              std::string &error_message)
{
    version.reset();
    if (!boost::filesystem::exists(package_root))
        return true;
    if (!boost::filesystem::is_directory(package_root)) {
        error_message = "The live plugin package path is not a directory.";
        return false;
    }

    try {
        boost::property_tree::ptree tree;
        boost::property_tree::read_ini((package_root / "version.ini").string(), tree);
        const boost::property_tree::ptree &plugin = tree.get_child("plugin");
        PluginInstalledVersion parsed;
        parsed.package_version = plugin.get<std::string>("package_version", std::string());
        parsed.slicer_version = plugin.get<std::string>("slicer_version", std::string());
        if (!Semver::parse(parsed.package_version) || !Semver::parse(parsed.slicer_version)) {
            error_message = "The live plugin version.ini contains an invalid package or slicer version.";
            return false;
        }
        version = std::move(parsed);
        return true;
    } catch (const std::exception &error) {
        error_message = "Cannot read the live plugin version.ini: " + std::string(error.what());
        return false;
    }
}

std::optional<PluginPackageLoadReport> package_manager_load_report(
    const PluginPackageLoadReport &report)
{
    PluginPackageLoadReport filtered = report;
    filtered.issues.erase(
        std::remove_if(filtered.issues.begin(), filtered.issues.end(),
            [](const PluginPackageLoadIssue &issue) {
                return issue.code == PluginPackageLoadErrorCode::ConfiguredPluginMissing ||
                       (issue.code == PluginPackageLoadErrorCode::PackageMissing && !issue.plugin_id.empty());
            }),
        filtered.issues.end());
    if (filtered.issues.empty())
        return std::nullopt;

    filtered.state = filtered.registered_plugin_ids.empty() ?
        PluginPackageLoadState::Failed : PluginPackageLoadState::LoadedWithErrors;
    return filtered;
}

PluginPackageLoadReport missing_live_package_report(
    const std::string &package_id, const boost::filesystem::path &package_root)
{
    PluginPackageLoadReport report;
    report.package_id = package_id;
    report.package_path = package_root.string();
    report.state = PluginPackageLoadState::Failed;
    PluginPackageLoadIssue issue;
    issue.code = PluginPackageLoadErrorCode::PackageMissing;
    issue.detail = "The package is selected as installed, but its live plugin directory is missing.";
    report.issues.emplace_back(std::move(issue));
    return report;
}

} // namespace

PluginUpdater::~PluginUpdater()
{
    // Queued operations and HTTP callback chains capture the derived model, so
    // they must finish before m_plugins begins destruction.
    shutdown_operation_executor();
}

UpdaterError PluginSync::parse_tags(const std::string &json)
{
    std::vector<RepositoryPackageVersion> parsed;
    std::string error_message;
    if (!parse_repository_versions(json, parsed, error_message))
        return make_updater_error(UpdaterError::Code::InvalidRepositoryMetadata, std::move(error_message));

    // Start the replacement model with versions that physically exist in the
    // cache. Clear their repository fields so a removed tag cannot leave a
    // stale download or changelog link on an otherwise valid local package.
    std::vector<PluginAvailable> refreshed;
    refreshed.reserve(available_packages.size() + parsed.size());
    for (const PluginAvailable &available : available_packages) {
        if (available.local_directory.empty())
            continue;
        PluginAvailable local = available;
        local.url_zip.clear();
        local.commit_sha.clear();
        local.commit_url.clear();
        refreshed.emplace_back(std::move(local));
    }

    // The parsed response is the complete remote projection. Matching local
    // entries keep their cache path and notes, while every remote-only entry is
    // recreated so tags omitted by this response disappear from the model.
    for (const RepositoryPackageVersion &version : parsed) {
        const std::vector<PluginAvailable>::iterator existing = std::find_if(
            refreshed.begin(), refreshed.end(),
            [&version](const PluginAvailable &candidate) { return candidate.tag == version.tag; });
        if (existing == refreshed.end()) {
            PluginAvailable available;
            static_cast<RepositoryPackageVersion &>(available) = version;
            const std::vector<PluginAvailable>::const_iterator previous = std::find_if(
                available_packages.begin(), available_packages.end(),
                [&version](const PluginAvailable &candidate) { return candidate.tag == version.tag; });
            if (previous != available_packages.end())
                available.notes = previous->notes;
            refreshed.emplace_back(std::move(available));
        } else {
            static_cast<RepositoryPackageVersion &>(*existing) = version;
        }
    }

    available_packages.swap(refreshed);
    sort_available();
    return UpdaterError();
}

void PluginSync::sort_available()
{
    std::sort(available_packages.begin(), available_packages.end(), [](const PluginAvailable &lhs, const PluginAvailable &rhs) {
        const std::optional<Semver> lhs_slicer = Semver::parse(lhs.slicer_version);
        const std::optional<Semver> rhs_slicer = Semver::parse(rhs.slicer_version);
        if (*lhs_slicer != *rhs_slicer)
            return *lhs_slicer > *rhs_slicer;
        return *Semver::parse(lhs.package_version) > *Semver::parse(rhs.package_version);
    });

    const PluginAvailable *best = best_available();
    if (best != nullptr && is_installed) {
        const std::optional<Semver> installed = Semver::parse(installed_version.package_version);
        const std::optional<Semver> available = Semver::parse(best->package_version);
        can_upgrade = installed && available && *available > *installed;
    } else {
        can_upgrade = false;
    }
}

const PluginAvailable *PluginSync::best_available() const
{
    const std::optional<Semver> current_slicer_version = Semver::parse(SLIC3R_VERSION_FULL);
    if (!current_slicer_version)
        return nullptr;

    for (const PluginAvailable &version : available_packages) {
        const std::optional<Semver> slicer_version = Semver::parse(version.slicer_version);
        if (slicer_version && *slicer_version <= *current_slicer_version)
            return &version;
    }
    return nullptr;
}

void PluginUpdater::reload_all_plugins()
{
    // Avoid replacing the model while a repository operation is publishing
    // results. The new model is otherwise built off-lock and swapped in as one
    // coherent snapshot after all filesystem reads have completed.
    if (sync_in_progress() || changelog_download_in_progress())
        return;

    std::map<std::string, PluginSync> plugins;

    const boost::filesystem::path configuration_directory(data_dir());
    std::string error_message;
    if (!prepare_plugin_cache(configuration_directory, error_message))
        BOOST_LOG_TRIVIAL(warning) << error_message;

    RepositoryPackageCache cache(configuration_directory, plugin_repository_cache_adapter());
    for (const RepositoryCachedEntry &repository : cache.scan()) {
        PluginSync &plugin = plugins[repository.description.id];
        plugin.description = repository.description;
        plugin.has_cache = true;
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
        const boost::filesystem::path package_root = boost::filesystem::path(data_dir()) / "plugins" / id;
        RepositoryDescription description;
        const std::map<std::string, PluginSync>::iterator existing = plugins.find(id);
        if (existing == plugins.end())
            read_plugin_description(package_root / DESCRIPTION_FILENAME, description, error_message);

        PluginSync &plugin = plugins[id];
        if (plugin.description.id.empty() && !description.id.empty())
            plugin.description = std::move(description);
        if (plugin.description.id.empty()) {
            plugin.description.type = RepositoryPackageType::Plugin;
            plugin.description.id = id;
            plugin.description.name = id;
            plugin.description.full_name = id;
        }
        plugin.is_installed = true;
        plugin.installed_version = version;
        // The installed section is the package manager's source of truth. A
        // missing live directory is therefore a package failure even when no
        // activated plugin id currently refers to this package.
        if (!boost::filesystem::is_directory(package_root))
            plugin.load_report = missing_live_package_report(id, package_root);
        plugin.has_cache = plugin.has_cache || boost::filesystem::is_directory(repository_package_cache_path(
            boost::filesystem::path(data_dir()), RepositoryPackageType::Plugin, id,
            version.package_version, version.slicer_version));
        plugin.sort_available();
    }

    // A failed or missing live package may have no cache descriptor to create
    // its row. The startup report still has a stable package id, so expose a
    // minimal model entry and let Plugin updates present the repair controls.
    for (const auto &[package_id, report] : Orchestrator::instance().plugin_package_load_reports()) {
        std::optional<PluginPackageLoadReport> filtered_report = package_manager_load_report(report);
        if (!filtered_report.has_value())
            continue;
        PluginSync &plugin = plugins[package_id];
        if (plugin.description.id.empty()) {
            plugin.description.type = RepositoryPackageType::Plugin;
            plugin.description.id = package_id;
            plugin.description.name = package_id;
            plugin.description.full_name = package_id;
        }
        plugin.load_report = std::move(filtered_report);
    }

    std::lock_guard<std::mutex> guard(m_model_mutex);
    m_plugins.swap(plugins);
}

void PluginUpdater::sync_async(std::function<void(int)> callback_result, bool force)
{
    std::vector<std::string> plugin_ids;
    {
        std::lock_guard<std::mutex> guard(m_model_mutex);
        plugin_ids.reserve(m_plugins.size());
        for (const auto &[plugin_id, plugin] : m_plugins)
            plugin_ids.emplace_back(plugin_id);
    }

    if (!begin_sync(plugin_ids.size(), std::move(callback_result)))
        return;
    for (const std::string &plugin_id : plugin_ids)
        update_plugin(plugin_id, force);
}

void PluginUpdater::update_plugin(const std::string &plugin_id, bool force)
{
    std::string rest_url;
    {
        std::lock_guard<std::mutex> guard(m_model_mutex);
        PluginSync *plugin = find_plugin_unlocked(plugin_id);
        if (plugin == nullptr) {
            finish_sync();
            return;
        }
        plugin->sync_state = RepositorySyncState::InProgress;
        plugin->sync_error = UpdaterError();
        rest_url = plugin->description.config_update_rest;
    }

    const RepositoryPackageCache cache(boost::filesystem::path(data_dir()),
                                       plugin_repository_cache_adapter());
    const boost::filesystem::path cache_path = cache.repository_tags_path(plugin_id);
    refresh_repository_tags(
        plugin_id, rest_url, cache_path, force,
        [this, plugin_id](const std::string &contents) {
            std::lock_guard<std::mutex> guard(m_model_mutex);
            PluginSync *plugin = find_plugin_unlocked(plugin_id);
            return plugin == nullptr ?
                make_updater_error(UpdaterError::Code::RepositoryNotFound) : plugin->parse_tags(contents);
        },
        [this, plugin_id](UpdaterError error) {
            std::lock_guard<std::mutex> guard(m_model_mutex);
            PluginSync *plugin = find_plugin_unlocked(plugin_id);
            if (plugin == nullptr)
                return;
            plugin->sync_state = error.succeeded() ? RepositorySyncState::Succeeded : RepositorySyncState::Failed;
            plugin->sync_error = std::move(error);
        });
}

void PluginUpdater::download_changelogs(const std::string &plugin_id,
                                        std::function<void(bool)> callback_result,
                                        bool force)
{
    std::vector<RepositoryChangelogVersion> versions;
    std::string rest_url;
    bool found = false;
    {
        std::lock_guard<std::mutex> guard(m_model_mutex);
        PluginSync *plugin = find_plugin_unlocked(plugin_id);
        if (plugin != nullptr) {
            found = true;
            rest_url = plugin->description.config_update_rest;
            versions.reserve(plugin->available_packages.size());
            for (const PluginAvailable &version : plugin->available_packages) {
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
                common_version.store_notes = [this, plugin_id, tag = version.tag](std::string notes) {
                    std::lock_guard<std::mutex> notes_guard(m_model_mutex);
                    PluginSync *current = find_plugin_unlocked(plugin_id);
                    if (current == nullptr)
                        return;
                    const std::vector<PluginAvailable>::iterator matching = std::find_if(
                        current->available_packages.begin(), current->available_packages.end(),
                        [&tag](const PluginAvailable &candidate) { return candidate.tag == tag; });
                    if (matching != current->available_packages.end())
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

    const RepositoryPackageCache cache(boost::filesystem::path(data_dir()),
                                       plugin_repository_cache_adapter());
    const boost::filesystem::path log_directory = cache.repository_logs_directory(plugin_id);
    download_repository_version_changelogs(std::move(versions), log_directory,
                                           rest_url,
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
            if (!prepare_plugin_cache(boost::filesystem::path(data_dir()), error_message) ||
                !cache.save_repository_description(description, error_message))
                return make_updater_error(UpdaterError::Code::Filesystem, std::move(error_message));
            return UpdaterError();
        },
        callback_result);
}

void PluginUpdater::cache_plugin_directory(const boost::filesystem::path &package_directory,
                                           std::function<void(UpdaterError)> callback_result)
{
    const bool accepted = enqueue_operation(
        [this, package_directory] {
            UpdaterError result = cache_plugin_directory_files(package_directory);
            if (result.succeeded())
                reload_all_plugins();
            return result;
        },
        callback_result);
    if (!accepted && callback_result)
        callback_result(make_updater_error(
            UpdaterError::Code::PreparationRejected,
            "The plugin updater is shutting down."));
}

UpdaterError PluginUpdater::cache_plugin_directory_files(const boost::filesystem::path &package_directory)
{
    if (!boost::filesystem::is_directory(package_directory))
        return make_updater_error(UpdaterError::Code::ArchiveUnavailable,
                                  "The selected plugin package directory does not exist.");

    RepositoryPackageCache cache(boost::filesystem::path(data_dir()), plugin_repository_cache_adapter());
    std::string error_message;
    RepositoryCachedVersion cached;
    if (!prepare_plugin_cache(boost::filesystem::path(data_dir()), error_message))
        return make_updater_error(UpdaterError::Code::Filesystem, std::move(error_message));
    if (!cache.cache_simple(package_directory, cached, error_message))
        return make_updater_error(UpdaterError::Code::InvalidArchive, std::move(error_message));
    return UpdaterError();
}

void PluginUpdater::install_plugin(const std::string &plugin_id,
                                   const PluginAvailable &version,
                                   std::function<void(UpdaterError)> callback_result)
{
    if (!plugin(plugin_id).has_value()) {
        callback_result(make_updater_error(UpdaterError::Code::ArchiveUnavailable));
        return;
    }

    // A previously downloaded package can be scheduled immediately. Validate
    // it before skipping HTTP so a corrupt cache is repaired by a fresh ZIP.
    std::string cache_error_message;
    const PluginInstalledVersion cached_version{version.package_version, version.slicer_version};
    if (plugin_package_cache_is_valid(boost::filesystem::path(data_dir()), plugin_id,
                                      cached_version, cache_error_message)) {
        schedule_cached_plugin_install_async(plugin_id, version, std::move(callback_result));
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

            // Extraction, cache publication and activation-file writes share
            // the updater worker with imports and cache removal. The HTTP
            // callback only hands over the completed transfer artifact.
            const bool accepted = enqueue_operation(
                [this, plugin_id, version, archive_path] {
                    UpdaterError result;
                    try {
                        std::string error_message;
                        if (!cache_plugin_package_archive(
                                boost::filesystem::path(data_dir()), archive_path, plugin_id,
                                version.package_version, version.slicer_version, error_message)) {
                            result = make_updater_error(
                                UpdaterError::Code::Cache, std::move(error_message));
                        } else {
                            result = schedule_cached_plugin_install(plugin_id, version);
                        }
                    } catch (...) {
                        result = make_updater_error_from_exception(std::current_exception());
                    }

                    // The downloaded ZIP is never durable state. Remove it
                    // after success or failure without masking the main error.
                    boost::system::error_code cleanup_error;
                    boost::filesystem::remove(archive_path, cleanup_error);
                    return result;
                },
                callback_result);
            if (!accepted && callback_result) {
                boost::system::error_code cleanup_error;
                boost::filesystem::remove(archive_path, cleanup_error);
                callback_result(make_updater_error(
                    UpdaterError::Code::PreparationRejected,
                    "The plugin updater is shutting down."));
            }
        });
}

void PluginUpdater::schedule_cached_plugin_install_async(
    const std::string &plugin_id,
    const PluginAvailable &version,
    std::function<void(UpdaterError)> callback_result)
{
    const bool accepted = enqueue_operation(
        [this, plugin_id, version] { return schedule_cached_plugin_install(plugin_id, version); },
        callback_result);
    if (!accepted && callback_result)
        callback_result(make_updater_error(
            UpdaterError::Code::PreparationRejected,
            "The plugin updater is shutting down."));
}

UpdaterError PluginUpdater::schedule_cached_plugin_install(const std::string &plugin_id,
                                                            const PluginAvailable &version)
{
    std::string error_message;
    if (!request_plugin_install(plugin_id, version.package_version, version.slicer_version, error_message))
        return make_updater_error(UpdaterError::Code::Cache, std::move(error_message));

    // The activation config names the package loaded on the next startup. The
    // current model mirrors that selection so the dialog updates immediately.
    std::lock_guard<std::mutex> guard(m_model_mutex);
    PluginSync *scheduled = find_plugin_unlocked(plugin_id);
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
    const std::optional<PluginSync> plugin_snapshot = plugin(plugin_id);
    if (!plugin_snapshot || !plugin_snapshot->is_installed) {
        callback_result(make_updater_error(UpdaterError::Code::ArchiveUnavailable));
        return;
    }

    const bool accepted = enqueue_operation(
        [this, plugin_id] { return uninstall_plugin_files(plugin_id); },
        callback_result);
    if (!accepted && callback_result)
        callback_result(make_updater_error(
            UpdaterError::Code::PreparationRejected,
            "The plugin updater is shutting down."));
}

UpdaterError PluginUpdater::uninstall_plugin_files(const std::string &plugin_id)
{
    std::string error_message;
    if (!request_plugin_uninstall(plugin_id, error_message))
        return make_updater_error(UpdaterError::Code::Filesystem, std::move(error_message));

    // The DLL remains loaded until restart, but the updater presents the
    // package state requested for that restart just as it does for installs.
    {
        std::lock_guard<std::mutex> guard(m_model_mutex);
        PluginSync *scheduled = find_plugin_unlocked(plugin_id);
        if (scheduled != nullptr) {
            scheduled->is_installed = false;
            scheduled->installed_version = {};
            scheduled->can_upgrade = false;
        }
    }
    return UpdaterError();
}

void PluginUpdater::clear_cache_plugin(const std::string &plugin_id, std::function<void(UpdaterError)> callback_result)
{
    if (sync_in_progress() || changelog_download_in_progress()) {
        callback_result(make_updater_error(
            UpdaterError::Code::PreparationRejected,
            "Cannot clear a plugin cache while repository data is being updated."));
        return;
    }

    const bool accepted = enqueue_operation(
        [this, plugin_id] { return clear_cache_plugin_files(plugin_id); },
        callback_result);
    if (!accepted && callback_result)
        callback_result(make_updater_error(
            UpdaterError::Code::PreparationRejected,
            "The plugin updater is shutting down."));
}

UpdaterError PluginUpdater::clear_cache_plugin_files(const std::string &plugin_id)
{
    UpdaterError result;
    try {
        const boost::filesystem::path data_directory(data_dir());

        // Clearing an installed package's cache must not remove it from the
        // desired set. Preserve and recache its live version. A package already
        // removed from [installed] must stay scheduled for deletion instead.
        std::optional<PluginInstalledVersion> live_version;
        std::string error_message;
        if (!read_live_plugin_version(data_directory / "plugins" / plugin_id,
                                      live_version, error_message)) {
            return make_updater_error(UpdaterError::Code::Cache, std::move(error_message));
        }
        PluginActivationConfig config;
        bool from_user_config = false;
        if (!ensure_plugin_activation_config(data_directory, config, from_user_config, error_message)) {
            return make_updater_error(UpdaterError::Code::Filesystem, std::move(error_message));
        }
        const bool package_is_desired = config.installed.count(plugin_id) != 0;
        if (live_version && package_is_desired)
            config.installed[plugin_id] = *live_version;
        else
            config.installed.erase(plugin_id);
        if (!write_plugin_activation_config(plugin_activation_config_path(data_directory), config, error_message)) {
            return make_updater_error(UpdaterError::Code::Filesystem, std::move(error_message));
        }

        boost::filesystem::remove_all(repository_cache_root_path(
            data_directory, RepositoryPackageType::Plugin, plugin_id));

        // The activation model keeps the currently installed version selected
        // across restarts. Re-cache that live package after removing downloaded
        // versions so the retained selection remains self-contained.
        std::optional<RepositoryCachedVersion> live_cached_version;
        if (live_version && package_is_desired) {
            RepositoryPackageCache cache(data_directory, plugin_repository_cache_adapter());
            live_cached_version.emplace();
            if (!cache.cache_simple(data_directory / "plugins" / plugin_id,
                                    *live_cached_version, error_message)) {
                return make_updater_error(UpdaterError::Code::Cache, std::move(error_message));
            }
        }

        {
            std::lock_guard<std::mutex> guard(m_model_mutex);
            PluginSync *current = find_plugin_unlocked(plugin_id);
            if (!live_cached_version) {
                // The cache description was the only source for this
                // uninstalled plugin, so its model row disappears as well.
                m_plugins.erase(plugin_id);
            } else if (current != nullptr) {
                // Keep remote versions, discard removed local-only entries and
                // restore the validated live version as the selected package.
                current->has_cache = true;
                for (PluginAvailable &version : current->available_packages)
                    version.local_directory.clear();
                current->available_packages.erase(
                    std::remove_if(current->available_packages.begin(), current->available_packages.end(),
                        [](const PluginAvailable &version) { return version.url_zip.empty(); }),
                    current->available_packages.end());

                const RepositoryPackageVersion &cached_version = live_cached_version->version;
                const std::vector<PluginAvailable>::iterator matching = std::find_if(
                    current->available_packages.begin(), current->available_packages.end(),
                    [&cached_version](const PluginAvailable &version) {
                        return version.package_version == cached_version.package_version &&
                               version.slicer_version == cached_version.slicer_version;
                    });
                if (matching != current->available_packages.end()) {
                    matching->local_directory = live_cached_version->directory.string();
                } else {
                    PluginAvailable available;
                    available.package_version = cached_version.package_version;
                    available.slicer_version = cached_version.slicer_version;
                    available.tag = RepositoryPackageCache::version_directory_name(
                        available.package_version, available.slicer_version);
                    available.local_directory = live_cached_version->directory.string();
                    current->available_packages.emplace_back(std::move(available));
                }
                current->is_installed = live_version.has_value() && package_is_desired;
                current->installed_version = current->is_installed ? *live_version : PluginInstalledVersion();
                current->sort_available();
            }
        }
    } catch (const boost::filesystem::filesystem_error &error) {
        BOOST_LOG_TRIVIAL(warning) << error.what();
        result = make_updater_error(UpdaterError::Code::Filesystem, error.what());
    }
    return result;
}

size_t PluginUpdater::count_available() const
{
    std::lock_guard<std::mutex> guard(m_model_mutex);
    return m_plugins.size();
}

size_t PluginUpdater::count_updates() const
{
    std::lock_guard<std::mutex> guard(m_model_mutex);
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
    std::lock_guard<std::mutex> guard(m_model_mutex);
    std::vector<std::string> ids;
    ids.reserve(m_plugins.size());
    for (const auto &[id, plugin] : m_plugins)
        ids.emplace_back(id);
    return ids;
}

std::vector<PluginSync> PluginUpdater::plugins() const
{
    std::lock_guard<std::mutex> guard(m_model_mutex);
    std::vector<PluginSync> result;
    result.reserve(m_plugins.size());
    for (const auto &[id, plugin] : m_plugins)
        result.emplace_back(plugin);
    return result;
}

std::optional<PluginSync> PluginUpdater::plugin(const std::string &id) const
{
    std::lock_guard<std::mutex> guard(m_model_mutex);
    const PluginSync *found = find_plugin_unlocked(id);
    return found == nullptr ? std::nullopt : std::optional<PluginSync>(*found);
}

PluginSync *PluginUpdater::find_plugin_unlocked(const std::string &id)
{
    const std::map<std::string, PluginSync>::iterator it = m_plugins.find(id);
    return it == m_plugins.end() ? nullptr : &it->second;
}

const PluginSync *PluginUpdater::find_plugin_unlocked(const std::string &id) const
{
    const std::map<std::string, PluginSync>::const_iterator it = m_plugins.find(id);
    return it == m_plugins.end() ? nullptr : &it->second;
}

} // namespace Slic3r
