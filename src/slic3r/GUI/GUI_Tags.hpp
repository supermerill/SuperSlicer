///|/ Copyright (c) SuperSlicer 2026 Durand R?mi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_GUI_Tags_hpp_
#define slic3r_GUI_Tags_hpp_

#include <wx/checklst.h>
#include <wx/combo.h>

#include "libslic3r/Config/ConfigOption.hpp"

#include "wxExtensions.hpp"
namespace Slic3r {
namespace GUI {


// ----------------------------------------------------------------------------
// ModeButton
// ----------------------------------------------------------------------------

class ModeButton : public ScalableButton
{
public:
    //ModeButton(
    //    wxWindow*           parent,
    //    wxWindowID          id,
    //    const std::string&  icon_name = "",
    //    const wxString&     mode = wxEmptyString,
    //    const wxSize&       size = wxDefaultSize,
    //    const wxPoint&      pos = wxDefaultPosition);

    //ModeButton(
    //    wxWindow*           parent,
    //    const wxString&     mode,
    //    const std::string&  icon_name,
    //    int                 px_cnt);

    //ModeButton(wxWindow* parent,
    //    const wxString&     mode,
    //    wxBitmap*           bitmap,
    //    int                 px_cnt);
		
     //ModeButton(
     //    wxWindow*           parent,
     //    uint64_t            mode_mask,/*ConfigOptionMode*/
     //    const wxString&     mode = wxEmptyString,
     //    int                 px_cnt = 16);
    ModeButton(
        wxWindow          *parent,
        const std::string &m_mode_name,
        int                px_cnt = 16);

    ~ModeButton() {}

    void Init(const wxString& mode);

    void    OnButton(wxCommandEvent& event);
    void    OnEnterBtn(wxMouseEvent& event) { focus_button(true); event.Skip(); }
    void    OnLeaveBtn(wxMouseEvent& event) { focus_button(m_is_selected); event.Skip(); }

    void    SetState(const bool state);
    void    update_bitmap();
    bool    is_selected() { return m_is_selected; }
    void    sys_color_changed() override;

protected:
    void    focus_button(const bool focus);

private:
    bool        m_is_selected = false;
    // uint64_t    m_mode_mask {uint64_t(-1)};
	std::string m_mode_name;

    wxString    m_tt_selected;
    wxString    m_tt_focused;
    wxBitmapBundle    m_bmp;
};



// ----------------------------------------------------------------------------
// ModeSizer
// ----------------------------------------------------------------------------

class ModeSizer : public wxFlexGridSizer
{
public:
    ModeSizer( wxWindow *parent, int hgap, int max_col);
    ~ModeSizer() {}

    void SetMode(const ConfigOptionMode mode);

    void set_items_flag(int flag);
    void set_items_border(int border);

    void sys_color_changed();
    void update_mode_markers();
    const std::vector<ModeButton*>& get_btns() { return m_mode_btns; }

private:
    std::vector<ConfigOptionMode> m_bt_mode;
    std::vector<ModeButton*> m_mode_btns;
    double                   m_hgap_unscaled;
};

} // namespace slic3r
} // namespace GUI
#endif // slic3r_GUI_Tags_hpp_
