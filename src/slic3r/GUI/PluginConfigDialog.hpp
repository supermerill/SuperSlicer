///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_GUI_PluginConfigDialog_hpp_
#define slic3r_GUI_PluginConfigDialog_hpp_

#include <string>
#include <map>
#include <set>
#include <vector>

#include "GUI_Utils.hpp"

class wxCheckBox;
class wxCommandEvent;
class wxRect;
class wxWindow;

namespace Slic3r::GUI {

class PluginConfigDialog : public DPIDialog
{
public:
    explicit PluginConfigDialog(wxWindow *parent);

private:
    struct PluginRow
    {
        std::string id;
        wxCheckBox *checkbox { nullptr };
        bool preserve_unavailable_activation { false };
    };

    void build();
    bool write_active_plugins(std::string &error_message) const;
    bool prepare_restart() const;
    void save_and_restart(wxCommandEvent &event);
    void on_dpi_changed(const wxRect &suggested_rect) override;

    std::set<std::string> m_original_active_plugin_ids;
    std::map<std::string, std::string> m_plugin_packages;
    std::vector<PluginRow> m_rows;
};

} // namespace Slic3r::GUI

#endif // slic3r_GUI_PluginConfigDialog_hpp_
