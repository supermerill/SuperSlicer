///|/ Copyright (c) SuperSlicer 2026 Durand R?mi @supermerill
///|/ Copyright (c) Prusa Research 2018 - 2019 Oleksandra Iushchenko @YuSanka, Vojtěch Bubník @bubnikv, Lukáš Matěna @lukasmatena
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef RAMMING_CHART_H_
#define RAMMING_CHART_H_

#include <vector>

#include <wx/wxprec.h>

#include "libslic3r/Config/ConfigOption.hpp"
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
        m_moveable_area = wxRect2DDouble(0.0, 0.0, 20., 20.);
        m_visible_area = wxRect2DDouble(0.0, 0.0, 20., 20.);
        m_buttons.clear();
        if (initial_buttons.size()>0)
            for (const auto& pair : initial_buttons)
                m_buttons.emplace_back(wxPoint2DDouble(pair.first,pair.second), *this);
        recalculate_line();
    }
    void set_xy_range(float min_x, float min_y, float max_x, float max_y) {
        if (!std::isnan(min_x)) {
            m_moveable_area.SetLeft(min_x);
        }
        if (!std::isnan(max_x) && max_x > m_moveable_area.GetLeft()) {
            m_moveable_area.SetRight(max_x);
        }
        if (!std::isnan(min_y)) {
            m_moveable_area.SetTop(min_y);
        }
        if (!std::isnan(max_y) && max_y > m_moveable_area.GetTop()) {
            m_moveable_area.SetBottom(max_y);
        }
        //recompute visible area (moveable are +5% on each side)
        m_visible_area.m_x = m_moveable_area.m_x - m_moveable_area.m_width / 20;
        m_visible_area.m_y = m_moveable_area.m_y - m_moveable_area.m_height / 20;
        m_visible_area.m_width = m_moveable_area.m_width * 1.1;
        m_visible_area.m_height = m_moveable_area.m_height * 1.1;
        recalculate_line();
    }
    void  set_manual_points_manipulation(bool manip) { m_manual_points_manipulation = manip; }
    void  set_x_label(const wxString &label, float incr = 0.1f);
    void  set_y_label(const wxString &label, float incr = 0.1f);
    void  set_no_point_label(const wxString &label) { m_no_point_legend = label; }
    void  set_type(Slic3r::GraphData::GraphType type) { m_type = type; recalculate_line(); }
    Slic3r::GraphData::GraphType  get_type() const { return m_type; }

    bool has_selected_point() { return m_clicked == nullptr; }
    wxPoint2DDouble get_selected_point_position() { assert(m_clicked); return m_clicked->get_pos(); }
    void set_selected_point_position(wxPoint2DDouble new_pos) {
        assert(m_clicked);
        m_clicked->move(new_pos.m_x - m_clicked->get_pos().m_x, new_pos.m_y - m_clicked->get_pos().m_y);
        this->recalculate_line();
    }
    void set_selection_point_callback(std::function<void(std::optional<wxPoint2DDouble>)> func) { m_func_selected_point_moved = func; }


    float get_volume() const { return m_total_volume; }
    float get_max_x() const { return m_moveable_area.m_x + m_moveable_area.m_width; }
    float get_min_x() const { return m_moveable_area.m_x; }

    std::vector<float> get_value_samples(float sampling) const; //returns sampled values
    std::vector<std::pair<float,float>> get_buttons() const; // returns buttons position
    void                                set_buttons(std::vector<std::pair<float, float>>, bool freeze_front = false, bool freeze_back = false);

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
    static const bool fixed_x = false;
    static const int side = 10; // side of draggable button

    Slic3r::GraphData::GraphType m_type = Slic3r::GraphData::GraphType::LINEAR;
    const int scale_unit;
    int legend_side;
    wxString m_x_legend;
    wxString m_y_legend;
    double m_x_legend_incr = 0.1f;
    int m_x_precision = 0;
    double m_y_legend_incr = 1.f;
    int m_y_precision = 0;
    wxString m_no_point_legend;
    bool m_manual_points_manipulation = false;

    wxBrush yellow_brush = wxBrush(wxColour(240,240,0), wxBRUSHSTYLE_SOLID);
    wxBrush orange_brush = wxBrush(wxColour(255,150,0), wxBRUSHSTYLE_SOLID);

    class ButtonToDrag {
        static inline const int pow10[] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000};
    public:
        bool operator<(const ButtonToDrag& a) const { return m_pos.m_x < a.m_pos.m_x; }
        ButtonToDrag(wxPoint2DDouble pos, const Chart &chart) : m_pos{pos}, m_pos_following{pos}, m_chart(&chart), freeze(false) {};
        wxPoint2DDouble get_pos() const { return m_pos; }
        void move(double x, double y) {
            m_pos_following.m_x += x;
            m_pos_following.m_y += y;
            m_pos = m_pos_following;
            m_pos.m_x = std::max(m_chart->m_moveable_area.m_x,
                                 std::min(m_chart->m_moveable_area.m_x + m_chart->m_moveable_area.m_width, m_pos.m_x));
            m_pos.m_y = std::max(m_chart->m_moveable_area.m_y,
                                 std::min(m_chart->m_moveable_area.m_y + m_chart->m_moveable_area.m_height, m_pos.m_y));
            if (m_chart->m_x_precision <= 0) {
                m_pos.m_x = int(m_pos.m_x);
            } else if (m_chart->m_x_precision < 10){
                //m_pos.m_x = int(m_pos.m_x * pow10[m_chart->m_x_precision]) / double(pow10[m_chart->m_x_precision]);
                m_pos.m_x = std::round(m_pos.m_x / m_chart->m_x_legend_incr) * m_chart->m_x_legend_incr;
            }
            if (m_chart->m_y_precision <= 0) {
                m_pos.m_y = int(m_pos.m_y);
            } else if (m_chart->m_y_precision < 10){
                m_pos.m_y = int(m_pos.m_y * pow10[m_chart->m_y_precision]) / double(pow10[m_chart->m_y_precision]);
            }
        }
        bool freeze = false;
    private:
        wxPoint2DDouble m_pos_following;    // position in math coordinates
        wxPoint2DDouble m_pos;              // position in math coordinates
        const Chart *m_chart;
    };

    wxPoint math_to_screen(const wxPoint2DDouble& math) const {
        wxPoint screen;
        screen.x = (math.m_x-m_visible_area.m_x) * (m_rect.GetWidth()  / m_visible_area.m_width  );
        screen.y = (math.m_y-m_visible_area.m_y) * (m_rect.GetHeight() / m_visible_area.m_height );
        screen.y *= -1;
        screen += m_rect.GetLeftBottom();
        return screen;
    }
    wxPoint2DDouble screen_to_math(const wxPoint& screen) const {
        wxPoint2DDouble math = screen;
        math -= m_rect.GetLeftBottom();
        math.m_y *= -1;
        math.m_x *= m_visible_area.m_width   / m_rect.GetWidth();    // scales to [0;1]x[0,1]
        math.m_y *= m_visible_area.m_height / m_rect.GetHeight();
        return (math+m_visible_area.GetLeftTop());
    }

    int which_button_is_clicked(const wxPoint& point) const;

    void recalculate_line();
    void recalculate_volume();

    wxRect m_rect;                  // rectangle on screen the chart is mapped into (screen coordinates)
    wxPoint m_previous_mouse;
    bool m_is_hover_point = false;
    wxPoint2DDouble m_mouse_hover_point;
    std::vector<ButtonToDrag> m_buttons;
    std::vector<int> m_line_to_draw;
    wxRect2DDouble m_visible_area;
    wxRect2DDouble m_moveable_area;
    ButtonToDrag* m_dragged = nullptr;
    ButtonToDrag* m_clicked = nullptr;
    wxPoint2DDouble m_last_clicked_position;
    size_t button_idx_hover = size_t(-1);
    float m_total_volume = 0.f;
    std::optional<std::function<void(std::optional<wxPoint2DDouble>)>> m_func_selected_point_moved;

};


#endif // RAMMING_CHART_H_
