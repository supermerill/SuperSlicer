///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// This compact dialog manages plugin repositories and package updates. Plugin
// activation deliberately remains in PluginConfigDialog because enabling a
// loaded plugin and selecting a package version are independent decisions.

#ifndef slic3r_GUI_PluginUpdateDialog_hpp_
#define slic3r_GUI_PluginUpdateDialog_hpp_

#include <memory>
#include <string>

#include "RepositoryUpdatesDialogBase.hpp"

class wxBoxSizer;
class wxBusyInfo;
class wxFlexGridSizer;
class wxGridBagSizer;
class wxScrolledWindow;
class wxTextCtrl;

namespace Slic3r {
class PluginUpdater;
struct PluginAvailable;
struct PluginSync;
}

namespace Slic3r::GUI {

class PluginUpdateDialog : public RepositoryUpdatesDialogBase {
public:
    PluginUpdateDialog(wxWindow *parent, PluginUpdater &updater);

private:
    void rebuild();
    void add_plugin_row(const PluginSync &plugin, wxFlexGridSizer &grid);
    void add_repository();
    void load_package_directory();
    void check_updates();
    void choose_version(const std::string &plugin_id);
    void install_latest(const std::string &plugin_id);
    void uninstall(const std::string &plugin_id);
    void clear_cache(const std::string &plugin_id);

    PluginUpdater &m_updater;
    wxBoxSizer *m_main_sizer = nullptr;
    wxTextCtrl *m_repository_url = nullptr;
};

// Lists every package version after its changelog has been loaded. Selecting a
// row downloads and validates that package immediately, but only records the
// requested version for installation during the next application startup.
class ChoosePluginVersionDialog : public wxDialog {
public:
    ChoosePluginVersionDialog(wxWindow *parent, PluginUpdater &updater, std::string plugin_id);
    ~ChoosePluginVersionDialog() override;

private:
    void build();
    void schedule_version(const PluginAvailable &version);

    PluginUpdater &m_updater;
    std::string m_plugin_id;
    wxScrolledWindow *m_scroll = nullptr;
    std::unique_ptr<wxBusyInfo> m_wait_dialog;
};

} // namespace Slic3r::GUI

#endif // slic3r_GUI_PluginUpdateDialog_hpp_
