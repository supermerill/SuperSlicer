///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/ Copyright (c) Prusa Research 2018 - 2023 David Kocik @kocikdav, Lukas Matena @lukasmatena, Vojtech Bubnik @bubnikv, Tomas Meszaros @tamasmeszaros, Vojtech Kral @vojtechkral
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// PresetUpdater is the non-GUI vendor-repository engine. It discovers vendor
// packages, refreshes tags, maintains the cache and changes vendor files. A
// small host interface lets a GUI create snapshots and reload its live preset
// state without making console or server builds depend on wxWidgets.

#ifndef slic3r_Updater_PresetUpdater_hpp_
#define slic3r_Updater_PresetUpdater_hpp_

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/Updater/RepositoryUpdater.hpp"
#include "libslic3r/Updater/UpdaterError.hpp"

namespace Slic3r {

class PresetBundle;
class PresetUpdater;

enum class VendorChange {
    Install,
    Uninstall,
    ClearCache,
    InstallAll,
    UpgradeAll
};

struct VendorAvailable {
    Semver config_version;
    Semver slicer_version;
    std::string local_file;
    std::string url_zip;
    std::string commit_sha;
    std::string commit_url;
    std::string tag;
    std::string notes;
};

struct VendorSync {
    VendorProfile profile;
    bool is_installed = false;
    bool has_cache = false;
    bool is_synch = false;
    bool synch_in_progress = false;
    bool synch_failed = false;
    bool can_upgrade = false;
    std::vector<VendorAvailable> available_profiles;
    VendorAvailable *best = nullptr;

    bool parse_tags(const std::string &json);
    void sort_available();
    void reset(const VendorProfile &profile, bool installed, bool has_cache);
};

class PresetUpdaterHost {
public:
    virtual ~PresetUpdaterHost() = default;

    // Called synchronously before the core changes vendor files. Returning
    // false leaves the filesystem untouched, for example after a failed GUI
    // snapshot or when a user cancelled a confirmation.
    virtual bool prepare_vendor_change(VendorChange change, const std::vector<std::string> &vendor_ids) = 0;

    // Called after a successful filesystem operation. The host owns AppConfig,
    // preset reloads and any UI refresh; the core never accesses them directly.
    virtual void vendor_files_changed(PresetUpdater &updater,
                                      VendorChange change,
                                      const std::vector<std::string> &vendor_ids) = 0;
};

class PresetUpdater : public RepositoryUpdater {
public:
    explicit PresetUpdater(PresetUpdaterHost *host = nullptr);
    PresetUpdater(const PresetUpdater &) = delete;
    PresetUpdater(PresetUpdater &&) = delete;
    PresetUpdater &operator=(const PresetUpdater &) = delete;
    PresetUpdater &operator=(PresetUpdater &&) = delete;

    void set_installed_vendors(const PresetBundle *preset_bundle);
    void reload_all_vendors();
    void sync_async(std::function<void(int)> callback_result, bool force = false);
    void download_logs(const std::string &vendor_id, std::function<void(bool)> callback_result, bool force = false);
    void download_new_repo(const std::string &github_org_repo, std::function<void(UpdaterError)> callback_result);

    void uninstall_vendor(const std::string &vendor_id, std::function<void(UpdaterError)> callback_result);
    void install_vendor(const std::string &vendor_id,
                        const VendorAvailable &version,
                        std::function<void(UpdaterError)> callback_result);
    void clear_cache_vendor(const std::string &vendor_id, std::function<void(UpdaterError)> callback_result);
    void uninstall_all_vendors(std::function<void(UpdaterError)> callback_result);
    void install_all_vendors(std::function<void(UpdaterErrors)> callback_result);
    void upgrade_all_installed_vendors(std::function<void(UpdaterErrors)> callback_result);

    int get_profile_count_to_update() const;
    size_t count_available() const;
    size_t count_installed() const;
    bool is_synchronized() const;
    std::vector<VendorSync> vendors() const;
    VendorSync *get_vendor(const std::string &id);
    const VendorSync *get_vendor(const std::string &id) const;

private:
    void load_unused_vendors(std::set<std::string> &vendors_id,
                             const boost::filesystem::path &vendor_dir,
                             bool is_installed);
    void update_vendor(VendorSync &vendor, bool force);
    int update_count() override;
    void on_sync_completed() override;

    UpdaterError install_vendor_files(VendorSync &vendor, const VendorAvailable &version);
    UpdaterError uninstall_vendor_files(VendorSync &vendor);
    UpdaterError clear_cache_vendor_files(VendorSync &vendor);
    bool prepare_vendor_change(VendorChange change, const std::vector<std::string> &vendor_ids);
    void notify_vendor_files_changed(VendorChange change, const std::vector<std::string> &vendor_ids);

    mutable std::recursive_mutex m_vendors_mutex;
    std::map<std::string, VendorSync> m_vendors;
    std::atomic_int m_pending_changelogs = 0;
    bool m_is_synchronized = false;
    PresetUpdaterHost *m_host = nullptr;
};

} // namespace Slic3r

#endif // slic3r_Updater_PresetUpdater_hpp_
