///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// The dialog exposes the same repository lifecycle as vendor updates: add a
// description, fetch compatible tags, cache an archive, then select it for a
// later install. It never touches a plugin DLL owned by this running process.

#include "PluginUpdateDialog.hpp"

#include <string>
#include <vector>

#include <wx/button.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include "slic3r/Utils/PluginUpdater.hpp"

#include "I18N.hpp"
#include "GUI.hpp"

namespace Slic3r::GUI {

PluginUpdateDialog::PluginUpdateDialog(wxWindow *parent, PluginUpdater &updater)
    : wxDialog(parent, wxID_ANY, _L("Plugin updates"), wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_updater(updater)
{
    m_updater.reload_all_plugins();
    rebuild();
    CentreOnParent();
}

void PluginUpdateDialog::rebuild()
{
    Freeze();
    if (m_main_sizer != nullptr)
        m_main_sizer->Clear(true);
    else
        m_main_sizer = new wxBoxSizer(wxVERTICAL);

    wxBoxSizer *repository_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_repository_url = new wxTextCtrl(this, wxID_ANY);
    wxButton *add_button = new wxButton(this, wxID_ANY, _L("Add repository"));
    wxButton *check_button = new wxButton(this, wxID_ANY, _L("Check for updates"));
    repository_sizer->Add(m_repository_url, 1, wxRIGHT, 6);
    repository_sizer->Add(add_button, 0, wxRIGHT, 6);
    repository_sizer->Add(check_button, 0);
    m_main_sizer->Add(repository_sizer, 0, wxEXPAND | wxALL, 10);
    add_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { add_repository(); });
    check_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { check_updates(); });

    wxFlexGridSizer *grid = new wxFlexGridSizer(5, 8, 10);
    grid->AddGrowableCol(1, 1);
    grid->Add(new wxStaticText(this, wxID_ANY, _L("Plugin")));
    grid->Add(new wxStaticText(this, wxID_ANY, _L("Description")));
    grid->Add(new wxStaticText(this, wxID_ANY, _L("Installed version")));
    grid->AddSpacer(0);
    grid->AddSpacer(0);

    const std::vector<std::string> plugin_ids = m_updater.plugin_ids();
    for (const std::string &id : plugin_ids) {
        PluginSync *plugin = m_updater.get_plugin(id);
        if (plugin == nullptr)
            continue;
        const wxString display_name = from_u8(plugin->description.full_name.empty() ? id : plugin->description.full_name);
        const wxString description = from_u8(plugin->description.description);
        const wxString installed = plugin->is_installed ? from_u8(plugin->installed_version.package_version) : _L("Not installed");
        wxButton *install_button = new wxButton(this, wxID_ANY,
            plugin->is_installed ? _L("Schedule update") : _L("Install"));
        install_button->Enable(plugin->best != nullptr && (!plugin->is_installed || plugin->can_upgrade));
        wxButton *clear_button = new wxButton(this, wxID_ANY, _L("Clear cache"));
        install_button->Bind(wxEVT_BUTTON, [this, id](wxCommandEvent &) { install_latest(id); });
        clear_button->Bind(wxEVT_BUTTON, [this, id](wxCommandEvent &) { clear_cache(id); });

        grid->Add(new wxStaticText(this, wxID_ANY, display_name), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(new wxStaticText(this, wxID_ANY, description), 0, wxALIGN_CENTER_VERTICAL | wxEXPAND);
        grid->Add(new wxStaticText(this, wxID_ANY, installed), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(install_button, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(clear_button, 0, wxALIGN_CENTER_VERTICAL);
    }
    if (plugin_ids.empty()) {
        grid->Add(new wxStaticText(this, wxID_ANY, _L("No plugin repository is configured.")));
        for (int index = 0; index < 4; ++index)
            grid->AddSpacer(0);
    }
    m_main_sizer->Add(grid, 1, wxEXPAND | wxLEFT | wxRIGHT, 10);

    wxStdDialogButtonSizer *buttons = CreateStdDialogButtonSizer(wxCLOSE);
    m_main_sizer->Add(buttons, 0, wxEXPAND | wxALL, 10);
    SetSizerAndFit(m_main_sizer);
    Thaw();
}

void PluginUpdateDialog::add_repository()
{
    const std::string url = m_repository_url->GetValue().utf8_string();
    if (url.empty())
        return;
    m_updater.download_new_repo(url, [this](bool success) {
        CallAfter([this, success] {
            if (!success)
                wxMessageBox(_L("Unable to read a valid plugin description.ini from this repository."), _L("Plugin updates"), wxICON_ERROR);
            else
                m_updater.reload_all_plugins();
            rebuild();
        });
    });
}

void PluginUpdateDialog::check_updates()
{
    m_updater.sync_async([this](int) { CallAfter([this] { rebuild(); }); }, true);
}

void PluginUpdateDialog::install_latest(const std::string &plugin_id)
{
    PluginSync *plugin = m_updater.get_plugin(plugin_id);
    if (plugin == nullptr || plugin->best == nullptr)
        return;
    const PluginAvailable version = *plugin->best;
    m_updater.install_plugin(plugin_id, version, [this](const std::string &error_message) {
        CallAfter([this, error_message] {
            if (!error_message.empty())
                wxMessageBox(from_u8(error_message), _L("Plugin updates"), wxICON_ERROR);
            else
                wxMessageBox(_L("The selected plugin version will be installed after restarting the application."),
                             _L("Plugin updates"), wxICON_INFORMATION);
            m_updater.reload_all_plugins();
            rebuild();
        });
    });
}

void PluginUpdateDialog::clear_cache(const std::string &plugin_id)
{
    m_updater.clear_cache_plugin(plugin_id, [this](bool success) {
        CallAfter([this, success] {
            if (!success)
                wxMessageBox(_L("Unable to clear this plugin cache."), _L("Plugin updates"), wxICON_ERROR);
            rebuild();
        });
    });
}

} // namespace Slic3r::GUI
