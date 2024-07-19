///|/ Copyright (c) Prusa Research 2018 - 2023 Oleksandra Iushchenko @YuSanka, Enrico Turri @enricoturri1966, Vojtěch Bubník @bubnikv, David Kocík @kocikdav, Lukáš Matěna @lukasmatena, Tomáš Mészáros @tamasmeszaros, Vojtěch Král @vojtechkral
///|/ Copyright (c) 2020 Benjamin Greiner
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "wxExtensions.hpp"

#include <stdexcept>
#include <cmath>

#include <wx/sizer.h>

#include <boost/algorithm/string/replace.hpp>


#include "libslic3r/AppConfig.hpp"

#include "BitmapCache.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "libslic3r/Config.hpp"
#include "I18N.hpp"
#include "GUI_Utils.hpp"
#include "Plater.hpp"
#include "../Utils/MacDarkMode.hpp"
#include "BitmapComboBox.hpp"
#include "libslic3r/Utils.hpp"
#include "OG_CustomCtrl.hpp"
#include "format.hpp"

#include "libslic3r/Color.hpp"

#ifndef __linux__
// msw_menuitem_bitmaps is used for MSW and OSX
static std::map<int, std::string> msw_menuitem_bitmaps;
void sys_color_changed_menu(wxMenu* menu)
{
	struct update_icons {
		static void run(wxMenuItem* item) {
			const auto it = msw_menuitem_bitmaps.find(item->GetId());
			if (it != msw_menuitem_bitmaps.end()) {
				wxBitmapBundle* item_icon = get_bmp_bundle(it->second);
				if (item_icon->IsOk())
					item->SetBitmap(*item_icon);
			}
			if (item->IsSubMenu())
				for (wxMenuItem *sub_item : item->GetSubMenu()->GetMenuItems())
					update_icons::run(sub_item);
		}
	};

	for (wxMenuItem *item : menu->GetMenuItems())
		update_icons::run(item);
}
#endif /* no __linux__ */

void enable_menu_item(wxUpdateUIEvent& evt, std::function<bool()> const cb_condition, wxMenuItem* item, wxWindow* win)
{
    const bool enable = cb_condition();
    evt.Enable(enable);
}

wxMenuItem* append_menu_item(wxMenu* menu, int id, const wxString& string, const wxString& description,
    std::function<void(wxCommandEvent& event)> cb, wxBitmapBundle* icon, wxEvtHandler* event_handler,
    std::function<bool()> const cb_condition, wxWindow* parent, int insert_pos/* = wxNOT_FOUND*/)
{
    if (id == wxID_ANY)
        id = wxNewId();

    auto *item = new wxMenuItem(menu, id, string, description);
    if (icon && icon->IsOk()) {
        item->SetBitmap(*icon);
    }
    if (insert_pos == wxNOT_FOUND)
        menu->Append(item);
    else
        menu->Insert(insert_pos, item);

#ifdef __WXMSW__
    if (event_handler != nullptr && event_handler != menu)
        event_handler->Bind(wxEVT_MENU, cb, id);
    else
#endif // __WXMSW__
        menu->Bind(wxEVT_MENU, cb, id);

    if (parent) {
        parent->Bind(wxEVT_UPDATE_UI, [cb_condition, item, parent](wxUpdateUIEvent& evt) {
            enable_menu_item(evt, cb_condition, item, parent); }, id);
    }

    return item;
}

wxMenuItem* append_menu_item(wxMenu* menu, int id, const wxString& string, const wxString& description,
    std::function<void(wxCommandEvent& event)> cb, const std::string& icon, wxEvtHandler* event_handler,
    std::function<bool()> const cb_condition, wxWindow* parent, int insert_pos/* = wxNOT_FOUND*/)
{
    if (id == wxID_ANY)
        id = wxNewId();

    wxBitmapBundle* bmp = icon.empty() ? nullptr : get_bmp_bundle(icon);

#ifndef __linux__
    if (bmp && bmp->IsOk())
        msw_menuitem_bitmaps[id] = icon;
#endif /* no __linux__ */

    return append_menu_item(menu, id, string, description, cb, bmp, event_handler, cb_condition, parent, insert_pos);
}

wxMenuItem* append_submenu(wxMenu* menu, wxMenu* sub_menu, int id, const wxString& string, const wxString& description, const std::string& icon,
    std::function<bool()> const cb_condition, wxWindow* parent)
{
    if (id == wxID_ANY)
        id = wxNewId();

    wxMenuItem* item = new wxMenuItem(menu, id, string, description);
    if (!icon.empty()) {
        item->SetBitmap(*get_bmp_bundle(icon));

#ifndef __linux__
        msw_menuitem_bitmaps[id] = icon;
#endif // no __linux__
    }

    item->SetSubMenu(sub_menu);
    menu->Append(item);

    if (parent) {
        parent->Bind(wxEVT_UPDATE_UI, [cb_condition, item, parent](wxUpdateUIEvent& evt) {
            enable_menu_item(evt, cb_condition, item, parent); }, id);
    }

    return item;
}

wxMenuItem* append_menu_radio_item(wxMenu* menu, int id, const wxString& string, const wxString& description,
    std::function<void(wxCommandEvent& event)> cb, wxEvtHandler* event_handler)
{
    if (id == wxID_ANY)
        id = wxNewId();

    wxMenuItem* item = menu->AppendRadioItem(id, string, description);

#ifdef __WXMSW__
    if (event_handler != nullptr && event_handler != menu)
        event_handler->Bind(wxEVT_MENU, cb, id);
    else
#endif // __WXMSW__
        menu->Bind(wxEVT_MENU, cb, id);

    return item;
}

wxMenuItem* append_menu_check_item(wxMenu* menu, int id, const wxString& string, const wxString& description,
    std::function<void(wxCommandEvent & event)> cb, wxEvtHandler* event_handler,
    std::function<bool()> const enable_condition, std::function<bool()> const check_condition, wxWindow* parent)
{
    if (id == wxID_ANY)
        id = wxNewId();

    wxMenuItem* item = menu->AppendCheckItem(id, string, description);

#ifdef __WXMSW__
    if (event_handler != nullptr && event_handler != menu)
        event_handler->Bind(wxEVT_MENU, cb, id);
    else
#endif // __WXMSW__
        menu->Bind(wxEVT_MENU, cb, id);

    if (parent)
        parent->Bind(wxEVT_UPDATE_UI, [enable_condition, check_condition](wxUpdateUIEvent& evt)
            {
                evt.Enable(enable_condition());
                evt.Check(check_condition());
            }, id);

    return item;
}

uint32_t color_from_hex(std::string hex)
{
    std::stringstream ss;
    ss << std::hex << hex;
    uint32_t color_bad_endian;
    ss >> color_bad_endian;
    uint32_t color = 0;
    color |= (color_bad_endian & 0xFF) << 16;
    color |= (color_bad_endian & 0xFF00);
    color |= (color_bad_endian & 0xFF0000) >> 16;
    return color;
}

wxColour color_from_int(uint32_t color)
{
    return wxColour{ uint8_t(color & 0xFF), uint8_t((color & 0xFF00) >> 8) , uint8_t((color & 0xFF0000) >> 16) , uint8_t(255) };
}

std::string color_to_hex(uint32_t color)
{
    uint32_t color_bad_endian = 0;
    color_bad_endian |= (color & 0xFF) << 16;
    color_bad_endian |= (color & 0xFF00);
    color_bad_endian |= (color & 0xFF0000) >> 16;
    std::stringstream ss;
    ss << std::hex << color_bad_endian;
    return ss.str();
}

const unsigned int wxCheckListBoxComboPopup::DefaultWidth = 200;
const unsigned int wxCheckListBoxComboPopup::DefaultHeight = 200;

bool wxCheckListBoxComboPopup::Create(wxWindow* parent)
{
    return wxCheckListBox::Create(parent, wxID_HIGHEST + 1, wxPoint(0, 0));
}

wxWindow* wxCheckListBoxComboPopup::GetControl()
{
    return this;
}

void wxCheckListBoxComboPopup::SetStringValue(const wxString& value)
{
    m_text = value;
}

wxString wxCheckListBoxComboPopup::GetStringValue() const
{
    return m_text;
}

wxSize wxCheckListBoxComboPopup::GetAdjustedSize(int minWidth, int prefHeight, int maxHeight)
{
    // set width dinamically in dependence of items text
    // and set height dinamically in dependence of items count

    wxComboCtrl* cmb = GetComboCtrl();
    if (cmb != nullptr) {
        wxSize size = GetComboCtrl()->GetSize();

        unsigned int count = GetCount();
        if (count > 0) {
            int max_width = size.x;
            for (unsigned int i = 0; i < count; ++i) {
                max_width = std::max(max_width, 60 + GetTextExtent(GetString(i)).x);
            }
            size.SetWidth(max_width);
            size.SetHeight(count * cmb->GetCharHeight());
        }
        else
            size.SetHeight(DefaultHeight);

        return size;
    }
    else
        return wxSize(DefaultWidth, DefaultHeight);
}

void wxCheckListBoxComboPopup::OnKeyEvent(wxKeyEvent& evt)
{
    // filters out all the keys which are not working properly
    switch (evt.GetKeyCode())
    {
    case WXK_LEFT:
    case WXK_UP:
    case WXK_RIGHT:
    case WXK_DOWN:
    case WXK_PAGEUP:
    case WXK_PAGEDOWN:
    case WXK_END:
    case WXK_HOME:
    case WXK_NUMPAD_LEFT:
    case WXK_NUMPAD_UP:
    case WXK_NUMPAD_RIGHT:
    case WXK_NUMPAD_DOWN:
    case WXK_NUMPAD_PAGEUP:
    case WXK_NUMPAD_PAGEDOWN:
    case WXK_NUMPAD_END:
    case WXK_NUMPAD_HOME:
    {
        break;
    }
    default:
    {
        evt.Skip();
        break;
    }
    }
}

void wxCheckListBoxComboPopup::OnCheckListBox(wxCommandEvent& evt)
{
    // forwards the checklistbox event to the owner wxComboCtrl

    if (m_check_box_events_status == OnCheckListBoxFunction::FreeToProceed )
    {
        wxComboCtrl* cmb = GetComboCtrl();
        if (cmb != nullptr) {
            wxCommandEvent event(wxEVT_CHECKLISTBOX, cmb->GetId());
            event.SetEventObject(cmb);
            cmb->ProcessWindowEvent(event);
        }
    }

    evt.Skip();

    #ifndef _WIN32  // events are sent differently on OSX+Linux vs Win (more description in header file)
        if ( m_check_box_events_status == OnCheckListBoxFunction::RefuseToProceed )
            // this happens if the event was resent by OnListBoxSelection - next call to OnListBoxSelection is due to user clicking the text, so the function should
            // explicitly change the state on the checkbox
            m_check_box_events_status = OnCheckListBoxFunction::WasRefusedLastTime;
        else
            // if the user clicked the checkbox square, this event was sent before OnListBoxSelection was called, so we don't want it to resend it
            m_check_box_events_status = OnCheckListBoxFunction::RefuseToProceed;
    #endif
}

void wxCheckListBoxComboPopup::OnListBoxSelection(wxCommandEvent& evt)
{
    // transforms list box item selection event into checklistbox item toggle event 

    int selId = GetSelection();
    if (selId != wxNOT_FOUND)
    {
        #ifndef _WIN32
            if (m_check_box_events_status == OnCheckListBoxFunction::RefuseToProceed)
        #endif
                Check((unsigned int)selId, !IsChecked((unsigned int)selId));

        m_check_box_events_status = OnCheckListBoxFunction::FreeToProceed; // so the checkbox reacts to square-click the next time

        SetSelection(wxNOT_FOUND);
        wxCommandEvent event(wxEVT_CHECKLISTBOX, GetId());
        event.SetInt(selId);
        event.SetEventObject(this);
        ProcessEvent(event);
    }
}


// ***  wxDataViewTreeCtrlComboPopup  ***

const unsigned int wxDataViewTreeCtrlComboPopup::DefaultWidth = 270;
const unsigned int wxDataViewTreeCtrlComboPopup::DefaultHeight = 200;
const unsigned int wxDataViewTreeCtrlComboPopup::DefaultItemHeight = 22;

bool wxDataViewTreeCtrlComboPopup::Create(wxWindow* parent)
{
	return wxDataViewTreeCtrl::Create(parent, wxID_ANY/*HIGHEST + 1*/, wxPoint(0, 0), wxDefaultSize/*wxSize(270, -1)*/, wxDV_NO_HEADER);
}
/*
wxSize wxDataViewTreeCtrlComboPopup::GetAdjustedSize(int minWidth, int prefHeight, int maxHeight)
{
	// matches owner wxComboCtrl's width
	// and sets height dinamically in dependence of contained items count
	wxComboCtrl* cmb = GetComboCtrl();
	if (cmb != nullptr)
	{
		wxSize size = GetComboCtrl()->GetSize();
		if (m_cnt_open_items > 0)
			size.SetHeight(m_cnt_open_items * DefaultItemHeight);
		else
			size.SetHeight(DefaultHeight);

		return size;
	}
	else
		return wxSize(DefaultWidth, DefaultHeight);
}
*/
void wxDataViewTreeCtrlComboPopup::OnKeyEvent(wxKeyEvent& evt)
{
	// filters out all the keys which are not working properly
	if (evt.GetKeyCode() == WXK_UP)
	{
		return;
	}
	else if (evt.GetKeyCode() == WXK_DOWN)
	{
		return;
	}
	else
	{
		evt.Skip();
		return;
	}
}

void wxDataViewTreeCtrlComboPopup::OnDataViewTreeCtrlSelection(wxCommandEvent& evt)
{
	wxComboCtrl* cmb = GetComboCtrl();
	auto selected = GetItemText(GetSelection());
	cmb->SetText(selected);
}

// edit tooltip : change Slic3r to SLIC3R_APP_KEY
// Temporary workaround for localization
void update_Slic3r_string(wxString& tooltip)
{
    tooltip.Replace("Slic3r", SLIC3R_APP_NAME, true);
}

/* Function for rescale of buttons in Dialog under MSW if dpi is changed.
 * btn_ids - vector of buttons identifiers
 */
void msw_buttons_rescale(wxDialog* dlg, const int em_unit, const std::vector<int>& btn_ids, double height_koef/* = 1.*/)
{
    const wxSize& btn_size = wxSize(-1, int(2.5 * em_unit * height_koef + 0.5f));

    for (int btn_id : btn_ids) {
        // There is a case [FirmwareDialog], when we have wxControl instead of wxButton
        // so let casting everything to the wxControl
        wxControl* btn = static_cast<wxControl*>(dlg->FindWindowById(btn_id, dlg));
        if (btn)
            btn->SetMinSize(btn_size);
    }
}

/* Function for getting of em_unit value from correct parent.
 * In most of cases it is m_em_unit value from GUI_App,
 * but for DPIDialogs it's its own value. 
 * This value will be used to correct rescale after moving between 
 * Displays with different HDPI */
int em_unit(wxWindow* win)
{
    if (win)
    {
        wxTopLevelWindow *toplevel = Slic3r::GUI::find_toplevel_parent(win);
        Slic3r::GUI::DPIDialog* dlg = dynamic_cast<Slic3r::GUI::DPIDialog*>(toplevel);
        if (dlg)
            return dlg->em_unit();
        Slic3r::GUI::DPIFrame* frame = dynamic_cast<Slic3r::GUI::DPIFrame*>(toplevel);
        if (frame)
            return frame->em_unit();
    }
    
    return Slic3r::GUI::wxGetApp().em_unit();
}

#ifdef __WXGTK2__
static int scale() 
{
    return int(em_unit(nullptr) * 0.1f + 0.5f);
}
#endif // __WXGTK2__

wxBitmapBundle* get_bmp_bundle(const std::string& bmp_name_in, int width/* = 16*/, int height/* = -1*/, const std::string& new_color/* = std::string()*/)
{

    // Try loading an SVG first, then PNG if SVG is not found:
    Slic3r::ColorReplaces changes;
    //grayscale: just ask for new_color="#606060"
    if (new_color.empty() || new_color.size() > 7 || new_color.size() < 6) {
        try {
            uint32_t color_int = Slic3r::GUI::wxGetApp().app_config->create_color(0.86f, 0.93f);
            changes.add("#ED6B21", color_int);
            changes.add("#2172eb", color_int);
        }
        catch (std::exception /*e*/) {
        }
    } else {
        changes.add("#ED6B21", new_color.size() == 7 ? new_color : (std::string("#") + new_color));
        changes.add("#2172eb", new_color.size() == 7 ? new_color : (std::string("#") + new_color));
    }
    
    if (Slic3r::GUI::wxGetApp().dark_mode()) {
        changes.add("#808080", "#FFFFFF");
    }

    return get_bmp_bundle(bmp_name_in, width, height, changes);
}


wxBitmapBundle *get_bmp_bundle(const std::string &bmp_name_in, int width, int height, Slic3r::ColorReplaces &new_colors_rgb)
{
    
#ifdef __WXGTK2__
    width *= scale();
    if (height > 0)
        height *= scale();
#endif // __WXGTK2__

    static Slic3r::GUI::BitmapCache cache;

    std::string bmp_name = bmp_name_in;
    boost::replace_last(bmp_name, ".png", "");

    if (height < 0)
        height = width;

    if (Slic3r::GUI::wxGetApp().dark_mode())
        new_colors_rgb.add("#808080", "#FFFFFF");

    //wxBitmap *bmp = cache.load_svg(bmp_name, width, height, grayscale, dark_mode, color);
    wxBitmapBundle* bmp = cache.from_svg(bmp_name, width, height, new_colors_rgb);
    if (bmp == nullptr) {
        bmp = cache.from_png(bmp_name, width, height, new_colors_rgb);
        if (!bmp)
        // Neither SVG nor PNG has been found, raise error
        throw Slic3r::RuntimeError("Could not load bitmap: " + bmp_name);
    }
    return bmp;
}

wxBitmapBundle* get_empty_bmp_bundle(int width, int height)
{
    static Slic3r::GUI::BitmapCache cache;
#ifdef __WXGTK2__
    return cache.mkclear_bndl(width * scale(), height * scale());
#else
    return cache.mkclear_bndl(width, height);
#endif // __WXGTK2__
}

wxBitmapBundle* get_solid_bmp_bundle(int width, int height, const std::string& color )
{
    static Slic3r::GUI::BitmapCache cache;
#ifdef __WXGTK2__
    return cache.mksolid_bndl(width * scale(), height * scale(), color, 1, Slic3r::GUI::wxGetApp().dark_mode());
#else
    return cache.mksolid_bndl(width, height, color, 1, Slic3r::GUI::wxGetApp().dark_mode());
#endif // __WXGTK2__
}

std::vector<wxBitmapBundle*> get_extruder_color_icons(bool thin_icon/* = false*/)
{
    // Create the bitmap with color bars.
    std::vector<wxBitmapBundle*> bmps;
    std::vector<std::string> colors = Slic3r::GUI::wxGetApp().plater()->get_extruder_colors_from_plater_config();

    if (colors.empty())
        return bmps;

    for (const std::string& color : colors)
        bmps.emplace_back(get_solid_bmp_bundle(thin_icon ? 16 : 32, 16, color));

    return bmps;
}


void apply_extruder_selector(Slic3r::GUI::BitmapComboBox** ctrl, 
                             wxWindow* parent,
                             const std::string& first_item/* = ""*/, 
                             wxPoint pos/* = wxDefaultPosition*/,
                             wxSize size/* = wxDefaultSize*/,
                             bool use_thin_icon/* = false*/)
{
    std::vector<wxBitmapBundle*> icons = get_extruder_color_icons(use_thin_icon);

    if (!*ctrl) {
        *ctrl = new Slic3r::GUI::BitmapComboBox(parent, wxID_ANY, wxEmptyString, pos, size, 0, nullptr, wxCB_READONLY);
        Slic3r::GUI::wxGetApp().UpdateDarkUI(*ctrl);
    }
    else
    {
        (*ctrl)->SetPosition(pos);
        (*ctrl)->SetMinSize(size);
        (*ctrl)->SetSize(size);
        (*ctrl)->Clear();
    }
    if (first_item.empty())
        (*ctrl)->Hide();    // to avoid unwanted rendering before layout (ExtruderSequenceDialog)

    if (icons.empty() && !first_item.empty()) {
        (*ctrl)->Append(_(first_item), wxNullBitmap);
        return;
    }

    // For ObjectList we use short extruder name (just a number)
    const bool use_full_item_name = dynamic_cast<Slic3r::GUI::ObjectList*>(parent) == nullptr;

    int i = 0;
    wxString str = _(L("Extruder"));
    for (wxBitmapBundle* bmp : icons) {
        if (i == 0) {
            if (!first_item.empty())
                (*ctrl)->Append(_(first_item), *bmp);
            ++i;
        }

        (*ctrl)->Append(use_full_item_name
                        ? Slic3r::GUI::from_u8((boost::format("%1% %2%") % str % i).str())
                        : wxString::Format("%d", i), *bmp);
        ++i;
    }
    (*ctrl)->SetSelection(0);
}


// ----------------------------------------------------------------------------
// LockButton
// ----------------------------------------------------------------------------

LockButton::LockButton( wxWindow *parent, 
                        wxWindowID id, 
                        const wxPoint& pos /*= wxDefaultPosition*/, 
                        const wxSize& size /*= wxDefaultSize*/):
                        wxButton(parent, id, wxEmptyString, pos, size, wxBU_EXACTFIT | wxNO_BORDER)
{
    m_bmp_lock_closed   = ScalableBitmap(this, "lock_closed");
    m_bmp_lock_closed_f = ScalableBitmap(this, "lock_closed_f");
    m_bmp_lock_open     = ScalableBitmap(this, "lock_open");
    m_bmp_lock_open_f   = ScalableBitmap(this, "lock_open_f");

    Slic3r::GUI::wxGetApp().UpdateDarkUI(this);
    SetBitmap(m_bmp_lock_open.bmp());
    SetBitmapDisabled(m_bmp_lock_open.bmp());
    SetBitmapCurrent(m_bmp_lock_closed_f.bmp());

    //button events
    Bind(wxEVT_BUTTON, &LockButton::OnButton, this);
}

void LockButton::OnButton(wxCommandEvent& event)
{
    if (m_disabled)
        return;

    SetLock(!m_is_pushed);
    event.Skip();
}

void LockButton::SetLock(bool lock)
{
    if (m_is_pushed != lock) {
        m_is_pushed = lock;
        update_button_bitmaps();
    }
}

void LockButton::sys_color_changed()
{
    Slic3r::GUI::wxGetApp().UpdateDarkUI(this);

    m_bmp_lock_closed.sys_color_changed();
    m_bmp_lock_closed_f.sys_color_changed();
    m_bmp_lock_open.sys_color_changed();
    m_bmp_lock_open_f.sys_color_changed();

    update_button_bitmaps();
}

void LockButton::update_button_bitmaps()
{
    SetBitmap(m_is_pushed ? m_bmp_lock_closed.bmp() : m_bmp_lock_open.bmp());
    SetBitmapCurrent(m_is_pushed ? m_bmp_lock_closed_f.bmp() : m_bmp_lock_open_f.bmp());

    Refresh();
    Update();
}

// ----------------------------------------------------------------------------
// MenuWithSeparators
// ----------------------------------------------------------------------------

void MenuWithSeparators::DestroySeparators()
{
    if (m_separator_frst) {
        Destroy(m_separator_frst);
        m_separator_frst = nullptr;
    }

    if (m_separator_scnd) {
        Destroy(m_separator_scnd);
        m_separator_scnd = nullptr;
    }
}

void MenuWithSeparators::SetFirstSeparator()
{
    m_separator_frst = this->AppendSeparator();
}

void MenuWithSeparators::SetSecondSeparator()
{
    m_separator_scnd = this->AppendSeparator();
}

// ----------------------------------------------------------------------------
// PrusaBitmap
// ----------------------------------------------------------------------------
ScalableBitmap::ScalableBitmap( wxWindow *parent, 
                                const std::string& icon_name,
                                const int  width/* = 16*/,
                                const int  height/* = -1*/,
                                const bool grayscale/* = false*/):
    m_parent(parent), m_icon_name(icon_name),
    m_bmp_width(width), m_bmp_height(height)
{
    m_bmp = *get_bmp_bundle(icon_name, width, height);
    m_bitmap = m_bmp.GetBitmapFor(m_parent);
}

ScalableBitmap::ScalableBitmap( wxWindow*           parent,
                                const std::string&  icon_name,
                                const wxSize        icon_size,
                                const bool          grayscale/* = false*/) :
ScalableBitmap(parent, icon_name, icon_size.x, icon_size.y, grayscale)
{
}

void ScalableBitmap::sys_color_changed()
{
    m_bmp = *get_bmp_bundle(m_icon_name, m_bmp_width, m_bmp_height);
}

// ----------------------------------------------------------------------------
// PrusaButton
// ----------------------------------------------------------------------------

ScalableButton::ScalableButton( wxWindow *          parent,
                                wxWindowID          id,
                                const std::string&  icon_name /*= ""*/,
                                const wxString&     label /* = wxEmptyString*/,
                                const wxSize&       size /* = wxDefaultSize*/,
                                const wxPoint&      pos /* = wxDefaultPosition*/,
                                long                style /*= wxBU_EXACTFIT | wxNO_BORDER*/,
                                int                 width/* = 16*/, 
                                int                 height/* = -1*/) :
    m_parent(parent),
    m_current_icon_name(icon_name),
    m_bmp_width(width),
    m_bmp_height(height),
    m_has_border(!(style & wxNO_BORDER))
{
    Create(parent, id, label, pos, size, style);
    Slic3r::GUI::wxGetApp().UpdateDarkUI(this);

    if (!icon_name.empty()) {
        SetBitmap(*get_bmp_bundle(icon_name, width, height));
        if (!label.empty())
            SetBitmapMargins(int(0.5* em_unit(parent)), 0);
    }

    if (size != wxDefaultSize)
    {
        const int em = em_unit(parent);
        m_width = size.x/em;
        m_height= size.y/em;
    }
}


ScalableButton::ScalableButton( wxWindow *          parent, 
                                wxWindowID          id,
                                const ScalableBitmap&  bitmap,
                                const wxString&     label /*= wxEmptyString*/, 
                                long                style /*= wxBU_EXACTFIT | wxNO_BORDER*/) :
    m_parent(parent),
    m_current_icon_name(bitmap.name()),
    m_bmp_width(bitmap.px_size().x),
    m_bmp_height(bitmap.px_size().y),
    m_has_border(!(style& wxNO_BORDER))
{
    Create(parent, id, label, wxDefaultPosition, wxDefaultSize, style);
    Slic3r::GUI::wxGetApp().UpdateDarkUI(this);

    SetBitmap(bitmap.bmp());
}

void ScalableButton::SetBitmap_(const ScalableBitmap& bmp)
{
    m_bmp_width = (bmp.px_size().x),
    m_bmp_height = (bmp.px_size().y),
    SetBitmap(bmp.bmp());
    m_current_icon_name = bmp.name();
}

void ScalableButton::SetBitmap_(const wxBitmap& bmp)
{
    SetBitmap(bmp);
}

bool ScalableButton::SetBitmap_(const std::string& bmp_name, int bmp_width)
{
    m_current_icon_name = bmp_name;
    if (m_current_icon_name.empty())
        return false;

    m_bmp_width = bmp_width;

    wxBitmapBundle bmp = *get_bmp_bundle(m_current_icon_name, m_bmp_width, m_bmp_height);
    SetBitmap(bmp);
    SetBitmapCurrent(bmp);
    SetBitmapPressed(bmp);
    SetBitmapFocus(bmp);
    SetBitmapDisabled(bmp);
    return true;
}

void ScalableButton::SetBitmapDisabled_(const ScalableBitmap& bmp)
{
    SetBitmapDisabled(bmp.bmp());
    m_disabled_icon_name = bmp.name();
}

int ScalableButton::GetBitmapHeight()
{
#ifdef __APPLE__
    return GetBitmap().GetScaledHeight();
#else
    return GetBitmap().GetHeight();
#endif
}

void ScalableButton::sys_color_changed()
{
    Slic3r::GUI::wxGetApp().UpdateDarkUI(this, m_has_border);

    wxBitmapBundle bmp = *get_bmp_bundle(m_current_icon_name, m_bmp_width, m_bmp_height);
        SetBitmap(bmp);
        SetBitmapCurrent(bmp);
        SetBitmapPressed(bmp);
        SetBitmapFocus(bmp);
        if (!m_disabled_icon_name.empty())
        SetBitmapDisabled(*get_bmp_bundle(m_disabled_icon_name, m_bmp_width, m_bmp_height));
    if (!GetLabelText().IsEmpty())
        SetBitmapMargins(int(0.5 * em_unit(m_parent)), 0);
}

// ----------------------------------------------------------------------------
// BlinkingBitmap
// ----------------------------------------------------------------------------

BlinkingBitmap::BlinkingBitmap(wxWindow* parent, const std::string& icon_name) :
    wxStaticBitmap(parent, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxSize(int(1.6 * Slic3r::GUI::wxGetApp().em_unit()), -1))
{
    bmp = ScalableBitmap(parent, icon_name);
}

void BlinkingBitmap::invalidate()
{
    this->SetBitmap(wxNullBitmap);
}

void BlinkingBitmap::activate()
{
    this->SetBitmap(bmp.bmp());
    show = true;
}

void BlinkingBitmap::blink()
{
    show = !show;
    this->SetBitmap(show ? bmp.bmp() : wxNullBitmap);
}

namespace Slic3r {
namespace GUI {

void Highlighter::set_timer_owner(wxWindow* owner, int timerid/* = wxID_ANY*/)
{
    m_timer.SetOwner(owner, timerid);
    bind_timer(owner);
}

bool Highlighter::init(bool input_failed)
{
    if (input_failed)
        return false;

    m_timer.Start(300, false);
    return true;
}
void Highlighter::invalidate()
{
    if (m_timer.IsRunning())
        m_timer.Stop();
    m_blink_counter = 0;
}

void Highlighter::blink()
{
    if ((++m_blink_counter) == 11)
        invalidate();
}

// HighlighterForWx

void HighlighterForWx::bind_timer(wxWindow* owner)
{
    owner->Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        blink();
    });
}

// using OG_CustomCtrl where arrow will be rendered and flag indicated "show/hide" state of this arrow
void HighlighterForWx::init(wxWindow* refresh_panel, bool* blink_bool)
{
    invalidate();
    if (!Highlighter::init(!refresh_panel && !blink_bool))
        return;

    m_custom_ctrl = refresh_panel;
    m_show_blink_ptr = blink_bool;

    *m_show_blink_ptr = true;
    m_custom_ctrl->Refresh();
}

// - using a BlinkingBitmap. Change state of this bitmap
void HighlighterForWx::init(BlinkingBitmap* blinking_bmp)
{
    invalidate();
    if (!Highlighter::init(!blinking_bmp))
        return;

    m_blinking_bitmap = blinking_bmp;
    m_blinking_bitmap->activate();
}

void HighlighterForWx::invalidate()
{
    Highlighter::invalidate();

    if (m_custom_ctrl && m_show_blink_ptr) {
        *m_show_blink_ptr = false;
        m_custom_ctrl->Refresh();
        m_show_blink_ptr = nullptr;
        m_custom_ctrl = nullptr;
    }
    else if (m_blinking_bitmap) {
        m_blinking_bitmap->invalidate();
        m_blinking_bitmap = nullptr;
    }
}

void HighlighterForWx::blink()
{
    if (m_custom_ctrl && m_show_blink_ptr) {
        *m_show_blink_ptr = !*m_show_blink_ptr;
        m_custom_ctrl->Refresh();
    }
    else if (m_blinking_bitmap)
        m_blinking_bitmap->blink();
    else
        return;

    Highlighter::blink();
}

}// GUI
}//Slicer




