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

namespace Slic3r{
namespace GUI{

// ----------------------------------------------------------------------------
// ModeButton
// ----------------------------------------------------------------------------

ModeButton::ModeButton( wxWindow *          parent,
                        wxWindowID          id,
                        const std::string&  icon_name   /* = ""*/,
                        const wxString&     mode        /* = wxEmptyString*/,
                        const wxSize&       size        /* = wxDefaultSize*/,
                        const wxPoint&      pos         /* = wxDefaultPosition*/) :
    ScalableButton(parent, id, icon_name, mode, size, pos, wxBU_EXACTFIT)
{
    Init(mode);
}

ModeButton::ModeButton(wxWindow* parent,
    const wxString& mode/* = wxEmptyString*/,
    const std::string& icon_name/* = ""*/,
    int                 px_cnt/* = 16*/) :
    ScalableButton(parent, wxID_ANY, ScalableBitmap(parent, icon_name, px_cnt), mode, wxBU_EXACTFIT)
{
    Init(mode);
}

ModeButton::ModeButton(wxWindow* parent,
    const wxString&     mode,
    wxBitmap*           bitmap,
    int                 px_cnt /* = 16*/ ):
    ScalableButton(parent, wxID_ANY, ScalableBitmap(parent, *bitmap, px_cnt), mode, wxBU_EXACTFIT)
{
    Init(mode);
}

void ModeButton::Init(const wxString &mode)
{
    std::string mode_str = std::string(mode.ToUTF8());
    m_tt_focused  = Slic3r::GUI::from_u8((boost::format(_utf8(L("Switch to the %s mode"))) % mode_str).str());
    m_tt_selected = Slic3r::GUI::from_u8((boost::format(_utf8(L("Current mode is %s"))) % mode_str).str());

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


// ----------------------------------------------------------------------------
// ModeSizer
// ----------------------------------------------------------------------------

int mode_icon_px_size()
{
#ifdef __APPLE__
    return 10;
#else
    return 12;
#endif
}


ModeSizer::ModeSizer(wxWindow *parent, int hgap, int max_col) :
    wxFlexGridSizer(3, 0, hgap),
    m_parent(parent),
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
//
//
//    std::vector < std::pair < wxString, std::string >> buttons = {
//        {_L("Simple"),    "mode_simple"},
////        {_(L("Advanced")),  "mode_advanced"},
//        {_L("Advanced") /*_CTX(L_CONTEXT("Advanced", "Mode") , "Mode")*/, "mode_advanced"},
//        {_L("Expert"),    "mode_expert"},
//    };

    auto modebtnfn = [this](wxCommandEvent &event, int mode_idx) {
        Slic3r::GUI::wxGetApp().save_mode(this->m_bt_mode[mode_idx]);
        event.Skip();
    };

    m_mode_btns.reserve(name_2_color.size());
    this->SetCols(max_col != 0 ? std::min(max_col, (int)name_2_color.size()) : (int)name_2_color.size());
    for (const auto& button : name_2_color) {
        // create bitmap
        AppConfig::hsv colorToDarken = AppConfig::rgb2hsv(AppConfig::int2rgb(AppConfig::hex2int(button.second)));
        colorToDarken.v *= 0.8;
        std::map<std::string, std::string> color_replace;
        color_replace["#E70000"] = button.second;
        color_replace["#D30000"] = "#" + AppConfig::int2hex(AppConfig::rgb2int(AppConfig::hsv2rgb(colorToDarken)));
        int px_cnt = (int)(em_unit(parent) * mode_icon_px_size() * 0.1f + 0.5f);
        wxBitmap* icon = cache.load_svg("mode_expert", 0, (unsigned int)px_cnt, color_replace);
        // create bt
        m_mode_btns.push_back(new ModeButton(parent, _(button.first), icon, px_cnt));
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

void ModeSizer::msw_rescale()
{
    this->SetHGap(std::lround(m_hgap_unscaled * em_unit(m_parent)));
    for (size_t m = 0; m < m_mode_btns.size(); m++)
        m_mode_btns[m]->msw_rescale();
}


} } //namespace Slic3r GUI

