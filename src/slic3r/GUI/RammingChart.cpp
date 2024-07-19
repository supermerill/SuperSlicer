///|/ Copyright (c) Prusa Research 2018 - 2021 Oleksandra Iushchenko @YuSanka, Lukáš Matěna @lukasmatena, Vojtěch Bubník @bubnikv
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include <algorithm>
#include <wx/dcbuffer.h>

#include "RammingChart.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"

wxDEFINE_EVENT(EVT_SLIC3R_CHART_CHANGED, wxCommandEvent);

void Chart::set_x_label(const wxString &label, float incr)
{
    m_x_legend      = label;
    m_x_legend_incr = incr;
    for (int i = 0; i < 6; i++) {
        if (std::round(m_x_legend_incr * std::pow(10, i)) > 0) {
            m_x_precision = i;
            break;
        }
    }
}

void Chart::set_y_label(const wxString &label, float incr)
{
    m_y_legend      = label;
    m_y_legend_incr = incr;
    for (int i = 0; i < 6; i++) {
        if (std::round(m_y_legend_incr * std::pow(10, i)) > 0) {
            m_y_precision = i;
            break;
        }
    }
}

void Chart::draw() {
    wxAutoBufferedPaintDC dc(this); // unbuffered DC caused flickering on win

    dc.SetBrush(GetBackgroundColour());
    dc.SetPen(GetBackgroundColour());
    dc.DrawRectangle(GetClientRect());  // otherwise the background would end up black on windows

#ifdef _WIN32
    dc.SetPen(wxPen(GetForegroundColour()));
    dc.SetBrush(wxBrush(Slic3r::GUI::wxGetApp().get_highlight_default_clr()));
#else
    dc.SetPen(*wxBLACK_PEN);
    dc.SetBrush(*wxWHITE_BRUSH);
#endif
    dc.DrawRectangle(m_rect);
    
    if (m_line_to_draw.empty()) {
        dc.DrawText(m_no_point_legend, wxPoint(m_rect.GetLeft() + m_rect.GetWidth() / 2 - 2*legend_side,
                                                     m_rect.GetBottom() - m_rect.GetHeight() / 2));
    }
    
    
    if (!m_line_to_draw.empty()) {
        for (unsigned int i=0;i<m_line_to_draw.size()-2;++i) {
            int color = 510*((m_rect.GetBottom()-(m_line_to_draw)[i])/double(m_rect.GetHeight()));
            dc.SetPen( wxPen( wxColor(std::min(255,color),255-std::max(color-255,0),0), 1 ) );
            dc.DrawLine(m_rect.GetLeft()+1+i, (m_line_to_draw)[i], m_rect.GetLeft()+1+i, m_rect.GetBottom());        
        }
#ifdef _WIN32
        dc.SetPen(wxPen(GetForegroundColour()));
#else
        dc.SetPen( wxPen( wxColor(0,0,0), 1 ) );
#endif
        for (unsigned int i=0; i<m_line_to_draw.size()-2; ++i) {
            if (this->m_type == Slic3r::GraphData::GraphType::SPLINE) {
                dc.DrawLine(m_rect.GetLeft()+i,  (m_line_to_draw)[i], m_rect.GetLeft()+i+1, (m_line_to_draw)[i+1]);
            } else if (this->m_type == Slic3r::GraphData::GraphType::LINEAR) {
                dc.DrawLine(m_rect.GetLeft()+i,  (m_line_to_draw)[i], m_rect.GetLeft()+i+1, (m_line_to_draw)[i+1]);
            } else {
                dc.DrawLine(m_rect.GetLeft()+i,  (m_line_to_draw)[i], m_rect.GetLeft()+i+1, (m_line_to_draw)[i]);
                dc.DrawLine(m_rect.GetLeft()+i+1,(m_line_to_draw)[i], m_rect.GetLeft()+i+1, (m_line_to_draw)[i+1]);
            }
        }
    }
    
    // draw draggable buttons
    dc.SetBrush(*wxBLUE_BRUSH);
#ifdef _WIN32
    dc.SetPen(wxPen(GetForegroundColour()));
#else
    dc.SetPen( wxPen( wxColor(0,0,0), 1 ) );
#endif
    for (size_t idx = 0; idx < m_buttons.size(); ++idx) {
        auto& button = m_buttons[idx];
        if (m_dragged != &button && button_idx_hover != idx) {
            if (visible_area.GetLeft() <= button.get_pos().m_x && button.get_pos().m_x <= visible_area.GetRight() &&
                visible_area.GetTop() <= button.get_pos().m_y && button.get_pos().m_y <= visible_area.GetBottom()) {
                dc.DrawCircle(math_to_screen(button.get_pos()), side / 2.);
            }
        }
    }
    if (m_dragged || button_idx_hover < m_buttons.size()) {
        dc.SetBrush(orange_brush);
        dc.DrawCircle(math_to_screen(m_dragged ? m_dragged->get_pos() : m_buttons[button_idx_hover].get_pos()),side/2.);
    }

    // draw x-axis:
    float last_mark = -10000;
    for (float math_x = int(visible_area.m_x * 10) / 10.f; math_x < (visible_area.m_x + visible_area.m_width); math_x += m_x_legend_incr) {
        int x = math_to_screen(wxPoint2DDouble(math_x,visible_area.m_y)).x;
        int y = m_rect.GetBottom();
        if (x-last_mark < legend_side) continue;
        dc.DrawLine(x,y+3,x,y-3);
        if (m_x_precision == 0) {
            dc.DrawText(wxString()<<int(math_x), wxPoint(x - scale_unit, y + 0.5 * scale_unit));
        } else if (m_x_precision < 5){
            dc.DrawText(Slic3r::to_string_nozero(math_x, m_x_precision), wxPoint(x - scale_unit, y + 0.5 * scale_unit));
        } else {
            dc.DrawText(wxString().Format(wxT("%f"), math_x), wxPoint(x - scale_unit, y + 0.5 * scale_unit));
        }
        last_mark = x;
    }
    
    // draw y-axis:
    last_mark=10000;
    for (float math_y = int(visible_area.m_y * 10) / 10.f; math_y < (visible_area.m_y + visible_area.m_height); math_y += m_y_legend_incr) {
        int y = math_to_screen(wxPoint2DDouble(visible_area.m_x,math_y)).y;
        int x = m_rect.GetLeft();
        if (last_mark-y < legend_side / 2) continue;    
        dc.DrawLine(x-3,y,x+3,y);
        if (m_y_precision == 0) {
            dc.DrawText(wxString()<<int(math_y), wxPoint(x - 2 * scale_unit, y - 0.5 * scale_unit));
        } else if (m_y_precision < 5){
            dc.DrawText(Slic3r::to_string_nozero(math_y, m_y_precision), wxPoint(x - (2 + m_y_precision * 0.5f) * scale_unit, y - 0.5 * scale_unit));
        } else {
            dc.DrawText(wxString().Format(wxT("%f"), math_y), wxPoint(x - 4 * scale_unit, y - 0.5 * scale_unit));
        }
        last_mark = y;
    }
    
    // axis labels:
    int text_width = 0;
    int text_height = 0;
    dc.GetTextExtent(m_x_legend, &text_width, &text_height);
    dc.DrawText(m_x_legend, wxPoint(0.5*(m_rect.GetRight()+m_rect.GetLeft())-text_width/2.f, m_rect.GetBottom()+0.5*legend_side));
    dc.GetTextExtent(m_y_legend, &text_width, &text_height);
    dc.DrawRotatedText(m_y_legend, wxPoint(0,0.5*(m_rect.GetBottom()+m_rect.GetTop())+text_width/2.f), 90);

    // drag value
    //
    {
        // get values
        wxPoint2DDouble pos;
        if (m_dragged) {
            pos = m_dragged->get_pos();
        } else if (m_mouse_hover_point.m_x == 0 && m_mouse_hover_point.m_y == 0) {
            pos = screen_to_math(m_previous_mouse);
        } else {
            pos = m_mouse_hover_point;
        }
        // show on bottom right
        // TODO: compute legend height instead of '3 * scale_unit'
        wxPoint ptx = math_to_screen(wxPoint2DDouble(visible_area.m_x + visible_area.m_width, visible_area.m_y));
        wxPoint pty = math_to_screen(wxPoint2DDouble(visible_area.m_x + visible_area.m_width, visible_area.m_y));
        ptx.x -= 1*legend_side;
        ptx.y -= 1*legend_side;
        pty.x -= 1*legend_side;
        pty.y -= 0.5*legend_side;
        dc.DrawText(wxString().Format(wxT("x: %.3f"), pos.m_x), ptx);
        dc.DrawText(wxString().Format(wxT("y: %.3f"), pos.m_y), pty);
    }
}

void Chart::mouse_right_button_clicked(wxMouseEvent& event) {
    if (!m_manual_points_manipulation)
        return;
    wxPoint point = event.GetPosition();
    int button_index = which_button_is_clicked(point);
    if (button_index != -1) {
        m_buttons.erase(m_buttons.begin() + button_index);
        recalculate_line();
    } else {
        // create a new point
        wxPoint point = event.GetPosition();
        if (!m_rect.Contains(point)) // the click is outside the chart
            return;
        wxPoint2DDouble dblpoint = screen_to_math(point);
        // trunc by precision
        dblpoint.m_x = int((dblpoint.m_x + this->m_x_legend_incr / 2) / this->m_x_legend_incr) * this->m_x_legend_incr;
        dblpoint.m_y = int((dblpoint.m_y + this->m_y_legend_incr / 2) / this->m_y_legend_incr) * this->m_y_legend_incr;
        //check it doesn't exist
        for (const ButtonToDrag &bt : m_buttons)
            if(bt.get_pos().m_x == dblpoint.m_x)
                return;
        m_buttons.push_back(dblpoint);
        std::sort(m_buttons.begin(), m_buttons.end());
        recalculate_line();
    }
}

void Chart::mouse_clicked(wxMouseEvent& event) {
    wxPoint point = event.GetPosition();
    int button_index = which_button_is_clicked(point);
    if ( button_index != -1) {
        m_dragged = &m_buttons[button_index];
        m_previous_mouse = point;
        m_mouse_hover_point = m_dragged->get_pos();
        Refresh();
    }
}

void Chart::mouse_moved(wxMouseEvent& event) {
    wxPoint pos = event.GetPosition();
    wxRect rect = m_rect;
    size_t button_idx_hover_old = button_idx_hover;
    button_idx_hover = size_t(-1);
    rect.Deflate(side/2.);
    if (!(rect.Contains(pos))) {  // the mouse left chart area
        mouse_left_window(event);
        return;
    }
    if (event.Dragging() && m_dragged) {
        int delta_x = pos.x - m_previous_mouse.x;
        int delta_y = pos.y - m_previous_mouse.y;
        m_dragged->move(fixed_x ? 0 : double(delta_x) / m_rect.GetWidth() * visible_area.m_width,
                        -double(delta_y) / m_rect.GetHeight() * visible_area.m_height);
        m_previous_mouse = pos;
        m_mouse_hover_point.m_x = 0;
        m_mouse_hover_point.m_y = 0;
    } else {
        int idx_bt = which_button_is_clicked(pos);
        if (idx_bt < 0) {
            m_previous_mouse = pos;
            m_mouse_hover_point.m_x = 0;
            m_mouse_hover_point.m_y = 0;
        } else {
            button_idx_hover = size_t(idx_bt);
            assert(button_idx_hover < m_buttons.size());
            m_previous_mouse = math_to_screen(m_buttons[button_idx_hover].get_pos());
            m_mouse_hover_point = m_buttons[button_idx_hover].get_pos();
        }
    }
    recalculate_line();
    Refresh();
}

void Chart::mouse_double_clicked(wxMouseEvent& event) {
    if (!m_manual_points_manipulation)
        return;
    wxPoint point = event.GetPosition();
    if (!m_rect.Contains(point))     // the click is outside the chart
        return;
    m_buttons.push_back(screen_to_math(point));
    std::sort(m_buttons.begin(),m_buttons.end());
    recalculate_line();
    return;
}

void Chart::mouse_left_window(wxMouseEvent &e)
{
    button_idx_hover = size_t(-1);
    if (m_dragged != nullptr) {
        wxPoint2DDouble dblpoint    = screen_to_math(e.GetPosition());
        double          exit_left   = (dblpoint.m_x - visible_area.m_x) / visible_area.m_width;
        double          exit_right  = (visible_area.m_x + visible_area.m_width - dblpoint.m_x) / visible_area.m_width;
        double          exit_bottom = (dblpoint.m_y - visible_area.m_y) / visible_area.m_height;
        double          exit_top = (visible_area.m_y + visible_area.m_height - dblpoint.m_y) / visible_area.m_height;
        bool            is_exit_left        = exit_left < exit_right;
        bool            is_exit_exit_bottom = exit_bottom < exit_top;
        bool is_exit_side = (is_exit_left ? exit_left : exit_right) < (is_exit_exit_bottom ? exit_bottom : exit_top);
        if (!fixed_x) {
            wxDouble m_x = m_dragged->get_pos().m_x;
            // check if exit by left / right
            if (is_exit_side) {
                if (is_exit_left) {
                    m_x = visible_area.m_x;
                } else {
                    m_x = visible_area.m_x + visible_area.m_width;
                }
            }
            m_x = int((m_x + this->m_x_legend_incr / 2) / this->m_x_legend_incr) * this->m_x_legend_incr;
            m_dragged->move(m_x - m_dragged->get_pos().m_x, 0);
        }
        wxDouble m_y = m_dragged->get_pos().m_y;
        // check if exit by top / bo
        if (!is_exit_side) {
            if (is_exit_exit_bottom) {
                m_y = visible_area.m_y;
            } else {
                m_y = visible_area.m_y + visible_area.m_height;
            }
        }
        m_y = int((m_y + this->m_y_legend_incr / 2) / this->m_y_legend_incr) * this->m_y_legend_incr;
        m_dragged->move(0, m_y - m_dragged->get_pos().m_y);
        m_previous_mouse = math_to_screen(m_dragged->get_pos());
        m_dragged        = nullptr;
        recalculate_line();
    }
    this->Refresh();
}
void Chart::mouse_released(wxMouseEvent &)
{
    if (m_dragged != nullptr) {
        if (!fixed_x) {
            float m_x = int((m_dragged->get_pos().m_x + this->m_x_legend_incr / 2) / this->m_x_legend_incr) * this->m_x_legend_incr;
            m_dragged->move(m_x - m_dragged->get_pos().m_x, 0);
        }
        float m_y = int((m_dragged->get_pos().m_y + this->m_y_legend_incr / 2) / this->m_y_legend_incr) * this->m_y_legend_incr;
        m_dragged->move(0, m_y - m_dragged->get_pos().m_y);
        m_previous_mouse = math_to_screen(m_dragged->get_pos());
        m_dragged = nullptr;
        recalculate_line();
    }
}

void Chart::recalculate_line() {
    m_line_to_draw.clear();
    m_total_volume = 0.f;

    std::vector<wxPoint> points;
    size_t before_area_idx = 0;
    for (auto& but : m_buttons) {
        before_area_idx += visible_area.m_x <= but.get_pos().m_x ? 1 : 0;
        if (visible_area.m_x <= but.get_pos().m_x && but.get_pos().m_x <= visible_area.m_x + visible_area.m_width) {
            points.push_back(wxPoint(math_to_screen(but.get_pos())));
        }
    }
    if (points.empty() && before_area_idx < m_buttons.size()) {
        points.push_back(wxPoint(math_to_screen(m_buttons[before_area_idx].get_pos())));
    }

    // The calculation wouldn't work in case the ramming is to be turned off completely.
    if (points.size()>1) {
        std::sort(points.begin(),points.end(),[](wxPoint& a,wxPoint& b) { return a.x < b.x; });

        // Cubic spline interpolation: see https://en.wikiversity.org/wiki/Cubic_Spline_Interpolation#Methods
        const bool boundary_first_derivative = true; // true - first derivative is 0 at the leftmost and rightmost point
                                                     // false - second ---- || -------
        const int N = points.size()-1; // last point can be accessed as N, we have N+1 total points
        std::vector<float> diag(N+1);
        std::vector<float> mu(N+1);
        std::vector<float> lambda(N+1);
        std::vector<float> h(N+1);
        std::vector<float> rhs(N+1);
        
        // let's fill in inner equations
        for (int i=1;i<=N;++i) h[i] = points[i].x-points[i-1].x;
        std::fill(diag.begin(),diag.end(),2.f);
        for (int i=1;i<=N-1;++i) {
            mu[i] = h[i]/(h[i]+h[i+1]);
            lambda[i] = 1.f - mu[i];
            rhs[i] = 6 * ( float(points[i+1].y-points[i].y  )/(h[i+1]*(points[i+1].x-points[i-1].x)) -
                           float(points[i].y  -points[i-1].y)/(h[i]  *(points[i+1].x-points[i-1].x))   );
        }

        // now fill in the first and last equations, according to boundary conditions:
        if (boundary_first_derivative) {
            const float endpoints_derivative = 0;
            lambda[0] = 1;
            mu[N]     = 1;
            rhs[0] = (6.f/h[1]) * (float(points[0].y-points[1].y)/(points[0].x-points[1].x) - endpoints_derivative);
            rhs[N] = (6.f/h[N]) * (endpoints_derivative - float(points[N-1].y-points[N].y)/(points[N-1].x-points[N].x));
        } else {
            lambda[0] = 0;
            mu[N]     = 0;
            rhs[0]    = 0;
            rhs[N]    = 0;
        }

        // the trilinear system is ready to be solved:
        for (int i=1;i<=N;++i) {
            float multiple = mu[i]/diag[i-1];    // let's subtract proper multiple of above equation
            diag[i]-= multiple * lambda[i-1];
            rhs[i] -= multiple * rhs[i-1];
        }
        // now the back substitution (vector mu contains invalid values from now on):
        rhs[N] = rhs[N]/diag[N];
        for (int i=N-1;i>=0;--i)
            rhs[i] = (rhs[i]-lambda[i]*rhs[i+1])/diag[i];

        size_t curr_idx = 1;
        float y=0.f;
        for (int x = m_rect.GetLeft(); x <= m_rect.GetRight(); ++x) {
            if (x < points.front().x) {
                m_line_to_draw.push_back(points.front().y);
            } else if (points.back().x < x) {
                m_line_to_draw.push_back(points.back().y);
            } else {
                assert(curr_idx <= N);
                if (curr_idx < N && points[curr_idx].x < x) {
                    ++curr_idx;
                }
                if (this->m_type == Slic3r::GraphData::GraphType::SPLINE) {
                    y = (rhs[curr_idx - 1] * pow(points[curr_idx].x - x, 3) + rhs[curr_idx] * pow(x - points[curr_idx - 1].x, 3)) / (6 * h[curr_idx]) +
                        (points[curr_idx - 1].y - rhs[curr_idx - 1] * h[curr_idx] * h[curr_idx] / 6.f) * (points[curr_idx].x - x) / h[curr_idx] +
                        (points[curr_idx].y - rhs[curr_idx] * h[curr_idx] * h[curr_idx] / 6.f) * (x - points[curr_idx - 1].x) / h[curr_idx];
                    m_line_to_draw.push_back(y);
                } else if (this->m_type == Slic3r::GraphData::GraphType::LINEAR) {
                    assert(points[curr_idx - 1].x < points[curr_idx].x);
                    float ratio = float(x - points[curr_idx - 1].x) /
                                    (points[curr_idx].x - points[curr_idx - 1].x);
                    y = (1 - ratio) * points[curr_idx - 1].y + ratio * points[curr_idx].y;
                    m_line_to_draw.push_back(y);
                } else /*if (this->m_type == Slic3r::GraphData::GraphType::SQUARE)*/ {
                    if (points.back().x == x)
                        m_line_to_draw.push_back(points[curr_idx].y);
                    else
                        m_line_to_draw.push_back(points[curr_idx - 1].y);
                }
            }

            m_line_to_draw.back() = std::max(m_line_to_draw.back(), m_rect.GetTop()-1);
            m_line_to_draw.back() = std::min(m_line_to_draw.back(), m_rect.GetBottom()-1);
            m_total_volume += (m_rect.GetBottom() - m_line_to_draw.back()) * (visible_area.m_width / m_rect.GetWidth()) * (visible_area.m_height / m_rect.GetHeight());
        }
    } else if (points.size() == 1) {
        
        m_line_to_draw.push_back(points.front().y);
        m_line_to_draw.back() = std::max(m_line_to_draw.back(), m_rect.GetTop()-1);
        m_line_to_draw.back() = std::min(m_line_to_draw.back(), m_rect.GetBottom()-1);
        for (int x = m_rect.GetLeft() + 1; x <= m_rect.GetRight(); ++x) {
            m_line_to_draw.push_back(m_line_to_draw.back());
        }
        m_total_volume += (m_rect.GetBottom() - m_line_to_draw.back()) * (visible_area.m_width) * (visible_area.m_height / m_rect.GetHeight());
    }

    wxPostEvent(this->GetParent(), wxCommandEvent(EVT_SLIC3R_CHART_CHANGED));
    Refresh();
}

std::vector<float> Chart::get_value_samples(float sampling) const {
    std::vector<float> smaples;
    
    const int number_of_samples = std::round( visible_area.m_width / sampling);
    if (number_of_samples > 0 && !m_line_to_draw.empty()) {
        const int dx = (m_line_to_draw.size()-1) / number_of_samples;
        for (int j=0;j<number_of_samples;++j) {
            float left =  screen_to_math(wxPoint(0,m_line_to_draw[j*dx])).m_y;
            float right = screen_to_math(wxPoint(0,m_line_to_draw[(j+1)*dx])).m_y;
            smaples.push_back((left+right)/2.f);            
        }
    }
    return smaples;
}

std::vector<std::pair<float,float>> Chart::get_buttons() const {
    std::vector<std::pair<float, float>> buttons_out;
    for (const auto& button : m_buttons)
        buttons_out.push_back(std::make_pair(float(button.get_pos().m_x),float(button.get_pos().m_y)));
    return buttons_out;
}


void Chart::set_buttons(std::vector<std::pair<float, float>> new_buttons) {
    m_buttons.clear();
    for (std::pair<float, float> &new_button : new_buttons) {
        m_buttons.emplace_back(wxPoint2DDouble(new_button.first, new_button.second));
    }
    recalculate_line();
}

BEGIN_EVENT_TABLE(Chart, wxWindow)
EVT_MOTION(Chart::mouse_moved)
EVT_LEFT_DOWN(Chart::mouse_clicked)
EVT_LEFT_UP(Chart::mouse_released)
EVT_LEFT_DCLICK(Chart::mouse_double_clicked)
EVT_RIGHT_DOWN(Chart::mouse_right_button_clicked)
EVT_LEAVE_WINDOW(Chart::mouse_left_window)
EVT_PAINT(Chart::paint_event)
END_EVENT_TABLE()
