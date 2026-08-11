///|/ Copyright (c) SuperSlicer 2026 Durand R?mi @supermerill
///|/ Copyright (c) Prusa Research 2018 - 2023 David Kocík @kocikdav, Lukáš Matěna @lukasmatena, Vojtěch Bubník @bubnikv, Oleksandra Iushchenko @YuSanka, Vojtěch Král @vojtechkral
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_PresetUpdate_hpp_
#define slic3r_PresetUpdate_hpp_

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <boost/filesystem/path.hpp>

#include <wx/event.h>

#include "libslic3r/Preset.hpp"
#include "libslic3r/Semver.hpp"

#include "RepositoryUpdater.hpp"
namespace Slic3r {


class AppConfig;
class PresetBundle;
class Semver;
class PresetUpdater;
class PresetBundle;

#define USE_GTHUB_PRESET_UPDATE 1

struct VendorAvailable
{
    Semver config_version;
    Semver slicer_version;
    std::string local_file;
    std::string url_zip;
    std::string commit_sha;
    std::string commit_url;
    std::string tag;
    std::string notes;
};
struct VendorSync
{
    VendorProfile profile;
    bool is_installed = false;
    bool has_cache = false;
    bool is_synch = false;
    bool synch_in_progress = false;
    bool synch_failed = false;
    bool can_upgrade = false;
    // true if "profile" is inside datadir's vendor directory 
    std::vector<VendorAvailable> available_profiles = {};
    VendorAvailable *best = nullptr;


    bool parse_tags(const std::string &json);
    void sort_available();
    // reurn error message (empty if install succeed)
    std::string install_vendor_config(const VendorAvailable &to_install, PresetUpdater& api_slot);
    bool uninstall_vendor_config();
    bool clear_cache();
    void reset(const VendorProfile &profile, bool installed, bool has_cache);
};
class PresetUpdater : public RepositoryUpdater
{
protected:
public:
    bool cancel = false; //TODO
    std::thread thread;

    // PresetUpdater still owns its legacy synchronization state because the
    // configuration wizard chains callbacks while it is displaying dialogs.
    std::atomic_int profiles_synch = 0;
    std::mutex callback_update_preset_mutex;
    std::function<void(int)> callback_update_preset;
    std::atomic_bool synch_process_ongoing = false;

    //std::vector<VendorSync> installed_vendors;
    //std::vector<VendorSync> unused_vendors;
    std::recursive_mutex all_vendors_mutex;
    std::map<std::string, VendorSync> all_vendors;

    bool is_synch = false;

    std::mutex callback_update_changelog_mutex;
    std::atomic_int changelog_synch = 0;

    PresetUpdater(wxEvtHandler* evt_handler);
    PresetUpdater(PresetUpdater &&) = delete;
    PresetUpdater(const PresetUpdater &) = delete;
    PresetUpdater &operator=(PresetUpdater &&) = delete;
    PresetUpdater &operator=(const PresetUpdater &) = delete;
    ~PresetUpdater();

    enum PresetUpdaterResult
    {
        PUR_Nothing,
        PUR_Updates
    };
    
    int get_profile_count_to_update();
    size_t count_available();
    size_t count_installed();
    VendorSync *get_vendor(std::string id) {
        auto it = all_vendors.find(id);
        return it == all_vendors.end() ? nullptr : &it->second;
    }

    void set_installed_vendors(const PresetBundle *preset_bundle);
    void reload_all_vendors();

    // If either version check or config updating is enabled, get the appropriate data in the background and cache it.
    // call the handler with PresetUpdaterResult if somethign isntalle
    void sync_async(std::function<void(int)> callback_update_preset, bool force = false);
    void download_logs(const std::string &vendor_id, std::function<void(bool)> callback_result, bool force = false);

    // Show the window to select what to download
    // It's only emitting the gui event, this can be called evrywhere and return immediatly.
    // call callback_dialog_closed when done (in the gui thread)
    void show_synch_window(wxWindow *parent,
                           const wxString &message,
                           std::function<void(bool)> callback_dialog_closed);

    void download_new_repo(const std::string &github_org_repo, std::function<void(bool)> callback_result);
    void uninstall_vendor(const std::string &vendor_id, std::function<void(bool)> callback_result);
    void install_vendor(const std::string &vendor_id, const VendorAvailable &version, std::function<void(const std::string &)> callback_result);
    void clear_cache_vendor(const std::string &vendor_id, std::function<void(bool)> callback_result);

    void uninstall_all_vendors(std::function<void(bool)> callback_result);
    void install_all_vendors(std::function<void(const std::string &)> callback_result);
    void upgrade_all_installed_vendors(std::function<void(const std::string &)> callback_result);
protected:
    void load_unused_vendors(std::set<std::string> &vendors_id,
                             const boost::filesystem::path vendor_dir,
                             bool is_installed);

    void update_vendor(VendorSync &vendor, bool force = false);
    // must be call by each update_vendor call at some point.
    int update_count() override;
    void on_sync_completed() override;
    void end_updating();
    bool has_api_request_slot(const std::string &url);

    std::mutex args_for_dialog_mutex;
    struct ShowDialogArgs {
        wxWindow *parent;
        wxString message;
        std::function<void(bool)> callback_dialog_closed;
    } args_for_dialog;
    void _show_synch_window_internal();

    wxEvtHandler* evt_handler;
};

wxDECLARE_EVENT(EVT_CONFIG_UPDATER_SYNC_DONE, wxCommandEvent);
// to create the window in the gui thread.
wxDECLARE_EVENT(EVT_CONFIG_UPDATER_SHOW_DIALOG, wxCommandEvent);
}
#endif
