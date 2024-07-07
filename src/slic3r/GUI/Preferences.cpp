#include "Preferences.hpp"
#include "OptionsGroup.hpp"
#include "GUI_App.hpp"
#include "Plater.hpp"
#include "MsgDialog.hpp"
#include "I18N.hpp"
#include "libslic3r/AppConfig.hpp"

#include "Notebook.hpp"
#include "ButtonsDescription.hpp"
#include "OG_CustomCtrl.hpp"
#include "wxExtensions.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/path.hpp>

#include <wx/display.h>
#include <wx/notebook.h>
#include <wx/scrolwin.h>

namespace Slic3r {

    static t_config_enum_names enum_names_from_keys_map(const t_config_enum_values& enum_keys_map)
    {
        t_config_enum_names names;
        int cnt = 0;
        for (const auto& kvp : enum_keys_map)
            cnt = std::max(cnt, kvp.second);
        cnt += 1;
        names.assign(cnt, "");
        for (const auto& kvp : enum_keys_map)
            names[kvp.second] = kvp.first;
        return names;
    }

#define CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(NAME) \
    static t_config_enum_names s_keys_names_##NAME = enum_names_from_keys_map(s_keys_map_##NAME); \
    template<> const t_config_enum_values& ConfigOptionEnum<NAME>::get_enum_values() { return s_keys_map_##NAME; } \
    template<> const t_config_enum_names& ConfigOptionEnum<NAME>::get_enum_names() { return s_keys_names_##NAME; }



    static const t_config_enum_values s_keys_map_NotifyReleaseMode = {
        {"all",         NotifyReleaseAll},
        {"release",     NotifyReleaseOnly},
        {"none",        NotifyReleaseNone},
    };

    CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(NotifyReleaseMode)

namespace GUI {

PreferencesDialog::PreferencesDialog(wxWindow* parent, int selected_tab, const std::string& highlight_opt_key) :
    DPIDialog(parent, wxID_ANY, _L("Preferences"), wxDefaultPosition,
              wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER )
{
#ifdef __WXOSX__
    isOSX = true;
#endif
    build(selected_tab);
    if (!highlight_opt_key.empty())
        init_highlighter(highlight_opt_key);
}
static std::shared_ptr<ConfigOptionsGroup>create_options_tab(const wxString& title, wxBookCtrlBase* tabs)
{
    //set inside a scrollable panel
    wxScrolledWindow *tab = new wxScrolledWindow(tabs, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                                 wxBK_LEFT | wxTAB_TRAVERSAL | wxVSCROLL);
    tabs->AddPage(tab, title);
    tab->SetFont(wxGetApp().normal_font());

    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->SetSizeHints(tab);
    tab->SetSizer(sizer);
    tab->SetScrollRate(0, 5);

    std::shared_ptr<ConfigOptionsGroup> optgroup = std::make_shared<ConfigOptionsGroup>(tab);
    optgroup->title_width = 40;
    optgroup->label_width = 40;
    return optgroup;
}


std::shared_ptr<ConfigOptionsGroup> PreferencesDialog::create_options_group(const wxString& title, wxBookCtrlBase* tabs, int page_idx)
{

    std::shared_ptr<ConfigOptionsGroup> optgroup = std::make_shared<ConfigOptionsGroup>((wxPanel*)tabs->GetPage(page_idx), title);
    optgroup->title_width = 40;
    optgroup->label_width = 40;
    optgroup->m_on_change = [this, tabs, optgroup](t_config_option_key opt_key, boost::any value) {
        Field* field = optgroup->get_field(opt_key);
        //very special cases
        if (opt_key == "default_action_on_close_application" || opt_key == "default_action_on_select_preset" || opt_key == "default_action_on_new_project")
            m_values[opt_key] = boost::any_cast<bool>(value) ? "none" : "discard";
        else if (opt_key == "default_action_on_dirty_project")
            m_values[opt_key] = boost::any_cast<bool>(value) ? "" : "0";
        else if (opt_key == "suppress_hyperlinks")
            m_values[opt_key] = boost::any_cast<bool>(value) ? "1" : "";
        else if ("ui_layout" == opt_key) {
            std::vector<std::string> splitted;
            boost::split(splitted, boost::any_cast<std::string>(value), boost::is_any_of(":"));
            m_values[opt_key] = splitted[0];
        } else if (field) {
            //common cases
            if (field->m_opt.type == coBool) {
                m_values[opt_key] = boost::any_cast<bool>(value) ? "1" : "0";
            } else if (field->m_opt.type == coInt) {
                m_values[opt_key] = std::to_string(boost::any_cast<int>(value));
            } else if (field->m_opt.type == coString && field->m_opt.gui_type == ConfigOptionDef::GUIType::color) {
                std::string str_color = boost::any_cast<std::string>(value);
                if (str_color.size() >= 6 && str_color.size() <= 7) {
                    m_values[opt_key] = str_color[0] == '#' ? str_color.substr(1) : str_color;
                }
            } else if (field->m_opt.type == coString || field->m_opt.type == coStrings) {
                m_values[opt_key] = boost::any_cast<std::string>(value);
            } else if (field->m_opt.type == coEnum) {
                int val_int = boost::any_cast<int>(value);
                for (const auto& item : *field->m_opt.enum_keys_map) {
                    if (item.second == val_int) {
                        m_values[opt_key] = item.first;
                        break;
                    }
                }
            }
        } else {
            assert(false);
            m_values[opt_key] = boost::any_cast<bool>(value) ? "1" : "0";
        }

        if (opt_key == "use_custom_toolbar_size") {
            m_icon_size_sizer->ShowItems(boost::any_cast<bool>(value));
            m_optgroups_gui.front()->parent()->Layout();
            tabs->Layout();
            this->layout();
        }
    };
    return optgroup;
}

static void activate_options_tab(std::shared_ptr<ConfigOptionsGroup> optgroup, int padding = 20)
{
    optgroup->activate([](){}, wxALIGN_RIGHT);
    optgroup->update_visibility(comSimple);
    wxBoxSizer* sizer = static_cast<wxBoxSizer*>(static_cast<wxPanel*>(optgroup->parent())->GetSizer());
    sizer->Add(optgroup->sizer, 0, wxEXPAND | wxALL, padding);
}

void PreferencesDialog::build(size_t selected_tab)
{
#ifdef _WIN32
    wxGetApp().UpdateDarkUI(this);
#else
    SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
#endif
    const wxFont& font = wxGetApp().normal_font();
    SetFont(font);

    auto app_config = get_app_config();

#ifdef _MSW_DARK_MODE
    wxBookCtrlBase* tabs;
//    if (wxGetApp().dark_mode())
        tabs = new Notebook(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxNB_TOP | wxTAB_TRAVERSAL | wxNB_NOPAGETHEME | wxNB_DEFAULT);
/*    else {
        tabs = new wxNotebook(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxNB_TOP | wxTAB_TRAVERSAL | wxNB_NOPAGETHEME | wxNB_DEFAULT);
        tabs->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
    }*/
#else
    wxNotebook* tabs = new wxNotebook(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxNB_TOP | wxTAB_TRAVERSAL  |wxNB_NOPAGETHEME | wxNB_DEFAULT );
    tabs->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
#endif

    // Add "General" tab
    m_optgroups_general.clear();
    m_optgroups_general.emplace_back(create_options_tab(_L("General"), tabs));


    bool is_editor = wxGetApp().is_editor();

    ConfigOptionDef def;
    Option option(def, "");

    if (is_editor) {

        //activate_options_tab(m_optgroups_general.back(), 3);
        m_optgroups_general.emplace_back(create_options_group(_L("Automation"), tabs, 0));

        def.label = L("Auto-center parts");
        def.type = coBool;
        def.tooltip = L("If this is enabled, Slic3r will auto-center objects "
            "around the print bed center.");
        def.set_default_value(new ConfigOptionBool{ app_config->get("autocenter") == "1" });
        option = Option(def, "autocenter");
        m_optgroups_general.back()->append_single_option_line(option);

        def.label = L("Background processing");
        def.type = coBool;
        def.tooltip = L("If this is enabled, Slic3r will pre-process objects as soon "
            "as they\'re loaded in order to save time when exporting G-code.");
        def.set_default_value(new ConfigOptionBool{ app_config->get("background_processing") == "1" });
        option = Option(def, "background_processing");
        m_optgroups_general.back()->append_single_option_line(option);

        //FIXME change it to enum, like the NotifyReleaseMode
        def_combobox_auto_switch_preview.label = L("Switch to Preview when sliced");
        def_combobox_auto_switch_preview.type = coStrings;
        def_combobox_auto_switch_preview.tooltip = L("When an object is sliced, it will switch your view from the curent view to the "
            "preview (and then gcode-preview) automatically, depending on the option choosen.");
        def_combobox_auto_switch_preview.gui_type = ConfigOptionDef::GUIType::f_enum_open;
        def_combobox_auto_switch_preview.gui_flags = "show_value";
        def_combobox_auto_switch_preview.enum_values.push_back(_u8L("Don't switch"));
        def_combobox_auto_switch_preview.enum_values.push_back(_u8L("Switch when possible"));
        def_combobox_auto_switch_preview.enum_values.push_back(_u8L("Only if on platter"));
        def_combobox_auto_switch_preview.enum_values.push_back(_u8L("Only when GCode is ready"));
        if (app_config->get("auto_switch_preview") == "0")
            def_combobox_auto_switch_preview.set_default_value(new ConfigOptionStrings{ def_combobox_auto_switch_preview.enum_values[0] });
        else if (app_config->get("auto_switch_preview") == "1")
            def_combobox_auto_switch_preview.set_default_value(new ConfigOptionStrings{ def_combobox_auto_switch_preview.enum_values[1] });
        else if (app_config->get("auto_switch_preview") == "2")
            def_combobox_auto_switch_preview.set_default_value(new ConfigOptionStrings{ def_combobox_auto_switch_preview.enum_values[2] });
        else if (app_config->get("auto_switch_preview") == "3")
            def_combobox_auto_switch_preview.set_default_value(new ConfigOptionStrings{ def_combobox_auto_switch_preview.enum_values[3] });
        else
            def_combobox_auto_switch_preview.set_default_value(new ConfigOptionStrings{ def_combobox_auto_switch_preview.enum_values[2] });
        option = Option(def_combobox_auto_switch_preview, "auto_switch_preview");
        m_optgroups_general.back()->append_single_option_line(option);


        activate_options_tab(m_optgroups_general.back(), 3);
        m_optgroups_general.emplace_back(create_options_group(_L("Presets and updates"), tabs, 0));
/*
        // Please keep in sync with ConfigWizard
        def.label = L("Check for application updates");
        def.type = coBool;
        def.tooltip = L("If enabled, Slic3r will check for the new versions of itself online. When a new version becomes available a notification is displayed at the next application startup (never during program usage). This is only a notification mechanisms, no automatic installation is done.");
        def.set_default_value(new ConfigOptionBool(app_config->get("version_check") == "1"));
            option = Option(def, "version_check");
        m_optgroups_general.back()->append_single_option_line(option);
*/
        // Please keep in sync with ConfigWizard
        def.label = L("Update built-in Presets automatically");
        def.type = coBool;
        def.tooltip = L("If enabled, Slic3r downloads updates of built-in system presets in the background. These updates are downloaded into a separate temporary location. When a new preset version becomes available it is offered at application startup.");
        def.set_default_value(new ConfigOptionBool(app_config->get("preset_update") == "1"));
        option = Option(def, "preset_update");
        m_optgroups_general.back()->append_single_option_line(option);

        def.label = L("Suppress \" - default - \" presets");
        def.type = coBool;
        def.tooltip = L("Suppress \" - default - \" presets in the Print / Filament / Printer "
            "selections once there are any other valid presets available.");
        def.set_default_value(new ConfigOptionBool{ app_config->get("no_defaults") == "1" });
        option = Option(def, "no_defaults");
        m_optgroups_general.back()->append_single_option_line(option);
        m_values_need_restart.push_back("no_defaults");

        def.label = L("Show incompatible print and filament presets");
        def.type = coBool;
        def.tooltip = L("When checked, the print and filament presets are shown in the preset editor "
            "even if they are marked as incompatible with the active printer");
        def.set_default_value(new ConfigOptionBool{ app_config->get("show_incompatible_presets") == "1" });
        option = Option(def, "show_incompatible_presets");
        m_optgroups_general.back()->append_single_option_line(option);

        def.label = L("Main GUI always in expert mode");
        def.type = coBool;
        def.tooltip = L("If enabled, the gui will be in expert mode even if the simple or advanced mode is selected (but not the setting tabs).");
        def.set_default_value(new ConfigOptionBool{ app_config->get("objects_always_expert") == "1" });
        option = Option(def, "objects_always_expert");
        m_optgroups_general.back()->append_single_option_line(option);

        activate_options_tab(m_optgroups_general.back(), 3);
        m_optgroups_general.emplace_back(create_options_group(_L("Files"), tabs, 0));

        // Please keep in sync with ConfigWizard
        def.label = L("Export sources full pathnames to 3mf and amf");
        def.type = coBool;
        def.tooltip = L("If enabled, allows the Reload from disk command to automatically find and load the files when invoked.");
        def.set_default_value(new ConfigOptionBool(app_config->get("export_sources_full_pathnames") == "1"));
        option = Option(def, "export_sources_full_pathnames");
        m_optgroups_general.back()->append_single_option_line(option);

#ifdef _WIN32
        // Please keep in sync with ConfigWizard
        def.label = (boost::format(_u8L("Associate .3mf files to %1%")) % SLIC3R_APP_NAME).str();
        def.type = coBool;
        def.tooltip = L("If enabled, sets Slic3r as default application to open .3mf files.");
        def.set_default_value(new ConfigOptionBool(app_config->get("associate_3mf") == "1"));
        option = Option(def, "associate_3mf");
        m_optgroups_general.back()->append_single_option_line(option);

        def.label = (boost::format(_u8L("Associate .stl files to %1%")) % SLIC3R_APP_NAME).str();
        def.type = coBool;
        def.tooltip = L("If enabled, sets Slic3r as default application to open .stl files.");
        def.set_default_value(new ConfigOptionBool(app_config->get("associate_stl") == "1"));
        option = Option(def, "associate_stl");
        m_optgroups_general.back()->append_single_option_line(option);
#endif // _WIN32

        def.label = L("Remember output directory");
        def.type = coBool;
        def.tooltip = L("If this is enabled, Slic3r will prompt the last output directory "
            "instead of the one containing the input files.");
        def.set_default_value(new ConfigOptionBool{ app_config->has("remember_output_path") ? app_config->get("remember_output_path") == "1" : true });
        option = Option(def, "remember_output_path");
        m_optgroups_general.back()->append_single_option_line(option);

        def.label = L("Export headers with date and time");
        def.type = coBool;
        def.tooltip = L("If this is enabled, Slic3r will add the date of the export to the first line of any exported config and gcode file. Note that some software may rely on that to work, be careful and report any problem if you deactivate it.");
        def.set_default_value(new ConfigOptionBool{ app_config->has("date_in_config_file") ? app_config->get("date_in_config_file") == "1" : true });
        option = Option(def, "date_in_config_file");
        m_optgroups_general.back()->append_single_option_line(option);

        def.label = L("Show a Pop-up with the current material when exporting");
        def.type = coBool;
        def.tooltip = L("If you constantly forgot to select the right filament/materail, check this option to have a really obtrusive reminder on each export.");
        def.set_default_value(new ConfigOptionBool{ app_config->has("check_material_export") ? app_config->get("check_material_export") == "1" : false });
        option = Option(def, "check_material_export");
        m_optgroups_general.back()->append_single_option_line(option);

        def.label = L("Show ignored settings when loading a project or configuration");
        def.type = coBool;
        def.tooltip = L("When loading a configuration, if it's coming from an earlier, a future or from another software, show the ignored settings that deosn't suit this version. Uncheck to remove this anoying pop-up.");
        def.set_default_value(new ConfigOptionBool{ app_config->has("show_unknown_setting") ? app_config->get("show_unknown_setting") == "1" : false });
        option = Option(def, "show_unknown_setting");
        m_optgroups_general.back()->append_single_option_line(option);

        activate_options_tab(m_optgroups_general.back(), 3);
        m_optgroups_general.emplace_back(create_options_group(_L("Dialogs"), tabs, 0));

        def.label = L("Show drop project dialog");
        def.type = coBool;
        def.tooltip = L("When checked, whenever dragging and dropping a project file on the application, shows a dialog asking to select the action to take on the file to load.");
        def.set_default_value(new ConfigOptionBool{ app_config->get("show_drop_project_dialog") == "1" });
        option = Option(def, "show_drop_project_dialog");
        m_optgroups_general.back()->append_single_option_line(option);

        def.label = L("Show overwrite dialog.");
        def.type = coBool;
        def.tooltip = L("If this is enabled, Slic3r will prompt for when overwriting files from save dialogs.");
        def.set_default_value(new ConfigOptionBool{ app_config->has("show_overwrite_dialog") ? app_config->get("show_overwrite_dialog") == "1" : true });
        option = Option(def, "show_overwrite_dialog");
        m_optgroups_general.back()->append_single_option_line(option);


#if __APPLE__
        def.label = (boost::format(_u8L("Allow just a single %1% instance")) % SLIC3R_APP_NAME).str();
        def.type = coBool;
    def.tooltip = L("On OSX there is always only one instance of app running by default. However it is allowed to run multiple instances of same app from the command line. In such case this settings will allow only one instance.");
#else
        def.label = (boost::format(_u8L("Allow just a single %1% instance")) % SLIC3R_APP_NAME).str();
        def.type = coBool;
        def.tooltip = L("If this is enabled, when starting Slic3r and another instance of the same Slic3r is already running, that instance will be reactivated instead.");
#endif
        def.set_default_value(new ConfigOptionBool{ app_config->has("single_instance") ? app_config->get("single_instance") == "1" : false });
        option = Option(def, "single_instance");
        m_optgroups_general.back()->append_single_option_line(option);


        def.label = L("Ask for unsaved changes in project");
        def.type = coBool;
        def.tooltip = L("Always ask for unsaved changes in project, when: \n"
                        "- Closing Slic3r,\n"
                        "- Loading or creating a new project");
        def.set_default_value(new ConfigOptionBool{ app_config->get("default_action_on_dirty_project").empty() });
        option = Option(def, "default_action_on_dirty_project");
        m_optgroups_general.back()->append_single_option_line(option);

        def.label = L("Ask to save unsaved changes in presets when closing the application or when loading a new project");
        def.type = coBool;
        def.tooltip = L("Always ask for unsaved changes in presets, when: \n"
                        "- Closing Slic3r while some presets are modified,\n"
                        "- Loading a new project while some presets are modified");
        def.set_default_value(new ConfigOptionBool{ app_config->get("default_action_on_close_application") == "none" });
        option = Option(def, "default_action_on_close_application");
        m_optgroups_general.back()->append_single_option_line(option);

        def.label = L("Ask for unsaved changes in presets when selecting new preset");
        def.type = coBool;
        def.tooltip = L("Always ask for unsaved changes in presets when selecting new preset or resetting a preset");
        def.set_default_value(new ConfigOptionBool{ app_config->get("default_action_on_select_preset") == "none" });
        option = Option(def, "default_action_on_select_preset");
        m_optgroups_general.back()->append_single_option_line(option);

        def.label = L("Ask for unsaved changes in presets when creating new project");
        def.type = coBool;
        def.tooltip = L("Always ask for unsaved changes in presets when creating new project");
        def.set_default_value(new ConfigOptionBool{ app_config->get("default_action_on_new_project") == "none" });
        option = Option(def, "default_action_on_new_project");
        m_optgroups_general.back()->append_single_option_line(option);

        def.label = L("Ask for 'new project' on 'Delete all'");
        def.type = coBool;
        def.tooltip = L("When you click on the garbage can (or ctrl+del), ask for the action to do. If disable, it will erase all object without asking");
        def.set_default_value(new ConfigOptionBool{ app_config->get("default_action_delete_all") == "1" });
        option = Option(def, "default_action_delete_all");
        m_optgroups_general.back()->append_single_option_line(option);

        // Clear Undo / Redo stack on new project
        def.label = L("Clear Undo / Redo stack on new project");
        def.type = coBool;
        def.tooltip = L("Clear Undo / Redo stack on new project or when an existing project is loaded.");
        def.set_default_value(new ConfigOptionBool{ app_config->get("clear_undo_redo_stack_on_new_project") == "1" });
        option = Option(def, "clear_undo_redo_stack_on_new_project");
        m_optgroups_general.back()->append_single_option_line(option);
    }
#ifdef _WIN32
    else {
        def.label = (boost::format(_u8L("Associate .gcode files to %1%")) % GCODEVIEWER_APP_NAME).str();
        def.type = coBool;
        def.tooltip = (boost::format(_u8L("If enabled, sets %1% as default application to open .gcode files.")) % GCODEVIEWER_APP_NAME).str();
        def.set_default_value(new ConfigOptionBool(app_config->get("associate_gcode") == "1"));
        option = Option(def, "associate_gcode");
        m_optgroups_general.back()->append_single_option_line(option);
    }
#endif // _WIN32

#if __APPLE__
    def.label = L("Use Retina resolution for the 3D scene");
    def.type = coBool;
    def.tooltip = L("If enabled, the 3D scene will be rendered in Retina resolution. "
                    "If you are experiencing 3D performance problems, disabling this option may help.");
    def.set_default_value(new ConfigOptionBool{ app_config->get("use_retina_opengl") == "1" });
    option = Option (def, "use_retina_opengl");
    m_optgroups_general.back()->append_single_option_line(option);
#endif

    if (is_editor) {
        activate_options_tab(m_optgroups_general.back(), 3);
    }


    if (is_editor) {
        m_optgroups_general.emplace_back(create_options_group(_L("Paths"), tabs, 0));
        m_optgroups_general.back()->title_width = 20;
        m_optgroups_general.back()->label_width = 20;

        def.label = L("FreeCAD path");
        def.type = coString;
        def.tooltip = L("If it point to a valid freecad instance, you can use the built-in python script to quickly generate geometry."
            "\nPut here the freecad directory from which you can access its 'lib' directory."
            "\nFreecad will use its own python (from the bin directoyr) on windows and will use the system python3 on linux & macos");
        def.set_default_value(new ConfigOptionString{ app_config->get("freecad_path") });
        option = Option(def, "freecad_path");
        //option.opt.full_width = true;
        option.opt.width = 50;
        m_optgroups_general.back()->append_single_option_line(option);
    }

    activate_options_tab(m_optgroups_general.back(), m_optgroups_general.back()->parent()->GetSizer()->GetItemCount() > 1 ? 3 : 20);

    // Add "Camera" tab
    m_optgroup_camera = create_options_tab(_L("Camera"), tabs);
    m_optgroup_camera->m_on_change = [this](t_config_option_key opt_key, boost::any value) {
        m_values[opt_key] = boost::any_cast<bool>(value) ? "1" : "0";
    };

    def.label = L("Use perspective camera");
    def.type = coBool;
    def.tooltip = L("If enabled, use perspective camera. If not enabled, use orthographic camera.");
    def.set_default_value(new ConfigOptionBool{ app_config->get("use_perspective_camera") == "1" });
    option = Option(def, "use_perspective_camera");
    m_optgroup_camera->append_single_option_line(option);

    def.label = L("Use free camera");
    def.type = coBool;
    def.tooltip = L("If enabled, use free camera. If not enabled, use constrained camera.");
    def.set_default_value(new ConfigOptionBool(app_config->get("use_free_camera") == "1"));
    option = Option(def, "use_free_camera");
    m_optgroup_camera->append_single_option_line(option);

    def.label = L("Reverse direction of zoom with mouse wheel");
    def.type = coBool;
    def.tooltip = L("If enabled, reverses the direction of zoom with mouse wheel");
    def.set_default_value(new ConfigOptionBool(app_config->get("reverse_mouse_wheel_zoom") == "1"));
    option = Option(def, "reverse_mouse_wheel_zoom");
    m_optgroup_camera->append_single_option_line(option);


#if defined(_WIN32) || defined(__APPLE__)
    //m_optgroup_camera->append_separator();

    def.label = L("Enable support for legacy 3DConnexion devices");
    def.type = coBool;
    def.tooltip = L("If enabled, the legacy 3DConnexion devices settings dialog is available by pressing CTRL+M");
    def.set_default_value(new ConfigOptionBool{ app_config->get("use_legacy_3DConnexion") == "1" });
    option = Option(def, "use_legacy_3DConnexion");
    m_optgroup_camera->append_single_option_line(option);
#endif // _WIN32 || __APPLE__

    def.label = L("Compress png textures");
    def.type = coBool;
    def.tooltip = L("If your custom texture (in png format) is displayed black, then disable this option to remove the problematic optimisation.");
    def.set_default_value(new ConfigOptionBool{ app_config->get("compress_png_texture") == "1" });
    option = Option(def, "compress_png_texture");
    m_optgroup_camera->append_single_option_line(option);

    activate_options_tab(m_optgroup_camera);

    // Add "GUI" tab
    m_optgroups_gui.clear();
    m_optgroups_gui.emplace_back(create_options_tab(_L("GUI"), tabs));

        //activate_options_tab(m_optgroups_general.back(), 3);
    m_optgroups_gui.emplace_back(create_options_group(_L("Controls"), tabs, 2));


    def.label = L("Sequential slider applied only to top layer");
    def.type = coBool;
    def.tooltip = L("If enabled, changes made using the sequential slider, in preview, apply only to gcode top layer. "
                    "If disabled, changes made using the sequential slider, in preview, apply to the whole gcode.");
    def.set_default_value(new ConfigOptionBool{ app_config->get("seq_top_layer_only") == "1" });
    option = Option(def, "seq_top_layer_only");
    m_optgroups_gui.back()->append_single_option_line(option);

    if (is_editor) {
        def.label = L("Show sidebar collapse/expand button");
        def.type = coBool;
        def.tooltip = L("If enabled, the button for the collapse sidebar will be appeared in top right corner of the 3D Scene");
        def.set_default_value(new ConfigOptionBool{ app_config->get("show_collapse_button") == "1" });
        option = Option(def, "show_collapse_button");
        m_optgroups_gui.back()->append_single_option_line(option);

        def.label = L("Suppress to open hyperlink in browser");
        def.type = coBool;
        def.tooltip = (boost::format(_u8L("If enabled, %1% will not open hyperlinks in your browser.")) % SLIC3R_APP_NAME).str();
        //def.tooltip = ("If enabled, the descriptions of configuration parameters in settings tabs wouldn't work as hyperlinks. "
        //    "If disabled, the descriptions of configuration parameters in settings tabs will work as hyperlinks.");
        def.set_default_value(new ConfigOptionBool{ app_config->get("suppress_hyperlinks") == "1" });
        option = Option(def, "suppress_hyperlinks");
        m_optgroups_gui.back()->append_single_option_line(option);

        def.label = L("Focusing platter on mouse over");
        def.type = coBool;
        def.tooltip = L("If disabled, moving the mouse over the platter panel will not change the focus but some shortcuts from the platter may not work. "
            "If enabled, moving the mouse over the platter panel will move focus there, and away from the current control.");
        def.set_default_value(new ConfigOptionBool{ app_config->get("focus_platter_on_mouse") == "1" });
        option = Option(def, "focus_platter_on_mouse");
        m_optgroups_gui.back()->append_single_option_line(option);

        activate_options_tab(m_optgroups_gui.back(), 3);
        m_optgroups_gui.emplace_back(create_options_group(_L("Appearance"), tabs, 2));

        def.label = L("Use colors for axes values in Manipulation panel");
        def.type = coBool;
        def.tooltip = L("If enabled, the axes names and axes values will be colorized according to the axes colors. "
                        "If disabled, old UI will be used.");
        def.set_default_value(new ConfigOptionBool{ app_config->get("color_manipulation_panel") == "1" });
        option = Option(def, "color_manipulation_panel");
        m_optgroups_gui.back()->append_single_option_line(option);

        def.label = L("Order object volumes by types");
        def.type = coBool;
        def.tooltip = L("If enabled, volumes will be always ordered inside the object. Correct order is Model Part, Negative Volume, Modifier, Support Blocker and Support Enforcer. "
                        "If disabled, you can reorder Model Parts, Negative Volumes and Modifiers. But one of the model parts have to be on the first place.");
        def.set_default_value(new ConfigOptionBool{ app_config->get("order_volumes") == "1" });
        option = Option(def, "order_volumes");
        m_optgroups_gui.back()->append_single_option_line(option);

#ifdef _USE_CUSTOM_NOTEBOOK
        def.label = L("Set settings tabs as menu items (experimental)");
        def.type = coBool;
        def.tooltip = L("If enabled, Settings Tabs will be placed as menu items. "
                        "If disabled, old UI will be used.");
        def.set_default_value(new ConfigOptionBool{ app_config->get("tabs_as_menu") == "1" });
        option = Option(def, "tabs_as_menu");
        m_optgroups_gui.back()->append_single_option_line(option);
        m_values_need_restart.push_back("tabs_as_menu");
#endif

        m_optgroups_gui.back()->append_separator();

        def.label = L("Show \"Tip of the day\" notification after start");
        def.type = coBool;
        def.tooltip = L("If enabled, useful hints are displayed at startup.");
        def.set_default_value(new ConfigOptionBool{ app_config->get("show_hints") == "1" });
        option = Option(def, "show_hints");
        m_optgroups_gui.back()->append_single_option_line(option);

        ConfigOptionDef def_enum;
        def_enum.label = L("Notify about new releases");
        def_enum.type = coEnum;
        def_enum.tooltip = L("You will be notified about new release after startup acordingly: All = Regular release and alpha / beta releases. Release only = regular release.");
        def_enum.enum_keys_map = &ConfigOptionEnum<NotifyReleaseMode>::get_enum_values();
        def_enum.enum_values.push_back("all");
        def_enum.enum_values.push_back("release");
        def_enum.enum_values.push_back("none");
        def_enum.enum_labels.push_back(L("All"));
        def_enum.enum_labels.push_back(L("Release only"));
        def_enum.enum_labels.push_back(L("None"));
        //def_enum.mode = comSimple;
        def_enum.set_default_value(new ConfigOptionEnum<NotifyReleaseMode>(static_cast<NotifyReleaseMode>(s_keys_map_NotifyReleaseMode.at(app_config->get("notify_release")))));
        option = Option(def_enum, "notify_release");
        m_optgroups_gui.back()->append_single_option_line(option);

        m_optgroups_gui.back()->append_separator(); //seems it's not working

        def.label = L("Use custom size for toolbar icons");
        def.type = coBool;
        def.tooltip = L("If enabled, you can change size of toolbar icons manually.");
        def.set_default_value(new ConfigOptionBool{ app_config->get("use_custom_toolbar_size") == "1" });
        option = Option(def, "use_custom_toolbar_size");
        m_optgroups_gui.back()->append_single_option_line(option);

        create_icon_size_slider(m_optgroups_gui.back().get());
        m_icon_size_sizer->ShowItems(app_config->get("use_custom_toolbar_size") == "1");

        def.label = L("Tab icon size");
        def.type = coInt;
        def.tooltip = L("Size of the tab icons, in pixels. Set to 0 to remove icons.");
        def.set_default_value(new ConfigOptionInt{ atoi(app_config->get("tab_icon_size").c_str()) });
        option = Option(def, "tab_icon_size");
        option.opt.width = 6;
        m_optgroups_gui.back()->append_single_option_line(option);
        m_values_need_restart.push_back("tab_icon_size");

        def.label = L("Font size");
        def.type = coInt;
        def.tooltip = L("Size of the font, and most of the gui (but not the menu and dialog ones). Set to 0 to let the Operating System decide.\nPlease don't set this preference unless your OS scaling factor doesn't works. Set 10 for 100% scaling, and 20 for 200% scaling.");
        def.set_default_value(new ConfigOptionInt{ atoi(app_config->get("font_size").c_str()) });
        option = Option(def, "font_size");
        option.opt.width = 6;
        m_optgroups_gui.back()->append_single_option_line(option);
        m_values_need_restart.push_back("font_size");

        def.label = L("Display setting icons");
        def.type = coBool;
        def.tooltip = L("The settings have a lock and dot to show how they are modified. You can hide them by uncheking this option.");
        def.set_default_value(new ConfigOptionBool{ app_config->get("setting_icon") == "1" });
        option = Option(def, "setting_icon");
        option.opt.width = 6;
        m_optgroups_gui.back()->append_single_option_line(option);
        m_values_need_restart.push_back("setting_icon");

        def.label = L("Use custom tooltip");
        def.type = coBool;
        def.tooltip = L("On some OS like MacOS or some Linux, tooltips can't stay on for a long time. This setting replaces native tooltips with custom dialogs to improve readability (only for settings)."
            "\nNote that for the number controls, you need to hover the arrows to get the custom tooltip. Also, it keeps the focus but will give it back when it closes. It won't show up if you are editing the field.");
        def.set_default_value(new ConfigOptionBool{ app_config->has("use_rich_tooltip") ? app_config->get("use_rich_tooltip") == "1" :
#if __APPLE__
            true
#else
            false
#endif
            });
        option = Option(def, "use_rich_tooltip");
        m_optgroups_gui.back()->append_single_option_line(option);
        m_values_need_restart.push_back("use_rich_tooltip");

        def.label = L("Hide tooltips on slice buttons");
        def.type = coBool;
        def.tooltip = L("These tooltip may be bothersome. You can hide them with this option.");
        def.set_default_value(new ConfigOptionBool{ app_config->has("hide_slice_tooltip") ? app_config->get("hide_slice_tooltip") == "1" : false });
        option = Option(def, "hide_slice_tooltip");
        m_optgroups_gui.back()->append_single_option_line(option);
        m_values_need_restart.push_back("hide_slice_tooltip");

        def.label = L("Show layer height on the scroll bar");
        def.type = coBool;
        def.tooltip = L("Add the layer height (first number after the layer z position) next to a widget of the layer double-scrollbar.");
        def.set_default_value(new ConfigOptionBool{ app_config->get("show_layer_height_doubleslider") == "1" });
        option = Option(def, "show_layer_height_doubleslider");
        m_optgroups_gui.back()->append_single_option_line(option);

        def.label = L("Show layer time on the scroll bar");
        def.type = coBool;
        def.tooltip = L("Add the layer time (after the layer height, or if it's hidden after the layer z position) next to a widget of the layer double-scrollbar.");
        def.set_default_value(new ConfigOptionBool{ app_config->get("show_layer_time_doubleslider") == "1" });
        option = Option(def, "show_layer_time_doubleslider");
        m_optgroups_gui.back()->append_single_option_line(option);

        def.label = L("Show layer area on the scroll bar");
        def.type = coBool;
        def.tooltip = L("Add the layer area (the number just below the layer id) next to a widget of the layer double-scrollbar.");
        def.set_default_value(new ConfigOptionBool{ app_config->get("show_layer_area_doubleslider") == "1" });
        option = Option(def, "show_layer_area_doubleslider");
        m_optgroups_gui.back()->append_single_option_line(option);
    }


    def.label = L("Decimals for gcode viewer colors");
    def.type = coInt;
    def.tooltip = L("On the gcode viewer window, how many decimals are used to separate colors?"
                    " It's used for height, width, volumetric rate, section. Default is 2.");
    def.set_default_value(new ConfigOptionInt{ atoi(app_config->get("gcodeviewer_decimals").c_str()) });
    option = Option(def, "gcodeviewer_decimals");
    option.opt.min = 0;
    option.opt.max = 5;
    option.opt.width = 6;
    m_optgroups_gui.back()->append_single_option_line(option);
    // as it's quite hard to detect a change and then clean & reload the gcode data... then asking for relaod is easier.
    m_values_need_restart.push_back("gcodeviewer_decimals");


    activate_options_tab(m_optgroups_gui.back(), 3);
    if (is_editor) {
        // set Field for notify_release to its value to activate the object
        boost::any val = s_keys_map_NotifyReleaseMode.at(app_config->get("notify_release"));
        m_optgroups_gui.back()->get_field("notify_release")->set_any_value(val, false);
    }

    //create layout options
    create_settings_mode_widget(tabs->GetPage(2));

    //create ui_layout check
    {
        m_optgroups_gui.emplace_back(create_options_group(_L("Settings layout and colors"), tabs, 2));
        m_optgroups_gui.back()->title_width = 0;
        m_optgroups_gui.back()->label_width = 0;
        ConfigOptionDef def_combobox;
        def_combobox.label = "_";
        def_combobox.type = coStrings;
        def_combobox.tooltip = L("Choose the gui package to use. It controls colors, settings layout, quick settings, tags (simple/expert).");
        def_combobox.gui_type = ConfigOptionDef::GUIType::f_enum_open;
        def_combobox.gui_flags = "show_value";
        def_combobox.full_width = true;
        //get all available configs
        for (const AppConfig::LayoutEntry& layout : get_app_config()->get_ui_layouts()) {
            def_combobox.enum_values.push_back(layout.name+": "+layout.description);
        }
        AppConfig::LayoutEntry selected = get_app_config()->get_ui_layout();
        def_combobox.set_default_value(new ConfigOptionStrings{ selected.name+": "+ selected.description });
        option = Option(def_combobox, "ui_layout");
        m_optgroups_gui.back()->append_single_option_line(option);
        m_values_need_restart.push_back("ui_layout");
        activate_options_tab(m_optgroups_gui.back(), 3);
    }

    m_optgroups_gui.emplace_back(create_options_group(_L("Splash screen"), tabs, 2));

    // Show/Hide splash screen
    def.label = L("Show splash screen");
    def.type = coBool;
    def.tooltip = L("Show splash screen");
    def.set_default_value(new ConfigOptionBool{ app_config->get("show_splash_screen") == "1" });
    option = Option(def, "show_splash_screen");
    m_optgroups_gui.back()->append_single_option_line(option);

    // splashscreen image
    {
        ConfigOptionDef def_combobox;
        def_combobox.label = L("Splash screen image");
        def_combobox.type = coStrings;
        def_combobox.tooltip = L("Choose the image to use as splashscreen");
        def_combobox.gui_type = ConfigOptionDef::GUIType::f_enum_open;
        def_combobox.gui_flags = "show_value";
        def_combobox.enum_values.push_back("default");
        def_combobox.enum_labels.push_back(L("Default"));
        def_combobox.enum_values.push_back("icon");
        def_combobox.enum_labels.push_back(L("Icon"));
        def_combobox.enum_values.push_back("random");
        def_combobox.enum_labels.push_back(L("Random"));
        //get all images in the spashscreen dir
        for (const boost::filesystem::directory_entry& dir_entry : boost::filesystem::directory_iterator(boost::filesystem::path(Slic3r::resources_dir()) / "splashscreen")) {
            if (dir_entry.path().has_extension() && std::set<std::string>{ ".jpg", ".JPG", ".jpeg" }.count(dir_entry.path().extension().string()) > 0) {
                def_combobox.enum_values.push_back(dir_entry.path().filename().string());
                def_combobox.enum_labels.push_back(dir_entry.path().stem().string());
            }
        }
        std::string current_file_name = app_config->get(is_editor ? "splash_screen_editor" : "splash_screen_gcodeviewer");
        if (std::find(def_combobox.enum_values.begin(), def_combobox.enum_values.end(), current_file_name) == def_combobox.enum_values.end())
            current_file_name = def_combobox.enum_values[0];
        def_combobox.set_default_value(new ConfigOptionStrings{ current_file_name });
        option = Option(def_combobox, is_editor ? "splash_screen_editor" : "splash_screen_gcodeviewer");
        m_optgroups_gui.back()->append_single_option_line(option);
    }

    def.label = L("Restore window position on start");
    def.type = coBool;
    def.tooltip = (boost::format(_u8L("If enabled, %1% will be open at the position it was closed")) % SLIC3R_APP_NAME).str();
    def.set_default_value(new ConfigOptionBool{ app_config->get("restore_win_position") == "1" });
    option = Option(def, "restore_win_position");
    m_optgroups_gui.back()->append_single_option_line(option);

#ifdef WIN32
    // Clear Undo / Redo stack on new project
    def.label = L("Check for problematic dynamic libraries");
    def.type = coBool;
    def.tooltip = L("Some software like (for example) ASUS Sonic Studio injects a DLL (library) that is known to create some instabilities."
        " This option let Slic3r check at startup if they are loaded.");
    def.set_default_value(new ConfigOptionBool{ app_config->get("check_blacklisted_library") == "1" });
    option = Option(def, "check_blacklisted_library");
    m_optgroups_gui.back()->append_single_option_line(option);
#endif

    activate_options_tab(m_optgroups_gui.back(), 3);

#if ENABLE_ENVIRONMENT_MAP
    if (is_editor) {
        // Add "Render" tab
        m_optgroup_render = create_options_tab(_L("Render"), tabs);
    m_optgroup_render->m_on_change = [this](t_config_option_key opt_key, boost::any value) {
        m_values[opt_key] = boost::any_cast<bool>(value) ? "1" : "0";
    };

    def.label = L("Use environment map");
    def.type = coBool;
    def.tooltip = L("If enabled, renders object using the environment map.");
    def.set_default_value(new ConfigOptionBool{ app_config->get("use_environment_map") == "1" });
    option = Option(def, "use_environment_map");
    m_optgroup_render->append_single_option_line(option);

        activate_options_tab(m_optgroup_render);
    }
#endif // ENABLE_ENVIRONMENT_MAP

    // Add "Colors" tab
    m_optgroups_colors.clear();

#ifdef _WIN32
    m_optgroups_colors.emplace_back(create_options_tab(_L("Colors"), tabs));
#else
    m_optgroups_colors.emplace_back(create_options_tab(_L("Colors"), tabs));
#endif

#ifdef _WIN32
    // Add "Dark Mode" group
    {
        // Add "Dark Mode" group
        m_optgroups_colors.emplace_back(create_options_group(_L("Dark mode (experimental)"), tabs, 3));

        def.label = L("Enable dark mode");
        def.type = coBool;
        def.tooltip = L("If enabled, UI will use Dark mode colors. "
            "If disabled, old UI will be used.");
        def.set_default_value(new ConfigOptionBool{ app_config->get("dark_color_mode") == "1" });
        option = Option(def, "dark_color_mode");
        m_optgroups_colors.back()->append_single_option_line(option);

        if (wxPlatformInfo::Get().GetOSMajorVersion() >= 10) // Use system menu just for Window newer then Windows 10
                                                             // Use menu with ownerdrawn items by default on systems older then Windows 10
        {
            def.label = L("Use system menu for application");
            def.type = coBool;
            def.tooltip = L("If enabled, application will use the standard Windows system menu,\n"
                "but on some combination of display scales it can looks ugly. If disabled, old UI will be used.");
            def.set_default_value(new ConfigOptionBool{ app_config->get("sys_menu_enabled") == "1" });
            option = Option(def, "sys_menu_enabled");
            m_optgroups_colors.back()->append_single_option_line(option);
        }

        activate_options_tab(m_optgroups_colors.back(), 3);
    }
#endif //_WIN32

    m_optgroups_colors.emplace_back(create_options_group(_L("Gui Colors"), tabs, 3));
    //prusa hue : ~22, Susi hue: ~216, slic3r hue: ~55
    // color prusa -> susie
    //ICON 237, 107, 33 -> ed6b21 ; 2172eb
    //DARK 237, 107, 33 -> ed6b21 ; 32, 113, 234 2071ea
    //MAIN 253, 126, 66 -> fd7e42 ; 66, 141, 253 428dfd
    //LIGHT 254, 177, 139 -> feac8b; 139, 185, 254 8bb9fe
    //TEXT 1.0f, 0.49f, 0.22f, 1.0f ff7d38 ; 0.26f, 0.55f, 1.0f, 1.0f 428cff

    // PS 237 107 33 ; SuSi 33 114 235
    def.label = L("Platter icons Color template");
    def.type = coString;
    def.tooltip = _u8L("Color template used by the icons on the platter.")
        + " " + _u8L("It may need a lighter color, as it's used to replace white on top of a dark background.")
        + "\n" + _u8L("Slic3r(yellow): ccbe29, PrusaSlicer(orange): cc6429, SuperSlicer(blue): 3d83ed");
    std::string color_str = app_config->get("color_light");
    if (color_str[0] != '#') color_str = "#" + color_str;
    def.set_default_value(new ConfigOptionString{ color_str });
    option = Option(def, "color_light");
    option.opt.gui_type = ConfigOptionDef::GUIType::color;
    m_optgroups_colors.back()->append_single_option_line(option);
    m_values_need_restart.push_back("color_light");

    // PS 253 126 66 ; SuSi 66 141 253
    def.label = L("Main Gui color template");
    def.type = coString;
    def.tooltip = _u8L("Main color template.")
        + " " + _u8L("If you use a color with higher than 80% saturation and/or value, these will be increased. If lower, they will be decreased.")
        + " " + _u8L("Slic3r(yellow): ccbe29, PrusaSlicer(orange): cc6429, SuperSlicer(blue): 296acc");
    color_str = app_config->get("color");
    if (color_str[0] != '#') color_str = "#" + color_str;
    def.set_default_value(new ConfigOptionString{ color_str });
    option = Option(def, "color");
    option.opt.gui_type = ConfigOptionDef::GUIType::color;
    m_optgroups_colors.back()->append_single_option_line(option);
    m_values_need_restart.push_back("color");

    // PS 254 172 139 ; SS 139 185 254
    def.label = L("Text color template");
    def.type = coString;
    def.tooltip = _u8L("This template will be used for drawing button text on hover.")
        + " " + _u8L("It can be a good idea to use a bit darker color, as some hues can be a bit difficult to read.")
        + " " + _u8L("Slic3r(yellow): ccbe29, PrusaSlicer(orange): cc6429, SuperSlicer(blue): 275cad");
    color_str = app_config->get("color_dark");
    if (color_str[0] != '#') color_str = "#" + color_str;
    def.set_default_value(new ConfigOptionString{ color_str });
    option = Option(def, "color_dark");
    option.opt.gui_type = ConfigOptionDef::GUIType::color;
    m_optgroups_colors.back()->append_single_option_line(option);
    m_values_need_restart.push_back("color_dark");

    activate_options_tab(m_optgroups_colors.back(), 3);

    //create text options
    create_settings_text_color_widget(tabs->GetPage(3));

    // update alignment of the controls for all tabs
    //update_ctrls_alignment();

    if (selected_tab < tabs->GetPageCount())
        tabs->SetSelection(selected_tab);

    auto sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(tabs, 1, wxEXPAND | wxTOP | wxLEFT | wxRIGHT, 5);

    auto buttons = CreateStdDialogButtonSizer(wxOK | wxCANCEL);
    this->Bind(wxEVT_BUTTON, &PreferencesDialog::accept, this, wxID_OK);

    for (int id : {wxID_OK, wxID_CANCEL})
        wxGetApp().UpdateDarkUI(static_cast<wxButton*>(FindWindowById(id, this)));

    sizer->Add(buttons, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM | wxTOP, 10);

    SetSizer(sizer);
    sizer->SetSizeHints(this);
    this->layout();
    this->CenterOnParent();
}

std::vector<ConfigOptionsGroup*> PreferencesDialog::optgroups()
{
    std::vector<ConfigOptionsGroup*> out;
    out.reserve(10);
//TO INTEGRATE
    for (int i = 0; i < (int)m_optgroups_general.size(); i++) {
        if (m_optgroups_general[i]) {
            out.push_back(m_optgroups_general[i].get());
        } else {
            m_optgroups_general.erase(m_optgroups_general.begin() + i);
            i--;
        }
    }
    if (m_optgroup_camera)
        out.push_back(m_optgroup_camera.get());
    for (int i = 0; i < (int)m_optgroups_gui.size(); i++) {
        if (m_optgroups_gui[i]) {
            out.push_back(m_optgroups_gui[i].get());
        } else {
            m_optgroups_gui.erase(m_optgroups_gui.begin() + i);
            i--;
        }
    }
    for (int i = 0; i < (int)m_optgroups_colors.size(); i++) {
        if (m_optgroups_colors[i]) {
            out.push_back(m_optgroups_colors[i].get());
        } else {
            m_optgroups_colors.erase(m_optgroups_colors.begin() + i);
            i--;
        }
    }
    return out;
}

void PreferencesDialog::update_ctrls_alignment()
{
    int max_ctrl_width{ 0 };
    for (ConfigOptionsGroup* og : this->optgroups())
        if (int max = og->custom_ctrl->get_max_win_width();
            max_ctrl_width < max)
            max_ctrl_width = max;
    if (max_ctrl_width)
        for (ConfigOptionsGroup* og : this->optgroups())
            og->custom_ctrl->set_max_win_width(max_ctrl_width);
}

void PreferencesDialog::accept(wxEvent&)
{
//    if (m_values.find("no_defaults") != m_values.end()
//        warning_catcher(this, wxString::Format(_L("You need to restart %s to make the changes effective."), SLIC3R_APP_NAME));

//    std::vector<std::string> options_to_recreate_GUI = { "no_defaults", "tabs_as_menu", "sys_menu_enabled" };

    for (const std::string& option : m_values_need_restart) {
        if (m_values.find(option) != m_values.end()) {
            wxString title = wxGetApp().is_editor() ? wxString(SLIC3R_APP_NAME) : wxString(GCODEVIEWER_APP_NAME);
            title += " - " + _L("Changes for the critical options");
            MessageDialog dialog(nullptr,
                _L("Changing some options will trigger application restart.\n"
                   "You will lose the content of the plater.") + "\n\n" +
                _L("Do you want to proceed?"),
                title,
                wxICON_QUESTION | wxYES | wxNO| wxCANCEL);
            int answer = dialog.ShowModal();
            if (answer == wxID_YES) {
                m_recreate_GUI = true;
            } else if (answer == wxID_CANCEL) {
                for (const std::string& option : m_values_need_restart)
                    m_values.erase(option);
            }
            break;
        }
    }

    auto app_config = get_app_config();

    m_seq_top_layer_only_changed = false;
    if (auto it = m_values.find("seq_top_layer_only"); it != m_values.end())
        m_seq_top_layer_only_changed = app_config->get("seq_top_layer_only") != it->second;

    m_settings_layout_changed = false;
    for (const std::string key : { "old_settings_layout_mode",
                                    "new_settings_layout_mode",
                                    "dlg_settings_layout_mode" })
    {
        auto it = m_values.find(key);
        if (it != m_values.end() && app_config->get(key) != it->second) {
            m_settings_layout_changed = true;
            break;
        }
    }

    for (const std::string key : {    "default_action_on_close_application",
                                    "default_action_on_select_preset",
                                    "default_action_on_new_project" }) {
        auto it = m_values.find(key);
        if (it != m_values.end() && it->second != "none" && app_config->get(key) != "none")
            m_values.erase(it); // we shouldn't change value, if some of those parameters were selected, and then deselected
    }
    {
        auto it = m_values.find("default_action_on_dirty_project");
        if (it != m_values.end() && !it->second.empty() && !app_config->get("default_action_on_dirty_project").empty())
            m_values.erase(it); // we shouldn't change value, if this parameter was selected, and then deselected
    }

#if 0 //#ifdef _WIN32 // #ysDarkMSW - Allow it when we deside to support the sustem colors for application
    if (m_values.find("always_dark_color_mode") != m_values.end())
        wxGetApp().force_sys_colors_update();
#endif
    auto it_auto_switch_preview = m_values.find("auto_switch_preview");
    if (it_auto_switch_preview != m_values.end()) {
        std::vector<std::string> values = def_combobox_auto_switch_preview.enum_values;
        for(size_t i=0; i< values.size(); i++)
        if (values[i] == it_auto_switch_preview->second)
            it_auto_switch_preview->second = std::to_string(i);
    }

    auto it_background_processing = m_values.find("background_processing");
    if (it_background_processing != m_values.end() && it_background_processing->second == "1") {
        bool warning = app_config->get("auto_switch_preview") != "0";
        if (it_auto_switch_preview != m_values.end())
            warning = it_auto_switch_preview->second == "1";
        if(warning) {
            wxMessageDialog dialog(nullptr, "Using background processing with automatic tab switching may be combersome"
                ", are-you sure to keep the automatic tab switching?", _L("Are you sure?"), wxOK | wxCANCEL | wxICON_QUESTION);
            if (dialog.ShowModal() == wxID_CANCEL)
                m_values["auto_switch_preview"] = "0";
        }
    }

    for (std::map<std::string, std::string>::iterator it = m_values.begin(); it != m_values.end(); ++it)
        app_config->set(it->first, it->second);

    app_config->save();
    if (wxGetApp().is_editor()) {
        wxGetApp().set_label_clr_sys(m_sys_colour->GetColour());
        wxGetApp().set_label_clr_modified(m_mod_colour->GetColour());
        wxGetApp().set_label_clr_default(m_def_colour->GetColour());
        wxGetApp().set_label_clr_phony(m_phony_colour->GetColour());
    }

    EndModal(wxID_OK);

#ifdef _WIN32
    if (m_values.find("dark_color_mode") != m_values.end())
        wxGetApp().force_colors_update();
#ifdef _MSW_DARK_MODE
    if (m_values.find("sys_menu_enabled") != m_values.end())
        wxGetApp().force_menu_update();
#endif //_MSW_DARK_MODE
#endif // _WIN32
    if (m_settings_layout_changed)
        ;// application will be recreated after Preference dialog will be destroyed
    else
    // Nothify the UI to update itself from the ini file.
    wxGetApp().update_ui_from_settings();
}

void PreferencesDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    for (ConfigOptionsGroup* og : this->optgroups())
        og->msw_rescale();

    msw_buttons_rescale(this, em_unit(), { wxID_OK, wxID_CANCEL });

    layout();
}

void PreferencesDialog::layout()
{
    const int em        = em_unit();
    SetMinSize(wxSize(47 * em, 28 * em));

    // Fit(); is SetSize(GetBestSize) but GetBestSize doesn't work for scroll pane. we need GetBestVirtualSize over all scroll panes
    wxSize best_size = this->GetBestSize();
    // Get ScrollPanels for each tab
    assert(!this->GetChildren().empty());
    assert(!this->GetChildren().front()->GetChildren().empty());
    if(this->GetChildren().empty() || this->GetChildren().front()->GetChildren().empty()) return;
    std::vector<wxPanel*> panels;
    for (auto c : this->GetChildren().front()->GetChildren()) {
        if (wxPanel *panel = dynamic_cast<wxPanel *>(c); panel)
            panels.push_back(panel);
    }

    if (!panels.empty()) {
        // get a size where all tabs fit into
        wxSize biggest_virtual_size = panels.front()->GetBestVirtualSize();
        for (wxPanel *tab : panels) {
            wxSize current_size    = tab->GetBestVirtualSize();
            biggest_virtual_size.x = std::max(biggest_virtual_size.x, current_size.x);
            biggest_virtual_size.y = std::max(biggest_virtual_size.y, current_size.y);
        }
        best_size = biggest_virtual_size;
        //best_size += tab_inset;
    }
    // add space for buttons and insets of the main panel
    best_size += wxSize(3 * em, 12 * em);
    // also reduce size to fit in screen if needed
    wxDisplay display(wxDisplay::GetFromWindow(this));
    wxRect    screen = display.GetClientArea();
    best_size.x      = std::min(best_size.x, screen.width);
    best_size.y      = std::min(best_size.y, screen.height);
    // apply
    SetSize(best_size);

    Refresh();
}

void PreferencesDialog::create_icon_size_slider(ConfigOptionsGroup* container)
{
    const auto app_config = get_app_config();

    const int em = em_unit();

    m_icon_size_sizer = new wxBoxSizer(wxHORIZONTAL);

    wxWindow* parent = container->parent();
    wxGetApp().UpdateDarkUI(parent);

    if (isOSX)
        // For correct rendering of the slider and value label under OSX
        // we should use system default background
        parent->SetBackgroundStyle(wxBG_STYLE_ERASE);

    auto label = new wxStaticText(parent, wxID_ANY, _L("Icon size in a respect to the default size") + " (%) :");

    m_icon_size_sizer->Add(label, 0, wxALIGN_CENTER_VERTICAL| wxRIGHT | (isOSX ? 0 : wxLEFT), em);

    const int def_val = atoi(app_config->get("custom_toolbar_size").c_str());

    long style = wxSL_HORIZONTAL;
    if (!isOSX)
        style |= wxSL_LABELS | wxSL_AUTOTICKS;

    auto slider = new wxSlider(parent, wxID_ANY, def_val, 30, 100,
                               wxDefaultPosition, wxDefaultSize, style);

    slider->SetTickFreq(10);
    slider->SetPageSize(10);
    slider->SetToolTip(_L("Select toolbar icon size in respect to the default one."));

    m_icon_size_sizer->Add(slider, 1, wxEXPAND);

    wxStaticText* val_label{ nullptr };
    if (isOSX) {
        val_label = new wxStaticText(parent, wxID_ANY, wxString::Format("%d", def_val));
        m_icon_size_sizer->Add(val_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, em);
    }

    slider->Bind(wxEVT_SLIDER, ([this, slider, val_label](wxCommandEvent e) {
        auto val = slider->GetValue();
        m_values["custom_toolbar_size"] = (boost::format("%d") % val).str();

        if (val_label)
            val_label->SetLabelText(wxString::Format("%d", val));
    }), slider->GetId());

    for (wxWindow* win : std::vector<wxWindow*>{ slider, label, val_label }) {
        if (!win) continue;
        win->SetFont(wxGetApp().normal_font());

        if (isOSX) continue; // under OSX we use wxBG_STYLE_ERASE
        win->SetBackgroundStyle(wxBG_STYLE_PAINT);
    }

    container->parent()->GetSizer()->Add(m_icon_size_sizer, 0, wxEXPAND | wxALL, em);
}

void PreferencesDialog::create_settings_mode_widget(wxWindow* tab)
{
#ifdef _USE_CUSTOM_NOTEBOOK
    bool disable_new_layout = wxGetApp().tabs_as_menu();
#endif
    std::vector<wxString> choices = {  _L("Layout with the tab bar"),
                                       _L("Legacy layout"),
                                       _L("Access via settings button in the top menu"),
                                       _L("Settings in non-modal window") };

    auto app_config = get_app_config();
    int selection = app_config->get("tab_settings_layout_mode") == "1" ? 0 :
                    app_config->get("old_settings_layout_mode") == "1" ? 1 :
                    app_config->get("new_settings_layout_mode") == "1" ? 2 :
                    app_config->get("dlg_settings_layout_mode") == "1" ? 3 :
#ifndef WIN32
        1;
#else
        0;
#endif

#ifdef _USE_CUSTOM_NOTEBOOK
    if (disable_new_layout) {
        choices = { _L("Layout with the tab bar"),
                    _L("Legacy layout"),
                    _L("Settings in non-modal window") };
        selection = app_config->get("tab_settings_layout_mode") == "1" ? 0 :
            app_config->get("old_settings_layout_mode") == "1" ? 1 :
            app_config->get("dlg_settings_layout_mode") == "1" ? 2 : 1;
    }
#endif

    wxWindow* parent = m_optgroups_gui.back()->parent();
    wxGetApp().UpdateDarkUI(parent);

    wxStaticBox* stb = new wxStaticBox(parent, wxID_ANY, _L("Tab layout Options"));
    wxGetApp().UpdateDarkUI(stb);
    if (!wxOSX) stb->SetBackgroundStyle(wxBG_STYLE_PAINT);
    stb->SetFont(wxGetApp().bold_font());

    wxSizer* stb_sizer = new wxStaticBoxSizer(stb, wxVERTICAL);
    wxString unstable_warning = _L("!! Can be unstable in some os distribution !!");
    stb->SetToolTip(_L("Choose how the windows are selectable and displayed:")
        + "\n* " + _L(" Tab layout: all windows are in the application, all are selectable via a tab.")
#ifndef _USE_CUSTOM_NOTEBOOK
        + " " + unstable_warning
#endif
        + "\n* " + _L("Old layout: all windows are in the application, settings are on the top tab bar and the platter choice in on the bottom of the platter view.")
        + "\n* " + _L("Settings button: all windows are in the application, no tabs: you have to clic on settings gears to switch to settings tabs.")
        + "\n* " + _L("Settings window: settings are displayed in their own window. You have to clic on settings gears to show the settings window.")
    );

    int id = 0;
    for (const wxString& label : choices) {
        wxRadioButton* btn = new wxRadioButton(parent, wxID_ANY, label, wxDefaultPosition, wxDefaultSize, id==0 ? wxRB_GROUP : 0);
        stb_sizer->Add(btn);
        btn->SetValue(id == selection);


        btn->Bind(wxEVT_RADIOBUTTON, [this, id
#ifdef _USE_CUSTOM_NOTEBOOK
            , disable_new_layout
#endif
        ](wxCommandEvent& ) {
            int test = 0;
            m_values["tab_settings_layout_mode"] = (id == test++) ? "1" : "0";
            m_values["old_settings_layout_mode"] = (id == test++) ? "1" : "0";
#ifdef _USE_CUSTOM_NOTEBOOK
            if (!disable_new_layout)
#endif
            m_values["new_settings_layout_mode"] = (id == test++) ? "1" : "0";
            m_values["dlg_settings_layout_mode"] = (id == test++) ? "1" : "0";
        });
        id++;
    }

    auto sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add(stb_sizer, 1, wxALIGN_CENTER_VERTICAL);
    wxBoxSizer* parent_sizer = static_cast<wxBoxSizer*>(tab->GetSizer());
    parent_sizer->Add(sizer, 0, wxEXPAND | wxTOP, em_unit());
}

void PreferencesDialog::create_settings_text_color_widget(wxWindow* tab)
{
    wxWindow* parent = tab;// m_optgroups_gui.back()->parent();

    wxStaticBox* stb = new wxStaticBox(parent, wxID_ANY, _L("Text colors"));
    wxGetApp().UpdateDarkUI(stb);
    if (!wxOSX) stb->SetBackgroundStyle(wxBG_STYLE_PAINT);
    stb->SetFont(wxGetApp().normal_font());


    //wxBoxSizer* sizer = static_cast<wxBoxSizer*>(static_cast<wxPanel*>(optgroup->parent())->GetSizer());
    wxSizer* stb_sizer = new wxStaticBoxSizer(stb, wxVERTICAL);
    ButtonsDescription::FillSizerWithTextColorDescriptions(stb_sizer, parent, &m_def_colour, &m_sys_colour, &m_mod_colour, &m_phony_colour);

    auto sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add(stb_sizer, 1, wxALIGN_CENTER_VERTICAL);
    wxBoxSizer* parent_sizer = static_cast<wxBoxSizer*>(tab->GetSizer());
    parent_sizer->Add(sizer, 0, wxEXPAND | wxTOP, em_unit());
}

void PreferencesDialog::init_highlighter(const t_config_option_key& opt_key)
{
    m_highlighter.set_timer_owner(this, 0);
    this->Bind(wxEVT_TIMER, [this](wxTimerEvent&)
        {
            m_highlighter.blink();
        });

    std::pair<OG_CustomCtrl*, bool*> ctrl = { nullptr, nullptr };
    for (ConfigOptionsGroup* opt_group : this->optgroups()) {
        ctrl = opt_group->get_custom_ctrl_with_blinking_ptr(opt_key, -1);
        if (ctrl.first && ctrl.second) {
            m_highlighter.init(ctrl);
            break;
        }
    }
}

void PreferencesDialog::PreferencesHighlighter::set_timer_owner(wxEvtHandler* owner, int timerid/* = wxID_ANY*/)
{
    m_timer.SetOwner(owner, timerid);
}

void PreferencesDialog::PreferencesHighlighter::init(std::pair<OG_CustomCtrl*, bool*> params)
{
    if (m_timer.IsRunning())
        invalidate();
    if (!params.first || !params.second)
        return;

    m_timer.Start(300, false);

    m_custom_ctrl = params.first;
    m_show_blink_ptr = params.second;

    *m_show_blink_ptr = true;
    m_custom_ctrl->Refresh();
}

void PreferencesDialog::PreferencesHighlighter::invalidate()
{
    m_timer.Stop();

    if (m_custom_ctrl && m_show_blink_ptr) {
        *m_show_blink_ptr = false;
        m_custom_ctrl->Refresh();
        m_show_blink_ptr = nullptr;
        m_custom_ctrl = nullptr;
    }

    m_blink_counter = 0;
}

void PreferencesDialog::PreferencesHighlighter::blink()
{
    if (m_custom_ctrl && m_show_blink_ptr) {
        *m_show_blink_ptr = !*m_show_blink_ptr;
        m_custom_ctrl->Refresh();
    }
    else
        return;

    if ((++m_blink_counter) == 11)
        invalidate();
}

} // GUI
} // Slic3r
