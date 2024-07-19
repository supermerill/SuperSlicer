#ifndef _GRAPH_DIALOG_H_
#define _GRAPH_DIALOG_H_

#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/checkbox.h>
#include <wx/msgdlg.h>

#include "libslic3r/Config.hpp" // for GraphSettings
#include "RammingChart.hpp"

namespace Slic3r { namespace GUI {

class GraphPanel : public wxPanel
{
public:
    GraphPanel(wxWindow *parent, GraphData data,const GraphSettings &settings);
    GraphData get_data();
    bool      is_disabled();

private:
    Chart *           m_chart        = nullptr;
    wxSpinCtrlDouble *m_widget_min_x = nullptr;
    wxSpinCtrlDouble *m_widget_max_x = nullptr;
    wxSpinCtrlDouble *m_widget_min_y = nullptr;
    wxSpinCtrlDouble *m_widget_max_y = nullptr;
    double            m_last_min_x   = 0.f;
    double            m_last_max_x   = 1.f;
    double            m_last_min_y   = 0.f;
    double            m_last_max_y   = 1.f;
};

class GraphDialog : public wxDialog
{
public:
    GraphDialog(wxWindow *parent, const GraphData &parameters, const GraphSettings &settings);
    GraphData get_data() { return m_output_data; }
    bool      is_disabled() { return m_disabled; }

private:
    GraphPanel *m_panel_graph = nullptr;
    GraphData m_output_data;
    bool m_disabled = false;
};

}}     // namespace Slic3r::GUI
#endif  // _GRAPH_DIALOG_H_