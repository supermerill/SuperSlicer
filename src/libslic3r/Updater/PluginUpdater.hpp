///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// This updater is the plugin counterpart to PresetUpdater. It reuses the
// repository description, tag and archive protocol from libslic3r. Its
// callbacks expose transport-neutral results so GUI code can localize them.

#ifndef slic3r_Updater_PluginUpdater_hpp_
#define slic3r_Updater_PluginUpdater_hpp_

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "libslic3r/Plugins/PluginRepository.hpp"
#include "libslic3r/Updater/RepositoryUpdater.hpp"
#include "libslic3r/Updater/UpdaterError.hpp"

namespace Slic3r {

struct PluginAvailable : public RepositoryPackageVersion {
    // Non-empty when this exact package version is already validated in the
    // local repository cache. The updater can schedule it without HTTP.
    std::string local_directory;
    std::string notes;
};

struct PluginSync {
    RepositoryDescription description;
    PluginInstalledVersion installed_version;
    bool is_installed = false;
    bool has_cache = false;
    bool sync_in_progress = false;
    bool sync_failed = false;
    bool can_upgrade = false;
    std::vector<PluginAvailable> available_packages;
    PluginAvailable *best = nullptr;

    bool parse_tags(const std::string &json, std::string &error_message);
    void sort_available();
};

class PluginUpdater : public RepositoryUpdater {
public:
    PluginUpdater() = default;

    // Uses an injected transport instead of the process HTTP backend. Pending
    // requests retain their callbacks, so the transport must outlive them.
    explicit PluginUpdater(UpdaterHttpTransport &http_transport) : RepositoryUpdater(http_transport) {}
    PluginUpdater(const PluginUpdater &) = delete;
    PluginUpdater(PluginUpdater &&) = delete;
    PluginUpdater &operator=(const PluginUpdater &) = delete;
    PluginUpdater &operator=(PluginUpdater &&) = delete;

    // Reload local descriptions and selected package versions. This does not
    // contact the network; sync_async() performs that work later.
    void reload_all_plugins();
    void sync_async(std::function<void(int)> callback_result, bool force = false);

    // Load the notes for every known package of one plugin. Notes cached less
    // than 24 hours ago are reused unless force is true. The callback reports
    // whether every version succeeded; partial results remain available.
    void download_changelogs(const std::string &plugin_id,
                             std::function<void(bool)> callback_result,
                             bool force = false);
    void download_new_repo(const std::string &rest_url, std::function<void(UpdaterError)> callback_result);

    // Import an unpacked package directory. Generic description.ini and
    // version.ini files are generated in the cache when their information can
    // be derived from the folder, payload metadata or local defaults.
    UpdaterError cache_plugin_directory(const boost::filesystem::path &package_directory);
    void install_plugin(const std::string &plugin_id,
                        const PluginAvailable &version,
                        std::function<void(UpdaterError)> callback_result);
    // Record a package removal for the next startup without unloading the DLL
    // currently used by this process.
    void uninstall_plugin(const std::string &plugin_id, std::function<void(UpdaterError)> callback_result);
    void clear_cache_plugin(const std::string &plugin_id, std::function<void(UpdaterError)> callback_result);

    size_t count_available() const;
    size_t count_updates() const;
    std::vector<std::string> plugin_ids() const;
    PluginSync *get_plugin(const std::string &id);

private:
    void update_plugin(PluginSync &plugin, bool force);
    UpdaterError schedule_cached_plugin_install(const std::string &plugin_id, const PluginAvailable &version);
    int update_count() override;

    std::recursive_mutex m_plugins_mutex;
    std::map<std::string, PluginSync> m_plugins;
};

} // namespace Slic3r

#endif // slic3r_Updater_PluginUpdater_hpp_
