///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "PluginConfigDialog.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/host/Plugin.hpp"
#include "libslic3r/Plugins/PluginRepository.hpp"
#include "libslic3r/Utils.hpp"

#include "format.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "slic3r/Utils/Process.hpp"

namespace Slic3r::GUI {

namespace {

PluginActivationConfig read_plugin_configuration()
{
    PluginActivationConfig config;
    if (!has_data_dir())
        return config;

    std::string error_message;
    const boost::filesystem::path config_path = plugin_activation_config_path(boost::filesystem::path(data_dir()));
    if (!read_plugin_activation_config(config_path, config, error_message)) {
        BOOST_LOG_TRIVIAL(warning) << error_message;
        return {};
    }
    return config;
}

// Explain why an activated id has no registered Plugin instance. Package
// installation and loading are separate from id activation, so the message
// distinguishes a package that is absent, broken, or simply does not provide
// the configured id.
wxString unavailable_plugin_tooltip(const std::string &plugin_id,
                                    const PluginActivationConfig &config)
{
    const std::map<std::string, std::string>::const_iterator provider =
        config.plugin_packages.find(plugin_id);
    if (provider == config.plugin_packages.end()) {
        return format_wxstr(
            _L("Plugin '%1%' is enabled in the configuration file, but it was not loaded and its package is unknown. "
               "Saving this dialog removes the orphaned id from the active plugin list."),
            from_u8(plugin_id));
    }

    const std::string &package_id = provider->second;
    const PluginPackageLoadReport *report =
        Orchestrator::instance().plugin_package_load_report(package_id);
    if (report != nullptr && !report->issues.empty()) {
        return format_wxstr(
            _L("Plugin '%1%' is enabled, but package '%2%' failed to load correctly. "
               "Open Plugin updates to inspect the package error or choose another version. "
               "Saving this dialog keeps the activation request."),
            from_u8(plugin_id), from_u8(package_id));
    }
    if (report != nullptr && report->state == PluginPackageLoadState::Loaded) {
        return format_wxstr(
            _L("Plugin '%1%' is enabled, but the loaded package '%2%' did not register this id. "
               "Disable the id or install a package version that provides it. "
               "Saving this dialog keeps the activation request."),
            from_u8(plugin_id), from_u8(package_id));
    }
    if (config.installed.find(package_id) == config.installed.end()) {
        return format_wxstr(
            _L("Plugin '%1%' is enabled, but package '%2%' is not installed. "
               "Install the package from Plugin updates or disable this id. "
               "Saving this dialog keeps the activation request."),
            from_u8(plugin_id), from_u8(package_id));
    }
    return format_wxstr(
        _L("Plugin '%1%' is enabled and package '%2%' is selected as installed, but it was not loaded. "
           "Open Plugin updates to repair the package. Saving this dialog keeps the activation request."),
        from_u8(plugin_id), from_u8(package_id));
}

wxString step_name(slicing_step_t step)
{
    switch (step) {
    case STEP_LAYER_HEIGHT:         return "Choose Layer Height";
    case STEP_SLICING:              return "Slice the 3d model";
    case STEP_POST_SLICING:         return "Post-process slices";
    case STEP_PRE_PERIMETER:        return "Prepare perimeter generation";
    case STEP_PERIMETER:            return "Perimeter generation";
    case STEP_POST_PERIMETER:       return "Post-process perimeters";
    case STEP_SURFACE_GENERATION:   return "Generate surfaces";
    case STEP_SKIRT_BRIM:           return "Skirt and brim";
    case STEP_PRE_INFILL:           return "Prepare filling";
    case STEP_INFILL:               return "Fill surfaces";
    case STEP_POST_INFILL:          return "Post-process infill";
    case STEP_SUPPORT_DEMAND:       return "Detect support areas";
    case STEP_SUPPORT:              return "Create support extrusions";
    case STEP_PRE_GCODE:            return "Prepare gcode creation";
    case STEP_CHECK_CONFLICT:       return "Check extrusions conflicts";
    case STEP_ORDERING:             return "Ordering iland extrusions";
    case STEP_WIPETOWER:            return "Create wipetower";
    case STEP_SUPPORT_SPOT:         return "Detect curling areas";
    case STEP_LAYER_EXTRUSION_EDIT: return "Edit extrusions";
    case STEP_EXTRUSION_SIMPLIFICATION: return "Create arcs";
    case STEP_GCODE:                return "Create output file";
    case STEP_NONE:                 return "Nothing";
    case STEP_ANY:                  return "Many steps";
    case BRIDGE_DETECTOR:           return "Detect bridges areas";
    case INFILL_PATTERN:            return "Fill a surface";
    default:                        return wxString::Format("STEP_%u", unsigned(step));
    }
}

} // namespace

PluginConfigDialog::PluginConfigDialog(wxWindow *parent)
    : DPIDialog(parent, wxID_ANY, _L("Plugin configuration"), wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER, "plugin_config")
{
    build();
}

void PluginConfigDialog::build()
{
    const PluginActivationConfig activation_config = read_plugin_configuration();
    m_plugin_packages = activation_config.plugin_packages;
    for (const auto &[plugin_id, enabled] : activation_config.activated)
        if (enabled)
            m_original_active_plugin_ids.insert(plugin_id);

    wxBoxSizer *main_sizer = new wxBoxSizer(wxVERTICAL);

    wxStaticText *description = new wxStaticText(
        this, wxID_ANY,
        _L("Choose which loaded plugins will be active after the next restart."));
    main_sizer->Add(description, 0, wxEXPAND | wxALL, 10);

    wxScrolledWindow *scrolled = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition,
                                                      wxSize(70 * em_unit(), 24 * em_unit()),
                                                      wxVSCROLL);
    scrolled->SetScrollRate(0, em_unit());

    wxFlexGridSizer *grid = new wxFlexGridSizer(4, 8, 12);
    grid->AddGrowableCol(1, 1);

    grid->Add(new wxStaticText(scrolled, wxID_ANY, _L("Active")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(new wxStaticText(scrolled, wxID_ANY, _L("Plugin")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(new wxStaticText(scrolled, wxID_ANY, _L("Step")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(new wxStaticText(scrolled, wxID_ANY, _L("Priority")), 0, wxALIGN_CENTER_VERTICAL);

    std::vector<Plugin *> plugins = Orchestrator::instance().registered_plugins();
    std::stable_sort(plugins.begin(), plugins.end(), [](const Plugin *lhs, const Plugin *rhs) {
        if (lhs->get_step() != rhs->get_step())
            return lhs->get_step() < rhs->get_step();
        return lhs->get_priority() < rhs->get_priority();
    });

    std::set<std::string> loaded_plugin_ids;
    for (Plugin *plugin : plugins) {
        loaded_plugin_ids.insert(plugin->get_id());
        const wxString plugin_tooltip = plugin->get_description().empty() ?
            from_u8(plugin->get_id()) :
            I18N::translate_in_domain(plugin->get_description(), plugin->get_translation_domain());

        wxCheckBox *checkbox = new wxCheckBox(scrolled, wxID_ANY, wxEmptyString);
        checkbox->SetValue(Orchestrator::instance().is_plugin_active(plugin) ||
                           m_original_active_plugin_ids.find(plugin->get_id()) != m_original_active_plugin_ids.end());
        checkbox->SetToolTip(plugin_tooltip);

        wxStaticText *name_label = new wxStaticText(
            scrolled, wxID_ANY, I18N::translate_in_domain(plugin->get_name(), plugin->get_translation_domain()));
        wxStaticText *step_label = new wxStaticText(scrolled, wxID_ANY, step_name(plugin->get_step()));
        wxStaticText *priority_label = new wxStaticText(scrolled, wxID_ANY, wxString::Format("%d", plugin->get_priority()));
        name_label->SetToolTip(plugin_tooltip);
        step_label->SetToolTip(plugin_tooltip);
        priority_label->SetToolTip(plugin_tooltip);

        grid->Add(checkbox, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(name_label, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(step_label, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(priority_label, 0, wxALIGN_CENTER_VERTICAL);

        m_rows.push_back({ plugin->get_id(), checkbox, false });
    }

    for (const std::string &plugin_id : m_original_active_plugin_ids) {
        if (loaded_plugin_ids.find(plugin_id) != loaded_plugin_ids.end())
            continue;

        // Keep unavailable configured ids visible even though there is no
        // Plugin instance from which to build a normal row. Known package
        // associations are preserved so repairing or installing the package
        // can satisfy the same activation request after restart.
        const std::map<std::string, std::string>::const_iterator provider = m_plugin_packages.find(plugin_id);
        const bool provider_known = provider != m_plugin_packages.end();
        const wxString plugin_tooltip = unavailable_plugin_tooltip(plugin_id, activation_config);

        wxCheckBox *checkbox = new wxCheckBox(scrolled, wxID_ANY, wxEmptyString);
        checkbox->SetValue(false);
        checkbox->Enable(false);
        checkbox->SetToolTip(plugin_tooltip);

        wxStaticText *name_label = new wxStaticText(scrolled, wxID_ANY,
                                                    format_wxstr(_L("%1% (not loaded)"), from_u8(plugin_id)));
        wxStaticText *step_label = new wxStaticText(scrolled, wxID_ANY, _L("Not loaded"));
        wxStaticText *priority_label = new wxStaticText(scrolled, wxID_ANY, wxEmptyString);
        name_label->SetToolTip(plugin_tooltip);
        step_label->SetToolTip(plugin_tooltip);
        priority_label->SetToolTip(plugin_tooltip);

        grid->Add(checkbox, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(name_label, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(step_label, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(priority_label, 0, wxALIGN_CENTER_VERTICAL);

        m_rows.push_back({ plugin_id, checkbox, provider_known });
    }

    if (m_rows.empty()) {
        grid->Add(new wxStaticText(scrolled, wxID_ANY, _L("No plugin is loaded.")), 0, wxALIGN_CENTER_VERTICAL);
        grid->AddSpacer(0);
        grid->AddSpacer(0);
        grid->AddSpacer(0);
    }

    scrolled->SetSizer(grid);
    main_sizer->Add(scrolled, 1, wxEXPAND | wxLEFT | wxRIGHT, 10);

    wxBoxSizer *buttons = new wxBoxSizer(wxHORIZONTAL);
    wxButton *save = new wxButton(this, wxID_OK, _L("Save and restart"));
    wxButton *cancel = new wxButton(this, wxID_CANCEL, _L("Cancel"));
    buttons->AddStretchSpacer();
    buttons->Add(save, 0, wxRIGHT, 6);
    buttons->Add(cancel, 0);
    main_sizer->Add(buttons, 0, wxEXPAND | wxALL, 10);

    save->Bind(wxEVT_BUTTON, &PluginConfigDialog::save_and_restart, this);
    cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });

    SetSizerAndFit(main_sizer);
    CentreOnParent();
}

bool PluginConfigDialog::write_active_plugins(std::string &error_message) const
{
    if (!has_data_dir()) {
        error_message = "The configuration directory is not initialized.";
        return false;
    }

    const boost::filesystem::path config_path = plugin_activation_config_path(boost::filesystem::path(data_dir()));
    try {
        std::set<std::string> active_ids = m_original_active_plugin_ids;
        for (const PluginRow &row : m_rows) {
            if (row.preserve_unavailable_activation)
                continue;
            active_ids.erase(row.id);
            if (row.checkbox != nullptr && row.checkbox->GetValue())
                active_ids.insert(row.id);
        }

        std::vector<std::string> selected_plugin_ids;
        for (const std::string &plugin_id : active_ids)
            if (Orchestrator::instance().get_plugin(plugin_id) != nullptr)
                selected_plugin_ids.push_back(plugin_id);
        if (!Orchestrator::instance().validate_plugin_activation(selected_plugin_ids, error_message))
            return false;

        PluginActivationConfig config;
        if (!read_plugin_activation_config(config_path, config, error_message))
            return false;

        for (const PluginRow &row : m_rows) {
            if (!row.preserve_unavailable_activation) {
                config.activated[row.id] = row.checkbox != nullptr && row.checkbox->GetValue();
                if (row.checkbox == nullptr || !row.checkbox->GetValue())
                    config.plugin_packages.erase(row.id);
            }
            if (row.checkbox != nullptr && row.checkbox->GetValue()) {
                const Plugin *plugin = Orchestrator::instance().get_plugin(row.id);
                if (plugin != nullptr && !plugin->get_package_root().empty())
                    config.plugin_packages[row.id] =
                        boost::filesystem::path(plugin->get_package_root()).filename().string();
            }
        }
        if (!write_plugin_activation_config(config_path, config, error_message))
            return false;
    } catch (const std::exception &error) {
        error_message = error.what();
        return false;
    }

    return true;
}

bool PluginConfigDialog::prepare_restart() const
{
    if (wxGetApp().plater() == nullptr)
        return true;

    const int saved_project = wxGetApp().plater()->save_project_if_dirty(
        format_wxstr(_L("Closing %1%. Current project is modified."), SLIC3R_APP_NAME));
    if (saved_project == wxID_CANCEL)
        return false;

    if (saved_project == wxID_NO && wxGetApp().plater()->is_presets_dirty())
        return wxGetApp().check_and_save_current_preset_changes(
            format_wxstr(_L("Closing %1%"), SLIC3R_APP_NAME),
            format_wxstr(_L("Closing %1% while some presets are modified."), SLIC3R_APP_NAME));

    return true;
}

void PluginConfigDialog::save_and_restart(wxCommandEvent &)
{
    if (!prepare_restart())
        return;

    std::string error_message;
    if (!write_active_plugins(error_message)) {
        show_error(this, error_message);
        return;
    }

    BOOST_LOG_TRIVIAL(info) << "Plugin activation configuration saved to '"
                            << plugin_activation_config_path(boost::filesystem::path(data_dir())).string() << "'. Restarting.";
    EndModal(wxID_OK);
    start_new_slicer(nullptr, false);
    if (wxGetApp().mainframe != nullptr)
        wxGetApp().mainframe->Close(true);
}

void PluginConfigDialog::on_dpi_changed(const wxRect &)
{
    SetFont(wxGetApp().normal_font());
    msw_buttons_rescale(this, em_unit(), { wxID_OK, wxID_CANCEL });
    Fit();
    Refresh();
}

} // namespace Slic3r::GUI
