#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Field.hpp"
#include "wxExtensions.hpp"
#include "Plater.hpp"
#include "MainFrame.hpp"
#include "format.hpp"

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <regex>
#include <wx/numformatter.h>
#include <wx/tooltip.h>
#include <wx/notebook.h>
#include <wx/listbook.h>
#include <wx/richtooltip.h>
#ifdef __WXGTK2__
#include <wx/tglbtn.h>
#endif
#include <wx/tokenzr.h>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/log/trivial.hpp>
#include "OG_CustomCtrl.hpp"
#include "MsgDialog.hpp"
#include "BitmapComboBox.hpp"

#ifdef __WXOSX__
#define wxOSX true
#else
#define wxOSX false
#endif

namespace Slic3r { namespace GUI {

wxString double_to_string(double const value, const int max_precision /*= 6*/)
{
// Style_NoTrailingZeroes does not work on OSX. It also does not work correctly with some locales on Windows.
//	return wxNumberFormatter::ToString(value, max_precision, wxNumberFormatter::Style_NoTrailingZeroes);

	wxString s = wxNumberFormatter::ToString(value, max_precision, wxNumberFormatter::Style_None);

	// The following code comes from wxNumberFormatter::RemoveTrailingZeroes(wxString& s)
	// with the exception that here one sets the decimal separator explicitely to dot.
    // If number is in scientific format, trailing zeroes belong to the exponent and cannot be removed.
    if (s.find_first_of("eE") == wxString::npos) {
        char dec_sep = is_decimal_separator_point() ? '.' : ',';
        const size_t posDecSep = s.find(dec_sep);
	    // No decimal point => removing trailing zeroes irrelevant for integer number.
	    if (posDecSep != wxString::npos) {
		    // Find the last character to keep.
		    size_t posLastNonZero = s.find_last_not_of("0");
            // If it's the decimal separator itself, don't keep it either.
		    if (posLastNonZero == posDecSep)
		        -- posLastNonZero;
		    s.erase(posLastNonZero + 1);
		    // Remove sign from orphaned zero.
		    if (s.compare("-0") == 0)
		        s = "0";
            if (s.Last() == '.')
                s.erase(s.length() -1);
		}
	}

    return s;
}

wxString get_points_string(const std::vector<Vec2d>& values)
{
    wxString ret_str;
	for (size_t i = 0; i < values.size(); ++ i) {
		const Vec2d& el = values[i];
		ret_str += wxString::Format((i == 0) ? "%ix%i" : ", %ix%i", int(el[0]), int(el[1]));
	}
    return ret_str;
}


Field::~Field()
{
	if (m_on_kill_focus)
		m_on_kill_focus = nullptr;
	if (m_on_change)
		m_on_change = nullptr;
	if (m_back_to_initial_value)
		m_back_to_initial_value = nullptr;
	if (m_back_to_sys_value)
		m_back_to_sys_value = nullptr;
	if (getWindow()) {
		wxWindow* win = getWindow();
		win->Destroy();
		win = nullptr;
	}
}

void Field::PostInitialize()
{
	auto color = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);

	switch (m_opt.type)
	{
	case coPercents:
	case coFloats:
	case coFloatsOrPercents:
	case coStrings:
	case coBools:
	case coPoints:
	case coInts: {
		auto tag_pos = m_opt_id.find("#");
		if (tag_pos != std::string::npos)
			m_opt_idx = stoi(m_opt_id.substr(tag_pos + 1, m_opt_id.size()));
		break;
	}
	default:
		break;
	}

    // initialize m_unit_value
    m_em_unit = em_unit(m_parent);
    parent_is_custom_ctrl = dynamic_cast<OG_CustomCtrl*>(m_parent) != nullptr;

	BUILD();

	// For the mode, when settings are in non-modal dialog, neither dialog nor tabpanel doesn't receive wxEVT_KEY_UP event, when some field is selected.
	// So, like a workaround check wxEVT_KEY_UP event for the Filed and switch between tabs if Ctrl+(1-6) was pressed 
	if (getWindow())
		getWindow()->Bind(wxEVT_KEY_UP, [](wxKeyEvent& evt) {
            if ((evt.GetModifiers() & wxMOD_CONTROL) != 0 && (evt.GetModifiers() & wxMOD_ALT) == 0) {
                MainFrame::ETabType tab_id = MainFrame::ETabType::Any;
                switch (evt.GetKeyCode()) {
                case '1': { tab_id = MainFrame::ETabType::Plater3D; break; }
                case '2': { tab_id = MainFrame::ETabType::PlaterPreview; break; }
                case '3': { tab_id = MainFrame::ETabType::PlaterGcode; break; }
                case '4': { tab_id = MainFrame::ETabType::PrintSettings; break; }
                case '5': { tab_id = MainFrame::ETabType::FilamentSettings; break; }
                case '6': { tab_id = MainFrame::ETabType::PrinterSettings; break; }
#ifdef __APPLE__
				case 'f':
#else /* __APPLE__ */
				case WXK_CONTROL_F:
#endif /* __APPLE__ */
				case 'F': { wxGetApp().plater()->search(false); break; }
			    default: break;
			    }
                if (tab_id < MainFrame::ETabType::Any) {
                    wxGetApp().mainframe->select_tab(tab_id);
                    if (wxGetApp().mainframe->get_layout() == MainFrame::ESettingsLayout::Tabs
                        || wxGetApp().mainframe->get_layout() == MainFrame::ESettingsLayout::Old
                        || tab_id >= MainFrame::ETabType::PrintSettings)
                        // tab panel should be focused for correct navigation between tabs
                        wxGetApp().tab_panel()->SetFocus();
                }
		    }

		    evt.Skip();
	    });
}

// Values of width to alignments of fields
int Field::def_width()			{ return 8; }
int Field::def_width_wider()	{ return 16; }
int Field::def_width_thinner()	{ return 4; }

void Field::on_kill_focus()
{
	// call the registered function if it is available
    if (m_on_kill_focus!=nullptr)
        m_on_kill_focus(m_opt_id);
}

void Field::on_change_field()
{
//       std::cerr << "calling Field::_on_change \n";
    if (m_on_change != nullptr && !m_disable_change_event) {
        m_on_change(m_opt_id, get_value());
    }
}

void Field::on_back_to_initial_value()
{
	if (m_back_to_initial_value != nullptr && m_is_modified_value)
		m_back_to_initial_value(m_opt_id);
}

void Field::on_back_to_sys_value()
{
	if (m_back_to_sys_value != nullptr && m_is_nonsys_value)
		m_back_to_sys_value(m_opt_id);
}

wxString Field::get_tooltip_text(const wxString& default_string)
{
	wxString tooltip_text("");
	wxString tooltip = _(m_opt.tooltip);
    update_Slic3r_string(tooltip);

    std::string opt_id = m_opt_id;
    auto hash_pos = opt_id.find("#");
    if (hash_pos != std::string::npos) {
        opt_id.replace(hash_pos, 1,"[");
        opt_id += "]";
    }

	if (tooltip.length() > 0)
        tooltip_text = tooltip + "\n" + _(L("default value")) + "\t: " +
        (boost::iends_with(opt_id, "_gcode") ? "\n" : "") + default_string + "\n" +
        _(L("parameter name")) + "\t: " + opt_id;

	return tooltip_text;
}

wxString Field::get_rich_tooltip_text(const wxString& default_string)
{
    wxString tooltip_text("");
    wxString tooltip = _(m_opt.tooltip);
    update_Slic3r_string(tooltip);
    std::wstring wtooltip = tooltip.ToStdWstring();
    std::wstring wtooltip_text;

    std::string opt_id = m_opt_id;
    auto hash_pos = opt_id.find("#");
    if (hash_pos != std::string::npos) {
        opt_id.replace(hash_pos, 1, "[");
        opt_id += "]";
    }

    //add "\n" to long tooltip lines
    int length = 0;
    for (int i = 0; i < wtooltip.size(); i++) {
        if (length >= 80 && wtooltip[i] == u' ')
            wtooltip_text.push_back(u'\n');
        else
            wtooltip_text.push_back(wtooltip[i]);
        length++;
        if (wtooltip_text.back() == u'\n')
            length = 0;
    }

    if (tooltip.length() > 0)
        tooltip_text = wtooltip_text + "\n" + _(L("default value")) + ": " +
        (boost::iends_with(opt_id, "_gcode") ? "\n" : "") + default_string;

    return tooltip_text;
}

wxString Field::get_rich_tooltip_title(const wxString& default_string)
{

    std::string opt_id = m_opt_id;
    auto hash_pos = opt_id.find("#");
    if (hash_pos != std::string::npos) {
        opt_id.replace(hash_pos, 1, "[");
        opt_id += "]";
    }

    return opt_id + ":";
}

void Field::set_tooltip(const wxString& default_string, wxWindow* window) {
    if (window == nullptr)
        window = getWindow();
    if (get_app_config()->get("use_rich_tooltip") == "1") {
        this->m_rich_tooltip_timer.m_value = default_string;
        window->Bind(wxEVT_ENTER_WINDOW, [this, window](wxMouseEvent& event) {
            if (!this->m_rich_tooltip_timer.IsRunning()
#ifdef __WXMSW__
                && wxGetActiveWindow() //don't activate if the currrent app is not the focus. (deactivated for linux as it check the field instead)
#endif /* __WXMSW__ */
                ) {
                this->m_rich_tooltip_timer.m_current_window = window;
                this->m_rich_tooltip_timer.m_is_rich_tooltip_ready = true;
                this->m_rich_tooltip_timer.StartOnce(500);
            }
            });
        window->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& event) {
            this->m_rich_tooltip_timer.m_is_rich_tooltip_ready = false;
            wxWindowList tipWindow = this->getWindow()->GetChildren();
            if (tipWindow.size() > 0) {
                wxWindow* tooltipWindow = tipWindow.GetLast()->GetData();
                if (tooltipWindow && tooltipWindow == this->m_rich_tooltip_timer.m_current_rich_tooltip) {
                    tooltipWindow->Hide();// DismissAndNotify();
                }
            }
            });
    } else
        window->SetToolTip(get_tooltip_text(default_string));
}

void RichTooltipTimer::Notify() {
    if (this->m_is_rich_tooltip_ready && m_current_window && !m_current_window->HasFocus()
#ifdef __WXMSW__
        && wxGetActiveWindow() //don't activate if the currrent app is not the focus. (deactivated for linux as it check the field instead)
#endif /* __WXMSW__ */
        ) {
        this->m_previous_focus = wxGetActiveWindow()->FindFocus();
        this->m_current_rich_tooltip = nullptr;
        wxRichToolTip richTooltip(
            m_field->get_rich_tooltip_title(this->m_value),
            m_field->get_rich_tooltip_text(this->m_value));
        richTooltip.SetTimeout(120000, 0);
        richTooltip.ShowFor(m_current_window);
        wxWindowList tipWindow = m_current_window->GetChildren();
        this->m_current_rich_tooltip = tipWindow.GetLast()->GetData();
        this->m_current_rich_tooltip->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_BACKGROUND));
#ifdef _WIN32
        this->m_current_rich_tooltip->SetForegroundColour(wxGetApp().get_label_clr_default());
#else
        this->m_current_rich_tooltip->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
#endif /* _WIN32 */
        this->m_current_rich_tooltip->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& event) {
            this->m_is_rich_tooltip_ready = false;
            wxWindowList tipWindow = m_current_window->GetChildren();
            if (tipWindow.size() > 0) {
                wxWindow* tooltipWindow = tipWindow.GetLast()->GetData();
                if (tooltipWindow && tooltipWindow == this->m_current_rich_tooltip) {
                    tooltipWindow->Hide();// DismissAndNotify();
                }
            }
            });
        this->m_current_rich_tooltip->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event) {
            CallAfter([this]() {
                if (this->m_previous_focus) this->m_previous_focus->SetFocus();
                });
            });
    }
}

bool Field::is_matched(const std::string& string, const std::string& pattern)
{
	std::regex regex_pattern(pattern, std::regex_constants::icase); // use ::icase to make the matching case insensitive like /i in perl
	return std::regex_match(string, regex_pattern);
}

static wxString na_value() { return _(L("N/A")); }

//TODO move value verification on another methos that won't be called at each value.get()
void Field::get_value_by_opt_type(wxString& str, const bool check_value/* = true*/)
{
    double val = 0;
	switch (m_opt.type) {
	case coInt:
		m_value = wxAtoi(str);
        val = wxAtoi(str);
		break;
	case coPercent:
	case coPercents:
	case coFloats:
	case coFloat:{
		if (m_opt.type == coPercent && !str.IsEmpty() &&  str.Last() == '%')
			str.RemoveLast();
		else if (!str.IsEmpty() && str.Last() == '%')
        {
            if (!check_value) {
                m_value.clear();
                break;
            }

			wxString label = m_opt.full_label.empty() ? _(m_opt.label) : _(m_opt.full_label);
            show_error(m_parent, from_u8((boost::format(_utf8(L("%s doesn't support percentage"))) % label).str()));
			set_value(double_to_string(m_opt.min, m_opt.precision), true);
			m_value = double(m_opt.min);
			break;
		}

        bool is_na_value = m_opt.nullable && str == na_value();

        const char dec_sep = is_decimal_separator_point() ? '.' : ',';
        const char dec_sep_alt = dec_sep == '.' ? ',' : '.';
        // Replace the first incorrect separator in decimal number, 
        // if this value doesn't "N/A" value in some language
        // see https://github.com/prusa3d/PrusaSlicer/issues/6921
        if (!is_na_value && str.Replace(dec_sep_alt, dec_sep, false) != 0)
            set_value(str, false);

        if (str == dec_sep)
            val = 0.0;
        else
        {
            if (is_na_value)
                val = ConfigOptionFloatsNullable::nil_value();
            else if (!str.ToDouble(&val))
            {
                if (!check_value) {
                    m_value.clear();
                    break;
                }
                show_error(m_parent, _(L("Invalid numeric input.")));
                set_value(double_to_string(val, m_opt.precision), true);
            }
            if (m_opt.min > val || val > m_opt.max)
            {
                if (!check_value) {
                    m_value.clear();
                    break;
                }
                if (m_opt_id == "extrusion_multiplier") {
                    if (m_value.empty() || boost::any_cast<double>(m_value) != val) {
                        wxString msg_text = format_wxstr(_L("Input value is out of range\n"
                            "Are you sure that %s is a correct value and that you want to continue?"), str);
//                        wxMessageDialog dialog(m_parent, msg_text, _L("Parameter validation") + ": " + m_opt_id, wxICON_WARNING | wxYES | wxNO);
                        WarningDialog dialog(m_parent, msg_text, _L("Parameter validation") + ": " + m_opt_id, wxYES | wxNO);
                        if (dialog.ShowModal() == wxID_NO) {
                            if (m_value.empty()) {
                                if (m_opt.min > val) val = m_opt.min;
                                if (val > m_opt.max) val = m_opt.max;
                            }
                            else
                                val = boost::any_cast<double>(m_value);
                            set_value(double_to_string(val, m_opt.precision), true);
                        }
                    }
                }
                else {
                    show_error(m_parent, _L("Input value is out of range"));
                    if (m_opt.min > val) val = m_opt.min;
                    if (val > m_opt.max) val = m_opt.max;
                    set_value(double_to_string(val, m_opt.precision), true);
                }
            }
        }
        m_value = val;
		break; }
	case coString:
	case coStrings:
        m_value = std::string(str.ToUTF8().data());
        break;
    case coFloatsOrPercents:
    case coFloatOrPercent: {
        if (!str.IsEmpty()) {
            if ("infill_overlap" == m_opt_id && m_last_validated_value != str) {
                bool bad = false;
                if (str.Last() != '%') {
                    if (str.ToDouble(&val)) {
                        const DynamicPrintConfig& printer_config = wxGetApp().preset_bundle->printers.get_edited_preset().config;
                            const std::vector<double>& nozzle_diameters = printer_config.option<ConfigOptionFloats>("nozzle_diameter")->values;
                            double nozzle_diameter = 0;
                            for (double diameter : nozzle_diameters)
                                nozzle_diameter = std::max(nozzle_diameter, diameter);
                        if (val > nozzle_diameter / 2) {
                            bad = true;
                        }
                    }
                } else {
                    if (str.substr(0, str.size() - 1).ToCDouble(&val)) {
                        if (val >= 50) {
                            bad = true;
                        }
                    }
                }
                if (bad && check_value) {
                    const wxString msg_text = from_u8(_u8L("The infill / perimeter encroachment can't be higher than half of the perimeter width.\n"
                        "Are you sure to use this value?"));
                    wxMessageDialog dialog(m_parent, msg_text, _L("Parameter validation") + ": " + m_opt_id, wxICON_WARNING | wxYES | wxNO);
                    auto ret = dialog.ShowModal();
                    if (ret == wxID_NO) {
                        str = from_u8("49%");
                        m_last_validated_value = str;
                        set_value(str, false);
                        str = m_last_validated_value;
                    }
                    m_last_validated_value = str;
                }
            }
            else if (str.Last() != '%') {
                const char dec_sep = is_decimal_separator_point() ? '.' : ',';
                const char dec_sep_alt = dec_sep == '.' ? ',' : '.';
                // Replace the first incorrect separator in decimal number.
                if (str.Replace(dec_sep_alt, dec_sep, false) != 0)
                    set_value(str, false);

                // remove space and "mm" substring, if any exists
                str.Replace(" ", "", true);
                str.Replace("m", "", true);

                if (m_opt.nullable && str == na_value()) {
                    val = ConfigOptionFloatsNullable::nil_value();
                    str = "nan";
                } else if (!str.ToDouble(&val)) {
                    if (!check_value) {
                        m_value.clear();
                        break;
                    }
                    show_error(m_parent, _(L("Invalid numeric input.")));
                    set_value(double_to_string(val, m_opt.precision), true);
                } else {

                    //at least check min, as we can want a 0 min
                    if (m_opt.min > val)
                    {
                        if (!check_value) {
                            m_value.clear();
                            break;
                        }
                        show_error(m_parent, _(L("Input value is out of range")));
                        if (m_opt.min > val) val = m_opt.min;
                        set_value(double_to_string(val, m_opt.precision), true);
                    } else if (m_value.empty() || into_u8(str) != boost::any_cast<std::string>(m_value)) {
                        bool not_ok = (m_opt.sidetext.rfind("mm/s") != std::string::npos && val > m_opt.max);
                        if (!not_ok && m_opt.max_literal.value != 0 && val != 0) {
                            if (m_opt.max_literal.percent) {
                                const DynamicPrintConfig& printer_config = wxGetApp().preset_bundle->printers.get_edited_preset().config;
                                const std::vector<double>& nozzle_diameters = printer_config.option<ConfigOptionFloats>("nozzle_diameter")->values;
                                double nozzle_diameter = 0;
                                for (double diameter : nozzle_diameters)
                                    nozzle_diameter = std::max(nozzle_diameter, diameter);
                                if (m_opt.max_literal.value > 0)
                                    not_ok = val > nozzle_diameter * m_opt.max_literal.value;
                                else
                                    not_ok = val < nozzle_diameter* (-m_opt.max_literal.value);
                            } else {
                                if (m_opt.max_literal.value > 0)
                                    not_ok = val > m_opt.max_literal.value;
                                else
                                    not_ok = val < -m_opt.max_literal.value;
                            }
                        }
                        if (not_ok && m_last_validated_value != str) {
                            if (!check_value) {
                                m_value.clear();
                                break;
                            }

                            bool infill_anchors = m_opt.opt_key == "infill_anchor" || m_opt.opt_key == "infill_anchor_max";

                            const std::string sidetext = m_opt.sidetext.rfind("mm/s") != std::string::npos ? "mm/s" : "mm";
                            const wxString stVal = double_to_string(val, m_opt.precision);
                            const wxString msg_text = from_u8((boost::format(_u8L("Do you mean %s%% instead of %s %s?\n"
                                "Select YES if you want to change this value to %s%%, \n"
                                "or NO if you are sure that %s %s is a correct value.")) % stVal % stVal % sidetext % stVal % stVal % sidetext).str());
                            WarningDialog dialog(m_parent, msg_text, _L("Parameter validation") + ": " + m_opt_id, wxYES | wxNO);
                            if ((!infill_anchors || val > 100) && dialog.ShowModal() == wxID_YES) {
                                str += "%";
                                m_last_validated_value = str;
                                set_value(str, false/*true*/);
                                str = m_last_validated_value;
                            } else
                                set_value(stVal, false); // it's no needed but can be helpful, when inputted value contained "," instead of "."
                            m_last_validated_value = str;
                        }
                    }
                }
            } else {
                str.ToDouble(&val);
            }
        }

        m_value = into_u8(str);
        break;
    }
    case coPoints: {
        std::vector<Vec2d> out_values;
        str.Replace(" ", wxEmptyString, true);
        if (!str.IsEmpty()) {
            bool invalid_val = false;
            bool out_of_range_val = false;
            wxStringTokenizer points(str, ",");
            while (points.HasMoreTokens()) {
                wxString token = points.GetNextToken();
                double x, y;
                wxStringTokenizer point(token, "x");
                if (point.HasMoreTokens()) {
                    wxString x_str = point.GetNextToken();
                    if (x_str.ToDouble(&x) && point.HasMoreTokens()) {
                        wxString y_str = point.GetNextToken();
                        if (y_str.ToDouble(&y) && !point.HasMoreTokens()) {
                            if (m_opt.min <= x && x <= m_opt.max && m_opt.min <= y && y <= m_opt.max) {
                                out_values.push_back(Vec2d(x, y));
                                continue;
                            }
                            out_of_range_val = true;
                            break;
                        }
                    }
                }
                invalid_val = true;
                break;
            }

            if (out_of_range_val) {
                wxString text_value;
                if (!m_value.empty())
                    text_value = get_points_string(boost::any_cast<std::vector<Vec2d>>(m_value));
                set_value(text_value, true);
                show_error(m_parent, _L("Input value is out of range"));
            }
            else if (invalid_val) {
                wxString text_value;
                if (!m_value.empty())
                    text_value = get_points_string(boost::any_cast<std::vector<Vec2d>>(m_value));
                set_value(text_value, true);
                show_error(m_parent, format_wxstr(_L("Invalid input format. Expected vector of dimensions in the following format: \"%1%\""),"XxY, XxY, ..." ));
            }
        }

        m_value = out_values;
        break; }

	default:
		break;
	}

    if (!Field::warn_zero_gapfillspeed && ("max_volumetric_speed" == m_opt_id || "gap_fill_speed" == m_opt_id)) {
        bool show_warning = false;
        const DynamicPrintConfig& print_config = wxGetApp().preset_bundle->fff_prints.get_edited_preset().config;
        if ("max_volumetric_speed" == m_opt_id && val > 0)
            show_warning = print_config.option<ConfigOptionFloatOrPercent>("gap_fill_speed")->value == 0;
        if ("gap_fill_speed" == m_opt_id && val == 0)
            show_warning = true;
        if (show_warning) {
            const wxString msg_text = from_u8(_u8L("Auto Speed will try to maintain a constant flow rate accross all print moves."
                "\nIt is not recommended to include gap moves to the Auto Speed calculation(by setting this value to 0)."
                "\nVery thin gap extrusions will often not max out the flow rate of your printer."
                "\nAs a result, this will cause Auto Speed to lower the speeds of all other print moves to match the low flow rate of these thin gaps."));
            wxMessageDialog dialog(m_parent, msg_text, _L("Parameter validation") + ": " + m_opt_id, wxICON_WARNING | wxOK);
            dialog.ShowModal();
            Field::warn_zero_gapfillspeed = true;
        }
    }
}

void Field::msw_rescale()
{
	// update em_unit value
	m_em_unit = em_unit(m_parent);
}

void Field::sys_color_changed()
{
#ifdef _WIN32
	if (wxWindow* win = this->getWindow())
		wxGetApp().UpdateDarkUI(win);
#endif
}

template<class T>
bool is_defined_input_value(wxWindow* win, const ConfigOptionType& type)
{
    if (!win || (static_cast<T*>(win)->GetValue().empty() && type != coString && type != coStrings && type != coPoints))
        return false;
    return true;
}

void TextCtrl::BUILD() {
    auto size = wxSize((this->m_opt.type == ConfigOptionType::coPercent ? def_width_thinner() : def_width())*m_em_unit, wxDefaultCoord);
    if (m_opt.height >= 0) size.SetHeight(m_opt.height*m_em_unit);
    if (m_opt.width >= 0) size.SetWidth(m_opt.width*m_em_unit);

	wxString text_value = wxString("");

	switch (m_opt.type) {
    case coFloatOrPercent:
    {
        text_value = double_to_string(m_opt.default_value->getFloat(), m_opt.precision);
        if (m_opt.get_default_value<ConfigOptionFloatOrPercent>()->percent)
            text_value += "%";
        break;
    }
	case coPercent:
	{
		text_value = double_to_string(m_opt.default_value->getFloat(), m_opt.precision);
		text_value += "%";
		break;
	}
	case coPercents:
	case coFloats:
	case coFloat:
	{
		double val = m_opt.type == coFloats ?
			m_opt.get_default_value<ConfigOptionFloats>()->get_at(m_opt_idx) :
			m_opt.type == coFloat ?
				m_opt.default_value->getFloat() :
				m_opt.get_default_value<ConfigOptionPercents>()->get_at(m_opt_idx);
		text_value = double_to_string(val, m_opt.precision);
		break;
	}
    case coFloatsOrPercents:
    {
        const ConfigOptionFloatsOrPercents* cofop = m_opt.get_default_value<ConfigOptionFloatsOrPercents>();
        text_value = double_to_string(cofop->get_at(m_opt_idx).value, m_opt.precision);
        if (cofop->get_at(m_opt_idx).percent)
            text_value += "%";
        break;
    }
	case coString:
		text_value = m_opt.get_default_value<ConfigOptionString>()->value;
		break;
	case coStrings:
	{
		const ConfigOptionStrings *vec = m_opt.get_default_value<ConfigOptionStrings>();
		if (vec == nullptr || vec->empty()) break; //for the case of empty default value
		text_value = vec->get_at(m_opt_idx);
		break;
	}
    case coPoint:
        text_value = get_points_string({ m_opt.get_default_value<ConfigOptionPoint>()->value });
        break;
    case coPoints:
        text_value = get_points_string(m_opt.get_default_value<ConfigOptionPoints>()->values);
        break;
	default:
		break;
	}
    m_last_meaningful_value = text_value;

    long style = m_opt.multiline ? wxTE_MULTILINE : wxTE_PROCESS_ENTER;
#ifdef _WIN32
	style |= wxBORDER_SIMPLE;
#endif
    wxTextCtrl* temp = new wxTextCtrl(m_parent, wxID_ANY, text_value, wxDefaultPosition, size, style);
    if (parent_is_custom_ctrl && m_opt.height < 0)
        opt_height = (double)temp->GetSize().GetHeight()/m_em_unit;
    temp->SetFont(m_opt.is_code ?
                  Slic3r::GUI::wxGetApp().code_font():
                  Slic3r::GUI::wxGetApp().normal_font());
	wxGetApp().UpdateDarkUI(temp);

    if (! m_opt.multiline && !wxOSX)
		// Only disable background refresh for single line input fields, as they are completely painted over by the edit control.
		// This does not apply to the multi-line edit field, where the last line and a narrow frame around the text is not cleared.
		temp->SetBackgroundStyle(wxBG_STYLE_PAINT);
#ifdef __WXOSX__
    temp->OSXDisableAllSmartSubstitutions();
#endif // __WXOSX__

    if (style & wxTE_PROCESS_ENTER) {
        temp->Bind(wxEVT_TEXT_ENTER, ([this, temp](wxEvent& e)
        {
#if !defined(__WXGTK__)
            e.Skip();
            temp->GetToolTip()->Enable(true);
#endif // __WXGTK__
            EnterPressed enter(this);
            propagate_value();
        }), temp->GetId());
    }

	temp->Bind(wxEVT_LEFT_DOWN, ([temp](wxEvent& event)
	{
		//! to allow the default handling
		event.Skip();
		//! eliminating the g-code pop up text description
		bool flag = false;
#ifdef __WXGTK__
		// I have no idea why, but on GTK flag works in other way
		flag = true;
#endif // __WXGTK__
		temp->GetToolTip()->Enable(flag);
	}), temp->GetId());

	temp->Bind(wxEVT_KILL_FOCUS, ([this, temp](wxEvent& e)
	{
		e.Skip();
#if !defined(__WXGTK__)
		temp->GetToolTip()->Enable(true);
#endif // __WXGTK__
        if (!bEnterPressed)
            propagate_value();
	}), temp->GetId());
/*
	// select all text using Ctrl+A
	temp->Bind(wxEVT_CHAR, ([temp](wxKeyEvent& event)
	{
		if (wxGetKeyState(wxKeyCode('A')) && wxGetKeyState(WXK_CONTROL))
			temp->SetSelection(-1, -1); //select all
		event.Skip();
	}));
*/
    // recast as a wxWindow to fit the calling convention
    window = dynamic_cast<wxWindow*>(temp);

    this->set_tooltip(text_value);
}	

bool TextCtrl::value_was_changed()
{
    if (m_value.empty())
        return true;

    boost::any val = m_value;
    wxString ret_str = static_cast<wxTextCtrl*>(window)->GetValue();
    // update m_value!
    // ret_str might be changed inside get_value_by_opt_type
    get_value_by_opt_type(ret_str);

    switch (m_opt.type) {
    case coInt:
        return boost::any_cast<int>(m_value) != boost::any_cast<int>(val);
    case coPercent:
    case coPercents:
    case coFloats:
    case coFloat: {
        if (m_opt.nullable && std::isnan(boost::any_cast<double>(m_value)) &&
                              std::isnan(boost::any_cast<double>(val)))
            return false;
        return boost::any_cast<double>(m_value) != boost::any_cast<double>(val);
    }
    case coString:
    case coStrings:
    case coFloatOrPercent:
    case coFloatsOrPercents:
        return boost::any_cast<std::string>(m_value) != boost::any_cast<std::string>(val);
    default:
        return true;
    }
}

void TextCtrl::propagate_value()
{
	if (!is_defined_input_value<wxTextCtrl>(window, m_opt.type) )
		// on_kill_focus() cause a call of OptionsGroup::reload_config(),
		// Thus, do it only when it's really needed (when undefined value was input)
        on_kill_focus();
	else if (value_was_changed())
        on_change_field();
}

void TextCtrl::set_value(const boost::any& value, bool change_event/* = false*/) {
    m_disable_change_event = !change_event;
    if (m_opt.nullable) {
        const bool m_is_na_val = boost::any_cast<wxString>(value) == na_value();
        if (!m_is_na_val)
            m_last_meaningful_value = value;
        dynamic_cast<wxTextCtrl*>(window)->SetValue(m_is_na_val ? na_value() : boost::any_cast<wxString>(value));
    }
    else
        dynamic_cast<wxTextCtrl*>(window)->SetValue(boost::any_cast<wxString>(value));
    m_disable_change_event = false;

    if (!change_event) {
        wxString ret_str = static_cast<wxTextCtrl*>(window)->GetValue();
        /* Update m_value to correct work of next value_was_changed().
         * But after checking of entered value, don't fix the "incorrect" value and don't show a warning message,
         * just clear m_value in this case.
         */
        get_value_by_opt_type(ret_str, false);
    }
}

void TextCtrl::set_last_meaningful_value()
{
    dynamic_cast<wxTextCtrl*>(window)->SetValue(boost::any_cast<wxString>(m_last_meaningful_value));
    propagate_value();
}

void TextCtrl::set_na_value()
{
    dynamic_cast<wxTextCtrl*>(window)->SetValue(na_value());
    propagate_value();
}

boost::any& TextCtrl::get_value()
{
	wxString ret_str = static_cast<wxTextCtrl*>(window)->GetValue();
	// update m_value
	get_value_by_opt_type(ret_str);

	return m_value;
}

void TextCtrl::msw_rescale()
{
    Field::msw_rescale();
    auto size = wxSize(def_width() * m_em_unit, wxDefaultCoord);

    if (m_opt.height >= 0)
        size.SetHeight(m_opt.height*m_em_unit);
    else if (parent_is_custom_ctrl && opt_height > 0)
        size.SetHeight(lround(opt_height*m_em_unit));
    if (m_opt.width >= 0) size.SetWidth(m_opt.width*m_em_unit);

    if (size != wxDefaultSize)
    {
        wxTextCtrl* field = dynamic_cast<wxTextCtrl*>(window);
        if (parent_is_custom_ctrl)
            field->SetSize(size);
        else
            field->SetMinSize(size);
    }

}

void TextCtrl::enable() { dynamic_cast<wxTextCtrl*>(window)->Enable(); dynamic_cast<wxTextCtrl*>(window)->SetEditable(true); }
void TextCtrl::disable() { dynamic_cast<wxTextCtrl*>(window)->Disable(); dynamic_cast<wxTextCtrl*>(window)->SetEditable(false); }

#ifdef __WXGTK__
void TextCtrl::change_field_value(wxEvent& event)
{
    if ((bChangedValueEvent = (event.GetEventType()==wxEVT_KEY_UP)))
		on_change_field();
    event.Skip();
};
#endif //__WXGTK__

void CheckBox::BUILD() {
    auto size = wxSize(wxDefaultSize);
    if (m_opt.height >= 0) size.SetHeight(m_opt.height * m_em_unit);
    if (m_opt.width >= 0) size.SetWidth(m_opt.width * m_em_unit);

    bool check_value = m_opt.type == coBool ?
        m_opt.default_value->getBool() : m_opt.type == coBools ?
        m_opt.get_default_value<ConfigOptionBools>()->get_at(m_opt_idx) :
        false;

    m_last_meaningful_value = static_cast<unsigned char>(check_value);

#ifdef __WXGTK2__
    //gtk2 can't resize checkboxes, so we are using togglable buttons instead
    if (m_em_unit > 14) {
        size = wxSize(def_width_thinner() * m_em_unit / 2, def_width_thinner() * m_em_unit / 2);
        auto temp = new wxToggleButton(m_parent, wxID_ANY, wxString(" "), wxDefaultPosition, size, m_opt.is_script ? wxCHK_3STATE : wxCHK_2STATE);
        temp->Bind(wxEVT_TOGGLEBUTTON, ([this, temp](wxCommandEvent e) {
            m_is_na_val = false;
            if (temp->GetValue())
                temp->SetLabel("X");
            else
                temp->SetLabel("");
            on_change_field();
        }), temp->GetId());
        // recast as a wxWindow to fit the calling convention
        window = dynamic_cast<wxWindow*>(temp);
    }
    else
#endif
    {
        // Set Label as a string of at least one space simbol to correct system scaling of a CheckBox
        auto temp = new wxCheckBox(m_parent, wxID_ANY, wxString(" "), wxDefaultPosition, size, m_opt.is_script ? wxCHK_3STATE : wxCHK_2STATE);
        temp->Bind(wxEVT_CHECKBOX, ([this](wxCommandEvent e) {
            m_is_na_val = false;
            on_change_field();
        }), temp->GetId());
        // recast as a wxWindow to fit the calling convention
        window = dynamic_cast<wxWindow*>(temp);
    }
    window->SetFont(Slic3r::GUI::wxGetApp().normal_font());
	if (!wxOSX) window->SetBackgroundStyle(wxBG_STYLE_PAINT);
	if (m_opt.readonly) window->Enable(false);
    

    //set value (need the window for the set_value)
    if (m_opt.is_script && !m_opt.default_script_value.empty())
        set_value(m_opt.default_script_value, false);
    else
        set_widget_value(check_value);

    // you need to set the window before the tooltip
    this->set_tooltip(check_value ? "true" : "false");
}

void CheckBox::set_widget_value(bool new_val)
{
    wxCheckBox* chk = dynamic_cast<wxCheckBox*>(window);
    if (chk != nullptr) {
        chk->SetValue(new_val);
    }
#ifdef __WXGTK2__
    else
    {
        wxToggleButton* tgl = dynamic_cast<wxToggleButton*>(window);
        if (tgl) {
            tgl->SetValue(new_val);
            if (new_val)
                tgl->SetLabel("X");
            else
                tgl->SetLabel("");
        }
    }
#endif
}

void CheckBox::set_value(const boost::any& value, bool change_event)
{
    m_disable_change_event = !change_event;
    if (m_opt.nullable) {
        m_is_na_val = boost::any_cast<unsigned char>(value) == ConfigOptionBoolsNullable::nil_value();
        if (!m_is_na_val)
            m_last_meaningful_value = value;
        set_widget_value(m_is_na_val ? false : boost::any_cast<unsigned char>(value) != 0);
    } else if (m_opt.is_script) {
        uint8_t val = boost::any_cast<uint8_t>(value);
        if (val == uint8_t(2) && dynamic_cast<wxCheckBox*>(window) != nullptr)
            dynamic_cast<wxCheckBox*>(window)->Set3StateValue(wxCheckBoxState::wxCHK_UNDETERMINED);
        else
            set_widget_value(val != 0);
    } else
        set_widget_value(boost::any_cast<bool>(value));
    m_disable_change_event = false;
}

void CheckBox::set_last_meaningful_value()
{
    if (m_opt.nullable) {
        m_is_na_val = false;
        set_widget_value(boost::any_cast<unsigned char>(m_last_meaningful_value) != 0);
        on_change_field();
    }
}

void CheckBox::set_na_value()
{
    if (m_opt.nullable) {
        m_is_na_val = true;
        set_widget_value(false);
        on_change_field();
    }
}

boost::any& CheckBox::get_value()
{
// 	boost::any m_value;
    bool value = false;
    wxCheckBox* chk = dynamic_cast<wxCheckBox*>(window);
    if (chk != nullptr) {
        value = chk->GetValue();
    }
#ifdef __WXGTK2__
    else
    {
        wxToggleButton* tgl = dynamic_cast<wxToggleButton*>(window);
        if (tgl) value = tgl->GetValue();
    }
#endif
	if (m_opt.type == coBool)
		m_value = static_cast<bool>(value);
	else
		m_value = m_is_na_val ? ConfigOptionBoolsNullable::nil_value() : static_cast<unsigned char>(value);
 	return m_value;
}

void CheckBox::msw_rescale()
{
    Field::msw_rescale();

    wxCheckBox* chk = dynamic_cast<wxCheckBox*>(window);
    if (chk != nullptr) {
        chk->SetMinSize(wxSize(-1, int(1.5f * chk->GetFont().GetPixelSize().y + 0.5f)));
    }
#ifdef __WXGTK2__
    else
    { //a bit useless as it's a windows-only func. To have a correct thing, you have to del the previous window and create a new one anyway.
        wxToggleButton* tgl = dynamic_cast<wxToggleButton*>(window);
        if (tgl) tgl->SetMinSize(wxSize(def_width_thinner() * m_em_unit / 2, def_width_thinner() * m_em_unit / 2));
    }
#endif
}


void SpinCtrl::BUILD() {
	auto size = wxSize(def_width() * m_em_unit, wxDefaultCoord);
    if (m_opt.height >= 0) size.SetHeight(m_opt.height*m_em_unit);
    if (m_opt.width >= 0) size.SetWidth(m_opt.width*m_em_unit);

	wxString	text_value = wxString("");
	int			default_value = 0;

	switch (m_opt.type) {
	case coInt:
		default_value = m_opt.default_value->getInt();
		text_value = wxString::Format(_T("%i"), default_value);
		break;
	case coInts:
	{
		const ConfigOptionInts *vec = m_opt.get_default_value<ConfigOptionInts>();
		if (vec == nullptr || vec->empty()) break;
		for (size_t id = 0; id < vec->size(); ++id)
		{
			default_value = vec->get_at(id);
			text_value += wxString::Format(_T("%i"), default_value);
		}
		break;
	}
	default:
		break;
	}

    const int min_val = m_opt.min == INT_MIN
#ifdef __WXOSX__
    // We will forcibly set the input value for SpinControl, since the value
    // inserted from the keyboard is not updated under OSX.
    // So, we can't set min control value bigger then 0.
    // Otherwise, it couldn't be possible to input from keyboard value
    // less then min_val.
    || m_opt.min > 0
#endif
    ? 0 : m_opt.min;
	const int max_val = m_opt.max < 2147483647 ? m_opt.max : 2147483647;

	auto temp = new wxSpinCtrl(m_parent, wxID_ANY, text_value, wxDefaultPosition, size,
		wxTE_PROCESS_ENTER | wxSP_ARROW_KEYS
#ifdef _WIN32
		| wxBORDER_SIMPLE
#endif 
		, min_val, max_val, default_value);

#ifdef __WXGTK3__
	wxSize best_sz = temp->GetBestSize();
	if (best_sz.x > size.x)
		temp->SetSize(wxSize(size.x + 2 * best_sz.y, best_sz.y));
#endif //__WXGTK3__
	temp->SetFont(Slic3r::GUI::wxGetApp().normal_font());
    if (!wxOSX) temp->SetBackgroundStyle(wxBG_STYLE_PAINT);
	wxGetApp().UpdateDarkUI(temp);

    if (m_opt.height < 0 && parent_is_custom_ctrl)
        opt_height = (double)temp->GetSize().GetHeight() / m_em_unit;

// XXX: On OS X the wxSpinCtrl widget is made up of two subwidgets, unfortunatelly
// the kill focus event is not propagated to the encompassing widget,
// so we need to bind it on the inner text widget instead. (Ugh.)
#ifdef __WXOSX__
	temp->GetText()->Bind(wxEVT_KILL_FOCUS, ([this](wxEvent& e)
#else
	temp->Bind(wxEVT_KILL_FOCUS, ([this](wxEvent& e)
#endif
	{
        e.Skip();
        if (bEnterPressed) {
            bEnterPressed = false;
            return;
        }

        propagate_value();
	}));

    temp->Bind(wxEVT_SPINCTRL, ([this](wxCommandEvent e) {  propagate_value();  }), temp->GetId());

    temp->Bind(wxEVT_TEXT_ENTER, ([this](wxCommandEvent e)
    {
        e.Skip();
        propagate_value();
        bEnterPressed = true;
    }), temp->GetId());

	temp->Bind(wxEVT_TEXT, ([this, temp](wxCommandEvent e)
	{
// 		# On OSX / Cocoa, wxSpinCtrl::GetValue() doesn't return the new value
// 		# when it was changed from the text control, so the on_change callback
// 		# gets the old one, and on_kill_focus resets the control to the old value.
// 		# As a workaround, we get the new value from $event->GetString and store
// 		# here temporarily so that we can return it from get_value()

		long value;
		const bool parsed = e.GetString().ToLong(&value);
        if (!parsed || value < INT_MIN || value > INT_MAX)
            tmp_value = UNDEF_VALUE;
        else {
            tmp_value = (int)std::min(std::max((double)value, m_opt.min), m_opt.max);
#ifdef __WXOSX__
            // Forcibly set the input value for SpinControl, since the value
            // inserted from the keyboard or clipboard is not updated under OSX
            temp->SetValue(tmp_value);
            // But in SetValue() is executed m_text_ctrl->SelectAll(), so
            // discard this selection and set insertion point to the end of string
            temp->GetText()->SetInsertionPointEnd();
#else
            // update value for the control only if it was changed in respect to the Min/max values
            if (tmp_value != (int)value) {
                temp->SetValue(tmp_value);
                // But after SetValue() cursor ison the first position
                // so put it to the end of string
                int pos = std::to_string(tmp_value).length();
                temp->SetSelection(pos, pos);
            }
#endif
        }
	}), temp->GetId());

	// recast as a wxWindow to fit the calling convention
	window = dynamic_cast<wxWindow*>(temp);

    //problem: it has 2 windows, with a child: the mouse enter event won't fire if in children! (also it need the windoww, so put it after)
    this->set_tooltip(text_value);
}

void SpinCtrl::propagate_value()
{
    // check if value was really changed
    if (boost::any_cast<int>(m_value) == tmp_value)
        return;

    if (tmp_value == UNDEF_VALUE) {
        on_kill_focus();
	} else {
#ifdef __WXOSX__
        // check input value for minimum
        if (m_opt.min > 0 && tmp_value < m_opt.min) {
            wxSpinCtrl* spin = static_cast<wxSpinCtrl*>(window);
            spin->SetValue(m_opt.min);
            spin->GetText()->SetInsertionPointEnd();
        }
#endif
        on_change_field();
    }
}

void SpinCtrl::msw_rescale()
{
    Field::msw_rescale();
    auto size = wxSize(wxDefaultSize);
    if (m_opt.height >= 0) size.SetHeight(m_opt.height * m_em_unit);
    if (m_opt.width >= 0) size.SetWidth(m_opt.width * m_em_unit);

    wxSpinCtrl* field = dynamic_cast<wxSpinCtrl*>(window);
    if (parent_is_custom_ctrl)
        field->SetSize(wxSize(def_width() * m_em_unit, lround(opt_height * m_em_unit)));
    else
        field->SetMinSize(wxSize(def_width() * m_em_unit, int(1.9f*field->GetFont().GetPixelSize().y)));
}

#ifdef __WXOSX__
static_assert(wxMAJOR_VERSION >= 3, "Use of wxBitmapComboBox on Settings Tabs requires wxWidgets 3.0 and newer");
using choice_ctrl = wxBitmapComboBox;
#else
#ifdef _WIN32
using choice_ctrl = BitmapComboBox;
#else
using choice_ctrl = wxComboBox;
#endif
#endif // __WXOSX__

void Choice::BUILD() {
    wxSize size(def_width_wider() * m_em_unit, wxDefaultCoord);
    if (m_opt.height >= 0) size.SetHeight(m_opt.height*m_em_unit);
    if (m_opt.width >= 0) size.SetWidth(m_opt.width*m_em_unit);

	choice_ctrl* temp;
    if (m_opt.gui_type != ConfigOptionDef::GUIType::undefined && m_opt.gui_type != ConfigOptionDef::GUIType::select_open) {
        m_is_editable = true;
        temp = new choice_ctrl(m_parent, wxID_ANY, wxString(""), wxDefaultPosition, size, 0, nullptr, wxTE_PROCESS_ENTER);
    }
    else {
#ifdef __WXOSX__
        /* wxBitmapComboBox with wxCB_READONLY style return NULL for GetTextCtrl(),
         * so ToolTip doesn't shown.
         * Next workaround helps to solve this problem
         */
        temp = new choice_ctrl();
        temp->SetTextCtrlStyle(wxTE_READONLY);
        temp->Create(m_parent, wxID_ANY, wxString(""), wxDefaultPosition, size, 0, nullptr);
#else
        temp = new choice_ctrl(m_parent, wxID_ANY, wxString(""), wxDefaultPosition, size, 0, nullptr, wxCB_READONLY);
#endif //__WXOSX__
    }

#ifdef __WXGTK3__
    wxSize best_sz = temp->GetBestSize();
    if (best_sz.x > size.x)
        temp->SetSize(best_sz);
#endif //__WXGTK3__

	temp->SetFont(Slic3r::GUI::wxGetApp().normal_font());
    if (!wxOSX) temp->SetBackgroundStyle(wxBG_STYLE_PAINT);

	// recast as a wxWindow to fit the calling convention
	window = dynamic_cast<wxWindow*>(temp);

	if (! m_opt.enum_labels.empty() || ! m_opt.enum_values.empty()) {
		if (m_opt.enum_labels.empty()) {
			// Append non-localized enum_values
			for (auto el : m_opt.enum_values)
				temp->Append(el);
		} else {
			// Append localized enum_labels
			for (auto el : m_opt.enum_labels)
				temp->Append(_(el));
		}
		set_selection();
	}

    temp->Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& e) {
        if (m_suppress_scroll && !m_is_dropped)
            e.StopPropagation();
        else
            e.Skip();
        });
    temp->Bind(wxEVT_COMBOBOX_DROPDOWN, [this](wxCommandEvent&) { m_is_dropped = true; });
    temp->Bind(wxEVT_COMBOBOX_CLOSEUP,  [this](wxCommandEvent&) { m_is_dropped = false; });

    temp->Bind(wxEVT_COMBOBOX,          [this](wxCommandEvent&) {
        //note: on_change_field() is never really called because m_disable_change_event is always true.
        // it should be fixed in a better way, but as modifying how m_disable_change_event is set will need exentive testing
        // on all platform, I add this stop-gap. If you can remove it and let the splash_screen_editor field working, do it!
        if (m_disable_change_event) {
            m_disable_change_event = false;
            on_change_field();
            m_disable_change_event = true;
        }else
            on_change_field(); 
    }, temp->GetId());

    if (m_is_editable) {
        temp->Bind(wxEVT_KILL_FOCUS, [this](wxEvent& e) {
            e.Skip();
            if (!bEnterPressed)
                propagate_value();
        } );

        temp->Bind(wxEVT_TEXT_ENTER, [this](wxEvent& e) {
            EnterPressed enter(this);
            propagate_value();
        } );
    }

    this->set_tooltip(temp->GetValue());
}

void Choice::propagate_value()
{
    if (m_opt.type == coStrings) {
        on_change_field();
        return;
    }

    if (is_defined_input_value<choice_ctrl>(window, m_opt.type)) {
        switch (m_opt.type) {
        case coFloatOrPercent:
        {
            std::string old_val = !m_value.empty() ? boost::any_cast<std::string>(m_value) : "";
            if (old_val == boost::any_cast<std::string>(get_value()))
                return;
            break;
        }
        case coInt:
        {
            int old_val = !m_value.empty() ? boost::any_cast<int>(m_value) : 0;
            if (old_val == boost::any_cast<int>(get_value()))
                return;
            break;
        }
        default:
        {
            double old_val = !m_value.empty() ? boost::any_cast<double>(m_value) : -99999;
            if (fabs(old_val - boost::any_cast<double>(get_value())) <= 0.0001)
                return;
        }
        }
        on_change_field();
    }
    else
        on_kill_focus();
}

void Choice::suppress_scroll()
{
    m_suppress_scroll = true;
}

void Choice::set_selection()
{
    /* To prevent earlier control updating under OSX set m_disable_change_event to true
     * (under OSX wxBitmapComboBox send wxEVT_COMBOBOX even after SetSelection())
     */
    m_disable_change_event = true;

	wxString text_value = wxString("");

    choice_ctrl* field = dynamic_cast<choice_ctrl*>(window);
	switch (m_opt.type) {
	case coEnum:{
        field->SetSelection(m_opt.default_value->getInt());
		break;
	}
	case coFloat:
	case coPercent:	{
		double val = m_opt.default_value->getFloat();
		text_value = val - int(val) == 0 ? wxString::Format(_T("%i"), int(val)) : wxNumberFormatter::ToString(val, 1);
		break;
	}
	case coInt:{
		text_value = wxString::Format(_T("%i"), int(m_opt.default_value->getInt()));
		break;
	}
	case coStrings:{
		text_value = m_opt.get_default_value<ConfigOptionStrings>()->get_at(m_opt_idx);
		break;
	}
	case coFloatOrPercent: {
		text_value = double_to_string(m_opt.default_value->getFloat(), m_opt.precision);
		if (m_opt.get_default_value<ConfigOptionFloatOrPercent>()->percent)
			text_value += "%";
		break;
	}
    default: break;
	}

	if (!text_value.IsEmpty()) {
        size_t idx = 0;
		for (auto el : m_opt.enum_values) {
			if (el == text_value)
				break;
			++idx;
		}
		idx == m_opt.enum_values.size() ? field->SetValue(text_value) : field->SetSelection(idx);
	}
}

void Choice::set_value(const std::string& value, bool change_event)  //! Redundant?
{
	m_disable_change_event = !change_event;

	size_t idx=0;
	for (auto el : m_opt.enum_values)
	{
		if (el == value)
			break;
		++idx;
	}

    choice_ctrl* field = dynamic_cast<choice_ctrl*>(window);
	idx == m_opt.enum_values.size() ?
		field->SetValue(value) :
		field->SetSelection(idx);

	m_disable_change_event = false;
}

int32_t Choice::idx_from_enum_value(int32_t val) {
    if (!m_opt.enum_values.empty()) {
        std::string key;
        const t_config_enum_values* map_names = m_opt.enum_keys_map;
        for (auto it : *map_names) {
            if (val == it.second) {
                key = it.first;
                break;
            }
        }

        size_t idx = 0;
        for (auto el : m_opt.enum_values)
        {
            if (el.compare(key) == 0)
                break;
            ++idx;
        }

        return idx == m_opt.enum_values.size() ? 0 : idx;
    }
    else
        return 0;
}

void Choice::set_value(const boost::any& value, bool change_event)
{
	m_disable_change_event = !change_event;

    choice_ctrl* field = dynamic_cast<choice_ctrl*>(window);

	switch (m_opt.type) {
	case coInt:
	case coFloat:
	case coPercent:
	case coFloatOrPercent:
	case coString:
	case coStrings: {
		wxString text_value;
		if (m_opt.type == coInt)
			text_value = wxString::Format(_T("%i"), int(boost::any_cast<int>(value)));
		else
			text_value = boost::any_cast<wxString>(value);
        size_t idx = 0;
        const std::vector<std::string>& enums = m_opt.enum_values.empty() ? m_opt.enum_labels : m_opt.enum_values;
		for (auto el : enums)
		{
			if (el == text_value)
				break;
			++idx;
		}
        if (idx == enums.size()) {
            // For editable Combobox under OSX is needed to set selection to -1 explicitly,
            // otherwise selection doesn't be changed
            field->SetSelection(-1);
            field->SetValue(text_value);
        }
        else
			field->SetSelection(idx);

        if (!m_value.empty() && m_opt.opt_key == "fill_density") {
            // If m_value was changed before, then update m_value here too to avoid case 
            // when control's value is already changed from the ConfigManipulation::update_print_fff_config(),
            // but m_value doesn't respect it.
            if (double val; text_value.ToDouble(&val))
                m_value = val;
        }

		break;
	}
	case coEnum: {
        int32_t val = boost::any_cast<int32_t>(value);
        val = idx_from_enum_value(val);
		BOOST_LOG_TRIVIAL(debug) << "Set field from key "<< m_opt_id << " as int " << boost::any_cast<int>(value) << " modified to " << val;
		field->SetSelection(val);
		break;
	}
	default:
		break;
	}

	m_disable_change_event = false;
}

//! it's needed for _update_serial_ports()
//Please don't use that on Enum fields it will just break everything
void Choice::set_values(const std::vector<std::string>& values)
{
    assert(m_opt.type != coEnum);
	if (values.empty())
		return;
	m_disable_change_event = true;

// 	# it looks that Clear() also clears the text field in recent wxWidgets versions,
// 	# but we want to preserve it
	auto ww = dynamic_cast<choice_ctrl*>(window);
	auto value = ww->GetValue();
	ww->Clear();
	ww->Append("");
	for (const auto &el : values)
		ww->Append(wxString(el));
	ww->SetValue(value);

	m_disable_change_event = false;
}

void Choice::convert_to_enum_value(int32_t ret_enum) {
    if (!m_opt.enum_values.empty()) {
        std::string key = m_opt.enum_values[ret_enum];
        const t_config_enum_values *map_names = m_opt.enum_keys_map;
        int32_t value = map_names->at(key);

        m_value = value;
    }
    else
        m_value = m_opt.default_value.get()->getInt();
}

//Please don't use that on Enum fields it will just break everything
void Choice::set_values(const wxArrayString &values)
{
    assert(m_opt.type != coEnum);
	if (values.empty())
		return;

	m_disable_change_event = true;

	// 	# it looks that Clear() also clears the text field in recent wxWidgets versions,
	// 	# but we want to preserve it
	auto ww = dynamic_cast<choice_ctrl*>(window);
	auto value = ww->GetValue();
	ww->Clear();
//	ww->Append("");
	for (const auto &el : values)
		ww->Append(el);
	ww->SetValue(value);

	m_disable_change_event = false;
}

boost::any& Choice::get_value()
{
    choice_ctrl* field = dynamic_cast<choice_ctrl*>(window);

	wxString ret_str = field->GetValue();

	// options from right panel
	std::vector <std::string> right_panel_options{ "support", "pad", "scale_unit" };
	for (auto rp_option: right_panel_options)
		if (m_opt_id == rp_option)
			return m_value = boost::any(ret_str);

	if (m_opt.type == coEnum)
    {
        int ret_enum = field->GetSelection();
        convert_to_enum_value(ret_enum);
    }
    else if (m_opt.gui_type == ConfigOptionDef::GUIType::f_enum_open || m_opt.gui_type == ConfigOptionDef::GUIType::i_enum_open) {
        const int ret_enum = field->GetSelection();
        if (!m_opt.enum_values.empty() && (m_opt.type == coString || m_opt.type == coStrings) && ret_enum >=0 && ret_enum < m_opt.enum_values.size()) {
            m_value = m_opt.enum_values[ret_enum];
        } else if (ret_enum < 0 || m_opt.enum_values.empty() || m_opt.type == coStrings ||
            (ret_str != m_opt.enum_values[ret_enum] && ret_str != _(m_opt.enum_labels[ret_enum])))
			// modifies ret_string!
            get_value_by_opt_type(ret_str);
        else if (m_opt.type == coFloatOrPercent)
            m_value = m_opt.enum_values[ret_enum];
        else if (m_opt.type == coInt)
            m_value = atoi(m_opt.enum_values[ret_enum].c_str());
        else
            m_value = string_to_double_decimal_point(m_opt.enum_values[ret_enum]);
    }
	else
		// modifies ret_string!
        get_value_by_opt_type(ret_str);

	return m_value;
}

void Choice::enable()  { dynamic_cast<choice_ctrl*>(window)->Enable(); }
void Choice::disable() { dynamic_cast<choice_ctrl*>(window)->Disable(); }

void Choice::msw_rescale()
{
    Field::msw_rescale();

    choice_ctrl* field = dynamic_cast<choice_ctrl*>(window);
#ifdef __WXOSX__
    const wxString selection = field->GetValue();// field->GetString(index);

	/* To correct scaling (set new controll size) of a wxBitmapCombobox
	 * we need to refill control with new bitmaps. So, in our case :
	 * 1. clear control
	 * 2. add content
	 * 3. add scaled "empty" bitmap to the at least one item
	 */
    field->Clear();
    wxSize size(wxDefaultSize);
    size.SetWidth((m_opt.width > 0 ? m_opt.width : def_width_wider()) * m_em_unit);

    // Set rescaled min height to correct layout
    field->SetMinSize(wxSize(-1, int(1.5f*field->GetFont().GetPixelSize().y + 0.5f)));
    // Set rescaled size
    field->SetSize(size);

    size_t idx = 0;
    if (! m_opt.enum_labels.empty() || ! m_opt.enum_values.empty()) {
    	size_t counter = 0;
    	bool   labels = ! m_opt.enum_labels.empty();
        for (const std::string &el : labels ? m_opt.enum_labels : m_opt.enum_values) {
        	wxString text = labels ? _(el) : from_u8(el);
            field->Append(text);
            if (text == selection)
                idx = counter;
            ++ counter;
        }
    }

    idx == m_opt.enum_values.size() ?
        field->SetValue(selection) :
        field->SetSelection(idx);
#else
#ifdef _WIN32
    field->Rescale();
#endif
    auto size = wxSize(def_width_wider() * m_em_unit, wxDefaultCoord);
    if (m_opt.height >= 0) size.SetHeight(m_opt.height * m_em_unit);
    if (m_opt.width >= 0) size.SetWidth(m_opt.width * m_em_unit);

    if (parent_is_custom_ctrl)
        field->SetSize(size);
    else
        field->SetMinSize(size);
#endif
}

void ColourPicker::BUILD()
{
	auto size = wxSize(def_width() * m_em_unit, wxDefaultCoord);
    if (m_opt.height >= 0) size.SetHeight(m_opt.height*m_em_unit);
    if (m_opt.width >= 0) size.SetWidth(m_opt.width*m_em_unit);

	// Validate the color
    wxColour clr = wxTransparentColour;
    if (m_opt.type == coStrings)
        clr = wxColour{wxString{ m_opt.get_default_value<ConfigOptionStrings>()->get_at(m_opt_idx) }};
    if (m_opt.type == coString)
        clr = wxColour{ wxString{ m_opt.get_default_value<ConfigOptionString>()->value } };
    if (m_opt.type == coInts)
        clr = wxColour{ (unsigned long)m_opt.get_default_value<ConfigOptionInts>()->get_at(m_opt_idx) };
    if (m_opt.type == coInt)
        clr = wxColour{ (unsigned long)m_opt.get_default_value<ConfigOptionInt>()->value };
	if (!clr.IsOk()) {
		clr = wxTransparentColour;
	}

	auto temp = new wxColourPickerCtrl(m_parent, wxID_ANY, clr, wxDefaultPosition, size);
    if (parent_is_custom_ctrl && m_opt.height < 0)
        opt_height = (double)temp->GetSize().GetHeight() / m_em_unit;
    temp->SetFont(Slic3r::GUI::wxGetApp().normal_font());
    if (!wxOSX) temp->SetBackgroundStyle(wxBG_STYLE_PAINT);

	wxGetApp().UpdateDarkUI(temp->GetPickerCtrl());

	// 	// recast as a wxWindow to fit the calling convention
	window = dynamic_cast<wxWindow*>(temp);

    window->Bind(wxEVT_COLOURPICKER_CHANGED, ([this](wxCommandEvent e) { on_change_field(); }), window->GetId());

    this->set_tooltip(clr.GetAsString());
}

void ColourPicker::set_undef_value(wxColourPickerCtrl* field)
{
    field->SetColour(wxTransparentColour);

    wxButton* btn = dynamic_cast<wxButton*>(field->GetPickerCtrl());
    wxBitmap bmp = btn->GetBitmap();
    wxMemoryDC dc(bmp);
    if (!dc.IsOk()) return;
    dc.SetTextForeground(*wxWHITE);
    dc.SetFont(wxGetApp().normal_font());

    const wxRect rect = wxRect(0, 0, bmp.GetWidth(), bmp.GetHeight());
    dc.DrawLabel("undef", rect, wxALIGN_CENTER_HORIZONTAL|wxALIGN_CENTER_VERTICAL);

    dc.SelectObject(wxNullBitmap);
    btn->SetBitmapLabel(bmp);
}

void ColourPicker::set_value(const boost::any& value, bool change_event)
{
    m_disable_change_event = !change_event;
    const wxString clr_str(boost::any_cast<wxString>(value));
    auto field = dynamic_cast<wxColourPickerCtrl*>(window);

    wxColour clr(clr_str);
    if (clr_str.IsEmpty() || !clr.IsOk())
        set_undef_value(field);
    else
        field->SetColour(clr);

    m_disable_change_event = false;
}

boost::any& ColourPicker::get_value()
{
	auto colour = static_cast<wxColourPickerCtrl*>(window)->GetColour();
    if (colour == wxTransparentColour)
        m_value = std::string("");
    else {
		auto clr_str = wxString::Format(wxT("#%02X%02X%02X"), colour.Red(), colour.Green(), colour.Blue());
		m_value = clr_str.ToStdString();
    }
	return m_value;
}

void ColourPicker::msw_rescale()
{
    Field::msw_rescale();

	wxColourPickerCtrl* field = dynamic_cast<wxColourPickerCtrl*>(window);
    auto size = wxSize(def_width() * m_em_unit, wxDefaultCoord);
    if (m_opt.height >= 0)
        size.SetHeight(m_opt.height * m_em_unit);
    else if (parent_is_custom_ctrl && opt_height > 0)
        size.SetHeight(lround(opt_height * m_em_unit));
    if (m_opt.width >= 0) size.SetWidth(m_opt.width * m_em_unit);
    if (parent_is_custom_ctrl)
        field->SetSize(size);
    else
        field->SetMinSize(size);

    if (field->GetColour() == wxTransparentColour)
        set_undef_value(field);
}

void ColourPicker::sys_color_changed()
{
#ifdef _WIN32
	if (wxWindow* win = this->getWindow())
		if (wxColourPickerCtrl* picker = dynamic_cast<wxColourPickerCtrl*>(win))
			wxGetApp().UpdateDarkUI(picker->GetPickerCtrl(), true);
#endif
}

void PointCtrl::BUILD()
{
	auto temp = new wxBoxSizer(wxHORIZONTAL);

    const wxSize field_size(4 * m_em_unit, -1);

    Vec2d default_pt;
    if(m_opt.type==coPoint)
        default_pt = m_opt.get_default_value<ConfigOptionPoint>()->value;
    else // coPoints
        default_pt = m_opt.get_default_value<ConfigOptionPoints>()->values.at(0);
	double val = default_pt(0);
	wxString X = val - int(val) == 0 ? wxString::Format(_T("%i"), int(val)) : wxNumberFormatter::ToString(val, 2, wxNumberFormatter::Style_None);
	val = default_pt(1);
	wxString Y = val - int(val) == 0 ? wxString::Format(_T("%i"), int(val)) : wxNumberFormatter::ToString(val, 2, wxNumberFormatter::Style_None);

	long style = wxTE_PROCESS_ENTER;
#ifdef _WIN32
	style |= wxBORDER_SIMPLE;
#endif
	x_textctrl = new wxTextCtrl(m_parent, wxID_ANY, X, wxDefaultPosition, field_size, style);
	y_textctrl = new wxTextCtrl(m_parent, wxID_ANY, Y, wxDefaultPosition, field_size, style);
    if (parent_is_custom_ctrl && m_opt.height < 0)
        opt_height = (double)x_textctrl->GetSize().GetHeight() / m_em_unit;

    x_textctrl->SetFont(Slic3r::GUI::wxGetApp().normal_font());
	x_textctrl->SetBackgroundStyle(wxBG_STYLE_PAINT);
	y_textctrl->SetFont(Slic3r::GUI::wxGetApp().normal_font());
	y_textctrl->SetBackgroundStyle(wxBG_STYLE_PAINT);

	auto static_text_x = new wxStaticText(m_parent, wxID_ANY, "x : ");
	auto static_text_y = new wxStaticText(m_parent, wxID_ANY, "   y : ");
	static_text_x->SetFont(Slic3r::GUI::wxGetApp().normal_font());
	static_text_x->SetBackgroundStyle(wxBG_STYLE_PAINT);
	static_text_y->SetFont(Slic3r::GUI::wxGetApp().normal_font());
	static_text_y->SetBackgroundStyle(wxBG_STYLE_PAINT);

	wxGetApp().UpdateDarkUI(x_textctrl);
	wxGetApp().UpdateDarkUI(y_textctrl);
	wxGetApp().UpdateDarkUI(static_text_x, false, true);
	wxGetApp().UpdateDarkUI(static_text_y, false, true);

	temp->Add(static_text_x, 0, wxALIGN_CENTER_VERTICAL, 0);
	temp->Add(x_textctrl);
	temp->Add(static_text_y, 0, wxALIGN_CENTER_VERTICAL, 0);
	temp->Add(y_textctrl);

    x_textctrl->Bind(wxEVT_TEXT_ENTER, ([this](wxCommandEvent e) { propagate_value(x_textctrl); }), x_textctrl->GetId());
	y_textctrl->Bind(wxEVT_TEXT_ENTER, ([this](wxCommandEvent e) { propagate_value(y_textctrl); }), y_textctrl->GetId());

    x_textctrl->Bind(wxEVT_KILL_FOCUS, ([this](wxEvent& e) { e.Skip(); propagate_value(x_textctrl); }), x_textctrl->GetId());
    y_textctrl->Bind(wxEVT_KILL_FOCUS, ([this](wxEvent& e) { e.Skip(); propagate_value(y_textctrl); }), y_textctrl->GetId());

	// 	// recast as a wxWindow to fit the calling convention
	sizer = dynamic_cast<wxSizer*>(temp);

    this->set_tooltip(X + ", " + Y, x_textctrl);
    this->set_tooltip(X + ", " + Y, y_textctrl);
}

void PointCtrl::msw_rescale()
{
    Field::msw_rescale();

    wxSize field_size(4 * m_em_unit, -1);

    if (parent_is_custom_ctrl) {
        field_size.SetHeight(lround(opt_height * m_em_unit));
        x_textctrl->SetSize(field_size);
        y_textctrl->SetSize(field_size);
    }
    else {
        x_textctrl->SetMinSize(field_size);
        y_textctrl->SetMinSize(field_size);
    }
}

void PointCtrl::sys_color_changed()
{
#ifdef _WIN32
    for (wxSizerItem* item: sizer->GetChildren())
        if (item->IsWindow())
            wxGetApp().UpdateDarkUI(item->GetWindow());
#endif
}

bool PointCtrl::value_was_changed(wxTextCtrl* win)
{
	if (m_value.empty())
		return true;

	boost::any val = m_value;
	// update m_value!
	get_value();

	return boost::any_cast<Vec2d>(m_value) != boost::any_cast<Vec2d>(val);
}

void PointCtrl::propagate_value(wxTextCtrl* win)
{
    if (win->GetValue().empty())
        on_kill_focus();
	else if (value_was_changed(win))
        on_change_field();
}

void PointCtrl::set_value(const Vec2d& value, bool change_event)
{
	m_disable_change_event = !change_event;

	double val = value(0);
	x_textctrl->SetValue(val - int(val) == 0 ? wxString::Format(_T("%i"), int(val)) : wxNumberFormatter::ToString(val, 2, wxNumberFormatter::Style_None));
	val = value(1);
	y_textctrl->SetValue(val - int(val) == 0 ? wxString::Format(_T("%i"), int(val)) : wxNumberFormatter::ToString(val, 2, wxNumberFormatter::Style_None));

	m_disable_change_event = false;
}

void PointCtrl::set_value(const boost::any& value, bool change_event)
{
	Vec2d pt(Vec2d::Zero());
	const Vec2d *ptf = boost::any_cast<Vec2d>(&value);
	if (!ptf)
	{
        if (m_opt.type == coPoint) {
            pt = boost::any_cast<ConfigOptionPoint*>(value)->value;
        } else { // coPoints
            ConfigOptionPoints* pts = boost::any_cast<ConfigOptionPoints*>(value);
            pt = pts->values.at(0);
        }
	}
	else
		pt = *ptf;
	set_value(pt, change_event);
}

boost::any& PointCtrl::get_value()
{
	double x, y;
	if (!x_textctrl->GetValue().ToDouble(&x) ||
		!y_textctrl->GetValue().ToDouble(&y))
	{
		set_value(m_value.empty() ? Vec2d(0.0, 0.0) : m_value, true);
        show_error(m_parent, _L("Invalid numeric input."));
	}
	else
	if (m_opt.min > x || x > m_opt.max ||
		m_opt.min > y || y > m_opt.max)
	{
		if (m_opt.min > x) x = m_opt.min;
		if (x > m_opt.max) x = m_opt.max;
		if (m_opt.min > y) y = m_opt.min;
		if (y > m_opt.max) y = m_opt.max;
		set_value(Vec2d(x, y), true);

		show_error(m_parent, _L("Input value is out of range"));
	}

	return m_value = Vec2d(x, y);
}

void StaticText::BUILD()
{
	auto size = wxSize(wxDefaultSize);
    if (m_opt.height >= 0) size.SetHeight(m_opt.height*m_em_unit);
    if (m_opt.width >= 0) size.SetWidth(m_opt.width*m_em_unit);

    const wxString legend = from_u8(m_opt.get_default_value<ConfigOptionString>()->value);
    auto temp = new wxStaticText(m_parent, wxID_ANY, legend, wxDefaultPosition, size, wxST_ELLIPSIZE_MIDDLE);
	temp->SetFont(Slic3r::GUI::wxGetApp().normal_font());
	temp->SetBackgroundStyle(wxBG_STYLE_PAINT);
    temp->SetFont(wxGetApp().bold_font());

	wxGetApp().UpdateDarkUI(temp);

	// 	// recast as a wxWindow to fit the calling convention
	window = dynamic_cast<wxWindow*>(temp);

    this->set_tooltip(legend);
}

void StaticText::msw_rescale()
{
    Field::msw_rescale();

    auto size = wxSize(wxDefaultSize);
    if (m_opt.height >= 0) size.SetHeight(m_opt.height*m_em_unit);
    if (m_opt.width >= 0) size.SetWidth(m_opt.width*m_em_unit);

    if (size != wxDefaultSize)
    {
        wxStaticText* field = dynamic_cast<wxStaticText*>(window);
        field->SetSize(size);
        field->SetMinSize(size);
    }
}

void SliderCtrl::BUILD()
{
	auto size = wxSize(wxDefaultSize);
	if (m_opt.height >= 0) size.SetHeight(m_opt.height);
	if (m_opt.width >= 0) size.SetWidth(m_opt.width);

	auto temp = new wxBoxSizer(wxHORIZONTAL);

	int def_val = m_opt.get_default_value<ConfigOptionInt>()->value;
    int min = m_opt.min == INT_MIN ? 0 : int(m_opt.min);
    int max = m_opt.max == INT_MAX ? 100 : int(m_opt.max);

	m_slider = new wxSlider(m_parent, wxID_ANY, def_val * m_scale,
							min * m_scale, max * m_scale,
							wxDefaultPosition, size);
	m_slider->SetFont(Slic3r::GUI::wxGetApp().normal_font());
	m_slider->SetBackgroundStyle(wxBG_STYLE_PAINT);
 	wxSize field_size(40, -1);

	m_textctrl = new wxTextCtrl(m_parent, wxID_ANY, wxString::Format("%d", m_slider->GetValue()/m_scale),
								wxDefaultPosition, field_size);
	m_textctrl->SetFont(Slic3r::GUI::wxGetApp().normal_font());
	m_textctrl->SetBackgroundStyle(wxBG_STYLE_PAINT);

	temp->Add(m_slider, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL, 0);
	temp->Add(m_textctrl, 0, wxALIGN_CENTER_VERTICAL, 0);

	m_slider->Bind(wxEVT_SLIDER, ([this](wxCommandEvent e) {
		if (!m_disable_change_event) {
			int val = boost::any_cast<int>(get_value());
			m_textctrl->SetLabel(wxString::Format("%d", val));
			on_change_field();
		}
	}), m_slider->GetId());

	m_textctrl->Bind(wxEVT_TEXT, ([this](wxCommandEvent e) {
		std::string value = e.GetString().utf8_str().data();
		if (is_matched(value, "^-?\\d+(\\.\\d*)?$")) {
			m_disable_change_event = true;
			m_slider->SetValue(stoi(value)*m_scale);
			m_disable_change_event = false;
			on_change_field();
		}
	}), m_textctrl->GetId());

	m_sizer = dynamic_cast<wxSizer*>(temp);
}

void SliderCtrl::set_value(const boost::any& value, bool change_event)
{
	m_disable_change_event = !change_event;

	m_slider->SetValue(boost::any_cast<int>(value)*m_scale);
	int val = boost::any_cast<int>(get_value());
	m_textctrl->SetLabel(wxString::Format("%d", val));

	m_disable_change_event = false;
}

boost::any& SliderCtrl::get_value()
{
// 	int ret_val;
// 	x_textctrl->GetValue().ToDouble(&val);
	return m_value = int(m_slider->GetValue()/m_scale);
}


} // GUI
} // Slic3r
