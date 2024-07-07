#ifndef slic3r_Preferences_hpp_
#define slic3r_Preferences_hpp_

#include "GUI.hpp"
#include "GUI_Utils.hpp"

#include <wx/dialog.h>
#include <wx/timer.h>
#include <vector>
#include <map>
#include <vector>

class wxColourPickerCtrl;
class wxBookCtrlBase;

namespace Slic3r {

    enum  NotifyReleaseMode {
        NotifyReleaseAll,
        NotifyReleaseOnly,
        NotifyReleaseNone
    };

namespace GUI {

class ConfigOptionsGroup;
class OG_CustomCtrl;

class PreferencesDialog : public DPIDialog
{
    std::map<std::string, std::string>    m_values;
    std::vector<std::string>            m_values_need_restart;
    std::vector<std::shared_ptr<ConfigOptionsGroup>> m_optgroups_general;
    std::shared_ptr<ConfigOptionsGroup>    m_optgroup_camera;
    std::vector<std::shared_ptr<ConfigOptionsGroup>> m_optgroups_gui;
    std::vector<std::shared_ptr<ConfigOptionsGroup>> m_optgroups_colors;

#if ENABLE_ENVIRONMENT_MAP
    std::shared_ptr<ConfigOptionsGroup>    m_optgroup_render;
#endif // ENABLE_ENVIRONMENT_MAP

    ConfigOptionDef def_combobox_auto_switch_preview;

    wxSizer*                            m_icon_size_sizer;
    wxColourPickerCtrl*                    m_sys_colour {nullptr};
    wxColourPickerCtrl*                    m_mod_colour {nullptr};
    wxColourPickerCtrl*                    m_def_colour {nullptr};
    wxColourPickerCtrl*                    m_phony_colour {nullptr};
    bool                                isOSX {false};
    bool                                m_settings_layout_changed {false};
    bool                                m_seq_top_layer_only_changed{ false };
    bool                                m_recreate_GUI{false};

public:
    explicit PreferencesDialog(wxWindow* parent, int selected_tab = 0, const std::string& highlight_opt_key = std::string());
    ~PreferencesDialog() = default;

    bool settings_layout_changed() const { return m_settings_layout_changed; }
    bool seq_top_layer_only_changed() const { return m_seq_top_layer_only_changed; }
    bool recreate_GUI() const { return m_recreate_GUI; }
    void    build(size_t selected_tab = 0);
    void    update_ctrls_alignment();
    void    accept(wxEvent&);

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;
    void layout();
    void create_icon_size_slider(ConfigOptionsGroup* parent);
    void create_settings_mode_widget(wxWindow* tab);
    std::shared_ptr<ConfigOptionsGroup> create_options_group(const wxString& title, wxBookCtrlBase* tabs, int page_idx);
    void create_settings_text_color_widget(wxWindow* tab);
    void init_highlighter(const t_config_option_key& opt_key);
    std::vector<ConfigOptionsGroup*> optgroups();

    struct PreferencesHighlighter
    {
        void set_timer_owner(wxEvtHandler* owner, int timerid = wxID_ANY);
        void init(std::pair<OG_CustomCtrl*, bool*>);
        void blink();
        void invalidate();

    private:
        OG_CustomCtrl* m_custom_ctrl{ nullptr };
        bool* m_show_blink_ptr{ nullptr };
        int                m_blink_counter{ 0 };
        wxTimer         m_timer;
    }
    m_highlighter;
};

} // GUI
} // Slic3r


#endif /* slic3r_Preferences_hpp_ */
