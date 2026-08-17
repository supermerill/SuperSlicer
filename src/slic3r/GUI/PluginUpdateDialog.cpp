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
#include "GUI_App.hpp"
#include "UpdaterErrorMessages.hpp"

namespace Slic3r::GUI {

namespace {

// Choose the compact status shown in the Upgrade column. The complete reason
// stays in the tooltip so the table remains easy to scan.
wxString plugin_package_load_label(const PluginPackageLoadReport &report);

// Format every issue recorded during startup, including the package path and
// plugin id when available. Multiple failures are kept because one package may
// register several plugin instances independently.
wxString plugin_package_load_tooltip(const PluginPackageLoadReport &report);

wxString plugin_package_load_label(const PluginPackageLoadReport &report)
{
    if (report.issues.empty())
        return _L("Loaded");
    switch (report.issues.front().code) {
    case PluginPackageLoadErrorCode::PackageMissing:             return _L("Package missing");
    case PluginPackageLoadErrorCode::DependencyMissing:          return _L("Missing dependency");
    case PluginPackageLoadErrorCode::MissingAbiExport:
    case PluginPackageLoadErrorCode::AbiMismatch:                return _L("Plugin API mismatch");
    case PluginPackageLoadErrorCode::PythonRuntimeUnavailable:   return _L("Python runtime missing");
    case PluginPackageLoadErrorCode::PythonReadFailed:
    case PluginPackageLoadErrorCode::PythonCompileFailed:
    case PluginPackageLoadErrorCode::PythonImportFailed:
    case PluginPackageLoadErrorCode::PythonRegistrationFailed:   return _L("Python error");
    case PluginPackageLoadErrorCode::InvalidPackage:             return _L("Invalid package");
    case PluginPackageLoadErrorCode::LibraryOpenFailed:          return _L("Load failed");
    case PluginPackageLoadErrorCode::MissingRegistrationExport:
    case PluginPackageLoadErrorCode::RegistrationFailed:
    case PluginPackageLoadErrorCode::NoPluginsRegistered:
    case PluginPackageLoadErrorCode::ConfiguredPluginMissing:    return _L("Registration failed");
    }
    return _L("Load failed");
}

wxString plugin_package_load_tooltip(const PluginPackageLoadReport &report)
{
    wxString tooltip = format(_L("Package: %1%\nPath: %2%"),
                              from_u8(report.package_id), from_u8(report.package_path));
    for (const PluginPackageLoadIssue &issue : report.issues) {
        tooltip += "\n\n";
        if (!issue.plugin_id.empty())
            tooltip += format(_L("Plugin: %1%\n"), from_u8(issue.plugin_id));
        tooltip += from_u8(issue.detail);
        if (issue.plugin_abi != 0 || issue.host_abi != 0)
            tooltip += format(_L("\nPlugin API: %1%; host API: %2%."), issue.plugin_abi, issue.host_abi);
        if (issue.system_error != 0)
            tooltip += format(_L("\nSystem error code: %1%."), issue.system_error);
    }
    tooltip += "\n\n";
    tooltip += _L("Choose another version or reinstall the package to repair it.");
    return tooltip;
}

} // namespace

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
    const bool first_build = m_main_sizer == nullptr;
    m_repository_action_controls.clear();
    if (m_main_sizer != nullptr) {
        m_main_sizer->Clear(true);
    } else {
        m_main_sizer = new wxBoxSizer(wxVERTICAL);
        SetSizer(m_main_sizer);
    }

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
    bind_repository_action(*add_button, [this](wxCommandEvent &) {
        add_repository();
    });
    bind_repository_action(*load_button, [this](wxCommandEvent &) {
        load_package_directory();
    });
    bind_repository_action(*check_button, [this](wxCommandEvent &) {
        check_updates();
    });
    m_repository_action_controls.emplace_back(m_repository_url);
    m_repository_action_controls.emplace_back(add_button);
    m_repository_action_controls.emplace_back(load_button);
    m_repository_action_controls.emplace_back(check_button);

    wxFlexGridSizer *grid = new wxFlexGridSizer(5, 8, 10);
    grid->AddGrowableCol(1, 1);
    grid->Add(new wxStaticText(this, wxID_ANY, _L("Plugin")));
    grid->Add(new wxStaticText(this, wxID_ANY, _L("Description")));
    grid->Add(new wxStaticText(this, wxID_ANY, _L("Selected version")));
    grid->Add(new wxStaticText(this, wxID_ANY, _L("Upgrade")));
    grid->Add(new wxStaticText(this, wxID_ANY, _L("Uninstall")));

    const std::vector<PluginSync> plugins = m_updater.plugins();
    for (const PluginSync &plugin : plugins)
        add_plugin_row(plugin, *grid);
    if (plugins.empty()) {
        grid->Add(new wxStaticText(this, wxID_ANY, _L("No plugin repository is configured.")));
        for (int index = 0; index < 4; ++index)
            grid->AddSpacer(0);
    }
    m_main_sizer->Add(grid, 1, wxEXPAND | wxLEFT | wxRIGHT, 10);

    wxStdDialogButtonSizer *buttons = CreateStdDialogButtonSizer(wxCLOSE);
    m_main_sizer->Add(buttons, 0, wxEXPAND | wxALL, 10);
    apply_plugin_operation_state();
    wxGetApp().UpdateDlgDarkUI(this);
    if (first_build)
        m_main_sizer->Fit(this);
    Layout();
    Thaw();
    Refresh();
}

void PluginUpdateDialog::add_plugin_row(const PluginSync &plugin,
                                        wxFlexGridSizer &grid)
{
    const std::string &plugin_id = plugin.description.id;
    const PluginAvailable *best = plugin.best_available();
    const bool has_compatible_version = best != nullptr;
    const wxString display_name = from_u8(
        plugin.description.full_name.empty() ? plugin_id : plugin.description.full_name);

    grid.Add(new wxStaticText(this, wxID_ANY, display_name), 0, wxALIGN_CENTER_VERTICAL);
    grid.Add(new wxStaticText(this, wxID_ANY, from_u8(plugin.description.description)),
             0, wxALIGN_CENTER_VERTICAL | wxEXPAND);

    // Keep the panel enabled when no package is available. On Windows a
    // disabled native button does not receive mouse events, while its parent
    // panel can still display the reason through a tooltip.
    wxString version_label;
    if (plugin.is_installed)
        version_label = from_u8(plugin.installed_version.package_version);
    else if (!plugin.available_packages.empty())
        version_label = _L("Choose version");
    else
        version_label = _L("Not installed");
    wxPanel *version_panel = new wxPanel(this, wxID_ANY);
    wxBoxSizer *version_sizer = new wxBoxSizer(wxHORIZONTAL);
    wxButton *version_button = new wxButton(version_panel, wxID_ANY, version_label);
    version_sizer->Add(version_button, 1, wxEXPAND);
    version_panel->SetSizer(version_sizer);
    if (plugin.available_packages.empty()) {
        version_button->Enable(false);
        version_panel->SetToolTip(
            _L("No plugin package version is available in the local cache or repository."));
    } else {
        version_button->SetToolTip(_L("Choose a plugin package version and review its changelog."));
        bind_repository_action(*version_button, [this, plugin_id](wxCommandEvent &) {
            choose_version(plugin_id);
        });
        m_repository_action_controls.emplace_back(version_panel);
    }
    grid.Add(version_panel, 0, wxALIGN_CENTER_VERTICAL | wxEXPAND);

    // The upgrade cell reports repository state when there is no immediate
    // install action. This avoids presenting a permanently disabled command as
    // if an update were available.
    wxWindow *upgrade_control = nullptr;
    if (plugin.load_report.has_value() && !plugin.load_report->issues.empty()) {
        upgrade_control = new wxStaticText(this, wxID_ANY,
                                           plugin_package_load_label(*plugin.load_report));
        upgrade_control->SetToolTip(plugin_package_load_tooltip(*plugin.load_report));
    } else if (!plugin.is_installed && has_compatible_version && !best->local_directory.empty()) {
        wxButton *install = new wxButton(
            this, wxID_ANY, format(_L("Install %1% (local)"), best->package_version));
        install->SetToolTip(
            _L("Install the compatible plugin package already available in the local cache after restarting the application."));
        bind_repository_action(*install, [this, plugin_id](wxCommandEvent &) {
            install_latest(plugin_id);
        });
        m_repository_action_controls.emplace_back(install);
        upgrade_control = install;
    } else if (plugin.description.config_update_rest.empty()) {
        if (plugin.can_upgrade && has_compatible_version) {
            wxButton *upgrade = new wxButton(
                this, wxID_ANY, format(_L("Upgrade to %1%"), best->package_version));
            upgrade->SetToolTip(
                _L("Download this plugin version and install it after restarting the application."));
            bind_repository_action(*upgrade, [this, plugin_id](wxCommandEvent &) {
                install_latest(plugin_id);
            });
            m_repository_action_controls.emplace_back(upgrade);
            upgrade_control = upgrade;
        } else if (!plugin.available_packages.empty() && !has_compatible_version) {
            upgrade_control = new wxStaticText(this, wxID_ANY, _L("No compatible plugin"));
            upgrade_control->SetToolTip(
                _L("The local cache contains plugin packages, but none target this slicer version."));
        } else {
            upgrade_control = new wxStaticText(this, wxID_ANY, _L("Local plugin"));
            upgrade_control->SetToolTip(
                _L("This plugin has no repository and therefore cannot be checked for online updates."));
        }
    } else if (plugin.sync_state == RepositorySyncState::Succeeded) {
        if (plugin.available_packages.empty()) {
            upgrade_control = new wxStaticText(this, wxID_ANY, _L("No package available"));
            upgrade_control->SetToolTip(
                _L("This plugin repository does not provide any package version."));
        } else if (!has_compatible_version) {
            upgrade_control = new wxStaticText(this, wxID_ANY, _L("No compatible plugin"));
            upgrade_control->SetToolTip(
                _L("The repository contains plugin packages, but none target this slicer version."));
        } else if (!plugin.is_installed || plugin.can_upgrade) {
            wxButton *upgrade = new wxButton(
                this, wxID_ANY,
                plugin.is_installed ? format(_L("Upgrade to %1%"), best->package_version) :
                                      format(_L("Install %1%"), best->package_version));
            upgrade->SetToolTip(
                _L("Download this plugin version and install it after restarting the application."));
            bind_repository_action(*upgrade, [this, plugin_id](wxCommandEvent &) {
                install_latest(plugin_id);
            });
            m_repository_action_controls.emplace_back(upgrade);
            upgrade_control = upgrade;
        } else {
            upgrade_control = new wxStaticText(this, wxID_ANY, _L("Up to date"));
        }
    } else if (plugin.sync_state == RepositorySyncState::InProgress) {
        upgrade_control = new wxStaticText(this, wxID_ANY, _L("Synch with github ..."));
    } else if (plugin.sync_state == RepositorySyncState::Failed) {
        wxString label = from_u8(updater_error_short_label(plugin.sync_error));
        if (label.empty())
            label = _L("Synchronization failed");
        upgrade_control = new wxStaticText(this, wxID_ANY, label);
        wxString tooltip = from_u8(format_updater_error(plugin.sync_error));
        if (!plugin.available_packages.empty() && !has_compatible_version) {
            tooltip += "\n\n";
            tooltip += _L("The local cache also contains no plugin package compatible with this slicer version.");
        }
        upgrade_control->SetToolTip(tooltip);
    } else {
        upgrade_control = new wxStaticText(this, wxID_ANY, _L("Unchecked"));
        upgrade_control->SetToolTip(
            _L("This plugin may have a new package available online. Click 'Check for updates' to check."));
    }
    grid.Add(upgrade_control, 0, wxALIGN_CENTER_VERTICAL | wxEXPAND);

    wxString remove_label;
    if (plugin.is_installed)
        remove_label = _L("Uninstall");
    else if (plugin.has_cache)
        remove_label = _L("Clear cache");
    else
        remove_label = _L("Not installed");
    wxButton *remove_button = new wxButton(this, wxID_ANY, remove_label);
    if (plugin.is_installed) {
        remove_button->SetToolTip(
            _L("Schedule this plugin for removal when the application next starts."));
        bind_repository_action(*remove_button, [this, plugin_id](wxCommandEvent &) {
            uninstall(plugin_id);
        });
        m_repository_action_controls.emplace_back(remove_button);
    } else if (plugin.has_cache) {
        remove_button->SetToolTip(
            _L("Remove every cached package and repository file for this plugin."));
        bind_repository_action(*remove_button, [this, plugin_id](wxCommandEvent &) {
            clear_cache(plugin_id);
        });
        m_repository_action_controls.emplace_back(remove_button);
    } else {
        remove_button->Enable(false);
    }
    grid.Add(remove_button, 0, wxALIGN_CENTER_VERTICAL | wxEXPAND);
}

void PluginUpdateDialog::add_repository()
{
    const std::string url = m_repository_url->GetValue().utf8_string();
    if (url.empty())
        return;
    begin_plugin_operation(_L("Adding the plugin repository, please wait"));
    m_updater.download_new_repo(
        url, repository_operation_callback<UpdaterError>(
                 *this, [](PluginUpdateDialog &dialog, UpdaterError error) {
                     dialog.finish_plugin_operation();
                     if (!error.succeeded())
                         wxMessageBox(
                             from_u8(format_updater_error(error)), _L("Plugin updates"), wxICON_ERROR, &dialog);
                     else
                         dialog.m_updater.reload_all_plugins();
                     dialog.rebuild();
                 }));
}

void PluginUpdateDialog::load_package_directory()
{
    wxDirDialog dialog(this, _L("Choose a plugin package folder"));
    if (dialog.ShowModal() != wxID_OK)
        return;

    begin_plugin_operation(_L("Loading the plugin package, please wait"));
    m_updater.cache_plugin_directory(
        boost::filesystem::path(dialog.GetPath().utf8_string()),
        repository_operation_callback<UpdaterError>(
            *this, [](PluginUpdateDialog &dialog, UpdaterError error) {
                dialog.finish_plugin_operation();
                if (!error.succeeded())
                    dialog.show_repository_error(format_updater_error(error));
                dialog.rebuild();
            }));
}

void PluginUpdateDialog::check_updates()
{
    begin_plugin_operation(_L("Checking plugin repositories, please wait"));
    m_updater.sync_async(
        repository_operation_callback<int>(
            *this, [](PluginUpdateDialog &dialog, int) {
                dialog.finish_plugin_operation();
                dialog.rebuild();
            }),
        true);
    rebuild();
}

void PluginUpdateDialog::choose_version(const std::string &plugin_id)
{
    // Changelog failures do not hide otherwise usable packages. The chooser
    // opens after every request has finished and leaves missing notes blank.
    begin_plugin_operation(_L("Loading plugin changelogs, please wait"));
    m_updater.download_changelogs(
        plugin_id,
        repository_operation_callback<bool>(
            *this, [plugin_id](PluginUpdateDialog &dialog, bool) {
                dialog.finish_plugin_operation();
                ChoosePluginVersionDialog chooser(&dialog, dialog.m_updater, plugin_id);
                chooser.ShowModal();
                dialog.rebuild();
            }));
}

void PluginUpdateDialog::install_latest(const std::string &plugin_id)
{
    const std::optional<PluginSync> plugin = m_updater.plugin(plugin_id);
    const PluginAvailable *best = plugin ? plugin->best_available() : nullptr;
    if (best == nullptr)
        return;
    const PluginAvailable version = *best;
    begin_plugin_operation(_L("Downloading the plugin package, please wait"));
    m_updater.install_plugin(
        plugin_id, version,
        repository_operation_callback<UpdaterError>(
            *this, [](PluginUpdateDialog &dialog, UpdaterError error) {
                dialog.finish_plugin_operation();
                if (!error.succeeded())
                    wxMessageBox(
                        from_u8(format_updater_error(error)), _L("Plugin updates"), wxICON_ERROR, &dialog);
                else
                    wxMessageBox(_L("The selected plugin version will be installed after restarting the application."),
                                 _L("Plugin updates"), wxICON_INFORMATION, &dialog);
                dialog.rebuild();
            }));
}

void PluginUpdateDialog::uninstall(const std::string &plugin_id)
{
    const std::optional<PluginSync> plugin = m_updater.plugin(plugin_id);
    if (!plugin || !plugin->is_installed)
        return;
    const wxString display_name = from_u8(
        plugin->description.full_name.empty() ? plugin_id : plugin->description.full_name);
    if (wxMessageBox(
            format(_L("Are you sure you want to uninstall this plugin:\n%1%?"), display_name),
            _L("Uninstall plugin"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this) != wxYES)
        return;

    begin_plugin_operation(_L("Scheduling the plugin removal, please wait"));
    m_updater.uninstall_plugin(
        plugin_id, repository_operation_callback<UpdaterError>(
                       *this, [](PluginUpdateDialog &dialog, UpdaterError error) {
                           dialog.finish_plugin_operation();
                           if (!error.succeeded()) {
                               wxMessageBox(from_u8(format_updater_error(error)),
                                            _L("Plugin updates"),
                                            wxICON_ERROR,
                                            &dialog);
                           } else {
                               wxMessageBox(_L("The plugin will be uninstalled after restarting the application."),
                                            _L("Plugin updates"),
                                            wxICON_INFORMATION,
                                            &dialog);
                           }
                           dialog.rebuild();
                       }));
}

void PluginUpdateDialog::clear_cache(const std::string &plugin_id)
{
    const std::optional<PluginSync> plugin = m_updater.plugin(plugin_id);
    if (!plugin || plugin->is_installed || !plugin->has_cache)
        return;
    const wxString display_name = from_u8(
        plugin->description.full_name.empty() ? plugin_id : plugin->description.full_name);
    if (wxMessageBox(
            format(_L("Are you sure you want to remove all cached files for this plugin:\n%1%?"), display_name),
            _L("Clear plugin cache"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this) != wxYES)
        return;

    begin_plugin_operation(_L("Clearing the plugin cache, please wait"));
    m_updater.clear_cache_plugin(
        plugin_id, repository_operation_callback<UpdaterError>(
                       *this, [](PluginUpdateDialog &dialog, UpdaterError error) {
                           dialog.finish_plugin_operation();
                           if (!error.succeeded())
                               wxMessageBox(
                                   from_u8(format_updater_error(error)), _L("Plugin updates"), wxICON_ERROR, &dialog);
                           dialog.rebuild();
                       }));
}

void PluginUpdateDialog::begin_plugin_operation(const wxString &message)
{
    begin_repository_operation(nullptr, message);
    apply_plugin_operation_state();
}

void PluginUpdateDialog::finish_plugin_operation()
{
    finish_repository_operation(nullptr);
    apply_plugin_operation_state();
}

void PluginUpdateDialog::apply_plugin_operation_state()
{
    const bool enable_actions = !repository_operation_in_progress() &&
                                !m_updater.repository_change_in_progress();
    for (wxWindow *control : m_repository_action_controls)
        control->Enable(enable_actions);
}

ChoosePluginVersionDialog::ChoosePluginVersionDialog(wxWindow *parent,
                                                     PluginUpdater &updater,
                                                     std::string plugin_id)
    : RepositoryUpdatesDialogBase(parent, _L("Choose plugin version"))
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

    const std::optional<PluginSync> plugin = m_updater.plugin(m_plugin_id);
    const std::optional<Semver> current_slicer = Semver::parse(SLIC3R_VERSION_FULL);
    int row = 1;
    if (plugin) {
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
                bind_repository_action(*select, [this, version](wxCommandEvent &) {
                    schedule_version(version);
                });
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
    begin_repository_operation(m_scroll, _L("Downloading the plugin package. Please wait."));
    m_updater.install_plugin(
        m_plugin_id, version,
        repository_operation_callback<UpdaterError>(
            *this, [](ChoosePluginVersionDialog &dialog, UpdaterError error) {
                dialog.finish_repository_operation(dialog.m_scroll);
                if (!error.succeeded()) {
                    wxMessageBox(from_u8(format_updater_error(error)), _L("Plugin updates"), wxICON_ERROR, &dialog);
                    return;
                }
                wxMessageBox(_L("The selected plugin version will be installed after restarting the application."),
                             _L("Plugin updates"), wxICON_INFORMATION, &dialog);
                dialog.EndModal(wxID_OK);
            }));
}

} // namespace Slic3r::GUI
