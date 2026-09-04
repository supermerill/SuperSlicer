///|/ Copyright (c) SuperSlicer 2026 Durand R?mi @supermerill
///|/ Copyright (c) Prusa Research 2018 - 2019 Lukáš Matěna @lukasmatena, Vojtěch Bubník @bubnikv, Oleksandra Iushchenko @YuSanka
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef _WIPE_TOWER_DIALOG_H_
#define _WIPE_TOWER_DIALOG_H_

// WipingPanel owns the simple and advanced editors and reports committed UI
// changes. WipingDialog validates the CSV draft and copies the active setting;
// disabled diagonal matrix cells retain their values separately in the panel.
#include <functional>
#include <string>
#include <vector>

#include <wx/checkbox.h>
#include <wx/msgdlg.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include "RammingChart.hpp"
#include "Widgets/SpinInput.hpp"

class ScalableButton;

class RammingPanel : public wxPanel {
public:
    RammingPanel(wxWindow* parent);
    RammingPanel(wxWindow* parent,const std::string& data);
    std::string get_parameters();

private:
    Chart* m_chart = nullptr;
    ::SpinInput* m_widget_volume = nullptr;
    ::SpinInput* m_widget_ramming_line_width_multiplicator = nullptr;
    ::SpinInput* m_widget_ramming_step_multiplicator = nullptr;
    ::SpinInputDouble* m_widget_time = nullptr;
    int m_ramming_step_multiplicator;
    int m_ramming_line_width_multiplicator;
      
    void line_parameters_changed();
};


class RammingDialog : public wxDialog {
public:
    RammingDialog(wxWindow* parent,const std::string& parameters);    
    std::string get_parameters() { return m_output_data; }
private:
    RammingPanel* m_panel_ramming = nullptr;
    std::string m_output_data;
};







class WipingPanel : public wxPanel {
public:
    WipingPanel(wxWindow* parent, const std::vector<float>& matrix, const std::vector<float>& extruders,
                const std::vector<std::string>& extruder_colours, wxButton* widget_button,
                std::function<void()> values_changed);
    std::vector<float> read_matrix_values();
    std::vector<float> read_extruders_values();
    // Read/apply the setting belonging to the visible mode, in config order.
    std::vector<float> read_serialized_values();
    void set_serialized_values(const std::vector<float>& values);
    void toggle_advanced(bool user_action = false);
    void format_sizer(wxSizer* sizer, wxPanel* page, wxGridSizer* grid_sizer, const wxString& info, const wxString& table_title, int table_lshift=0);
        
private:
    void fill_in_matrix();
    bool advanced_matches_simple();
    void notify_values_changed();
        
    std::vector<::SpinInput*> m_old;
    std::vector<::SpinInput*> m_new;
    std::vector<std::vector<wxTextCtrl*>> edit_boxes;
    std::vector<float> m_matrix_diagonal;
    std::vector<wxColour> m_colours;
    std::function<void()> m_values_changed;
    unsigned int m_number_of_extruders  = 0;
    bool m_advanced                     = false;
    bool m_suppress_change_notification = false;
	wxPanel*	m_page_simple = nullptr;
	wxPanel*	m_page_advanced = nullptr;
    wxBoxSizer*	m_sizer = nullptr;
    wxBoxSizer* m_sizer_simple = nullptr;
    wxBoxSizer* m_sizer_advanced = nullptr;
    wxGridSizer* m_gridsizer_advanced = nullptr;
    wxButton* m_widget_button     = nullptr;
};





class WipingDialog : public wxDialog {
public:
    WipingDialog(wxWindow* parent, const std::vector<float>& matrix, const std::vector<float>& extruders, const std::vector<std::string>& extruder_colours);
    std::vector<float> get_matrix() const    { return m_output_matrix; }
    std::vector<float> get_extruders() const { return m_output_extruders; }


private:
    // Refresh without generating wxEVT_TEXT, and validate a complete CSV draft.
    void update_serialized_values();
    bool commit_serialized_values();

    WipingPanel* m_panel_wiping = nullptr;
    wxTextCtrl* m_serialized_values = nullptr;
    ScalableButton* m_copy_button = nullptr;
    bool m_updating_serialized_values = false;
    bool m_closing = false;
    std::vector<float> m_output_matrix;
    std::vector<float> m_output_extruders;
};

#endif  // _WIPE_TOWER_DIALOG_H_
