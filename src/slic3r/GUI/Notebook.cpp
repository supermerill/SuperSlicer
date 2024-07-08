#include "Notebook.hpp"

#include "libslic3r/AppConfig.hpp"

#include "GUI_App.hpp"
#include "GUI_Tags.hpp"
#include "wxExtensions.hpp"

#include <wx/button.h>
#include <wx/sizer.h>

wxDEFINE_EVENT(wxCUSTOMEVT_NOTEBOOK_SEL_CHANGED, wxCommandEvent);
wxDEFINE_EVENT(wxCUSTOMEVT_NOTEBOOK_BT_PRESSED, wxCommandEvent);

ButtonsListCtrl::ButtonsListCtrl(wxWindow *parent, bool add_mode_buttons/* = false*/) :
    wxControl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxTAB_TRAVERSAL)
{
#ifdef __WINDOWS__
    SetDoubleBuffered(true);
#endif //__WINDOWS__

    int em = em_unit(this);// Slic3r::GUI::wxGetApp().em_unit();
#ifdef __WINDOWS__
    m_btn_margin = std::lround(0.3 * em);
#else
    m_btn_margin = std::lround(0.4 * em);
#endif
    m_line_margin = std::lround(0.1 * em);

    m_sizer = new wxBoxSizer(wxHORIZONTAL);
    this->SetSizer(m_sizer);

    // Create the buttons sizer with adjustable gaps
    m_buttons_sizer = new wxFlexGridSizer(1, m_btn_margin, m_btn_margin);
#ifdef __APPLE__
    m_buttons_sizer->SetHGap(m_btn_margin);  // Horizontal gap between buttons
    m_buttons_sizer->SetVGap(m_btn_margin);  // Vertical gap between buttons
#endif
    m_sizer->Add(m_buttons_sizer, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxBOTTOM, m_btn_margin);

    if (add_mode_buttons) {
        m_mode_sizer = new Slic3r::GUI::ModeSizer(this, m_btn_margin, 0);
        m_sizer->AddStretchSpacer(20);  // Adjust the stretch spacer to ensure buttons align correctly
        m_sizer->Add(m_mode_sizer, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT | wxBOTTOM, m_btn_margin);
    }

    this->Bind(wxEVT_PAINT, &ButtonsListCtrl::OnPaint, this);
}

void ButtonsListCtrl::OnPaint(wxPaintEvent&)
{
    Slic3r::GUI::wxGetApp().UpdateDarkUI(this);
    const wxSize sz = GetSize();
    wxPaintDC dc(this);

    if (m_selection < 0 || m_selection >= (int)m_pageButtons.size())
        return;

    const wxColour& selected_btn_bg  = Slic3r::GUI::wxGetApp().get_color_selected_btn_bg();
    const wxColour& default_btn_bg   = Slic3r::GUI::wxGetApp().get_highlight_default_clr();
    const wxColour& btn_marker_color = Slic3r::GUI::wxGetApp().get_color_hovered_btn();

    // highlight selected notebook button
    for (int idx = 0; idx < int(m_pageButtons.size()); idx++) {
        wxButton* btn = m_pageButtons[idx];
        btn->SetBackgroundColour(idx == m_selection ? selected_btn_bg : default_btn_bg);
#ifdef __APPLE__
        // Adding border to make it look more like buttons
        btn->SetWindowStyle(wxBORDER_SUNKEN | wxBORDER_SIMPLE);
#else
        wxPoint pos = btn->GetPosition();
        wxSize size = btn->GetSize();
        const wxColour& clr = idx == m_selection ? btn_marker_color : default_btn_bg;
        dc.SetPen(clr);
        dc.SetBrush(clr);
        dc.DrawRectangle(pos.x, pos.y + size.y, size.x, sz.y - size.y);
#endif
    }

    // highlight selected mode button
    if (m_mode_sizer) {
        const std::vector<Slic3r::GUI::ModeButton*>& mode_btns = m_mode_sizer->get_btns();
        for (int idx = 0; idx < int(mode_btns.size()); idx++) {
            Slic3r::GUI::ModeButton* btn = mode_btns[idx];
            btn->SetBackgroundColour(btn->is_selected() ? selected_btn_bg : default_btn_bg);
#ifdef __APPLE_
            // Adding border to make it look more like buttons
            btn->SetWindowStyle(wxBORDER_SUNKEN | wxBORDER_SIMPLE);
#endif
        }
    }

    // Draw orange bottom line
    dc.SetPen(btn_marker_color);
    dc.SetBrush(btn_marker_color);
    dc.DrawRectangle(1, sz.y - m_line_margin, sz.x, m_line_margin);
}

void ButtonsListCtrl::UpdateMode()
{
    m_mode_sizer->SetMode(Slic3r::GUI::wxGetApp().get_mode());
}

void ButtonsListCtrl::Rescale()
{
    m_mode_sizer->msw_rescale();
    for (ScalableButton* btn : m_pageButtons)
        btn->msw_rescale();

    int em = em_unit(this);

#ifdef __APPLE__
    // Adjust margins and sizes specifically for macOS
    m_btn_margin = std::lround(0.4 * em);
    m_line_margin = std::lround(0.1 * em);
#else
    m_btn_margin = std::lround(0.3 * em);
    m_line_margin = std::lround(0.1 * em);
#endif //__APPLE__

    m_buttons_sizer->SetVGap(m_btn_margin);  // Adjust vertical gap here
    m_buttons_sizer->SetHGap(m_btn_margin);  // Adjust horizontal gap here

    m_sizer->Layout();
}

void ButtonsListCtrl::SetSelection(int sel)
{
    if (m_selection == sel)
        return;
    m_selection = sel;
    Refresh();
}

bool ButtonsListCtrl::InsertPage(size_t n, const wxString& text, bool bSelect/* = false*/, const std::string& bmp_name/* = ""*/, const int bmp_size)
{

    ScalableButton* btn = new ScalableButton(this, wxID_ANY, bmp_name, text, wxDefaultSize, wxDefaultPosition,
#ifdef __APPLE__
        wxBU_EXACTFIT | wxBORDER_SIMPLE | (bmp_name.empty() ? 0 : wxBU_LEFT),
#else
        wxBU_EXACTFIT | wxNO_BORDER | (bmp_name.empty() ? 0 : wxBU_LEFT),
#endif //__APPLE__
        false, bmp_size);

    // Set custom background color for the button
    btn->SetBackgroundColour(Slic3r::GUI::wxGetApp().get_color_hovered_btn());

    btn->Bind(wxEVT_BUTTON, [this, btn](wxCommandEvent& event) {
        if (auto it = std::find(m_pageButtons.begin(), m_pageButtons.end(), btn); it != m_pageButtons.end()) {
            m_selection = (it - m_pageButtons.begin());
            wxCommandEvent evt = wxCommandEvent(wxCUSTOMEVT_NOTEBOOK_SEL_CHANGED);
            evt.SetId(m_selection);
            wxPostEvent(this->GetParent(), evt);
            Refresh();
        }
    });
    Slic3r::GUI::wxGetApp().UpdateDarkUI(btn);
    m_pageButtons.insert(m_pageButtons.begin() + n, btn);
    m_spacers.insert(m_spacers.begin() + n, false);
    size_t idx = n;
    for (int i = 0; i < n; i++) {
        if (m_spacers[i]) idx++;
    }
    m_buttons_sizer->Insert(idx, new wxSizerItem(btn));
    m_buttons_sizer->SetCols(m_buttons_sizer->GetCols() + 1);
    m_sizer->Layout();
    return true;
}

bool ButtonsListCtrl::InsertSpacer(size_t n, int size)
{
    if (m_spacers.size() <= n) {
        assert(false);
        return false; // error
    }
    m_spacers[n] = true;
    size_t idx = n;
    for (int i = 0; i < n; i++) {
        if (m_spacers[i]) idx++;
    }
    m_buttons_sizer->Insert(idx, size, 1);
    m_buttons_sizer->SetCols(m_buttons_sizer->GetCols() + 1);
    m_sizer->Layout();
    return true;
}

bool ButtonsListCtrl::HasSpacer(size_t n)
{
    if (m_spacers.size() <= n) {
        assert(false);
        return false; // error
    }
    return m_spacers[n];
}

void ButtonsListCtrl::RemovePage(size_t n)
{
    ScalableButton* btn = m_pageButtons[n];
    m_pageButtons.erase(m_pageButtons.begin() + n);
    size_t idx = n;
    for (int i = 0; i < n; i++) {
        if (m_spacers[i]) idx++;
    }
    if (m_spacers[n])
        m_buttons_sizer->Remove(idx);
    m_buttons_sizer->Remove(idx);
    m_spacers.erase(m_spacers.begin() + n);
    btn->Reparent(nullptr);
    btn->Destroy();
    m_sizer->Layout();
}

void ButtonsListCtrl::RemoveSpacer(size_t n)
{
    if (m_spacers[n]) {
        size_t idx = n;
        for (int i = 0; i < n; i++) {
            if (m_spacers[i]) idx++;
        }
        m_buttons_sizer->Remove(idx);
        m_sizer->Layout();
    }
}

bool ButtonsListCtrl::SetPageImage(size_t n, const std::string& bmp_name, const int bmp_size) const
{
    if (n >= m_pageButtons.size())
        return false;
    return m_pageButtons[n]->SetBitmap_(bmp_name, bmp_size);
}

bool ButtonsListCtrl::SetPageImage(size_t n, const wxBitmap& bmp) const
{
    if (n >= m_pageButtons.size())
        return false;
    m_pageButtons[n]->SetBitmap_(bmp);
    return true;
}

void ButtonsListCtrl::SetPageText(size_t n, const wxString& strText)
{
    ScalableButton* btn = m_pageButtons[n];
    btn->SetLabel(strText);
}

wxString ButtonsListCtrl::GetPageText(size_t n) const
{
    ScalableButton* btn = m_pageButtons[n];
    return btn->GetLabel();
}

ScalableButton* ButtonsListCtrl::GetPageButton(size_t n)
{
    if (n < m_pageButtons.size())
        return m_pageButtons[n];
    return nullptr;
}

void Notebook::EmitEventSelChanged(int16_t new_sel) {

    //emit event for changed tab
    if (new_sel >=0 && GetBtnsListCtrl() && this->GetPageCount() > new_sel) {
        ScalableButton* btn = GetBtnsListCtrl()->GetPageButton(new_sel);
        if (btn) {
            wxCommandEvent* evt = new wxCommandEvent(wxCUSTOMEVT_NOTEBOOK_BT_PRESSED);
            evt->SetId(new_sel);
            //btn->ProcessEvent(*evt);
            wxQueueEvent(btn, evt);
        }
    }
}
 
