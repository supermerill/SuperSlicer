///|/ Copyright (c) Prusa Research 2018 - 2023 Oleksandra Iushchenko @YuSanka, David Kocík @kocikdav, Vojtěch Bubník @bubnikv, Enrico Turri @enricoturri1966
///|/ Copyright (c) 2021 Jurriaan Pruis
///|/
///|/ ported from lib/Slic3r/GUI/Preferences.pm:
///|/ Copyright (c) Prusa Research 2016 - 2018 Vojtěch Bubník @bubnikv
///|/ Copyright (c) Slic3r 2013 - 2014 Alessandro Ranellucci @alranel
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Preferences_hpp_
#define slic3r_Preferences_hpp_

#include "GUI.hpp"
#include "GUI_Utils.hpp"
#include "wxExtensions.hpp"

#include <wx/dialog.h>
#include <wx/timer.h>
#include <vector>
#include <map>
#include <vector>

class wxColourPickerCtrl;
class wxBookCtrlBase;
class wxSlider;
class wxRadioButton;

namespace Slic3r {

	enum  NotifyReleaseMode {
		NotifyReleaseAll,
		NotifyReleaseOnly,
		NotifyReleaseNone
	};

namespace GUI {

class ConfigOptionsGroup;
class OG_CustomCtrl;

namespace DownloaderUtils {
	class Worker;
}

class PreferencesDialog : public DPIDialog
{
	std::map<std::string, std::string>	m_values;
	std::vector<std::string>			m_values_need_restart;
	std::vector<std::vector<std::shared_ptr<ConfigOptionsGroup>>> m_tabid_2_optgroups;
//	std::vector<std::shared_ptr<ConfigOptionsGroup>> m_optgroups_general;
//	std::shared_ptr<ConfigOptionsGroup>	m_optgroup_camera;
//	std::vector<std::shared_ptr<ConfigOptionsGroup>> m_optgroups_gui;
//	std::vector<std::shared_ptr<ConfigOptionsGroup>> m_optgroups_colors;
//	std::shared_ptr<ConfigOptionsGroup> m_optgroup_other;
//#if ENABLE_ENVIRONMENT_MAP
//	std::shared_ptr<ConfigOptionsGroup>	m_optgroup_render;
//#endif // ENABLE_ENVIRONMENT_MAP

	// to retreive the group to get the field, or request a refresh
	std::map<std::string, std::shared_ptr<ConfigOptionsGroup>> m_optkey_to_optgroup;

    ConfigOptionDef def_combobox_auto_switch_preview; //is this useful here?
	wxSizer*                            m_icon_size_sizer {nullptr};
	wxSlider*							m_icon_size_slider {nullptr};
	wxRadioButton*						m_rb_old_settings_layout_mode {nullptr};
	wxRadioButton*						m_rb_new_settings_layout_mode {nullptr};
	wxRadioButton*						m_rb_dlg_settings_layout_mode {nullptr};

	wxColourPickerCtrl*					m_sys_colour {nullptr};
	wxColourPickerCtrl*					m_mod_colour {nullptr};
	wxColourPickerCtrl*					m_def_colour {nullptr};
	wxColourPickerCtrl*					m_phony_colour {nullptr};

	std::vector<wxColour>				m_mode_palette;
	std::map<ConfigOptionMode, wxColourPickerCtrl*> m_tag_color;

	DownloaderUtils::Worker*			m_downloader { nullptr };

	wxBookCtrlBase*						tabs {nullptr};

    bool                                isOSX {false};
	bool								m_settings_layout_changed {false};
	bool								m_seq_top_layer_only_changed{ false };
	bool								m_recreate_GUI{false};

	int									m_custom_toolbar_size{-1};
	bool								m_use_custom_toolbar_size{false};

public:
	explicit PreferencesDialog(wxWindow* paren);
	~PreferencesDialog() = default;

	bool settings_layout_changed() const { return m_settings_layout_changed; }
	bool seq_top_layer_only_changed() const { return m_seq_top_layer_only_changed; }
	bool recreate_GUI() const { return m_recreate_GUI; }
	void	build();
	void	update_ctrls_alignment();
	void	accept(wxEvent&);
	void    revert(wxEvent&);
	void	show(const std::string& highlight_option = std::string(), const std::string& group_name = std::string());

protected:
	void msw_rescale();
	void on_dpi_changed(const wxRect& suggested_rect) override { msw_rescale(); }
	void on_sys_color_changed() override;
    void layout();
	void clear_cache();
	void refresh_og(std::shared_ptr<ConfigOptionsGroup> og);
    void create_icon_size_slider(wxWindow* tab, std::shared_ptr<ConfigOptionsGroup> opt_grp);
    void create_settings_mode_widget(wxWindow* tab, std::shared_ptr<ConfigOptionsGroup> opt_grp);
	void create_options_tab(const wxString& title);
    std::shared_ptr<ConfigOptionsGroup> create_options_group(const wxString& title, wxBookCtrlBase* tabs, int page_idx);
    void create_settings_text_color_widget(wxWindow* tab, std::shared_ptr<ConfigOptionsGroup> opt_grp);
    void create_settings_mode_color_widget(wxWindow* tab, std::shared_ptr<ConfigOptionsGroup> opt_grp);
    void create_settings_font_widget(wxWindow* tab, std::shared_ptr<ConfigOptionsGroup> opt_grp);
    void create_downloader_path_sizer(wxWindow* tab, std::shared_ptr<ConfigOptionsGroup> opt_grp);
	void init_highlighter(const t_config_option_key& opt_key);
	std::vector<ConfigOptionsGroup*> optgroups();

	void append_bool_option( std::shared_ptr<ConfigOptionsGroup> optgroup,
								const std::string& opt_key,
								const std::string& label,
								const std::string& tooltip,
								bool def_val,
								ConfigOptionMode mode = ConfigOptionMode::comNone);
	void append_int_option( std::shared_ptr<ConfigOptionsGroup> optgroup,
								const std::string& opt_key,
								const std::string& label,
								const std::string& tooltip,
								int option_width,
								int def_val,
								ConfigOptionMode mode = ConfigOptionMode::comNone,
								int32_t min = std::numeric_limits<int32_t>::min(),
								int32_t max = std::numeric_limits<int32_t>::max());
	
	void append_color_option( std::shared_ptr<ConfigOptionsGroup> optgroup,
								const std::string& opt_key,
								const std::string& label,
								const std::string& tooltip,
								std::string color_str,
								ConfigOptionMode mode = ConfigOptionMode::comNone);
	template<typename EnumType>
	void append_enum_option( std::shared_ptr<ConfigOptionsGroup> optgroup,
								const std::string& opt_key,
								const std::string& label,
								const std::string& tooltip,
								const ConfigOption* def_val,
								std::initializer_list<std::pair<std::string_view, std::string_view>> enum_values,
								ConfigOptionMode mode = ConfigOptionMode::comNone);

	HighlighterForWx						m_highlighter;
	std::map<std::string, BlinkingBitmap*>	m_blinkers;
};

} // GUI
} // Slic3r


#endif /* slic3r_Preferences_hpp_ */
