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
#include <memory>
#include <mutex>
#include <optional>
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
    // sync_error is populated only for Failed. Starting a new attempt clears
    // it before exposing InProgress to callers and dialog copies.
    RepositorySyncState sync_state = RepositorySyncState::Unchecked;
    UpdaterError sync_error;
    bool can_upgrade = false;
    std::vector<VendorAvailable> available_profiles;

    UpdaterError parse_tags(const std::string &json);
    void sort_available();
    void reset(const VendorProfile &profile, bool installed, bool has_cache);

    // Return the first slicer-compatible entry from the sorted version list.
    // The result belongs to this VendorSync, so copied snapshots never retain
    // a pointer into the updater's internal model.
    const VendorAvailable *best_available() const;
};

class PresetUpdaterHost {
public:
    virtual ~PresetUpdaterHost() = default;

    // Create the application backup used if publication fails. The returned
    // string is an opaque token understood only by the host. nullopt rejects
    // the operation and guarantees that the core leaves live vendor files
    // untouched.
    virtual std::optional<std::string> prepare_vendor_change(
        VendorChange change, const std::vector<std::string> &vendor_ids) = 0;

    // Restore the backup identified by prepare_vendor_change(). This is called
    // synchronously after a live vendor-file operation fails, before the
    // updater reports the error to its caller.
    virtual UpdaterError rollback_vendor_change(const std::string &token) = 0;

    // Run the live-file transaction on the thread that owns application
    // configuration and preset state. GUI hosts enqueue the operation on the
    // wx thread; non-GUI hosts may execute it immediately. The host must run
    // every accepted operation exactly once.
    virtual void dispatch_vendor_change(std::function<void()> operation) = 0;

    // Called after a successful filesystem operation. The host owns AppConfig,
    // preset reloads and any UI refresh; the core never accesses them directly.
    virtual void vendor_files_changed(PresetUpdater &updater,
                                      VendorChange change,
                                      const std::vector<std::string> &vendor_ids) = 0;
};

class PresetUpdater : public RepositoryUpdater {
public:
    explicit PresetUpdater(PresetUpdaterHost *host = nullptr);

    // Uses an injected transport for deterministic repository tests. The
    // caller owns both host and transport for the updater's full lifetime.
    PresetUpdater(PresetUpdaterHost *host, UpdaterHttpTransport &http_transport);
    PresetUpdater(const PresetUpdater &) = delete;
    PresetUpdater(PresetUpdater &&) = delete;
    PresetUpdater &operator=(const PresetUpdater &) = delete;
    PresetUpdater &operator=(PresetUpdater &&) = delete;

    void set_installed_vendors(const PresetBundle *preset_bundle);
    void reload_all_vendors();
    void sync_async(std::function<void(int)> callback_result, bool force = false);

    // Load version notes before opening the detailed vendor selector. A false
    // result means at least one note failed; other successfully loaded notes
    // remain usable by the dialog.
    void download_changelogs(const std::string &vendor_id,
                             std::function<void(bool)> callback_result,
                             bool force = false);
    void download_new_repo(const std::string &github_org_repo, std::function<void(UpdaterError)> callback_result);

    // Import a downloaded vendor ZIP into the versioned cache. The archive
    // must contain one profiles/<vendor-id>.ini file, either at its root or
    // below one wrapper directory. Call reload_all_vendors() after success to
    // expose the imported version through vendors().
    UpdaterError cache_vendor_archive(const boost::filesystem::path &archive_path);

    // Cache one standalone vendor INI and its optional sibling icon directory.
    // Reimporting the same version replaces that version atomically; other
    // versions of the vendor remain available to the selector.
    UpdaterError cache_vendor_ini(const boost::filesystem::path &profile_path);

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

    // Return detached model copies for dialogs and other readers. HTTP
    // callbacks may safely replace the updater's internal versions afterwards.
    std::vector<VendorSync> vendors() const;
    std::optional<VendorSync> vendor(const std::string &id) const;

private:
    // One entry owns the detached model snapshot and prepared cache source
    // needed to publish a single vendor inside an install transaction.
    struct PendingVendorInstall {
        std::string vendor_id;
        VendorAvailable version;
        VendorSync vendor;
        boost::filesystem::path source_directory;
    };

    void load_unused_vendors(std::map<std::string, VendorSync> &vendors,
                             bool &is_synchronized,
                             std::set<std::string> &vendors_id,
                             const boost::filesystem::path &vendor_dir,
                             bool is_installed);
    void update_vendor(const std::string &vendor_id, bool force);
    VendorSync *find_vendor_unlocked(const std::string &id);
    const VendorSync *find_vendor_unlocked(const std::string &id) const;
    int update_count() override;
    void on_sync_completed() override;

    // Resolve a local cache source immediately or download and validate its
    // archive asynchronously. The callback runs exactly once and no live
    // vendor file has changed when it is called.
    void prepare_vendor_install_source_async(
        const VendorSync &vendor,
        const VendorAvailable &version,
        std::function<void(UpdaterError, boost::filesystem::path)> callback_result);
    UpdaterError install_vendor_files(VendorSync &vendor, const boost::filesystem::path &source_directory);
    UpdaterError uninstall_vendor_files(VendorSync &vendor);
    UpdaterError clear_cache_vendor_files(VendorSync &vendor);
    // Prepare batch sources sequentially, then publish them under one host
    // snapshot on the thread selected by PresetUpdaterHost.
    void install_vendor_batch(VendorChange change,
                              const std::vector<std::pair<std::string, VendorAvailable>> &installs,
                              std::function<void(UpdaterErrors)> callback_result);
    void prepare_vendor_install_batch(
        VendorChange change,
        std::shared_ptr<std::vector<PendingVendorInstall>> installs,
        size_t install_idx,
        std::function<void(UpdaterErrors)> callback_result);
    void publish_vendor_install_batch(
        VendorChange change,
        std::shared_ptr<std::vector<PendingVendorInstall>> installs,
        std::function<void(UpdaterErrors)> callback_result);
    // Only one cache/live vendor mutation may own the snapshot transaction at
    // a time. Async operations retain this gate until their terminal callback.
    bool begin_vendor_change_operation();
    void finish_vendor_change_operation();

    // Non-GUI use executes inline; GUI use hands the transaction to wx without
    // exposing wxWidgets in libslic3r.
    void dispatch_vendor_change(std::function<void()> operation);
    std::optional<std::string> prepare_vendor_change(VendorChange change,
                                                     const std::vector<std::string> &vendor_ids);
    UpdaterError rollback_vendor_change(const std::string &token, UpdaterError operation_error);
    void notify_vendor_files_changed(VendorChange change, const std::vector<std::string> &vendor_ids);

    std::map<std::string, VendorSync> m_vendors;
    bool m_is_synchronized = false;
    PresetUpdaterHost *m_host = nullptr;
    std::atomic_bool m_vendor_change_in_progress = false;
};

} // namespace Slic3r

#endif // slic3r_Updater_PresetUpdater_hpp_
