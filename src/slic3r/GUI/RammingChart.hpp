///|/ Copyright (c) Prusa Research 2018 - 2019 Oleksandra Iushchenko @YuSanka, Vojtěch Bubník @bubnikv, Lukáš Matěna @lukasmatena
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef RAMMING_CHART_H_
#define RAMMING_CHART_H_

#include <libslic3r/Config.hpp>

#include <vector>
#include <wx/wxprec.h>
#ifndef WX_PRECOMP
    #include <wx/wx.h>
#endif

wxDECLARE_EVENT(EVT_SLIC3R_CHART_CHANGED, wxCommandEvent);


class Chart : public wxWindow {
public:

    Chart(wxWindow* parent, wxRect rect,const std::vector<std::pair<float,float>>& initial_buttons, int scale_unit=10) :
        wxWindow(parent,wxID_ANY,rect.GetTopLeft(),rect.GetSize()),
        scale_unit(scale_unit), legend_side(5*scale_unit)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        m_rect       = wxRect(wxPoint(2 * legend_side, 0), rect.GetSize() - wxSize(2 * legend_side, legend_side));
        visible_area = wxRect2DDouble(0.0, 0.0, 20., 20.);
        m_buttons.clear();
        if (initial_buttons.size()>0)
            for (const auto& pair : initial_buttons)
                m_buttons.push_back(wxPoint2DDouble(pair.first,pair.second));
        recalculate_line();
    }
    void set_xy_range(float min_x, float min_y, float max_x, float max_y) {
        if (!std::isnan(min_x)) {
            visible_area.SetLeft(min_x);
        }
        if (!std::isnan(max_x) && max_x > visible_area.GetLeft()) {
            visible_area.SetRight(max_x);
        }
        if (!std::isnan(min_y)) {
            visible_area.SetTop(min_y);
        }
        if (!std::isnan(max_y) && max_y > visible_area.GetTop()) {
            visible_area.SetBottom(max_y);
        }
        recalculate_line();
    }
    void  set_manual_points_manipulation(bool manip) { m_manual_points_manipulation = manip; }
    void  set_x_label(const wxString &label, float incr = 0.1f);
    void  set_y_label(const wxString &label, float incr = 0.1f);
    void  set_no_point_label(const wxString &label) { m_no_point_legend = label; }
    void  set_type(Slic3r::GraphData::GraphType type) { m_type = type; recalculate_line(); }
    Slic3r::GraphData::GraphType  get_type() const { return m_type; }

    float get_volume() const { return m_total_volume; }
    float get_max_x() const { return visible_area.m_x + visible_area.m_width; }
    float get_min_x() const { return visible_area.m_x; }
    
    std::vector<float> get_value_samples(float sampling) const; //returns sampled values
    std::vector<std::pair<float,float>> get_buttons() const; // returns buttons position
    void                                set_buttons(std::vector<std::pair<float, float>>);
    
    void draw();
    
    void mouse_clicked(wxMouseEvent& event);
    void mouse_right_button_clicked(wxMouseEvent& event);
    void mouse_moved(wxMouseEvent& event);
    void mouse_double_clicked(wxMouseEvent& event);
    void mouse_left_window(wxMouseEvent &);
    void mouse_released(wxMouseEvent &);
    void paint_event(wxPaintEvent&) { draw(); }
    DECLARE_EVENT_TABLE()
    


        
private:
    static const bool fixed_x = true;
    static const int side = 10; // side of draggable button

    Slic3r::GraphData::GraphType m_type = Slic3r::GraphData::GraphType::LINEAR;
    const int scale_unit;
    int legend_side;
    wxString m_x_legend;
    wxString m_y_legend;
    float m_x_legend_incr = 0.1f;
    int m_x_precision = 0;
    float m_y_legend_incr = 1.f;
    int m_y_precision = 0;
    wxString m_no_point_legend;
    bool m_manual_points_manipulation = false;

    wxBrush orange_brush = wxBrush(wxColour(255,150,0), wxBRUSHSTYLE_SOLID);

    class ButtonToDrag {
    public:
        bool operator<(const ButtonToDrag& a) const { return m_pos.m_x < a.m_pos.m_x; }
        ButtonToDrag(wxPoint2DDouble pos) : m_pos{pos} {};
        wxPoint2DDouble get_pos() const { return m_pos; }            
        void move(double x,double y) { m_pos.m_x+=x; m_pos.m_y+=y; }
    private:
        wxPoint2DDouble m_pos;              // position in math coordinates                       
    };
    
    
    
    wxPoint math_to_screen(const wxPoint2DDouble& math) const {
        wxPoint screen;
        screen.x = (math.m_x-visible_area.m_x) * (m_rect.GetWidth()  / visible_area.m_width  );
        screen.y = (math.m_y-visible_area.m_y) * (m_rect.GetHeight() / visible_area.m_height );
        screen.y *= -1;
        screen += m_rect.GetLeftBottom();            
        return screen;
    }
    wxPoint2DDouble screen_to_math(const wxPoint& screen) const {
        wxPoint2DDouble math = screen;
        math -= m_rect.GetLeftBottom();
        math.m_y *= -1;
        math.m_x *= visible_area.m_width   / m_rect.GetWidth();    // scales to [0;1]x[0,1]
        math.m_y *= visible_area.m_height / m_rect.GetHeight();
        return (math+visible_area.GetLeftTop());
    }
        
    int which_button_is_clicked(const wxPoint& point) const {
        if (!m_rect.Contains(point))
            return -1;
        int dist = 99999;
        int idx = -1;
        for (size_t i = 0; i < m_buttons.size(); ++i) {
            wxRect rect(math_to_screen(m_buttons[i].get_pos())-wxPoint(side,side),wxSize(side*2,side*2)); // bounding rectangle of this button
            if (rect.Contains(point)) {
                int new_dist = std::abs(rect.x + rect.width/2 - point.x) + std::abs(rect.y + rect.height/2 - point.y);
                if (new_dist < dist) {
                    dist = new_dist;
                    idx = int(i);
                }
            }
        }
        return idx;
    }
        
        
    void recalculate_line();
    void recalculate_volume();
     
    
    wxRect m_rect;                  // rectangle on screen the chart is mapped into (screen coordinates)
    wxPoint m_previous_mouse;
    wxPoint2DDouble m_mouse_hover_point;
    std::vector<ButtonToDrag> m_buttons;
    std::vector<int> m_line_to_draw;
    wxRect2DDouble visible_area;
    ButtonToDrag* m_dragged = nullptr;
    size_t button_idx_hover = size_t(-1);
    float m_total_volume = 0.f;  
    
};


#endif // RAMMING_CHART_H_