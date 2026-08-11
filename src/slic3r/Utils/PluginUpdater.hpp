///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// This updater is the plugin counterpart to PresetUpdater. It reuses the
// repository description, tag and archive protocol from libslic3r, but keeps
// HTTP and wx callbacks in slic3r so console and server code remain GUI-free.

#ifndef slic3r_PluginUpdater_hpp_
#define slic3r_PluginUpdater_hpp_

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "libslic3r/Plugins/PluginRepository.hpp"

#include "RepositoryUpdater.hpp"

namespace Slic3r {

struct PluginAvailable : public RepositoryPackageVersion {
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
    PluginUpdater(const PluginUpdater &) = delete;
    PluginUpdater(PluginUpdater &&) = delete;
    PluginUpdater &operator=(const PluginUpdater &) = delete;
    PluginUpdater &operator=(PluginUpdater &&) = delete;

    // Reload local descriptions and selected package versions. This does not
    // contact the network; sync_async() performs that work later.
    void reload_all_plugins();
    void sync_async(std::function<void(int)> callback_result, bool force = false);
    void download_new_repo(const std::string &rest_url, std::function<void(bool)> callback_result);
    void install_plugin(const std::string &plugin_id,
                        const PluginAvailable &version,
                        std::function<void(const std::string &)> callback_result);
    void clear_cache_plugin(const std::string &plugin_id, std::function<void(bool)> callback_result);

    size_t count_available() const;
    size_t count_updates() const;
    std::vector<std::string> plugin_ids() const;
    PluginSync *get_plugin(const std::string &id);

private:
    void update_plugin(PluginSync &plugin, bool force);
    int update_count() override;

    std::recursive_mutex m_plugins_mutex;
    std::map<std::string, PluginSync> m_plugins;
};

} // namespace Slic3r

#endif // slic3r_PluginUpdater_hpp_
