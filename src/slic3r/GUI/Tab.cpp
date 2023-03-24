// #include "libslic3r/GCodeSender.hpp"
#include "slic3r/Utils/Serial.hpp"
#include "Tab.hpp"
#include "PresetHints.hpp"
#include "libslic3r/Log.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"

#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/PrintHost.hpp"
#include "BonjourDialog.hpp"
#include "WipeTowerDialog.hpp"
#include "ButtonsDescription.hpp"
#include "Search.hpp"
#include "OG_CustomCtrl.hpp"

#include <wx/app.h>
#include <wx/button.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>

#include <wx/bmpcbox.h>
#include <wx/bmpbuttn.h>
#include <wx/collpane.h>
#include <wx/treectrl.h>
#include <wx/imaglist.h>
#include <wx/settings.h>
#include <wx/filedlg.h>

#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/lexical_cast.hpp>

#include "wxExtensions.hpp"
#include "PresetComboBoxes.hpp"
#include <wx/wupdlock.h>

#include <libslic3r/GCodeWriter.hpp>
#include <libslic3r/Slicing.hpp>

#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "Plater.hpp"
#include "MainFrame.hpp"
#include "GLCanvas3D.hpp"
#include "format.hpp"
#include "PhysicalPrinterDialog.hpp"
#include "UnsavedChangesDialog.hpp"
#include "SavePresetDialog.hpp"
#include "Search.hpp"
#include "MsgDialog.hpp"
#include "Notebook.hpp"

#ifdef WIN32
    #include <commctrl.h>
#endif // WIN32

namespace Slic3r {
namespace GUI {


void Tab::Highlighter::set_timer_owner(wxEvtHandler* owner, int timerid/* = wxID_ANY*/)
{
    m_timer.SetOwner(owner, timerid);
}

void Tab::Highlighter::init(std::pair<OG_CustomCtrl*, bool*> params)
{
    if (m_timer.IsRunning())
        invalidate();
    if (!params.first || !params.second)
        return;

    m_timer.Start(300, false);

    m_custom_ctrl = params.first;
    m_show_blink_ptr = params.second;

    *m_show_blink_ptr = true;
    m_custom_ctrl->Refresh();
}

void Tab::Highlighter::invalidate()
{
    m_timer.Stop();

    if (m_custom_ctrl && m_show_blink_ptr) {
        *m_show_blink_ptr = false;
        m_custom_ctrl->Refresh();
        m_show_blink_ptr = nullptr;
        m_custom_ctrl = nullptr;
    }

    m_blink_counter = 0;
}

void Tab::Highlighter::blink()
{
    if (m_custom_ctrl && m_show_blink_ptr) {
        *m_show_blink_ptr = !*m_show_blink_ptr;
        m_custom_ctrl->Refresh();
    }
    else
        return;

    if ((++m_blink_counter) == 11)
        invalidate();
}


Tab::Tab(wxBookCtrlBase* parent, const wxString& title, Preset::Type type) :
    m_parent(parent), m_title(title), m_type(type), m_script_exec()
{
    Create(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBK_LEFT | wxTAB_TRAVERSAL/*, name*/);
    this->SetFont(Slic3r::GUI::wxGetApp().normal_font());

    wxGetApp().UpdateDarkUI(this);

    m_compatible_printers.type			= Preset::TYPE_PRINTER;
    m_compatible_printers.key_list		= "compatible_printers";
    m_compatible_printers.key_condition	= "compatible_printers_condition";
    m_compatible_printers.dialog_title  = _L("Compatible printers");
    m_compatible_printers.dialog_label  = _L("Select the printers this profile is compatible with.");

    m_compatible_prints.type			= Preset::TYPE_FFF_PRINT;
    m_compatible_prints.key_list 		= "compatible_prints";
    m_compatible_prints.key_condition	= "compatible_prints_condition";
    m_compatible_prints.dialog_title 	= _L("Compatible print profiles");
    m_compatible_prints.dialog_label 	= _L("Select the print profiles this profile is compatible with.");

    wxGetApp().tabs_list.push_back(this);

    m_em_unit = em_unit(m_parent); //wxGetApp().em_unit();

    m_config_manipulation = get_config_manipulation();

    Bind(wxEVT_SIZE, ([](wxSizeEvent &evt) {
        //for (auto page : m_pages)
        //    if (! page.get()->IsShown())
        //        page->layout_valid = false;
        evt.Skip();
    }));

    m_highlighter.set_timer_owner(this, 0);
    this->Bind(wxEVT_TIMER, [this](wxTimerEvent&)
    {
        m_highlighter.blink();
    });
    
    std::string tab_key = Preset::type_name(type);
    try {
        m_script_exec.init(tab_key, this);
    }
    catch (ScriptError ex) {
        m_script_exec.disable();
        BOOST_LOG_TRIVIAL(error) << format("An error has occured when compiling %1%/%2%.as ; The scripted widgets for this tab won't be built.", Slic3r::GUI::get_app_config()->layout_config_path().string(), tab_key);
    }
}

// sub new
void Tab::create_preset_tab()
{
#ifdef __WINDOWS__
    SetDoubleBuffered(true);
#endif //__WINDOWS__

    m_preset_bundle = wxGetApp().preset_bundle;

    // Vertical sizer to hold the choice menu and the rest of the page.
#ifdef __WXOSX__
    auto  *main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->SetSizeHints(this);
    this->SetSizer(main_sizer);

    // Create additional panel to Fit() it from OnActivate()
    // It's needed for tooltip showing on OSX
    m_tmp_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBK_LEFT | wxTAB_TRAVERSAL);
    auto panel = m_tmp_panel;
    auto  sizer = new wxBoxSizer(wxVERTICAL);
    m_tmp_panel->SetSizer(sizer);
    m_tmp_panel->Layout();

    main_sizer->Add(m_tmp_panel, 1, wxEXPAND | wxALL, 0);
#else
    Tab *panel = this;
    auto  *sizer = new wxBoxSizer(wxVERTICAL);
    sizer->SetSizeHints(panel);
    panel->SetSizer(sizer);
#endif //__WXOSX__

    // preset chooser
    m_presets_choice = new TabPresetComboBox(panel, m_type);
    m_presets_choice->set_selection_changed_function([this](int selection) {
        if (!m_presets_choice->selection_is_changed_according_to_physical_printers())
        {
            if (m_type == Preset::TYPE_PRINTER && !m_presets_choice->is_selected_physical_printer())
                m_preset_bundle->physical_printers.unselect_printer();

            // select preset
            std::string preset_name = m_presets_choice->GetString(selection).ToUTF8().data();
            select_preset(Preset::remove_suffix_modified(preset_name));
        }
    });

    auto color = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);

    //buttons
    m_scaled_buttons.reserve(6);
    m_scaled_buttons.reserve(2);

    add_scaled_button(panel, &m_btn_compare_preset, "compare");
    add_scaled_button(panel, &m_btn_save_preset, "save");
    add_scaled_button(panel, &m_btn_delete_preset, "cross");
    if (m_type == Preset::Type::TYPE_PRINTER)
        add_scaled_button(panel, &m_btn_edit_ph_printer, "cog");

    m_show_incompatible_presets = false;
    add_scaled_bitmap(this, m_bmp_show_incompatible_presets, "flag_red");
    add_scaled_bitmap(this, m_bmp_hide_incompatible_presets, "flag_green");

    add_scaled_button(panel, &m_btn_hide_incompatible_presets, m_bmp_hide_incompatible_presets.name());

    m_btn_compare_preset->SetToolTip(_L("Compare this preset with some another"));
    // TRN "Save current Settings"
    m_btn_save_preset->SetToolTip(from_u8((boost::format(_utf8(L("Save current %s"))) % m_title).str()));
    m_btn_delete_preset->SetToolTip(_(L("Delete this preset")));
    m_btn_delete_preset->Hide();

    add_scaled_button(panel, &m_question_btn, "question");
    m_question_btn->SetToolTip(_(L("Hover the cursor over buttons to find more information \n"
                                   "or click this button.")));

    add_scaled_button(panel, &m_search_btn, "search");
    m_search_btn->SetToolTip(format_wxstr(_L("Search in settings [%1%]"), "Ctrl+F"));

    // Bitmaps to be shown on the "Revert to system" aka "Lock to system" button next to each input field.
    add_scaled_bitmap(this, m_bmp_value_lock  , "lock_closed");
    add_scaled_bitmap(this, m_bmp_value_unlock, "lock_open");
    m_bmp_non_system = &m_bmp_white_bullet;
    // Bitmaps to be shown on the "Undo user changes" button next to each input field.
    add_scaled_bitmap(this, m_bmp_value_revert, "undo");
    add_scaled_bitmap(this, m_bmp_white_bullet, "dot");

    fill_icon_descriptions();
    set_tooltips_text();

    add_scaled_button(panel, &m_undo_btn,        m_bmp_white_bullet.name());
    add_scaled_button(panel, &m_undo_to_sys_btn, m_bmp_white_bullet.name());

    m_undo_btn->Bind(wxEVT_BUTTON, ([this](wxCommandEvent) { on_roll_back_value(); }));
    m_undo_to_sys_btn->Bind(wxEVT_BUTTON, ([this](wxCommandEvent) { on_roll_back_value(true); }));
    m_question_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent) {
        ButtonsDescription dlg(this, m_icon_descriptions);
        if (dlg.ShowModal() == wxID_OK)
            wxGetApp().update_label_colours();
    });
    m_search_btn->Bind(wxEVT_BUTTON, [](wxCommandEvent) { wxGetApp().plater()->search(false); });

    // Colors for ui "decoration"
    m_sys_label_clr			= wxGetApp().get_label_clr_sys();
    m_modified_label_clr	= wxGetApp().get_label_clr_modified();
    m_default_label_clr     = wxGetApp().get_label_clr_default();
    m_phony_label_clr       = wxGetApp().get_label_clr_phony();

#ifdef _USE_CUSTOM_NOTEBOOK
    // Sizer with buttons for mode changing
    if (wxGetApp().tabs_as_menu())
#endif
        m_mode_sizer = nullptr;//new ModeSizer(panel, int (0.5*em_unit(this)));

    const float scale_factor = /*wxGetApp().*/em_unit(this)*0.1;// GetContentScaleFactor();
    m_hsizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add(m_hsizer, 0, wxEXPAND | wxBOTTOM, 3);
    m_hsizer->Add(m_presets_choice, 0, wxLEFT | wxRIGHT | wxTOP | wxALIGN_CENTER_VERTICAL, 3);
    m_hsizer->AddSpacer(int(4*scale_factor));
    m_hsizer->Add(m_btn_save_preset, 0, wxALIGN_CENTER_VERTICAL);
    m_hsizer->AddSpacer(int(4 * scale_factor));
    m_hsizer->Add(m_btn_delete_preset, 0, wxALIGN_CENTER_VERTICAL);
    if (m_btn_edit_ph_printer) {
        m_hsizer->AddSpacer(int(4 * scale_factor));
        m_hsizer->Add(m_btn_edit_ph_printer, 0, wxALIGN_CENTER_VERTICAL);
    }
    m_hsizer->AddSpacer(int(/*16*/8 * scale_factor));
    m_hsizer->Add(m_btn_hide_incompatible_presets, 0, wxALIGN_CENTER_VERTICAL);
    m_hsizer->AddSpacer(int(8 * scale_factor));
    m_hsizer->Add(m_question_btn, 0, wxALIGN_CENTER_VERTICAL);
    m_hsizer->AddSpacer(int(32 * scale_factor));
    m_hsizer->Add(m_undo_to_sys_btn, 0, wxALIGN_CENTER_VERTICAL);
    m_hsizer->Add(m_undo_btn, 0, wxALIGN_CENTER_VERTICAL);
    m_hsizer->AddSpacer(int(32 * scale_factor));
    m_hsizer->Add(m_search_btn, 0, wxALIGN_CENTER_VERTICAL);
    m_hsizer->AddSpacer(int(8*scale_factor));
    m_hsizer->Add(m_btn_compare_preset, 0, wxALIGN_CENTER_VERTICAL);
    m_hsizer->AddSpacer(int(16*scale_factor));
    // m_hsizer->AddStretchSpacer(32);
    // StretchSpacer has a strange behavior under OSX, so
    // There is used just additional sizer for m_mode_sizer with right alignment
    if (m_mode_sizer) {
    auto mode_sizer = new wxBoxSizer(wxVERTICAL);
        // Don't set the 2nd parameter to 1, making the sizer rubbery scalable in Y axis may lead 
        // to wrong vertical size assigned to wxBitmapComboBoxes, see GH issue #7176.
        mode_sizer->Add(m_mode_sizer, 0, wxALIGN_RIGHT);
    m_hsizer->Add(mode_sizer, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, wxOSX ? 15 : 10);
    }

    //Horizontal sizer to hold the tree and the selected page.
    m_hsizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add(m_hsizer, 1, wxEXPAND, 0);

    //left vertical sizer
    m_left_sizer = new wxBoxSizer(wxVERTICAL);
    m_hsizer->Add(m_left_sizer, 0, wxEXPAND | wxLEFT | wxTOP | wxBOTTOM, 3);

    // tree
    m_treectrl = new wxTreeCtrl(panel, wxID_ANY, wxDefaultPosition, wxSize(20 * m_em_unit, -1),
        wxTR_NO_BUTTONS | wxTR_HIDE_ROOT | wxTR_SINGLE | wxTR_NO_LINES | wxBORDER_SUNKEN | wxWANTS_CHARS);
    m_left_sizer->Add(m_treectrl, 1, wxEXPAND);
    const int img_sz = int(16 * scale_factor + 0.5f);
    m_icons = new wxImageList(img_sz, img_sz, true, 1);
    // Index of the last icon inserted into $self->{icons}.
    m_icon_count = -1;
    m_treectrl->AssignImageList(m_icons);
    m_treectrl->AddRoot("root");
    m_treectrl->SetIndent(0);
    wxGetApp().UpdateDarkUI(m_treectrl);

    // Delay processing of the following handler until the message queue is flushed.
    // This helps to process all the cursor key events on Windows in the tree control,
    // so that the cursor jumps to the last item.
    m_treectrl->Bind(wxEVT_TREE_SEL_CHANGED, [this](wxTreeEvent&) {
#ifdef __linux__
        // Events queue is opposite On Linux. wxEVT_SET_FOCUS invokes after wxEVT_TREE_SEL_CHANGED,
        // and a result wxEVT_KILL_FOCUS doesn't invoke for the TextCtrls.
        // see https://github.com/prusa3d/PrusaSlicer/issues/5720
        // So, call SetFocus explicitly for this control before changing of the selection
        m_treectrl->SetFocus();
#endif
            if (!m_disable_tree_sel_changed_event && !m_pages.empty()) {
                if (m_page_switch_running)
                    m_page_switch_planned = true;
                else {
                    m_page_switch_running = true;
                    do {
                        m_page_switch_planned = false;
                        m_treectrl->Update();
                    } while (this->tree_sel_change_delayed());
                    m_page_switch_running = false;
            }
        }
        });

    m_treectrl->Bind(wxEVT_KEY_DOWN, &Tab::OnKeyDown, this);

    // Initialize the page.
#ifdef __WXOSX__
    auto page_parent = m_tmp_panel;
#else
    auto page_parent = this;
#endif

    m_page_view = new wxScrolledWindow(page_parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_page_sizer = new wxBoxSizer(wxVERTICAL);
    m_page_view->SetSizer(m_page_sizer);
    m_page_view->SetScrollbars(1, 20, 1, 2);
    m_hsizer->Add(m_page_view, 1, wxEXPAND | wxLEFT, 5);

    m_btn_compare_preset->Bind(wxEVT_BUTTON, ([this](wxCommandEvent e) { compare_preset(); }));
    m_btn_save_preset->Bind(wxEVT_BUTTON, ([this](wxCommandEvent e) { save_preset(); }));
    m_btn_delete_preset->Bind(wxEVT_BUTTON, ([this](wxCommandEvent e) { delete_preset(); }));
    m_btn_hide_incompatible_presets->Bind(wxEVT_BUTTON, ([this](wxCommandEvent e) {
        toggle_show_hide_incompatible();
    }));

    if (m_btn_edit_ph_printer)
        m_btn_edit_ph_printer->Bind(wxEVT_BUTTON, [this](wxCommandEvent e) {
            // ask for saving modif before
            if (m_presets->current_is_dirty()) {
                //ok = may_discard_current_dirty_preset(nullptr, "");
                UnsavedChangesDialog dlg(Preset::Type::TYPE_PRINTER, m_presets, "");
                if (dlg.ShowModal() == wxID_CANCEL)
                    return;

                if (dlg.save_preset())  // save selected changes
                {
                    const std::vector<std::string>& unselected_options = dlg.get_unselected_options(Preset::Type::TYPE_PRINTER);
                    const std::string& name = dlg.get_preset_name();

                    // revert unselected options to the old values
                    m_presets->get_edited_preset().config.apply_only(m_presets->get_selected_preset().config, unselected_options);
                    save_preset(name);

                    for (const std::pair<std::string, Preset::Type>& nt : dlg.get_names_and_types())
                        m_preset_bundle->save_changes_for_preset(nt.first, nt.second, dlg.get_unselected_options(nt.second));

                    // if we saved changes to the new presets, we should to 
                    // synchronize config.ini with the current selections.
                    m_preset_bundle->export_selections(*wxGetApp().app_config);
                } else {
                    // discard all changes
                    m_presets->discard_current_changes();
                }
            }
            if (m_preset_bundle->physical_printers.has_selection())
                m_presets_choice->edit_physical_printer();
            else
                m_presets_choice->add_physical_printer();
        });

    // Initialize the DynamicPrintConfig by default keys/values.
    build();

    // ys_FIXME: Following should not be needed, the function will be called later
    // (update_mode->update_visibility->rebuild_page_tree). This does not work, during the
    // second call of rebuild_page_tree m_treectrl->GetFirstVisibleItem(); returns zero
    // for some unknown reason (and the page is not refreshed until user does a selection).
    rebuild_page_tree();

    m_completed = true;
}

void Tab::add_scaled_button(wxWindow* parent,
                            ScalableButton** btn,
                            const std::string& icon_name,
                            const wxString& label/* = wxEmptyString*/,
                            long style /*= wxBU_EXACTFIT | wxNO_BORDER*/)
{
    *btn = new ScalableButton(parent, wxID_ANY, icon_name, label, wxDefaultSize, wxDefaultPosition, style, true);
    m_scaled_buttons.push_back(*btn);
}

void Tab::add_scaled_bitmap(wxWindow* parent,
                            ScalableBitmap& bmp,
                            const std::string& icon_name)
{
    bmp = ScalableBitmap(parent, icon_name);
    m_scaled_bitmaps.push_back(&bmp);
}

void Tab::load_initial_data()
{
    m_config = &m_presets->get_edited_preset().config;
    bool has_parent = m_presets->get_selected_preset_parent() != nullptr;
    m_bmp_non_system = has_parent ? &m_bmp_value_unlock : &m_bmp_white_bullet;
    m_ttg_non_system = has_parent ? &m_ttg_value_unlock : &m_ttg_white_bullet_ns;
    m_tt_non_system  = has_parent ? &m_tt_value_unlock  : &m_ttg_white_bullet_ns;
    m_tt_non_system_script = has_parent ? &m_tt_value_unlock_script : &m_ttg_white_bullet_ns;
}

Slic3r::GUI::PageShp Tab::create_options_page(const wxString& title, const std::string& icon)
{
    // Index of icon in an icon list $self->{icons}.
    auto icon_idx = 0;
    if (!icon.empty()) {
        icon_idx = (m_icon_index.find(icon) == m_icon_index.end()) ? -1 : m_icon_index.at(icon);
        if (icon_idx == -1) {
            // Add a new icon to the icon list.
            m_scaled_icons_list.push_back(ScalableBitmap(this, icon));
            m_icons->Add(m_scaled_icons_list.back().bmp());
            icon_idx = ++m_icon_count;
            m_icon_index[icon] = icon_idx;
        }

        if (m_category_icon.find(title) == m_category_icon.end()) {
            // Add new category to the category_to_icon list.
            m_category_icon[title] = icon;
    }
    }
    // Initialize the page.
    PageShp page(new Page(m_page_view, title, icon_idx));
//	page->SetBackgroundStyle(wxBG_STYLE_SYSTEM);
#ifdef __WINDOWS__
//	page->SetDoubleBuffered(true);
#endif //__WINDOWS__

    page->set_config(m_config);
    return page;
}

// Names of categories is save in English always. We translate them only for UI.
// But category "Extruder n" can't be translated regularly (using _()), so
// just for this category we should splite the title and translate "Extruder" word separately
wxString Tab::translate_category(const wxString& title, Preset::Type preset_type)
{
    if (preset_type == Preset::TYPE_PRINTER && title.Contains("Extruder ")) {
        return _("Extruder") + title.SubString(8, title.Last());
    }
    return _(title);
}

void Tab::OnActivate()
{
    wxWindowUpdateLocker noUpdates(this);
#ifdef __WXOSX__
//    wxWindowUpdateLocker noUpdates(this);
    auto size = GetSizer()->GetSize();
    m_tmp_panel->GetSizer()->SetMinSize(size.x + m_size_move, size.y);
    Fit();
    m_size_move *= -1;
#endif // __WXOSX__

#ifdef __WXMSW__
    // Workaround for tooltips over Tree Controls displayed over excessively long
    // tree control items, stealing the window focus.
    //
    // In case the Tab was reparented from the MainFrame to the floating dialog,
    // the tooltip created by the Tree Control before reparenting is not reparented, 
    // but it still points to the MainFrame. If the tooltip pops up, the MainFrame 
    // is incorrectly focussed, stealing focus from the floating dialog.
    //
    // The workaround is to delete the tooltip control.
    // Vojtech tried to reparent the tooltip control, but it did not work,
    // and if the Tab was later reparented back to MainFrame, the tooltip was displayed
    // at an incorrect position, therefore it is safer to just discard the tooltip control
    // altogether.
    HWND hwnd_tt = TreeView_GetToolTips(m_treectrl->GetHandle());
    if (hwnd_tt) {
        HWND hwnd_toplevel 	= find_toplevel_parent(m_treectrl)->GetHandle();
        HWND hwnd_parent 	= ::GetParent(hwnd_tt);
        if (hwnd_parent != hwnd_toplevel) {
            ::DestroyWindow(hwnd_tt);
            TreeView_SetToolTips(m_treectrl->GetHandle(), nullptr);
        }
    }
#endif

    // create controls on active page
    activate_selected_page([](){});
    m_hsizer->Layout();

#ifdef _USE_CUSTOM_NOTEBOOK
    // Because of DarkMode we use our own Notebook (inherited from wxSiplebook) instead of wxNotebook
    // And it looks like first Layout of the page doesn't update a size of the m_presets_choice
    // So we have to set correct size explicitely
    if (wxSize ok_sz = wxSize(35 * m_em_unit, m_presets_choice->GetBestSize().y);
        ok_sz != m_presets_choice->GetSize()) {
        m_presets_choice->SetMinSize(ok_sz);
        m_presets_choice->SetSize(ok_sz);
        GetSizer()->GetItem(size_t(0))->GetSizer()->Layout();
        if (wxGetApp().tabs_as_menu())
            m_presets_choice->update();
    }
#endif // _USE_CUSTOM_NOTEBOOK
    Refresh();
}

void Tab::update_label_colours()
{
    if (m_sys_label_clr == wxGetApp().get_label_clr_sys() 
        && m_modified_label_clr == wxGetApp().get_label_clr_modified()
        && m_default_label_clr == wxGetApp().get_label_clr_default()
        && m_phony_label_clr == wxGetApp().get_label_clr_phony())
        return;
    m_default_label_clr = wxGetApp().get_label_clr_default();
    m_sys_label_clr = wxGetApp().get_label_clr_sys();
    m_modified_label_clr = wxGetApp().get_label_clr_modified();
    m_phony_label_clr = wxGetApp().get_label_clr_phony();

    //update options "decoration"
    for (const std::pair<std::string, int> &opt : m_options_list)
    {
        const wxColour *color = &m_sys_label_clr;

        // value isn't equal to system value
        if ((opt.second & osSystemValue) == 0) {
            // value is equal to last saved
            if ((opt.second & osInitValue) != 0)
                color = &m_default_label_clr;
            // value is modified
            else
                color = &m_modified_label_clr;
        }
        if ((opt.second & osCurrentPhony) != 0)
            color = &m_phony_label_clr;
        else {
            if ((opt.second & osInitPhony) != 0)
                color = &m_modified_label_clr;
            else if ((opt.second & osSystemPhony) != 0)
                color = &m_default_label_clr;
        }

        if (OptionsGroup::is_option_without_field(opt.first)) {
            if (m_colored_Label_colors.find(opt.first) != m_colored_Label_colors.end())
                m_colored_Label_colors.at(opt.first) = *color;
            continue;
        }

        Field* field = get_field(opt.first);
        if (field == nullptr) continue;
        field->set_label_colour(color);
    }

    auto cur_item = m_treectrl->GetFirstVisibleItem();
    if (!cur_item || !m_treectrl->IsVisible(cur_item))
        return;
    while (cur_item) {
        auto title = m_treectrl->GetItemText(cur_item);
        for (auto page : m_pages)
        {
            if (translate_category(page->title(), m_type) != title)
                continue;

            const wxColor *clr = !page->m_is_nonsys_values ? &m_sys_label_clr :
                page->m_is_modified_values ? &m_modified_label_clr :
                &m_default_label_clr;

            m_treectrl->SetItemTextColour(cur_item, *clr);
            break;
        }
        cur_item = m_treectrl->GetNextVisible(cur_item);
    }

    decorate();
}

void Tab::decorate()
{
    for (const auto& opt : m_options_list)
    {
        Field*          field = nullptr;
        wxColour*   colored_label_clr = nullptr;

        if (OptionsGroup::is_option_without_field(opt.first))
            colored_label_clr = (m_colored_Label_colors.find(opt.first) == m_colored_Label_colors.end()) ? nullptr : &m_colored_Label_colors.at(opt.first);

        if (!colored_label_clr) {
            field = get_field(opt.first);
            if (!field)
                continue;
        }

        bool is_nonsys_value = false;
        bool is_modified_value = true;
        const ScalableBitmap* sys_icon  = &m_bmp_value_lock;
        const ScalableBitmap* icon      = &m_bmp_value_revert;

        const wxColour* color = m_is_default_preset ? &m_default_label_clr : &m_sys_label_clr;

        const wxString* sys_tt  = &m_tt_value_lock;
        const wxString* tt      = &m_tt_value_revert;

        // value isn't equal to system value
        if ((opt.second & osSystemValue) == 0) {
            is_nonsys_value = true;
            sys_icon = m_bmp_non_system;
            sys_tt = m_tt_non_system;
            // value is equal to last saved
            if ((opt.second & osInitValue) != 0)
                color = &m_default_label_clr;
            // value is modified
            else
                color = &m_modified_label_clr;
        }
        if ((opt.second & osInitValue) != 0)
        {
            is_modified_value = false;
            icon = &m_bmp_white_bullet;
            tt = &m_tt_white_bullet;
        }

        //color for phony things
        if ((opt.second & osCurrentPhony) != 0)
            color = &m_phony_label_clr;
        else {
            if ((opt.second & osInitPhony) != 0)
                color = &m_modified_label_clr;
            else if ((opt.second & osSystemPhony) != 0)
                color = &m_default_label_clr;
        }

        if (colored_label_clr) {
            *colored_label_clr = *color;
            continue;
        }
        
        field->m_is_nonsys_value = is_nonsys_value;
        field->m_is_modified_value = is_modified_value;
        field->set_undo_bitmap(icon);
        field->set_undo_to_sys_bitmap(sys_icon);
        field->set_undo_tooltip(tt);
        field->set_undo_to_sys_tooltip(sys_tt);
        field->set_label_colour(color);
    }
    for (const auto& opt_key2id : this->m_options_script) {
        Field* field = get_field(opt_key2id.first);
        if (!field)
            continue;

        bool is_nonsys_value = false;
        bool is_modified_value = true;
        const ScalableBitmap* sys_icon = &m_bmp_value_lock;
        const ScalableBitmap* icon = &m_bmp_value_revert;

        const wxColour* color = m_is_default_preset ? &m_default_label_clr : &m_sys_label_clr;

        const wxString* sys_tt = &m_tt_value_lock_script;
        const wxString* tt = &m_tt_value_revert_script;

        //get the values of the other ones
        bool is_not_sys = false;
        bool is_not_initial = false;
        for (const std::string &dep : field->m_opt.depends_on) {
            const auto& it = m_options_list.find(dep);
            if (it != m_options_list.end()) {
                is_not_sys |= ((it->second & osSystemValue) == 0);
                is_not_initial |= ((it->second & osInitValue) == 0);
            }
        }

        // value isn't equal to system value
        if (is_not_sys) {
            is_nonsys_value = true;
            sys_icon = m_bmp_non_system;
            sys_tt = m_tt_non_system_script;
            // value is equal to last saved
            if (!is_not_initial)
                color = &m_default_label_clr;
            // value is modified
            else
                color = &m_modified_label_clr;
        }
        if (!is_not_initial)
        {
            is_modified_value = false;
            icon = &m_bmp_white_bullet;
            tt = &m_tt_white_bullet_script;
        }

        field->m_is_nonsys_value = is_nonsys_value;
        field->m_is_modified_value = is_modified_value;
        field->set_undo_bitmap(icon);
        field->set_undo_to_sys_bitmap(sys_icon);
        field->set_undo_tooltip(tt);
        field->set_undo_to_sys_tooltip(sys_tt);
        field->set_label_colour(color);
    }

    if (m_active_page)
        m_active_page->refresh();
}

// Update UI according to changes
void Tab::update_changed_ui()
{
    if (m_postpone_update_ui)
        return;

    const bool deep_compare = (m_type == Slic3r::Preset::TYPE_PRINTER || m_type == Slic3r::Preset::TYPE_SLA_MATERIAL);
    auto dirty_options = m_presets->current_dirty_options(deep_compare);
    auto nonsys_options = m_presets->current_different_from_parent_options(deep_compare);
    if (m_type == Preset::TYPE_PRINTER && static_cast<TabPrinter*>(this)->m_printer_technology == ptFFF) {
        TabPrinter* tab = static_cast<TabPrinter*>(this);
        if (tab->m_initial_extruders_count != tab->m_extruders_count)
            dirty_options.emplace_back("extruders_count");
        if (tab->m_sys_extruders_count != tab->m_extruders_count)
            nonsys_options.emplace_back("extruders_count");
        if (tab->m_initial_milling_count != tab->m_milling_count)
            dirty_options.emplace_back("milling_count");
        if (tab->m_sys_milling_count != tab->m_milling_count)
            nonsys_options.emplace_back("milling_count");
    }

    for (auto& it : m_options_list)
        it.second = m_opt_status_value;

    dirty_options.insert(dirty_options.end(), m_options_dirty.begin(), m_options_dirty.end());
    m_options_dirty.clear();

    const Preset& edited_preset   = m_presets->get_edited_preset();
    const Preset& selected_preset = m_presets->get_selected_preset();
    const Preset* system_preset   = m_presets->get_selected_preset_parent();
    for (auto& opt_key : m_presets->get_edited_preset().config.keys()) {
        if (edited_preset.config.option(opt_key)->is_phony())
            //ensure that osCurrentPhony is in the bitmask 
            m_options_list[opt_key] |= osCurrentPhony;
        if (selected_preset.config.option(opt_key) && selected_preset.config.option(opt_key)->is_phony())
            m_options_list[opt_key] |= osInitPhony;
        if (system_preset && system_preset->config.option(opt_key) && system_preset->config.option(opt_key)->is_phony())
            m_options_list[opt_key] |= osSystemPhony;
    }

    //don't let option that were phony be resetable.
    for (auto opt_key : dirty_options)
        if( (m_options_list[opt_key] & osInitPhony) == 0)
            //ensure that osInitValue is not in the bitmask 
            m_options_list[opt_key] &= ~osInitValue;
    for (auto opt_key : nonsys_options)
        if ((m_options_list[opt_key] & osSystemPhony) == 0)
            m_options_list[opt_key] &= ~osSystemValue;

    decorate();

    wxTheApp->CallAfter([this]() {
        if (parent()) //To avoid a crash, parent should be exist for a moment of a tree updating
            update_changed_tree_ui();
    });
}

void Tab::init_options_list()
{
    if (!m_options_list.empty())
        m_options_list.clear();

    for (const std::string& opt_key : m_config->keys())
        m_options_list.emplace(opt_key, m_opt_status_value);
}

template<class T>
void add_correct_opts_to_options_list(const std::string &opt_key, std::map<std::string, int>& map, Tab *tab, const int& value)
{
    T *opt_cur = static_cast<T*>(tab->m_config->option(opt_key));
    for (size_t i = 0; i < opt_cur->values.size(); i++)
        map.emplace(opt_key + "#" + std::to_string(i), value);
}

void TabPrinter::init_options_list()
{
    if (!m_options_list.empty())
        m_options_list.clear();

    for (const std::string& opt_key : m_config->keys())
    {
        if (opt_key == "bed_shape") {
            m_options_list.emplace(opt_key, m_opt_status_value);
            continue;
        }
        switch (m_config->option(opt_key)->type())
        {
        case coInts:	add_correct_opts_to_options_list<ConfigOptionInts		>(opt_key, m_options_list, this, m_opt_status_value);	break;
        case coBools:	add_correct_opts_to_options_list<ConfigOptionBools		>(opt_key, m_options_list, this, m_opt_status_value);	break;
        case coFloats:	add_correct_opts_to_options_list<ConfigOptionFloats		>(opt_key, m_options_list, this, m_opt_status_value);	break;
        case coStrings:	add_correct_opts_to_options_list<ConfigOptionStrings	>(opt_key, m_options_list, this, m_opt_status_value);	break;
        case coPercents:add_correct_opts_to_options_list<ConfigOptionPercents	>(opt_key, m_options_list, this, m_opt_status_value);	break;
        case coFloatsOrPercents:add_correct_opts_to_options_list<ConfigOptionFloatsOrPercents>(opt_key, m_options_list, this, m_opt_status_value);	break;
        case coPoints:	add_correct_opts_to_options_list<ConfigOptionPoints		>(opt_key, m_options_list, this, m_opt_status_value);	break;
        default:		m_options_list.emplace(opt_key, m_opt_status_value);		break;
        }
    }
    if (m_printer_technology == ptFFF)
        m_options_list.emplace("extruders_count", m_opt_status_value);
}

void TabPrinter::msw_rescale()
{
    Tab::msw_rescale();

    if (m_reset_to_filament_color)
        m_reset_to_filament_color->msw_rescale();

    Layout();
}

void TabSLAMaterial::init_options_list()
{
    if (!m_options_list.empty())
        m_options_list.clear();

    for (const std::string& opt_key : m_config->keys())
    {
        if (opt_key == "compatible_prints" || opt_key == "compatible_printers") {
            m_options_list.emplace(opt_key, m_opt_status_value);
            continue;
        }
        switch (m_config->option(opt_key)->type())
        {
        case coInts:	add_correct_opts_to_options_list<ConfigOptionInts		>(opt_key, m_options_list, this, m_opt_status_value);	break;
        case coBools:	add_correct_opts_to_options_list<ConfigOptionBools		>(opt_key, m_options_list, this, m_opt_status_value);	break;
        case coFloats:	add_correct_opts_to_options_list<ConfigOptionFloats		>(opt_key, m_options_list, this, m_opt_status_value);	break;
        case coStrings:	add_correct_opts_to_options_list<ConfigOptionStrings	>(opt_key, m_options_list, this, m_opt_status_value);	break;
        case coPercents:add_correct_opts_to_options_list<ConfigOptionPercents	>(opt_key, m_options_list, this, m_opt_status_value);	break;
        case coFloatsOrPercents:add_correct_opts_to_options_list<ConfigOptionFloatsOrPercents	>(opt_key, m_options_list, this, m_opt_status_value);	break;
        case coPoints:	add_correct_opts_to_options_list<ConfigOptionPoints		>(opt_key, m_options_list, this, m_opt_status_value);	break;
        default:		m_options_list.emplace(opt_key, m_opt_status_value);		break;
        }
    }
}

void Tab::get_sys_and_mod_flags(const std::string& opt_key, bool& sys_page, bool& modified_page)
{
    auto opt = m_options_list.find(opt_key);
    if (opt == m_options_list.end()) 
        return;

    if (sys_page) sys_page = (opt->second & osSystemValue) != 0;
    modified_page |= (opt->second & osInitValue) == 0;
}

void Tab::update_changed_tree_ui()
{
    if (m_options_list.empty())
        return;
    auto cur_item = m_treectrl->GetFirstVisibleItem();
    if (!cur_item || !m_treectrl->IsVisible(cur_item))
        return;

    auto selected_item = m_treectrl->GetSelection();
    auto selection = selected_item ? m_treectrl->GetItemText(selected_item) : "";

    while (cur_item) {
        auto title = m_treectrl->GetItemText(cur_item);
        for (auto page : m_pages)
        {
            if (translate_category(page->title(), m_type) != title)
                continue;
            bool sys_page = true;
            bool modified_page = false;
            if (page->title() == "General") {
                std::initializer_list<const char*> optional_keys{ "extruders_count", "bed_shape" };
                for (auto &opt_key : optional_keys) {
                    get_sys_and_mod_flags(opt_key, sys_page, modified_page);
                }
            }
            if (m_type == Preset::TYPE_FFF_FILAMENT && page->title() == "Advanced") {
                get_sys_and_mod_flags("filament_ramming_parameters", sys_page, modified_page);
            }
            if (page->title() == "Dependencies") {
                if (m_type == Slic3r::Preset::TYPE_PRINTER) {
                    sys_page = m_presets->get_selected_preset_parent() != nullptr;
                    modified_page = false;
                } else {
                    if (m_type == Slic3r::Preset::TYPE_FFF_FILAMENT || m_type == Slic3r::Preset::TYPE_SLA_MATERIAL)
                        get_sys_and_mod_flags("compatible_prints", sys_page, modified_page);
                    get_sys_and_mod_flags("compatible_printers", sys_page, modified_page);
                }
            }
            for (auto group : page->m_optgroups)
            {
                if (!sys_page && modified_page)
                    break;
                for (const auto &kvp : group->opt_map()) {
                    const std::string& opt_key = kvp.first;
                    get_sys_and_mod_flags(opt_key, sys_page, modified_page);
                }
            }

            const wxColor *clr = sys_page		?	(m_is_default_preset ? &m_default_label_clr : &m_sys_label_clr) :
                                 modified_page	?	&m_modified_label_clr :
                                                    &m_default_label_clr;

            if (page->set_item_colour(clr))
                m_treectrl->SetItemTextColour(cur_item, *clr);

            page->m_is_nonsys_values = !sys_page;
            page->m_is_modified_values = modified_page;

            if (selection == title) {
                m_is_nonsys_values = page->m_is_nonsys_values;
                m_is_modified_values = page->m_is_modified_values;
            }
            break;
        }
        auto next_item = m_treectrl->GetNextVisible(cur_item);
        cur_item = next_item;
    }
    update_undo_buttons();
}

void Tab::update_undo_buttons()
{
    m_undo_btn->        SetBitmap_(m_is_modified_values ? m_bmp_value_revert: m_bmp_white_bullet);
    m_undo_to_sys_btn-> SetBitmap_(m_is_nonsys_values   ? *m_bmp_non_system : m_bmp_value_lock);

    m_undo_btn->SetToolTip(m_is_modified_values ? m_ttg_value_revert : m_ttg_white_bullet);
    m_undo_to_sys_btn->SetToolTip(m_is_nonsys_values ? *m_ttg_non_system : m_ttg_value_lock);
}

void Tab::on_roll_back_value(const bool to_sys /*= true*/)
{
    if (!m_active_page) return;

    int os;
    if (to_sys)	{
        if (!m_is_nonsys_values) return;
        os = osSystemValue;
    }
    else {
        if (!m_is_modified_values) return;
        os = osInitValue;
    }

    m_postpone_update_ui = true;

    for (auto group : m_active_page->m_optgroups) {
        if (group->title == "Capabilities") {
            if ((m_options_list["extruders_count"] & os) == 0)
                to_sys ? group->back_to_sys_value("extruders_count") : group->back_to_initial_value("extruders_count");
        }
        if (group->title == "Size and coordinates") {
            if ((m_options_list["bed_shape"] & os) == 0) {
                to_sys ? group->back_to_sys_value("bed_shape") : group->back_to_initial_value("bed_shape");
                load_key_value("bed_shape", true/*some value*/, true);
            }
        }
        if (group->title == "Toolchange parameters with single extruder MM printers") {
            if ((m_options_list["filament_ramming_parameters"] & os) == 0)
                to_sys ? group->back_to_sys_value("filament_ramming_parameters") : group->back_to_initial_value("filament_ramming_parameters");
        }
        if (group->title == "G-code Substitutions") {
            if ((m_options_list["gcode_substitutions"] & os) == 0) {
                to_sys ? group->back_to_sys_value("gcode_substitutions") : group->back_to_initial_value("gcode_substitutions");
                load_key_value("gcode_substitutions", true/*some value*/, true);
            }
        }
        if (group->title == "Profile dependencies") {
            // "compatible_printers" option doesn't exists in Printer Settimgs Tab
            if (m_type != Preset::TYPE_PRINTER && (m_options_list["compatible_printers"] & os) == 0) {
                to_sys ? group->back_to_sys_value("compatible_printers") : group->back_to_initial_value("compatible_printers");
                load_key_value("compatible_printers", true/*some value*/, true);

                bool is_empty = m_config->option<ConfigOptionStrings>("compatible_printers")->values.empty();
                m_compatible_printers.checkbox->SetValue(is_empty);
                is_empty ? m_compatible_printers.btn->Disable() : m_compatible_printers.btn->Enable();
            }
            // "compatible_prints" option exists only in Filament Settimgs and Materials Tabs
            if ((m_type == Preset::TYPE_FFF_FILAMENT || m_type == Preset::TYPE_SLA_MATERIAL) && (m_options_list["compatible_prints"] & os) == 0) {
                to_sys ? group->back_to_sys_value("compatible_prints") : group->back_to_initial_value("compatible_prints");
                load_key_value("compatible_prints", true/*some value*/, true);

                bool is_empty = m_config->option<ConfigOptionStrings>("compatible_prints")->values.empty();
                m_compatible_prints.checkbox->SetValue(is_empty);
                is_empty ? m_compatible_prints.btn->Disable() : m_compatible_prints.btn->Enable();
            }
        }
        for (const auto &kvp : group->opt_map()) {
            const std::string& opt_key = kvp.first;
            if ((m_options_list[opt_key] & os) == 0)
                to_sys ? group->back_to_sys_value(opt_key) : group->back_to_initial_value(opt_key);
        }
    }

    m_postpone_update_ui = false;

    // When all values are rolled, then we hane to update whole tab in respect to the reverted values
    update();

    update_changed_ui();
}

void Tab::add_dirty_setting(const std::string& opt_key)
{
    m_options_dirty.push_back(opt_key);
}

// Update the combo box label of the selected preset based on its "dirty" state,
// comparing the selected preset config with $self->{config}.
void Tab::update_dirty()
{
    m_presets_choice->update_dirty();
    on_presets_changed();
    update_changed_ui();
}

void Tab::update_tab_ui()
{
    m_presets_choice->update();
}

// Load a provied DynamicConfig into the tab, modifying the active preset.
// This could be used for example by setting a Wipe Tower position by interactive manipulation in the 3D view.
void Tab::load_config(const DynamicPrintConfig& config)
{
    bool modified = 0;
    for (auto opt_key : m_config->diff(config)) {
        m_config->set_key_value(opt_key, config.option(opt_key)->clone());
        modified = 1;
    }
    if (modified) {
        update_dirty();
        //# Initialize UI components with the config values.
        reload_config();
        update();
    }
}

// Reload current $self->{config} (aka $self->{presets}->edited_preset->config) into the UI fields.
void Tab::reload_config()
{
    if (m_active_page)
        m_active_page->reload_config();
    //also reload scripted that aren't on the active page.
    for (PageShp page : m_pages) {
        if (page.get() != m_active_page) {
            for (auto group : page->m_optgroups) {
                // ask for activated the preset even if the gui isn't created, as the script may want to modify the conf.
                group->update_script_presets(true);
            }
        }
    }
}

void Tab::update_mode()
{
    m_mode = wxGetApp().get_mode();

    // update mode for ModeSizer
    if (m_mode_sizer)
    m_mode_sizer->SetMode(m_mode);

    update_visibility();

    update_changed_tree_ui();
}

void Tab::update_visibility()
{
    Freeze(); // There is needed Freeze/Thaw to avoid a flashing after Show/Layout

    for (auto page : m_pages)
        page->update_visibility(m_mode, page.get() == m_active_page);
    rebuild_page_tree();

    if (m_type == Preset::TYPE_SLA_PRINT || m_type == Preset::TYPE_FFF_PRINT)
        update_description_lines();

    Layout();
    Thaw();
}

void Tab::msw_rescale()
{
    m_em_unit = em_unit(m_parent);

    if (m_mode_sizer)
    m_mode_sizer->msw_rescale();
    m_presets_choice->msw_rescale();

    m_treectrl->SetMinSize(wxSize(20 * m_em_unit, -1));

    // rescale buttons and cached bitmaps
    for (const auto btn : m_scaled_buttons)
        btn->msw_rescale();
    for (const auto bmp : m_scaled_bitmaps)
        bmp->msw_rescale();

    if (m_detach_preset_btn)
        m_detach_preset_btn->msw_rescale();

    // rescale icons for tree_ctrl
    for (ScalableBitmap& bmp : m_scaled_icons_list)
        bmp.msw_rescale();
    // recreate and set new ImageList for tree_ctrl
    m_icons->RemoveAll();
    m_icons = new wxImageList(m_scaled_icons_list.front().bmp().GetWidth(), m_scaled_icons_list.front().bmp().GetHeight());
    for (ScalableBitmap& bmp : m_scaled_icons_list)
        m_icons->Add(bmp.bmp());
    m_treectrl->AssignImageList(m_icons);

    // rescale options_groups
    if (m_active_page)
        m_active_page->msw_rescale();

    Layout();
}

void Tab::sys_color_changed()
{
    m_presets_choice->sys_color_changed();

    // update buttons and cached bitmaps
    for (const auto btn : m_scaled_buttons)
        btn->msw_rescale();
    for (const auto bmp : m_scaled_bitmaps)
        bmp->msw_rescale();
    if (m_detach_preset_btn)
        m_detach_preset_btn->msw_rescale();

    // update icons for tree_ctrl
    for (ScalableBitmap& bmp : m_scaled_icons_list)
        bmp.msw_rescale();
    // recreate and set new ImageList for tree_ctrl
    m_icons->RemoveAll();
    m_icons = new wxImageList(m_scaled_icons_list.front().bmp().GetWidth(), m_scaled_icons_list.front().bmp().GetHeight());
    for (ScalableBitmap& bmp : m_scaled_icons_list)
        m_icons->Add(bmp.bmp());
    m_treectrl->AssignImageList(m_icons);

    // Colors for ui "decoration"
    update_label_colours();
#ifdef _WIN32
    wxWindowUpdateLocker noUpdates(this);
    if (m_mode_sizer)
        m_mode_sizer->msw_rescale();
    wxGetApp().UpdateDarkUI(this);
    wxGetApp().UpdateDarkUI(m_treectrl);
#endif
    update_changed_tree_ui();

    // update options_groups
    if (m_active_page)
        m_active_page->sys_color_changed();

    Layout();
}

Field* Tab::get_field(const t_config_option_key& opt_key, int opt_index/* = -1*/) const
{
    return m_active_page ? m_active_page->get_field(opt_key, opt_index) : nullptr;
}

std::pair<OG_CustomCtrl*, bool*> Tab::get_custom_ctrl_with_blinking_ptr(const t_config_option_key& opt_key, int opt_index/* = -1*/)
{
    if (!m_active_page)
        return {nullptr, nullptr};

    std::pair<OG_CustomCtrl*, bool*> ret = {nullptr, nullptr};

    for (auto opt_group : m_active_page->m_optgroups) {
        ret = opt_group->get_custom_ctrl_with_blinking_ptr(opt_key, opt_index);
        if (ret.first && ret.second)
            break;
    }
    return ret;
}

Field* Tab::get_field(Page*& selected_page, const t_config_option_key& opt_key, int opt_index/* = -1*/)
{
    Field* field = nullptr;
    for (auto page : m_pages) {
        field = page->get_field(opt_key, opt_index);
        if (field != nullptr) {
            selected_page = page.get();
            return field;
        }
    }
    return field;
}

void Tab::toggle_option(const std::string& opt_key, bool toggle, int opt_index/* = -1*/)
{
    if (!m_active_page)
        return;
    Field* field = m_active_page->get_field(opt_key, opt_index);
    if (field)
        field->toggle(toggle);
};

// To be called by custom widgets, load a value into a config,
// update the preset selection boxes (the dirty flags)
// If value is saved before calling this function, put saved_value = true,
// and value can be some random value because in this case it will not been used
void Tab::load_key_value(const std::string& opt_key, const boost::any& value, bool saved_value /*= false*/)
{
    if (!saved_value) change_opt_value(*m_config, opt_key, value);
    // Mark the print & filament enabled if they are compatible with the currently selected preset.
    if (opt_key == "compatible_printers" || opt_key == "compatible_prints") {
        // Don't select another profile if this profile happens to become incompatible.
        m_preset_bundle->update_compatible(PresetSelectCompatibleType::Never);
    }
    m_presets_choice->update_dirty();
    on_presets_changed();
    update();
}

bool Tab::set_value(const t_config_option_key& opt_key, const boost::any& value) {
    bool changed = false;
    for (auto page : m_pages) {
        if (page->set_value(opt_key, value))
            changed = true;
    }
    return changed;
}

static wxString support_combo_value_for_config(const DynamicPrintConfig &config, bool is_fff)
{
    const std::string support         = is_fff ? "support_material"                 : "supports_enable";
    const std::string buildplate_only = is_fff ? "support_material_buildplate_only" : "support_buildplate_only";
    return
        ! config.opt_bool(support) ?
            _("None") :
            (is_fff && !config.opt_bool("support_material_auto")) ?
                _("For support enforcers only") :
                (config.opt_bool(buildplate_only) ? _("Support on build plate only") :
                                                    _("Everywhere"));
}

static wxString pad_combo_value_for_config(const DynamicPrintConfig &config)
{
    return config.opt_bool("pad_enable") ? (config.opt_bool("pad_around_object") ? _("Around object") : _("Below object")) : _("None");
}

void Tab::on_value_change(const std::string& opt_key, const boost::any& value)
{
    if (wxGetApp().plater() == nullptr) {
        return;
    }

    PrinterTechnology pt = get_printer_technology();
    ConfigOptionsGroup* og_freq_chng_params = wxGetApp().sidebar().og_freq_chng_params(pt);
    
    // script presets
    if (this->m_script_exec.is_intialized()) {
        auto it = deps_id_2_script_ids.find(opt_key);
        if (it != deps_id_2_script_ids.end()) {
            for (const std::string& preset_id : it->second) {
                for (PageShp page : m_pages) {
                    Field* field = page->get_field(preset_id, -1);
                    if (field) {
                        boost::any script_val = this->m_script_exec.call_script_function_get_value(field->m_opt);
                        if (!script_val.empty())
                            field->set_value(script_val, false);
                    }
                }
                { // also check freq changed params
                    Field* field = og_freq_chng_params->get_field(preset_id);
                    if (field) {
                        boost::any script_val = this->m_script_exec.call_script_function_get_value(field->m_opt);
                        if (!script_val.empty())
                            field->set_value(script_val, false);
                    }
                }
            }
        }
    }

    // update unscripted freq params
    Field* field = og_freq_chng_params->get_field(opt_key);
    if (field) {
        boost::any val = og_freq_chng_params->get_config_value(*m_config, opt_key);
        field->set_value(val, false);
    }


    if (opt_key == "wipe_tower" || opt_key == "single_extruder_multi_material" || opt_key == "extruders_count" )
        update_wiping_button_visibility();

    if (opt_key == "extruders_count") {
        wxGetApp().plater()->on_extruders_change(boost::any_cast<int>(value));
    }

    if (opt_key == "duplicate_distance") {
        wxGetApp().mainframe->plater()->canvas3D()->set_arrange_settings(m_presets->get_edited_preset().config, m_presets->get_edited_preset().printer_technology());
    }

    // reset variable layer height if min/max has changed, as it's probably now invalid.
    if (opt_key.find("min_layer_height") == 0   || opt_key.find("max_layer_height") == 0) {
        wxPostEvent((wxEvtHandler*)wxGetApp().mainframe->plater()->canvas3D()->get_wxglcanvas(), SimpleEvent(EVT_GLCANVAS_RESET_LAYER_HEIGHT_PROFILE));
    }

    //wxGetApp().preset_bundle->value_changed(opt_key);
    // update phony fields
    
    //auto thing = wxGetApp().plater()->
    std::set<const DynamicPrintConfig*> changed = m_config->value_changed(opt_key, {
        &wxGetApp().preset_bundle->prints(wxGetApp().plater()->printer_technology()).get_edited_preset().config,
        &wxGetApp().preset_bundle->materials(wxGetApp().plater()->printer_technology()).get_edited_preset().config,
        &wxGetApp().preset_bundle->printers.get_edited_preset().config,
        /*&wxGetApp().preset_bundle->full_config()*/ });
    if (changed.find(m_config) != changed.end()) {
        update_dirty();
        //# Initialize UI components with the config values.
        reload_config();
    }
    if (changed.find(&wxGetApp().preset_bundle->fff_prints.get_edited_preset().config) != changed.end()) {
        wxGetApp().get_tab(Preset::Type::TYPE_FFF_PRINT)->update_dirty();
        wxGetApp().get_tab(Preset::Type::TYPE_FFF_PRINT)->reload_config();
    }
    if (changed.find(&wxGetApp().preset_bundle->sla_prints.get_edited_preset().config) != changed.end()) {
        wxGetApp().get_tab(Preset::Type::TYPE_SLA_PRINT)->update_dirty();
        wxGetApp().get_tab(Preset::Type::TYPE_SLA_PRINT)->reload_config();
    }
    if (changed.find(&wxGetApp().preset_bundle->filaments.get_edited_preset().config) != changed.end()) {
        wxGetApp().get_tab(Preset::Type::TYPE_FFF_FILAMENT)->update_dirty();
        wxGetApp().get_tab(Preset::Type::TYPE_FFF_FILAMENT)->reload_config();
    }
    if (changed.find(&wxGetApp().preset_bundle->sla_materials.get_edited_preset().config) != changed.end()) {
        wxGetApp().get_tab(Preset::Type::TYPE_SLA_MATERIAL)->update_dirty();
        wxGetApp().get_tab(Preset::Type::TYPE_SLA_MATERIAL)->reload_config();
    }
    if (changed.find(&wxGetApp().preset_bundle->printers.get_edited_preset().config) != changed.end()) {
        wxGetApp().get_tab(Preset::Type::TYPE_PRINTER)->update_dirty();
        wxGetApp().get_tab(Preset::Type::TYPE_PRINTER)->reload_config();
    }

    if (m_postpone_update_ui) {
        // It means that not all values are rolled to the system/last saved values jet.
        // And call of the update() can causes a redundant check of the config values,
        // see https://github.com/prusa3d/PrusaSlicer/issues/7146
        return;
    }

    update();
}

// Show/hide the 'purging volumes' button
void Tab::update_wiping_button_visibility() {
    if (m_preset_bundle->printers.get_selected_preset().printer_technology() != ptFFF)
        return; // ys_FIXME
    bool wipe_tower_enabled = dynamic_cast<ConfigOptionBool*>(  (m_preset_bundle->fff_prints.get_edited_preset().config  ).option("wipe_tower"))->value;
    bool multiple_extruders = dynamic_cast<ConfigOptionFloats*>((m_preset_bundle->printers.get_edited_preset().config).option("nozzle_diameter"))->values.size() > 1;

    auto wiping_dialog_button = wxGetApp().sidebar().get_wiping_dialog_button();
    if (wiping_dialog_button) {
        wiping_dialog_button->Show(wipe_tower_enabled && multiple_extruders);
        wiping_dialog_button->GetParent()->Layout();
    }
}

void Tab::activate_option(const std::string& opt_key, const wxString& category)
{
    wxString page_title = translate_category(category, m_type);

    auto cur_item = m_treectrl->GetFirstVisibleItem();
    if (!cur_item)
        return;

    // We should to activate a tab with searched option, if it doesn't.
    // And do it before finding of the cur_item to avoid a case when Tab isn't activated jet and all treeItems are invisible
    wxGetApp().mainframe->select_tab(this);

    while (cur_item) {
        auto title = m_treectrl->GetItemText(cur_item);
        if (page_title != title) {
            cur_item = m_treectrl->GetNextVisible(cur_item);
            continue;
        }

        m_treectrl->SelectItem(cur_item);
        break;
    }

    auto set_focus = [](wxWindow* win) {
        win->SetFocus();
#ifdef WIN32
        if (wxTextCtrl* text = dynamic_cast<wxTextCtrl*>(win))
            text->SetSelection(-1, -1);
        else if (wxSpinCtrl* spin = dynamic_cast<wxSpinCtrl*>(win))
            spin->SetSelection(-1, -1);
#endif // WIN32
    };

    Field* field = get_field(opt_key);

    // focused selected field
    if (field)
        set_focus(field->getWindow());
    else if (category == "Single extruder MM setup") {
        // When we show and hide "Single extruder MM setup" page, 
        // related options are still in the search list
        // So, let's hightlighte a "single_extruder_multi_material" option, 
        // as a "way" to show hidden page again
        field = get_field("single_extruder_multi_material");
        if (field)
            set_focus(field->getWindow());
        }

    m_highlighter.init(get_custom_ctrl_with_blinking_ptr(opt_key));
}

void Tab::cache_config_diff(const std::vector<std::string>& selected_options)
{
    m_cache_config.apply_only(m_presets->get_edited_preset().config, selected_options);
}

void Tab::apply_config_from_cache()
{
    bool was_applied = false;
    // check and apply extruders count for printer preset
    if (m_type == Preset::TYPE_PRINTER)
        was_applied = static_cast<TabPrinter*>(this)->apply_extruder_cnt_from_cache();

    if (!m_cache_config.empty()) {
        m_presets->get_edited_preset().config.apply(m_cache_config);
        m_cache_config.clear();

        was_applied = true;
    }

    if (was_applied)
        update_dirty();
    }


// Call a callback to update the selection of presets on the plater:
// To update the content of the selection boxes,
// to update the filament colors of the selection boxes,
// to update the "dirty" flags of the selection boxes,
// to update number of "filament" selection boxes when the number of extruders change.
void Tab::on_presets_changed()
{
    if (wxGetApp().plater() == nullptr)
        return;

    // Instead of PostEvent (EVT_TAB_PRESETS_CHANGED) just call update_presets
    wxGetApp().plater()->sidebar().update_presets(m_type);

    // Printer selected at the Printer tab, update "compatible" marks at the print and filament selectors.
    for (auto t: m_dependent_tabs)
    {
        Tab* tab = wxGetApp().get_tab(t);
        // If the printer tells us that the print or filament/sla_material preset has been switched or invalidated,
        // refresh the print or filament/sla_material tab page.
        // But if there are options, moved from the previously selected preset, update them to edited preset
        tab->apply_config_from_cache();
        tab->load_current_preset();
    }

    // clear m_dependent_tabs after first update from select_preset()
    // to avoid needless preset loading from update() function
    m_dependent_tabs.clear();

    // Update Project dirty state, update application title bar.
    wxGetApp().plater()->update_project_dirty_from_presets();
}

void Tab::build_preset_description_line(ConfigOptionsGroup* optgroup)
{
    auto description_line = [this](wxWindow* parent) {
        return description_line_widget(parent, &m_parent_preset_description_line);
    };

    auto detach_preset_btn = [this](wxWindow* parent) {
        m_detach_preset_btn = new ScalableButton(parent, wxID_ANY, "lock_open_sys", _L("Detach from system preset"), 
                                                 wxDefaultSize, wxDefaultPosition, wxBU_LEFT | wxBU_EXACTFIT, true);
        ScalableButton* btn = m_detach_preset_btn;
        btn->SetFont(Slic3r::GUI::wxGetApp().normal_font());

        auto sizer = new wxBoxSizer(wxHORIZONTAL);
        sizer->Add(btn);

        btn->Bind(wxEVT_BUTTON, [this, parent](wxCommandEvent&)
        {
            bool system = m_presets->get_edited_preset().is_system;
            bool dirty  = m_presets->get_edited_preset().is_dirty;
            wxString msg_text = system ? 
                _(L("A copy of the current system preset will be created, which will be detached from the system preset.")) :
                _(L("The current custom preset will be detached from the parent system preset."));
            if (dirty) {
                msg_text += "\n\n";
                msg_text += _(L("Modifications to the current profile will be saved."));
            }
            msg_text += "\n\n";
            msg_text += _(L("This action is not revertible.\nDo you want to proceed?"));

            //wxMessageDialog dialog(parent, msg_text, _(L("Detach preset")), wxICON_WARNING | wxYES_NO | wxCANCEL);
            MessageDialog dialog(parent, msg_text, _(L("Detach preset")), wxICON_WARNING | wxYES_NO | wxCANCEL);
            if (dialog.ShowModal() == wxID_YES)
                save_preset(m_presets->get_edited_preset().is_system ? std::string() : m_presets->get_edited_preset().name, true);
        });

        btn->Hide();

        return sizer;
    };

    Line line = Line{ "", "" };
    line.full_width = 1;

    line.append_widget(description_line);
    line.append_widget(detach_preset_btn);
    optgroup->append_line(line);
}

void Tab::update_preset_description_line()
{
    const Preset* parent = m_presets->get_selected_preset_parent();
    const Preset& preset = m_presets->get_edited_preset();

    wxString description_line;

    if (preset.is_default) {
        description_line = _(L("This is a default preset."));
    } else if (preset.is_system) {
        description_line = _(L("This is a system preset."));
    } else if (parent == nullptr) {
        description_line = _(L("Current preset is inherited from the default preset."));
    } else {
        std::string name = parent->name;
        boost::replace_all(name, "&", "&&");
        description_line = _(L("Current preset is inherited from")) + ":\n\t" + from_u8(name);
    }

    if (preset.is_default || preset.is_system)
        description_line += "\n\t" + _(L("It can't be deleted or modified.")) +
                            "\n\t" + _(L("Any modifications should be saved as a new preset inherited from this one.")) +
                            "\n\t" + _(L("To do that please specify a new name for the preset."));

    if (parent && parent->vendor)
    {
        description_line += "\n\n" + _(L("Additional information:")) + "\n";
        description_line += "\t" + _(L("vendor")) + ": " + (m_type == Slic3r::Preset::TYPE_PRINTER ? "\n\t\t" : "") + parent->vendor->name +
                            ", ver: " + parent->vendor->config_version.to_string();
        if (m_type == Slic3r::Preset::TYPE_PRINTER) {
            const std::string &printer_model = preset.config.opt_string("printer_model");
            if (! printer_model.empty())
                description_line += "\n\n\t" + _(L("printer model")) + ": \n\t\t" + printer_model;
            switch (preset.printer_technology()) {
            case ptFFF:
            {
                //FIXME add prefered_sla_material_profile for SLA
                const std::string              &default_print_profile = preset.config.opt_string("default_print_profile");
                const std::vector<std::string> &default_filament_profiles = preset.config.option<ConfigOptionStrings>("default_filament_profile")->values;
                if (!default_print_profile.empty())
                    description_line += "\n\n\t" + _(L("default print profile")) + ": \n\t\t" + default_print_profile;
                if (!default_filament_profiles.empty())
                {
                    description_line += "\n\n\t" + _(L("default filament profile")) + ": \n\t\t";
                    for (auto& profile : default_filament_profiles) {
                        if (&profile != &*default_filament_profiles.begin())
                            description_line += ", ";
                        description_line += profile;
                    }
                }
                break;
            }
            case ptSLA:
            {
                //FIXME add prefered_sla_material_profile for SLA
                const std::string &default_sla_material_profile = preset.config.opt_string("default_sla_material_profile");
                if (!default_sla_material_profile.empty())
                    description_line += "\n\n\t" + _(L("default SLA material profile")) + ": \n\t\t" + default_sla_material_profile;

                const std::string &default_sla_print_profile = preset.config.opt_string("default_sla_print_profile");
                if (!default_sla_print_profile.empty())
                    description_line += "\n\n\t" + _(L("default SLA print profile")) + ": \n\t\t" + default_sla_print_profile;
                break;
            }
            default: break;
            }
        }
        else if (!preset.alias.empty())
        {
            description_line += "\n\n\t" + _(L("full profile name"))     + ": \n\t\t" + preset.name;
            description_line += "\n\t"   + _(L("symbolic profile name")) + ": \n\t\t" + preset.alias;
        }
    }

    if (m_parent_preset_description_line)
        m_parent_preset_description_line->SetText(description_line, false);

    if (m_detach_preset_btn)
        m_detach_preset_btn->Show(parent && parent->is_system && !preset.is_default);
    Layout();
}

void Tab::update_frequently_changed_parameters()
{
    PrinterTechnology pt = get_printer_technology();
    wxGetApp().plater()->sidebar().og_freq_chng_params(pt)->reload_config();

    if (supports_printer_technology(ptFFF))
        update_wiping_button_visibility();
}

void Tab::update_script_presets()
{
    for (PageShp& page : m_pages)
        page->update_script_presets();
}

t_change Tab::set_or_add(t_change previous, t_change toadd) {
    if (previous == nullptr)
        return toadd;
    else
        return [previous, toadd](t_config_option_key opt_key, boost::any value) {
        try {
            toadd(opt_key, value);
            previous(opt_key, value);

        }
        catch (const std::exception & ex) {
            std::cerr << "Exception while calling group event about "<<opt_key<<": " << ex.what();
            throw ex;
        }
    };
}

std::vector<Slic3r::GUI::PageShp> Tab::create_pages(std::string setting_type_name, int idx_page)
{
    //search for the file
    const boost::filesystem::path ui_layout_file = Slic3r::GUI::get_app_config()->layout_config_path() / setting_type_name;
    if (!boost::filesystem::exists(ui_layout_file)) {
        std::cerr << "Error: cannot create " << setting_type_name << "settings, cannot find file " << ui_layout_file << "\n";
        return {};
    } else
        Slic3r::slic3r_log->info("settings gui") << "create settings  " << setting_type_name << "\n";

    bool no_page_yet = true;
#ifdef __WXMSW__
    /* Workaround for correct layout of controls inside the created page:
     * In some _strange_ way we should we should imitate page resizing.
     */
/*    auto layout_page = [this](PageShp page)
    {
        const wxSize& sz = page->GetSize();
        page->SetSize(sz.x + 1, sz.y + 1);
        page->SetSize(sz);
    };*/
#endif
    std::vector<Slic3r::GUI::PageShp> pages;
    Slic3r::GUI::PageShp current_page;
    ConfigOptionsGroupShp current_group;
    Line current_line{ "", "" };
    bool in_line = false;
    int height = 0;
    bool logs = false;

    //read file
    //std::ifstream filestream(ui_layout_file.c_str());
    boost::filesystem::ifstream filestream(ui_layout_file);
    std::string full_line;
    while (std::getline(filestream, full_line)) {
        //remove spaces
        boost::algorithm::trim(full_line);
        if (full_line.size() < 4 || full_line[0] == '#') continue;
        //get main command
        if (boost::starts_with(full_line, "logs"))
        {
            logs = true;
        }
        else if (boost::starts_with(full_line, "page"))
        {
#ifdef __WXMSW__
//            if(!no_page_yet)
//                layout_page(current_page);
#endif
            no_page_yet = false;
            if (in_line) {
                current_group->append_line(current_line);
                if (logs) Slic3r::slic3r_log->info("settings gui") << "add line\n";
                in_line = false;
            }
            std::vector<std::string> params;
            boost::split(params, full_line, boost::is_any_of(":"));
            for (std::string &str : params) {
                while (str.size() > 1 && (str.front() == ' ' || str.front() == '\t')) str = str.substr(1, str.size() - 1);
                while (str.size() > 1 && (str.back() == ' ' || str.back() == '\t')) str = str.substr(0, str.size() - 1);
            }
            if (params.size() < 2) std::cerr << "error, you need to add the title and icon of the page example: page:awsome page:shell, \n";
            if (params.size() < 2) continue;
            if (params.size() == 2) params.push_back("wrench");

            std::string label = L(params[params.size()-2]);

            for (int i = 1; i < params.size() - 1; i++) {
                if (params[i] == "idx")
                {
                    label = label + " " + std::to_string(int(idx_page + 1));
                }
            }

            if(logs) Slic3r::slic3r_log->info("settings gui") << "create page " << label.c_str() <<" : "<< params[params.size() - 1] << "\n";
            pages.push_back(create_options_page(L(label), params[params.size() - 1]));
            current_page = pages.back();
        }
        else if (boost::starts_with(full_line, "end_page"))
        {
            if (in_line) {
                current_group->append_line(current_line);
                if (logs) Slic3r::slic3r_log->info("settings gui") << "add line\n";
                in_line = false;
            }
            current_page.reset();
        }
        else if (boost::starts_with(full_line, "group"))
        {
            if (in_line) {
                current_group->append_line(current_line);
                if (logs) Slic3r::slic3r_log->info("settings gui") << "add line\n";
                in_line = false;
            }
            std::vector<std::string> params;
            boost::split(params, full_line, boost::is_any_of(":"));
            for (std::string &str : params) {
                while (str.size() > 1 && (str.front() == ' ' || str.front() == '\t')) str = str.substr(1, str.size() - 1);
                while (str.size() > 1 && (str.back() == ' ' || str.back() == '\t')) str = str.substr(0, str.size() - 1);
            }
            bool no_title = false;
            bool no_search = false;
            for (int i = 1; i < params.size() - 1; i++) {
                if (params[i] == "nolabel")
                {
                    no_title = true;
                    std::cerr << "Warning: 'nolabel' is deprecated, please replace it by 'no_title' in your " << setting_type_name << " ui file";
                }
                if (params[i] == "no_title")
                    no_title = true;
                if ("no_search" == params[i])
                    no_search = true;
            }
            
            current_group = current_page->new_optgroup(_(params.back()), no_title, !no_search);
            for (int i = 1; i < params.size() - 1; i++) {
                if (boost::starts_with(params[i], "title_width$")) {
                    current_group->title_width = atoi(params[i].substr(12, params[i].size() - 12).c_str());
                }
                else if (params[i].find("label_width$") != std::string::npos)
                {
                    current_group->label_width = atoi(params[i].substr(12, params[i].size() - 12).c_str());
                }
                else if (params[i].find("sidetext_width$") != std::string::npos)
                {
                    current_group->sidetext_width = atoi(params[i].substr(15, params[i].size() - 15).c_str());
                } else if (params[i] == "extruders_count_event") {
                    TabPrinter* tab = nullptr;
                    if ((tab = dynamic_cast<TabPrinter*>(this)) == nullptr) continue;
                    current_group->m_on_change = set_or_add(current_group->m_on_change, [this, tab, current_group](t_config_option_key opt_key, boost::any value) {
                        // optgroup->get_value() return int for def.type == coInt,
                        // Thus, there should be boost::any_cast<int> !
                        // Otherwise, boost::any_cast<size_t> causes an "unhandled unknown exception"
                        if (opt_key == "extruders_count" || opt_key == "single_extruder_multi_material") {
                            size_t extruders_count = size_t(boost::any_cast<int>(current_group->get_value("extruders_count")));
                            if (opt_key == "extruders_count") {
                                tab->extruders_count_changed(extruders_count);
                            } else if (opt_key == "single_extruder_multi_material") {
                                tab->build_unregular_pages(false);
                                wxGetApp().sidebar().update_objects_list_extruder_column(extruders_count);
                            }
                            init_options_list(); // m_options_list should be updated before UI updating
                            update_dirty();
                            if (opt_key == "single_extruder_multi_material") { // the single_extruder_multimaterial was added to force pages
                                on_value_change(opt_key, value);                      // rebuild - let's make sure the on_value_change is not skipped

                                if (boost::any_cast<bool>(value) && tab->m_extruders_count > 1) {
                                    SuppressBackgroundProcessingUpdate sbpu;
                                    std::vector<double> nozzle_diameters = static_cast<const ConfigOptionFloats*>(m_config->option("nozzle_diameter"))->values;
                                    const double frst_diam = nozzle_diameters[0];

                                    for (auto cur_diam : nozzle_diameters) {
                                        // if value is differs from first nozzle diameter value
                                        if (fabs(cur_diam - frst_diam) > EPSILON) {
                                            const wxString msg_text = _L("Single Extruder Multi Material is selected, \n"
                                                "and all extruders must have the same diameter.\n"
                                                "Do you want to change the diameter for all extruders to first extruder nozzle diameter value?");
                                            MessageDialog dialog(parent(), msg_text, _L("Nozzle diameter"), wxICON_WARNING | wxYES_NO);

                                            DynamicPrintConfig new_conf = *m_config;
                                            if (dialog.ShowModal() == wxID_YES) {
                                                for (size_t i = 1; i < nozzle_diameters.size(); i++)
                                                    nozzle_diameters[i] = frst_diam;

                                                new_conf.set_key_value("nozzle_diameter", (new ConfigOptionFloats(nozzle_diameters))->set_is_extruder_size(true));
                                            } else
                                                new_conf.set_key_value("single_extruder_multi_material", new ConfigOptionBool(false));

                                            load_config(new_conf);
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                    });
                } else if (params[i] == "milling_count_event") {
                    TabPrinter* tab = nullptr;
                    if ((tab = dynamic_cast<TabPrinter*>(this)) == nullptr) continue;
                    current_group->m_on_change = set_or_add(current_group->m_on_change, [this, tab, current_group](t_config_option_key opt_key, boost::any value) {
                        // optgroup->get_value() return int for def.type == coInt,
                        // Thus, there should be boost::any_cast<int> !
                        // Otherwise, boost::any_cast<size_t> causes an "unhandled unknown exception"
                        if (opt_key == "milling_count") {
                            size_t milling_count = size_t(boost::any_cast<int>(current_group->get_value("milling_count")));
                            tab->milling_count_changed(milling_count);
                            init_options_list(); // m_options_list should be updated before UI updating
                        }
                    });
                }
                else if (params[i] == "silent_mode_event") {
                    TabPrinter* tab = nullptr;
                    if ((tab = dynamic_cast<TabPrinter*>(this)) == nullptr) continue;
                    current_group->m_on_change = set_or_add(current_group->m_on_change, [this, tab](t_config_option_key opt_key, boost::any value) {
                        tab->update_fff(); //check for kinematic rebuild
                        tab->build_unregular_pages(false);
                    });
                }
                else if (params[i] == "material_density_event") {
                    current_group->m_on_change = set_or_add(current_group->m_on_change, [this, current_group](t_config_option_key opt_key, boost::any value)
                    {
                        DynamicPrintConfig new_conf = *m_config;

                        if (opt_key == "bottle_volume") {
                            double new_bottle_weight = boost::any_cast<double>(value) / (new_conf.option("material_density")->getFloat() * 1000);
                            new_conf.set_key_value("bottle_weight", new ConfigOptionFloat(new_bottle_weight));
                        }
                        if (opt_key == "bottle_weight") {
                            double new_bottle_volume = boost::any_cast<double>(value)*(new_conf.option("material_density")->getFloat() * 1000);
                            new_conf.set_key_value("bottle_volume", new ConfigOptionFloat(new_bottle_volume));
                        }
                        if (opt_key == "material_density") {
                            double new_bottle_volume = new_conf.option("bottle_weight")->getFloat() * boost::any_cast<double>(value) * 1000;
                            new_conf.set_key_value("bottle_volume", new ConfigOptionFloat(new_bottle_volume));
                        }

                        load_config(new_conf);

                        if (opt_key == "bottle_volume" || opt_key == "bottle_cost") {
                            wxGetApp().sidebar().update_sliced_info_sizer();
                            wxGetApp().sidebar().Layout();
                        }
                    });
                } else if (params[i] == "filament_spool_weight_event") {
                    current_group->m_on_change = set_or_add(current_group->m_on_change, [this, current_group](t_config_option_key opt_key, boost::any value)
                        {
                            update_dirty();
                            if (opt_key == "filament_spool_weight") {
                                // Change of this option influences for an update of "Sliced Info"
                                wxGetApp().sidebar().update_sliced_info_sizer();
                                wxGetApp().sidebar().Layout();
                            } else
                                on_value_change(opt_key, value);
                        });
                } else if (params[i] == "validate_gcode") {
                    current_group->m_on_change = set_or_add(current_group->m_on_change, [this, &current_group](t_config_option_key opt_key, boost::any value) {
                        //validate_custom_gcode_cb(this, current_group, opt_key, value);
                        this->validate_custom_gcodes_was_shown = !Tab::validate_custom_gcode(current_group->title, boost::any_cast<std::string>(value));
                        this->update_dirty();
                        this->on_value_change(opt_key, value);
                    });
                }
            }
            if (logs) Slic3r::slic3r_log->info("settings gui") << "create group " << params.back() << "\n";
        }
        else if (boost::starts_with(full_line, "end_group"))
        {
            if (in_line) {
                current_group->append_line(current_line);
                if (logs) Slic3r::slic3r_log->info("settings gui") << "add line\n";
                in_line = false;
            }
            current_group.reset();
        }
        else if (boost::starts_with(full_line, "line"))
        {
            if (in_line) {
                current_group->append_line(current_line);
                if (logs) Slic3r::slic3r_log->info("settings gui") << "add line\n";
                in_line = false;
            }
            std::vector<std::string> params;
            boost::split(params, full_line, boost::is_any_of(":"));
            for (std::string& str : params) {
                while (str.size() > 1 && (str.front() == ' ' || str.front() == '\t')) str = str.substr(1, str.size() - 1);
                while (str.size() > 1 && (str.back() == ' ' || str.back() == '\t')) str = str.substr(0, str.size() - 1);
            }

            current_line = { _L(params.empty()?"":params.back().c_str()), wxString{""} };
            for (int i = 1; i < params.size() - 1; i++) {
                if (boost::starts_with(params[i], "url$")) { // only on line
                    current_line.label_path = params[i].substr(4, params[i].size() - 4);
                }
            }
            in_line = true;
            if (logs) Slic3r::slic3r_log->info("settings gui") << "create line " << (params.empty() ? "" : params.back()) << "\n";
        }
        else if (boost::starts_with(full_line, "end_line"))
        {
            if (!current_line.get_options().empty()) {
                current_group->append_line(current_line);
                if (logs) Slic3r::slic3r_log->info("settings gui") << "add line\n";
            }
            in_line = false;
        }
        else if (boost::starts_with(full_line, "setting"))
        {
            std::vector<std::string> params;
            boost::split(params, full_line, boost::is_any_of(":"));
            for (std::string& str : params) {
                while (str.size() > 1 && (str.front() == ' ' || str.front() == '\t')) str = str.substr(1, str.size() - 1);
                while (str.size() > 1 && (str.back() == ' ' || str.back() == '\t')) str = str.substr(0, str.size() - 1);
            }

            bool is_script = std::find(params.begin(), params.end(), "script") != params.end();

            if (is_script && !this->m_script_exec.is_intialized()) {
                BOOST_LOG_TRIVIAL(error) << "Error: trying to creater a scripted widget for '"<< setting_type_name << "' but the .as file doesn't exist or can't be parsed";
                continue;
            }
            std::string setting_id = "";
            if (params.size() > 1) setting_id = params.back();
            if (setting_id.size() < 2) continue;
            if (!m_config->has(setting_id) && !is_script) {
                std::cerr << "No " << setting_id << " in ConfigOptionsGroup config, tab " << setting_type_name << ".\n";
                continue;
            }

            if (setting_id == "compatible_printers") {
                create_line_with_widget(current_group.get(), "compatible_printers", "", [this](wxWindow* parent) {
                    return compatible_widget_create(parent, m_compatible_printers);
                    });
                continue;
            } else if (setting_id == "compatible_prints") {
                create_line_with_widget(current_group.get(), "compatible_prints", "", [this](wxWindow* parent) {
                    return compatible_widget_create(parent, m_compatible_prints);
                    });
                continue;
            }

            int id = -1;
            for (int i = 1; i < params.size() - 1; i++) {
                if (boost::starts_with(params[i], "id$"))
                    id = atoi(params[i].substr(3, params[i].size() - 3).c_str());
                else if (params[i] == "idx")
                    id = idx_page;
            }

            Option option = is_script ? 
                Option(ConfigOptionDef{}, setting_id) : 
                current_group->get_option(setting_id, id);
            if (is_script) {
                option.opt.opt_key = setting_id;
                option.opt.label = setting_id;
                option.opt.type = coBool;
                option.opt.set_default_value(new ConfigOptionBool(false));
                option.opt.gui_type = ConfigOptionDef::GUIType::undefined;
                option.opt.is_script = true;
                option.script = &this->m_script_exec;
            }
            if (current_group->label_width >= 0)
                option.opt.label_width = current_group->label_width;
            if (current_group->sidetext_width >= 0)
                option.opt.sidetext_width = current_group->sidetext_width;

            // global before the loop because can be overriden
            if (height > 0)
                option.opt.height = height;
            
            auto fct_add_enum = [this, &option](std::string& str_list) {
                std::vector<std::string> enum_strs;
                boost::split(enum_strs, str_list, boost::is_any_of("$"));
                if (enum_strs.size() > 2 && enum_strs.size() % 2 == 1) {
                    for (size_t idx = 1; idx < enum_strs.size(); idx += 2) {
                        option.opt.enum_values.push_back(enum_strs[idx]);
                        option.opt.enum_labels.push_back(enum_strs[idx + 1]);
                    }
                    option.opt.set_default_value(new ConfigOptionString(option.opt.enum_values[0]));
                    option.opt.gui_type = ConfigOptionDef::GUIType::f_enum_open;
                    option.opt.gui_flags = "show_value";
                }
            };

            bool need_to_notified_search = false;
            bool colored = false;
            bool custom_label = false;
            std::string label_path;
            for (int i = 1; i < params.size() - 1; i++) {
                if (params[i] == "simple")
                {
                    option.opt.mode = ConfigOptionMode::comSimpleAE;
                }
                else if (params[i] == "advanced")
                {
                    option.opt.mode = ConfigOptionMode::comAdvancedE;
                }
                else if (params[i] == "expert")
                {
                    option.opt.mode = ConfigOptionMode::comExpert;
                }
                else if (boost::starts_with(params[i], "tags"))
                {
                    option.opt.mode = comNone;
                    std::vector<std::string> tag_strs;
                    boost::split(tag_strs, params[i], boost::is_any_of("$"));
                    for (size_t idx = 1; idx < tag_strs.size(); ++idx) {
                        auto it = ConfigOptionDef::names_2_tag_mode.find(tag_strs[idx]);
                        if (it == ConfigOptionDef::names_2_tag_mode.end()) {
                            if (ConfigOptionDef::names_2_tag_mode.size() > 62) { //full
                                continue;
                            }
                            ConfigOptionDef::names_2_tag_mode[tag_strs[idx]] = (ConfigOptionMode)(((uint64_t)1) << ConfigOptionDef::names_2_tag_mode.size());
                            it = ConfigOptionDef::names_2_tag_mode.find(tag_strs[idx]);
                        }
                        option.opt.mode |= it->second;
                    }
                }
                else if (params[i] == "full_label")
                {
                    option.opt.full_label = (params[i].substr(11, params[i].size() - 11));
                    need_to_notified_search = true;
                }
                else if (params[i] == "full_label")
                {
                    option.opt.label = option.opt.full_label;
                    need_to_notified_search = true;
                }
                else if (boost::starts_with(params[i], "label$"))
                {
                    // store current label into full_label if no full_label to prevent rpoblem in the rest of the gui (all empty).
                    if (option.opt.full_label.empty())
                        option.opt.full_label = option.opt.label;
                    option.opt.label = (params[i].substr(6, params[i].size() - 6));
                    need_to_notified_search = true;
                }
                else if (boost::starts_with(params[i], "label_width$")) {
                    option.opt.label_width = atoi(params[i].substr(12, params[i].size() - 12).c_str());
                }
                else if (boost::starts_with(params[i], "label_left")) {
                    option.opt.aligned_label_left = true;
                }
                else if (boost::starts_with(params[i], "sidetext$"))
                {
                    option.opt.sidetext = (params[i].substr(9, params[i].size() - 9));
                }
                else if (boost::starts_with(params[i], "sidetext_width$"))
                {
                    option.opt.sidetext_width = atoi(params[i].substr(15, params[i].size() - 15).c_str());
                }
                else if (params[i] == "full_width") {
                    option.opt.full_width = true;
                }
                else if (boost::starts_with(params[i], "width$")) {
                    option.opt.width = atoi(params[i].substr(6, params[i].size() - 6).c_str());
#ifdef __WXGTK3__
                    option.opt.width += 4; // add width for the big [-][+] buttons
#endif
                }
                else if (boost::starts_with(params[i], "height$")) {
                    option.opt.height = atoi(params[i].substr(7, params[i].size() - 7).c_str());
                }
                else if (boost::starts_with(params[i], "precision$")) {
                    option.opt.precision = atoi(params[i].substr(7, params[i].size() - 7).c_str());
                }
                else if (params[i] == "color") {
                    colored = true;
                }
                else if (boost::starts_with(params[i], "url$")) { // only on line
                    label_path = params[i].substr(4, params[i].size() - 4);
                }
                else if (boost::starts_with(params[i], "tooltip$"))
                {
                    option.opt.tooltip = (params[i].substr(8, params[i].size() - 8));
                    boost::replace_all(option.opt.tooltip, "\\n", "\n");
                    boost::replace_all(option.opt.tooltip, "\\t", "\t");
                    boost::replace_all(option.opt.tooltip, "\\.", ":");
                    boost::replace_all(option.opt.tooltip, "\\£", "$");
                    need_to_notified_search = true;
                }
                else if (boost::starts_with(params[i], "max_literal$"))
                {
                    if(params[i].back() == '%')
                        option.opt.max_literal = { boost::lexical_cast<double>(params[i].substr(12, params[i].size() - 13).c_str()), true };
                    else
                        option.opt.max_literal = { boost::lexical_cast<double>(params[i].substr(12, params[i].size() - 12).c_str()), false };

                } else if (is_script) {
                    //be careful, "floatX" has to deteted before "float".
                    if (params[i] == "bools") {
                        option.opt.type = coBools;
                        option.opt.set_default_value(new ConfigOptionBools{ false });
                    } else if (boost::starts_with(params[i], "ints")) {
                        option.opt.type = coInts;
                        option.opt.set_default_value(new ConfigOptionInts{ 0 });
                        fct_add_enum(params[i]);
                    } else if (boost::starts_with(params[i], "floats_or_percents")) {
                        option.opt.type = coFloatsOrPercents;
                        option.opt.set_default_value(new ConfigOptionFloatsOrPercents{ FloatOrPercent{0.f, false} });
                        fct_add_enum(params[i]);
                    } else if (boost::starts_with(params[i], "floats")) {
                        option.opt.type = coFloats;
                        option.opt.set_default_value(new ConfigOptionFloats{ 0. });
                        fct_add_enum(params[i]);
                    } else if (boost::starts_with(params[i], "percents")) {
                        option.opt.type = coPercents;
                        option.opt.set_default_value(new ConfigOptionPercents{ 0 });
                        fct_add_enum(params[i]);
                    } else if (boost::starts_with(params[i], "strings")) {
                        option.opt.type = coStrings;
                        option.opt.set_default_value(new ConfigOptionString{ "" });
                        fct_add_enum(params[i]);
                    } else if (params[i] == "bool") {
                        option.opt.type = coBool;
                        option.opt.set_default_value(new ConfigOptionBool(false));
                    } else if (boost::starts_with(params[i], "int")) {
                        option.opt.type = coInt;
                        option.opt.set_default_value(new ConfigOptionInt(0));
                        fct_add_enum(params[i]);
                    } else if (boost::starts_with(params[i], "float_or_percent")) {
                        option.opt.type = coFloatOrPercent;
                        option.opt.set_default_value(new ConfigOptionFloatOrPercent(0.f, false));
                        fct_add_enum(params[i]);
                    } else if (boost::starts_with(params[i], "float")) {
                        option.opt.type = coFloat;
                        option.opt.set_default_value(new ConfigOptionFloat(0.));
                        fct_add_enum(params[i]);
                    } else if (boost::starts_with(params[i], "percent")) {
                        option.opt.type = coPercent;
                        option.opt.set_default_value(new ConfigOptionPercent(0));
                        fct_add_enum(params[i]);
                    } else if (boost::starts_with(params[i], "string")) {
                        option.opt.type = coString;
                        option.opt.set_default_value(new ConfigOptionString(""));
                        fct_add_enum(params[i]);
                    } else if (boost::starts_with(params[i], "enum")) {
                        option.opt.type = coEnum;
                        std::vector<std::string> enum_strs;
                        boost::split(enum_strs, params[i], boost::is_any_of("$"));
                        if (enum_strs.size() < 3 || enum_strs.size() % 2 == 0) continue;
                        // FIXME here is a little memory leak. It need to say until the end of the program anyway...
                        t_config_enum_values* enum_map = new t_config_enum_values{};
                        for (size_t idx = 1; idx < enum_strs.size(); idx += 2) {
                            option.opt.enum_values.push_back(enum_strs[idx]);
                            option.opt.enum_labels.push_back(enum_strs[idx + 1]);
                            (*enum_map)[enum_strs[idx]] = idx / 2;
                        }
                        option.opt.enum_keys_map = enum_map;
                        option.opt.set_default_value(new ConfigOptionEnumGeneric{ enum_map, 0 });
                    } else if (boost::starts_with(params[i], "depends")) {
                        std::vector<std::string> depends_str;
                        boost::split(depends_str, params[i], boost::is_any_of("$"));
                        for (size_t idx = 1; idx < depends_str.size(); ++idx) {
                            this->deps_id_2_script_ids[depends_str[idx]].push_back(option.opt.opt_key);
                            option.opt.depends_on.push_back(depends_str[idx]);
                        }
                    }
                }
            }


            current_group->register_to_search(option.opt.opt_key, option.opt, id, false);
            //if (need_to_notified_search)
            //    Search::OptionsSearcher::register_label_override(option.opt.opt_key, option.opt.label, option.opt.full_label, option.opt.tooltip);

            if (is_script) {
                //register on tab to get the icons
                this->m_options_script[option.opt.opt_key] = 0;
            }

            if (!in_line) {
                if (colored) {
                    m_colored_Label_colors[setting_id] = m_default_label_clr;
                    Line l = current_group->create_single_option_line(option, label_path.empty() ? "" : label_path);
                    l.full_Label_color = &m_colored_Label_colors[setting_id];
                    current_group->append_line(l);
                } else {
                    current_group->append_line(current_group->create_single_option_line(option, label_path.empty() ? "" : label_path));
                }
            } else {
                current_line.append_option(option);
            }
            if (logs) Slic3r::slic3r_log->info("settings gui") << "create setting " << setting_id <<"  with label "<< option.opt.label << "and height "<< option.opt.height<<" fw:"<< option.opt.full_width << "\n";
        }
        else if (boost::starts_with(full_line, "height")) {
            std::string arg = "";
            if (full_line.size() > 6 && full_line.find(":") != std::string::npos)
                arg = full_line.substr(full_line.find(":") + 1, full_line.size() - 1 - full_line.find(":"));
            while (arg.size() > 1 && (arg.back() == ' ' || arg.back() == '\t')) arg = arg.substr(0, arg.size() - 1);
            height = atoi(arg.c_str());
        }
        else if (boost::starts_with(full_line, "recommended_thin_wall_thickness_description")) {
            TabPrint* tab = nullptr;
            if ((tab = dynamic_cast<TabPrint*>(this)) == nullptr) continue;
            current_line = { "", "" };
            current_line.full_width = 1;
            current_line.widget = [this, tab](wxWindow* parent) {
                return description_line_widget(parent, &(tab->m_recommended_thin_wall_thickness_description_line));
            };
            current_group->append_line(current_line);
            current_page->descriptions.push_back("wall_thickness");
        }
        else if (boost::starts_with(full_line, "recommended_extrusion_width_description")) {
            TabPrint* tab = nullptr;
            if ((tab = dynamic_cast<TabPrint*>(this)) == nullptr) continue;
            current_line = { "", "" };
            current_line.full_width = 1;
            current_line.widget = [this, tab](wxWindow* parent) {
                //return description_line_widget(parent, &(tab->m_recommended_extrusion_width_description_line));

                auto sizer = new wxBoxSizer(wxVERTICAL);
                wxCollapsiblePane* collpane = new wxCollapsiblePane(parent, wxID_ANY, _L("Help / Details:"));
                wxGetApp().UpdateDarkUI(collpane);
                // add the pane with a zero proportion value to the 'sz' sizer which contains it
                sizer->Add(collpane, 0, wxGROW | wxALL, 5);
                // now add a test label in the collapsible pane using a sizer to layout it:
                wxWindow* win = collpane->GetPane();
                collpane->Bind(wxEVT_COLLAPSIBLEPANE_CHANGED, [tab](wxEvent&) {
                    tab->Layout();
                });

                const wxBitmap& bmp_width = create_scaled_bitmap("explanation_width", this, 80);
                wxStaticBitmap* image_width = new wxStaticBitmap(win, wxID_ANY, bmp_width);
                const wxBitmap& bmp_spacing = create_scaled_bitmap("explanation_spacing", this, 80);
                wxStaticBitmap* image_spacing = new wxStaticBitmap(win, wxID_ANY, bmp_spacing);
                auto sizerV = new wxBoxSizer(wxVERTICAL);
                auto sizerH2 = new wxBoxSizer(wxHORIZONTAL);
                auto sizerH3 = new wxBoxSizer(wxHORIZONTAL);
                sizerH2->Add(image_width, 1, 0, 0);
                sizerH2->Add(image_spacing, 1, 0, 0);
                sizerV->Add(sizerH2);
                ogStaticText* text = new ogStaticText(win, _L("If the perimeter overlap is set at 100%, the yellow areas should be filled by the overlap.\nIf overlap is at 0%, width = spacing."));
                text->SetFont(wxGetApp().normal_font());
                sizerH3->Add(text, 1, wxEXPAND | wxALL, 0);
                sizerV->Add(sizerH3);

                win->SetSizer(sizerV);

                sizerV->SetSizeHints(win);
                collpane->Collapse(true);

                return sizer;
            };
            current_group->append_line(current_line);
            current_page->descriptions.push_back("extrusion_width");
        }
        else if (boost::starts_with(full_line, "top_bottom_shell_thickness_explanation")) {
            TabPrint* tab = nullptr;
            if ((tab = dynamic_cast<TabPrint*>(this)) == nullptr) continue;
            current_line = { "", "" };
            current_line.full_width = 1;
            current_line.widget = [this, tab](wxWindow* parent) {
                return description_line_widget(parent, &(tab->m_top_bottom_shell_thickness_explanation));
            };
            current_group->append_line(current_line);
            current_page->descriptions.push_back("top_bottom_shell");
        }
        else if (boost::starts_with(full_line, "parent_preset_description")) {
            build_preset_description_line(current_group.get());
        } else if (boost::starts_with(full_line, "cooling_description")) {
            TabFilament* tab = nullptr;
            if ((tab = dynamic_cast<TabFilament*>(this)) == nullptr) continue;
            current_line = Line{ "", "" };
            current_line.full_width = 1;
            current_line.widget = [this, tab](wxWindow* parent) {
                return description_line_widget(parent, &(tab->m_cooling_description_line));
            };
            current_group->append_line(current_line);
            current_page->descriptions.push_back("cooling");
        } else if (boost::starts_with(full_line, "volumetric_speed_description")) {
            TabFilament* tab = nullptr;
            if ((tab = dynamic_cast<TabFilament*>(this)) == nullptr) continue;
            current_line = Line{ "", "" };
            current_line.full_width = 1;
            current_line.widget = [this, tab](wxWindow* parent) {
                return description_line_widget(parent, &(tab->m_volumetric_speed_description_line));
            };
            current_group->append_line(current_line);
            current_page->descriptions.push_back("volumetric_speed");
        } else if(boost::starts_with(full_line, "support_object_elevation_description")) {
            TabSLAPrint* tab = nullptr;
            if ((tab = dynamic_cast<TabSLAPrint*>(this)) == nullptr) continue;
            Line line{ "", "" };
            line.full_width = 1;
            line.widget = [this, tab](wxWindow* parent) {
                return description_line_widget(parent, &(tab->m_support_object_elevation_description_line));
            };
            current_group->append_line(line);
            current_page->descriptions.push_back("support_object_elevation");
        } else if (boost::starts_with(full_line, "print_host_upload_description")) {
            TabPrinter* tab = nullptr;
            if ((tab = dynamic_cast<TabPrinter*>(this)) == nullptr) continue;
            wxString description_line_text = wxString::Format(_L(""
                "Note: All parameters from this group are moved to the Physical Printer settings (see changelog).\n\n"
                "A new Physical Printer profile is created by clicking on the \"cog\" icon right of the Printer profiles combo box, "
                "by selecting the \"Add physical printer\" item in the Printer combo box. The Physical Printer profile editor opens "
                "also when clicking on the \"cog\" icon in the Printer settings tab. The Physical Printer profiles are being stored "
                "into %s/physical_printer directory."), SLIC3R_APP_KEY);

            current_line = { "", "" };
            current_line.full_width = 1;
            current_line.widget = [tab, description_line_text](wxWindow* parent) {
                return tab->description_line_widget(parent, tab->m_presets->get_selected_preset().printer_technology() == ptFFF ?
                    &tab->m_fff_print_host_upload_description_line : &tab->m_sla_print_host_upload_description_line,
                    description_line_text);
            };
            current_group->append_line(current_line);
            current_page->descriptions.push_back("print_host_upload");
        } else if(boost::starts_with(full_line, "post_process_explanation")) {
            TabPrint* tab = nullptr;
            if ((tab = dynamic_cast<TabPrint*>(this)) == nullptr) continue;
            Line line{ "", "" };
            line.full_width = 1;
            line.widget = [this, tab](wxWindow* parent) {
                return description_line_widget(parent, &(tab->m_post_process_explanation));
            };
            current_group->append_line(line);
            current_page->descriptions.push_back("post_process_explanation");
        } else if (boost::starts_with(full_line, "filament_ramming_parameters")) {
            Line thisline = current_group->create_single_option_line("filament_ramming_parameters");// { _(L("Ramming")), "" };
            thisline.widget = [this](wxWindow* parent) {
                auto ramming_dialog_btn = new wxButton(parent, wxID_ANY, _(L("Ramming settings")) + dots, wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
                ramming_dialog_btn->SetFont(Slic3r::GUI::wxGetApp().normal_font());
                wxGetApp().UpdateDarkUI(ramming_dialog_btn);
                auto sizer = new wxBoxSizer(wxHORIZONTAL);
                sizer->Add(ramming_dialog_btn);

                ramming_dialog_btn->Bind(wxEVT_BUTTON, ([this](wxCommandEvent& e)
                {
                    RammingDialog dlg(this, (m_config->option<ConfigOptionStrings>("filament_ramming_parameters"))->get_at(0));
                    if (dlg.ShowModal() == wxID_OK)
                        (m_config->option<ConfigOptionStrings>("filament_ramming_parameters"))->get_at(0) = dlg.get_parameters();
                }));
                return sizer;
            };
            current_group->append_line(thisline);
        } else if (boost::starts_with(full_line, "filament_overrides_page")) {
            TabFilament* tab = nullptr;
            if ((tab = dynamic_cast<TabFilament*>(this)) == nullptr) continue;
            pages.push_back(tab->create_filament_overrides_page());
        } else if (full_line == "unregular_pages") {
            TabPrinter* tab = nullptr;
            if ((tab = dynamic_cast<TabPrinter*>(this)) == nullptr) continue;
            tab->m_unregular_page_pos = pages.size();
        } else if (full_line == "bed_shape") {
            TabPrinter* tab = nullptr;
            if ((tab = dynamic_cast<TabPrinter*>(this)) == nullptr) continue;
            create_line_with_widget(current_group.get(), "bed_shape",
                "custom-svg-and-png-bed-textures_124612", [tab](wxWindow* parent) {
                    return 	tab->create_bed_shape_widget(parent);
                });
        } else if(full_line == "freq_purging_volumes") {
            //hack (see FreqChangedParams::init() in plater.cpp)
            current_line.label_tooltip = full_line;
        } else if (full_line == "gcode_substitutions") {
            TabPrint* tab = nullptr;
            if ((tab = dynamic_cast<TabPrint*>(this)) == nullptr) continue;
            create_line_with_widget(current_group.get(), "gcode_substitutions", "g-code-substitutions_301694", [tab](wxWindow* parent) {
                return tab->create_manage_substitution_widget(parent);
            });
            current_line = { "", "" };
            current_line.full_width = 1;
            current_line.widget = [this, tab](wxWindow* parent) {
                return tab->create_substitutions_widget(parent);
            };
            current_group->append_line(current_line);
            current_page->descriptions.push_back("substitutions_widget");
        } else if (full_line == "extruders_count") {
            ConfigOptionDef def;
            def.type = coInt,
                def.set_default_value(new ConfigOptionInt(1));
            def.label = L("Extruders");
            def.tooltip = L("Number of extruders of the printer.");
            def.min = 1;
            def.max = 256;
            def.mode = comAdvancedE | comPrusa;
            Option option(def, "extruders_count");
            current_group->append_single_option_line(option);
        } else if (full_line == "milling_count") {
            ConfigOptionDef def;
            def.type = coInt,
                def.set_default_value(new ConfigOptionInt(0));
            def.label = L("Milling cutters");
            def.tooltip = L("Number of milling heads.");
            def.min = 0;
            def.mode = comAdvancedE | comSuSi;
            Option option(def, "milling_count");
            current_group->append_single_option_line(option);
        } else if (full_line == "update_nozzle_diameter") {
            current_group->m_on_change = set_or_add(current_group->m_on_change, [this, idx_page](const t_config_option_key& opt_key, boost::any value)
            {
                TabPrinter* tab = nullptr;
                if ((tab = dynamic_cast<TabPrinter*>(this)) == nullptr) return;
                if (m_config->opt_bool("single_extruder_multi_material") && tab->m_extruders_count > 1 && opt_key.find("nozzle_diameter") != std::string::npos)
                {
                    SuppressBackgroundProcessingUpdate sbpu;
                    const double new_nd = boost::any_cast<double>(value);
                    std::vector<double> nozzle_diameters = static_cast<const ConfigOptionFloats*>(m_config->option("nozzle_diameter"))->values;

                    // if value was changed
                    if (fabs(nozzle_diameters[idx_page == 0 ? 1 : 0] - new_nd) > EPSILON)
                    {
                        const wxString msg_text = _L("This is a single extruder multimaterial printer, diameters of all extruders "
                            "will be set to the new value. Do you want to proceed?");
                        wxMessageDialog dialog(parent(), msg_text, _L("Nozzle diameter"), wxICON_WARNING | wxYES_NO);

                        DynamicPrintConfig new_conf = *m_config;
                        if (dialog.ShowModal() == wxID_YES) {
                            for (size_t i = 0; i < nozzle_diameters.size(); i++) {
                                if (i == idx_page)
                                    continue;
                                nozzle_diameters[i] = new_nd;
                            }
                        } else
                            nozzle_diameters[idx_page] = nozzle_diameters[idx_page == 0 ? 1 : 0];

                        new_conf.set_key_value("nozzle_diameter", (new ConfigOptionFloats(nozzle_diameters))->set_is_extruder_size(true));
                        load_config(new_conf);
                    }
                }

                update();
            });
        }else if(full_line == "reset_to_filament_color") {
            TabPrinter* tab = nullptr;
            if ((tab = dynamic_cast<TabPrinter*>(this)) == nullptr) continue;
            widget_t reset_to_filament_color = [this, idx_page, tab](wxWindow* parent) -> wxBoxSizer* {
                tab->m_reset_to_filament_color = new ScalableButton(parent, wxID_ANY, "undo", _L("Reset to Filament Color"),
                    wxDefaultSize, wxDefaultPosition, wxBU_LEFT | wxBU_EXACTFIT, true);
                ScalableButton* btn = tab->m_reset_to_filament_color;
                btn->SetFont(Slic3r::GUI::wxGetApp().normal_font());
                btn->SetSize(btn->GetBestSize());
                wxBoxSizer* sizer = new wxBoxSizer(wxHORIZONTAL);
                sizer->Add(btn);

                btn->Bind(wxEVT_BUTTON, [this, idx_page](wxCommandEvent& e)
                {
                    std::vector<std::string> colors = static_cast<const ConfigOptionStrings*>(m_config->option("extruder_colour"))->values;
                    colors[idx_page] = "";

                    DynamicPrintConfig new_conf = *m_config;
                    new_conf.set_key_value("extruder_colour", (new ConfigOptionStrings(colors))->set_is_extruder_size(true));
                    load_config(new_conf);

                    update_dirty();
                    update();
                });

                return sizer;
            };
            current_line = current_group->create_single_option_line("extruder_colour", "", idx_page);
            current_line.append_widget(reset_to_filament_color);
            current_group->append_line(current_line);
        }
    }

   /* fs::ifstream inFileUTF(ui_layout_file);tyj
    std::wbuffer_convert<std::codecvt_utf8<wchar_t>> inFilebufConverted(inFileUTF.rdbuf());
    std::wistream inFileConverted(&inFilebufConverted);
    for (std::wstring s; getline(inFileConverted, s); )
    {
        std::wcout << p_list_utf.c_str() << '\n' << s << '\n';
        if (fs::exists(s))
            std::wcout << "File exists!\n";
        else
            std::wcout << "File DOES NOT exist!\n";
    }*/
#ifdef __WXMSW__
//    if (!no_page_yet)
//        layout_page(current_page);
#endif

    if(logs) Slic3r::slic3r_log->info("settings gui") << "END create settings  " << setting_type_name << "\n";

    return pages;
}

void TabPrint::build()
{
    m_presets = &m_preset_bundle->fff_prints;
    load_initial_data();
    append(this->m_pages, create_pages("print.ui"));

}

// Reload current config (aka presets->edited_preset->config) into the UI fields.
void TabPrint::reload_config()
{
    this->compatible_widget_reload(m_compatible_printers);
    Tab::reload_config();
}

void TabPrint::update_description_lines()
{
    Tab::update_description_lines();

    if (m_preset_bundle->printers.get_selected_preset().printer_technology() == ptSLA)
        return;

    if (m_active_page && m_recommended_thin_wall_thickness_description_line
        && std::find(m_active_page->descriptions.begin(), m_active_page->descriptions.end(), "wall_thickness") != m_active_page->descriptions.end())
    {
        m_recommended_thin_wall_thickness_description_line->SetText(
            from_u8(PresetHints::recommended_thin_wall_thickness(*m_preset_bundle)));
    }
    if (m_active_page && m_top_bottom_shell_thickness_explanation
        && std::find(m_active_page->descriptions.begin(), m_active_page->descriptions.end(), "top_bottom_shell") != m_active_page->descriptions.end())
    {
        m_top_bottom_shell_thickness_explanation->SetText(
            from_u8(PresetHints::top_bottom_shell_thickness_explanation(*m_preset_bundle)));
    }
    if (m_active_page && m_recommended_extrusion_width_description_line
        && std::find(m_active_page->descriptions.begin(), m_active_page->descriptions.end(), "extrusion_width") != m_active_page->descriptions.end())
    {
        m_recommended_extrusion_width_description_line->SetText(
            from_u8(PresetHints::recommended_extrusion_width(*m_preset_bundle)));
    }

    if (m_active_page && m_post_process_explanation && std::find(m_active_page->descriptions.begin(), m_active_page->descriptions.end(), "post_process_explanation") != m_active_page->descriptions.end()) {
        m_post_process_explanation->SetText(
            _L("Post processing scripts shall modify G-code file in place."));
#ifndef __linux__
//        m_post_process_explanation->SetPathEnd("post-processing-scripts_283913");
#endif // __linux__
    }

    // upadte G-code substitutions from the current configuration
    if (m_active_page && m_subst_manager.is_active() &&std::find(m_active_page->descriptions.begin(), m_active_page->descriptions.end(), "substitutions_widget") != m_active_page->descriptions.end()) {
        m_subst_manager.update_from_config();
        if (m_del_all_substitutions_btn)
            m_del_all_substitutions_btn->Show(!m_subst_manager.is_empty_substitutions());
    }
}

void TabPrint::toggle_options()
{
    if (!m_active_page) return;

    m_config_manipulation.toggle_print_fff_options(m_config);
}

void TabPrint::update()
{
    if (m_preset_bundle->printers.get_selected_preset().printer_technology() == ptSLA)
        return; // ys_FIXME

    m_update_cnt++;

    // see https://github.com/prusa3d/PrusaSlicer/issues/6814
    // ysFIXME: It's temporary workaround and should be clewer reworked:
    // Note: This workaround works till "support_material" and "overhangs" is exclusive sets of mutually no-exclusive parameters.
    // But it should be corrected when we will have more such sets.
    // Disable check of the compatibility of the "support_material" and "overhangs" options for saved user profile
    // NOTE: Initialization of the support_material_overhangs_queried value have to be processed just ones
    if (!m_config_manipulation.is_initialized_support_material_overhangs_queried())
    {
        const Preset& selected_preset = m_preset_bundle->fff_prints.get_selected_preset();
        bool is_user_and_saved_preset = !selected_preset.is_system && !selected_preset.is_dirty;
        bool support_material_overhangs_queried = m_config->opt_bool("support_material") && m_config->option("overhangs_width_speed")->getFloat() == 0;
        m_config_manipulation.initialize_support_material_overhangs_queried(is_user_and_saved_preset && support_material_overhangs_queried);
    }

    m_config_manipulation.update_print_fff_config(m_config, true);

    update_description_lines();
    Layout();

    m_update_cnt--;

    if (m_update_cnt==0) {
        toggle_options();

        // update() could be called during undo/redo execution
        // Update of objectList can cause a crash in this case (because m_objects doesn't match ObjectList) 
        if (!wxGetApp().plater()->inside_snapshot_capture())
            wxGetApp().obj_list()->update_and_show_object_settings_item();

        wxGetApp().mainframe->on_config_changed(m_config);
    }
}

void TabPrint::clear_pages()
{
    Tab::clear_pages();

    m_recommended_thin_wall_thickness_description_line = nullptr;
    m_recommended_extrusion_width_description_line = nullptr;
    m_top_bottom_shell_thickness_explanation = nullptr;
    m_post_process_explanation = nullptr;
}

bool Tab::validate_custom_gcode(const wxString& title, const std::string& gcode)
{
    std::vector<std::string> tags;
    bool invalid = GCodeProcessor::contains_reserved_tags(gcode, 5, tags);
    if (invalid) {
        std::string lines = ":\n";
        for (const std::string& keyword : tags)
            lines += ";" + keyword + "\n";
        wxString reports = format_wxstr(
            _L_PLURAL("The following line %s contains reserved keywords.\nPlease remove it, as it may cause problems in G-code visualization and printing time estimation.", 
                      "The following lines %s contain reserved keywords.\nPlease remove them, as they may cause problems in G-code visualization and printing time estimation.", 
                      tags.size()),
            lines);
        //wxMessageDialog dialog(wxGetApp().mainframe, reports, _L("Found reserved keywords in") + " " + _(title), wxICON_WARNING | wxOK);
        MessageDialog dialog(wxGetApp().mainframe, reports, _L("Found reserved keywords in") + " " + _(title), wxICON_WARNING | wxOK);
        dialog.ShowModal();
    }
    return !invalid;
}

//void validate_custom_gcode_cb(Tab* tab, ConfigOptionsGroupShp opt_group, const t_config_option_key& opt_key, const boost::any& value) {
//    tab->validate_custom_gcodes_was_shown = !Tab::validate_custom_gcode(opt_group->title, boost::any_cast<std::string>(value));
//    tab->update_dirty();
//    tab->on_value_change(opt_key, value);
//}

PageShp TabFilament::create_filament_overrides_page()
{
    PageShp page = create_options_page(L("Filament Overrides"), "wrench");
    ConfigOptionsGroupShp optgroup = page->new_optgroup(L("Retraction"));

    auto append_single_option_line = [optgroup, this](const std::string& opt_key, int opt_index)
    {
        Line line {"",""};
        if (opt_key == "filament_retract_lift_above" || opt_key == "filament_retract_lift_below") {
            Option opt = optgroup->get_option_and_register(opt_key);
            opt.opt.label = opt.opt.get_full_label();
            line = optgroup->create_single_option_line(opt);
        } else {
            line = optgroup->create_single_option_line(optgroup->get_option_and_register(opt_key));
        }

        line.near_label_widget = [this, optgroup, opt_key, opt_index](wxWindow* parent) {
            wxCheckBox* check_box = new wxCheckBox(parent, wxID_ANY, "");

            check_box->Bind(wxEVT_CHECKBOX, [optgroup, opt_key, opt_index](wxCommandEvent& evt) {
                const bool is_checked = evt.IsChecked();
                Field* field = optgroup->get_fieldc(opt_key, opt_index);
                if (field) {
                    field->toggle(is_checked);
                    if (is_checked)
                        field->set_last_meaningful_value();
                    else
                        field->set_na_value();
                }
            }, check_box->GetId());

            m_overrides_options[opt_key] = check_box;
            return check_box;
        };

        optgroup->append_line(line);
    };

    const int extruder_idx = 0; // #ys_FIXME

    for (const std::string opt_key : {  "filament_retract_before_wipe",
                                        "filament_retract_length",
                                        "filament_retract_lift",
                                        "filament_retract_lift_above",
                                        "filament_retract_lift_below",
                                        "filament_retract_speed",
                                        "filament_deretract_speed",
                                        "filament_retract_restart_extra",
                                        "filament_retract_before_travel",
                                        "filament_retract_layer_change",
                                        "filament_seam_gap",
                                        "filament_wipe",
                                        "filament_wipe_extra_perimeter",
                                        "filament_wipe_inside_depth",
                                        "filament_wipe_inside_end",
                                        "filament_wipe_inside_start",
                                        "filament_wipe_only_crossing",
                                        "filament_wipe_speed"
                                     })
        append_single_option_line(opt_key, extruder_idx);

    return page;
}

void TabFilament::update_filament_overrides_page()
{
    if (!m_active_page || m_active_page->title() != "Filament Overrides")
        return;
    Page* page = m_active_page;

    const auto og_it = std::find_if(page->m_optgroups.begin(), page->m_optgroups.end(), [](const ConfigOptionsGroupShp og) { return og->title == "Retraction"; });
    if (og_it == page->m_optgroups.end())
        return;
    ConfigOptionsGroupShp optgroup = *og_it;

    std::vector<std::string> opt_keys = {   "filament_retract_before_wipe",
                                            "filament_retract_length",
                                            "filament_retract_lift",
                                            "filament_retract_lift_above",
                                            "filament_retract_lift_below",
                                            "filament_retract_speed",
                                            "filament_deretract_speed",
                                            "filament_retract_restart_extra",
                                            "filament_retract_before_travel",
                                            "filament_retract_layer_change",
                                            "filament_seam_gap",
                                            "filament_wipe",
                                            "filament_wipe_inside_end",
                                            "filament_wipe_inside_depth",
                                            "filament_wipe_inside_start",
                                            "filament_wipe_extra_perimeter",
                                            "filament_wipe_only_crossing",
                                            "filament_wipe_speed"
                                        };

    const int extruder_idx = 0; // #ys_FIXME

    const bool have_retract_length = m_config->option("filament_retract_length")->is_nil() ||
                                     m_config->opt_float("filament_retract_length", extruder_idx) > 0;

    for (const std::string& opt_key : opt_keys)
    {
        bool is_checked = opt_key=="filament_retract_length" ? true : have_retract_length;
        m_overrides_options[opt_key]->Enable(is_checked);

        is_checked &= !m_config->option(opt_key)->is_nil();
        m_overrides_options[opt_key]->SetValue(is_checked);

        Field* field = optgroup->get_fieldc(opt_key, extruder_idx);
        if (field)
            field->toggle(is_checked);
    }
}

void TabFilament::build()
{
    m_presets = &m_preset_bundle->filaments;
    load_initial_data();
    append(this->m_pages, create_pages("filament.ui"));

}

// Reload current config (aka presets->edited_preset->config) into the UI fields.
void TabFilament::reload_config()
{
    this->compatible_widget_reload(m_compatible_printers);
    this->compatible_widget_reload(m_compatible_prints);
    Tab::reload_config();
}

void TabFilament::update_volumetric_flow_preset_hints()
{
    wxString text;
    try {
        text = from_u8(PresetHints::maximum_volumetric_flow_description(*m_preset_bundle));
    } catch (std::exception &ex) {
        text = _(L("Volumetric flow hints not available")) + "\n\n" + from_u8(ex.what());
    }
    if(m_volumetric_speed_description_line)
        m_volumetric_speed_description_line->SetText(text);
}

void TabFilament::update_description_lines()
{
    Tab::update_description_lines();

    if (!m_active_page)
        return;

    if (std::find(m_active_page->descriptions.begin(), m_active_page->descriptions.end(), "cooling") != m_active_page->descriptions.end() && m_cooling_description_line)
        m_cooling_description_line->SetText(from_u8(PresetHints::cooling_description(m_presets->get_edited_preset())));
    if (std::find(m_active_page->descriptions.begin(), m_active_page->descriptions.end(), "volumetric_speed") != m_active_page->descriptions.end() && m_volumetric_speed_description_line)
        this->update_volumetric_flow_preset_hints();
}

void TabFilament::toggle_options()
{ //TODO: check prusa changes
    if (!m_active_page)
        return;

    if ( std::find(m_active_page->descriptions.begin(), m_active_page->descriptions.end(), "cooling") != m_active_page->descriptions.end())
    {
        bool fan_always_on = m_config->opt_bool("fan_always_on", 0);

        //get_field("max_fan_speed")->toggle(m_config->opt_float("fan_below_layer_time", 0) > 0);
        Field* min_print_speed_field = get_field("min_print_speed");
        if (min_print_speed_field)
            min_print_speed_field->toggle(m_config->opt_float("slowdown_below_layer_time", 0) > 0);

        // hidden 'cooling', it's now deactivated.
             //for (auto el : { "min_fan_speed", "disable_fan_first_layers" })
        //for (auto el : { "max_fan_speed", "fan_below_layer_time", "slowdown_below_layer_time", "min_print_speed" })
        //    get_field(el)->toggle(cooling);


        //for (auto el : { "min_fan_speed", "disable_fan_first_layers" })
        //    get_field(el)->toggle(fan_always_on);

        Field* max_fan_speed_field = get_field("max_fan_speed");
        if (max_fan_speed_field)
            max_fan_speed_field->toggle(m_config->opt_float("fan_below_layer_time", 0) > 0 || m_config->opt_float("slowdown_below_layer_time", 0) > 0);
    }

    update_filament_overrides_page();
}

void TabFilament::update()
{
    if (m_preset_bundle->printers.get_selected_preset().printer_technology() == ptSLA)
        return; // ys_FIXME

    m_update_cnt++;

    update_description_lines();
    Layout();

    toggle_options();

    m_update_cnt--;

    if (m_update_cnt == 0)
        wxGetApp().mainframe->on_config_changed(m_config);
}

void TabFilament::clear_pages()
{
    Tab::clear_pages();

    m_volumetric_speed_description_line = nullptr;
    m_cooling_description_line = nullptr;
}

wxSizer* Tab::description_line_widget(wxWindow* parent, ogStaticText* *StaticText, wxString text /*= wxEmptyString*/)
{
    *StaticText = new ogStaticText(parent, text);

//	auto font = (new wxSystemSettings)->GetFont(wxSYS_DEFAULT_GUI_FONT);
    (*StaticText)->SetFont(wxGetApp().normal_font());

    auto sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add(*StaticText, 1, wxEXPAND|wxALL, 0);
    return sizer;
}

bool Tab::saved_preset_is_dirty() const { return m_presets->saved_is_dirty(); }
void Tab::update_saved_preset_from_current_preset() { m_presets->update_saved_preset_from_current_preset(); }
bool Tab::current_preset_is_dirty() const { return m_presets->current_is_dirty(); }

void TabPrinter::build()
{
    m_presets = &m_preset_bundle->printers;
    m_printer_technology = m_presets->get_selected_preset().printer_technology();

    // For DiffPresetDialog we use options list which is saved in Searcher class.
    // Options for the Searcher is added in the moment of pages creation.
    // So, build first of all printer pages for non-selected printer technology...
    std::string def_preset_name = "- default " + std::string(m_printer_technology == ptSLA ? "FFF" : "SLA") + " -";
    m_config = &m_presets->find_preset(def_preset_name)->config;
    m_printer_technology == ptSLA ? build_fff() : build_sla();
    if (m_printer_technology == ptSLA)
        m_extruders_count_old = 0;// revert this value 

    // ... and than for selected printer technology
    load_initial_data();
    m_printer_technology == ptSLA ? build_sla() : build_fff();
}

void TabPrinter::build_fff()
{
    if (!m_pages.empty())
        m_pages.resize(0);
    // to avoid redundant memory allocation / deallocation during extruders count changing
    m_pages.reserve(30);

    auto* nozzle_diameter = dynamic_cast<const ConfigOptionFloats*>(m_config->option("nozzle_diameter"));
    m_initial_extruders_count = m_extruders_count = nozzle_diameter->values.size();
    wxGetApp().sidebar().update_objects_list_extruder_column(m_initial_extruders_count);

    auto* milling_diameter = dynamic_cast<const ConfigOptionFloats*>(m_config->option("milling_diameter"));
    m_initial_milling_count = m_milling_count = milling_diameter->values.size();

    const Preset* parent_preset = m_printer_technology == ptSLA ? nullptr // just for first build, if SLA printer preset is selected 
                                  : m_presets->get_selected_preset_parent();
    m_sys_extruders_count = parent_preset == nullptr ? 0 :
        static_cast<const ConfigOptionFloats*>(parent_preset->config.option("nozzle_diameter"))->values.size();
    m_sys_milling_count = parent_preset == nullptr ? 0 :
        static_cast<const ConfigOptionFloats*>(parent_preset->config.option("milling_diameter"))->values.size();

    append(this->m_pages, create_pages("printer_fff.ui"));


    if (m_unregular_page_pos >= 0) {
        TabPrinter* tab = nullptr;
        if ((tab = dynamic_cast<TabPrinter*>(this)) != nullptr)
            tab->build_unregular_pages(true);
    }

}

void TabPrinter::build_sla()
{
    if (!m_pages.empty())
        m_pages.resize(0);

    append(this->m_pages, create_pages("printer_sla.ui"));

}


void TabPrinter::extruders_count_changed(size_t extruders_count)
{
    bool is_count_changed = false;
    if (m_extruders_count != extruders_count) {
        m_extruders_count = extruders_count;
        m_preset_bundle->printers.get_edited_preset().set_num_extruders(extruders_count);
        m_preset_bundle->update_multi_material_filament_presets();
        is_count_changed = true;
    } else if (m_extruders_count == 1 &&
        m_preset_bundle->project_config.option<ConfigOptionFloats>("wiping_volumes_matrix")->values.size() > 1)
        m_preset_bundle->update_multi_material_filament_presets();


    if (is_count_changed) {
        /* This function should be call in any case because of correct updating/rebuilding
         * of unregular pages of a Printer Settings
         * But now that is only varies extruders & milling, it's not really needed to do bouble-update everytime.
         * It also crate bugs as some gui can't rebuild the tree two times.
         */
        build_unregular_pages(false);

        //propagate change
        on_value_change("extruders_count", (int)extruders_count);
        //update default tool_name => not used, no need to do that
        //ConfigOptionStrings* names = this->m_config->option<ConfigOptionStrings>("tool_name");
        //for (size_t ss = 0; ss < names->values.size(); ss++)
        //    if (names->values[ss] == "")
        //        names->values[ss] = std::to_string(ss);
        //update gui
        wxGetApp().sidebar().update_objects_list_extruder_column(extruders_count);
    }
}

void TabPrinter::milling_count_changed(size_t milling_count)
{
    bool is_count_changed = false;
    if (m_milling_count != milling_count) {
        m_milling_count = milling_count;
        m_preset_bundle->printers.get_edited_preset().set_num_milling(milling_count);
        is_count_changed = true;
    }

    if (is_count_changed) {
        /* This function should be call in any case because of correct updating/rebuilding
         * of unregular pages of a Printer Settings
         */
        build_unregular_pages(false);

        //propagate change
        on_value_change("milling_count", milling_count);
        //wxGetApp().sidebar().update_objects_list_milling_column(milling_count);
    }
}

void TabPrinter::append_option_line_kinematics(ConfigOptionsGroupShp optgroup, const std::string opt_key, const std::string override_sidetext)
{
    Option option = optgroup->get_option_and_register(opt_key, 0);
    if (!override_sidetext.empty()) {
        option.opt.sidetext = override_sidetext;
        option.opt.sidetext_width = override_sidetext.length() + 1;
    }
    Line line = Line{ _(option.opt.full_label), "" };
    option.opt.width = 10;
    line.append_option(option);
    if (m_use_silent_mode
        || m_printer_technology == ptSLA // just for first build, if SLA printer preset is selected 
        ) {
        option = optgroup->get_option_and_register(opt_key, 1);
        if (!override_sidetext.empty()) {
            option.opt.sidetext = override_sidetext;
            option.opt.sidetext_width = override_sidetext.length() + 1;
        }
        option.opt.width = 10;
        line.append_option(option);
    }
    optgroup->append_line(line);
}

PageShp TabPrinter::build_kinematics_page()
{
    PageShp page = create_options_page(L("Machine limits"), "cog");
    ConfigOptionsGroupShp optgroup;
    Line line{ "", "" };
    GCodeFlavor flavor = m_config->option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")->value;
    optgroup = page->new_optgroup(_L("Time estimation compensation"));
    if (flavor != gcfMarlinLegacy && flavor != gcfMarlinFirmware && flavor != gcfLerdge) {
        optgroup->append_single_option_line("time_estimation_compensation");
    }
    line = { _L("Flat time compensation"), wxString{""} };
    line.append_option(optgroup->get_option_and_register("time_start_gcode"));
    line.append_option(optgroup->get_option_and_register("time_toolchange"));
    optgroup->append_line(line);
    { // like optgroup->append_single_option_line("time_cost"); but with sidetext_width set
        Option option = optgroup->get_option_and_register("time_cost");
        option.opt.sidetext_width = 8;
        optgroup->append_single_option_line(option);
    }

    optgroup = page->new_optgroup(_L("Machine Limits"));
    optgroup->append_single_option_line("machine_limits_usage");
    line = { "", "" };
    line.full_width = 1;
    line.widget = [this](wxWindow* parent) {
        return description_line_widget(parent, &m_machine_limits_description_line);
    };
    optgroup->append_line(line);
    page->descriptions.push_back("machine_limits");

    if (m_use_silent_mode) {
        // Legend for OptionsGroups
        optgroup = page->new_optgroup("");
        auto line = Line{ "", "" };

        ConfigOptionDef def;
        def.type = coString;
        def.width = 15;
        def.gui_type = ConfigOptionDef::GUIType::legend;
        def.mode = comAdvancedE | comPrusa;
        def.tooltip = L("Values in this column are for Normal mode");
        def.set_default_value(new ConfigOptionString{ _u8L("Normal").data() });

        auto option = Option(def, "full_power_legend");
        line.append_option(option);

        def.tooltip = L("Values in this column are for Stealth mode");
        def.set_default_value(new ConfigOptionString{ _u8L("Stealth").data() });
        option = Option(def, "silent_legend");
        line.append_option(option);

        optgroup->append_line(line);
    }

    const std::vector<std::string> axes{ "x", "y", "z", "e" };
    optgroup = page->new_optgroup(L("Maximum feedrates"));
    for (const std::string& axis : axes) {
        append_option_line_kinematics(optgroup, "machine_max_feedrate_" + axis);
    }

    optgroup = page->new_optgroup(L("Maximum accelerations"));
    for (const std::string& axis : axes) {
        append_option_line_kinematics(optgroup, "machine_max_acceleration_" + axis);
    }
    append_option_line_kinematics(optgroup, "machine_max_acceleration_extruding");
    append_option_line_kinematics(optgroup, "machine_max_acceleration_retracting");
    append_option_line_kinematics(optgroup, "machine_max_acceleration_travel");

    optgroup = page->new_optgroup(L("Jerk limits"));
    for (const std::string& axis : axes) {
        append_option_line_kinematics(optgroup, "machine_max_jerk_" + axis);
    }

    optgroup = page->new_optgroup(L("Minimum feedrates"));
    append_option_line_kinematics(optgroup, "machine_min_extruding_rate");
    append_option_line_kinematics(optgroup, "machine_min_travel_rate");
    return page;
}

/* Previous name build_extruder_pages().
 *
 * This function was renamed because of now it implements not just an extruder pages building,
 * but "Machine limits" and "Single extruder MM setup" too
 * (These pages can changes according to the another values of a current preset)
 * */
void TabPrinter::build_unregular_pages(bool from_initial_build/* = false*/)
{
    size_t		n_before_extruders = m_unregular_page_pos;			//	Count of pages before Extruder pages
    bool changed = false;
    GCodeFlavor flavor = m_config->option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")->value;

    /* ! Freeze/Thaw in this function is needed to avoid call OnPaint() for erased pages
     * and be cause of application crash, when try to change Preset in moment,
     * when one of unregular pages is selected.
     *  */
    Freeze();

    // Add/delete Kinematics page
    size_t kinematic_page_id = 0;
    for (size_t i = 0; i < m_pages.size(); ++i) { // first make sure it's not there already
        if (m_pages[i]->title().find(L("Machine limits")) != std::string::npos) {
            if (m_rebuild_kinematics_page)
                m_pages.erase(m_pages.begin() + i);
            else
                kinematic_page_id = i;
            n_before_extruders = i;
            break;
        }
    }

    if (kinematic_page_id < n_before_extruders) {
        auto page = build_kinematics_page();
        changed = true;
        m_rebuild_kinematics_page = false;
        m_pages.insert(m_pages.begin() + n_before_extruders, page);
    }

    n_before_extruders++; // kinematic page is always here

    if (m_has_single_extruder_MM_page && (!m_config->opt_bool("single_extruder_multi_material") || m_extruders_count == 1))
    {
        // if we have a single extruder MM setup, add a page with configuration options:
        for (size_t i = 0; i < m_pages.size(); ++i) // first make sure it's not there already
            if (m_pages[i]->title().find(L("Single extruder MM setup")) != std::string::npos) {
                m_pages.erase(m_pages.begin() + i);
                changed = true;
                break;
            }
        m_has_single_extruder_MM_page = false;
    }
    if (from_initial_build ||
        (m_extruders_count > 1 && m_config->opt_bool("single_extruder_multi_material") && !m_has_single_extruder_MM_page)) {
        // create a page, but pretend it's an extruder page, so we can add it to m_pages ourselves
        auto page = create_options_page(L("Single extruder MM setup"), "printer");
        auto optgroup = page->new_optgroup(L("Single extruder multimaterial parameters"));
        optgroup->append_single_option_line("cooling_tube_retraction");
        optgroup->append_single_option_line("cooling_tube_length");
        optgroup->append_single_option_line("parking_pos_retraction");
        optgroup->append_single_option_line("extra_loading_move");
        optgroup->append_single_option_line("high_current_on_filament_swap");
        optgroup = page->new_optgroup(_(L("Advanced wipe tower purge volume calculs")));
        optgroup->append_single_option_line("wipe_advanced");
        optgroup->append_single_option_line("wipe_advanced_nozzle_melted_volume");
        optgroup->append_single_option_line("wipe_advanced_multiplier");
        optgroup->append_single_option_line("wipe_advanced_algo");
        if (from_initial_build) {
            page->clear();
        } else {
            m_pages.insert(m_pages.begin() + n_before_extruders, page);
            m_has_single_extruder_MM_page = true;
        }
        changed = true;
    }
    if(m_has_single_extruder_MM_page)
        n_before_extruders++;

    // Build missed extruder pages
    for (size_t extruder_idx = m_extruders_count_old; extruder_idx < m_extruders_count; ++extruder_idx) {
        std::vector<PageShp> pages = this->create_pages("extruder.ui", extruder_idx);
        assert(pages.size() == 1);
        if (m_pages.size() > n_before_extruders + 1)
            m_pages.insert(m_pages.begin() + n_before_extruders + extruder_idx, pages[0]);
        changed = true;
    }
    // # remove extra pages
    if (m_extruders_count < m_extruders_count_old) {
        m_pages.erase(m_pages.begin() + n_before_extruders + m_extruders_count,
            m_pages.begin() + n_before_extruders + m_extruders_count_old);
        changed = true;
    }
    m_extruders_count_old = m_extruders_count;

    // Build missed milling pages
    for (size_t milling_idx = m_milling_count_old; milling_idx < m_milling_count; ++milling_idx) {
        std::vector<PageShp> pages = this->create_pages("milling.ui", milling_idx);
        assert(pages.size() == 1);
        if (m_pages.size() > n_before_extruders + 1)
            m_pages.insert(m_pages.begin() + n_before_extruders + m_extruders_count + milling_idx, pages[0]);
        changed = true;

    }
    // # remove extra pages
    if (m_milling_count < m_milling_count_old) {
        m_pages.erase(m_pages.begin() + n_before_extruders + m_extruders_count + m_milling_count,
            m_pages.begin() + n_before_extruders + m_extruders_count + m_milling_count_old);
        changed = true;
    }
    m_milling_count_old = m_milling_count;

    Thaw();

    if(changed)

    if (from_initial_build && m_printer_technology == ptSLA)
        return; // next part of code is no needed to execute at this moment

    rebuild_page_tree();

    // Reload preset pages with current configuration values
    reload_config();
}

// this gets executed after preset is loaded and before GUI fields are updated
void TabPrinter::on_preset_loaded()
{
    // update the extruders count field
    auto   *nozzle_diameter = dynamic_cast<const ConfigOptionFloats*>(m_config->option("nozzle_diameter"));
    size_t extruders_count = nozzle_diameter->values.size();
    // update the GUI field according to the number of nozzle diameters supplied
    extruders_count_changed(extruders_count);

    //same for milling
    auto* milling_diameter = dynamic_cast<const ConfigOptionFloats*>(m_config->option("milling_diameter"));
    size_t milling_count = milling_diameter->values.size();
    milling_count_changed(milling_count);
}

void TabPrinter::update_pages()
{

    // update m_pages ONLY if printer technology is changed
    const PrinterTechnology new_printer_technology = m_presets->get_edited_preset().printer_technology();
    if (new_printer_technology == m_printer_technology)
        return;

    //clear all active pages before switching
    clear_pages();

    // set m_pages to m_pages_(technology before changing)
    m_printer_technology == ptFFF ? m_pages.swap(m_pages_fff) : m_pages.swap(m_pages_sla);

    // build Tab according to the technology, if it's not exist jet OR
    // set m_pages_(technology after changing) to m_pages
    // m_printer_technology will be set by Tab::load_current_preset()
    if (new_printer_technology == ptFFF)
    {
        if (m_pages_fff.empty())
        {
            build_fff();
            if (m_extruders_count > 1)
            {
                m_preset_bundle->update_multi_material_filament_presets();
                on_value_change("extruders_count", (int)m_extruders_count);
            }
        }
        else
            m_pages.swap(m_pages_fff);

         wxGetApp().sidebar().update_objects_list_extruder_column(m_extruders_count);
    }
    else
        m_pages_sla.empty() ? build_sla() : m_pages.swap(m_pages_sla);

    rebuild_page_tree();
}

void TabPrinter::reload_config()
{
    Tab::reload_config();

    // "extruders_count" doesn't update from the update_config(),
    // so update it implicitly
    if (m_active_page && m_active_page->get_field("extruders_count"))
        m_active_page->set_value("extruders_count", int(m_extruders_count));
    if (m_active_page && m_active_page->get_field("milling_count"))
        m_active_page->set_value("milling_count", int(m_milling_count));
}

void TabPrinter::activate_selected_page(std::function<void()> throw_if_canceled)
{
    Tab::activate_selected_page(throw_if_canceled);

    // "extruders_count" doesn't update from the update_config(),
    // so update it implicitly
    if (m_active_page && m_active_page->get_field("extruders_count"))
        m_active_page->set_value("extruders_count", int(m_extruders_count));
    if (m_active_page && m_active_page->get_field("milling_count"))
        m_active_page->set_value("milling_count", int(m_milling_count));
}

void TabPrinter::clear_pages()
{
    Tab::clear_pages();
    m_reset_to_filament_color = nullptr;
}

void TabPrinter::toggle_options()
{
    if (!m_active_page || m_presets->get_edited_preset().printer_technology() != ptFFF)
        return;

    Field* field;

    const DynamicPrintConfig& print_config = m_preset_bundle->fff_prints.get_edited_preset().config;
    const DynamicPrintConfig& filament_config = m_preset_bundle->filaments.get_edited_preset().config;
    const DynamicPrintConfig& printer_config = m_preset_bundle->printers.get_edited_preset().config;

    // Print config values
    DynamicPrintConfig full_print_config;
    full_print_config.apply(print_config);
    full_print_config.apply(filament_config);
    full_print_config.apply(printer_config);

    bool have_multiple_extruders = m_extruders_count > 1;
    field = get_field("toolchange_gcode");
    if (field) field->toggle(have_multiple_extruders);
    field = get_field("single_extruder_multi_material");
    if (field) field->toggle(have_multiple_extruders);

    //thumbnails
    bool custom_color = m_config->opt_bool("thumbnails_custom_color");
    field = get_field("thumbnails_color");
    if (field) field->toggle(custom_color);

    //firmware
    bool have_remaining_times = m_config->opt_bool("remaining_times");
    field = get_field("remaining_times_type");
    if (field) field->toggle(have_remaining_times);
	
    bool have_arc_fitting = m_config->opt_bool("arc_fitting");
    field = get_field("arc_fitting_tolerance");
    if (field) field->toggle(have_arc_fitting);

    auto flavor = m_config->option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")->value;
    bool is_marlin_flavor = flavor == gcfMarlinLegacy || flavor == gcfMarlinFirmware;
    // Disable silent mode for non-marlin firmwares.
    field = get_field("silent_mode");
    if (field) field->toggle(is_marlin_flavor);

    if (m_config->option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")->value == gcfKlipper)
        GCodeWriter::PausePrintCode = "PAUSE";
    else 
        GCodeWriter::PausePrintCode = "M601";

    if (m_last_gcode_flavor != uint8_t(m_config->option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")->value)) {
        m_last_gcode_flavor = uint8_t(m_config->option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")->value);
        m_rebuild_kinematics_page = true;
    }

    if (m_use_silent_mode != (m_last_gcode_flavor == gcfMarlinLegacy || m_last_gcode_flavor == gcfMarlinFirmware) && m_config->opt_bool("silent_mode")) {
        m_rebuild_kinematics_page = true;
        m_use_silent_mode = (m_last_gcode_flavor == gcfMarlinLegacy || m_last_gcode_flavor == gcfMarlinFirmware) && m_config->opt_bool("silent_mode");
    }

    wxString extruder_number;
    long val;
    if (m_active_page->title().StartsWith("Extruder ", &extruder_number) && extruder_number.ToLong(&val) &&
        val > 0 && (size_t)val <= m_extruders_count)
    {
        size_t i = size_t(val) - 1;
        bool have_retract_length = m_config->opt_float("retract_length", i) > 0;

        // when using firmware retraction, firmware decides retraction length
        bool use_firmware_retraction = m_config->opt_bool("use_firmware_retraction");
        field = get_field("retract_length", i);
        if (field)
            field->toggle(!use_firmware_retraction);

        // user can customize travel length if we have retraction length or we"re using
        // firmware retraction
        field = get_field("retract_before_travel", i);
        if (field)
            field->toggle(have_retract_length || use_firmware_retraction);

        // user can customize other retraction options if retraction is enabled
        bool retraction = (have_retract_length || use_firmware_retraction);
        std::vector<std::string> vec = { "retract_lift", "retract_layer_change" };
        for (auto el : vec) {
            field = get_field(el, i);
            if (field)
                field->toggle(retraction);
        }

        // retract lift above / below only applies if using retract lift
        vec.resize(0);
        vec = { "retract_lift_above", "retract_lift_below", "retract_lift_top", "retract_lift_first_layer" };
        for (auto el : vec) {
            field = get_field(el, i);
            if (field)
                field->toggle(retraction && m_config->opt_float("retract_lift", i) > 0);
        }

        // some options only apply when not using firmware retraction
        vec.resize(0);
        vec = { "retract_speed", "deretract_speed", "retract_before_wipe", "retract_restart_extra", "wipe", "wipe_speed" , "wipe_only_crossing"};
        for (auto el : vec) {
            field = get_field(el, i);
            if (field)
                field->toggle(retraction && !use_firmware_retraction);
        }

        bool wipe = m_config->opt_bool("wipe", i) && have_retract_length;
        vec = { "retract_before_wipe", "wipe_only_crossing", "wipe_speed" };
        for (auto el : vec) {
            field = get_field(el, i);
            if (field)
                field->toggle(wipe);
        }

        // wipe_only_crossing can only work if avoid_crossing_perimeters
        if (!full_print_config.opt_bool("avoid_crossing_perimeters")) {
            field = get_field("wipe_only_crossing", i);
            if (field)
                field->toggle(false);
        }

        if (use_firmware_retraction && wipe) {
            //wxMessageDialog dialog(parent(),
            MessageDialog dialog(parent(),
                _(L("The Wipe option is not available when using the Firmware Retraction mode.\n"
                "\nShall I disable it in order to enable Firmware Retraction?")),
                _(L("Firmware Retraction")), wxICON_WARNING | wxYES | wxNO);

            DynamicPrintConfig new_conf = *m_config;
            if (dialog.ShowModal() == wxID_YES) {
                auto wipe = static_cast<ConfigOptionBools*>(m_config->option("wipe")->clone());
                for (size_t w = 0; w < wipe->values.size(); w++)
                    wipe->values[w] = false;
                new_conf.set_key_value("wipe", wipe);
            } else {
                new_conf.set_key_value("use_firmware_retraction", new ConfigOptionBool(false));
            }
            load_config(new_conf);
        }

        field = get_field("retract_length_toolchange", i);
        if (field)
            field->toggle(have_multiple_extruders);

        bool toolchange_retraction = m_config->opt_float("retract_length_toolchange", i) > 0;
        field = get_field("retract_restart_extra_toolchange", i);
        if (field)
            field->toggle(have_multiple_extruders && toolchange_retraction);
    }
    if (m_has_single_extruder_MM_page) {
        bool have_advanced_wipe_volume = m_config->opt_bool("wipe_advanced");
        for (auto el : { "wipe_advanced_nozzle_melted_volume", "wipe_advanced_multiplier", "wipe_advanced_algo" }) {
            Field *field = get_field(el);
            if (field)
                field->toggle(have_advanced_wipe_volume);
        }
    }

    if (std::find(m_active_page->descriptions.begin(), m_active_page->descriptions.end(), "machine_limits") != m_active_page->descriptions.end() && m_machine_limits_description_line) {
//        assert(m_config->option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")->value == gcfMarlinLegacy
//            || m_config->option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")->value == gcfMarlinFirmware);
        const auto *machine_limits_usage = m_config->option<ConfigOptionEnum<MachineLimitsUsage>>("machine_limits_usage");
        bool enabled = machine_limits_usage->value != MachineLimitsUsage::Ignore;
        bool silent_mode = (m_last_gcode_flavor == gcfMarlinLegacy || m_last_gcode_flavor == gcfMarlinFirmware) && m_config->opt_bool("silent_mode");
        int  max_field = silent_mode ? 2 : 1;
        for (const std::string &opt : Preset::machine_limits_options())
            for (int i = 0; i < max_field; ++i) {
                Field* field = get_field(opt, i);
                if (field)
                    field->toggle(enabled);
            }
        update_machine_limits_description(machine_limits_usage->value);
    }

    //z step checks
    double z_step = m_config->opt_float("z_step");
    if(z_step > 0){
        int64_t z_step_Mlong = (int64_t)(z_step * 1000000.);
        DynamicPrintConfig new_conf;
        bool has_changed = false;
        const std::vector<double>& nozzle_diameters = m_config->option<ConfigOptionFloats>("nozzle_diameter")->values;
        const std::vector<FloatOrPercent>& min_layer_height = m_config->option<ConfigOptionFloatsOrPercents>("min_layer_height")->values;
        for (int i = 0; i < min_layer_height.size(); i++) {
            if(!min_layer_height[i].percent)
                if (min_layer_height[i].value != 0 && (int64_t)(min_layer_height[i].value * 1000000.) % z_step_Mlong != 0) {
                    if (!has_changed)
                        new_conf = *m_config;
                    new_conf.option<ConfigOptionFloatsOrPercents>("min_layer_height")->values[i].value = std::max(z_step, Slic3r::check_z_step(min_layer_height[i].value, z_step));
                    has_changed = true;
                }
        }
        std::vector<FloatOrPercent> max_layer_height = m_config->option<ConfigOptionFloatsOrPercents>("max_layer_height")->values;
        for (int i = 0; i < max_layer_height.size(); i++) {
            if (!max_layer_height[i].percent)
                if ((int64_t)(max_layer_height[i].value * 1000000.) % z_step_Mlong != 0) {
                    if (!has_changed)
                        new_conf = *m_config;
                    new_conf.option<ConfigOptionFloatsOrPercents>("max_layer_height")->values[i].value = std::max(z_step, Slic3r::check_z_step(max_layer_height[i].value, z_step));
                    has_changed = true;
                }
        }
        if (has_changed) {
            load_config(new_conf);
        }
    }
}

void TabPrinter::update()
{
    m_update_cnt++;
    m_presets->get_edited_preset().printer_technology() == ptFFF ? update_fff() : update_sla();
    m_update_cnt--;

    update_description_lines();
    Layout();

    if (m_update_cnt == 0)
        wxGetApp().mainframe->on_config_changed(m_config);
}

void TabPrinter::update_fff()
{
    if (m_use_silent_mode != m_config->opt_bool("silent_mode")) {
        m_rebuild_kinematics_page = true;
        m_use_silent_mode = m_config->opt_bool("silent_mode");
    }

    // not used
    //const auto flavor = m_config->option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")->value;
    //bool supports_travel_acceleration = (flavor == gcfMarlinFirmware || flavor == gcfRepRap);
    //bool supports_min_feedrates = (flavor == gcfMarlinFirmware || flavor == gcfMarlinLegacy);
    //if (m_supports_travel_acceleration != supports_travel_acceleration || m_supports_min_feedrates != supports_min_feedrates) {
    //    m_rebuild_kinematics_page = true;
    //    m_supports_travel_acceleration = supports_travel_acceleration;
    //    m_supports_min_feedrates = supports_min_feedrates;
    //}

    toggle_options();
}

void TabPrinter::update_sla()
{ ; }

void Tab::update_ui_items_related_on_parent_preset(const Preset* selected_preset_parent)
{
    m_is_default_preset = selected_preset_parent != nullptr && selected_preset_parent->is_default;

    m_bmp_non_system = selected_preset_parent ? &m_bmp_value_unlock : &m_bmp_white_bullet;
    m_ttg_non_system = selected_preset_parent ? &m_ttg_value_unlock : &m_ttg_white_bullet_ns;
    m_tt_non_system  = selected_preset_parent ? &m_tt_value_unlock  : &m_ttg_white_bullet_ns;
    m_tt_non_system_script = selected_preset_parent ? &m_tt_value_unlock_script : &m_ttg_white_bullet_ns;
}

// Initialize the UI from the current preset
void Tab::load_current_preset()
{
    const Preset& preset = m_presets->get_edited_preset();

    update_btns_enabling();

    update();
    if (m_type == Slic3r::Preset::TYPE_PRINTER) {
        // For the printer profile, generate the extruder pages.
        if (preset.printer_technology() == ptFFF)
            on_preset_loaded();
        else
            wxGetApp().sidebar().update_objects_list_extruder_column(1);
    }
    // Reload preset pages with the new configuration values.
    reload_config();

    update_ui_items_related_on_parent_preset(m_presets->get_selected_preset_parent());


    // apply duplicate_distance for print preset
    if (m_type == Preset::TYPE_FFF_PRINT || m_type == Preset::TYPE_SLA_PRINT) {
        wxGetApp().mainframe->plater()->canvas3D()->set_arrange_settings(m_presets->get_edited_preset().config, m_presets->get_edited_preset().printer_technology());
    }

//	m_undo_to_sys_btn->Enable(!preset.is_default);

#if 0
    // use CallAfter because some field triggers schedule on_change calls using CallAfter,
    // and we don't want them to be called after this update_dirty() as they would mark the
    // preset dirty again
    // (not sure this is true anymore now that update_dirty is idempotent)
    wxTheApp->CallAfter([this]
#endif
    {
        // checking out if this Tab exists till this moment
        if (!wxGetApp().checked_tab(this))
            return;
        update_tab_ui();

        // update show/hide tabs
        //merill note: this is a bit of anti-inheritance pattern
        if (m_type == Slic3r::Preset::TYPE_PRINTER) {
            const PrinterTechnology printer_technology = m_presets->get_edited_preset().printer_technology();
            const PrinterTechnology old_printer_technology = static_cast<TabPrinter*>(this)->m_printer_technology;
            if (printer_technology != old_printer_technology)
            {
                // The change of the technology requires to remove some of unrelated Tabs
                // During this action, wxNoteBook::RemovePage invoke wxEVT_NOTEBOOK_PAGE_CHANGED
                // and as a result a function select_active_page() is called fron Tab::OnActive()
                // But we don't need it. So, to avoid activation of the page, set m_active_page to NULL 
                // till unusable Tabs will be deleted
                Page* tmp_page = m_active_page;
                m_active_page = nullptr;
                for (auto tab : wxGetApp().tabs_list) {
                    if (tab->type() == Preset::TYPE_PRINTER) { // Printer tab shouln't be swapped
                        int cur_selection = wxGetApp().tab_panel()->GetSelection();
#ifdef _USE_CUSTOM_NOTEBOOK
                        //update icon
                        int icon_size = 0;
                        try {
                            icon_size = atoi(wxGetApp().app_config->get("tab_icon_size").c_str());
                        }
                        catch (std::exception e) {}
                        if (icon_size > 0) {
                            Notebook* notebook = dynamic_cast<Notebook*>(wxGetApp().tab_panel());
                            notebook->SetPageImage(notebook->FindFirstBtPage(tab), tab->icon_name(icon_size, printer_technology), icon_size);
                        }
#endif
                        if (cur_selection != 0)
                            wxGetApp().tab_panel()->SetSelection(wxGetApp().tab_panel()->GetPageCount() - 1);
                        continue;
                    }
                    if (tab->supports_printer_technology(printer_technology))
                    {
                        //search the other one to be replaced
                        for (auto tab_old : wxGetApp().tabs_list) {
                            if ((tab->type() & Preset::TYPE_TAB) == (tab_old->type() & Preset::TYPE_TAB) && tab_old->supports_printer_technology(old_printer_technology) ) {
                                wxGetApp().mainframe->change_tab(tab_old, tab);
//#ifdef _MSW_DARK_MODE // maybe it's needed???
//                        if (!wxGetApp().tabs_as_menu()) {
//                            std::string bmp_name = tab->type() == Slic3r::Preset::TYPE_FFF_FILAMENT      ? "spool" :
//                                                   tab->type() == Slic3r::Preset::TYPE_SLA_MATERIAL  ? "resin" : "cog";
//                            tab->Hide(); // #ys_WORKAROUND : Hide tab before inserting to avoid unwanted rendering of the tab
//                            dynamic_cast<Notebook*>(wxGetApp().tab_panel())->InsertBtPage(wxGetApp().tab_panel()->FindPage(this), tab, tab->title(), bmp_name);
//                        }
//                        else
//#                            
                            }
                        }
                    }
                }
                //wxGetApp().mainframe->update_layout();
                static_cast<TabPrinter*>(this)->m_printer_technology = printer_technology;
                m_active_page = tmp_page;
//#ifdef _MSW_DARK_MODE // change_tab already call update_icon, no need to re-do it here.
//                if (!wxGetApp().tabs_as_menu())
//                    dynamic_cast<Notebook*>(wxGetApp().tab_panel())->SetPageImage(wxGetApp().tab_panel()->FindPage(this), printer_technology == ptFFF ? "printer" : "sla_printer");
//#endif
            }
            on_presets_changed();
            if (printer_technology == ptFFF) {
                static_cast<TabPrinter*>(this)->m_initial_extruders_count = static_cast<const ConfigOptionFloats*>(m_presets->get_selected_preset().config.option("nozzle_diameter"))->values.size(); //static_cast<TabPrinter*>(this)->m_extruders_count;
                const Preset* parent_preset = m_presets->get_selected_preset_parent();
                static_cast<TabPrinter*>(this)->m_sys_extruders_count = parent_preset == nullptr ? 0 :
                    static_cast<const ConfigOptionFloats*>(parent_preset->config.option("nozzle_diameter"))->values.size();
                static_cast<TabPrinter*>(this)->m_initial_milling_count = static_cast<TabPrinter*>(this)->m_milling_count;
                static_cast<TabPrinter*>(this)->m_sys_milling_count = parent_preset == nullptr ? 0 :
                    static_cast<const ConfigOptionFloats*>(parent_preset->config.option("milling_diameter"))->values.size();
            }
        }
        else {
            on_presets_changed();
            if ((m_type & Preset::TYPE_PRINT1) != 0)
                update_frequently_changed_parameters();

            //update width/spacing links
            if (m_type == Preset::TYPE_FFF_PRINT) {
                //verify that spacings are set
                if (m_config && !m_config->update_phony({
                        &wxGetApp().preset_bundle->prints(wxGetApp().plater()->printer_technology()).get_edited_preset().config,
                        &wxGetApp().preset_bundle->materials(wxGetApp().plater()->printer_technology()).get_edited_preset().config,
                        &wxGetApp().preset_bundle->printers.get_edited_preset().config
                    }).empty()) {
                    update_dirty(); // will call on_presets_changed() again
                    reload_config();
                }
            }
        }

        m_opt_status_value = (m_presets->get_selected_preset_parent() ? osSystemValue : 0) | osInitValue;
        init_options_list();
        update_visibility();
        update_changed_ui();
    }
#if 0
    );
#endif
}

//Regerenerate content of the page tree.
void Tab::rebuild_page_tree()
{
    // get label of the currently selected item
    const auto sel_item = m_treectrl->GetSelection();
    const auto selected = sel_item ? m_treectrl->GetItemText(sel_item) : "";
    const auto rootItem = m_treectrl->GetRootItem();

    wxTreeItemId item;

    // Delete/Append events invoke wxEVT_TREE_SEL_CHANGED event.
    // To avoid redundant clear/activate functions call
    // suppress activate page before page_tree rebuilding
    m_disable_tree_sel_changed_event = true;
    m_treectrl->DeleteChildren(rootItem);

    for (auto p : m_pages)
    {
        if (!p->get_show())
            continue;
        auto itemId = m_treectrl->AppendItem(rootItem, translate_category(p->title(), m_type), p->iconID());
        m_treectrl->SetItemTextColour(itemId, p->get_item_colour());
        if (translate_category(p->title(), m_type) == selected)
            item = itemId;
        }
    if (!item) {
        // this is triggered on first load, so we don't disable the sel change event
        item = m_treectrl->GetFirstVisibleItem();
    }

    // allow activate page before selection of a page_tree item
    m_disable_tree_sel_changed_event = false;
    if (item)
            m_treectrl->SelectItem(item);
}

void Tab::update_btns_enabling()
{
    // we can delete any preset from the physical printer
    // and any user preset
        const Preset& preset = m_presets->get_edited_preset();
    m_btn_delete_preset->Show((m_type == Preset::TYPE_PRINTER && m_preset_bundle->physical_printers.has_selection())
                              || (!preset.is_default && !preset.is_system));

    if (m_btn_edit_ph_printer)
        m_btn_edit_ph_printer->SetToolTip( m_preset_bundle->physical_printers.has_selection() ?
                                           _L("Edit physical printer") : _L("Add physical printer"));
}

void Tab::update_preset_choice()
{
    m_presets_choice->update();
    update_btns_enabling();
}

// Called by the UI combo box when the user switches profiles, and also to delete the current profile.
// Select a preset by a name.If !defined(name), then the default preset is selected.
// If the current profile is modified, user is asked to save the changes.
void Tab::select_preset(std::string preset_name, bool delete_current /*=false*/, const std::string& last_selected_ph_printer_name/* =""*/)
{
    if (preset_name.empty()) {
        if (delete_current) {
            // Find an alternate preset to be selected after the current preset is deleted.
            const std::deque<Preset> &presets 		= m_presets->get_presets();
            size_t    				  idx_current   = m_presets->get_idx_selected();
            // Find the next visible preset.
            size_t 				      idx_new       = idx_current + 1;
            if (idx_new < presets.size())
                for (; idx_new < presets.size() && ! presets[idx_new].is_visible; ++ idx_new) ;
            if (idx_new == presets.size())
                for (idx_new = idx_current - 1; idx_new > 0 && ! presets[idx_new].is_visible; -- idx_new);
            preset_name = presets[idx_new].name;
        } else {
            // If no name is provided, select the "-- default --" preset.
            preset_name = m_presets->default_preset().name;
        }
    }
    assert(! delete_current || (m_presets->get_edited_preset().name != preset_name && m_presets->get_edited_preset().is_user()));
    bool current_dirty = ! delete_current && m_presets->current_is_dirty();
    bool print_tab     = m_presets->type() == Preset::TYPE_FFF_PRINT || m_presets->type() == Preset::TYPE_SLA_PRINT;
    bool printer_tab   = m_presets->type() == Preset::TYPE_PRINTER;
    bool canceled      = false;
    bool technology_changed = false;
    m_dependent_tabs.clear();
    if (current_dirty && ! may_discard_current_dirty_preset(nullptr, preset_name)) {
        canceled = true;
    } else if (print_tab) {
        // Before switching the print profile to a new one, verify, whether the currently active filament or SLA material
        // are compatible with the new print.
        // If it is not compatible and the current filament or SLA material are dirty, let user decide
        // whether to discard the changes or keep the current print selection.
        PresetWithVendorProfile printer_profile = m_preset_bundle->printers.get_edited_preset_with_vendor_profile();
        PrinterTechnology  printer_technology = printer_profile.preset.printer_technology();
        PresetCollection  &dependent = (printer_technology == ptFFF) ? m_preset_bundle->filaments : m_preset_bundle->sla_materials;
        bool 			   old_preset_dirty = dependent.current_is_dirty();
        bool 			   new_preset_compatible = is_compatible_with_print(dependent.get_edited_preset_with_vendor_profile(), 
            m_presets->get_preset_with_vendor_profile(*m_presets->find_preset(preset_name, true)), printer_profile);
        if (! canceled)
            canceled = old_preset_dirty && ! new_preset_compatible && ! may_discard_current_dirty_preset(&dependent, preset_name);
        if (! canceled) {
            // The preset will be switched to a different, compatible preset, or the '-- default --'.
            m_dependent_tabs.emplace_back((printer_technology == ptFFF) ? Preset::Type::TYPE_FFF_FILAMENT : Preset::Type::TYPE_SLA_MATERIAL);
            if (old_preset_dirty && ! new_preset_compatible)
                dependent.discard_current_changes();
        }
    } else if (printer_tab) {
        // Before switching the printer to a new one, verify, whether the currently active print and filament
        // are compatible with the new printer.
        // If they are not compatible and the current print or filament are dirty, let user decide
        // whether to discard the changes or keep the current printer selection.
        //
        // With the introduction of the SLA printer types, we need to support switching between
        // the FFF and SLA printers.
        const Preset 		&new_printer_preset     = *m_presets->find_preset(preset_name, true);
        const PresetWithVendorProfile new_printer_preset_with_vendor_profile = m_presets->get_preset_with_vendor_profile(new_printer_preset);
        PrinterTechnology    old_printer_technology = m_presets->get_edited_preset().printer_technology();
        PrinterTechnology    new_printer_technology = new_printer_preset.printer_technology();
        if (new_printer_technology == ptSLA && old_printer_technology == ptFFF && !wxGetApp().may_switch_to_SLA_preset(_L("New printer preset selected")))
            canceled = true;
        else {
            struct PresetUpdate {
                Preset::Type         tab_type;
                PresetCollection 	*presets;
                PrinterTechnology    technology;
                bool    	         old_preset_dirty;
                bool         	     new_preset_compatible;
            };
            std::vector<PresetUpdate> updates = {
                { Preset::Type::TYPE_FFF_PRINT,     &m_preset_bundle->fff_prints,   ptFFF },
                { Preset::Type::TYPE_SLA_PRINT,     &m_preset_bundle->sla_prints,   ptSLA },
                { Preset::Type::TYPE_FFF_FILAMENT,  &m_preset_bundle->filaments,    ptFFF },
                { Preset::Type::TYPE_SLA_MATERIAL,  &m_preset_bundle->sla_materials,ptSLA }
            };
            for (PresetUpdate &pu : updates) {
                pu.old_preset_dirty = (old_printer_technology == pu.technology) && pu.presets->current_is_dirty();
                pu.new_preset_compatible = (new_printer_technology == pu.technology) && is_compatible_with_printer(pu.presets->get_edited_preset_with_vendor_profile(), new_printer_preset_with_vendor_profile);
                if (!canceled)
                    canceled = pu.old_preset_dirty && !pu.new_preset_compatible && !may_discard_current_dirty_preset(pu.presets, preset_name);
            }
            if (!canceled) {
                for (PresetUpdate &pu : updates) {
                    // The preset will be switched to a different, compatible preset, or the '-- default --'.
                    if (pu.technology == new_printer_technology)
                        m_dependent_tabs.emplace_back(pu.tab_type);
                    if (pu.old_preset_dirty && !pu.new_preset_compatible)
                        pu.presets->discard_current_changes();
                }
            }
        }
        if (! canceled)
            technology_changed = old_printer_technology != new_printer_technology;
    }

    if (! canceled && delete_current) {
        // Delete the file and select some other reasonable preset.
        // It does not matter which preset will be made active as the preset will be re-selected from the preset_name variable.
        // The 'external' presets will only be removed from the preset list, their files will not be deleted.
        try {
            m_presets->delete_current_preset();
        } catch (const std::exception & /* e */) {
            //FIXME add some error reporting!
            canceled = true;
        }
    }

    if (canceled) {
        if (m_type == Preset::TYPE_PRINTER) {
            if (!last_selected_ph_printer_name.empty() &&
                m_presets->get_edited_preset().name == PhysicalPrinter::get_preset_name(last_selected_ph_printer_name)) {
                // If preset selection was canceled and previously was selected physical printer, we should select it back
                m_preset_bundle->physical_printers.select_printer(last_selected_ph_printer_name);
            }
            if (m_preset_bundle->physical_printers.has_selection()) {
                // If preset selection was canceled and physical printer was selected
                // we must disable selection marker for the physical printers
                m_preset_bundle->physical_printers.unselect_printer();
        }
        }

        update_tab_ui();

        // Trigger the on_presets_changed event so that we also restore the previous value in the plater selector,
        // if this action was initiated from the plater.
        on_presets_changed();
    } else {
        if (current_dirty)
            m_presets->discard_current_changes();

        const bool is_selected = m_presets->select_preset_by_name(preset_name, false) || delete_current;
        assert(m_presets->get_edited_preset().name == preset_name || ! is_selected);
        // Mark the print & filament enabled if they are compatible with the currently selected preset.
        // The following method should not discard changes of current print or filament presets on change of a printer profile,
        // if they are compatible with the current printer.
        auto update_compatible_type = [delete_current](bool technology_changed, bool on_page, bool show_incompatible_presets) {
            return (delete_current || technology_changed) ? PresetSelectCompatibleType::Always :
                   on_page                                ? PresetSelectCompatibleType::Never  :
                   show_incompatible_presets              ? PresetSelectCompatibleType::OnlyIfWasCompatible : PresetSelectCompatibleType::Always;
        };
        if (current_dirty || delete_current || print_tab || printer_tab)
            m_preset_bundle->update_compatible(
                update_compatible_type(technology_changed, print_tab,   (print_tab ? this : wxGetApp().get_tab(Preset::TYPE_FFF_PRINT))->m_show_incompatible_presets),
                update_compatible_type(technology_changed, false, 		wxGetApp().get_tab(Preset::TYPE_FFF_FILAMENT)->m_show_incompatible_presets));
        // Initialize the UI from the current preset.
        if (printer_tab)
            static_cast<TabPrinter*>(this)->update_pages();

        if (! is_selected && printer_tab)
        {
            /* There is a case, when :
             * after Config Wizard applying we try to select previously selected preset, but
             * in a current configuration this one:
             *  1. doesn't exist now,
             *  2. have another printer_technology
             * So, it is necessary to update list of dependent tabs
             * to the corresponding printer_technology
             */
            const PrinterTechnology printer_technology = m_presets->get_edited_preset().printer_technology();
            if (printer_technology == ptFFF && m_dependent_tabs.front() != Preset::Type::TYPE_FFF_PRINT)
                m_dependent_tabs = { Preset::Type::TYPE_FFF_PRINT, Preset::Type::TYPE_FFF_FILAMENT };
            else if (printer_technology == ptSLA && m_dependent_tabs.front() != Preset::Type::TYPE_SLA_PRINT)
                m_dependent_tabs = { Preset::Type::TYPE_SLA_PRINT, Preset::Type::TYPE_SLA_MATERIAL };
        }

        // check if there is something in the cache to move to the new selected preset
        apply_config_from_cache();

        load_current_preset();


    }

    if (technology_changed)
        wxGetApp().mainframe->technology_changed();
}

// If the current preset is dirty, the user is asked whether the changes may be discarded.
// if the current preset was not dirty, or the user agreed to discard the changes, 1 is returned.
bool Tab::may_discard_current_dirty_preset(PresetCollection* presets /*= nullptr*/, const std::string& new_printer_name /*= ""*/)
{
    if (presets == nullptr) presets = m_presets;

    UnsavedChangesDialog dlg(m_type, presets, new_printer_name);
    if (wxGetApp().app_config->get("default_action_on_select_preset") == "none" && dlg.ShowModal() == wxID_CANCEL)
        return false;

    if (dlg.save_preset())  // save selected changes
    {
        const std::vector<std::string>& unselected_options = dlg.get_unselected_options(presets->type());
        const std::string& name = dlg.get_preset_name();

        if (m_type == presets->type()) // save changes for the current preset from this tab
        {
            // revert unselected options to the old values
            presets->get_edited_preset().config.apply_only(presets->get_selected_preset().config, unselected_options);
            save_preset(name);
        }
        else
        {
            m_preset_bundle->save_changes_for_preset(name, presets->type(), unselected_options);

            // If filament preset is saved for multi-material printer preset,
            // there are cases when filament comboboxs are updated for old (non-modified) colors,
            // but in full_config a filament_colors option aren't.
            if (presets->type() == Preset::TYPE_FFF_FILAMENT && wxGetApp().extruders_edited_cnt() > 1)
                wxGetApp().plater()->force_filament_colors_update();
        }
    }
    else if (dlg.transfer_changes()) // move selected changes
    {
        std::vector<std::string> selected_options = dlg.get_selected_options();
        if (m_type == presets->type()) // move changes for the current preset from this tab
        {
            if (m_type == Preset::TYPE_PRINTER) {
                auto it = std::find(selected_options.begin(), selected_options.end(), "extruders_count");
                if (it != selected_options.end()) {
                    // erase "extruders_count" option from the list
                    selected_options.erase(it);
                    // cache the extruders count
                    static_cast<TabPrinter*>(this)->cache_extruder_cnt();
                }
            }

            // copy selected options to the cache from edited preset
            cache_config_diff(selected_options);
        } else
            wxGetApp().get_tab(presets->type())->cache_config_diff(selected_options);
    }

    return true;
}

void Tab::clear_pages()
{
    // invalidated highlighter, if any exists
    m_highlighter.invalidate();
    m_page_sizer->Clear(true);
    // clear pages from the controlls
    for (auto p : m_pages)
        p->clear();

    // nulling pointers
    m_parent_preset_description_line = nullptr;
    m_detach_preset_btn = nullptr;

    m_compatible_printers.checkbox  = nullptr;
    m_compatible_printers.btn       = nullptr;

    m_compatible_prints.checkbox    = nullptr;
    m_compatible_prints.btn         = nullptr;
}

void Tab::update_description_lines()
{
    if (m_active_page && m_active_page->title() == "Dependencies" && m_parent_preset_description_line)
        update_preset_description_line();
}

void Tab::activate_selected_page(std::function<void()> throw_if_canceled)
{
    if (!m_active_page)
        return;

    m_active_page->activate(m_mode, throw_if_canceled);
    update_changed_ui();
    update_description_lines();
    toggle_options();
}

bool Tab::tree_sel_change_delayed()
{
    // There is a bug related to Ubuntu overlay scrollbars, see https://github.com/prusa3d/PrusaSlicer/issues/898 and https://github.com/prusa3d/PrusaSlicer/issues/952.
    // The issue apparently manifests when Show()ing a window with overlay scrollbars while the UI is frozen. For this reason,
    // we will Thaw the UI prematurely on Linux. This means destroing the no_updates object prematurely.
#ifdef __linux__
    std::unique_ptr<wxWindowUpdateLocker> no_updates(new wxWindowUpdateLocker(this));
#else
    /* On Windows we use DoubleBuffering during rendering,
     * so on Window is no needed to call a Freeze/Thaw functions.
     * But under OSX (builds compiled with MacOSX10.14.sdk) wxStaticBitmap rendering is broken without Freeze/Thaw call.
     */
//#ifdef __WXOSX__  // Use Freeze/Thaw to avoid flickering during clear/activate new page
    wxWindowUpdateLocker noUpdates(this);
//#endif
#endif

    Page* page = nullptr;
    const auto sel_item = m_treectrl->GetSelection();
    const auto selection = sel_item ? m_treectrl->GetItemText(sel_item) : "";
    for (auto p : m_pages)
        if (translate_category(p->title(), m_type) == selection)
        {
            page = p.get();
            m_is_nonsys_values = page->m_is_nonsys_values;
            m_is_modified_values = page->m_is_modified_values;
            break;
        }
    if (page == nullptr || m_active_page == page)
        return false;

    // clear pages from the controls
    m_active_page = page;
    
    auto throw_if_canceled = std::function<void()>([this](){
#ifdef WIN32
            wxCheckForInterrupt(m_treectrl);
            if (m_page_switch_planned)
                throw UIBuildCanceled();
#else // WIN32
            (void)this; // silence warning
#endif
        });

    try {
        clear_pages();
        throw_if_canceled();

        if (wxGetApp().mainframe!=nullptr && wxGetApp().mainframe->is_active_and_shown_tab(this))
            activate_selected_page(throw_if_canceled);

    #ifdef __linux__
        no_updates.reset(nullptr);
    #endif

    update_undo_buttons();
        throw_if_canceled();

        m_hsizer->Layout();
        throw_if_canceled();
        Refresh();
    } catch (const UIBuildCanceled&) {
        if (m_active_page)
            m_active_page->clear();
        return true;
    }

    return false;
}

void Tab::OnKeyDown(wxKeyEvent& event)
{
    if (event.GetKeyCode() == WXK_TAB)
        m_treectrl->Navigate(event.ShiftDown() ? wxNavigationKeyEvent::IsBackward : wxNavigationKeyEvent::IsForward);
    else
        event.Skip();
}

void Tab::compare_preset()
{
    wxGetApp().mainframe->diff_dialog.show(m_type);
}

// Save the current preset into file.
// This removes the "dirty" flag of the preset, possibly creates a new preset under a new name,
// and activates the new preset.
// Wizard calls save_preset with a name "My Settings", otherwise no name is provided and this method
// opens a Slic3r::GUI::SavePresetDialog dialog.
void Tab::save_preset(std::string name /*= ""*/, bool detach)
{
    // since buttons(and choices too) don't get focus on Mac, we set focus manually
    // to the treectrl so that the EVT_* events are fired for the input field having
    // focus currently.is there anything better than this ?
//!	m_treectrl->OnSetFocus();

    if (name.empty()) {
        SavePresetDialog dlg(m_parent, m_type, detach ? _u8L("Detached") : "");
        auto result = dlg.ShowModal();
        // OK => ADD, APPLY => RENAME
        if (result != wxID_OK && result != wxID_APPLY)
            return;
        name = dlg.get_name();
    }

    // Print bed has to be updated, when printer preset is detached from the system preset
    if (detach && m_type == Preset::TYPE_PRINTER)
        m_config->opt_string("printer_model", true) = "";

    // Save the preset into Slic3r::data_dir / presets / section_name / preset_name.ini
    m_presets->save_current_preset(name, detach);

    // Print bed has to be updated, when printer preset is detached from the system preset
    if (detach && m_type == Preset::TYPE_PRINTER)
        wxGetApp().mainframe->on_config_changed(m_config);

    // Mark the print & filament enabled if they are compatible with the currently selected preset.
    // If saving the preset changes compatibility with other presets, keep the now incompatible dependent presets selected, however with a "red flag" icon showing that they are no more compatible.
    m_preset_bundle->update_compatible(PresetSelectCompatibleType::Never);
    // Add the new item into the UI component, remove dirty flags and activate the saved item.
    update_tab_ui();
    // Update the selection boxes at the plater.
    on_presets_changed();
    // If current profile is saved, "delete preset" button have to be enabled
    m_btn_delete_preset->Show();
    m_btn_delete_preset->GetParent()->Layout();

    if (m_type == Preset::TYPE_PRINTER)
        static_cast<TabPrinter*>(this)->m_initial_extruders_count = static_cast<TabPrinter*>(this)->m_extruders_count;
    if (m_type == Preset::TYPE_PRINTER)
        static_cast<TabPrinter*>(this)->m_initial_milling_count = static_cast<TabPrinter*>(this)->m_milling_count;

    // Parent preset is "default" after detaching, so we should to update UI values, related on parent preset  
    if (detach)
        update_ui_items_related_on_parent_preset(m_presets->get_selected_preset_parent());

    update_changed_ui();

    /* If filament preset is saved for multi-material printer preset, 
     * there are cases when filament comboboxs are updated for old (non-modified) colors, 
     * but in full_config a filament_colors option aren't.*/
    if (m_type == Preset::TYPE_FFF_FILAMENT && wxGetApp().extruders_edited_cnt() > 1)
        wxGetApp().plater()->force_filament_colors_update();

    {
        // Profile compatiblity is updated first when the profile is saved.
        // Update profile selection combo boxes at the depending tabs to reflect modifications in profile compatibility.
        std::vector<Preset::Type> dependent;
        switch (m_type) {
        case Preset::TYPE_FFF_PRINT:
            dependent = { Preset::TYPE_FFF_FILAMENT };
            break;
        case Preset::TYPE_SLA_PRINT:
            dependent = { Preset::TYPE_SLA_MATERIAL };
            break;
        case Preset::TYPE_PRINTER:
            if (static_cast<const TabPrinter*>(this)->m_printer_technology == ptFFF)
                dependent = { Preset::TYPE_FFF_PRINT, Preset::TYPE_FFF_FILAMENT };
            else
                dependent = { Preset::TYPE_SLA_PRINT, Preset::TYPE_SLA_MATERIAL };
            break;
        default:
            break;
        }
        for (Preset::Type preset_type : dependent)
            wxGetApp().get_tab(preset_type)->update_tab_ui();
    }

    // update preset comboboxes in DiffPresetDlg
    wxGetApp().mainframe->diff_dialog.update_presets(m_type);

    //when "Detach from system preset" makes the btton disappear after click on it and detaching of the profile from system profile
    if (detach)
        update_description_lines();
}

// Called for a currently selected preset.
void Tab::delete_preset()
{
    auto current_preset = m_presets->get_selected_preset();
    // Don't let the user delete the ' - default - ' configuration.
    std::string action = current_preset.is_external ? _utf8(L("remove")) : _utf8(L("delete"));
    // TRN  remove/delete

    PhysicalPrinterCollection& physical_printers = m_preset_bundle->physical_printers;
    wxString msg;
    if (m_presets_choice->is_selected_physical_printer())
    {
        PhysicalPrinter& printer = physical_printers.get_selected_printer();
        if (printer.preset_names.size() == 1) {
            if (m_presets_choice->del_physical_printer(_L("It's a last preset for this physical printer.")))
                Layout();
            return;
        }
        
        msg = format_wxstr(_L("Are you sure you want to delete \"%1%\" preset from the physical printer \"%2%\"?"), current_preset.name, printer.name);
    }
    else
    {
        if (m_type == Preset::TYPE_PRINTER && !physical_printers.empty())
        {
            // Check preset for delete in physical printers
            // Ask a customer about next action, if there is a printer with just one preset and this preset is equal to delete
            std::vector<std::string> ph_printers        = physical_printers.get_printers_with_preset(current_preset.name);
            std::vector<std::string> ph_printers_only   = physical_printers.get_printers_with_only_preset(current_preset.name);

            if (!ph_printers.empty()) {
                msg += _L_PLURAL("The physical printer below is based on the preset, you are going to delete.", 
                                 "The physical printers below are based on the preset, you are going to delete.", ph_printers.size());
                for (const std::string& printer : ph_printers)
                    msg += "\n    \"" + from_u8(printer) + "\",";
                msg.RemoveLast();
                msg += "\n" + _L_PLURAL("Note, that the selected preset will be deleted from this printer too.", 
                                        "Note, that the selected preset will be deleted from these printers too.", ph_printers.size()) + "\n\n";
            }

            if (!ph_printers_only.empty()) {
                msg += _L_PLURAL("The physical printer below is based only on the preset, you are going to delete.", 
                                 "The physical printers below are based only on the preset, you are going to delete.", ph_printers_only.size());
                for (const std::string& printer : ph_printers_only)
                    msg += "\n    \"" + from_u8(printer) + "\",";
                msg.RemoveLast();
                msg += "\n" + _L_PLURAL("Note, that this printer will be deleted after deleting the selected preset.",
                                        "Note, that these printers will be deleted after deleting the selected preset.", ph_printers_only.size()) + "\n\n";
            }
        }
    
        msg += from_u8((boost::format(_u8L("Are you sure you want to %1% the selected preset?")) % action).str());
    }

    action = current_preset.is_external ? _utf8(L("Remove")) : _utf8(L("Delete"));
    // TRN  Remove/Delete
    wxString title = from_u8((boost::format(_utf8(L("%1% Preset"))) % action).str());  //action + _(L(" Preset"));
    if (current_preset.is_default ||
        //wxID_YES != wxMessageDialog(parent(), msg, title, wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION).ShowModal())
        wxID_YES != MessageDialog(parent(), msg, title, wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION).ShowModal())
        return;

    // if we just delete preset from the physical printer
    if (m_presets_choice->is_selected_physical_printer()) {
        PhysicalPrinter& printer = physical_printers.get_selected_printer();

        // just delete this preset from the current physical printer
        printer.delete_preset(m_presets->get_edited_preset().name);
        // select first from the possible presets for this printer
        physical_printers.select_printer(printer);

        this->select_preset(physical_printers.get_selected_printer_preset_name());
        return;
    }

    // delete selected preset from printers and printer, if it's needed
    if (m_type == Preset::TYPE_PRINTER && !physical_printers.empty())
        physical_printers.delete_preset_from_printers(current_preset.name);

    // Select will handle of the preset dependencies, of saving & closing the depending profiles, and
    // finally of deleting the preset.
    this->select_preset("", true);
}

void Tab::toggle_show_hide_incompatible()
{
    m_show_incompatible_presets = !m_show_incompatible_presets;
    m_presets_choice->set_show_incompatible_presets(m_show_incompatible_presets);
    update_show_hide_incompatible_button();
    update_tab_ui();
}

void Tab::update_show_hide_incompatible_button()
{
    m_btn_hide_incompatible_presets->SetBitmap_(m_show_incompatible_presets ?
        m_bmp_show_incompatible_presets : m_bmp_hide_incompatible_presets);
    m_btn_hide_incompatible_presets->SetToolTip(m_show_incompatible_presets ?
        "Both compatible an incompatible presets are shown. Click to hide presets not compatible with the current printer." :
        "Only compatible presets are shown. Click to show both the presets compatible and not compatible with the current printer.");
}

void Tab::update_ui_from_settings()
{
    // Show the 'show / hide presets' button only for the print and filament tabs, and only if enabled
    // in application preferences.
    m_show_btn_incompatible_presets = wxGetApp().app_config->get("show_incompatible_presets")[0] == '1' ? true : false;
    bool show = m_show_btn_incompatible_presets && m_type != Slic3r::Preset::TYPE_PRINTER;
    Layout();
    show ? m_btn_hide_incompatible_presets->Show() :  m_btn_hide_incompatible_presets->Hide();
    // If the 'show / hide presets' button is hidden, hide the incompatible presets.
    if (show) {
        update_show_hide_incompatible_button();
    }
    else {
        if (m_show_incompatible_presets) {
            m_show_incompatible_presets = false;
            update_tab_ui();
        }
    }
}

void Tab::create_line_with_widget(ConfigOptionsGroup* optgroup, const std::string& opt_key, const std::string& path, widget_t widget)
{
    Line line = optgroup->create_single_option_line(opt_key);
    line.widget = widget;
    line.label_path = path;

    m_colored_Label_colors[opt_key] = m_default_label_clr;
    line.full_Label_color = &m_colored_Label_colors[opt_key];

    optgroup->append_line(line);
}

// Return a callback to create a Tab widget to mark the preferences as compatible / incompatible to the current printer.
wxSizer* Tab::compatible_widget_create(wxWindow* parent, PresetDependencies &deps)
{
    deps.checkbox = new wxCheckBox(parent, wxID_ANY, _(L("All")));
    deps.checkbox->SetFont(Slic3r::GUI::wxGetApp().normal_font());
    wxGetApp().UpdateDarkUI(deps.checkbox, false, true);
    deps.btn = new ScalableButton(parent, wxID_ANY, "printer", from_u8((boost::format(" %s %s") % _utf8(L("Set")) % std::string(dots.ToUTF8())).str()),
                                  wxDefaultSize, wxDefaultPosition, wxBU_LEFT | wxBU_EXACTFIT, true);
    deps.btn->SetFont(Slic3r::GUI::wxGetApp().normal_font());
    deps.btn->SetSize(deps.btn->GetBestSize());

    auto sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add((deps.checkbox), 0, wxALIGN_CENTER_VERTICAL);
    sizer->Add((deps.btn), 0, wxALIGN_CENTER_VERTICAL);

    deps.checkbox->Bind(wxEVT_CHECKBOX, ([this, &deps](wxCommandEvent e)
    {
        deps.btn->Enable(! deps.checkbox->GetValue());
        // All printers have been made compatible with this preset.
        if (deps.checkbox->GetValue())
            this->load_key_value(deps.key_list, std::vector<std::string> {});
        this->get_field(deps.key_condition)->toggle(deps.checkbox->GetValue());
        this->update_changed_ui();
    }) );

    deps.btn->Bind(wxEVT_BUTTON, ([this, parent, &deps](wxCommandEvent e)
    {
        // Collect names of non-default non-external profiles.
        PrinterTechnology printer_technology = m_preset_bundle->printers.get_edited_preset().printer_technology();
        PresetCollection &depending_presets  = (deps.type == Preset::TYPE_PRINTER) ? m_preset_bundle->printers : m_preset_bundle->prints(printer_technology);
        wxArrayString presets;
        for (size_t idx = 0; idx < depending_presets.size(); ++ idx)
        {
            Preset& preset = depending_presets.preset(idx);
            bool add = ! preset.is_default && ! preset.is_external;
            if (add && deps.type == Preset::TYPE_PRINTER)
                // Only add printers with the same technology as the active printer.
                add &= preset.printer_technology() == printer_technology;
            if (add)
                presets.Add(from_u8(preset.name));
        }

        wxMultiChoiceDialog dlg(parent, deps.dialog_title, deps.dialog_label, presets);
        wxGetApp().UpdateDlgDarkUI(&dlg);
        // Collect and set indices of depending_presets marked as compatible.
        wxArrayInt selections;
        auto *compatible_printers = dynamic_cast<const ConfigOptionStrings*>(m_config->option(deps.key_list));
        if (compatible_printers != nullptr || !compatible_printers->values.empty())
            for (auto preset_name : compatible_printers->values)
                for (size_t idx = 0; idx < presets.GetCount(); ++idx)
                    if (presets[idx] == preset_name) {
                        selections.Add(idx);
                        break;
                    }
        dlg.SetSelections(selections);
        std::vector<std::string> value;
        // Show the dialog.
        if (dlg.ShowModal() == wxID_OK) {
            selections.Clear();
            selections = dlg.GetSelections();
            for (auto idx : selections)
                value.push_back(presets[idx].ToUTF8().data());
            if (value.empty()) {
                deps.checkbox->SetValue(1);
                deps.btn->Disable();
            }
            // All depending_presets have been made compatible with this preset.
            this->load_key_value(deps.key_list, value);
            this->update_changed_ui();
        }
    }));

    return sizer;
}

// G-code substitutions

void SubstitutionManager::init(DynamicPrintConfig* config, wxWindow* parent, wxFlexGridSizer* grid_sizer)
{
    m_config = config;
    m_parent = parent;
    m_grid_sizer = grid_sizer;
    m_em = em_unit(parent);
}

void SubstitutionManager::validate_lenth()
{
    std::vector<std::string>& substitutions = m_config->option<ConfigOptionStrings>("gcode_substitutions")->values;
    if ((substitutions.size() % 4) != 0) {
        WarningDialog(m_parent, "Value of gcode_substitutions parameter will be cut to valid length",
            "Invalid length of gcode_substitutions parameter").ShowModal();
        substitutions.resize(substitutions.size() - (substitutions.size() % 4));
    }
}

bool SubstitutionManager::is_compatibile_with_ui()
{
    const std::vector<std::string>& substitutions = m_config->option<ConfigOptionStrings>("gcode_substitutions")->values;
    if (int(substitutions.size() / 4) != m_grid_sizer->GetEffectiveRowsCount() - 1) {
        ErrorDialog(m_parent, "Invalid compatibility between UI and BE", false).ShowModal();
        return false;
    }
    return true;
};

bool SubstitutionManager::is_valid_id(int substitution_id, const wxString& message)
{
    const std::vector<std::string>& substitutions = m_config->option<ConfigOptionStrings>("gcode_substitutions")->values;
    if (int(substitutions.size() / 4) < substitution_id) {
        ErrorDialog(m_parent, message, false).ShowModal();
        return false;
    }
    return true;
}

void SubstitutionManager::create_legend()
{
    if (!m_grid_sizer->IsEmpty())
        return;
    // name of the first column is empty
    m_grid_sizer->Add(new wxStaticText(m_parent, wxID_ANY, wxEmptyString));

    // Legend for another columns
    auto legend_sizer = new wxBoxSizer(wxHORIZONTAL); // "Find", "Replace", "Notes"
    legend_sizer->Add(new wxStaticText(m_parent, wxID_ANY, _L("Find")), 3, wxEXPAND);
    legend_sizer->Add(new wxStaticText(m_parent, wxID_ANY, _L("Replace with")), 3, wxEXPAND);
    legend_sizer->Add(new wxStaticText(m_parent, wxID_ANY, _L("Notes")), 2, wxEXPAND);

    m_grid_sizer->Add(legend_sizer, 1, wxEXPAND);
}

// delete substitution_id from substitutions
void SubstitutionManager::delete_substitution(int substitution_id)
{
    validate_lenth();
    if (!is_valid_id(substitution_id, "Invalid substitution_id to delete"))
        return;

    // delete substitution
    std::vector<std::string>& substitutions = m_config->option<ConfigOptionStrings>("gcode_substitutions")->values;
    substitutions.erase(std::next(substitutions.begin(), substitution_id * 4), std::next(substitutions.begin(), substitution_id * 4 + 4));
    call_ui_update();

    // update grid_sizer
    update_from_config();
}

// Add substitution line
void SubstitutionManager::add_substitution(int substitution_id,
    const std::string& plain_pattern,
    const std::string& format,
    const std::string& params,
    const std::string& notes)
{
    bool call_after_layout = false;

    if (substitution_id < 0) {
        if (m_grid_sizer->IsEmpty()) {
            create_legend();
            substitution_id = 0;
        }
        substitution_id = m_grid_sizer->GetEffectiveRowsCount() - 1;

        // create new substitution
        // it have to be added to config too
        std::vector<std::string>& substitutions = m_config->option<ConfigOptionStrings>("gcode_substitutions")->values;
        for (size_t i = 0; i < 4; i++)
            substitutions.push_back(std::string());

        call_after_layout = true;
    }

    auto del_btn = new ScalableButton(m_parent, wxID_ANY, "cross");
    del_btn->Bind(wxEVT_BUTTON, [substitution_id, this](wxEvent&) {
        delete_substitution(substitution_id);
        });

    m_grid_sizer->Add(del_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT | wxLEFT, int(0.5 * m_em));

    auto top_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto add_text_editor = [substitution_id, top_sizer, this](const wxString& value, int opt_pos, int proportion) {
        auto editor = new wxTextCtrl(m_parent, wxID_ANY, value, wxDefaultPosition, wxSize(15 * m_em, wxDefaultCoord), wxTE_PROCESS_ENTER
#ifdef _WIN32
            | wxBORDER_SIMPLE
#endif
        );

        editor->SetFont(wxGetApp().normal_font());
        wxGetApp().UpdateDarkUI(editor);
        top_sizer->Add(editor, proportion, wxALIGN_CENTER_VERTICAL | wxRIGHT, m_em);

        editor->Bind(wxEVT_TEXT_ENTER, [this, editor, substitution_id, opt_pos](wxEvent& e) {
#if !defined(__WXGTK__)
            e.Skip();
#endif // __WXGTK__
            edit_substitution(substitution_id, opt_pos, into_u8(editor->GetValue()));
            });

        editor->Bind(wxEVT_KILL_FOCUS, [this, editor, substitution_id, opt_pos](wxEvent& e) {
            e.Skip();
            edit_substitution(substitution_id, opt_pos, into_u8(editor->GetValue()));
            });
    };

    add_text_editor(from_u8(plain_pattern), 0, 3);
    add_text_editor(from_u8(format), 1, 3);
    add_text_editor(from_u8(notes), 3, 2);

    auto params_sizer = new wxBoxSizer(wxHORIZONTAL);
    bool regexp = strchr(params.c_str(), 'r') != nullptr || strchr(params.c_str(), 'R') != nullptr;
    bool case_insensitive = strchr(params.c_str(), 'i') != nullptr || strchr(params.c_str(), 'I') != nullptr;
    bool whole_word = strchr(params.c_str(), 'w') != nullptr || strchr(params.c_str(), 'W') != nullptr;
    bool match_single_line = strchr(params.c_str(), 's') != nullptr || strchr(params.c_str(), 'S') != nullptr;

    auto chb_regexp = new wxCheckBox(m_parent, wxID_ANY, _L("Regular expression"));
    chb_regexp->SetValue(regexp);
    params_sizer->Add(chb_regexp, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, m_em);

    auto chb_case_insensitive = new wxCheckBox(m_parent, wxID_ANY, _L("Case insensitive"));
    chb_case_insensitive->SetValue(case_insensitive);
    params_sizer->Add(chb_case_insensitive, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT | wxLEFT, m_em);

    auto chb_whole_word = new wxCheckBox(m_parent, wxID_ANY, _L("Whole word"));
    chb_whole_word->SetValue(whole_word);
    params_sizer->Add(chb_whole_word, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT | wxLEFT, m_em);

    auto chb_match_single_line = new wxCheckBox(m_parent, wxID_ANY, _L("Match single line"));
    chb_match_single_line->SetValue(match_single_line);
    chb_match_single_line->Show(regexp);
    params_sizer->Add(chb_match_single_line, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT | wxLEFT, m_em);

    for (wxCheckBox* chb : std::initializer_list<wxCheckBox*>{ chb_regexp, chb_case_insensitive, chb_whole_word, chb_match_single_line }) {
        chb->SetFont(wxGetApp().normal_font());
        chb->Bind(wxEVT_CHECKBOX, [this, substitution_id, chb_regexp, chb_case_insensitive, chb_whole_word, chb_match_single_line](wxCommandEvent e) {
            std::string value = std::string();
            if (chb_regexp->GetValue())
                value += "r";
            if (chb_case_insensitive->GetValue())
                value += "i";
            if (chb_whole_word->GetValue())
                value += "w";
            if (chb_match_single_line->GetValue())
                value += "s";

            chb_match_single_line->Show(chb_regexp->GetValue());
            m_grid_sizer->Layout();

            edit_substitution(substitution_id, 2, value);
            });
    }

    auto v_sizer = new wxBoxSizer(wxVERTICAL);
    v_sizer->Add(top_sizer, 1, wxEXPAND);
    v_sizer->Add(params_sizer, 1, wxEXPAND | wxTOP | wxBOTTOM, int(0.5 * m_em));
    m_grid_sizer->Add(v_sizer, 1, wxEXPAND);

    if (call_after_layout) {
        m_parent->GetParent()->Layout();
        call_ui_update();
    }
}

void SubstitutionManager::update_from_config()
{
    if (!m_grid_sizer->IsEmpty())
        m_grid_sizer->Clear(true);

    std::vector<std::string>& subst = m_config->option<ConfigOptionStrings>("gcode_substitutions")->values;
    if (subst.empty())
        hide_delete_all_btn();
    else
        create_legend();

    validate_lenth();

    int subst_id = 0;
    for (size_t i = 0; i < subst.size(); i += 4)
        add_substitution(subst_id++, subst[i], subst[i + 1], subst[i + 2], subst[i + 3]);

    m_parent->GetParent()->Layout();
}

void SubstitutionManager::delete_all()
{
    m_config->option<ConfigOptionStrings>("gcode_substitutions")->values.clear();
    call_ui_update();

    if (!m_grid_sizer->IsEmpty())
        m_grid_sizer->Clear(true);

    m_parent->GetParent()->Layout();
}

void SubstitutionManager::edit_substitution(int substitution_id, int opt_pos, const std::string& value)
{
    std::vector<std::string>& substitutions = m_config->option<ConfigOptionStrings>("gcode_substitutions")->values;

    validate_lenth();
    if (!is_compatibile_with_ui() || !is_valid_id(substitution_id, "Invalid substitution_id to edit"))
        return;

    substitutions[substitution_id * 4 + opt_pos] = value;

    call_ui_update();
}

bool SubstitutionManager::is_empty_substitutions()
{
    return m_config->option<ConfigOptionStrings>("gcode_substitutions")->values.empty();
}

// Return a callback to create a TabPrint widget to edit G-code substitutions
wxSizer* TabPrint::create_manage_substitution_widget(wxWindow* parent)
{
    auto create_btn = [parent](ScalableButton** btn, const wxString& label, const std::string& icon_name) {
        *btn = new ScalableButton(parent, wxID_ANY, icon_name, " " + label + " ", wxDefaultSize, wxDefaultPosition, wxBU_LEFT | wxBU_EXACTFIT, true);
        (*btn)->SetFont(wxGetApp().normal_font());
        (*btn)->SetSize((*btn)->GetBestSize());
    };

    ScalableButton* add_substitution_btn;
    create_btn(&add_substitution_btn, _L("Add"), "add_copies");
    add_substitution_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent e) {
        m_subst_manager.add_substitution();
        m_del_all_substitutions_btn->Show();
        });

    create_btn(&m_del_all_substitutions_btn, _L("Delete all"), "cross");
    m_del_all_substitutions_btn->Bind(wxEVT_BUTTON, [this, parent](wxCommandEvent e) {
        if (MessageDialog(parent, _L("Are you sure you want to delete all substitutions?"), SLIC3R_APP_NAME, wxYES_NO | wxCANCEL | wxICON_QUESTION).
            ShowModal() != wxID_YES)
            return;
        m_subst_manager.delete_all();
        m_del_all_substitutions_btn->Hide();
        });

    auto sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add(add_substitution_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT | wxLEFT, em_unit(parent));
    sizer->Add(m_del_all_substitutions_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT | wxLEFT, em_unit(parent));

    parent->GetParent()->Layout();
    return sizer;
}

// Return a callback to create a TabPrint widget to edit G-code substitutions
wxSizer* TabPrint::create_substitutions_widget(wxWindow* parent)
{
    wxFlexGridSizer* grid_sizer = new wxFlexGridSizer(2, 5, wxGetApp().em_unit()); // delete_button,  edit column contains "Find", "Replace", "Notes"
    grid_sizer->SetFlexibleDirection(wxBOTH);
    grid_sizer->AddGrowableCol(1);

    m_subst_manager.init(m_config, parent, grid_sizer);
    m_subst_manager.set_cb_edited_substitution([this]() {
        update_dirty();
        wxGetApp().mainframe->on_config_changed(m_config); // invalidate print
        });
    m_subst_manager.set_cb_hide_delete_all_btn([this]() {
        m_del_all_substitutions_btn->Hide();
        });

    parent->GetParent()->Layout();
    return grid_sizer;
}

// Return a callback to create a TabPrinter widget to edit bed shape
wxSizer* TabPrinter::create_bed_shape_widget(wxWindow* parent)
{
    ScalableButton* btn = new ScalableButton(parent, wxID_ANY, "printer", " " + _(L("Set")) + " " + dots,
        wxDefaultSize, wxDefaultPosition, wxBU_LEFT | wxBU_EXACTFIT, true);
    btn->SetFont(wxGetApp().normal_font());
    btn->SetSize(btn->GetBestSize());

    auto sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add(btn, 0, wxALIGN_CENTER_VERTICAL);

    btn->Bind(wxEVT_BUTTON, ([this](wxCommandEvent e)
        {
            BedShapeDialog dlg(this);
            dlg.build_dialog(*m_config->option<ConfigOptionPoints>("bed_shape"),
                *m_config->option<ConfigOptionString>("bed_custom_texture"),
                *m_config->option<ConfigOptionString>("bed_custom_model"));
            if (dlg.ShowModal() == wxID_OK) {
                const std::vector<Vec2d>& shape = dlg.get_shape();
                const std::string& custom_texture = dlg.get_custom_texture();
                const std::string& custom_model = dlg.get_custom_model();
                if (!shape.empty())
                {
                    load_key_value("bed_shape", shape);
                    load_key_value("bed_custom_texture", custom_texture);
                    load_key_value("bed_custom_model", custom_model);
                    update_changed_ui();
                }
            }
        }));

    // may be it is not a best place, but 
    // add information about Category/Grope for "bed_custom_texture" and "bed_custom_model" as a copy from "bed_shape" option
    {
        Search::OptionsSearcher& searcher = wxGetApp().sidebar().get_searcher();
        const Search::GroupAndCategory& gc = searcher.get_group_and_category(std::to_string(int(Preset::Type::TYPE_PRINTER)) + ";" + "bed_shape", ConfigOptionMode::comNone);
        searcher.add_key("bed_custom_texture", m_type, gc.group, gc.category, *m_config->def()->get("bed_custom_texture"));
        searcher.add_key("bed_custom_model", m_type, gc.group, gc.category, *m_config->def()->get("bed_custom_model"));
    }

    return sizer;
}

void TabPrinter::cache_extruder_cnt()
{
    if (m_presets->get_edited_preset().printer_technology() == ptSLA)
        return;

    m_cache_extruder_count = m_extruders_count;
    m_cache_milling_count = m_milling_count;
}

bool TabPrinter::apply_extruder_cnt_from_cache()
{
    if (m_presets->get_edited_preset().printer_technology() == ptSLA)
        return false;

    if (m_cache_milling_count > 0) {
        m_presets->get_edited_preset().set_num_milling(m_cache_milling_count);
        m_cache_milling_count = 0;
    }

    if (m_cache_extruder_count > 0) {
        m_presets->get_edited_preset().set_num_extruders(m_cache_extruder_count);
        m_cache_extruder_count = 0;
        return true;
    }
    return false;
}

void TabPrinter::update_machine_limits_description(const MachineLimitsUsage usage)
{
    GCodeFlavor flavor = m_config->option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")->value;
    wxString text;
    switch (usage) {
    case MachineLimitsUsage::EmitToGCode:
        text = _L("Machine limits will be emitted to G-code and used to estimate print time."
            " They are also used as safegard when generating gcode");
        text += " "+ _L("(even if the acceleration is set to 3000 in the print profile, if this is at 1500, it won't export a gcode that will tell to go over 1500).");
        if (flavor != gcfMarlinLegacy || flavor == gcfMarlinFirmware)
            text += "\n" + _L("Grey values means that they can't be send to your firmware (no g-code available).");
        break;
    case MachineLimitsUsage::TimeEstimateOnly:
        text = _L("Machine limits will NOT be emitted to G-code, however they will be used to estimate print time"
                ", which may therefore not be accurate as the printer may apply a different set of machine limits."
                " They are also used as safegard when generating gcode");
        text += " " + _L("(even if the acceleration is set to 3000 in the print profile, if this is at 1500, it won't export a gcode that will tell to go over 1500).");
        break;
    case MachineLimitsUsage::Limits:
        text = _L("Machine limits are used as safegard when generating gcode");
        text += " " + _L("(even if the acceleration is set to 3000 in the print profile, if this is at 1500, it won't export a gcode that will tell to go over 1500).");
        break;
    case MachineLimitsUsage::Ignore:
        text = _L("Machine limits are disabled. They are not used for anything.");
        break;
    default: assert(false);
    }
    if(m_machine_limits_description_line)
        m_machine_limits_description_line->SetText(text);

    //update fields used
    //no need to worry for "silent" version, as it's only for marlin.
    if (usage == MachineLimitsUsage::EmitToGCode) {
        wxColour grey_color(128, 128, 128);
        wxColour black_color = wxGetApp().get_label_clr_default();//wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
        Field* field;
        std::vector<std::string> axes{ "x", "y", "z", "e" };

        wxColour color = (std::set<uint8_t>{gcfKlipper, gcfMach3, gcfMachinekit, gcfMakerWare, gcfSailfish, gcfTeacup}.count(flavor) > 0) ? grey_color : black_color;
            for (const std::string& axis : axes) {
                field = m_active_page->get_field("machine_max_feedrate_" + axis, 0);
                if (field) dynamic_cast<wxTextCtrl*>(field->getWindow())->SetForegroundColour(color);
            }
        color = (std::set<uint8_t>{gcfKlipper, gcfSmoothie, gcfMach3, gcfMachinekit, gcfMakerWare, gcfSailfish, gcfTeacup}.count(flavor) > 0) ? grey_color : black_color;
            for (const std::string& axis : axes) {
                field = m_active_page->get_field("machine_max_acceleration_" + axis, 0);
                if (field) dynamic_cast<wxTextCtrl*>(field->getWindow())->SetForegroundColour(color);
            }
        color = (std::set<uint8_t>{gcfSmoothie, gcfMach3, gcfMachinekit, gcfMakerWare, gcfSailfish, gcfTeacup}.count(flavor) > 0) ? grey_color : black_color;
        {
            field = m_active_page->get_field("machine_max_acceleration_extruding", 0);
            if (field) dynamic_cast<wxTextCtrl*>(field->getWindow())->SetForegroundColour(color);
        }
        color = (flavor != gcfMarlinLegacy && flavor != gcfMarlinFirmware) ? grey_color : black_color;
        {
            field = m_active_page->get_field("machine_max_acceleration_retracting", 0);
            if (field) dynamic_cast<wxTextCtrl*>(field->getWindow())->SetForegroundColour(color);
        }
        color = (std::set<uint8_t>{gcfSmoothie, gcfMach3, gcfMachinekit, gcfMakerWare, gcfSailfish, gcfTeacup}.count(flavor) > 0) ? grey_color : black_color;
        {
            field = m_active_page->get_field("machine_max_acceleration_travel", 0);
            if (field) dynamic_cast<wxTextCtrl*>(field->getWindow())->SetForegroundColour(color);
        }
        color = (std::set<uint8_t>{gcfKlipper, gcfMach3, gcfMachinekit, gcfMakerWare, gcfSailfish, gcfTeacup}.count(flavor) > 0) ? grey_color : black_color;
            for (const std::string& axis : axes) {
                field = m_active_page->get_field("machine_max_jerk_" + axis, 0);
                if (field) dynamic_cast<wxTextCtrl*>(field->getWindow())->SetForegroundColour(color);
            }
        color = (flavor != gcfMarlinLegacy && m_last_gcode_flavor != gcfMarlinFirmware && flavor != gcfRepRap) ? grey_color : black_color;
        {
            field = m_active_page->get_field("machine_min_extruding_rate", 0);
            if (field) dynamic_cast<wxTextCtrl*>(field->getWindow())->SetForegroundColour(color);
        }
        color = (flavor != gcfMarlinLegacy && m_last_gcode_flavor != gcfMarlinFirmware) ? grey_color : black_color;
        {
            field = m_active_page->get_field("machine_min_travel_rate", 0);
            if (field) dynamic_cast<wxTextCtrl*>(field->getWindow())->SetForegroundColour(color);
        }
    } else {
        Field* field;
        std::vector<std::string> axes{ "x", "y", "z", "e" };
        const wxColour color = wxGetApp().get_label_clr_default();//wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
        for (const std::string& axis : axes) {
            field = m_active_page->get_field("machine_max_feedrate_" + axis, 0);
            if (field) dynamic_cast<wxTextCtrl*>(field->getWindow())->SetForegroundColour(color);
        }
        for (const std::string& axis : axes) {
            field = m_active_page->get_field("machine_max_acceleration_" + axis, 0);
            if (field) dynamic_cast<wxTextCtrl*>(field->getWindow())->SetForegroundColour(color);
        }
        field = m_active_page->get_field("machine_max_acceleration_extruding", 0);
        if (field) dynamic_cast<wxTextCtrl*>(field->getWindow())->SetForegroundColour(color);
        field = m_active_page->get_field("machine_max_acceleration_retracting", 0);
        if (field) dynamic_cast<wxTextCtrl*>(field->getWindow())->SetForegroundColour(color);
        field = m_active_page->get_field("machine_max_acceleration_travel", 0);
        if (field) dynamic_cast<wxTextCtrl*>(field->getWindow())->SetForegroundColour(color);
        for (const std::string& axis : axes) {
            field = m_active_page->get_field("machine_max_jerk_" + axis, 0);
            if (field) dynamic_cast<wxTextCtrl*>(field->getWindow())->SetForegroundColour(color);
        }
        field = m_active_page->get_field("machine_min_extruding_rate", 0);
        if (field) dynamic_cast<wxTextCtrl*>(field->getWindow())->SetForegroundColour(color);
        field = m_active_page->get_field("machine_min_travel_rate", 0);
        if (field) dynamic_cast<wxTextCtrl*>(field->getWindow())->SetForegroundColour(color);
    }
}

void Tab::compatible_widget_reload(PresetDependencies &deps)
{
    if (deps.btn == nullptr) return; // check if it has been initalised (should be, but someone may want to remove it from the ui)

    Field* field = this->get_field(deps.key_condition);
    if (!field)
        return;

    bool has_any = ! m_config->option<ConfigOptionStrings>(deps.key_list)->values.empty();
    has_any ? deps.btn->Enable() : deps.btn->Disable();
    deps.checkbox->SetValue(! has_any);

    field->toggle(! has_any);
}

void Tab::fill_icon_descriptions()
{
    m_icon_descriptions.emplace_back(&m_bmp_value_lock, L("LOCKED LOCK"),
        // TRN Description for "LOCKED LOCK"
        L("indicates that the settings are the same as the system (or default) values for the current option group"));

    m_icon_descriptions.emplace_back(&m_bmp_value_unlock, L("UNLOCKED LOCK"),
        // TRN Description for "UNLOCKED LOCK"
        L("indicates that some settings were changed and are not equal to the system (or default) values for "
        "the current option group.\n"
        "Click the UNLOCKED LOCK icon to reset all settings for current option group to "
        "the system (or default) values."));

    m_icon_descriptions.emplace_back(&m_bmp_white_bullet, L("WHITE BULLET"),
        // TRN Description for "WHITE BULLET"
        L("for the left button: indicates a non-system (or non-default) preset,\n"
          "for the right button: indicates that the settings hasn't been modified."));

    m_icon_descriptions.emplace_back(&m_bmp_value_revert, L("BACK ARROW"),
        // TRN Description for "BACK ARROW"
        L("indicates that the settings were changed and are not equal to the last saved preset for "
        "the current option group.\n"
        "Click the BACK ARROW icon to reset all settings for the current option group to "
        "the last saved preset."));
}

void Tab::set_tooltips_text()
{
    // --- Tooltip text for reset buttons (for whole options group)
    // Text to be shown on the "Revert to system" aka "Lock to system" button next to each input field.
    m_ttg_value_lock =		_(L("LOCKED LOCK icon indicates that the settings are the same as the system (or default) values "
                                "for the current option group"));
    m_ttg_value_unlock =	_(L("UNLOCKED LOCK icon indicates that some settings were changed and are not equal "
                                "to the system (or default) values for the current option group.\n"
                                "Click to reset all settings for current option group to the system (or default) values."));
    m_ttg_white_bullet_ns =	_(L("WHITE BULLET icon indicates a non system (or non default) preset."));
    m_ttg_non_system =		&m_ttg_white_bullet_ns;
    // Text to be shown on the "Undo user changes" button next to each input field.
    m_ttg_white_bullet =	_(L("WHITE BULLET icon indicates that the settings are the same as in the last saved "
                                "preset for the current option group."));
    m_ttg_value_revert =	_(L("BACK ARROW icon indicates that the settings were changed and are not equal to "
                                "the last saved preset for the current option group.\n"
                                "Click to reset all settings for the current option group to the last saved preset."));

    // --- Tooltip text for reset buttons (for each option in group)
    // Text to be shown on the "Revert to system" aka "Lock to system" button next to each input field.
    m_tt_value_lock =		_(L("LOCKED LOCK icon indicates that the value is the same as the system (or default) value."));
    m_tt_value_unlock =		_(L("UNLOCKED LOCK icon indicates that the value was changed and is not equal "
                                "to the system (or default) value.\n"
                                "Click to reset current value to the system (or default) value."));
    // 	m_tt_white_bullet_ns=	_(L("WHITE BULLET icon indicates a non system preset."));
    m_tt_non_system =		&m_ttg_white_bullet_ns;
    // Text to be shown on the "Undo user changes" button next to each input field.
    m_tt_white_bullet =		_(L("WHITE BULLET icon indicates that the value is the same as in the last saved preset."));
    m_tt_value_revert =		_(L("BACK ARROW icon indicates that the value was changed and is not equal to the last saved preset.\n"
                                "Click to reset current value to the last saved preset."));
    // Text for scripted gitwget icon/button
    m_tt_value_lock_script = _(L("LOCKED LOCK icon indicates that the values this widget control are all the same as the system (or default) values."));
    m_tt_value_unlock_script = _(L("UNLOCKED LOCK icon indicates that the values this widget control were changed and at least one is not equal "
        "to the system (or default) value.\n"
        "Click to reset current all values to the system (or default) values."));
    m_tt_white_bullet_script = _(L("WHITE BULLET icon indicates that the values this widget control are all the same as in the last saved preset."));
    m_tt_value_revert_script = _(L("BACK ARROW icon indicates that the values this widget control were changed and at least one is not equal to the last saved preset.\n"
        "Click to reset current all values to the last saved preset."));
}

Page::Page(wxWindow* parent, const wxString& title, int iconID) :
        m_parent(parent),
        m_title(title),
        m_iconID(iconID)
{
    m_vsizer = (wxBoxSizer*)parent->GetSizer();
    m_item_color = &wxGetApp().get_label_clr_default();
}

void Page::reload_config()
{
    for (auto group : m_optgroups)
        group->reload_config();
}

void Page::update_script_presets()
{
    for (auto group : m_optgroups)
        group->update_script_presets();
}

void Page::update_visibility(ConfigOptionMode mode, bool update_contolls_visibility)
{
    bool ret_val = false;
    for (auto group : m_optgroups) {
        if (update_contolls_visibility && group->get_grid_sizer() ? //if not created, use the method that works 
            group->update_visibility(mode) :  // update visibility for all controlls in group
            group->is_visible(mode)           // just detect visibility for the group
            ) {
            // now that it's updated, don't consider the legend groups, uless it's the only thing in the page
            ret_val |= (!(group->is_legend_line() && m_optgroups.size() > 1));
        }
    }

    m_show = ret_val;
}

void Page::activate(ConfigOptionMode mode, std::function<void()> throw_if_canceled)
{
    for (auto group : m_optgroups) {
        if (!group->activate(throw_if_canceled))
            continue;
        m_vsizer->Add(group->sizer, 0, wxEXPAND | (group->is_legend_line() ? (wxLEFT|wxTOP) : wxALL), 10);
        group->update_visibility(mode);
        group->reload_config();
        throw_if_canceled();
    }
}

void Page::clear()
{
    for (auto group : m_optgroups)
        group->clear();
}

void Page::msw_rescale()
{
    for (auto group : m_optgroups)
        group->msw_rescale();
}

void Page::sys_color_changed()
{
    for (auto group : m_optgroups)
        group->sys_color_changed();
}

void Page::refresh()
{
    for (auto group : m_optgroups)
        group->refresh();
}

Field* Page::get_field(const t_config_option_key& opt_key, int opt_index /*= -1*/) const
{
    Field* field = nullptr;
    for (auto opt : m_optgroups) {
        field = opt->get_fieldc(opt_key, opt_index);
        if (field != nullptr)
            return field;
    }
    return field;
}

bool Page::set_value(const t_config_option_key& opt_key, const boost::any& value) {
    bool changed = false;
    for (auto optgroup : m_optgroups) {
        if (optgroup->set_value(opt_key, value))
            changed = true;
    }
    return changed;
}

// package Slic3r::GUI::Tab::Page;
ConfigOptionsGroupShp Page::new_optgroup(const wxString& title, bool no_title /*= false*/, bool is_tab_opt /*= true*/)
{
    //! config_ have to be "right"
    ConfigOptionsGroupShp optgroup = std::make_shared<ConfigOptionsGroup>(m_parent, title, m_config, is_tab_opt);
    optgroup->no_title = no_title;
    if (no_title)
        optgroup->title_width = 0;

#ifdef __WXOSX__
    auto tab = parent()->GetParent()->GetParent();// GetParent()->GetParent();
#else
    auto tab = parent()->GetParent();// GetParent();
#endif
    optgroup->set_config_category_and_type(m_title, static_cast<Tab*>(tab)->type());
    optgroup->m_on_change = [tab](t_config_option_key opt_key, boost::any value) {
        //! This function will be called from OptionGroup.
        //! Using of CallAfter is redundant.
        //! And in some cases it causes update() function to be recalled again
//!        wxTheApp->CallAfter([this, opt_key, value]() {
            static_cast<Tab*>(tab)->update_dirty();
            static_cast<Tab*>(tab)->on_value_change(opt_key, value);
//!        });
    };

    optgroup->m_get_initial_config = [tab]() {
        DynamicPrintConfig config = static_cast<Tab*>(tab)->m_presets->get_selected_preset().config;
        return config;
    };

    optgroup->m_get_sys_config = [tab]() {
        DynamicPrintConfig config = static_cast<Tab*>(tab)->m_presets->get_selected_preset_parent()->config;
        return config;
    };

    optgroup->have_sys_config = [tab]() {
        return static_cast<Tab*>(tab)->m_presets->get_selected_preset_parent() != nullptr;
    };

    optgroup->rescale_extra_column_item = [](wxWindow* win) {
        auto *ctrl = dynamic_cast<wxStaticBitmap*>(win);
        if (ctrl == nullptr)
            return;

        ctrl->SetBitmap(reinterpret_cast<ScalableBitmap*>(ctrl->GetClientData())->bmp());
    };

    m_optgroups.push_back(optgroup);

    return optgroup;
}

const ConfigOptionsGroupShp Page::get_optgroup(const wxString& title) const
{
    for (ConfigOptionsGroupShp optgroup : m_optgroups) {
        if (optgroup->title == title)
            return optgroup;
    }

    return nullptr;
}

void TabSLAMaterial::build()
{
    m_presets = &m_preset_bundle->sla_materials;
    load_initial_data();

    append(this->m_pages, create_pages("sla_material.ui"));

}

// Reload current config (aka presets->edited_preset->config) into the UI fields.
void TabSLAMaterial::reload_config()
{
    this->compatible_widget_reload(m_compatible_printers);
    this->compatible_widget_reload(m_compatible_prints);
    Tab::reload_config();
}

void TabSLAMaterial::toggle_options()
{
    const Preset &current_printer = wxGetApp().preset_bundle->printers.get_edited_preset();
    std::string model = current_printer.config.opt_string("printer_model");
    m_config_manipulation.toggle_field("material_print_speed", model != "SL1");
}

void TabSLAMaterial::update()
{
    if (m_preset_bundle->printers.get_selected_preset().printer_technology() == ptFFF)
        return;

    update_description_lines();
    Layout();

// #ys_FIXME. Just a template for this function
//     m_update_cnt++;
//     ! something to update
//     m_update_cnt--;
//
//     if (m_update_cnt == 0)
        wxGetApp().mainframe->on_config_changed(m_config);
}

void TabSLAPrint::build()
{
    m_presets = &m_preset_bundle->sla_prints;
    load_initial_data();

    append(this->m_pages, create_pages("sla_print.ui"));

}

// Reload current config (aka presets->edited_preset->config) into the UI fields.
void TabSLAPrint::reload_config()
{
    this->compatible_widget_reload(m_compatible_printers);
    Tab::reload_config();
}

void TabSLAPrint::update_description_lines()
{
    Tab::update_description_lines();

    if (m_active_page && m_active_page->title() == "Supports")
    {
        bool is_visible = m_config->def()->get("support_object_elevation")->mode <= m_mode;
        if (m_support_object_elevation_description_line)
        {
            m_support_object_elevation_description_line->Show(is_visible);
            if (is_visible)
            {
                bool elev = !m_config->opt_bool("pad_enable") || !m_config->opt_bool("pad_around_object");
                m_support_object_elevation_description_line->SetText(elev ? "" :
                    from_u8((boost::format(_u8L("\"%1%\" is disabled because \"%2%\" is on in \"%3%\" category.\n"
                        "To enable \"%1%\", please switch off \"%2%\""))
                        % _L("Object elevation") % _L("Pad around object") % _L("Pad")).str()));
            }
        }
    }
}

void TabSLAPrint::toggle_options()
{
    if (m_active_page)
        m_config_manipulation.toggle_print_sla_options(m_config);
}

void TabSLAPrint::update()
{
    if (m_preset_bundle->printers.get_selected_preset().printer_technology() == ptFFF)
        return;

    m_update_cnt++;

    m_config_manipulation.update_print_sla_config(m_config, true);

    update_description_lines();
    Layout();

    m_update_cnt--;

    if (m_update_cnt == 0) {
        toggle_options();

        // update() could be called during undo/redo execution
        // Update of objectList can cause a crash in this case (because m_objects doesn't match ObjectList) 
        if (!wxGetApp().plater()->inside_snapshot_capture())
            wxGetApp().obj_list()->update_and_show_object_settings_item();

        wxGetApp().mainframe->on_config_changed(m_config);
    }
}

void TabSLAPrint::clear_pages()
{
    Tab::clear_pages();

    m_support_object_elevation_description_line = nullptr;
}

ConfigManipulation Tab::get_config_manipulation()
{
    auto load_config = [this]()
    {
        update_dirty();
        // Initialize UI components with the config values.
        reload_config();
        update();
    };

    auto cb_toggle_field = [this](const t_config_option_key& opt_key, bool toggle, int opt_index) {
        return toggle_option(opt_key, toggle, opt_index);
    };

    auto cb_value_change = [this](const std::string& opt_key, const boost::any& value) {
        return on_value_change(opt_key, value);
    };

    return ConfigManipulation(load_config, cb_toggle_field, cb_value_change, nullptr, this);
}


} // GUI
} // Slic3r
