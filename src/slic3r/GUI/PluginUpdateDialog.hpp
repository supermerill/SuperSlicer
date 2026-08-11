///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// This compact dialog manages plugin repositories and package updates. Plugin
// activation deliberately remains in PluginConfigDialog because enabling a
// loaded plugin and selecting a package version are independent decisions.

#ifndef slic3r_GUI_PluginUpdateDialog_hpp_
#define slic3r_GUI_PluginUpdateDialog_hpp_

#include <string>

#include "RepositoryUpdatesDialogBase.hpp"

class wxBoxSizer;
class wxTextCtrl;

namespace Slic3r {
class PluginUpdater;
}

namespace Slic3r::GUI {

class PluginUpdateDialog : public RepositoryUpdatesDialogBase {
public:
    PluginUpdateDialog(wxWindow *parent, PluginUpdater &updater);

private:
    void rebuild();
    void add_repository();
    void check_updates();
    void install_latest(const std::string &plugin_id);
    void clear_cache(const std::string &plugin_id);

    PluginUpdater &m_updater;
    wxBoxSizer *m_main_sizer = nullptr;
    wxTextCtrl *m_repository_url = nullptr;
};

} // namespace Slic3r::GUI

#endif // slic3r_GUI_PluginUpdateDialog_hpp_
