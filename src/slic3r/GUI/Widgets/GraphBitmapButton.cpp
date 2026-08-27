///|/ Copyright (c) SuperSlicer 2026 Durand R?mi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "GraphBitmapButton.hpp"

#include <wx/dcmemory.h>

#include "libslic3r/Config/ConfigOption.hpp"
#include "libslic3r/Point.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "UIColors.hpp"
const int px_cnt = 16;

GraphBitmapButton::GraphBitmapButton(wxWindow* parent, const wxSize &size/*, const wxString& name*/)
    : wxBitmapButton()
    , m_image(size)
    , m_image_disabled(size)
    , m_image_focused(size)
{
    wxBitmapButton::Create(parent, wxID_ANY, m_image, wxDefaultPosition, size, wxBORDER_NONE/*wxBORDER_SIMPLE/*, wxDefaultValidator, name*/);
#ifdef __WXMSW__
    if (parent) {
        SetBackgroundColour(parent->GetBackgroundColour());
        SetForegroundColour(parent->GetForegroundColour());
    }

#elif __WXGTK3__
    SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
#endif
#ifdef __WXOSX__ // State not fully implement on MacOS
    Bind(wxEVT_SET_FOCUS, &GraphBitmapButton::update_state, this);
    Bind(wxEVT_KILL_FOCUS, &GraphBitmapButton::update_state, this);
    Bind(wxEVT_ENTER_WINDOW, &GraphBitmapButton::update_state, this);
    Bind(wxEVT_LEAVE_WINDOW, &GraphBitmapButton::update_state, this);
#endif

    update();
}

void GraphBitmapButton::update_size()
{
#ifndef __WXGTK3__
    wxSize best_sz = wxBitmapButton::GetBestSize();
    wxBitmapButton::SetSize(best_sz);
#endif
}

void GraphBitmapButton::Update()
{
    update();
}

void GraphBitmapButton::Rescale()
{
    update();
}

void GraphBitmapButton::update()
{
#ifdef __WXOSX__
    wxCommandEvent e(wxEVT_UPDATE_UI);
    update_state(e);
#endif

    if (GetBitmapMargins().GetWidth() == 0 && !GetLabelText().IsEmpty())
        SetBitmapMargins(4, 0);
    update_size();
}

#ifdef __WXMSW__

GraphBitmapButton::State GraphBitmapButton::GetNormalState() const
{
    return State_Normal;
}

#endif

bool GraphBitmapButton::Enable(bool enable)
{
    bool result = wxBitmapButton::Enable(enable);

#ifdef __WXOSX__
    if (result) {
        m_disable = !enable;
        wxCommandEvent e(wxEVT_ACTIVATE);
        update_state(e);
    }
#endif
    return result;
}

#ifdef __WXOSX__

wxBitmap GraphBitmapButton::DoGetBitmap(State which) const
{
    if (m_disable) {
        return wxBitmapButton::DoGetBitmap(State_Disabled);
    }
    if (m_focus) {
        return wxBitmapButton::DoGetBitmap(State_Current);
    }
    return wxBitmapButton::DoGetBitmap(which);
}

void GraphBitmapButton::update_state(wxEvent & evt)
{
    evt.Skip();
    //get state for DoGetBitmap
    if (evt.GetEventType() == wxEVT_ENTER_WINDOW) {
        m_hover = true;
    } else if (evt.GetEventType() == wxEVT_LEAVE_WINDOW) {
        m_hover = false;
    } else {
        if (evt.GetEventType() == wxEVT_SET_FOCUS) {
            m_focus = true;
        } else if (evt.GetEventType() == wxEVT_KILL_FOCUS) {
            m_focus = false;
        }
        wxMouseEvent e;
        if (m_hover)
            OnEnterWindow(e);
        else
            OnLeaveWindow(e);
    }
}
    
#endif

void GraphBitmapButton::draw_bitmap(wxDC &mem_dc,
                                    wxColour Graph_color,
                                    wxSize image_size,
                                    const Slic3r::GraphSettings &graph_settings,
                                    const Slic3r::GraphData &data_storage)
{
    wxBrush foreground = mem_dc.GetBrush();
    wxBrush background = mem_dc.GetBackground();
    // clear
    mem_dc.Clear();
    // solid background isn't drawned on windows, so I force it for the DrawRectangle
    if (background.GetStyle() == wxBRUSHSTYLE_SOLID)
        mem_dc.SetBrush(background);
    // "border"
    mem_dc.DrawRectangle(0, 0, image_size.GetWidth(), image_size.GetHeight());
    mem_dc.SetBrush(foreground);
    mem_dc.SetPen(wxPen(wxColour(Graph_color)));
    // iterate from 0 to max X
    Slic3r::Pointfs points = data_storage.data();
    if (points.empty()) {
        // TODO: draw localised "disabled"
        //this->SetLabelText(_L("Disabled"));
        mem_dc.DrawText(_L("Disabled"), 1, 1);
    } else {
        //if (!GetLabelText().empty()) {
            //this->SetLabelText("");
        //}
        const int max_px_y = image_size.GetHeight() - 2;

        // Get x & y min & max
        double minx = graph_settings.min_x;
        double maxx = graph_settings.max_x;
        double miny = graph_settings.min_y;
        double maxy = graph_settings.max_y;
        for (Slic3r::Vec2d &pt : points) {
            minx = std::min(minx, pt.x());
            maxx = std::max(maxx, pt.x());
            miny = std::min(miny, pt.y());
            maxy = std::max(maxy, pt.y());
        }

        if (points.size() == 1) {
            wxCoord px_y = max_px_y - (max_px_y * (points.front().y() - miny) / (maxy - miny));
            mem_dc.DrawLine(1, 1 + px_y, image_size.GetWidth() - 1, px_y);
        } else {
            const double max_px_x = double(image_size.GetWidth() - 2);
            for (int px_x = 0; px_x < image_size.GetWidth() - 2; ++px_x) {
                const double val_x = minx + (maxx - minx) * (px_x / max_px_x);
                const double val_y = data_storage.interpolate(val_x);
                const wxCoord px_y = 1 + max_px_y - (max_px_y * (val_y - miny) / (maxy - miny));
                //draw a pixel on the right y
                mem_dc.DrawPoint(wxCoord(1 + px_x), px_y);
            }
        }
    }
}

void GraphBitmapButton::update_bitmap(const Slic3r::GraphSettings &graph_settings, const Slic3r::GraphData &data_storage)
{
    const unsigned long clr_background_disabled = Slic3r::GUI::wxGetApp().dark_mode() ?
        Slic3r::GUI::Widget::clr_background_disabled_dark :
        Slic3r::GUI::Widget::clr_background_disabled_light;
    wxBitmap  image;
    wxBitmap  image_disabled;
    wxBitmap  image_focused;
    {
        wxMemoryDC mem_dc(m_image);
        mem_dc.SetBackground(wxBrush(this->GetBackgroundColour(), wxBRUSHSTYLE_TRANSPARENT));
        mem_dc.SetPen(wxPen(Slic3r::GUI::Widget::clr_border_normal/*, 1, wxPENSTYLE_SOLID*/));
        draw_bitmap(mem_dc, Slic3r::GUI::wxGetApp().get_color_default_btn_label(), m_image.GetSize(), graph_settings, data_storage);
    }
    {
        wxMemoryDC mem_dc(m_image_disabled);
        mem_dc.SetPen(wxPen(wxColour(Slic3r::GUI::Widget::clr_border_disabled)));
        mem_dc.SetBackground(wxBrush(wxColour(clr_background_disabled), wxBRUSHSTYLE_SOLID));
        draw_bitmap(mem_dc, Slic3r::GUI::Widget::clr_foreground_disabled, m_image_disabled.GetSize(), graph_settings, data_storage);
    }
    {
        wxMemoryDC mem_dc(m_image_focused);
        mem_dc.SetBackground(wxBrush(this->GetBackgroundColour(), wxBRUSHSTYLE_TRANSPARENT));
        mem_dc.SetPen(wxPen(Slic3r::GUI::Widget::clr_border_normal));
        draw_bitmap(mem_dc, Slic3r::GUI::wxGetApp().get_color_hovered_btn_label(), m_image_focused.GetSize(), graph_settings, data_storage);
    }
    SetBitmap(m_image);
    SetBitmapCurrent(m_image_focused);
    SetBitmapDisabled(m_image_disabled);
#ifdef __WXMSW__
    SetBitmapFocus(m_image_focused);
#endif
}
