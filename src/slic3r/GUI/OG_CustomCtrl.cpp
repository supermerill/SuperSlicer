#include "OG_CustomCtrl.hpp"
#include "OptionsGroup.hpp"
#include "Plater.hpp"
#include "GUI_App.hpp"
#include "libslic3r/AppConfig.hpp"

#include <wx/utils.h>
#include <boost/algorithm/string/split.hpp>
#include "libslic3r/Utils.hpp"
#include "I18N.hpp"
#include "format.hpp"

namespace Slic3r { namespace GUI {


static bool is_point_in_rect(const wxPoint& pt, const wxRect& rect)
{
    return  rect.GetLeft() <= pt.x && pt.x <= rect.GetRight() &&
            rect.GetTop() <= pt.y && pt.y <= rect.GetBottom();
}

static wxSize get_bitmap_size(const wxBitmap& bmp)
{
#ifdef __APPLE__
    return bmp.GetScaledSize();
#else
    return bmp.GetSize();
#endif
}

// static wxString get_url(const wxString& path_end, bool get_default = false) 
// {
//     if (path_end.IsEmpty())
//         return wxEmptyString;

//     wxString language = wxGetApp().app_config->get("translation_language");
//     wxString lang_marker = language.IsEmpty() ? "en" : language.BeforeFirst('_');

//     return wxString("https://help.prusa3d.com/") + lang_marker + "/article/" + path_end;
// }

int OG_CustomCtrl::m_has_icon = (-1);

OG_CustomCtrl::OG_CustomCtrl(   wxWindow*            parent,
                                OptionsGroup*        og,
                                const wxPoint&       pos /* = wxDefaultPosition*/,
                                const wxSize&        size/* = wxDefaultSize*/,
                                const wxValidator&   val /* = wxDefaultValidator*/,
                                const wxString&      name/* = wxEmptyString*/) :
    wxPanel(parent, wxID_ANY, pos, size, /*wxWANTS_CHARS |*/ wxBORDER_NONE | wxTAB_TRAVERSAL),
    opt_group(og)
{
    if (m_has_icon < 0)
        m_has_icon = get_app_config()->get("setting_icon") == "1";
    if (!wxOSX)
        SetDoubleBuffered(true);// SetDoubleBuffered exists on Win and Linux/GTK, but is missing on OSX

    m_font      = wxGetApp().normal_font();
    m_em_unit   = em_unit(m_parent);
    m_v_gap     = lround(1.0 * m_em_unit);
    m_h_gap     = lround(0.2 * m_em_unit);

    m_bmp_mode_sz       = (!m_has_icon) ? wxSize(0,0) : get_bitmap_size(create_scaled_bitmap("mode_simple", this, wxOSX ? 10 : 12));
    m_bmp_blinking_sz   = get_bitmap_size(create_scaled_bitmap("search_blink", this));
    if (!m_has_icon) m_bmp_blinking_sz.x = 0;

    init_ctrl_lines();// from og.lines()

    this->Bind(wxEVT_PAINT,     &OG_CustomCtrl::OnPaint, this);
    this->Bind(wxEVT_MOTION,    &OG_CustomCtrl::OnMotion, this);
    this->Bind(wxEVT_LEFT_DOWN, &OG_CustomCtrl::OnLeftDown, this);
    this->Bind(wxEVT_LEAVE_WINDOW, &OG_CustomCtrl::OnLeaveWin, this);
}

void OG_CustomCtrl::init_ctrl_lines()
{
    const std::vector<Line>& og_lines = opt_group->get_lines();
    for (const Line& line : og_lines)
    {
        if (line.full_width && (
            // description line
            line.widget != nullptr ||
            // description line with widget (button)
            !line.get_extra_widgets().empty())
            )
            continue;

        const std::vector<Option>& option_set = line.get_options();
        wxCoord height;

        // if we have a single option with no label, no sidetext just add it directly to sizer
        if (option_set.size() == 1 && opt_group->title_width == 0 && option_set.front().opt.full_width &&
            option_set.front().opt.sidetext.size() == 0 && option_set.front().side_widget == nullptr &&
            line.get_extra_widgets().size() == 0)
        {
            height = m_bmp_blinking_sz.GetHeight() + m_v_gap;
            ctrl_lines.emplace_back(CtrlLine(height, this, line, true));
        }
        else if (opt_group->title_width != 0 && (!line.label.IsEmpty() || option_set.front().opt.gui_type == ConfigOptionDef::GUIType::legend) )
        {
            wxSize label_sz = GetTextExtent(line.label);
            height = label_sz.y * (label_sz.GetWidth() > int(opt_group->title_width * m_em_unit) ? 2 : 1) + m_v_gap;
            ctrl_lines.emplace_back(CtrlLine(height, this, line, false, opt_group->staticbox));
        }
        else
        {
            height = m_bmp_blinking_sz.GetHeight() + m_v_gap;
            ctrl_lines.emplace_back(CtrlLine(height, this, line, opt_group->no_title, opt_group->staticbox));
        }
    }
}

int OG_CustomCtrl::get_height(const Line& line)
{
    for (auto ctrl_line : ctrl_lines)
        if (&ctrl_line.og_line == &line)
            return ctrl_line.height;
        
    return 0;
}

wxPoint OG_CustomCtrl::get_pos(const Line& line, Field* field_in/* = nullptr*/)
{
    wxCoord v_pos = 0;
    wxCoord h_pos = 0;

    auto correct_line_height = [](int& line_height, wxWindow* win)
    {
        int win_height = win->GetSize().GetHeight();
        if (line_height < win_height)
            line_height = win_height;
    };

    // auto correct_horiz_pos = [this](int& h_pos, Field* field) {
    //     if (m_max_win_width > 0 && field->getWindow()) {
    //         int win_width = field->getWindow()->GetSize().GetWidth();
    //         if (dynamic_cast<CheckBox*>(field))
    //             win_width *= 0.5;
    //         h_pos += m_max_win_width - win_width;
    //     }
    // };

    for (CtrlLine& ctrl_line : ctrl_lines) {
        if (&ctrl_line.og_line == &line)
        {
            h_pos = m_bmp_mode_sz.GetWidth();
            if (h_pos) h_pos += m_h_gap;
            if (line.near_label_widget_win) {
                wxSize near_label_widget_sz = line.near_label_widget_win->GetSize();
                if (field_in)
                    h_pos += near_label_widget_sz.GetWidth() + m_h_gap;
                else
                    break;
            }

            //round it to next m_em_unit
            h_pos += (h_pos % m_em_unit == 0) ? 0 : m_em_unit - (h_pos % m_em_unit);

            wxString label = line.label;
            if (opt_group->title_width != 0)
                h_pos += opt_group->title_width * m_em_unit + m_h_gap;

            int blinking_button_width = m_bmp_blinking_sz.GetWidth();
            if (blinking_button_width) blinking_button_width += m_h_gap;

            if (line.widget) {
                h_pos += blinking_button_width;

                for (auto child : line.widget_sizer->GetChildren())
                    if (child->IsWindow())
                        correct_line_height(ctrl_line.height, child->GetWindow());
                break;
            }

            //round it to next m_em_unit
            h_pos += (h_pos % m_em_unit == 0) ? 0 : m_em_unit - (h_pos % m_em_unit);

            // If we have a single option with no sidetext
            const std::vector<Option>& option_set = line.get_options();
            if (option_set.size() == 1 && option_set.front().opt.sidetext.size() == 0 &&
                option_set.front().side_widget == nullptr && line.get_extra_widgets().size() == 0)
            {
                h_pos += 2 * blinking_button_width;
                Field* field = opt_group->get_field(option_set.front().opt_id);
                correct_line_height(ctrl_line.height, field->getWindow());
                //correct_horiz_pos(h_pos, field); //TODO test
                break;
            }

            bool is_multioption_line = option_set.size() > 1 || !option_set.front().opt.label.empty();
            for (size_t i = 0; i < option_set.size(); ++i) {
                if (i >= ctrl_line.is_visible.size() || !ctrl_line.is_visible[i])
                    continue;
                const Option& opt = option_set[i];
                Field* field = opt_group->get_field(opt.opt_id);
                correct_line_height(ctrl_line.height, field->getWindow());

                ConfigOptionDef option = opt.opt;
                // add label if any
                if (is_multioption_line && !option.label.empty()) {
                    std::string opt_label = (option.label.empty() || option.label.back() != '_') ? option.label : option.label.substr(0, option.label.size() - 1);
                    //!            To correct translation by context have to use wxGETTEXT_IN_CONTEXT macro from wxWidget 3.1.1
                    label = /*(opt_label == L_CONTEXT("Top", "Layers") || opt_label == L_CONTEXT("Bottom", "Layers")) ?
                        _CTX(opt_label, "Layers") :*/ _(opt_label);
                    bool no_dots = label.empty() || option.label.back() == '_';
                    if (!no_dots)
                        label += ":";

                    if (!label.empty() || option.label_width > 0) {
                        wxCoord label_w, label_h;
#ifdef __WXMSW__
                        // when we use 2 monitors with different DPIs, GetTextExtent() return value for the primary display
                        // so, use dc.GetMultiLineTextExtent on Windows 
                    wxClientDC dc(this);
                        dc.SetFont(m_font);
                        if (option.label_width >= 0) {
                            if (option.label_width != 0) {
                                h_pos += option.label_width * m_em_unit;
                            } else {
                                dc.GetMultiLineTextExtent(label, &label_w, &label_h);
                                h_pos += label_w;
                            }
                        } else {
                            if (opt_group->label_width > 0) {
                                h_pos += opt_group->label_width * m_em_unit;
                            } else {
                                dc.GetMultiLineTextExtent(label, &label_w, &label_h);
                                h_pos += label_w;
                            }
                        }
#else
                        if (option.label_width >= 0) {
                            if (option.label_width != 0) {
                                h_pos += option.label_width * m_em_unit;
                            } else {
                                GetTextExtent(label, &label_w, &label_h, 0, 0, &m_font);
                                h_pos += label_w;
                            }
                        } else {
                            if (opt_group->label_width > 0) {
                                h_pos += opt_group->label_width * m_em_unit;
                            } else {
                                GetTextExtent(label, &label_w, &label_h, 0, 0, &m_font);
                                h_pos += label_w;
                            }
                        }
#endif //__WXMSW__
                        h_pos += m_h_gap;
                    }
                }

                //round it to next m_em_unit
                h_pos += (h_pos % m_em_unit == 0) ? 0 : m_em_unit - (h_pos % m_em_unit);

                // size of little widget before the real one
                h_pos += 2 * blinking_button_width;
                
                if (field == field_in) {
                    //correct_horiz_pos(h_pos, field);
                    break;
                }

                if (opt.opt.gui_type == ConfigOptionDef::GUIType::legend)
                    h_pos += 2 * blinking_button_width;
                if (field->getSizer()) {
                    for (auto child : field->getSizer()->GetChildren()) {
                        if (child->IsWindow() && child->IsShown()) {
                            wxSize  sz = child->GetWindow()->GetSize();
                            h_pos += sz.x + m_h_gap;
                        }
                    }
                } else
                    h_pos += field->getWindow()->GetSize().x + m_h_gap;

                if (option_set.size() == 1 && option_set.front().opt.full_width)
                    break;

                // add sidetext if any
                if ((!option.sidetext.empty() || opt_group->sidetext_width > 0 || option.sidetext_width > 0) && option.sidetext_width != 0)
                    h_pos += (option.sidetext_width > 0 ? option.sidetext_width : opt_group->sidetext_width)* m_em_unit + m_h_gap;

                if (opt.opt_id != option_set.back().opt_id) //! istead of (opt != option_set.back())
                    h_pos += lround(0.6 * m_em_unit);
            }
            break;
        }
        if (ctrl_line.is_line_visible)
            v_pos += ctrl_line.height;
    }

    return wxPoint(h_pos, v_pos);
}


void OG_CustomCtrl::OnPaint(wxPaintEvent&)
{
    // case, when custom controll is destroyed but doesn't deleted from the evet loop
    if(!this->opt_group->custom_ctrl)
        return;

    wxPaintDC dc(this);
    dc.SetFont(m_font);

    wxCoord v_pos = 0;
    for (CtrlLine& line : ctrl_lines) {
        if (!line.is_line_visible)
            continue;
        line.render(dc, v_pos);
        v_pos += line.height;
    }
}

void OG_CustomCtrl::OnMotion(wxMouseEvent& event)
{
    const wxPoint pos = event.GetLogicalPosition(wxClientDC(this));
    wxString tooltip;

    wxString language = wxGetApp().app_config->get("translation_language");

    bool suppress_hyperlinks = get_app_config()->get("suppress_hyperlinks") == "1";

    for (CtrlLine& line : ctrl_lines) {
        line.is_focused = false;
        wxString *str_tooltip;
        size_t idx = 0;
        for (; idx < line.rects_tooltip.size(); ++idx) {
            line.is_focused = is_point_in_rect(pos, line.rects_tooltip[idx].first);
            if (line.is_focused) {
                str_tooltip = &line.rects_tooltip[idx].second;
                break;
            }
        }
        if (line.is_focused) {
            if (!suppress_hyperlinks && !line.og_line.label_path.empty())
                tooltip = OptionsGroup::get_url(line.og_line.label_path) +"\n\n";
            if(str_tooltip)
                tooltip += *str_tooltip;
            break;
        }

        for (size_t opt_idx = 0; opt_idx < line.rects_undo_icon.size(); opt_idx++)
            if (is_point_in_rect(pos, line.rects_undo_icon[opt_idx])) {
                const std::vector<Option>& option_set = line.og_line.get_options();
                Field* field = opt_group->get_field(option_set[opt_idx].opt_id);
                if (field) {
                    const wxString* tooltip_str = field->undo_tooltip();
                    if(tooltip_str)
                        tooltip = *tooltip_str;
                }
                break;
            }
        for (size_t opt_idx = 0; opt_idx < line.rects_undo_to_sys_icon.size(); opt_idx++)
            if (is_point_in_rect(pos, line.rects_undo_to_sys_icon[opt_idx])) {
                const std::vector<Option>& option_set = line.og_line.get_options();
                Field* field = opt_group->get_field(option_set[opt_idx].opt_id);
                if (field) {
                    const wxString* tooltip_str = field->undo_to_sys_tooltip();
                    if (tooltip_str)
                        tooltip = *tooltip_str;
                }
                break;
            }
        if (!tooltip.IsEmpty())
            break;
    }

    // Set tooltips with information for each icon
    this->SetToolTip(tooltip);

    Refresh();
    Update();
    event.Skip();
}

void OG_CustomCtrl::OnLeftDown(wxMouseEvent& event)
{
    const wxPoint pos = event.GetLogicalPosition(wxClientDC(this));

    for (const CtrlLine& line : ctrl_lines) {
        if (line.launch_browser())
            return;
        for (size_t opt_idx = 0; opt_idx < line.rects_undo_icon.size(); opt_idx++)
            if (is_point_in_rect(pos, line.rects_undo_icon[opt_idx])) {
                const std::vector<Option>& option_set = line.og_line.get_options();
                Field* field = opt_group->get_field(option_set[opt_idx].opt_id);
                if (field)
                    field->on_back_to_initial_value();
                event.Skip();
                return;
            }
        for (size_t opt_idx = 0; opt_idx < line.rects_undo_to_sys_icon.size(); opt_idx++)
            if (is_point_in_rect(pos, line.rects_undo_to_sys_icon[opt_idx])) {
                const std::vector<Option>& option_set = line.og_line.get_options();
                Field* field = opt_group->get_field(option_set[opt_idx].opt_id);
                if (field)
                    field->on_back_to_sys_value();
                event.Skip();
                return;
            }
    }

}

void OG_CustomCtrl::OnLeaveWin(wxMouseEvent& event)
{
    for (CtrlLine& line : ctrl_lines)
        line.is_focused = false;

    Refresh();
    Update();
    event.Skip();
}

bool OG_CustomCtrl::update_visibility(ConfigOptionMode mode)
{
    wxCoord    v_pos = 0;

    size_t invisible_lines = 0;
    for (CtrlLine& line : ctrl_lines) {
        line.update_visibility(mode);
        if (line.is_line_visible)
            v_pos += (wxCoord)line.height;
        else
            invisible_lines++;
    }    

    this->SetMinSize(wxSize(wxDefaultCoord, v_pos));

    return invisible_lines != ctrl_lines.size();
}

void OG_CustomCtrl::correct_window_position(wxWindow* win, const Line& line, Field* field/* = nullptr*/)
{
    wxPoint pos = get_pos(line, field);
    int line_height = get_height(line);
    pos.y += std::max(0, int(0.5 * (line_height - win->GetSize().y)));
    win->SetPosition(pos);
};

void OG_CustomCtrl::correct_widgets_position(wxSizer* widget, const Line& line, Field* field/* = nullptr*/) {
    auto children = widget->GetChildren();
    wxPoint line_pos = get_pos(line, field);
    int line_height = get_height(line);
    for (auto child : children) {
        if (child->IsWindow() && child->IsShown()) {
            wxPoint pos = line_pos;
            wxSize  sz = child->GetWindow()->GetSize();
            pos.y += std::max(0, int(0.5 * (line_height - sz.y)));
            if (line.extra_widget_sizer && widget == line.extra_widget_sizer)
                pos.x += m_h_gap;
            child->GetWindow()->SetPosition(pos);
            line_pos.x += sz.x + m_h_gap;
        }
    }
};

void OG_CustomCtrl::init_max_win_width()
{
    if (opt_group->ctrl_horiz_alignment == wxALIGN_RIGHT && m_max_win_width == 0)
        for (CtrlLine& line : ctrl_lines) {
            if (int max_win_width = line.get_max_win_width();
                m_max_win_width < max_win_width)
                m_max_win_width = max_win_width;
    }
}

void OG_CustomCtrl::set_max_win_width(int max_win_width)
{
    if (m_max_win_width == max_win_width)
        return;
    m_max_win_width = max_win_width;
    for (CtrlLine& line : ctrl_lines)
        line.correct_items_positions();

    GetParent()->Layout();
}

void OG_CustomCtrl::msw_rescale()
{
#ifdef __WXOSX__
    return;
#endif
    m_font      = wxGetApp().normal_font();
    m_em_unit   = em_unit(m_parent);
    m_v_gap     = lround(1.0 * m_em_unit);
    m_h_gap     = lround(0.2 * m_em_unit);

    m_bmp_mode_sz = (!m_has_icon) ? wxSize(0, 0) : create_scaled_bitmap("mode_simple", this, wxOSX ? 10 : 12).GetSize();
    m_bmp_blinking_sz = create_scaled_bitmap("search_blink", this).GetSize();
    if (!m_has_icon) m_bmp_blinking_sz.x = 0;

    m_max_win_width = 0;

    wxCoord    v_pos = 0;
    for (CtrlLine& line : ctrl_lines) {
        line.msw_rescale();
        if (line.is_line_visible) {
            v_pos += (wxCoord)line.height;
        }
    }
    this->SetMinSize(wxSize(wxDefaultCoord, v_pos));

    GetParent()->Layout();
}

void OG_CustomCtrl::sys_color_changed()
{
}

OG_CustomCtrl::CtrlLine::CtrlLine(  wxCoord         height,
                                    OG_CustomCtrl*  ctrl,
                                    const Line&     og_line,
                                    bool            draw_just_act_buttons /* = false*/,
                                    bool            draw_mode_bitmap/* = true*/):
    height(height),
    ctrl(ctrl),
    og_line(og_line),
    draw_just_act_buttons(draw_just_act_buttons),
    draw_mode_bitmap(draw_mode_bitmap)
{

    for (size_t i = 0; i < og_line.get_options().size(); i++) {
        rects_undo_icon.emplace_back(wxRect());
        rects_undo_to_sys_icon.emplace_back(wxRect());
    }
}

int OG_CustomCtrl::CtrlLine::get_max_win_width()
{
    int max_win_width = 0;
    if (!draw_just_act_buttons) {
        const std::vector<Option>& option_set = og_line.get_options();
        for (auto opt : option_set) {
            Field* field = ctrl->opt_group->get_field(opt.opt_id);
            if (field && field->getWindow())
                max_win_width = field->getWindow()->GetSize().GetWidth();
        }
    }

    return max_win_width;
}

void OG_CustomCtrl::CtrlLine::correct_items_positions()
{
    if (draw_just_act_buttons || !is_line_visible)
        return;

    if (og_line.near_label_widget_win)
        ctrl->correct_window_position(og_line.near_label_widget_win, og_line);
    if (og_line.widget_sizer)
        ctrl->correct_widgets_position(og_line.widget_sizer, og_line);
    if (og_line.extra_widget_sizer)
        ctrl->correct_widgets_position(og_line.extra_widget_sizer, og_line);

    const std::vector<Option>& option_set = og_line.get_options();
    for (auto opt : option_set) {
        Field* field = ctrl->opt_group->get_field(opt.opt_id);
        if (!field)
            continue;
        if (field->getSizer())
            ctrl->correct_widgets_position(field->getSizer(), og_line, field);
        else if (field->getWindow())
            ctrl->correct_window_position(field->getWindow(), og_line, field);
    }
}

void OG_CustomCtrl::CtrlLine::msw_rescale()
{
    // if we have a single option with no label, no sidetext
    if (draw_just_act_buttons)
        height = (!m_has_icon) ? 0 : get_bitmap_size(create_scaled_bitmap("empty")).GetHeight();

    if (ctrl->opt_group->title_width != 0 && !og_line.label.IsEmpty()) {
        wxSize label_sz = ctrl->GetTextExtent(og_line.label);
        height = label_sz.y * (label_sz.GetWidth() > int(ctrl->opt_group->title_width * ctrl->m_em_unit) ? 2 : 1) + ctrl->m_v_gap;
    }
    
    correct_items_positions();
}

void OG_CustomCtrl::CtrlLine::update_visibility(ConfigOptionMode mode)
{
    if (og_line.is_separator())
        return;
    const std::vector<Option> &option_set = og_line.get_options();
    if (option_set.empty() && og_line.tags_override != comNone)
        return;

    ConfigOptionMode line_mode = option_set.empty() ? og_line.tags_override : option_set.front().opt.mode;
    for (const Option& opt : option_set)
        line_mode |= opt.opt.mode;
    is_line_visible = line_mode == comNone || (line_mode & mode) == mode;

    if (draw_just_act_buttons)
        return;

    if (og_line.near_label_widget_win)
        og_line.near_label_widget_win->Show(is_line_visible);
    if (og_line.widget_sizer)
        og_line.widget_sizer->ShowItems(is_line_visible);
    if (og_line.extra_widget_sizer)
        og_line.extra_widget_sizer->ShowItems(is_line_visible);

    is_visible.clear();
    for (const Option& opt : option_set) {
        Field* field = ctrl->opt_group->get_field(opt.opt_id);
        is_visible.push_back(opt.opt.mode == comNone || (opt.opt.mode & mode) == mode);
        if (!field)
            continue;

        if (field->getSizer()) {
            auto children = field->getSizer()->GetChildren();
            for (auto child : children)
                if (child->IsWindow())
                    child->GetWindow()->Show(is_visible.back());
            field->getSizer()->Show(is_visible.back());
        }
        else if (field->getWindow())
            field->getWindow()->Show(is_visible.back());
    }

    correct_items_positions();
}

void OG_CustomCtrl::CtrlLine::render_separator(wxDC& dc, wxCoord v_pos)
{
    wxPoint begin(ctrl->m_h_gap, v_pos);
    wxPoint end(ctrl->GetSize().GetWidth() - ctrl->m_h_gap, v_pos);

    wxPen pen, old_pen = pen = dc.GetPen();
    pen.SetColour(*wxLIGHT_GREY);
    dc.SetPen(pen);
    dc.DrawLine(begin, end);
    dc.SetPen(old_pen);
}

//TODO push string manipulation out of the render loop (tooltip replace, adding ':')
void OG_CustomCtrl::CtrlLine::render(wxDC& dc, wxCoord v_pos)
{
    if (is_separator()) {
        render_separator(dc, v_pos);
        return;
    }
    if (og_line.get_options().empty())
        return;

    rects_tooltip.clear(); // reset the tooltip detection area, they are re-created by draw_text

    Field* front_field = og_line.get_options().empty() ? nullptr : ctrl->opt_group->get_field(og_line.get_options().front().opt_id);
    int blinking_button_width = ctrl->m_bmp_blinking_sz.GetWidth();
    if(blinking_button_width ) blinking_button_width  += ctrl->m_h_gap;

    bool suppress_hyperlinks = get_app_config()->get("suppress_hyperlinks") == "1";
    if (draw_just_act_buttons) {
        if (front_field && front_field->undo_to_sys_bitmap())
            draw_act_bmps(dc, wxPoint(0, v_pos), front_field->undo_to_sys_bitmap()->bmp(), front_field->undo_bitmap()->bmp(), front_field->blink());
        return;
    }

    wxCoord h_pos = draw_mode_bmp(dc, v_pos);

    if (og_line.near_label_widget_win)
        h_pos += og_line.near_label_widget_win->GetSize().x + ctrl->m_h_gap;

    const std::vector<Option>& option_set = og_line.get_options();

    //round it to next m_em_unit
    h_pos += (h_pos % ctrl->m_em_unit == 0) ? 0 : ctrl->m_em_unit - (h_pos % ctrl->m_em_unit);

    bool is_url_string = false;
    if (ctrl->opt_group->title_width != 0 && !og_line.label.IsEmpty()) {
        bool is_multiline = option_set.size() > 1 || !option_set.front().opt.label.empty();
        // Color the line label is the first settig doesn't have a label
        const wxColour* text_clr = ((option_set.front().opt.label.empty() || option_set.front().opt.label == "_") && front_field ?
            front_field->label_color() : og_line.full_Label_color);
        is_url_string = !suppress_hyperlinks && !og_line.label_path.empty();
        wxString opt_label = (og_line.label.empty() || og_line.label.Last() != '_') ? og_line.label : og_line.label.substr(0, og_line.label.size() - 1);
        bool no_dots = og_line.label.empty() || og_line.label.Last() == '_' || is_multiline;
        h_pos = draw_text(dc, wxPoint(h_pos, v_pos), (no_dots ? opt_label : opt_label + ':'), og_line.label_tooltip , text_clr, ctrl->opt_group->title_width * ctrl->m_em_unit, is_url_string);
    }

    // If there's a widget, build it and set result to the correct position.
    if (og_line.widget != nullptr) {
        draw_blinking_bmp(dc, wxPoint(h_pos, v_pos), og_line.blink);
        return;
    }

    //round it to next m_em_unit
    h_pos += (h_pos % ctrl->m_em_unit == 0) ? 0 : ctrl->m_em_unit - (h_pos % ctrl->m_em_unit);

    // If we're here, we have more than one option or a single option with sidetext
    // so we need a horizontal sizer to arrange these things

    // If we have a single option with no sidetext just add it directly to the grid sizer
    if (option_set.size() == 1 && option_set.front().opt.sidetext.size() == 0 &&
        option_set.front().side_widget == nullptr && og_line.get_extra_widgets().size() == 0)
    {
        if (front_field) {
            if (front_field->undo_to_sys_bitmap())
                h_pos = draw_act_bmps(dc, wxPoint(h_pos, v_pos), front_field->undo_to_sys_bitmap()->bmp(), front_field->undo_bitmap()->bmp(), front_field->blink()) + ctrl->m_h_gap;
            else if (!front_field->undo_to_sys_bitmap() && front_field->blink())
                draw_blinking_bmp(dc, wxPoint(h_pos, v_pos), front_field->blink());
            else
                h_pos += 2 * blinking_button_width;
            // update width for full_width fields
            if (option_set.front().opt.full_width && front_field->getWindow())
                front_field->getWindow()->SetSize(ctrl->GetSize().x - h_pos, -1);
        }
        return;
    }

    size_t bmp_rect_id = 0;
    bool is_multioption_line = option_set.size() > 1 || !option_set.front().opt.label.empty();
    for (size_t i = 0; i < option_set.size(); ++i) {
        if (i >= is_visible.size() || !is_visible[i])
            continue;
        const Option& opt = option_set[i];
        Field* field = ctrl->opt_group->get_field(opt.opt_id);
        ConfigOptionDef option = opt.opt;

        //tooltip for labels
        wxString option_tooltip = _(option.tooltip);
        update_Slic3r_string(option_tooltip);

        // add label if any
        if (is_multioption_line && !option.label.empty()) {
            std::string opt_label = (option.label.empty() || option.label.back() != '_') ? option.label : option.label.substr(0, option.label.size() - 1);
            //!            To correct translation by context have to use wxGETTEXT_IN_CONTEXT macro from wxWidget 3.1.1
            wxString label = /*(opt_label == L_CONTEXT("Top", "Layers") || opt_label == L_CONTEXT("Bottom", "Layers")) ?
                _CTX(opt_label, "Layers") :*/ _(opt_label);
            bool no_dots = label.empty() || option.label.back() == '_';
            if (!no_dots)
                label += ":";
            if (!label.empty() || option.label_width > 0) {
                int width = ctrl->opt_group->sublabel_width * ctrl->m_em_unit;
                if (option.label_width >= 0) {
                    if (option.label_width != 0) {
                        width = option.label_width * ctrl->m_em_unit;
                    }
                } else {
                    if (ctrl->opt_group->label_width > 0) {
                        width = ctrl->opt_group->label_width * ctrl->m_em_unit;
                    }
                }

                if (is_url_string)
                    is_url_string = false;
                else if (opt == option_set.front())
                    is_url_string = !suppress_hyperlinks && !og_line.label_path.empty();
                h_pos = draw_text(dc, wxPoint(h_pos, v_pos), label, option_tooltip, field ? field->label_color() : nullptr, width, is_url_string, !field->m_opt.aligned_label_left);
            }
        }

        //round it to next m_em_unit
        h_pos += (h_pos % ctrl->m_em_unit == 0 ) ? 0 : ctrl->m_em_unit - (h_pos % ctrl->m_em_unit);

        if (field) {
            if(field->undo_to_sys_bitmap())
                h_pos = draw_act_bmps(dc, wxPoint(h_pos, v_pos), field->undo_to_sys_bitmap()->bmp(), field->undo_bitmap()->bmp(), field->blink(), bmp_rect_id++);
            else
                h_pos += 2 * blinking_button_width;

            if (field->getSizer())
            {
                auto children = field->getSizer()->GetChildren();
                for (auto child : children)
                    if (child->IsWindow())
                        h_pos += child->GetWindow()->GetSize().x + ctrl->m_h_gap;
            }
            else if (field->getWindow())
                h_pos += field->getWindow()->GetSize().x + ctrl->m_h_gap;
        }

        // add field
        if (option_set.size() == 1 && option_set.front().opt.full_width)
            break;

        // add sidetext if any
        if ( (!option.sidetext.empty() || ctrl->opt_group->sidetext_width > 0 || option.sidetext_width > 0 ) && option.sidetext_width != 0)
            h_pos = draw_text(dc, wxPoint(h_pos, v_pos), _(option.sidetext), option_tooltip, nullptr, (option.sidetext_width > 0 ? option.sidetext_width : ctrl->opt_group->sidetext_width ) * ctrl->m_em_unit);

        if (opt.opt_id != option_set.back().opt_id) //! istead of (opt != option_set.back())
            h_pos += lround(0.6 * ctrl->m_em_unit);
    }
}

wxCoord OG_CustomCtrl::CtrlLine::draw_mode_bmp(wxDC& dc, wxCoord v_pos)
{
    if (!m_has_icon || og_line.is_separator() || og_line.get_options().empty())
        return 0;
    if (!draw_mode_bitmap)
        return ctrl->m_h_gap;

    ConfigOptionMode mode = og_line.get_options()[0].opt.mode;
    //get the easiest setting
    for (size_t i = 1; i < og_line.get_options().size(); i++)
        mode |= og_line.get_options()[i].opt.mode;
    std::string bmp_name = "mode_other";
    if ((mode & ConfigOptionMode::comSimple) != 0)
        bmp_name = "mode_simple";
    else if ((mode & ConfigOptionMode::comAdvanced) != 0)
        bmp_name = "mode_advanced";
    else if ((mode & ConfigOptionMode::comExpert) != 0)
        bmp_name = "mode_expert";
    wxBitmap bmp = create_scaled_bitmap(bmp_name, ctrl, wxOSX ? 10 : 12);
    wxCoord y_draw = v_pos + lround((height - get_bitmap_size(bmp).GetHeight()) / 2);

    if (og_line.get_options().front().opt.gui_type != ConfigOptionDef::GUIType::legend)
    dc.DrawBitmap(bmp, 0, y_draw);

    return get_bitmap_size(bmp).GetWidth() + ctrl->m_h_gap;
}

wxCoord    OG_CustomCtrl::CtrlLine::draw_text(wxDC& dc, wxPoint pos, const wxString& text, const wxString& tooltip, const wxColour* color, int width, bool is_url/* = false*/, bool align_right/* = false*/)
{
    wxString multiline_text;
    if (width > 0 && dc.GetTextExtent(text).x > width) {
        multiline_text = text;

        size_t idx = size_t(-1);
        for (size_t i = 0; i < multiline_text.Len(); i++)
        {
            if (multiline_text[i] == ' ')
            {
                if (dc.GetTextExtent(multiline_text.SubString(0, i)).x < width)
                    idx = i;
                else {
                    if (idx != size_t(-1))
                        multiline_text[idx] = '\n';
                    else
                        multiline_text[i] = '\n';
                    break;
                }
            }
        }

        if (idx != size_t(-1))
            multiline_text[idx] = '\n';
    }

    if (!text.IsEmpty()) {
        const wxString& out_text = multiline_text.IsEmpty() ? text : multiline_text;
        wxCoord text_width, text_height;
        dc.GetMultiLineTextExtent(out_text, &text_width, &text_height);

        pos.y = pos.y + lround((height - text_height) / 2);
        wxPoint draw_pos = pos;
        if (align_right && width > 0)
            draw_pos.x += width - text_width;
        rects_tooltip.emplace_back(wxRect{draw_pos, wxSize(text_width, text_height)}, tooltip);

        wxColour old_clr = dc.GetTextForeground();
        wxFont old_font = dc.GetFont();
        if (is_focused && is_url)
        // temporary workaround for the OSX because of strange Bold font behavior on BigSerf
#ifdef __APPLE__
            dc.SetFont(old_font.Underlined());
#else
            dc.SetFont(old_font.Bold().Underlined());
#endif            
        dc.SetTextForeground(color ? *color : wxGetApp().get_label_clr_default());
        dc.DrawText(out_text, draw_pos);
        dc.SetTextForeground(old_clr);
        dc.SetFont(old_font);

        if (width < 1)
            width = text_width;
    }

    return pos.x + width + ctrl->m_h_gap;
}

wxPoint OG_CustomCtrl::CtrlLine::draw_blinking_bmp(wxDC& dc, wxPoint pos, bool is_blinking)
{
    wxBitmap bmp_blinking = create_scaled_bitmap(is_blinking ? "search_blink" : "empty", ctrl);
    wxCoord h_pos = pos.x;
    wxCoord v_pos = pos.y + lround((height - get_bitmap_size(bmp_blinking).GetHeight()) / 2);

    int bmp_dim = get_bitmap_size(bmp_blinking).GetWidth();

    dc.DrawBitmap(bmp_blinking, h_pos, v_pos);


    h_pos += bmp_dim + ctrl->m_h_gap;
    return wxPoint(h_pos, v_pos);
}

wxCoord OG_CustomCtrl::CtrlLine::draw_act_bmps(wxDC& dc, wxPoint pos, const wxBitmap& bmp_undo_to_sys, const wxBitmap& bmp_undo, bool is_blinking, size_t rect_id)
{
    wxCoord h_pos = pos.x;
    wxCoord v_pos = pos.y + height / 2 - this->ctrl->m_bmp_blinking_sz.GetHeight() / 2;

    if (m_has_icon) {
        dc.DrawBitmap(bmp_undo_to_sys, h_pos, v_pos);

        int bmp_dim = get_bitmap_size(bmp_undo_to_sys).GetWidth();
        rects_undo_to_sys_icon[rect_id] = wxRect(h_pos, v_pos, bmp_dim, bmp_dim);

        h_pos += bmp_dim + ctrl->m_h_gap;
        dc.DrawBitmap(bmp_undo, h_pos, v_pos);

        bmp_dim = get_bitmap_size(bmp_undo).GetWidth();
        rects_undo_icon[rect_id] = wxRect(h_pos, v_pos, bmp_dim, bmp_dim);

        if(is_blinking)
            draw_blinking_bmp(dc, wxPoint(h_pos, v_pos), is_blinking);

        h_pos += bmp_dim + ctrl->m_h_gap;

    } else if (is_blinking) {
        draw_blinking_bmp(dc, wxPoint(h_pos, v_pos), is_blinking);
    }
    return h_pos;
}

bool OG_CustomCtrl::CtrlLine::launch_browser() const
{
    if (!is_focused || og_line.label_path.empty())
        return false;

    return OptionsGroup::launch_browser(og_line.label_path);
}

} // GUI
} // Slic3r
