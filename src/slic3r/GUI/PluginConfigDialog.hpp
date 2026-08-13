///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_GUI_PluginConfigDialog_hpp_
#define slic3r_GUI_PluginConfigDialog_hpp_

#include <memory>
#include <string>

#include "GUI_Utils.hpp"

class wxCommandEvent;
class wxRect;
class wxWindow;

namespace Slic3r::GUI {

class PluginConfigDialogState;

class PluginConfigDialog : public DPIDialog
{
public:
    explicit PluginConfigDialog(wxWindow *parent);
    ~PluginConfigDialog() override;

private:
    void build();
    void build_catalog();
    void build_navigation();
    void refresh_navigation_counts();
    void refresh_plugin_list();
    void refresh_details();
    bool write_active_plugins(std::string &error_message) const;
    bool prepare_restart() const;
    void save_and_restart(wxCommandEvent &event);
    void on_dpi_changed(const wxRect &suggested_rect) override;

    std::unique_ptr<PluginConfigDialogState> m_state;
};

} // namespace Slic3r::GUI

#endif // slic3r_GUI_PluginConfigDialog_hpp_
