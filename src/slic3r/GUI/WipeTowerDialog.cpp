///|/ Copyright (c) SuperSlicer 2026 Durand R?mi @supermerill
///|/ Copyright (c) Prusa Research 2018 - 2022 Enrico Turri @enricoturri1966, Oleksandra Iushchenko @YuSanka, Vojtěch Bubník @bubnikv, Lukáš Matěna @lukasmatena
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// Ramming edits a flow chart; wiping edits either per-extruder pairs or a
// tool-to-tool matrix. The wiping dialog mirrors its current editor as CSV.
// Panel edits refresh that text, while Enter/focus loss validates an entire
// CSV draft before applying it. Guards prevent nested focus notifications from
// applying a draft twice while a validation dialog is open.
#include "WipeTowerDialog.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <utility>

#include <wx/clipbrd.h>
#include <wx/sizer.h>

#include "libslic3r/Color.hpp"
#include "libslic3r/Config/ConfigOption.hpp"
#include "libslic3r/Utils.hpp"

#include "BitmapCache.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "wxExtensions.hpp"
using namespace Slic3r::GUI;

// Keep configuration serialization independent of the displayed matrix cells.
static std::string serialize_wiping_values(const std::vector<float>& values);
// Reject incomplete tokens and values which cannot be stored in the float UI model.
static bool deserialize_wiping_values(const wxString& serialized, std::vector<float>& values);

int scale(const int val) { return val * wxGetApp().em_unit(); }
#ifdef __WXGTK3__
int ITEM_WIDTH() { return scale(10); }
#else
int ITEM_WIDTH() { return scale(6); }
#endif

static void update_ui(wxWindow* window)
{
    wxGetApp().UpdateDarkUI(window);
}

// Converts the currently visible setting to the same compact CSV representation used in configuration files.
static std::string serialize_wiping_values(const std::vector<float>& values)
{
    std::vector<double> values_as_double(values.begin(), values.end());
    Slic3r::ConfigOptionFloats option(std::move(values_as_double));
    return option.serialize();
}

// Validates the CSV grammar before ConfigOptionFloats parses it. Its historical parser does not report invalid tokens.
static bool deserialize_wiping_values(const wxString& serialized, std::vector<float>& values)
{
    if (serialized.empty())
        return false;

    const std::string utf8(serialized.ToUTF8().data());
    if (utf8.empty() || utf8.back() == ',')
        return false;

    std::istringstream list(utf8);
    std::string token;
    while (std::getline(list, token, ',')) {
        std::istringstream number_stream(token);
        double value = 0.;
        if (!(number_stream >> value) || !std::isfinite(value) ||
            std::abs(value) > std::numeric_limits<float>::max())
            return false;
        // Reading the number may already set eofbit. A subsequent std::ws
        // can then set failbit, which does not make that number invalid.
        number_stream >> std::ws;
        if (!number_stream.eof())
            return false;
    }

    Slic3r::ConfigOptionFloats option;
    if (!option.deserialize(utf8))
        return false;

    const std::vector<double>& deserialized = option.get_values();
    values.assign(deserialized.begin(), deserialized.end());
    return true;
}

RammingDialog::RammingDialog(wxWindow* parent,const std::string& parameters)
: wxDialog(parent, wxID_ANY, _(L("Ramming customization")), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE/* | wxRESIZE_BORDER*/)
{
    SetFont(wxGetApp().normal_font());
    update_ui(this);
    m_panel_ramming  = new RammingPanel(this,parameters);

    // Not found another way of getting the background colours of RammingDialog, RammingPanel and Chart correct than setting
    // them all explicitely. Reading the parent colour yielded colour that didn't really match it, no wxSYS_COLOUR_... matched
    // colour used for the dialog. Same issue (and "solution") here : https://forums.wxwidgets.org/viewtopic.php?f=1&t=39608
    // Whoever can fix this, feel free to do so.
#ifndef _WIN32
    this->           SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_FRAMEBK));
    m_panel_ramming->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_FRAMEBK));
#endif
    m_panel_ramming->Show(true);
    this->Show();

    auto main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->Add(m_panel_ramming, 1, wxEXPAND | wxTOP | wxLEFT | wxRIGHT, 5);
    auto buttons = CreateStdDialogButtonSizer(wxOK | wxCANCEL);
    wxGetApp().SetWindowVariantForButton(buttons->GetAffirmativeButton());
    wxGetApp().SetWindowVariantForButton(buttons->GetCancelButton());
    main_sizer->Add(buttons, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP | wxBOTTOM, 10);
    SetSizer(main_sizer);
    main_sizer->SetSizeHints(this);

    update_ui(static_cast<wxButton*>(this->FindWindowById(wxID_OK, this)));
    update_ui(static_cast<wxButton*>(this->FindWindowById(wxID_CANCEL, this)));

    this->Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& e) { EndModal(wxCANCEL); });

    this->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) {
        m_output_data = m_panel_ramming->get_parameters();
        EndModal(wxID_OK);
        },wxID_OK);
    this->Show();
    MessageDialog dlg(this, _(L("Ramming denotes the rapid extrusion just before a tool change in a single-extruder MM printer. Its purpose is to "
        "properly shape the end of the unloaded filament so it does not prevent insertion of the new filament and can itself "
        "be reinserted later. This phase is important and different materials can require different extrusion speeds to get "
        "the good shape. For this reason, the extrusion rates during ramming are adjustable.\n\nThis is an expert-level "
        "setting, incorrect adjustment will likely lead to jams, extruder wheel grinding into filament etc.")), _(L("Warning")), wxOK | wxICON_EXCLAMATION);// .ShowModal();
    dlg.ShowModal();
}


#ifdef _WIN32
#define style wxSP_ARROW_KEYS | wxBORDER_SIMPLE
#else 
#define style wxSP_ARROW_KEYS
#endif



RammingPanel::RammingPanel(wxWindow* parent, const std::string& parameters)
: wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize/*,wxPoint(50,50), wxSize(800,350),wxBORDER_RAISED*/)
{
    update_ui(this);
	auto sizer_chart = new wxBoxSizer(wxVERTICAL);
	auto sizer_param = new wxBoxSizer(wxVERTICAL);

	std::stringstream stream{ parameters };
	stream >> m_ramming_line_width_multiplicator >> m_ramming_step_multiplicator;
	int ramming_speed_size = 0;
	float dummy = 0.f;
	while (stream >> dummy)
		++ramming_speed_size;
	stream.clear();
	stream.get();

	std::vector<std::pair<float, float>> buttons;
	float x = 0.f;
	float y = 0.f;
	while (stream >> x >> y)
		buttons.push_back(std::make_pair(x, y));

	m_chart = new Chart(this, wxRect(scale(1),scale(1),scale(48),scale(36)), buttons, scale(1));
    m_chart->set_type(Slic3r::GraphData::GraphType::SPLINE);
    m_chart->set_xy_range(0, 0, 0.25f * ramming_speed_size, 20.);
    m_chart->set_x_label(_L("Time") + " ("+_L("s")+")", 0.05f);
    m_chart->set_y_label(_L("Volumetric speed") + " (" + _L("mm³/s") + ")", 0.1f);
    m_chart->set_no_point_label(_L("NO RAMMING AT ALL"));
#ifdef _WIN32
    update_ui(m_chart);
#else
    m_chart->SetBackgroundColour(parent->GetBackgroundColour()); // see comment in RammingDialog constructor
#endif
 	sizer_chart->Add(m_chart, 0, wxALL, 5);

    m_widget_time						= new ::SpinInputDouble(this,"", wxEmptyString, wxDefaultPosition, wxSize(ITEM_WIDTH(), -1), style, 0., 5., 3., 0.5);
    m_widget_time->SetDigits(2);
    m_widget_volume							  = new ::SpinInput(this,"",wxEmptyString,wxDefaultPosition,wxSize(ITEM_WIDTH(), -1),style,0,10000,0);
    m_widget_ramming_line_width_multiplicator = new ::SpinInput(this,"",wxEmptyString,wxDefaultPosition,wxSize(ITEM_WIDTH(), -1),style,10,200,100);
    m_widget_ramming_step_multiplicator		  = new ::SpinInput(this,"",wxEmptyString,wxDefaultPosition,wxSize(ITEM_WIDTH(), -1),style,10,200,100);

#ifdef _WIN32
    update_ui(m_widget_time->GetText());
    update_ui(m_widget_volume);
    update_ui(m_widget_ramming_line_width_multiplicator);
    update_ui(m_widget_ramming_step_multiplicator);
#endif

	auto gsizer_param = new wxFlexGridSizer(2, 5, 15);
	gsizer_param->Add(new wxStaticText(this, wxID_ANY, wxString(_(L("Total ramming time")) + " (" + _(L("s")) + "):")), 0, wxALIGN_CENTER_VERTICAL);
	gsizer_param->Add(m_widget_time);
	gsizer_param->Add(new wxStaticText(this, wxID_ANY, wxString(_(L("Total rammed volume")) + " (" + _(L("mm")) + wxString("³):", wxConvUTF8))), 0, wxALIGN_CENTER_VERTICAL);
	gsizer_param->Add(m_widget_volume);
	gsizer_param->AddSpacer(20);
	gsizer_param->AddSpacer(20);
	gsizer_param->Add(new wxStaticText(this, wxID_ANY, wxString(_(L("Ramming line width")) + " (%):")), 0, wxALIGN_CENTER_VERTICAL);
	gsizer_param->Add(m_widget_ramming_line_width_multiplicator);
	gsizer_param->Add(new wxStaticText(this, wxID_ANY, wxString(_(L("Ramming line spacing")) + " (%):")), 0, wxALIGN_CENTER_VERTICAL);
	gsizer_param->Add(m_widget_ramming_step_multiplicator);

	sizer_param->Add(gsizer_param, 0, wxTOP, scale(10));

    m_widget_time->SetValue(m_chart->get_max_x());
    m_widget_volume->SetValue(m_chart->get_volume());
    m_widget_volume->Disable();
    m_widget_ramming_line_width_multiplicator->SetValue(m_ramming_line_width_multiplicator);
    m_widget_ramming_step_multiplicator->SetValue(m_ramming_step_multiplicator);        
    
    m_widget_ramming_step_multiplicator->Bind(wxEVT_TEXT,[this](wxCommandEvent&) { line_parameters_changed(); });
    m_widget_ramming_line_width_multiplicator->Bind(wxEVT_TEXT,[this](wxCommandEvent&) { line_parameters_changed(); });

	auto sizer = new wxBoxSizer(wxHORIZONTAL);
	sizer->Add(sizer_chart, 0, wxALL, 5);
	sizer->Add(sizer_param, 0, wxALL, 10);

	sizer->SetSizeHints(this);
	SetSizer(sizer);

    m_widget_time->Bind(wxEVT_SPINCTRL,[this](wxCommandEvent&) { m_chart->set_xy_range(0, 0, m_widget_time->GetValue(), -1); });
    m_widget_time->Bind(wxEVT_CHAR,[](wxKeyEvent&){});      // do nothing - prevents the user to change the value
    m_widget_time->GetText()->Bind(wxEVT_CHAR,[](wxKeyEvent&){}); // do nothing - prevents the user to change the value
    m_widget_volume->Bind(wxEVT_CHAR,[](wxKeyEvent&){});    // do nothing - prevents the user to change the value   
    Bind(EVT_SLIC3R_CHART_CHANGED,[this](wxCommandEvent&) {m_widget_volume->SetValue(m_chart->get_volume()); m_widget_time->SetValue(m_chart->get_max_x());} );
    Refresh(true); // erase background
}

void RammingPanel::line_parameters_changed() {
    m_ramming_line_width_multiplicator = m_widget_ramming_line_width_multiplicator->GetValue();
    m_ramming_step_multiplicator = m_widget_ramming_step_multiplicator->GetValue();
}

std::string RammingPanel::get_parameters()
{
    std::vector<float> speeds = m_chart->get_value_samples(0.25f);
    std::vector<std::pair<float,float>> buttons = m_chart->get_buttons();
    std::stringstream stream;
    stream << m_ramming_line_width_multiplicator << " " << m_ramming_step_multiplicator;
    for (const float& speed_value : speeds)
        stream << " " << speed_value;
    stream << "|";    
    for (const auto& button : buttons)
        stream << " " << button.first << " " << button.second;
    return stream.str();
}


// Parent dialog for purging volume adjustments - it fathers WipingPanel widget (that contains all controls) and a button to toggle simple/advanced mode:
WipingDialog::WipingDialog(wxWindow* parent, const std::vector<float>& matrix, const std::vector<float>& extruders, const std::vector<std::string>& extruder_colours)
: wxDialog(parent, wxID_ANY, _(L("Wipe tower - Purging volume adjustment")), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE/* | wxRESIZE_BORDER*/)
{
    SetFont(wxGetApp().normal_font());
    update_ui(this);
    auto widget_button = new wxButton(this,wxID_ANY,"-",wxPoint(0,0),wxDefaultSize);
    update_ui(widget_button);
    wxGetApp().SetWindowVariantForButton(widget_button);
    m_panel_wiping  = new WipingPanel(this, matrix, extruders, extruder_colours, widget_button,
                                      [this]() { update_serialized_values(); });

    auto main_sizer = new wxBoxSizer(wxVERTICAL);

	// set min sizer width according to extruders count
	const auto sizer_width = (int)((sqrt(matrix.size()) + 2.8)*ITEM_WIDTH());
	main_sizer->SetMinSize(wxSize(sizer_width, -1));

    main_sizer->Add(m_panel_wiping, 0, wxEXPAND | wxALL, 5);

    wxBoxSizer* serialized_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_serialized_values = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                          wxTE_PROCESS_ENTER);
    update_ui(m_serialized_values);
    serialized_sizer->Add(m_serialized_values, 1, wxEXPAND);

    // Match the text field height on both axes. wxBU_EXACTFIT would otherwise
    // collapse an icon-only button into a narrow rectangle on Windows.
    const int copy_button_side = m_serialized_values->GetBestSize().y;
    const wxSize copy_button_size(copy_button_side, copy_button_side);
    m_copy_button = new ScalableButton(this, wxID_ANY, "copy", wxEmptyString,
                                       copy_button_size, wxDefaultPosition,
                                       wxBU_EXACTFIT | wxBORDER_SIMPLE);
    m_copy_button->SetBitmapMargins(0, 0);
    m_copy_button->SetToolTip(_L("Copy to clipboard"));
    m_copy_button->SetMinSize(copy_button_size);
    m_copy_button->SetMaxSize(copy_button_size);
    // Use the normal background selected by UpdateDarkUI for a bordered,
    // icon-only ScalableButton, so mouse-leave restores the same colour.
    m_copy_button->SetBackgroundColour(wxGetApp().get_highlight_default_clr());
    serialized_sizer->Add(m_copy_button, 0);
    main_sizer->Add(serialized_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 20);

	main_sizer->Add(widget_button, 0, wxALIGN_CENTER_HORIZONTAL | wxCENTER | wxBOTTOM, 5);
    auto buttons = CreateStdDialogButtonSizer(wxOK | wxCANCEL);
    wxGetApp().SetWindowVariantForButton(buttons->GetAffirmativeButton());
    wxGetApp().SetWindowVariantForButton(buttons->GetCancelButton());
    main_sizer->Add(buttons, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, 10);
    SetSizer(main_sizer);
    main_sizer->SetSizeHints(this);

    update_ui(static_cast<wxButton*>(this->FindWindowById(wxID_OK, this)));
    update_ui(static_cast<wxButton*>(this->FindWindowById(wxID_CANCEL, this)));

    this->Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { m_closing = true; EndModal(wxID_CANCEL); });
    this->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_closing = true; EndModal(wxID_CANCEL); }, wxID_CANCEL);

    m_serialized_values->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { commit_serialized_values(); });
    m_serialized_values->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
        if (!m_updating_serialized_values) {
            m_copy_button->SetBitmap_("copy");
            m_copy_button->SetToolTip(_L("Copy to clipboard"));
        }
    });
    m_serialized_values->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event) {
        // Cancel discards the draft, rather than opening another modal dialog.
        if (!m_closing && (event.GetWindow() == nullptr || event.GetWindow()->GetId() != wxID_CANCEL))
            commit_serialized_values();
        event.Skip();
    });
    m_copy_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        update_serialized_values();
        bool copied = false;
        if (wxTheClipboard->Open()) {
            copied = wxTheClipboard->SetData(new wxTextDataObject(m_serialized_values->GetValue()));
            wxTheClipboard->Close();
        }
        // The green check confirms that the clipboard accepted the current
        // value. A failed copy deliberately retains the actionable copy icon.
        if (copied) {
            m_copy_button->SetBitmap_("tick_mark");
            m_copy_button->SetToolTip(_L("Copied to clipboard"));
        }
    });
    
    this->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) {                 // if OK button is clicked..
        if (!commit_serialized_values())
            return;
        m_output_matrix    = m_panel_wiping->read_matrix_values();    // ..query wiping panel and save returned values
        m_output_extruders = m_panel_wiping->read_extruders_values(); // so they can be recovered later by calling get_...()
        m_closing = true;
        EndModal(wxID_OK);
        },wxID_OK);

    update_serialized_values();
    this->Show();
}

// Refreshes the field from the active editor, so it always exposes the setting currently represented on screen.
void WipingDialog::update_serialized_values()
{
    if (m_serialized_values == nullptr || m_updating_serialized_values)
        return;

    m_updating_serialized_values = true;
    Slic3r::ScopeGuard reset_guard([this]() { m_updating_serialized_values = false; });
    const wxString serialized = wxString::FromUTF8(serialize_wiping_values(m_panel_wiping->read_serialized_values()));
    if (m_serialized_values->GetValue() != serialized) {
        m_copy_button->SetBitmap_("copy");
        m_copy_button->SetToolTip(_L("Copy to clipboard"));
        m_serialized_values->ChangeValue(serialized);
    }
}

// Applies a user supplied CSV value, retaining the current setting when a confirmed repair must fill missing entries.
bool WipingDialog::commit_serialized_values()
{
    if (m_updating_serialized_values || m_closing)
        return false;

    std::vector<float> current = m_panel_wiping->read_serialized_values();
    if (m_serialized_values->GetValue() == wxString::FromUTF8(serialize_wiping_values(current)))
        return true;

    // ShowModal() can dispatch another focus-loss event. Keep the whole
    // validation/application transaction guarded, including its error paths.
    m_updating_serialized_values = true;
    Slic3r::ScopeGuard reset_guard([this]() {
        m_updating_serialized_values = false;
        update_serialized_values();
    });

    std::vector<float> deserialized;
    if (!deserialize_wiping_values(m_serialized_values->GetValue(), deserialized)) {
        MessageDialog(this, _L("The purging volume value must be a comma-separated list of finite numbers."),
                      _L("Invalid value"), wxOK | wxICON_EXCLAMATION).ShowModal();
        return false;
    }

    if (deserialized.size() != current.size()) {
        const wxString message = wxString::Format(
            _L("This setting requires %zu values, but %zu were provided.\n\n"
               "Keep the provided values, ignore extras, and complete missing entries from the current setting?"),
            current.size(), deserialized.size());
        if (MessageDialog(this, message, _L("Invalid value count"), wxOK | wxCANCEL | wxICON_EXCLAMATION).ShowModal() != wxID_OK) {
            return false;
        }

        const size_t count_to_copy = std::min(current.size(), deserialized.size());
        std::copy_n(deserialized.begin(), count_to_copy, current.begin());
        deserialized = std::move(current);
    }

    m_panel_wiping->set_serialized_values(deserialized);
    return true;
}

// This function allows to "play" with sizers parameters (like align or border)
void WipingPanel::format_sizer(wxSizer* sizer, wxPanel* page, wxGridSizer* grid_sizer, const wxString& info, const wxString& table_title, int table_lshift/*=0*/)
{
    wxSize text_size = GetTextExtent(info);
    auto info_str = new wxStaticText(page, wxID_ANY, info ,wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER);
    info_str->Wrap(int(0.6*text_size.x));
	sizer->Add( info_str, 0, wxEXPAND);
	auto table_sizer = new wxBoxSizer(wxVERTICAL);
	sizer->Add(table_sizer, 0, wxALIGN_CENTER | wxCENTER, table_lshift);
	table_sizer->Add(new wxStaticText(page, wxID_ANY, table_title), 0, wxALIGN_CENTER | wxTOP, 50);
	table_sizer->Add(grid_sizer, 0, wxALIGN_CENTER | wxTOP, 10);
}

// This panel contains all control widgets for both simple and advanced mode (these reside in separate sizers)
WipingPanel::WipingPanel(wxWindow* parent, const std::vector<float>& matrix, const std::vector<float>& extruders,
                         const std::vector<std::string>& extruder_colours, wxButton* widget_button,
                         std::function<void()> values_changed)
: wxPanel(parent,wxID_ANY, wxDefaultPosition, wxDefaultSize/*,wxBORDER_RAISED*/)
{
    m_values_changed = std::move(values_changed);
    m_widget_button = widget_button;    // pointer to the button in parent dialog
    m_widget_button->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){ toggle_advanced(true); });

    m_number_of_extruders = (int)(sqrt(matrix.size())+0.001);
    m_matrix_diagonal.reserve(m_number_of_extruders);
    for (unsigned int i = 0; i < m_number_of_extruders; ++i)
        m_matrix_diagonal.push_back(matrix[m_number_of_extruders * i + i]);

    for (const std::string& color : extruder_colours) {
        Slic3r::ColorRGB rgb;
        Slic3r::decode_color(color, rgb);
        m_colours.push_back(wxColor(rgb.r_uchar(), rgb.g_uchar(), rgb.b_uchar()));
    }

	// Create two switched panels with their own sizers
    m_sizer_simple          = new wxBoxSizer(wxVERTICAL);
    m_sizer_advanced        = new wxBoxSizer(wxVERTICAL);
	m_page_simple			= new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
	m_page_advanced			= new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
	m_page_simple->SetSizer(m_sizer_simple);
	m_page_advanced->SetSizer(m_sizer_advanced);

    update_ui(m_page_simple);
    update_ui(m_page_advanced);

    auto gridsizer_simple   = new wxGridSizer(3, 5, 10);
    m_gridsizer_advanced = new wxGridSizer(m_number_of_extruders+1, 5, 1);

	// First create controls for advanced mode and assign them to m_page_advanced:
	for (unsigned int i = 0; i < m_number_of_extruders; ++i) {
		edit_boxes.push_back(std::vector<wxTextCtrl*>(0));

		for (unsigned int j = 0; j < m_number_of_extruders; ++j) {
#ifdef _WIN32
            wxTextCtrl* text = new wxTextCtrl(m_page_advanced, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(ITEM_WIDTH(), -1), wxBORDER_SIMPLE);
            update_ui(text);
            edit_boxes.back().push_back(text);
#else
			edit_boxes.back().push_back(new wxTextCtrl(m_page_advanced, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(ITEM_WIDTH(), -1)));
#endif
			if (i == j)
				edit_boxes[i][j]->Disable();
			else
                edit_boxes[i][j]->SetValue(wxString::Format("%.*g", std::numeric_limits<float>::max_digits10,
                    static_cast<double>(matrix[m_number_of_extruders*j + i])));

            edit_boxes[i][j]->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { notify_values_changed(); });
		}
	}

    const int clr_icon_side = edit_boxes.front().front()->GetSize().y;
    const auto icon_size = wxSize(clr_icon_side, clr_icon_side);

	m_gridsizer_advanced->Add(new wxStaticText(m_page_advanced, wxID_ANY, wxString("")));
	for (unsigned int i = 0; i < m_number_of_extruders; ++i) {
        auto hsizer = new wxBoxSizer(wxHORIZONTAL);
        hsizer->AddSpacer(20);
        hsizer->Add(new wxStaticText(m_page_advanced, wxID_ANY, wxString("") << i + 1), 0, wxALIGN_CENTER);
        wxWindow* w = new wxWindow(m_page_advanced, wxID_ANY, wxDefaultPosition, icon_size, wxBORDER_SIMPLE);
        w->SetCanFocus(false);
        w->SetBackgroundColour(m_colours[i]);
        hsizer->AddStretchSpacer();
        hsizer->Add(w);
		m_gridsizer_advanced->Add(hsizer, 1, wxEXPAND);
    }
	for (unsigned int i = 0; i < m_number_of_extruders; ++i) {
        auto hsizer = new wxBoxSizer(wxHORIZONTAL);
        wxWindow* w = new wxWindow(m_page_advanced, wxID_ANY, wxDefaultPosition, icon_size, wxBORDER_SIMPLE);
        w->SetCanFocus(false);
        w->SetBackgroundColour(m_colours[i]);
        hsizer->AddSpacer(20);
        hsizer->Add(new wxStaticText(m_page_advanced, wxID_ANY, wxString("") << i + 1), 0, wxALIGN_CENTER | wxALIGN_CENTER_VERTICAL);
        hsizer->AddStretchSpacer();
        hsizer->Add(w);
        m_gridsizer_advanced->Add(hsizer, 1, wxEXPAND);

    for (unsigned int j = 0; j < m_number_of_extruders; ++j)
        m_gridsizer_advanced->Add(edit_boxes[j][i], 0);
    }

	// collect and format sizer
	format_sizer(m_sizer_advanced, m_page_advanced, m_gridsizer_advanced,
		_(L("Here you can adjust required purging volume (mm³) for any given pair of tools.")),
		_(L("Extruder changed to")));

	// Hide preview page before new page creating 
	// It allows to do that from a beginning of the main panel
	m_page_advanced->Hide(); 

	// Now the same for simple mode:
	gridsizer_simple->Add(new wxStaticText(m_page_simple, wxID_ANY, wxString("")), 0, wxALIGN_CENTER | wxALIGN_CENTER_VERTICAL);
	gridsizer_simple->Add(new wxStaticText(m_page_simple, wxID_ANY, wxString(_(L("unloaded")))), 0, wxALIGN_CENTER | wxALIGN_CENTER_VERTICAL);
    gridsizer_simple->Add(new wxStaticText(m_page_simple,wxID_ANY,wxString(_(L("loaded")))), 0, wxALIGN_CENTER | wxALIGN_CENTER_VERTICAL);

    auto add_spin_ctrl = [this](std::vector<::SpinInput*>& vec, float initial)
    {
        ::SpinInput* spin_ctrl = new ::SpinInput(m_page_simple, "", wxEmptyString, wxDefaultPosition, wxSize(ITEM_WIDTH(), -1), style | wxALIGN_RIGHT, 0, 300, (int)initial);
        update_ui(spin_ctrl);
        vec.push_back(spin_ctrl);
    };

	for (unsigned int i=0;i<m_number_of_extruders;++i) {
        add_spin_ctrl(m_old, extruders[2 * i]);
        add_spin_ctrl(m_new, extruders[2 * i+1]);
        m_old.back()->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent&) { notify_values_changed(); });
        m_new.back()->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent&) { notify_values_changed(); });

        auto hsizer = new wxBoxSizer(wxHORIZONTAL);
        wxWindow* w = new wxWindow(m_page_simple, wxID_ANY, wxDefaultPosition, icon_size, wxBORDER_SIMPLE);
        w->SetCanFocus(false);
        w->SetBackgroundColour(m_colours[i]);
        hsizer->Add(w, wxALIGN_CENTER_VERTICAL);
        hsizer->AddSpacer(10);
        hsizer->Add(new wxStaticText(m_page_simple, wxID_ANY, wxString(_(L("Tool #"))) << i + 1 << ": "), 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL);

        gridsizer_simple->Add(hsizer, 1, wxEXPAND);
        gridsizer_simple->Add(m_old.back(),0);
        gridsizer_simple->Add(m_new.back(),0);
	}

	// collect and format sizer
	format_sizer(m_sizer_simple, m_page_simple, gridsizer_simple,
		_(L("Total purging volume is calculated by summing two values below, depending on which tools are loaded/unloaded.")),
		_(L("Volume to purge (mm³) when the filament is being")), 50);

	m_sizer = new wxBoxSizer(wxVERTICAL);
	m_sizer->Add(m_page_simple, 0, wxEXPAND | wxALL, 25);
	m_sizer->Add(m_page_advanced, 0, wxEXPAND | wxALL, 25);

	m_sizer->SetSizeHints(this);
	SetSizer(m_sizer);

    toggle_advanced(); // to show/hide what is appropriate
    
    m_page_advanced->Bind(wxEVT_PAINT,[this](wxPaintEvent&) {
                                              wxPaintDC dc(m_page_advanced);
                                              int y_pos = 0.5 * (edit_boxes[0][0]->GetPosition().y + edit_boxes[0][edit_boxes.size()-1]->GetPosition().y + edit_boxes[0][edit_boxes.size()-1]->GetSize().y);
                                              wxString label = _(L("From"));
                                              int text_width = 0;
                                              int text_height = 0;
                                              dc.GetTextExtent(label,&text_width,&text_height);
                                              int xpos = m_gridsizer_advanced->GetPosition().x;
                                              dc.DrawRotatedText(label,xpos-text_height,y_pos + text_width/2.f,90);
    });
}




// Reads values from the (advanced) wiping matrix:
std::vector<float> WipingPanel::read_matrix_values() {
    if (!m_advanced)
        fill_in_matrix();
    std::vector<float> output;
    for (unsigned int i=0;i<m_number_of_extruders;++i) {
        for (unsigned int j=0;j<m_number_of_extruders;++j) {
            if (i == j) {
                output.push_back(m_matrix_diagonal[i]);
                continue;
            }
            double val = 0.;
            // A cell can temporarily contain an incomplete number while it is
            // being edited. Keep the serialized preview finite in that state.
            if (!edit_boxes[j][i]->GetValue().ToDouble(&val) || !std::isfinite(val) ||
                std::abs(val) > std::numeric_limits<float>::max())
                val = 0.;
            output.push_back((float)val);
        }
    }
    return output;
}

// The serialized setting follows the active UI: simple controls use load/unload pairs, advanced controls use the full matrix.
std::vector<float> WipingPanel::read_serialized_values()
{
    return m_advanced ? read_matrix_values() : read_extruders_values();
}

// Replaces the active setting after validation. The disabled matrix diagonal is kept separately so its values round-trip.
void WipingPanel::set_serialized_values(const std::vector<float>& values)
{
    const size_t expected = m_advanced ? size_t(m_number_of_extruders) * m_number_of_extruders :
                                        size_t(m_number_of_extruders) * 2;
    if (values.size() != expected)
        throw std::invalid_argument("Incorrect number of purging volume values.");
    m_suppress_change_notification = true;
    if (m_advanced) {
        for (unsigned int i = 0; i < m_number_of_extruders; ++i) {
            for (unsigned int j = 0; j < m_number_of_extruders; ++j) {
                const float value = values[m_number_of_extruders * i + j];
                if (i == j)
                    m_matrix_diagonal[i] = value;
                else
                    edit_boxes[j][i]->ChangeValue(wxString::Format("%.*g", std::numeric_limits<float>::max_digits10,
                        static_cast<double>(value)));
            }
        }
    } else {
        for (unsigned int i = 0; i < m_number_of_extruders; ++i) {
            // Clamp before the integer conversion as well: a finite float can
            // exceed the integer range, even though SpinInput clamps integers.
            m_old[i]->SetValue(static_cast<int>(std::clamp(double(values[2 * i]),
                double(m_old[i]->GetMin()), double(m_old[i]->GetMax()))));
            m_new[i]->SetValue(static_cast<int>(std::clamp(double(values[2 * i + 1]),
                double(m_new[i]->GetMin()), double(m_new[i]->GetMax()))));
        }
    }
    m_suppress_change_notification = false;
    notify_values_changed();
}

// Reads values from simple mode to save them for next time:
std::vector<float> WipingPanel::read_extruders_values() {
    std::vector<float> output;
    for (unsigned int i=0;i<m_number_of_extruders;++i) {
        output.push_back(m_old[i]->GetValue());
        output.push_back(m_new[i]->GetValue());
    }
    return output;
}

// This updates the "advanced" matrix based on values from "simple" mode
void WipingPanel::fill_in_matrix() {
    m_suppress_change_notification = true;
    for (unsigned i=0;i<m_number_of_extruders;++i) {
        for (unsigned j=0;j<m_number_of_extruders;++j) {
            if (i==j) continue;
                edit_boxes[j][i]->SetValue(wxString("")<< (m_old[i]->GetValue() + m_new[j]->GetValue()));
        }
    }
    m_suppress_change_notification = false;
}



// Function to check if simple and advanced settings are matching
bool WipingPanel::advanced_matches_simple() {
    for (unsigned i=0;i<m_number_of_extruders;++i) {
        for (unsigned j=0;j<m_number_of_extruders;++j) {
            if (i==j) continue;
            if (edit_boxes[j][i]->GetValue() != (wxString("")<< (m_old[i]->GetValue() + m_new[j]->GetValue())))
                return false;
        }
    }
    return true;
}


// Switches the dialog from simple to advanced mode and vice versa
void WipingPanel::toggle_advanced(bool user_action) {
    if (m_advanced && !advanced_matches_simple() && user_action) {
        if (MessageDialog(this, _L("Switching to simple settings will discard changes done in the advanced mode!\n\nDo you want to proceed?"),
                            _L("Warning"),wxYES_NO|wxICON_EXCLAMATION).ShowModal() != wxID_YES)
            return;
    }
    if (user_action)
        m_advanced = !m_advanced;                // user demands a change -> toggle
    else
        m_advanced = !advanced_matches_simple(); // if called from constructor, show what is appropriate

    (m_advanced ? m_page_advanced : m_page_simple)->Show();
	(!m_advanced ? m_page_advanced : m_page_simple)->Hide();

    m_widget_button->SetLabel(m_advanced ? _(L("Show simplified settings")) : _(L("Show advanced settings")));
    if (m_advanced)
        if (user_action) fill_in_matrix();  // otherwise keep values loaded from config

   m_sizer->Layout();
   Refresh();
   notify_values_changed();
}

// Changes triggered by programmatic synchronization are ignored; only user-visible edits refresh the serialized field.
void WipingPanel::notify_values_changed()
{
    if (!m_suppress_change_notification && m_values_changed)
        m_values_changed();
}
