#include "GraphDialog.hpp"

#include "BitmapCache.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

#include <wx/sizer.h>
#include <wx/spinctrl.h>

namespace Slic3r { namespace GUI {

int scale(const int val) { return val * Slic3r::GUI::wxGetApp().em_unit(); }
#ifdef __WXGTK3__
int ITEM_WIDTH() { return scale(10); }
#else
int ITEM_WIDTH() { return scale(6); }
#endif

static void update_ui(wxWindow *window) { Slic3r::GUI::wxGetApp().UpdateDarkUI(window); }


GraphDialog::GraphDialog(wxWindow *parent, const GraphData &parameters, const GraphSettings &settings)
    : wxDialog(parent,
               wxID_ANY,
               _(settings.title),
               wxDefaultPosition,
               wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE /* | wxRESIZE_BORDER*/)
{
    update_ui(this);
    m_panel_graph = new GraphPanel(this, parameters, settings);

    // Not found another way of getting the background colours of GraphDialog, GraphPanel and Chart correct than
    // setting them all explicitely. Reading the parent colour yielded colour that didn't really match it, no
    // wxSYS_COLOUR_... matched colour used for the dialog. Same issue (and "solution") here :
    // https://forums.wxwidgets.org/viewtopic.php?f=1&t=39608 Whoever can fix this, feel free to do so.
#ifndef _WIN32
    this->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_FRAMEBK));
    m_panel_graph->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_FRAMEBK));
#endif
    m_panel_graph->Show(true);
    this->Show();

    auto main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->Add(m_panel_graph, 1, wxEXPAND | wxTOP | wxLEFT | wxRIGHT, 5);
    main_sizer->Add(CreateButtonSizer(wxOK | wxCANCEL), 0, wxALIGN_CENTER_HORIZONTAL | wxTOP | wxBOTTOM, 10);
    SetSizer(main_sizer);
    main_sizer->SetSizeHints(this);

    update_ui(static_cast<wxButton *>(this->FindWindowById(wxID_OK, this)));
    update_ui(static_cast<wxButton *>(this->FindWindowById(wxID_CANCEL, this)));

    this->Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent &e) { 
        EndModal(wxCANCEL);
    });

    this->Bind(
        wxEVT_BUTTON,
        [this](wxCommandEvent &) {
            m_output_data = m_panel_graph->get_data();
            m_disabled = m_panel_graph->is_disabled();
            EndModal(wxID_OK);
        },
        wxID_OK);
    this->Show();
    // Slic3r::GUI::MessageDialog dlg(this, _(L("Graph .")), _(L("Warning")), wxOK | wxICON_EXCLAMATION);
    // dlg.ShowModal();
}

#ifdef _WIN32
#define style wxSP_ARROW_KEYS | wxBORDER_SIMPLE
#else
#define style wxSP_ARROW_KEYS
#endif


double text_enter_event(wxSpinCtrlDouble *widget,
                                    double &          last_val,
                                    const double &    curr_min,
                                    const double &    curr_max,
                                    double            min,
                                    double            max,
                                    double            step,
                                    wxCommandEvent &  evt)
{
    // m_widget_max_x->Bind(wxEVT_SPINCTRLDOUBLE, [this, settings](wxSpinDoubleEvent &evt) {
    double old_val = last_val;
    last_val         = std::atof(evt.GetString().c_str());
    last_val         = int32_t(last_val / step) * step;
    last_val         = std::min(std::max(last_val, min), max);
    if (curr_min >= curr_max)
        last_val = old_val;
    widget->SetValue(last_val);
    double precision = 0.000001f;
    while (precision < (curr_max - curr_min)) precision *= 10;
    return precision / 1000;
}
double spin_move(wxSpinCtrlDouble * widget,
                             double &           last_val,
                             const double &     curr_min,
                             const double &     curr_max,
                             double             min,
                             double             max,
                             double             step,
                             wxSpinDoubleEvent &evt)
{
    double       old_val     = last_val;
    const double evt_dbl_val = std::atof(evt.GetString().c_str());
    // if second call, from our SetValue, ignore.
    if (std::abs(evt_dbl_val - last_val) < 0.0000001)
        return 0;
    double incr = step;
    while (last_val > incr * 20) incr *= 10;
    if (evt_dbl_val > last_val) {
        last_val += incr;
    } else {
        last_val -= incr;
    }
    last_val = std::min(std::max(last_val, min), max);
    if (curr_min >= curr_max)
        last_val = old_val;
    widget->SetValue(last_val);
    float precision = 0.000001f;
    while (precision < (curr_max - curr_min)) precision *= 10;
    return precision / 1000;
}

GraphPanel::GraphPanel(wxWindow *parent, GraphData data, const GraphSettings &settings)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize /*,wxPoint(50,50), wxSize(800,350),wxBORDER_RAISED*/)
{
    assert(data.validate());
    update_ui(this);
    auto sizer_chart = new wxBoxSizer(wxVERTICAL);
    
    // safe
    if (data.end_idx == 0) {
        if (data.graph_points.size() > 0) {
            data.begin_idx = 0;
            data.end_idx   = data.graph_points.size();
        }
    }

    //int   Graph_speed_size = 0;
    //float dummy            = 0.f;
    m_last_min_y              = 2.f;
    m_last_max_y              = 0.f;
    //while (stream >> dummy) {
    //    ++Graph_speed_size;
    //    if (dummy > 0 && dummy <= 2) {
    //        min = std::min(min, dummy);
    //        max = std::max(max, dummy);
    //    }
    //}
    //stream.clear();
    //stream.get();
    
    // min & max
    Pointfs data_points = data.data();
    if (!data_points.empty()) {
        for (Vec2d &point : data_points) {
            m_last_min_y = std::min(m_last_min_y, (point.y()));
            m_last_max_y = std::max(m_last_max_y, (point.y()));
        }
        if (m_last_min_y >= m_last_max_y) {
            m_last_max_y = std::max(m_last_min_y, m_last_max_y) * 1.1f;
            m_last_min_y = std::min(m_last_min_y, m_last_max_y) * 0.9f;
        } else {
            m_last_min_y = int(m_last_min_y * 10 - 1 + EPSILON) / 10.f;
            m_last_max_y = int(1.9f + m_last_max_y * 10 - EPSILON) / 10.f;
        }
        if (m_last_max_y == m_last_min_y) {
            m_last_max_y = settings.max_y;
        }
    } else {
        m_last_min_y = settings.min_y;
        m_last_max_y = settings.max_y;
    }
    
    if (!data_points.empty()) {
        if (data_points.size() == 1) {
            m_last_min_x = settings.min_x;
        } else {
            m_last_min_x = std::max(settings.min_x, data_points.begin()->x() - settings.step_x);
        }
        if (data_points.rbegin()->x() <= m_last_min_x + settings.step_x) {
            m_last_max_x = settings.max_x;
        } else {
            m_last_max_x = std::min(settings.max_x, data_points.rbegin()->x() + settings.step_x);
        }
    } else {
        m_last_min_x = settings.min_x;
        m_last_max_x = settings.max_x;
    }

    std::vector<std::pair<float, float>> buttons;
    for (const Vec2d &point : data.graph_points) buttons.emplace_back(float(point.x()), float(point.y()));

    m_chart = new Chart(this, wxRect(scale(1), scale(1), scale(64), scale(36)), buttons, scale(1));
    m_chart->set_manual_points_manipulation(true);
    double precision = 0.000001f;
    while (precision < (m_last_max_x - m_last_min_x)) precision *= 10;
    m_chart->set_x_label(_(settings.x_label), precision / 1000);
    precision = 0.000001f;
    while (precision < (m_last_max_y - m_last_min_y)) precision *= 10;
    m_chart->set_y_label(_(settings.y_label), precision / 1000);
    m_chart->set_no_point_label(_(settings.null_label));
    m_chart->set_type(data.type);
#ifdef _WIN32
    update_ui(m_chart);
#else
    m_chart->SetBackgroundColour(parent->GetBackgroundColour()); // see comment in GraphDialog constructor
#endif
    sizer_chart->Add(new wxStaticText(this, wxID_ANY, 
        _(settings.description)));
    sizer_chart->Add(m_chart, 0, wxALL, 5);


    if (!settings.label_min_x.empty())
        m_widget_min_x = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                              wxSize(ITEM_WIDTH(), -1), style | wxTE_PROCESS_ENTER, settings.min_x,
                                              settings.max_x, m_last_min_x, settings.step_x);
    if (!settings.label_max_x.empty())
        m_widget_max_x = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                              wxSize(ITEM_WIDTH(), -1), style | wxTE_PROCESS_ENTER, settings.min_x,
                                              settings.max_x, m_last_max_x, settings.step_x);
    // note: wxTE_PROCESS_ENTER allow the wxSpinCtrl to receive wxEVT_TEXT_ENTER events
 
    if (!settings.label_min_y.empty())
        m_widget_min_y = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                              wxSize(ITEM_WIDTH(), -1), style | wxTE_PROCESS_ENTER, settings.min_y,
                                              settings.max_y, m_last_min_y, settings.step_y);
    if (!settings.label_max_y.empty())
        m_widget_max_y = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                              wxSize(ITEM_WIDTH(), -1), style | wxTE_PROCESS_ENTER, settings.min_y,
                                              settings.max_y, m_last_max_y, settings.step_y);
    
    m_chart->set_xy_range(m_last_min_x, m_last_min_y, m_last_max_x, m_last_max_y);

#ifdef _WIN32
    if (!settings.label_min_x.empty())
        update_ui(m_widget_min_x);
    if (!settings.label_max_x.empty())
        update_ui(m_widget_max_x);
    if (!settings.label_min_y.empty())
        update_ui(m_widget_min_y);
    if (!settings.label_max_y.empty())
        update_ui(m_widget_max_y);
#endif
    // line for speed max & reset
    if (!settings.label_min_x.empty() || !settings.label_max_x.empty()) {
        wxBoxSizer *size_line = new wxBoxSizer(wxHORIZONTAL);
        if (!settings.label_min_x.empty()) {
            size_line->Add(new wxStaticText(this, wxID_ANY, wxString(_(settings.label_min_x) + " :")), 0, wxALIGN_CENTER_VERTICAL);
            size_line->Add(m_widget_min_x);
            size_line->AddSpacer(20);
        }
        if (!settings.label_max_x.empty()) {
            size_line->Add(new wxStaticText(this, wxID_ANY, wxString(_(settings.label_max_x) + " :")), 0, wxALIGN_CENTER_VERTICAL);
            size_line->Add(m_widget_max_x);
        }
        sizer_chart->Add(size_line);
    }

    //line for y min & max
    if (!settings.label_min_y.empty() || !settings.label_max_y.empty()) {
        wxBoxSizer *size_line = new wxBoxSizer(wxHORIZONTAL);
        if (!settings.label_min_y.empty()) {
            size_line->Add(new wxStaticText(this, wxID_ANY, wxString(_(settings.label_min_y) + " :")), 0, wxALIGN_CENTER_VERTICAL);
            size_line->Add(m_widget_min_y);
            size_line->AddSpacer(20);
        }
        
        if (!settings.label_max_y.empty()) {
            size_line->Add(new wxStaticText(this, wxID_ANY, wxString(_(settings.label_max_y) + " :")), 0, wxALIGN_CENTER_VERTICAL);
            size_line->Add(m_widget_max_y);
        }
        sizer_chart->Add(size_line);
    }
    
    
    wxBoxSizer *size_line = new wxBoxSizer(wxHORIZONTAL);
    wxButton *bt_reset = new wxButton(this, wxID_ANY, _L("Reset"));
    bt_reset->SetToolTip(_L("Reset all points to defaults."));
    size_line->Add(bt_reset);
    size_line->AddSpacer(20);
    wxButton *bt_type = new wxButton(this, wxID_ANY, _L("Change Type"));
    bt_type->SetToolTip(_L("Change the graph type into square, linear or spline."));
    size_line->Add(bt_type);
    sizer_chart->Add(size_line);
    
    sizer_chart->SetSizeHints(this);
    SetSizer(sizer_chart);

    bt_reset->Bind(wxEVT_BUTTON, ([this, reset_vals = settings.reset_vals](wxCommandEvent& e) {
        std::vector<std::pair<float,float>> buttons;// = m_chart->get_buttons();
        for (const auto &x2y: reset_vals.graph_points) {
            buttons.emplace_back(float(x2y.x()), float(x2y.y()));
        }
        m_chart->set_buttons(buttons);
        m_chart->set_xy_range(
            reset_vals.begin_idx >= 0 && reset_vals.begin_idx < reset_vals.graph_points.size() ? reset_vals.graph_points[reset_vals.begin_idx].x() : std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::quiet_NaN(), 
            reset_vals.end_idx >= 0 && reset_vals.end_idx < reset_vals.graph_points.size() ? reset_vals.graph_points[reset_vals.end_idx].x() : std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::quiet_NaN());
    }));
    
    bt_type->Bind(wxEVT_BUTTON, ([this, allowed_types = settings.allowed_types](wxCommandEvent& e) {
        if(allowed_types.empty())
            m_chart->set_type(GraphData::GraphType((uint8_t(m_chart->get_type()) + 1) % GraphData::GraphType::COUNT));
        else {
            auto it_search = std::find(allowed_types.begin(), allowed_types.end(), m_chart->get_type());
            if (it_search == allowed_types.end() || (it_search + 1) == allowed_types.end()) {
                m_chart->set_type(allowed_types.front());
            } else {
                m_chart->set_type(*(it_search + 1));
            }

        }
    }));

    if (!settings.label_max_x.empty()) {
        m_widget_max_x->Bind(wxEVT_TEXT_ENTER, [this, settings](wxCommandEvent &evt) {
            // m_widget_max_x->Bind(wxEVT_SPINCTRLDOUBLE, [this, settings](wxSpinDoubleEvent &evt) {
            // double old_speed = m_last_max_x;
            // m_last_max_x = std::atof(evt.GetString().c_str());
            // m_last_max_x = int32_t(m_last_max_x / settings.step_x)  * settings.step_x;
            // m_last_max_x = std::min(std::max(m_last_max_x, settings.min_x), settings.max_x);
            // m_last_max_x = std::max(m_last_max_x, m_last_min_x + settings.step_x);
            // m_widget_max_x->SetValue(m_last_max_x);
            // double precision = 0.000001f;
            // while(precision < (m_last_max_x - m_last_min_x)) precision *= 10;
            // m_chart->set_x_label(_(settings.x_label), float(precision/1000));
            // m_chart->set_xy_range(m_last_min_x, std::numeric_limits<float>::quiet_NaN(), m_last_max_x,
            // std::numeric_limits<float>::quiet_NaN());
            double new_precision = text_enter_event(m_widget_max_x, m_last_max_x, m_last_min_x, m_last_max_x,
                                                    settings.min_x, settings.max_x, settings.step_x, evt);
            if (new_precision != 0) {
                m_chart->set_x_label(_(settings.x_label), new_precision);
                m_chart->set_xy_range(m_last_min_x, std::numeric_limits<float>::quiet_NaN(), m_last_max_x,
                                      std::numeric_limits<float>::quiet_NaN());
            }
        });
        // m_widget_max_x->Bind(wxEVT_TEXT, [this, settings](wxCommandEvent &evt) {
        m_widget_max_x->Bind(wxEVT_SPINCTRLDOUBLE, [this, settings](wxSpinDoubleEvent &evt) {
            // const double evt_dbl_val = std::atof(evt.GetString().c_str());
            //// if second call, from our SetValue, ignore.
            // if(std::abs(evt_dbl_val - m_last_max_x) < 0.0000001) return;
            // double incr = settings.step_x;
            // while (m_last_max_x > incr * 20)
            //    incr *= 10;
            // if (evt_dbl_val > m_last_max_x) {
            //    m_last_max_x += incr;
            //} else {
            //    m_last_max_x -= incr;
            //}
            // m_last_max_x = std::min(std::max(m_last_max_x, settings.min_x), settings.max_x);
            // m_last_max_x = std::max(m_last_max_x, m_last_min_x + settings.step_x);
            // float precision = 0.000001f;
            // while(precision < (m_last_max_x - m_last_min_x)) precision *= 10;
            double new_precision = spin_move(m_widget_max_x, m_last_max_x, m_last_min_x, m_last_max_x, settings.min_x,
                                             settings.max_x, settings.step_x, evt);
            if (new_precision != 0) {
                m_chart->set_x_label(_(settings.x_label), new_precision);
                m_chart->set_xy_range(m_last_min_x, std::numeric_limits<float>::quiet_NaN(), m_last_max_x,
                                      std::numeric_limits<float>::quiet_NaN());
            }
        });
    }
    if (!settings.label_min_x.empty()) {
        m_widget_min_x->Bind(wxEVT_TEXT_ENTER, [this, settings](wxCommandEvent &evt) {
            // double old_speed = m_last_min_x;
            // m_last_min_x = std::atof(evt.GetString().c_str());
            // m_last_min_x = int32_t(m_last_min_x / settings.step_x)  * settings.step_x;
            // m_last_min_x = std::min(std::max(m_last_min_x, settings.min_x), settings.max_x);
            // m_last_min_x = std::min(m_last_min_x, m_last_max_x - settings.step_x);
            // m_widget_min_x->SetValue(m_last_min_x);
            // double precision = 0.000001f;
            // while(precision < (m_last_max_x - m_last_min_x)) precision *= 10;
            // m_chart->set_x_label(_(settings.x_label), float(precision/1000));
            // m_chart->set_xy_range(m_last_min_x, std::numeric_limits<float>::quiet_NaN(), m_last_max_x,
            // std::numeric_limits<float>::quiet_NaN());
            double new_precision = text_enter_event(m_widget_min_x, m_last_min_x, m_last_min_x, m_last_max_x,
                                                    settings.min_x, settings.max_x, settings.step_x, evt);
            if (new_precision != 0) {
                m_chart->set_x_label(_(settings.x_label), new_precision);
                m_chart->set_xy_range(m_last_min_x, std::numeric_limits<float>::quiet_NaN(), m_last_max_x,
                                      std::numeric_limits<float>::quiet_NaN());
            }
        });
        // m_widget_min_x->Bind(wxEVT_TEXT, [this, settings](wxCommandEvent &evt) {
        m_widget_min_x->Bind(wxEVT_SPINCTRLDOUBLE, [this, settings](wxSpinDoubleEvent &evt) {
            // double evt_dbl_val = std::atof(evt.GetString().c_str());
            //// if second call, from our SetValue, ignore.
            // if(std::abs(evt_dbl_val - m_last_min_x) < 0.0000001) return;
            // double incr = settings.step_x;
            // while (m_last_min_x > incr * 20)
            //    incr *= 10;
            // if (evt_dbl_val > m_last_min_x) {
            //    m_last_min_x += incr;
            //} else {
            //    m_last_min_x -= incr;
            //}
            // m_last_min_x = std::min(std::max(m_last_min_x, settings.min_x), settings.max_x);
            // m_last_min_x = std::min(m_last_min_x, m_last_max_x - settings.step_x);
            // float precision = 0.000001f;
            // while(precision < (m_last_max_x - m_last_min_x)) precision *= 10;
            // m_chart->set_x_label(_(settings.x_label), precision / 1000);
            // m_widget_min_x->SetValue(m_last_min_x);
            // m_chart->set_xy_range(m_last_min_x, std::numeric_limits<float>::quiet_NaN(), m_last_max_x,
            // std::numeric_limits<float>::quiet_NaN());
            double new_precision = spin_move(m_widget_min_x, m_last_min_x, m_last_min_x, m_last_max_x, settings.min_x,
                                             settings.max_x, settings.step_x, evt);
            if (new_precision != 0) {
                m_chart->set_x_label(_(settings.x_label), new_precision);
                m_chart->set_xy_range(m_last_min_x, std::numeric_limits<float>::quiet_NaN(), m_last_max_x,
                                      std::numeric_limits<float>::quiet_NaN());
            }
        });
    }
    // these work for wxSpinCtrl but not for wxSpinCtrlDouble
    //m_widget_max_y->Bind(wxEVT_SPINCTRL, [this, settings](wxSpinEvent &evt) { // this one works 
    //m_widget_max_y->Bind(wxEVT_SPIN_UP, [this](wxSpinEvent &evt) {
    //        std::cout<<"up";
    //    });
    //m_widget_max_y->Bind(wxEVT_SPIN_DOWN, [this](wxSpinEvent &evt) {
    //        std::cout<<"down";
    //    });
    
    //m_widget_min_y->Bind(wxEVT_TEXT, [this, settings](wxCommandEvent &) {
    //    double precision = 0.000001f;
    //    while(precision < (m_widget_max_y->GetValue() - m_widget_min_y->GetValue())) precision *= 10;
    //    m_chart->set_y_label(_(settings.y_label), float(precision/1000));
    //    m_chart->set_xy_range(std::numeric_limits<float>::quiet_NaN(), m_widget_min_y->GetValue(), std::numeric_limits<float>::quiet_NaN(), m_widget_max_y->GetValue());
    //});
    //m_widget_min_y->Bind(wxEVT_CHAR, [](wxKeyEvent &) {});   // do nothing - prevents the user to change the value
    //m_widget_max_y->Bind(wxEVT_TEXT, [this, settings](wxCommandEvent &) {
    //    double precision = 0.000001f;
    //    while(precision < (m_widget_max_y->GetValue() - m_widget_min_y->GetValue())) precision *= 10;
    //    m_chart->set_y_label(_(settings.y_label), float(precision/1000));
    //    m_chart->set_xy_range(std::numeric_limits<float>::quiet_NaN(), m_widget_min_y->GetValue(), std::numeric_limits<float>::quiet_NaN(), m_widget_max_y->GetValue());
    //});
    if (!settings.label_min_y.empty()) {
        m_widget_min_y->Bind(wxEVT_TEXT_ENTER, [this, settings](wxCommandEvent &evt) {
            double new_precision = text_enter_event(m_widget_min_y, m_last_min_y, m_last_min_y, m_last_max_y,
                                                    settings.min_y, settings.max_y, settings.step_y, evt);
            if (new_precision != 0) {
                m_chart->set_y_label(_(settings.y_label), new_precision);
                m_chart->set_xy_range(std::numeric_limits<float>::quiet_NaN(), m_last_min_y,
                                      std::numeric_limits<float>::quiet_NaN(), m_last_max_y);
            }
        });
        m_widget_min_y->Bind(wxEVT_SPINCTRLDOUBLE, [this, settings](wxSpinDoubleEvent &evt) {
            double new_precision = spin_move(m_widget_min_y, m_last_min_y, m_last_min_y, m_last_max_y, settings.min_y,
                                             settings.max_y, settings.step_y, evt);
            if (new_precision != 0) {
                m_chart->set_y_label(_(settings.y_label), new_precision);
                m_chart->set_xy_range(std::numeric_limits<float>::quiet_NaN(), m_last_min_y,
                                      std::numeric_limits<float>::quiet_NaN(), m_last_max_y);
            }
        });
    }
    
    if (!settings.label_max_y.empty()) {
        m_widget_max_y->Bind(wxEVT_TEXT_ENTER, [this, settings](wxCommandEvent &evt) {
            double new_precision = text_enter_event(m_widget_max_y, m_last_max_y, m_last_min_y, m_last_max_y,
                                                    settings.min_y, settings.max_y, settings.step_y, evt);
            if (new_precision != 0) {
                m_chart->set_y_label(_(settings.y_label), new_precision);
                m_chart->set_xy_range(std::numeric_limits<float>::quiet_NaN(), m_last_min_y,
                                      std::numeric_limits<float>::quiet_NaN(), m_last_max_y);
            }
        });
        m_widget_max_y->Bind(wxEVT_SPINCTRLDOUBLE, [this, settings](wxSpinDoubleEvent &evt) {
            double new_precision = spin_move(m_widget_max_y, m_last_max_y, m_last_min_y, m_last_max_y, settings.min_y,
                                             settings.max_y, settings.step_y, evt);
            if (new_precision != 0) {
                m_chart->set_y_label(_(settings.y_label), new_precision);
                m_chart->set_xy_range(std::numeric_limits<float>::quiet_NaN(), m_last_min_y,
                                      std::numeric_limits<float>::quiet_NaN(), m_last_max_y);
            }
        });
    }
    //m_widget_max_y->Bind(wxEVT_CHAR, [](wxKeyEvent &) {});   // do nothing - prevents the user to change the value
    //Bind(EVT_SLIC3R_CHART_CHANGED, [this](wxCommandEvent &) {
    //    //int nb_samples = m_chart->get_value_samples(10.f).size();
    //    //m_last_max_x = 10 * nb_samples;
    //    //m_widget_max_x->SetValue(m_last_max_x);
    //});
    Refresh(true); // erase background
}

bool GraphPanel::is_disabled()
{
    GraphData data = get_data();
    if (data.graph_points.empty())
        return true;
    if (data.begin_idx == data.end_idx)
        return true;
    if (data.end_idx == size_t(-1))
        return true;
    return false;
}

GraphData GraphPanel::get_data()
{
    const std::vector<std::pair<float, float>> &buttons = m_chart->get_buttons();
    GraphData                                   data;
    data.type      = m_chart->get_type();
    data.begin_idx = size_t(-1);
    data.end_idx   = size_t(-1);
    assert(m_chart->get_max_x() > m_chart->get_min_x());
    for (size_t idx = 0; idx < buttons.size(); ++idx) {
        const std::pair<float, float> &pt = buttons[idx];
        data.graph_points.emplace_back(pt.first, pt.second);
        if (data.begin_idx == size_t(-1) && m_chart->get_min_x() <= pt.first) {
            data.begin_idx = idx;
        }
        if (m_chart->get_max_x() >= pt.first) {
            data.end_idx = idx + 1;
        }
    }
    if (data.graph_points.empty()) {
        data.begin_idx = 0;
        data.end_idx   = 0;
    }
    if (size_t(-1) == data.begin_idx || size_t(-1) == data.end_idx) {
        data.begin_idx = 0;
        data.end_idx   = 0;
    }
    assert(data.end_idx >= data.begin_idx);
    assert(data.validate());
    return data;
}

}} // namespace Slic3r::GUI
