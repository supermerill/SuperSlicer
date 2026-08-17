///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// This adapter connects the reusable vendor updater to the desktop
// application. The core manages repository files and HTTP; this class owns
// the GUI-only responsibilities: snapshots, AppConfig reconciliation, preset
// reloads and posting asynchronous callbacks back to the wx event loop.

#ifndef slic3r_Utils_PresetUpdater_hpp_
#define slic3r_Utils_PresetUpdater_hpp_

#ifndef USE_GTHUB_PRESET_UPDATE
#define USE_GTHUB_PRESET_UPDATE 1
#endif

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "libslic3r/Updater/PresetUpdater.hpp"

class wxWindow;
class wxString;

namespace Slic3r::GUI {

class GUI_App;

class PresetUpdater final : private Slic3r::PresetUpdaterHost {
public:
    explicit PresetUpdater(GUI_App &app);
    PresetUpdater(const PresetUpdater &) = delete;
    PresetUpdater(PresetUpdater &&) = delete;
    PresetUpdater &operator=(const PresetUpdater &) = delete;
    PresetUpdater &operator=(PresetUpdater &&) = delete;
    ~PresetUpdater() override;

    void set_installed_vendors(const Slic3r::PresetBundle *preset_bundle);
    void reload_all_vendors();
    void sync_async(std::function<void(int)> callback_result, bool force = false);
    void download_changelogs(const std::string &vendor_id,
                             std::function<void(bool)> callback_result,
                             bool force = false);
    // Download and validate a repository description, then return the complete
    // updater result on the GUI thread so callers can report the exact failure.
    void download_new_repo(const std::string &rest_url,
                           std::function<void(Slic3r::UpdaterError)> callback_result);

    // Cache a local vendor ZIP, reload the updater model after validation, and
    // report an empty error string on success. The callback runs on the GUI
    // thread so a dialog may rebuild its controls directly.
    void cache_vendor_archive(const boost::filesystem::path &archive_path,
                              std::function<void(const std::string &)> callback_result);

    // Cache a standalone vendor profile, reload the model, then invoke the
    // callback on the GUI thread with an empty string on success.
    void cache_vendor_ini(const boost::filesystem::path &profile_path,
                          std::function<void(const std::string &)> callback_result);

    // Vendor removals preserve the structured updater result so the caller can
    // distinguish snapshot, filesystem and rollback failures.
    void uninstall_vendor(const std::string &vendor_id,
                          std::function<void(Slic3r::UpdaterError)> callback_result);
    void install_vendor(const std::string &vendor_id,
                        const Slic3r::VendorAvailable &version,
                        std::function<void(const std::string &)> callback_result);
    void clear_cache_vendor(const std::string &vendor_id,
                            std::function<void(Slic3r::UpdaterError)> callback_result);
    void uninstall_all_vendors(std::function<void(Slic3r::UpdaterError)> callback_result);
    void install_all_vendors(std::function<void(const std::string &)> callback_result);
    void upgrade_all_installed_vendors(std::function<void(const std::string &)> callback_result);

    int get_profile_count_to_update() const;
    size_t count_available() const;
    size_t count_installed() const;
    bool is_synchronized() const;
    std::vector<Slic3r::VendorSync> vendors() const;
    std::optional<Slic3r::VendorSync> vendor(const std::string &id) const;

    // The dialog may be requested while a background refresh is pending. It
    // waits for that refresh when possible, then creates the modal window on
    // the wx thread instead of relying on updater-specific wx events.
    void show_synch_window(wxWindow *parent,
                           const wxString &message,
                           std::function<void(bool)> callback_dialog_closed);

private:
    void prepare_vendor_change_async(
        Slic3r::VendorChange change,
        const std::vector<std::string> &vendor_ids,
        Slic3r::PresetUpdaterHost::PrepareCallback callback) override;
    void rollback_vendor_change_async(
        const std::string &token,
        Slic3r::PresetUpdaterHost::RollbackCallback callback) override;
    void vendor_files_changed(Slic3r::PresetUpdater &updater,
                              Slic3r::VendorChange change,
                              const std::vector<std::string> &vendor_ids) override;

    void reload_application_presets(Slic3r::VendorChange change, const std::vector<std::string> &vendor_ids);
    void show_synch_window_internal();
    void dispatch_error_callback(const std::function<void(const std::string &)> &callback_result,
                                 Slic3r::UpdaterError error);
    void dispatch_errors_callback(const std::function<void(const std::string &)> &callback_result,
                                  Slic3r::UpdaterErrors errors);
    void post_to_gui(std::function<void()> operation) noexcept;

    GUI_App &m_app;
    Slic3r::PresetUpdater m_core;
    std::mutex m_dialog_mutex;
    wxWindow *m_dialog_parent = nullptr;
    std::string m_dialog_message;
    std::function<void(bool)> m_dialog_callback;
    Slic3r::UpdaterOperationExecutor m_snapshot_executor;
};

} // namespace Slic3r::GUI

#endif // slic3r_Utils_PresetUpdater_hpp_
