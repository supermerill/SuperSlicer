///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "PluginConfigDialog.hpp"

#include <algorithm>
#include <ios>
#include <set>
#include <string>
#include <vector>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/host/Plugin.hpp"
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

const char *const PLUGIN_ACTIVATION_DIR = "plugin";
const char *const ACTIVATED_PLUGINS_FILENAME = "activated.ini";

boost::filesystem::path active_plugin_config_path()
{
    return boost::filesystem::path(data_dir()) / PLUGIN_ACTIVATION_DIR / ACTIVATED_PLUGINS_FILENAME;
}

bool ini_value_is_enabled(const std::string &value)
{
    return boost::algorithm::iequals(value, "1") ||
           boost::algorithm::iequals(value, "true") ||
           boost::algorithm::iequals(value, "yes") ||
           boost::algorithm::iequals(value, "on") ||
           boost::algorithm::iequals(value, "enabled");
}

std::set<std::string> read_active_plugin_ids()
{
    std::set<std::string> out;
    if (!has_data_dir())
        return out;

    const boost::filesystem::path config_path = active_plugin_config_path();
    boost::nowide::ifstream stream(config_path.string());
    if (!stream)
        return out;

    try {
        boost::property_tree::ptree tree;
        boost::property_tree::read_ini(stream, tree);
        const boost::property_tree::ptree &const_tree = tree;
        const boost::optional<const boost::property_tree::ptree&> activated =
            const_tree.get_child_optional("activated");
        if (!activated)
            return out;

        for (const boost::property_tree::ptree::value_type &entry : *activated) {
            const std::string plugin_id = boost::algorithm::trim_copy(entry.first);
            if (!plugin_id.empty() && ini_value_is_enabled(entry.second.get_value<std::string>()))
                out.insert(plugin_id);
        }
    } catch (const std::exception &error) {
        BOOST_LOG_TRIVIAL(warning) << "Cannot read active plugin configuration '"
                                   << config_path.string() << "': " << error.what();
    }
    return out;
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
    m_original_active_plugin_ids = read_active_plugin_ids();

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
            from_u8(plugin->get_description());

        wxCheckBox *checkbox = new wxCheckBox(scrolled, wxID_ANY, wxEmptyString);
        checkbox->SetValue(Orchestrator::instance().is_plugin_active(plugin) ||
                           m_original_active_plugin_ids.find(plugin->get_id()) != m_original_active_plugin_ids.end());
        checkbox->SetToolTip(plugin_tooltip);

        wxStaticText *name_label = new wxStaticText(scrolled, wxID_ANY, from_u8(plugin->get_name()));
        wxStaticText *step_label = new wxStaticText(scrolled, wxID_ANY, step_name(plugin->get_step()));
        wxStaticText *priority_label = new wxStaticText(scrolled, wxID_ANY, wxString::Format("%d", plugin->get_priority()));
        name_label->SetToolTip(plugin_tooltip);
        step_label->SetToolTip(plugin_tooltip);
        priority_label->SetToolTip(plugin_tooltip);

        grid->Add(checkbox, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(name_label, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(step_label, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(priority_label, 0, wxALIGN_CENTER_VERTICAL);

        m_rows.push_back({ plugin->get_id(), checkbox });
    }

    for (const std::string &plugin_id : m_original_active_plugin_ids) {
        if (loaded_plugin_ids.find(plugin_id) != loaded_plugin_ids.end())
            continue;

        // activated.ini may name a plugin whose DLL failed to load, for example
        // after an ABI bump. Show it as a disabled, unchecked row so saving the
        // dialog removes the stale id instead of trapping the user behind a
        // validation error for a plugin that cannot be unchecked elsewhere.
        const wxString plugin_tooltip = format_wxstr(
            _L("Plugin '%1%' is enabled in the configuration file, but it was not loaded. "
               "This usually means the plugin file is missing or was built for another plugin API version. "
               "Saving this dialog will remove it from the active plugin list."),
            from_u8(plugin_id));

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

        m_rows.push_back({ plugin_id, checkbox });
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

    const boost::filesystem::path config_path = active_plugin_config_path();
    try {
        std::set<std::string> active_ids = m_original_active_plugin_ids;
        for (const PluginRow &row : m_rows) {
            active_ids.erase(row.id);
            if (row.checkbox != nullptr && row.checkbox->GetValue())
                active_ids.insert(row.id);
        }

        std::vector<std::string> selected_plugin_ids(active_ids.begin(), active_ids.end());
        if (!Orchestrator::instance().validate_plugin_activation(selected_plugin_ids, error_message))
            return false;

        boost::filesystem::create_directories(config_path.parent_path());
        boost::nowide::ofstream stream(config_path.string(), std::ios::out | std::ios::trunc);
        if (!stream) {
            error_message = "Cannot write " + config_path.string();
            return false;
        }

        stream << "[activated]\n";
        stream << "; Plugin ids enabled by the user.\n";
        for (const std::string &plugin_id : active_ids)
            stream << plugin_id << " = 1\n";
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
                            << active_plugin_config_path().string() << "'. Restarting.";
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
