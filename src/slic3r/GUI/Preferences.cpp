///|/ Copyright (c) Prusa Research 2018 - 2023 Oleksandra Iushchenko @YuSanka, David Kocík @kocikdav, Vojtěch Bubník @bubnikv, Pavel Mikuš @Godrak, Enrico Turri @enricoturri1966, Lukáš Matěna @lukasmatena, Vojtěch Král @vojtechkral
///|/
///|/ ported from lib/Slic3r/GUI/Preferences.pm:
///|/ Copyright (c) Prusa Research 2016 - 2018 Vojtěch Bubník @bubnikv
///|/ Copyright (c) Slic3r 2013 - 2014 Alessandro Ranellucci @alranel
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "Preferences.hpp"
#include "OptionsGroup.hpp"
#include "GUI_App.hpp"
#include "Plater.hpp"
#include "MsgDialog.hpp"
#include "I18N.hpp"
#include "format.hpp"
#include "libslic3r/AppConfig.hpp"

#include "Notebook.hpp"
#include "ButtonsDescription.hpp"
#include "OG_CustomCtrl.hpp"
#include "GLCanvas3D.hpp"
#include "ConfigWizard.hpp"
#include "Widgets/SpinInput.hpp"
#include "wxExtensions.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/dll/runtime_symbol_info.hpp>

#include <wx/display.h>
#include <wx/notebook.h>
#include <wx/scrolwin.h>



#ifdef WIN32
#include <wx/msw/registry.h>
#endif // WIN32
#ifdef __linux__
#include "DesktopIntegrationDialog.hpp"
#endif //__linux__

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

PreferencesDialog::PreferencesDialog(wxWindow* parent) :
    DPIDialog(parent, wxID_ANY, _L("Preferences"), wxDefaultPosition, 
              wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER, "preferences")
{
#ifdef __WXOSX__
    isOSX = true;
#endif
	build();

    wxSize sz = GetSize();
    bool is_scrollbar_shown = false;

    const size_t pages_cnt = tabs->GetPageCount();
    for (size_t tab_id = 0; tab_id < pages_cnt; tab_id++) {
        wxScrolledWindow* scrolled = static_cast<wxScrolledWindow*>(tabs->GetPage(tab_id));
        is_scrollbar_shown |= scrolled->GetScrollLines(wxVERTICAL) > 0;
    }

    if (is_scrollbar_shown)
        sz.x += 2*em_unit();
#ifdef __WXGTK__
    // To correct Layout of wxScrolledWindow we need at least small change of size
    else
        sz.x += 1;
#endif
    SetSize(sz);

	m_highlighter.set_timer_owner(this, 0);
}

static void update_color(wxColourPickerCtrl* color_pckr, const wxColour& color) 
{
	if (color_pckr->GetColour() != color) {
		color_pckr->SetColour(color);
		wxPostEvent(color_pckr, wxCommandEvent(wxEVT_COLOURPICKER_CHANGED));
	}
}

void PreferencesDialog::show(const std::string& highlight_opt_key /*= std::string()*/, const std::string& group_name/*= std::string()*/)
{
    if (!highlight_opt_key.empty()) {
        if (auto it = m_optkey_to_optgroup.find(highlight_opt_key); it != m_optkey_to_optgroup.end() && it->second) {
            // search the tab that contains this group
            assert(m_tabid_2_optgroups.size() == tabs->GetPageCount());
            for (size_t page_idx = 0; page_idx < m_tabid_2_optgroups.size(); page_idx++) {
                if (std::find(m_tabid_2_optgroups[page_idx].begin(), m_tabid_2_optgroups[page_idx].end(),
                              it->second) != m_tabid_2_optgroups[page_idx].end()) {
                    tabs->SetSelection(page_idx);
                    break;
                }
            }
        } else {
            // fail to find the preference, bad registration?
            assert(false);
            return;
        }
		init_highlighter(highlight_opt_key);
    }

	// cache input values for custom toolbar size
	m_custom_toolbar_size		= atoi(get_app_config()->get("custom_toolbar_size").c_str());
	m_use_custom_toolbar_size	= get_app_config()->get_bool("use_custom_toolbar_size");

	// set Field for notify_release to its value
	if (m_optkey_to_optgroup.find("notify_release") != m_optkey_to_optgroup.end())
		if(auto field = m_optkey_to_optgroup["notify_release"]->get_field("notify_release"); field != nullptr) {
			assert(s_keys_map_NotifyReleaseMode.find(wxGetApp().app_config->get("notify_release")) != s_keys_map_NotifyReleaseMode.end());
			boost::any val;
			if(s_keys_map_NotifyReleaseMode.find(wxGetApp().app_config->get("notify_release")) != s_keys_map_NotifyReleaseMode.end()) {
				val = ConfigOptionEnum<NotifyReleaseMode>(NotifyReleaseMode(s_keys_map_NotifyReleaseMode.at(wxGetApp().app_config->get("notify_release")))).get_any();
			} else {
				val = ConfigOptionEnum<NotifyReleaseMode>(NotifyReleaseMode::NotifyReleaseNone).get_any();
			}
			field->set_any_value(val, false);
		}
	

	if (wxGetApp().is_editor()) {
		auto app_config = get_app_config();

		if (m_downloader) {
            this->m_downloader->set_path_name(app_config->get("url_downloader_dest"));
            this->m_downloader->allow(!app_config->has("downloader_url_registered") || app_config->get_bool("downloader_url_registered"));
        }
		for (const std::string& opt_key : {"suppress_hyperlinks", "downloader_url_registered"})
			m_optkey_to_optgroup[opt_key]->set_value(opt_key, app_config->get_bool(opt_key));

		for (const std::string  opt_key : { "default_action_on_close_application"
										   ,"default_action_on_new_project"
										   ,"default_action_on_select_preset" })
			m_optkey_to_optgroup[opt_key]->set_value(opt_key, app_config->get(opt_key) == "none");
		m_optkey_to_optgroup["default_action_on_dirty_project"]->set_value("default_action_on_dirty_project", app_config->get("default_action_on_dirty_project").empty());

		// update colors for color pickers of the labels
		update_color(m_sys_colour, wxGetApp().get_label_clr_sys());
		update_color(m_mod_colour, wxGetApp().get_label_clr_modified());
		
#ifdef GUI_TAG_PALETTE
		// update color pickers for mode palette
		//const std::map<ConfigOptionMode, wxColour> palette = wxGetApp().get_mode_palette(); 
		//std::vector<wxColourPickerCtrl*> color_pickres = {m_mode_simple, m_mode_advanced, m_mode_expert};
		//for (size_t mode = 0; mode < color_pickres.size(); ++mode)
			//update_color(color_pickres[mode], palette[mode]);
#endif
	}

	this->ShowModal();
}
void PreferencesDialog::create_options_tab(const wxString& title)
{
	// note: prusa wxScrolledWindow into a panel that only contains it, I don't know why.
    // set inside a scrollable panel
	// FIXME: HSCROLL
    wxScrolledWindow *tab = new wxScrolledWindow(tabs, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                                 wxBK_LEFT | wxTAB_TRAVERSAL | wxVSCROLL);

	tabs->AddPage(tab, _(title));
	tab->SetFont(wxGetApp().normal_font());

	// Sizer in the scrolled area
	wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
	sizer->SetSizeHints(tab);
	tab->SetSizer(sizer);
    tab->SetScrollRate(0, 5);

	// also reflect the new tab into the tab é optgroup struct
	m_tabid_2_optgroups.emplace_back();
}


std::shared_ptr<ConfigOptionsGroup> PreferencesDialog::create_options_group(const wxString& title, wxBookCtrlBase* tabs, int page_idx)
{

	std::shared_ptr<ConfigOptionsGroup> optgroup = std::make_shared<ConfigOptionsGroup>((wxPanel*)tabs->GetPage(page_idx), title, true);
	optgroup->title_width = 40;
	optgroup->label_width = 40;
	optgroup->set_config_category_and_type(title, int(Preset::TYPE_PREFERENCES));
	optgroup->m_on_change = [this, tabs, optgroup](t_config_option_key opt_key, boost::any value) {
		Field* field = optgroup->get_field(opt_key);
		if (auto it = m_values.find(opt_key); it != m_values.end()) { //TODO: test that
			m_values.erase(it); // we shouldn't change value, if some of those parameters were selected, and then deselected
			return;
		}
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
				auto vector = field->m_opt.enum_def->enums();
				assert(vector.size() > val_int && val_int >= 0);
				if(vector.size() > val_int && val_int >= 0){
					m_values[opt_key] = vector[val_int];
				}
			}
		} else {
			assert(false);
			m_values[opt_key] = boost::any_cast<bool>(value) ? "1" : "0";
		}

// GUI STUFF //TODO: tidy ip
		if (opt_key == "notify_release") {
			int val_int = boost::any_cast<int>(value);
			for (const auto& item : s_keys_map_NotifyReleaseMode) {
				if (item.second == val_int) {
					m_values[opt_key] = item.first;
					return;
				}
			}
		}
		if (opt_key == "use_custom_toolbar_size") {
			m_icon_size_sizer->ShowItems(boost::any_cast<bool>(value));
			refresh_og(m_optkey_to_optgroup["use_custom_toolbar_size"]);
			get_app_config()->set("use_custom_toolbar_size", boost::any_cast<bool>(value) ? "1" : "0");
			wxGetApp().plater()->get_current_canvas3D()->render();
			return;
		}
		if (opt_key == "tabs_as_menu") {
			bool disable_new_layout = boost::any_cast<bool>(value);
			m_rb_new_settings_layout_mode->Show(!disable_new_layout);
			if (disable_new_layout && m_rb_new_settings_layout_mode->GetValue()) {
				m_rb_new_settings_layout_mode->SetValue(false);
				m_rb_old_settings_layout_mode->SetValue(true);
			}
			refresh_og(m_optkey_to_optgroup["tabs_as_menu"]);
		}

	};
	return optgroup;
}

static void activate_options_tab(std::shared_ptr<ConfigOptionsGroup> optgroup, int padding = 20)
{
	optgroup->activate([](){}, wxALIGN_RIGHT);
	optgroup->update_visibility(comNone);
	wxBoxSizer* sizer = static_cast<wxBoxSizer*>(static_cast<wxPanel*>(optgroup->parent())->GetSizer());
	sizer->Add(optgroup->sizer, 0, wxEXPAND | wxALL, padding);

	optgroup->parent()->Layout();

	// apply sercher
	wxGetApp().sidebar().get_searcher().append_preferences_options(optgroup->get_lines());
}

void PreferencesDialog::append_bool_option( std::shared_ptr<ConfigOptionsGroup> optgroup,
								const std::string& opt_key,
								const std::string& label,
								const std::string& tooltip,
								bool def_val,
								ConfigOptionMode mode)
{
	ConfigOptionDef def = {opt_key, coBool};
	def.label = label;
	def.tooltip = tooltip;
	def.mode = mode;
	def.set_default_value(new ConfigOptionBool{ def_val });
	Option option(def, opt_key);
	optgroup->append_single_option_line(option);
	
	m_optkey_to_optgroup[opt_key] = optgroup;

	// fill data to the Search Dialog
	wxGetApp().sidebar().get_searcher().add_key(opt_key, Preset::TYPE_PREFERENCES, optgroup->config_category(), L("Preferences"), def);
}

void PreferencesDialog::append_int_option( std::shared_ptr<ConfigOptionsGroup> optgroup,
								const std::string& opt_key,
								const std::string& label,
								const std::string& tooltip,
								int option_width,
								int def_val,
								ConfigOptionMode mode,
								int32_t min /*= -FLT_MAX*/,
								int32_t max /*= FLT_MAX*/)
{
	ConfigOptionDef def = {opt_key, coInt};
	def.label = label;
	def.tooltip = tooltip;
	def.mode = mode;
	def.min = double(min);
	def.max = double(max);
	def.set_default_value(new ConfigOptionInt{ def_val });
	Option option(def, opt_key);
	option.opt.width = option_width;
	optgroup->append_single_option_line(option);
	
	m_optkey_to_optgroup[opt_key] = optgroup;

	// fill data to the Search Dialog
	wxGetApp().sidebar().get_searcher().add_key(opt_key, Preset::TYPE_PREFERENCES, optgroup->config_category(), L("Preferences"), def);
}

void PreferencesDialog::append_color_option( std::shared_ptr<ConfigOptionsGroup> optgroup,
								const std::string& opt_key,
								const std::string& label,
								const std::string& tooltip,
								std::string color_str,
								ConfigOptionMode mode)
{
	ConfigOptionDef def = {opt_key, coString};
	def.label = label;
	def.tooltip = tooltip;
	def.mode = mode;
	if (color_str[0] != '#') color_str = "#" + color_str;
	def.set_default_value(new ConfigOptionString{ color_str });
	Option option(def, opt_key);
	option.opt.gui_type = ConfigOptionDef::GUIType::color;
	optgroup->append_single_option_line(option);
	
	m_optkey_to_optgroup[opt_key] = optgroup;
	m_values_need_restart.push_back("color_light");

	// fill data to the Search Dialog
	wxGetApp().sidebar().get_searcher().add_key(opt_key, Preset::TYPE_PREFERENCES, optgroup->config_category(), L("Preferences"), def);
}

template<typename EnumType>
void PreferencesDialog::append_enum_option( std::shared_ptr<ConfigOptionsGroup> optgroup,
								const std::string& opt_key,
								const std::string& label,
								const std::string& tooltip,
								const ConfigOption* def_val,
								std::initializer_list<std::pair<std::string_view, std::string_view>> enum_values,
								ConfigOptionMode mode)
{
	ConfigOptionDef def = {opt_key, coEnum };
	def.label = label;
	def.tooltip = tooltip;
	def.mode = mode;
	def.set_enum<EnumType>(enum_values);

	def.set_default_value(def_val);
	Option option(def, opt_key);
	optgroup->append_single_option_line(option);
	
	m_optkey_to_optgroup[opt_key] = optgroup;

	// fill data to the Search Dialog
	wxGetApp().sidebar().get_searcher().add_key(opt_key, Preset::TYPE_PREFERENCES, optgroup->config_category(), L("Preferences"), def);
}

static void append_preferences_option_to_searcher(std::shared_ptr<ConfigOptionsGroup> optgroup,
												const std::string& opt_key,
												const wxString& label)
{
	// fill data to the Search Dialog
    wxGetApp().sidebar().get_searcher().add_key(opt_key, Preset::TYPE_PREFERENCES, optgroup->config_category(), L("Preferences"), {});
	// apply sercher
	wxGetApp().sidebar().get_searcher().append_preferences_option(Line(opt_key, label, ""));
}

void PreferencesDialog::build()
{
#ifdef _WIN32
	wxGetApp().UpdateDarkUI(this);
#else
	//SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
#endif
	const wxFont& font = wxGetApp().normal_font();
	SetFont(font);

	auto app_config = get_app_config();

#ifdef _MSW_DARK_MODE
		tabs = new Notebook(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxNB_TOP | wxTAB_TRAVERSAL | wxNB_NOPAGETHEME | wxNB_DEFAULT);
#else
    tabs = new wxNotebook(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxNB_TOP | wxTAB_TRAVERSAL  |wxNB_NOPAGETHEME | wxNB_DEFAULT );
#ifdef __linux__
	tabs->Bind(wxEVT_NOTEBOOK_PAGE_CHANGED, [this](wxBookCtrlEvent& e) {
		e.Skip();
		CallAfter([this]() { tabs->GetCurrentPage()->Layout(); });
    });
#endif
#endif
	assert(m_tabid_2_optgroups.empty());

	// Add "General" tab
	create_options_tab(_L("General"));
	assert(!m_tabid_2_optgroups.empty());

	bool is_editor = wxGetApp().is_editor();

    m_tabid_2_optgroups.back().emplace_back(create_options_group(_L("Automation"), tabs, 0));
	if (is_editor) {

		append_bool_option(m_tabid_2_optgroups.back().back(), "remember_output_path", 
			L("Remember output directory"),
			L("If this is enabled, Slic3r will prompt the last output directory instead of the one containing the input files."),
			app_config->has("remember_output_path") ? app_config->get_bool("remember_output_path") : true);
		
		append_bool_option(m_tabid_2_optgroups.back().back(), "autocenter", 
			L("Auto-center parts"),
			L("If this is enabled, Slic3r will auto-center objects around the print bed center."),
			app_config->get_bool("autocenter"));
		
		append_bool_option(m_tabid_2_optgroups.back().back(), "background_processing", 
			L("Background processing"),
			L("If this is enabled, Slic3r will pre-process objects as soon "
				"as they\'re loaded in order to save time when exporting G-code."),
			app_config->get_bool("background_processing"));
		
		append_bool_option(m_tabid_2_optgroups.back().back(), "alert_when_supports_needed", 
			L("Alert when supports needed"),
			L("If this is enabled, Slic3r will raise alerts when it detects "
				"issues in the sliced object, that can be resolved with supports (and brim). "
				"Examples of such issues are floating object parts, unsupported extrusions and low bed adhesion."),
			app_config->get_bool("alert_when_supports_needed"));

		//FIXME change it to enum, like the NotifyReleaseMode
		m_optkey_to_optgroup["autocenter"] = m_tabid_2_optgroups.back().back();
		def_combobox_auto_switch_preview.label = L("Switch to Preview when sliced");
		def_combobox_auto_switch_preview.type = coStrings;
		def_combobox_auto_switch_preview.tooltip = L("When an object is sliced, it will switch your view from the curent view to the "
			"preview (and then gcode-preview) automatically, depending on the option choosen.");
		def_combobox_auto_switch_preview.gui_type = ConfigOptionDef::GUIType::f_enum_open;
		def_combobox_auto_switch_preview.gui_flags = "show_value";
        def_combobox_auto_switch_preview.set_enum_labels(ConfigOptionDef::GUIType::f_enum_open, 
        { _u8L("Don't switch"), _u8L("Switch when possible"), _u8L("Only if on platter"), _u8L("Only when GCode is ready") });
		if (app_config->get("auto_switch_preview") == "0")
			def_combobox_auto_switch_preview.set_default_value(new ConfigOptionStrings{ def_combobox_auto_switch_preview.enum_def->label(0) });
		else if (app_config->get("auto_switch_preview") == "1")
			def_combobox_auto_switch_preview.set_default_value(new ConfigOptionStrings{ def_combobox_auto_switch_preview.enum_def->label(1) });
		else if (app_config->get("auto_switch_preview") == "2")
			def_combobox_auto_switch_preview.set_default_value(new ConfigOptionStrings{ def_combobox_auto_switch_preview.enum_def->label(2) });
		else if (app_config->get("auto_switch_preview") == "3")
			def_combobox_auto_switch_preview.set_default_value(new ConfigOptionStrings{ def_combobox_auto_switch_preview.enum_def->label(3) });
		else
			def_combobox_auto_switch_preview.set_default_value(new ConfigOptionStrings{ def_combobox_auto_switch_preview.enum_def->label(2) });
		Option option = Option(def_combobox_auto_switch_preview, "auto_switch_preview");
		m_tabid_2_optgroups.back().back()->append_single_option_line(option);
		m_optkey_to_optgroup["auto_switch_preview"] = m_tabid_2_optgroups.back().back();
		wxGetApp().sidebar().get_searcher().add_key("auto_switch_preview", Preset::TYPE_PREFERENCES, m_tabid_2_optgroups.back().back()->config_category(), L("Preferences"), def_combobox_auto_switch_preview);

		// Please keep in sync with ConfigWizard
		append_bool_option(m_tabid_2_optgroups.back().back(), "export_sources_full_pathnames",
			L("Export sources full pathnames to 3mf and amf"),
			L("If enabled, allows the Reload from disk command to automatically find and load the files when invoked."),
			app_config->get_bool("export_sources_full_pathnames"));

        activate_options_tab(m_tabid_2_optgroups.back().back(), 3);
        m_tabid_2_optgroups.back().emplace_back(create_options_group(_L("Presets and updates"), tabs, 0));
/*
        // Please keep in sync with ConfigWizard
        def.label = L("Check for application updates");
        def.type = coBool;
        def.tooltip = L("If enabled, Slic3r will check for the new versions of itself online. When a new version becomes available a notification is displayed at the next application startup (never during program usage). This is only a notification mechanisms, no automatic installation is done.");
        def.set_default_value(new ConfigOptionBool(app_config->get("version_check") == "1"));
            option = Option(def, "version_check");
        m_tabid_2_optgroups.back().back()->append_single_option_line(option);
*/
        // Please keep in sync with ConfigWizard
		append_bool_option(m_tabid_2_optgroups.back().back(), "preset_update",
			L("Update built-in Presets automatically"),
			L("If enabled, Slic3r downloads updates of built-in system presets in the background. These updates are downloaded "
			  "into a separate temporary location. When a new preset version becomes available it is offered at application startup."),
			app_config->get_bool("preset_update"));

		append_bool_option(m_tabid_2_optgroups.back().back(), "no_defaults",
			L("Suppress \" - default - \" presets"),
			L("Suppress \" - default - \" presets in the Print / Filament / Printer selections once there are any other valid presets available."),
			app_config->get_bool("no_defaults"));
		m_values_need_restart.push_back("no_defaults");

		append_bool_option(m_tabid_2_optgroups.back().back(), "no_templates",
			L("Suppress \" Template \" filament presets"),
			L("Suppress \" Template \" filament presets in configuration wizard and sidebar visibility."),
			app_config->get_bool("no_templates"));

		append_bool_option(m_tabid_2_optgroups.back().back(), "show_incompatible_presets",
			L("Show incompatible print and filament presets"),
			L("When checked, the print and filament presets are shown in the preset editor "
			"even if they are marked as incompatible with the active printer"),
			app_config->get_bool("show_incompatible_presets"));

		append_bool_option(m_tabid_2_optgroups.back().back(), "objects_always_expert",
			L("Main GUI always in expert mode"),
			L("If enabled, the gui will be in expert mode even if the simple or advanced mode is selected (but not the setting tabs)."),
			app_config->get_bool("objects_always_expert"));

        activate_options_tab(m_tabid_2_optgroups.back().back(), 3);
        m_tabid_2_optgroups.back().emplace_back(create_options_group(_L("Files"), tabs, 0));

        // Please keep in sync with ConfigWizard
		append_bool_option(m_tabid_2_optgroups.back().back(), "export_sources_full_pathnames",
			L("Export sources full pathnames to 3mf and amf"),
			L("If enabled, allows the Reload from disk command to automatically find and load the files when invoked."),
			app_config->get_bool("export_sources_full_pathnames"));

#ifdef _WIN32
		// Please keep in sync with ConfigWizard
		append_bool_option(m_tabid_2_optgroups.back().back(), "associate_3mf",
			(boost::format(_u8L("Associate .3mf files to %1%")) % SLIC3R_APP_NAME).str(),
			L("If enabled, sets Slic3r as default application to open .3mf files."),
			app_config->get_bool("associate_3mf"));
		
		append_bool_option(m_tabid_2_optgroups.back().back(), "associate_stl",
			(boost::format(_u8L("Associate .stl files to %1%")) % SLIC3R_APP_NAME).str(),
			L("If enabled, sets Slic3r as default application to open .stl files."),
			app_config->get_bool("associate_stl"));
#endif // _WIN32
		
		append_bool_option(m_tabid_2_optgroups.back().back(), "remember_output_path",
			L("Remember output directory"),
			L("If this is enabled, Slic3r will prompt the last output directory "
            "instead of the one containing the input files."),
			app_config->get_bool("remember_output_path"));
		
		append_bool_option(m_tabid_2_optgroups.back().back(), "date_in_config_file",
			L("Export headers with date and time"),
            L("If this is enabled, Slic3r will add the date of the export to the first line of any exported config and gcode file."
              " Note that some software may rely on that to work, be careful and report any problem if you deactivate it."),
			app_config->get_bool("date_in_config_file"));
		
		append_bool_option(m_tabid_2_optgroups.back().back(), "check_material_export",
			L("Show a Pop-up with the current material when exporting"),
            L("If you constantly forgot to select the right filament/material, check this option to have a really obtrusive reminder on each export."),
			app_config->get_bool("check_material_export"));

		append_bool_option(m_tabid_2_optgroups.back().back(), "show_unknown_setting",
			L("Show ignored settings when loading a project or configuration"),
            L("When loading a configuration, if it's coming from an earlier, a future or from another software, show the ignored settings that doesn't suit this version. Uncheck to remove this anoying pop-up."),
			app_config->get_bool("show_unknown_setting"));

        append_bool_option(m_tabid_2_optgroups.back().back(), "use_binary_gcode_when_supported", L("Use binary G-code when the printer supports it"),
                    L("If the 'Supports binary G-code' option is enabled in Printer Settings, "
                      "checking this option will result in the export of G-code in binary format."),
                    app_config->get_bool("use_binary_gcode_when_supported"));

        activate_options_tab(m_tabid_2_optgroups.back().back(), 3);
        m_tabid_2_optgroups.back().emplace_back(create_options_group(_L("Dialogs"), tabs, 0));
		
		append_bool_option(m_tabid_2_optgroups.back().back(), "show_drop_project_dialog",
			L("Show drop project dialog"),
            L("When checked, whenever dragging and dropping a project file on the application, shows a dialog asking to select the action to take on the file to load."),
			app_config->get_bool("show_drop_project_dialog"));
		
		append_bool_option(m_tabid_2_optgroups.back().back(), "show_overwrite_dialog",
			L("Show overwrite dialog."),
            L("If this is enabled, Slic3r will prompt for when overwriting files from save dialogs."),
			app_config->get_bool("show_overwrite_dialog"));

		append_bool_option(m_tabid_2_optgroups.back().back(), "single_instance",
#if __APPLE__
			L("Allow just a single PrusaSlicer instance"),
			L("On OSX there is always only one instance of app running by default. However it is allowed to run multiple instances "
			  "of same app from the command line. In such case this settings will allow only one instance."),
#else
			L("Allow just a single PrusaSlicer instance"),
			L("If this is enabled, when starting PrusaSlicer and another instance of the same PrusaSlicer is already running, that instance will be reactivated instead."),
#endif
		app_config->has("single_instance") ? app_config->get_bool("single_instance") : false );


		append_bool_option(m_tabid_2_optgroups.back().back(), "default_action_on_dirty_project",
			L("Ask for unsaved changes in project"),
			L("Always ask for unsaved changes in project, when: \n"
						"- Closing Slic3r,\n"
						"- Loading or creating a new project"),
			app_config->get("default_action_on_dirty_project").empty());

		append_bool_option(m_tabid_2_optgroups.back().back(), "default_action_on_close_application",
			L("Ask to save unsaved changes in presets when closing the application or when loading a new project"),
			L("Always ask for unsaved changes in presets, when: \n"
						"- Closing Slic3r while some presets are modified,\n"
						"- Loading a new project while some presets are modified"),
			app_config->get("default_action_on_close_application") == "none");

		append_bool_option(m_tabid_2_optgroups.back().back(), "default_action_on_select_preset",
			L("Ask for unsaved changes in presets when selecting new preset"),
			L("Always ask for unsaved changes in presets when selecting new preset or resetting a preset"),
			app_config->get("default_action_on_select_preset") == "none");

		append_bool_option(m_tabid_2_optgroups.back().back(), "default_action_on_new_project",
			L("Ask for unsaved changes in presets when creating new project"),
			L("Always ask for unsaved changes in presets when creating new project"),
			app_config->get("default_action_on_new_project") == "none");
		
		append_bool_option(m_tabid_2_optgroups.back().back(), "default_action_delete_all",
			L("Ask for 'new project' on 'Delete all'"),
			L("When you click on the garbage can (or ctrl+del), ask for the action to do. If disable, it will erase all object without asking"),
			app_config->get_bool("default_action_delete_all"));

        // Clear Undo / Redo stack on new project
		append_bool_option(m_tabid_2_optgroups.back().back(), "clear_undo_redo_stack_on_new_project",
			L("Clear Undo / Redo stack on new project"),
			L("Clear Undo / Redo stack on new project or when an existing project is loaded."),
			app_config->get_bool("clear_undo_redo_stack_on_new_project"));
	}
#ifdef _WIN32
	else {
		append_bool_option(m_tabid_2_optgroups.back().back(), "associate_gcode",
			(boost::format(_u8L("Associate .gcode files to %1%")) % GCODEVIEWER_APP_NAME).str(),
			(boost::format(_u8L("If enabled, sets %1% as default application to open .gcode files.")) % GCODEVIEWER_APP_NAME).str(),
			app_config->get_bool("associate_gcode"));
		append_bool_option(m_tabid_2_optgroups.back().back(), "associate_bgcode",
			(boost::format(_u8L("Associate .bgcode files to %1%")) % GCODEVIEWER_APP_NAME).str(),
			(boost::format(_u8L("If enabled, sets %1% as default application to open .bgcode files.")) % GCODEVIEWER_APP_NAME).str(),
			app_config->get_bool("associate_bgcode"));
	}
#endif // _WIN32

#if __APPLE__
	append_bool_option(m_tabid_2_optgroups.back().back(), "use_retina_opengl",
		L("Use Retina resolution for the 3D scene"),
		L("If enabled, the 3D scene will be rendered in Retina resolution. "
	      "If you are experiencing 3D performance problems, disabling this option may help."),
		app_config->get_bool("use_retina_opengl"));
#endif

	if (is_editor) {
        activate_options_tab(m_tabid_2_optgroups.back().back(), 3);
		m_tabid_2_optgroups.back().emplace_back(create_options_group(_L("Paths"), tabs, 0));
		m_tabid_2_optgroups.back().back()->title_width = 20;
		m_tabid_2_optgroups.back().back()->label_width = 20;

		m_optkey_to_optgroup["freecad_path"] = m_tabid_2_optgroups.back().back();
		ConfigOptionDef def = {"freecad_path", coString};
		def.label = L("FreeCAD path");
		def.tooltip = L("If it point to a valid freecad instance, you can use the built-in python script to quickly generate geometry."
            "\nPut here the freecad directory from which you can access its 'lib' directory."
            "\nFreecad will use its own python (from the bin directoyr) on windows and will use the system python3 on linux & macos");
		def.set_default_value(new ConfigOptionString{ app_config->get("freecad_path") });
		Option option(def, "freecad_path");
		//option.opt.full_width = true;
		option.opt.width = 50;
		m_tabid_2_optgroups.back().back()->append_single_option_line(option);
		m_optkey_to_optgroup["freecad_path"] = m_tabid_2_optgroups.back().back();
		wxGetApp().sidebar().get_searcher().add_key("freecad_path", Preset::TYPE_PREFERENCES, m_tabid_2_optgroups.back().back()->config_category(), L("Preferences"), def);
		

		append_bool_option(m_tabid_2_optgroups.back().back(), "downloader_url_registered",
			L("Allow downloads from Printables.com"),
			L("If enabled, Slic3r will be allowed to download from Printables.com"),
			app_config->get_bool("downloader_url_registered"));
		assert(m_tabid_2_optgroups.size() == tabs->GetPageCount());
		create_downloader_path_sizer(tabs->GetPage(tabs->GetPageCount()-1), m_tabid_2_optgroups.back().back());
		create_settings_font_widget(tabs->GetPage(tabs->GetPageCount()-1), m_tabid_2_optgroups.back().back());
	}

    activate_options_tab(m_tabid_2_optgroups.back().back(), m_tabid_2_optgroups.back().back()->parent()->GetSizer()->GetItemCount() > 1 ? 3 : 20);
	// end of general

	// Add "Camera" tab
	create_options_tab(L("Camera"));
	m_tabid_2_optgroups.back().emplace_back(create_options_group("", tabs, 1)); // no title -> no borders
	m_tabid_2_optgroups.back().back()->set_config_category_and_type(_L("Camera"), int(Preset::TYPE_PREFERENCES));
	m_tabid_2_optgroups.back().back()->m_on_change = [this](t_config_option_key opt_key, boost::any value) {
		if (auto it = m_values.find(opt_key);it != m_values.end()) {
			m_values.erase(it); // we shouldn't change value, if some of those parameters were selected, and then deselected
			return;
		}
		m_values[opt_key] = boost::any_cast<bool>(value) ? "1" : "0";
	};

	append_bool_option(m_tabid_2_optgroups.back().back(), "use_perspective_camera",
		L("Use perspective camera"),
		L("If enabled, use perspective camera. If not enabled, use orthographic camera."),
		app_config->get_bool("use_perspective_camera"));

	append_bool_option(m_tabid_2_optgroups.back().back(), "use_free_camera",
		L("Use free camera"),
		L("If enabled, use free camera. If not enabled, use constrained camera."),
		app_config->get_bool("use_free_camera"));

	append_bool_option(m_tabid_2_optgroups.back().back(), "reverse_mouse_wheel_zoom",
		L("Reverse direction of zoom with mouse wheel"),
		L("If enabled, reverses the direction of zoom with mouse wheel"),
		app_config->get_bool("reverse_mouse_wheel_zoom"));


#if defined(_WIN32) || defined(__APPLE__)
	//m_tabid_2_optgroups.back().back()->append_separator();
	
	append_bool_option(m_tabid_2_optgroups.back().back(), "use_legacy_3DConnexion",
		L("Enable support for legacy 3DConnexion devices"),
		L("If enabled, the legacy 3DConnexion devices settings dialog is available by pressing CTRL+M"),
		app_config->get_bool("use_legacy_3DConnexion"));
#endif // _WIN32 || __APPLE__

	append_bool_option(m_tabid_2_optgroups.back().back(), "compress_png_texture",
		L("Compress png textures"),
		L("If your custom texture (in png format) is displayed black, then disable this option to remove the problematic optimisation."),
		app_config->get_bool("compress_png_texture"));

	activate_options_tab(m_tabid_2_optgroups.back().back());

	// Add "GUI" tab
	create_options_tab(_L("GUI"));

		//activate_options_tab(m_tabid_2_optgroups.back().back(), 3);
	m_tabid_2_optgroups.back().emplace_back(create_options_group(_L("Controls"), tabs, 2));

	append_bool_option(m_tabid_2_optgroups.back().back(), "seq_top_layer_only",
		L("Sequential slider applied only to top layer"),
		L("If enabled, changes made using the sequential slider, in preview, apply only to gcode top layer. "
		  "If disabled, changes made using the sequential slider, in preview, apply to the whole gcode."),
		app_config->get_bool("seq_top_layer_only"));

	if (is_editor) {
		append_bool_option(m_tabid_2_optgroups.back().back(), "show_collapse_button",
			L("Show sidebar collapse/expand button"),
			L("If enabled, the button for the collapse sidebar will be appeared in top right corner of the 3D Scene"),
			app_config->get_bool("show_collapse_button"));
		
		append_bool_option(m_tabid_2_optgroups.back().back(), "suppress_hyperlinks",
			L("Suppress to open hyperlink in browser"),
			L("If enabled, Slic3r will not open a hyperlinks in your browser."),
			//L("If enabled, the descriptions of configuration parameters in settings tabs wouldn't work as hyperlinks. "
			//  "If disabled, the descriptions of configuration parameters in settings tabs will work as hyperlinks."),
			app_config->get_bool("suppress_hyperlinks"));
		
		append_bool_option(m_tabid_2_optgroups.back().back(), "color_manipulation_panel",
			L("Use colors for axes values in Manipulation panel"),
			L("If enabled, the axes names and axes values will be colorized according to the axes colors. "
			  "If disabled, old UI will be used."),
			app_config->get_bool("color_manipulation_panel"));
		
		append_bool_option(m_tabid_2_optgroups.back().back(), "order_volumes",
			L("Order object volumes by types"),
			L("If enabled, volumes will be always ordered inside the object. Correct order is Model Part, Negative Volume, Modifier, Support Blocker and Support Enforcer. "
			  "If disabled, you can reorder Model Parts, Negative Volumes and Modifiers. But one of the model parts have to be on the first place."),
			app_config->get_bool("order_volumes"));
		
		append_bool_option(m_tabid_2_optgroups.back().back(), "focus_platter_on_mouse",
			L("Focusing platter on mouse over"),
			L("If disabled, moving the mouse over the platter panel will not change the focus but some shortcuts from the platter may not work. "
			"If enabled, moving the mouse over the platter panel will move focus there, and away from the current control."),
			app_config->get_bool("focus_platter_on_mouse"));

		activate_options_tab(m_tabid_2_optgroups.back().back(), 3);
		m_tabid_2_optgroups.back().emplace_back(create_options_group(_L("Appearance"), tabs, 2));

		
		append_bool_option(m_tabid_2_optgroups.back().back(), "allow_auto_color_change",
			L("Allow automatically color change"),
			L("If enabled, related notification will be shown, when sliced object looks like a logo or a sign."),
			app_config->get_bool("allow_auto_color_change"));

#ifdef _USE_CUSTOM_NOTEBOOK
		append_bool_option(m_tabid_2_optgroups.back().back(), "tabs_as_menu",
			L("Set settings tabs as menu items"),
			L("If enabled, Settings Tabs will be placed as menu items. If disabled, old UI will be used."),
			app_config->get_bool("tabs_as_menu"));
		m_values_need_restart.push_back("tabs_as_menu");
#endif

		// FIXME separator don't work anymore
		m_tabid_2_optgroups.back().back()->append_separator();
/*
		append_bool_option(m_tabid_2_optgroups.back().back(), "suppress_round_corners",
			L("Suppress round corners for controls (experimental)"),
			L("If enabled, Settings Tabs will be placed as menu items. If disabled, old UI will be used."),
			app_config->get("suppress_round_corners") == "1");
		m_values_need_restart.push_back("suppress_round_corners");

		m_tabid_2_optgroups.back().back()->append_separator();
*/
		append_bool_option(m_tabid_2_optgroups.back().back(), "show_hints",
			L("Show \"Tip of the day\" notification after start"),
			L("If enabled, useful hints are displayed at startup."),
			app_config->get_bool("show_hints"));

		append_enum_option<NotifyReleaseMode>(m_tabid_2_optgroups.back().back(), "notify_release",
			L("Notify about new releases"),
			L("You will be notified about new release after startup acordingly: All = Regular release and alpha / beta releases. Release only = regular release."),
			new ConfigOptionEnum<NotifyReleaseMode>(static_cast<NotifyReleaseMode>(s_keys_map_NotifyReleaseMode.at(app_config->get("notify_release")))),
			{ { "all", L("All") },
			  { "release", L("Release only") },
			  { "none", L("None") }
			});

		m_tabid_2_optgroups.back().back()->append_separator(); //seems it's not working
		
		append_bool_option(m_tabid_2_optgroups.back().back(), "use_custom_toolbar_size",
			L("Use custom size for toolbar icons"),
			L("If enabled, you can change size of toolbar icons manually."),
			app_config->get_bool("use_custom_toolbar_size"));
		create_icon_size_slider(tabs->GetPage(m_tabid_2_optgroups.size() - 1), m_tabid_2_optgroups.back().back());
		m_icon_size_sizer->ShowItems(app_config->get("use_custom_toolbar_size") == "1");
		
		append_int_option(m_tabid_2_optgroups.back().back(), "tab_icon_size",
			L("Tab icon size"),
			L("Size of the tab icons, in pixels. Set to 0 to remove icons."),
			6,
			app_config->get_int("tab_icon_size"));
		m_values_need_restart.push_back("tab_icon_size");
		
		append_int_option(m_tabid_2_optgroups.back().back(), "font_size",
			L("Font size"),
            L("Size of the font, and most of the gui (but not the menu and dialog ones). Set to 0 to let the Operating System decide."
              "\nPlease don't set this preference unless your OS scaling factor doesn't works. Set 10 for 100% scaling, and 20 for 200% scaling."),
			6,
			app_config->get_int("font_size"));
		m_values_need_restart.push_back("font_size");
		
		append_bool_option(m_tabid_2_optgroups.back().back(), "setting_icon",
			L("Display setting icons"),
			L("The settings have a lock and dot to show how they are modified. You can hide them by uncheking this option."),
			app_config->get_bool("setting_icon"));
		m_values_need_restart.push_back("setting_icon");
		
		append_bool_option(m_tabid_2_optgroups.back().back(), "use_rich_tooltip",
			L("Use custom tooltip"),
			L("On some OS like MacOS or some Linux, tooltips can't stay on for a long time."
		      " This setting replaces native tooltips with custom dialogs to improve readability (only for settings)."
			  "\nNote that for the number controls, you need to hover the arrows to get the custom tooltip."
			  " Also, it keeps the focus but will give it back when it closes. It won't show up if you are editing the field."),
			app_config->get_bool("use_rich_tooltip"));
		m_values_need_restart.push_back("use_rich_tooltip");
		
		append_bool_option(m_tabid_2_optgroups.back().back(), "hide_slice_tooltip",
			L("Hide tooltips on slice buttons"),
			L("These tooltip may be bothersome. You can hide them with this option."),
			app_config->get_bool("hide_slice_tooltip"));
		m_values_need_restart.push_back("hide_slice_tooltip");
		
		append_bool_option(m_tabid_2_optgroups.back().back(), "show_layer_height_doubleslider",
			L("Show layer height on the scroll bar"),
			L("Add the layer height (first number after the layer z position) next to a widget of the layer double-scrollbar."),
			app_config->get_bool("show_layer_height_doubleslider"));
		
		append_bool_option(m_tabid_2_optgroups.back().back(), "show_layer_time_doubleslider",
			L("Show layer time on the scroll bar"),
			L("Add the layer height (after the layer height, or if it's hidden after the layer z position) next to a widget of the layer double-scrollbar."),
			app_config->get_bool("show_layer_time_doubleslider"));
		
		append_bool_option(m_tabid_2_optgroups.back().back(), "show_layer_area_doubleslider",
			L("Show layer area on the scroll bar"),
			L("Add the layer area (the number just below the layer id) next to a widget of the layer double-scrollbar."),
			app_config->get_bool("show_layer_area_doubleslider"));
	}
	
	
	append_int_option(m_tabid_2_optgroups.back().back(), "gcodeviewer_decimals",
		L("Decimals for gcode viewer colors"),
		L("On the gcode viewer window, how many decimals are used to separate colors?"
					" It's used for height, width, volumetric rate, section. Default is 2."),
		/*width*/6,
		app_config->get_int("gcodeviewer_decimals"),
		ConfigOptionMode::comNone,
		/*min,max*/0, 5);
	// as it's quite hard to detect a change and then clean & reload the gcode data... then asking for relaod is easier.
	m_values_need_restart.push_back("gcodeviewer_decimals");

	activate_options_tab(m_tabid_2_optgroups.back().back(), 3);

	if (is_editor) {
		// set Field for notify_release to its value to activate the object
		if (auto field = m_tabid_2_optgroups.back().back()->get_field("notify_release"); field != nullptr) {
			boost::any val;
			if(s_keys_map_NotifyReleaseMode.find(wxGetApp().app_config->get("notify_release")) != s_keys_map_NotifyReleaseMode.end()) {
				val = ConfigOptionEnum<NotifyReleaseMode>(NotifyReleaseMode(s_keys_map_NotifyReleaseMode.at(wxGetApp().app_config->get("notify_release")))).get_any();
			} else {
				val = ConfigOptionEnum<NotifyReleaseMode>(NotifyReleaseMode::NotifyReleaseNone).get_any();
			}
			field->set_any_value(val, false);
		} else assert(false);

	//create layout options
    assert(m_tabid_2_optgroups.size() - 1 == 2);
	create_settings_mode_widget(tabs->GetPage(m_tabid_2_optgroups.size() - 1), m_tabid_2_optgroups.back().back());
	//create ui_layout check
	{
		m_tabid_2_optgroups.back().emplace_back(create_options_group(_L("Settings layout and colors"), tabs, 2));
		m_tabid_2_optgroups.back().back()->title_width = 0;
		m_tabid_2_optgroups.back().back()->label_width = 0;
		ConfigOptionDef def_combobox;
		def_combobox.label = "_";
		def_combobox.type = coStrings;
		def_combobox.tooltip = L("Choose the gui package to use. It controls colors, settings layout, quick settings, tags (simple/expert).");
		def_combobox.full_width = false; //true doesn't set the space for the search arrow (and add a line before for it but it fails).
		def_combobox.width = 64;
		//get all available configs
		std::vector<std::string> enum_values;
		for (const AppConfig::LayoutEntry& layout : get_app_config()->get_ui_layouts()) {
			enum_values.push_back(layout.name+": "+layout.description);
		}
		def_combobox.set_enum_values(ConfigOptionDef::GUIType::select_close, enum_values);
		def_combobox.gui_flags = "show_value";

		AppConfig::LayoutEntry selected = get_app_config()->get_ui_layout();
		def_combobox.set_default_value(new ConfigOptionStrings{ selected.name+": "+ selected.description });
		Option option = Option(def_combobox, "ui_layout");
		m_tabid_2_optgroups.back().back()->append_single_option_line(option);
		m_values_need_restart.push_back("ui_layout");
		m_optkey_to_optgroup["ui_layout"] = m_tabid_2_optgroups.back().back();
		wxGetApp().sidebar().get_searcher().add_key("ui_layout", Preset::TYPE_PREFERENCES, m_tabid_2_optgroups.back().back()->config_category(), L("Preferences"), def_combobox);
		activate_options_tab(m_tabid_2_optgroups.back().back(), 3);
	}

    m_tabid_2_optgroups.back().emplace_back(create_options_group(_L("Splash screen"), tabs, 2));

    // Show/Hide splash screen
	append_bool_option(m_tabid_2_optgroups.back().back(), "show_splash_screen",
		L("Show splash screen"),
		L("Show splash screen"),
		app_config->get_bool("show_splash_screen"));

    // splashscreen image
    {

        ConfigOptionDef def_combobox;
        def_combobox.label = L("Splash screen image");
        def_combobox.type = coStrings;
        def_combobox.tooltip = L("Choose the image to use as splashscreen");
		std::vector<std::pair<std::string,std::string>> enum_key_values = {
			{"default", L("Default")}, 
			{"icon", L("Icon")}, 
			{"random", L("Random")}
			};
        //get all images in the spashscreen dir
        for (const boost::filesystem::directory_entry& dir_entry : boost::filesystem::directory_iterator(boost::filesystem::path(Slic3r::resources_dir()) / "splashscreen")) {
            if (dir_entry.path().has_extension() && std::set<std::string>{ ".jpg", ".JPG", ".jpeg" }.count(dir_entry.path().extension().string()) > 0) {
                enum_key_values.push_back({dir_entry.path().filename().string(), dir_entry.path().stem().string()});
            }
        }
        def_combobox.set_enum_values(ConfigOptionDef::GUIType::select_close, enum_key_values);
        def_combobox.gui_flags = "show_value";
		assert(def_combobox.enum_def->is_valid_open_enum());
        std::string current_file_name = app_config->get(is_editor ? "splash_screen_editor" : "splash_screen_gcodeviewer");
        if (std::find(def_combobox.enum_def->values().begin(), def_combobox.enum_def->values().end(), current_file_name) == def_combobox.enum_def->values().end()) {
			assert(false);
            current_file_name = def_combobox.enum_def->values()[0];
			app_config->set(is_editor ? "splash_screen_editor" : "splash_screen_gcodeviewer", current_file_name);
        }
        def_combobox.set_default_value(new ConfigOptionStrings{ current_file_name });
        Option option = Option(def_combobox, is_editor ? "splash_screen_editor" : "splash_screen_gcodeviewer");
        m_tabid_2_optgroups.back().back()->append_single_option_line(option);
		m_optkey_to_optgroup[is_editor ? "splash_screen_editor" : "splash_screen_gcodeviewer"] = m_tabid_2_optgroups.back().back();
		wxGetApp().sidebar().get_searcher().add_key(is_editor ? "splash_screen_editor" : "splash_screen_gcodeviewer", Preset::TYPE_PREFERENCES, m_tabid_2_optgroups.back().back()->config_category(), L("Preferences"), def_combobox);
    }
	
	append_bool_option(m_tabid_2_optgroups.back().back(), "restore_win_position",
		L("Restore window position on start"),
		L("If enabled, Slic3r will be open at the position it was closed"),
		app_config->get_bool("restore_win_position"));

#ifdef WIN32
    // Clear Undo / Redo stack on new project
	append_bool_option(m_tabid_2_optgroups.back().back(), "check_blacklisted_library",
		L("Check for problematic dynamic libraries"),
		L("Some software like (for example) ASUS Sonic Studio injects a DLL (library) that is known to create some instabilities."
        " This option let Slic3r check at startup if they are loaded."),
		app_config->get_bool("check_blacklisted_library"));
#endif

    activate_options_tab(m_tabid_2_optgroups.back().back(), 3);

#if ENABLE_ENVIRONMENT_MAP
		// Add "Render" tab
		create_options_tab(L("Render"));
		m_tabid_2_optgroups.back().emplace_back(create_options_group("", tabs, 1));
		m_tabid_2_optgroups.back().back()->set_config_category_and_type(L("Render"), int(Preset::TYPE_PREFERENCES));
		m_tabid_2_optgroups.back().back()->m_on_change = [this](t_config_option_key opt_key, boost::any value) {
			if (auto it = m_values.find(opt_key); it != m_values.end()) {
				m_values.erase(it); // we shouldn't change value, if some of those parameters were selected, and then deselected
				return;
			}
			m_values[opt_key] = boost::any_cast<bool>(value) ? "1" : "0";
		};

		append_bool_option(m_tabid_2_optgroups.back().back(), "use_environment_map",
			L("Use environment map"),
			L("If enabled, renders object using the environment map."),
			app_config->get_bool("use_environment_map"));

		activate_options_tab(m_tabid_2_optgroups.back().back());
#endif // ENABLE_ENVIRONMENT_MAP
	}

	// Add "Colors" tab
	create_options_tab(_L("Colors"));
#ifdef _WIN32
	// Add "Dark Mode" group
    {
        // Add "Dark Mode" group
        m_tabid_2_optgroups.back().emplace_back(create_options_group(_L("Dark mode (experimental)"), tabs, 3));

        append_bool_option(m_tabid_2_optgroups.back().back(), "dark_color_mode", L("Enable dark mode"),
                           L("If enabled, UI will use Dark mode colors. If disabled, old UI will be used."),
                           app_config->get_bool("dark_color_mode"));

        if (wxPlatformInfo::Get().GetOSMajorVersion() >= 10) // Use system menu just for Window newer then Windows 10
                                                             // Use menu with ownerdrawn items by default on systems older then Windows 10
        {
            append_bool_option(m_tabid_2_optgroups.back().back(), "sys_menu_enabled", L("Use system menu for application"),
                               L("If enabled, application will use the standard Windows system menu,\n"
                                 "but on some combination of display scales it can look ugly. If disabled, old UI will be used."),
                               app_config->get_bool("sys_menu_enabled"));
            m_values_need_restart.push_back("sys_menu_enabled");
        }

        activate_options_tab(m_tabid_2_optgroups.back().back(), 3);
    }
#endif //_WIN32

	m_tabid_2_optgroups.back().emplace_back(create_options_group(_L("Gui Colors"), tabs, 3));
	//prusa hue : ~22, Susi hue: ~216, slic3r hue: ~55
	// color prusa -> susie
	//ICON 237, 107, 33 -> ed6b21 ; 2172eb
	//DARK 237, 107, 33 -> ed6b21 ; 32, 113, 234 2071ea
	//MAIN 253, 126, 66 -> fd7e42 ; 66, 141, 253 428dfd
	//LIGHT 254, 177, 139 -> feac8b; 139, 185, 254 8bb9fe
	//TEXT 1.0f, 0.49f, 0.22f, 1.0f ff7d38 ; 0.26f, 0.55f, 1.0f, 1.0f 428cff

	// PS 237 107 33 ; SuSi 33 114 235
	append_color_option(m_tabid_2_optgroups.back().back(), "color_light",
		L("Platter icons Color template"),
		_u8L("Color template used by the icons on the platter.") +" "+
		_u8L("It may need a lighter color, as it's used to replace white on top of a dark background.") +"\n"+
		_u8L("Slic3r(yellow): ccbe29, PrusaSlicer(orange): cc6429, SuperSlicer(blue): 3d83ed"),
		app_config->get("color_light"));

	// PS 253 126 66 ; SuSi 66 141 253
	append_color_option(m_tabid_2_optgroups.back().back(), "color",
		L("Main Gui color template"),
		_u8L("Main color template.") +" "+
		_u8L("If you use a color with higher than 80% saturation and/or value, these will be increased. If lower, they will be decreased.") +"\n"+
		_u8L("Slic3r(yellow): ccbe29, PrusaSlicer(orange): cc6429, SuperSlicer(blue): 296acc"),
		app_config->get("color"));

	// PS 254 172 139 ; SS 139 185 254
	append_color_option(m_tabid_2_optgroups.back().back(), "color_dark",
		L("Text color template"),
		_u8L("This template will be used for drawing button text on hover.") +" "+
		_u8L("It can be a good idea to use a bit darker color, as some hues can be a bit difficult to read.") +"\n"+
		_u8L("Slic3r(yellow): ccbe29, PrusaSlicer(orange): cc6429, SuperSlicer(blue): 275cad"),
		app_config->get("color_dark"));

	activate_options_tab(m_tabid_2_optgroups.back().back(), 3);

	//create text options
	create_settings_text_color_widget(tabs->GetPage(m_tabid_2_optgroups.size() - 1), m_tabid_2_optgroups.back().back());
	create_settings_mode_color_widget(tabs->GetPage(m_tabid_2_optgroups.size() - 1), m_tabid_2_optgroups.back().back());

	// update alignment of the controls for all tabs
	//update_ctrls_alignment();

	auto sizer = new wxBoxSizer(wxVERTICAL);
	sizer->Add(tabs, 1, wxEXPAND | wxTOP | wxLEFT | wxRIGHT, 5);

	auto buttons = CreateStdDialogButtonSizer(wxOK | wxCANCEL);
	wxGetApp().SetWindowVariantForButton(buttons->GetAffirmativeButton());
	wxGetApp().SetWindowVariantForButton(buttons->GetCancelButton());
	this->Bind(wxEVT_BUTTON, &PreferencesDialog::accept, this, wxID_OK);
	this->Bind(wxEVT_BUTTON, &PreferencesDialog::revert, this, wxID_CANCEL);

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
    for (auto &opt_group_list : m_tabid_2_optgroups) {
        for (int i = 0; i < (int) opt_group_list.size(); i++) {
            if (opt_group_list[i]) {
                out.push_back(opt_group_list[i].get());
            } else {
                opt_group_list.erase(opt_group_list.begin() + i);
                i--;
            }
        }
    }
	return out;
}

void PreferencesDialog::update_ctrls_alignment()
{
    int max_ctrl_width{0};
    for (ConfigOptionsGroup *og : this->optgroups()) {
        if (og->custom_ctrl) {
            if (int max = og->custom_ctrl->get_max_win_width(); max_ctrl_width < max) {
                max_ctrl_width = max;
            }
        }
    }
    if (max_ctrl_width) {
        for (ConfigOptionsGroup *og : this->optgroups()) {
            if (og->custom_ctrl) {
                og->custom_ctrl->set_max_win_width(max_ctrl_width);
            }
        }
    }
}

void PreferencesDialog::accept(wxEvent&)
{
	if(wxGetApp().is_editor()) {
		if (const auto it = m_values.find("downloader_url_registered"); it != m_values.end())
			this->m_downloader->allow(it->second == "1");
		if (!this->m_downloader->on_finish())
			return;
#ifdef __linux__
		if( this->m_downloader->get_perform_registration_linux()) 
			DesktopIntegrationDialog::perform_downloader_desktop_integration();
#endif // __linux__
	}
//	std::vector<std::string> options_to_recreate_GUI = { "no_defaults", "tabs_as_menu", "sys_menu_enabled", "font_pt_size", "suppress_round_corners" };


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

#if 0 //#ifdef _WIN32 // #ysDarkMSW - Allow it when we deside to support the sustem colors for application
	if (m_values.find("always_dark_color_mode") != m_values.end())
		wxGetApp().force_sys_colors_update();
#endif
	auto it_auto_switch_preview = m_values.find("auto_switch_preview");
	if (it_auto_switch_preview != m_values.end()) {
		assert(def_combobox_auto_switch_preview.enum_def);
		std::vector<std::string> values = def_combobox_auto_switch_preview.enum_def->values();
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

	if (wxGetApp().is_editor()) {
		wxGetApp().set_label_clr_sys(m_sys_colour->GetColour());
		wxGetApp().set_label_clr_modified(m_mod_colour->GetColour());
		wxGetApp().set_label_clr_default(m_def_colour->GetColour());
		wxGetApp().set_label_clr_phony(m_phony_colour->GetColour());
#ifdef GUI_TAG_PALETTE
		wxGetApp().set_mode_palette(m_mode_palette);
#endif
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

	if (m_values.find("no_templates") != m_values.end())
		wxGetApp().plater()->force_filament_cb_update();

	wxGetApp().update_ui_from_settings();
	clear_cache();
}

void PreferencesDialog::revert(wxEvent&)
{
	auto app_config = get_app_config();

	if (m_custom_toolbar_size != atoi(app_config->get("custom_toolbar_size").c_str())) {
		app_config->set("custom_toolbar_size", (boost::format("%d") % m_custom_toolbar_size).str());
		m_icon_size_slider->SetValue(m_custom_toolbar_size);
	}
	if (m_use_custom_toolbar_size != (get_app_config()->get_bool("use_custom_toolbar_size"))) {
		app_config->set("use_custom_toolbar_size", m_use_custom_toolbar_size ? "1" : "0");

		m_optkey_to_optgroup["use_custom_toolbar_size"]->set_value("use_custom_toolbar_size", m_use_custom_toolbar_size);
		m_icon_size_sizer->ShowItems(m_use_custom_toolbar_size);
		refresh_og(m_optkey_to_optgroup["use_custom_toolbar_size"]);
	}

	for (auto value : m_values) {
		const std::string& key = value.first;
		// special cases
		if (key == "default_action_on_dirty_project") {
			m_optkey_to_optgroup[key]->set_value(key, app_config->get(key).empty());
			continue;
		}
		if (key == "default_action_on_close_application" || key == "default_action_on_select_preset" || key == "default_action_on_new_project") {
			m_optkey_to_optgroup[key]->set_value(key, app_config->get(key) == "none");
			continue;
		}
		if (key == "notify_release") {
			m_optkey_to_optgroup[key]->set_value(key, s_keys_map_NotifyReleaseMode.at(app_config->get(key)));
			continue;
		}
		if (key == "old_settings_layout_mode") {
			m_rb_old_settings_layout_mode->SetValue(app_config->get_bool(key));
			m_settings_layout_changed = false;
			continue;
		}
		if (key == "new_settings_layout_mode") {
			m_rb_new_settings_layout_mode->SetValue(app_config->get_bool(key));
			m_settings_layout_changed = false;
			continue;
		}
		if (key == "dlg_settings_layout_mode") {
			m_rb_dlg_settings_layout_mode->SetValue(app_config->get_bool(key));
			m_settings_layout_changed = false;
			continue;
		}
		if (key == "tabs_as_menu") {
			m_rb_new_settings_layout_mode->Show(!app_config->get_bool(key));
			refresh_og(m_optkey_to_optgroup[key]);
			continue;
		}
		//general case
		Field* field = m_optkey_to_optgroup[key]->get_field(key);
        if (field->m_opt.type == coBool) {
			 m_optkey_to_optgroup[key]->set_value("",true);
			field->set_any_value(ConfigOptionBool(app_config->get_bool(key)).get_any(), false);
			continue;
		}
        if (field->m_opt.type == coString) {
			std::string val = app_config->get(key);
			if(field->m_opt.gui_type == ConfigOptionDef::GUIType::color)
				if (val[0] != '#') val = "#" + val;
			field->set_any_value(ConfigOptionString(val).get_any(), false);
			continue;
		}
        if (field->m_opt.type == coInt) {
			field->set_any_value(ConfigOptionInt(app_config->get_int(key)).get_any(), false);
			continue;
		}
		assert(false);
	}

	clear_cache();
	EndModal(wxID_CANCEL);
}

void PreferencesDialog::msw_rescale()
{
	for (ConfigOptionsGroup* og : this->optgroups())
		og->msw_rescale();

	update_ctrls_alignment();

    msw_buttons_rescale(this, em_unit(), { wxID_OK, wxID_CANCEL });

    layout();
}

void PreferencesDialog::on_sys_color_changed()
{
#ifdef _WIN32
	wxGetApp().UpdateDlgDarkUI(this);
#endif
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

void PreferencesDialog::clear_cache()
{
	m_values.clear();
	m_custom_toolbar_size = -1;
}

void PreferencesDialog::refresh_og(std::shared_ptr<ConfigOptionsGroup> og)
{
	og->parent()->Layout();
	tabs->Layout();
//	this->layout();
}

void PreferencesDialog::create_icon_size_slider(wxWindow* tab, std::shared_ptr<ConfigOptionsGroup> opt_grp)
{
    const auto app_config = get_app_config();

    const int em = em_unit();

    m_icon_size_sizer = new wxBoxSizer(wxHORIZONTAL);

	wxWindow* parent = tab; //container->parent();
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

    m_icon_size_slider = new wxSlider(parent, wxID_ANY, def_val, 30, 100, 
                               wxDefaultPosition, wxDefaultSize, style);

    m_icon_size_slider->SetTickFreq(10);
    m_icon_size_slider->SetPageSize(10);
    m_icon_size_slider->SetToolTip(_L("Select toolbar icon size in respect to the default one."));

    m_icon_size_sizer->Add(m_icon_size_slider, 1, wxEXPAND);

    wxStaticText* val_label{ nullptr };
    if (isOSX) {
        val_label = new wxStaticText(parent, wxID_ANY, wxString::Format("%d", def_val));
        m_icon_size_sizer->Add(val_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, em);
    }

    m_icon_size_slider->Bind(wxEVT_SLIDER, ([this, val_label, app_config](wxCommandEvent e) {
        auto val = m_icon_size_slider->GetValue();

		app_config->set("custom_toolbar_size", (boost::format("%d") % val).str());
		wxGetApp().plater()->get_current_canvas3D()->render();

        if (val_label)
            val_label->SetLabelText(wxString::Format("%d", val));
    }), m_icon_size_slider->GetId());

    for (wxWindow* win : std::vector<wxWindow*>{ m_icon_size_slider, label, val_label }) {
        if (!win) continue;         
        win->SetFont(wxGetApp().normal_font());

        if (isOSX) continue; // under OSX we use wxBG_STYLE_ERASE
        win->SetBackgroundStyle(wxBG_STYLE_PAINT);
    }

	opt_grp->parent()->GetSizer()->Add(m_icon_size_sizer, 0, wxEXPAND | wxALL, em);
}

void PreferencesDialog::create_settings_mode_widget(wxWindow* tab, std::shared_ptr<ConfigOptionsGroup> opt_grp)
{
#ifdef _USE_CUSTOM_NOTEBOOK
	bool disable_new_layout = wxGetApp().tabs_as_menu();
#endif
	wxWindow* parent = tab;//opt_grp->parent();

	wxString title = L("Layout Options");
    wxStaticBox* stb = new wxStaticBox(parent, wxID_ANY, _(title));
	wxGetApp().UpdateDarkUI(stb);
	if (!wxOSX) stb->SetBackgroundStyle(wxBG_STYLE_PAINT);
	stb->SetFont(wxGetApp().normal_font());

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

	auto sizer_v = new wxBoxSizer(wxVERTICAL);

	auto app_config = get_app_config();
	std::vector<wxString> choices = {  _L("Layout with the tab bar"),
                                       _L("Legacy layout"),
                                       _L("Access via settings button in the top menu"),
                                       _L("Settings in non-modal window") };
	int id = -1;
	auto add_radio = [this, parent, sizer_v, choices](wxRadioButton** rb, int id, bool select) {
		*rb = new wxRadioButton(parent, wxID_ANY, choices[id], wxDefaultPosition, wxDefaultSize, id == 0 ? wxRB_GROUP : 0);
		sizer_v->Add(*rb);
		(*rb)->SetValue(select);
		(*rb)->Bind(wxEVT_RADIOBUTTON, [this, id](wxCommandEvent&) {
			m_values["tab_settings_layout_mode"] = (id == 0) ? "1" : "0";
			m_values["old_settings_layout_mode"] = (id == 1) ? "1" : "0";
			m_values["new_settings_layout_mode"] = (id == 2) ? "1" : "0";
			m_values["dlg_settings_layout_mode"] = (id == 3) ? "1" : "0";
		});
	};


	add_radio(&m_rb_dlg_settings_layout_mode, ++id, app_config->get_bool("tab_settings_layout_mode"));
	add_radio(&m_rb_old_settings_layout_mode, ++id, app_config->get_bool("old_settings_layout_mode"));
	add_radio(&m_rb_new_settings_layout_mode, ++id, app_config->get_bool("new_settings_layout_mode"));
	add_radio(&m_rb_dlg_settings_layout_mode, ++id, app_config->get_bool("dlg_settings_layout_mode"));
/* //TODO: to merge	int id = 0;
	for (const wxString& label : choices) {
		wxRadioButton* btn = new wxRadioButton(parent, wxID_ANY, label, wxDefaultPosition, wxDefaultSize, id==0 ? wxRB_GROUP : 0);
		sizer_v->Add(btn);
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
	}*/
#ifdef _USE_CUSTOM_NOTEBOOK
	if (app_config->get_bool("tabs_as_menu")) {
		m_rb_new_settings_layout_mode->Hide();
		if (m_rb_new_settings_layout_mode->GetValue()) {
			m_rb_new_settings_layout_mode->SetValue(false);
			m_rb_old_settings_layout_mode->SetValue(true);


		}
	}
#endif
	
	wxSizer* stb_sizer = new wxStaticBoxSizer(stb, wxHORIZONTAL);
	std::string opt_key = "settings_layout_mode";
	m_blinkers[opt_key] = new BlinkingBitmap(parent);
	m_optkey_to_optgroup[opt_key] = opt_grp;
	stb_sizer->Add(m_blinkers[opt_key], 0, wxRIGHT, 2);
	stb_sizer->Add(sizer_v, 1, wxEXPAND);
	wxBoxSizer* parent_sizer = static_cast<wxBoxSizer*>(tab->GetSizer());
	parent_sizer->Add(stb_sizer, 0, wxEXPAND | wxALL, 3);
	//opt_grp->sizer->Add(sizer, 0, wxEXPAND | wxTOP, em_unit());

	append_preferences_option_to_searcher(opt_grp, opt_key, title);
}

void PreferencesDialog::create_settings_text_color_widget(wxWindow* tab, std::shared_ptr<ConfigOptionsGroup> opt_grp)
{
	wxWindow* parent = tab;//opt_grp->parent();
	wxString title = L("Text colors");
	wxStaticBox* stb = new wxStaticBox(parent, wxID_ANY, _(title));	wxGetApp().UpdateDarkUI(stb);
	if (!wxOSX) stb->SetBackgroundStyle(wxBG_STYLE_PAINT);

	std::string opt_key = "text_colors";
	m_blinkers[opt_key] = new BlinkingBitmap(parent);
	m_optkey_to_optgroup[opt_key] = opt_grp;

	wxSizer* stb_sizer = new wxStaticBoxSizer(stb, wxHORIZONTAL);
	stb_sizer->Add(m_blinkers[opt_key], 0, wxRIGHT, 2);
	GUI_Descriptions::FillSizerWithTextColorDescriptions(stb_sizer, parent, &m_def_colour, &m_sys_colour, &m_mod_colour, &m_phony_colour);

	wxBoxSizer* parent_sizer = static_cast<wxBoxSizer*>(tab->GetSizer());
	//opt_grp->sizer->Add(sizer, 0, wxEXPAND | wxTOP, em_unit());
	parent_sizer->Add(stb_sizer, 0, wxEXPAND | wxALL, 3);

	append_preferences_option_to_searcher(opt_grp, opt_key, title);
}


void PreferencesDialog::create_settings_mode_color_widget(wxWindow* tab, std::shared_ptr<ConfigOptionsGroup> opt_grp)
{
	wxWindow* parent = tab;//opt_grp->parent();

	wxString title = L("Mode markers");
	wxStaticBox* stb = new wxStaticBox(parent, wxID_ANY, _(title));
	wxGetApp().UpdateDarkUI(stb);
	if (!wxOSX) stb->SetBackgroundStyle(wxBG_STYLE_PAINT);
	stb->SetFont(wxGetApp().normal_font());

	std::string opt_key = "mode_markers";
	m_blinkers[opt_key] = new BlinkingBitmap(parent);
	m_optkey_to_optgroup[opt_key] = opt_grp;
	wxSizer* stb_sizer = new wxStaticBoxSizer(stb, wxHORIZONTAL);

    // Mode color markers description
	//check if we have enough colour picker
	std::vector<std::pair<wxColourPickerCtrl**, AppConfig::Tag>> clr_pickers_2_color;
    for (AppConfig::Tag &tag : get_app_config()->tags()) {
		//create nullptr if not present yet
		if(m_tag_color.find(tag.tag) == m_tag_color.end())
			m_tag_color[tag.tag] = nullptr;
	}
	//now tags is fixed for the end of this method
    for (AppConfig::Tag &tag : get_app_config()->tags()) {
		clr_pickers_2_color.emplace_back(&m_tag_color[tag.tag], tag);
	}
	stb_sizer->Add(m_blinkers[opt_key], 0, wxRIGHT, 2);
	GUI_Descriptions::FillSizerWithModeColorDescriptions(stb_sizer, parent, clr_pickers_2_color);
	
	wxBoxSizer* parent_sizer = static_cast<wxBoxSizer*>(tab->GetSizer());
	//opt_grp->sizer->Add(sizer, 0, wxEXPAND | wxTOP, em_unit());
	parent_sizer->Add(stb_sizer, 0, wxEXPAND | wxALL, 3);

	append_preferences_option_to_searcher(opt_grp, opt_key, title);
}

void PreferencesDialog::create_settings_font_widget(wxWindow* tab, std::shared_ptr<ConfigOptionsGroup> opt_grp)
{
	wxWindow* parent = tab; //opt_grp->parent();
	wxGetApp().UpdateDarkUI(parent); //?

	const wxString title = L("Application font size");
	wxStaticBox* stb = new wxStaticBox(parent, wxID_ANY, _(title));
	if (!wxOSX) stb->SetBackgroundStyle(wxBG_STYLE_PAINT);

	const std::string opt_key = "font_pt_size";
	m_blinkers[opt_key] = new BlinkingBitmap(parent);
	m_optkey_to_optgroup[opt_key] = opt_grp;

	m_values_need_restart.push_back("font_pt_size");

	wxSizer* stb_sizer = new wxStaticBoxSizer(stb, wxHORIZONTAL);

	wxStaticText* font_example = new wxStaticText(parent, wxID_ANY, "Application text");
    int val = wxGetApp().normal_font().GetPointSize();
	SpinInput* size_sc = new SpinInput(parent, format_wxstr("%1%", val), "", wxDefaultPosition, wxSize(15 * em_unit(), -1), wxTE_PROCESS_ENTER | wxSP_ARROW_KEYS
#ifdef _WIN32
		| wxBORDER_SIMPLE
#endif 
	, 8, wxGetApp().get_max_font_pt_size());
	wxGetApp().UpdateDarkUI(size_sc);

	auto apply_font = [this, font_example, opt_key, stb_sizer, opt_grp](const int val, const wxFont& font) {
		font_example->SetFont(font);
		m_values[opt_key] = format("%1%", val);
		stb_sizer->Layout();
#ifdef __linux__
		// CallAfter([this]() { refresh_og(opt_grp); });
		CallAfter([this, opt_grp]() { refresh_og(opt_grp); });		
#else
		refresh_og(opt_grp);
#endif
	};

	auto change_value = [size_sc, apply_font](wxCommandEvent& evt) {
		const int val = size_sc->GetValue();
		wxFont font = wxGetApp().normal_font();
		font.SetPointSize(val);

		apply_font(val, font);
	};
    size_sc->Bind(wxEVT_SPINCTRL, change_value);
	size_sc->Bind(wxEVT_TEXT_ENTER, change_value);

	auto revert_btn = new ScalableButton(parent, wxID_ANY, "undo");
	revert_btn->SetToolTip(_L("Revert font to default"));
	revert_btn->Bind(wxEVT_BUTTON, [size_sc, apply_font](wxEvent& event) {
		wxFont font = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
		const int val = font.GetPointSize();
	    size_sc->SetValue(val);
		apply_font(val, font);
		});
	parent->Bind(wxEVT_UPDATE_UI, [size_sc](wxUpdateUIEvent& evt) {
		const int def_size = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT).GetPointSize();
		evt.Enable(def_size != size_sc->GetValue());
	}, revert_btn->GetId());
	
	stb_sizer->Add(m_blinkers[opt_key], 0, wxRIGHT, 2);
    stb_sizer->Add(new wxStaticText(parent, wxID_ANY, _L("Font size") + ":"), 0, wxALIGN_CENTER_VERTICAL | wxLEFT, em_unit());
    stb_sizer->Add(size_sc, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT | wxLEFT, em_unit());
    stb_sizer->Add(revert_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em_unit());
	wxBoxSizer* font_sizer = new wxBoxSizer(wxVERTICAL);
	font_sizer->Add(font_example, 1, wxALIGN_CENTER_HORIZONTAL);
    stb_sizer->Add(font_sizer, 1, wxALIGN_CENTER_VERTICAL);

	wxBoxSizer* parent_sizer = static_cast<wxBoxSizer*>(tab->GetSizer());
	//opt_grp->sizer->Add(sizer, 0, wxEXPAND | wxTOP, em_unit());
	parent_sizer->Add(stb_sizer,  0, wxEXPAND | wxALL, 3);

	append_preferences_option_to_searcher(opt_grp, opt_key, title);
}

void PreferencesDialog::create_downloader_path_sizer(wxWindow* tab, std::shared_ptr<ConfigOptionsGroup> opt_grp)
{
	wxWindow* parent = tab; //opt_grp->parent();

	wxString title = L("Download path");
	std::string opt_key = "url_downloader_dest";
	this->m_blinkers[opt_key] = new BlinkingBitmap(parent);
	m_optkey_to_optgroup[opt_key] = opt_grp;

	this->m_downloader = new DownloaderUtils::Worker(parent);

	auto sizer = new wxBoxSizer(wxHORIZONTAL);
	sizer->Add(this->m_blinkers[opt_key], 0, wxRIGHT, 2);
	sizer->Add(this->m_downloader, 1, wxALIGN_CENTER_VERTICAL);

	wxBoxSizer* parent_sizer = static_cast<wxBoxSizer*>(tab->GetSizer());
	//opt_grp->sizer->Add(sizer, 0, wxEXPAND | wxTOP, em_unit());
	parent_sizer->Add(sizer, 0, wxEXPAND | wxTOP, em_unit());

	append_preferences_option_to_searcher(opt_grp, opt_key, title);
}

void PreferencesDialog::init_highlighter(const t_config_option_key& opt_key)
{
	if (m_blinkers.find(opt_key) != m_blinkers.end())
		if (BlinkingBitmap* blinker = m_blinkers.at(opt_key); blinker) {
			m_highlighter.init(blinker);
			return;
	}
	
	assert(m_optkey_to_optgroup.find(opt_key) != m_optkey_to_optgroup.end()); 
	assert(m_optkey_to_optgroup[opt_key]);
    if (auto it = m_optkey_to_optgroup.find(opt_key); it != m_optkey_to_optgroup.end() && it->second) {
        std::pair<OG_CustomCtrl *, bool *> ctrl = it->second->get_custom_ctrl_with_blinking_ptr(opt_key, -1);
        if (ctrl.second) {
            wxWindow *window = it->second->parent();
            if (ctrl.first)
                window = ctrl.first;
            m_highlighter.init(window, ctrl.second);
        } else
            assert(false);
    }
}

} // GUI
} // Slic3r
