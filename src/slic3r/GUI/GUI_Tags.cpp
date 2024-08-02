#include "GUI_Tags.hpp"

#include <stdexcept>
#include <cmath>

#include <wx/sizer.h>

#include <boost/algorithm/string/replace.hpp>


#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Config.hpp"

#include "BitmapCache.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "libslic3r/Config.hpp"
#include "I18N.hpp"
#include "GUI_Utils.hpp"
#include "Plater.hpp"
#include "../Utils/MacDarkMode.hpp"
#include "BitmapComboBox.hpp"
#include "format.hpp"

namespace Slic3r{
namespace GUI{

// ----------------------------------------------------------------------------
// ModeButton
// ----------------------------------------------------------------------------

 //ModeButton::ModeButton( wxWindow *          parent,
 //                        wxWindowID          id,
 //                        const std::string&  icon_name   /* = ""*/,
 //                        const wxString&     mode        /* = wxEmptyString*/,
 //                        const wxSize&       size        /* = wxDefaultSize*/,
 //                        const wxPoint&      pos         /* = wxDefaultPosition*/) :
 //    ScalableButton(parent, id, icon_name, mode, size, pos, wxBU_EXACTFIT)
 //{
 //    Init(mode);
 //}

 //ModeButton::ModeButton(wxWindow* parent,
 //    const wxString& mode/* = wxEmptyString*/,
 //    const std::string& icon_name/* = ""*/,
 //    int                 px_cnt/* = 16*/) :
 //    ScalableButton(parent, wxID_ANY, icon_name, mode, wxDefaultSize, wxDefaultPosition, wxBU_EXACTFIT, px_cnt)

 //{
 //    Init(mode);
 //}

ModeButton::ModeButton(wxWindow *parent, const std::string &mode_name, int px_cnt)
    : ScalableButton(parent, wxID_ANY, "mode_expert", _(mode_name), wxDefaultSize, wxDefaultPosition, wxBU_EXACTFIT, px_cnt),
      m_mode_name(mode_name)
{
    update_bitmap();
    Init(_(mode_name));
}

 //ModeButton::ModeButton(wxWindow* parent,
 //    const wxString&     mode,
 //    wxBitmap*           bitmap,
 //    int                 px_cnt /* = 16*/ ):
 //    ScalableButton(parent, wxID_ANY, icon_name, mode, wxDefaultSize, wxDefaultPosition, wxBU_EXACTFIT, px_cnt)
 //{
 //    update_bitmap();
 //    Init(mode);
 //}

//ModeButton::ModeButton( wxWindow*           parent,
//                        std::string         mode_name,/*ConfigOptionMode*/
//                        const wxString&     mode_label /*= wxEmptyString*/,
//                        int                 px_cnt /*= = 16*/) :
//    ScalableButton(parent, wxID_ANY, "", mode_label, wxDefaultSize, wxDefaultPosition, wxBU_EXACTFIT, px_cnt),
//    m_mode_mask(mode_mask), m_mode_name(mode_name)
//{
//    update_bitmap();
//    Init(mode_label);
//}

void ModeButton::Init(const wxString &mode)
{
    m_tt_focused  = Slic3r::GUI::format_wxstr(_L("Switch to the %s mode"), mode);
    m_tt_selected = Slic3r::GUI::format_wxstr(_L("Current mode is %s"), mode);

    SetBitmapMargins(3, 0);

    //button events
    Bind(wxEVT_BUTTON,          &ModeButton::OnButton, this);
    Bind(wxEVT_ENTER_WINDOW,    &ModeButton::OnEnterBtn, this);
    Bind(wxEVT_LEAVE_WINDOW,    &ModeButton::OnLeaveBtn, this);
}

void ModeButton::OnButton(wxCommandEvent& event)
{
    m_is_selected = true;
    focus_button(m_is_selected);

    event.Skip();
}

void ModeButton::SetState(const bool state)
{
    m_is_selected = state;
    focus_button(m_is_selected);
    SetToolTip(state ? m_tt_selected : m_tt_focused);
}

int mode_icon_px_size()
{
#ifdef __APPLE__
    return 10;
#else
    return 12;
#endif
}

void ModeButton::update_bitmap()
{
    //get color
    std::string color_hash;
    for (const AppConfig::Tag& tag : Slic3r::GUI::get_app_config()->tags()) {
        if (tag.name == m_mode_name) {
            color_hash = tag.color_hash;
            break;
        }
    }
    assert(!color_hash.empty());
    if(color_hash.empty())
        return;

    // create bitmap
    hsv color_to_darken = rgb2hsv(int2rgb(hex2int(color_hash)));
    if (wxGetApp().dark_mode()) {
        color_to_darken.v *= 0.8;
    }
    Slic3r::ColorReplaces color_replaces;
    assert(!color_hash.empty() && color_hash[0]=='#');
    color_replaces.add("#E70000", color_hash);
    color_replaces.add("#D30000", "#" + int2hex(rgb2int(hsv2rgb(color_to_darken))));
    int px_cnt = (int)(em_unit(m_parent) * mode_icon_px_size() * 0.1f + 0.5f);
    // wxBitmap* icon = cache.load_svg("mode_expert", 0, (unsigned int)px_cnt, color_replace);
    std::string icon_template_name = "mode_expert";
    auto bundle = get_bmp_bundle(icon_template_name, m_bmp_width, m_bmp_height, color_replaces);
    m_bmp = *bundle;
    // m_bmp = *get_bmp_bundle("mode", m_bmp_width, m_bmp_height, Slic3r::GUI::wxGetApp().get_mode_btn_color(m_mode_mask));
	
    SetBitmap(m_bmp);
    SetBitmapCurrent(m_bmp);
    SetBitmapPressed(m_bmp);
}

void ModeButton::focus_button(const bool focus)
{
    const wxFont& new_font = focus ? 
                             Slic3r::GUI::wxGetApp().bold_font() : 
                             Slic3r::GUI::wxGetApp().normal_font();

    SetFont(new_font);
#ifdef _WIN32
    GetParent()->Refresh(); // force redraw a background of the selected mode button
#else
    SetForegroundColour(wxSystemSettings::GetColour(focus ? wxSYS_COLOUR_BTNTEXT : 
#if defined (__linux__) && defined (__WXGTK3__)
        wxSYS_COLOUR_GRAYTEXT
#elif defined (__linux__) && defined (__WXGTK2__)
        wxSYS_COLOUR_BTNTEXT
#else 
        wxSYS_COLOUR_BTNSHADOW
#endif    
    ));
#endif /* no _WIN32 */

    Refresh();
    Update();
}

void ModeButton::sys_color_changed()
{
    Slic3r::GUI::wxGetApp().UpdateDarkUI(this, m_has_border);
    update_bitmap();
}


// ----------------------------------------------------------------------------
// ModeSizer
// ----------------------------------------------------------------------------

ModeSizer::ModeSizer(wxWindow *parent, int hgap, int max_col) :
    wxFlexGridSizer(3, 0, hgap),
    m_hgap_unscaled((double)(hgap)/em_unit(parent))
{
    static BitmapCache cache;
    SetFlexibleDirection(wxHORIZONTAL);

    std::vector<std::pair<std::string, std::string>> name_2_color;
    // load colors from ui file
    for (const AppConfig::Tag& tag : Slic3r::GUI::get_app_config()->tags()) {
        name_2_color.emplace_back(tag.name, tag.color_hash);
        m_bt_mode.push_back(tag.tag);
    }

    auto modebtnfn = [this](wxCommandEvent &event, int mode_idx) {
        if (Slic3r::GUI::wxGetApp().save_mode(this->m_bt_mode[mode_idx]))
            event.Skip();
        else
            SetMode(Slic3r::GUI::wxGetApp().get_mode());
    };
    
    m_mode_btns.reserve(name_2_color.size());
    this->SetCols(max_col != 0 ? std::min(max_col, (int)name_2_color.size()) : (int)name_2_color.size());
    for (const auto& button : name_2_color) {
        // create bt
        m_mode_btns.push_back(new ModeButton(parent, button.first, mode_icon_px_size()));
        // add event
        m_mode_btns.back()->Bind(wxEVT_BUTTON, std::bind(modebtnfn, std::placeholders::_1, int(m_mode_btns.size() - 1)));
        Add(m_mode_btns.back());
    }
}

void ModeSizer::SetMode(ConfigOptionMode mode)
{
    for (size_t m = 0; m < m_mode_btns.size(); m++)
        m_mode_btns[m]->SetState( (m_bt_mode[m] & mode) != 0);
}

void ModeSizer::set_items_flag(int flag)
{
    for (wxSizerItem* item : this->GetChildren())
        item->SetFlag(flag);
}

void ModeSizer::set_items_border(int border)
{
    for (wxSizerItem* item : this->GetChildren())
        item->SetBorder(border);
}

void ModeSizer::sys_color_changed()
{
    for (ModeButton* btn : m_mode_btns)
        btn->sys_color_changed();
}

void ModeSizer::update_mode_markers()
{
    for (ModeButton* btn : m_mode_btns)
        btn->update_bitmap();
}


} } //namespace Slic3r GUI

