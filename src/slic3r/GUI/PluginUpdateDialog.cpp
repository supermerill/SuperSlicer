///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// The dialog exposes the same repository lifecycle as vendor updates: add a
// description, fetch compatible tags, cache an archive, then select it for a
// later install. It never touches a plugin DLL owned by this running process.

#include "PluginUpdateDialog.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <wx/busyinfo.h>
#include <wx/button.h>
#include <wx/dirdlg.h>
#include <wx/gbsizer.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include "libslic3r/Updater/PluginUpdater.hpp"
#include "libslic3r/Semver.hpp"

#include "I18N.hpp"
#include "GUI.hpp"
#include "UpdaterErrorMessages.hpp"

namespace Slic3r::GUI {

PluginUpdateDialog::PluginUpdateDialog(wxWindow *parent, PluginUpdater &updater)
    : RepositoryUpdatesDialogBase(parent, _L("Plugin updates"))
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
    wxButton *load_button = new wxButton(this, wxID_ANY, _L("Load plugin folder"));
    wxButton *check_button = new wxButton(this, wxID_ANY, _L("Check for updates"));
    repository_sizer->Add(m_repository_url, 1, wxRIGHT, 6);
    repository_sizer->Add(add_button, 0, wxRIGHT, 6);
    repository_sizer->Add(load_button, 0, wxRIGHT, 6);
    repository_sizer->Add(check_button, 0);
    m_main_sizer->Add(repository_sizer, 0, wxEXPAND | wxALL, 10);
    add_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { add_repository(); });
    load_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { load_package_directory(); });
    check_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { check_updates(); });

    wxFlexGridSizer *grid = new wxFlexGridSizer(5, 8, 10);
    grid->AddGrowableCol(1, 1);
    grid->Add(new wxStaticText(this, wxID_ANY, _L("Plugin")));
    grid->Add(new wxStaticText(this, wxID_ANY, _L("Description")));
    grid->Add(new wxStaticText(this, wxID_ANY, _L("Selected version")));
    grid->AddSpacer(0);
    grid->AddSpacer(0);

    const std::vector<std::string> plugin_ids = m_updater.plugin_ids();
    for (const std::string &id : plugin_ids) {
        PluginSync *plugin = m_updater.get_plugin(id);
        if (plugin == nullptr)
            continue;
        const wxString display_name = from_u8(plugin->description.full_name.empty() ? id : plugin->description.full_name);
        const wxString description = from_u8(plugin->description.description);
        wxString version_label;
        if (plugin->is_installed)
            version_label = from_u8(plugin->installed_version.package_version);
        else if (plugin->available_packages.size() > 1)
            version_label = _L("Choose version");
        else
            version_label = _L("Not installed");
        wxButton *version_button = new wxButton(this, wxID_ANY, version_label);
        version_button->Enable(!plugin->available_packages.empty() &&
                               (plugin->is_installed || plugin->available_packages.size() > 1));
        version_button->SetToolTip(_L("Choose a plugin package version and review its changelog."));
        version_button->Bind(wxEVT_BUTTON, [this, id](wxCommandEvent &) { choose_version(id); });
        wxButton *install_button = new wxButton(this, wxID_ANY,
            plugin->is_installed ? _L("Schedule update") : _L("Install"));
        install_button->Enable(plugin->best != nullptr && (!plugin->is_installed || plugin->can_upgrade));
        wxButton *clear_button = new wxButton(this, wxID_ANY, _L("Clear cache"));
        install_button->Bind(wxEVT_BUTTON, [this, id](wxCommandEvent &) { install_latest(id); });
        clear_button->Bind(wxEVT_BUTTON, [this, id](wxCommandEvent &) { clear_cache(id); });

        grid->Add(new wxStaticText(this, wxID_ANY, display_name), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(new wxStaticText(this, wxID_ANY, description), 0, wxALIGN_CENTER_VERTICAL | wxEXPAND);
        grid->Add(version_button, 0, wxALIGN_CENTER_VERTICAL | wxEXPAND);
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
    m_updater.download_new_repo(url, [this](UpdaterError error) {
        CallAfter([this, error = std::move(error)] {
            if (!error.succeeded())
                wxMessageBox(from_u8(format_updater_error(error)), _L("Plugin updates"), wxICON_ERROR);
            else
                m_updater.reload_all_plugins();
            rebuild();
        });
    });
}

void PluginUpdateDialog::load_package_directory()
{
    wxDirDialog dialog(this, _L("Choose a plugin package folder"));
    if (dialog.ShowModal() != wxID_OK)
        return;

    const UpdaterError error = m_updater.cache_plugin_directory(
        boost::filesystem::path(dialog.GetPath().utf8_string()));
    if (!error.succeeded()) {
        wxMessageBox(from_u8(format_updater_error(error)), _L("Plugin updates"), wxICON_ERROR);
        return;
    }
    m_updater.reload_all_plugins();
    rebuild();
}

void PluginUpdateDialog::check_updates()
{
    m_updater.sync_async([this](int) { CallAfter([this] { rebuild(); }); }, true);
}

void PluginUpdateDialog::choose_version(const std::string &plugin_id)
{
    // Changelog failures do not hide otherwise usable packages. The chooser
    // opens after every request has finished and leaves missing notes blank.
    m_updater.download_changelogs(plugin_id, [this, plugin_id](bool) {
        CallAfter([this, plugin_id] {
            ChoosePluginVersionDialog dialog(this, m_updater, plugin_id);
            dialog.ShowModal();
            rebuild();
        });
    });
}

void PluginUpdateDialog::install_latest(const std::string &plugin_id)
{
    PluginSync *plugin = m_updater.get_plugin(plugin_id);
    if (plugin == nullptr || plugin->best == nullptr)
        return;
    const PluginAvailable version = *plugin->best;
    m_updater.install_plugin(plugin_id, version, [this](UpdaterError error) {
        CallAfter([this, error = std::move(error)] {
            if (!error.succeeded())
                wxMessageBox(from_u8(format_updater_error(error)), _L("Plugin updates"), wxICON_ERROR);
            else
                wxMessageBox(_L("The selected plugin version will be installed after restarting the application."),
                             _L("Plugin updates"), wxICON_INFORMATION);
            rebuild();
        });
    });
}

void PluginUpdateDialog::clear_cache(const std::string &plugin_id)
{
    m_updater.clear_cache_plugin(plugin_id, [this](UpdaterError error) {
        CallAfter([this, error = std::move(error)] {
            if (!error.succeeded())
                wxMessageBox(from_u8(format_updater_error(error)), _L("Plugin updates"), wxICON_ERROR);
            rebuild();
        });
    });
}

ChoosePluginVersionDialog::ChoosePluginVersionDialog(wxWindow *parent,
                                                     PluginUpdater &updater,
                                                     std::string plugin_id)
    : wxDialog(parent, wxID_ANY, _L("Choose plugin version"), wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_updater(updater)
    , m_plugin_id(std::move(plugin_id))
{
    build();
    CentreOnParent();
}

ChoosePluginVersionDialog::~ChoosePluginVersionDialog() = default;

void ChoosePluginVersionDialog::build()
{
    wxBoxSizer *main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->Add(new wxStaticText(
        this, wxID_ANY,
        _L("Choose the plugin package to install after restarting the application.")),
        0, wxALL, 10);

    // The scrollable table keeps long changelogs usable without making the
    // dialog taller than the screen. Its rows borrow no updater data: button
    // callbacks retain a complete PluginAvailable value.
    m_scroll = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    wxGridBagSizer *grid = new wxGridBagSizer(8, 16);
    grid->AddGrowableCol(2, 1);
    grid->Add(new wxStaticText(m_scroll, wxID_ANY, _L("Plugin version")), wxGBPosition(0, 0));
    grid->Add(new wxStaticText(m_scroll, wxID_ANY, _L("Slicer version")), wxGBPosition(0, 1));
    grid->Add(new wxStaticText(m_scroll, wxID_ANY, _L("Changelog")), wxGBPosition(0, 2));

    PluginSync *plugin = m_updater.get_plugin(m_plugin_id);
    const std::optional<Semver> current_slicer = Semver::parse(SLIC3R_VERSION_FULL);
    int row = 1;
    if (plugin != nullptr) {
        for (const PluginAvailable &version : plugin->available_packages) {
            const bool selected = plugin->is_installed &&
                plugin->installed_version.package_version == version.package_version &&
                plugin->installed_version.slicer_version == version.slicer_version;
            const std::optional<Semver> target_slicer = Semver::parse(version.slicer_version);
            const bool compatible = current_slicer && target_slicer && *target_slicer <= *current_slicer;

            if (selected) {
                // This row describes the package selected for startup, so it
                // uses a non-interactive status panel instead of a disabled
                // button. The centered label and background match the vendor
                // version chooser without suggesting that it can be clicked.
                wxPanel *installed_panel = new wxPanel(m_scroll, wxID_ANY);
                wxBoxSizer *installed_sizer = new wxBoxSizer(wxHORIZONTAL);
                wxStaticText *installed = new wxStaticText(installed_panel, wxID_ANY, from_u8(version.package_version));
                installed_sizer->AddStretchSpacer();
                installed_sizer->Add(installed, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, 5);
                installed_sizer->AddStretchSpacer();
                installed_panel->SetSizer(installed_sizer);
                installed_panel->SetBackgroundColour(wxColour(127, 250, 127));
                installed->SetBackgroundColour(wxColour(127, 250, 127));
                installed_panel->SetToolTip(_L("This plugin package is selected for the next application startup."));
                installed->SetToolTip(_L("This plugin package is selected for the next application startup."));
                grid->Add(installed_panel, wxGBPosition(row, 0), wxDefaultSpan, wxEXPAND);
            } else {
                wxButton *select = new wxButton(m_scroll, wxID_ANY, from_u8(version.package_version));
                select->Enable(compatible);
                select->SetToolTip(compatible ?
                    _L("Download this package and install it after restarting the application.") :
                    _L("This package requires a newer slicer version."));
                select->Bind(wxEVT_BUTTON, [this, version](wxCommandEvent &) { schedule_version(version); });
                grid->Add(select, wxGBPosition(row, 0), wxDefaultSpan, wxEXPAND);
            }

            wxStaticText *slicer = new wxStaticText(m_scroll, wxID_ANY, from_u8(version.slicer_version));
            slicer->SetToolTip(format(_L("This package targets slicer version %1%. Current version: %2%."),
                                      version.slicer_version, SLIC3R_VERSION_FULL));
            grid->Add(slicer, wxGBPosition(row, 1), wxDefaultSpan, wxALIGN_CENTER_VERTICAL);

            wxStaticText *notes = new wxStaticText(m_scroll, wxID_ANY, from_u8(version.notes));
            notes->Wrap(450);
            grid->Add(notes, wxGBPosition(row, 2), wxDefaultSpan, wxALIGN_CENTER_VERTICAL | wxEXPAND);
            ++row;
        }
    }

    m_scroll->SetScrollRate(0, 30);
    m_scroll->SetSizer(grid);
    grid->FitInside(m_scroll);
    m_scroll->SetMinSize(wxSize(760, std::min(500, grid->GetMinSize().GetHeight() + 20)));
    main_sizer->Add(m_scroll, 1, wxEXPAND | wxLEFT | wxRIGHT, 10);

    wxStdDialogButtonSizer *buttons = CreateStdDialogButtonSizer(wxCLOSE);
    main_sizer->Add(buttons, 0, wxEXPAND | wxALL, 10);
    SetSizerAndFit(main_sizer);
}

void ChoosePluginVersionDialog::schedule_version(const PluginAvailable &version)
{
    m_wait_dialog = std::make_unique<wxBusyInfo>(_L("Downloading the plugin package. Please wait."), this);
    m_updater.install_plugin(m_plugin_id, version, [this](UpdaterError error) {
        CallAfter([this, error = std::move(error)] {
            m_wait_dialog.reset();
            if (!error.succeeded()) {
                wxMessageBox(from_u8(format_updater_error(error)), _L("Plugin updates"), wxICON_ERROR, this);
                return;
            }
            wxMessageBox(_L("The selected plugin version will be installed after restarting the application."),
                         _L("Plugin updates"), wxICON_INFORMATION, this);
            EndModal(wxID_OK);
        });
    });
}

} // namespace Slic3r::GUI
