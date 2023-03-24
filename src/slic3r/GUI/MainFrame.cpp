#include "MainFrame.hpp"

#include <wx/debug.h>
#include <wx/filename.h>
//#include <wx/glcanvas.h>
#include <wx/listbook.h>
#include <wx/simplebook.h>
#include <wx/icon.h>
#include <wx/menu.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/progdlg.h>
#include <wx/sizer.h>
#include <wx/tooltip.h>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/log/trivial.hpp>

#include "libslic3r/Polygon.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/Time.hpp"

#include "../Utils/Process.hpp"
#include "3DScene.hpp"
#include "GLCanvas3D.hpp"
#include "GUI_ObjectList.hpp"
#include "I18N.hpp"
#include "InstanceCheck.hpp"
#include "Mouse3DController.hpp"
#include "Plater.hpp"
#include "PrintHostDialogs.hpp"
// #include "ProgressStatusBar.hpp"
#include "RemovableDriveManager.hpp"
#include "Tab.hpp"
#include "format.hpp"
#include "wxExtensions.hpp"

#include <fstream>
#include <string_view>

#include "GUI_App.hpp"
#include "UnsavedChangesDialog.hpp"
#include "MsgDialog.hpp"
#include "Notebook.hpp"
#include "GUI_Factories.hpp"
#include "GUI_ObjectList.hpp"
#include "GalleryDialog.hpp"
#include "NotificationManager.hpp"

#ifdef _WIN32
#include <dbt.h>
#include <shlobj.h>
#endif // _WIN32

namespace Slic3r {
namespace GUI {

constexpr int32_t MAINFRAME_MENU_ITEM_COUNT = 8;

enum class ERescaleTarget
{
    Mainframe,
    SettingsDialog
};

#ifdef __APPLE__
class PrusaSlicerTaskBarIcon : public wxTaskBarIcon
{
public:
    PrusaSlicerTaskBarIcon(wxTaskBarIconType iconType = wxTBI_DEFAULT_TYPE) : wxTaskBarIcon(iconType) {}
    wxMenu *CreatePopupMenu() override {
        wxMenu *menu = new wxMenu;
        if(wxGetApp().app_config->get("single_instance") == "0") {
            // Only allow opening a new Slic3r instance on OSX if "single_instance" is disabled, 
            // as starting new instances would interfere with the locking mechanism of "single_instance" support.
            append_menu_item(menu, wxID_ANY, _L("Open new instance"), wxString::Format(_L("Open a new %s instance"), SLIC3R_APP_NAME),
            [](wxCommandEvent&) { start_new_slicer(); }, "", nullptr);
        }
        append_menu_item(menu, wxID_ANY, _L("G-code preview") + dots, _L("Open G-code viewer"),
            [](wxCommandEvent&) { start_new_gcodeviewer_open_file(); }, "", nullptr);
        return menu;
    }
};
class GCodeViewerTaskBarIcon : public wxTaskBarIcon
{
public:
    GCodeViewerTaskBarIcon(wxTaskBarIconType iconType = wxTBI_DEFAULT_TYPE) : wxTaskBarIcon(iconType) {}
    wxMenu *CreatePopupMenu() override {
        wxMenu *menu = new wxMenu;
        append_menu_item(menu, wxID_ANY, wxString::Format(_L("Open %s"), SLIC3R_APP_NAME), wxString::Format(_L("Open a new %s instance"), SLIC3R_APP_NAME),
            [](wxCommandEvent&) { start_new_slicer(nullptr, true); }, "", nullptr);
        append_menu_item(menu, wxID_ANY, _L("G-code preview") + dots, _L("Open new G-code viewer"),
            [](wxCommandEvent&) { start_new_gcodeviewer_open_file(); }, "", nullptr);
        return menu;
    }
};
#endif // __APPLE__

// Load the icon either from the exe, or from the ico file.
static wxIcon main_frame_icon(GUI_App::EAppMode app_mode)
{
#if _WIN32
    std::wstring path(size_t(MAX_PATH), wchar_t(0));
    int len = int(::GetModuleFileName(nullptr, path.data(), MAX_PATH));
    if (len > 0 && len < MAX_PATH) {
        path.erase(path.begin() + len, path.end());
        if (app_mode == GUI_App::EAppMode::GCodeViewer) {
            // Only in case the slicer was started with --gcodeviewer parameter try to load the icon from gcodeviewer.exe
            // Otherwise load it from the exe.
            for (const std::wstring_view exe_name : { std::wstring_view(SLIC3R_APP_WCMD ".exe"), std::wstring_view(SLIC3R_APP_WCMD "_console.exe") })
                if (boost::iends_with(path, exe_name)) {
                    path.erase(path.end() - exe_name.size(), path.end());
                    path += GCODEVIEWER_APP_WCMD ".exe";
                    break;
                }
        }
    }
    return wxIcon(path, wxBITMAP_TYPE_ICO);
#else // _WIN32
    return wxIcon(Slic3r::var(app_mode == GUI_App::EAppMode::Editor ? SLIC3R_APP_KEY "_128px.png" : GCODEVIEWER_APP_KEY "_128px.png"), wxBITMAP_TYPE_PNG);
#endif // _WIN32
}

MainFrame::MainFrame() :
DPIFrame(NULL, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxDEFAULT_FRAME_STYLE, "mainframe"),
    m_printhost_queue_dlg(new PrintHostQueueDialog(this))
    , m_recent_projects(9)
    , m_settings_dialog(this)
    , diff_dialog(this)
{
    // Fonts were created by the DPIFrame constructor for the monitor, on which the window opened.
    wxGetApp().update_fonts(this);
/*
#ifndef __WXOSX__ // Don't call SetFont under OSX to avoid name cutting in ObjectList 
    this->SetFont(this->normal_font());
#endif
    // Font is already set in DPIFrame constructor
*/

#ifdef __APPLE__
    // Initialize the docker task bar icon.
    switch (wxGetApp().get_app_mode()) {
    default:
    case GUI_App::EAppMode::Editor:
        m_taskbar_icon = std::make_unique<PrusaSlicerTaskBarIcon>(wxTBI_DOCK);
        m_taskbar_icon->SetIcon(wxIcon(Slic3r::var(SLIC3R_APP_KEY "_256_icns.png"), wxBITMAP_TYPE_PNG), SLIC3R_APP_KEY);
        break;
    case GUI_App::EAppMode::GCodeViewer:
        m_taskbar_icon = std::make_unique<GCodeViewerTaskBarIcon>(wxTBI_DOCK);
        m_taskbar_icon->SetIcon(wxIcon(Slic3r::var(GCODEVIEWER_APP_KEY "-mac_128px.png"), wxBITMAP_TYPE_PNG), GCODEVIEWER_APP_NAME);
        break;
    }
#endif // __APPLE__

    // Load the icon either from the exe, or from the ico file.
    SetIcon(main_frame_icon(wxGetApp().get_app_mode()));

	// initialize status bar
//    m_statusbar = std::make_shared<ProgressStatusBar>(this);
//    m_statusbar->set_font(GUI::wxGetApp().normal_font());
//    if (wxGetApp().is_editor())
//        m_statusbar->embed(this);
//    m_statusbar->set_status_text(_L("Version") + " " +
//        SLIC3R_VERSION + " - " +
//       _L("Remember to check for updates at https://github.com/prusa3d/PrusaSlicer/releases"));

    // initialize tabpanel and menubar
    init_tabpanel();
    if (wxGetApp().is_gcode_viewer())
        init_menubar_as_gcodeviewer();
    else
        init_menubar_as_editor();

#if _WIN32
    // This is needed on Windows to fake the CTRL+# of the window menu when using the numpad
    wxAcceleratorEntry entries[6];
    entries[0].Set(wxACCEL_CTRL, WXK_NUMPAD1, wxID_HIGHEST + 1);
    entries[1].Set(wxACCEL_CTRL, WXK_NUMPAD2, wxID_HIGHEST + 2);
    entries[2].Set(wxACCEL_CTRL, WXK_NUMPAD3, wxID_HIGHEST + 3);
    entries[3].Set(wxACCEL_CTRL, WXK_NUMPAD4, wxID_HIGHEST + 4);
    entries[4].Set(wxACCEL_CTRL, WXK_NUMPAD5, wxID_HIGHEST + 5);
    entries[5].Set(wxACCEL_CTRL, WXK_NUMPAD6, wxID_HIGHEST + 6);
    wxAcceleratorTable accel(6, entries);
    SetAcceleratorTable(accel);
#endif // _WIN32

    // set default tooltip timer in msec
    // SetAutoPop supposedly accepts long integers but some bug doesn't allow for larger values
    // (SetAutoPop is not available on GTK.)
    wxToolTip::SetAutoPop(32767);

    m_loaded = true;

    // initialize layout
    m_main_sizer = new wxBoxSizer(wxVERTICAL);
    wxSizer* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_main_sizer, 1, wxEXPAND);
    SetSizer(sizer);
    // initialize layout from config
        update_layout();
    sizer->SetSizeHints(this);
    Fit();

    const wxSize min_size = wxGetApp().get_min_size(); //wxSize(76*wxGetApp().em_unit(), 49*wxGetApp().em_unit());
#ifdef __APPLE__
    // Using SetMinSize() on Mac messes up the window position in some cases
    // cf. https://groups.google.com/forum/#!topic/wx-users/yUKPBBfXWO0
    SetSize(min_size/*wxSize(760, 490)*/);
#else
    SetMinSize(min_size/*wxSize(760, 490)*/);
    SetSize(GetMinSize());
#endif
    Layout();

    update_title();

    // declare events
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) {

        //std::cout << "closing...\n";
        //std::cout << "is_project_dirty=" << plater()->is_project_dirty() << "\n";
        //std::cout << "is_presets_dirty=" << plater()->is_presets_dirty() << "\n";
        //std::cout << "is_platter_dirty=" << plater()->get_dirty().is_plater_dirty() << "\n";
        //std::cout << "is_projectconf_dirty=" << plater()->get_dirty().is_project_config_dirty() << "\n";
        //std::cout << "has_current_preset_changes=" << wxGetApp().has_current_preset_changes() << "\n";
        //std::cout << "has_unsaved_preset_changes=" << wxGetApp().has_unsaved_preset_changes() << "\n";
        //std::cout << "Not closing!\n";
        //event.Veto();
        //return;

        if (event.CanVeto() && m_plater->canvas3D()->get_gizmos_manager().is_in_editing_mode(true)) {
            // prevents to open the save dirty project dialog
            event.Veto();
            return;
        }

        if (event.CanVeto() && m_plater != nullptr) {
            int saved_project = m_plater->save_project_if_dirty(format_wxstr(_L("Closing %1%. Current project is modified."), SLIC3R_APP_NAME));
            if (saved_project == wxID_CANCEL) {
                event.Veto();
                return;
            }
            // check unsaved changes only if project wasn't saved
            else if (saved_project == wxID_NO && event.CanVeto() &&
                     (plater()->is_presets_dirty() && !wxGetApp().check_and_save_current_preset_changes(format_wxstr(_L("%1% is closing"), SLIC3R_APP_NAME), format_wxstr(_L("Closing %1% while some presets are modified."), SLIC3R_APP_NAME)))) {
                event.Veto();
                return;
            }
        }

        if (event.CanVeto() && !wxGetApp().check_print_host_queue()) {
            event.Veto();
            return;
        }
        this->shutdown();
        // propagate event
        event.Skip();
    });

    //FIXME it seems this method is not called on application start-up, at least not on Windows. Why?
    // The same applies to wxEVT_CREATE, it is not being called on startup on Windows.
    Bind(wxEVT_ACTIVATE, [this](wxActivateEvent& event) {
        if (m_plater != nullptr && event.GetActive())
            m_plater->on_activate();
        event.Skip();
    });

// OSX specific issue:
// When we move application between Retina and non-Retina displays, The legend on a canvas doesn't redraw
// So, redraw explicitly canvas, when application is moved
//FIXME maybe this is useful for __WXGTK3__ as well?
#if __APPLE__
    Bind(wxEVT_MOVE, [](wxMoveEvent& event) {
        wxGetApp().plater()->get_current_canvas3D()->set_as_dirty();
        wxGetApp().plater()->get_current_canvas3D()->request_extra_frame();
        event.Skip();
    });
#endif

    wxGetApp().persist_window_geometry(this, true);
    wxGetApp().persist_window_geometry(&m_settings_dialog, true);

    update_ui_from_settings();    // FIXME (?)

    if (m_plater != nullptr) {
        m_plater->get_collapse_toolbar().set_enabled(wxGetApp().app_config->get("show_collapse_button") == "1");
        m_plater->show_action_buttons(true);
    }
}

void MainFrame::update_icon() {

#ifndef _USE_CUSTOM_NOTEBOOK
    // icons for ESettingsLayout::Hidden
    wxImageList* img_list = nullptr;
    int icon_size = 0;
    try {
        icon_size = atoi(wxGetApp().app_config->get("tab_icon_size").c_str());
    }
    catch (std::exception e) {}
    switch (m_layout)
    {
    case ESettingsLayout::Unknown:
    {
        break;
    } case ESettingsLayout::Old:
      case ESettingsLayout::Hidden:
    {
        if (m_tabpanel->GetPageCount() == 4 && icon_size >= 8) {
            m_tabpanel->SetPageImage(0, 0);
            m_tabpanel->SetPageImage(1, 3);
            m_tabpanel->SetPageImage(2, m_plater->printer_technology() == PrinterTechnology::ptSLA ? 6 : 4);
            m_tabpanel->SetPageImage(3, m_plater->printer_technology() == PrinterTechnology::ptSLA ? 7 : 5);
        }
        break;
    }
    case ESettingsLayout::Tabs:
    {
        if (icon_size >= 8)
        {
            m_tabpanel->SetPageImage(0, 0);
            m_tabpanel->SetPageImage(1, 1);
            m_tabpanel->SetPageImage(2, 2);
            m_tabpanel->SetPageImage(3, 3);
            m_tabpanel->SetPageImage(4, m_plater->printer_technology() == PrinterTechnology::ptSLA ? 6 : 4);
            m_tabpanel->SetPageImage(5, m_plater->printer_technology() == PrinterTechnology::ptSLA ? 7 : 5);
        }
        break;
    }
    case ESettingsLayout::Dlg:
    {
        if (m_tabpanel->GetPageCount() == 4 && icon_size >= 8) {
            m_tabpanel->SetPageImage(0, 3);
            m_tabpanel->SetPageImage(1, m_plater->printer_technology() == PrinterTechnology::ptSLA ? 6 : 4);
            m_tabpanel->SetPageImage(2, m_plater->printer_technology() == PrinterTechnology::ptSLA ? 7 : 5);
        }
        break;
    }
    case ESettingsLayout::GCodeViewer:
    {
        break;
    }
    }
#endif
}

#ifdef _USE_CUSTOM_NOTEBOOK
static wxString pref() { return " [ "; }
static wxString suff() { return " ] "; }
static void append_tab_menu_items_to_menubar(wxMenuBar* bar, PrinterTechnology pt, MainFrame::ESettingsLayout layout)
{
    // Add separator 
    bar->Append(new wxMenu(), "          ");
    bar->EnableTop(MAINFRAME_MENU_ITEM_COUNT, false);

    bool has_marker = false;
    if (layout == MainFrame::ESettingsLayout::Tabs) {
        bar->Append(new wxMenu(), pref() + _L("3D view") + suff());
        bar->Append(new wxMenu(), _L("Sliced preview"));
        bar->Append(new wxMenu(),  _L("Gcode preview"));
        has_marker = true;
        // Add separator 
        bar->Append(new wxMenu(), "          ");
        bar->EnableTop(MAINFRAME_MENU_ITEM_COUNT + 4, false);
    } else if (layout == MainFrame::ESettingsLayout::Old) {
        bar->Append(new wxMenu(), pref() + _L("Platter") + suff());
        has_marker = true;
        // Add separator 
        bar->Append(new wxMenu(), "          ");
        bar->EnableTop(MAINFRAME_MENU_ITEM_COUNT + 2, false);
    }

    for (const wxString& title : { has_marker           ? _L("Print Settings")       : pref() + _L("Print Settings") + suff(),
                                   pt == ptSLA          ? _L("Material Settings")    : _L("Filament Settings"),
                                   _L("Printer Settings") })
        bar->Append(new wxMenu(), title);
}

// update markers for selected/unselected menu items
static void update_marker_for_tabs_menu(wxMenuBar* bar, const wxString& title, int idx, MainFrame::ESettingsLayout layout)
{
    if (!bar)
        return;
    size_t items_cnt = bar->GetMenuCount();
    size_t to_remove = 3;
    if (layout == MainFrame::ESettingsLayout::Old) {
        to_remove = 5;
        if (idx > 0) idx++;
    } else if (layout == MainFrame::ESettingsLayout::Tabs) {
        to_remove = 7;
        if (idx > 2) idx++;
    }
    for (size_t id = items_cnt - to_remove; id < items_cnt; id++) {
        wxString label = bar->GetMenuLabel(id);
        if (label.First(pref()) == 0) {
            if (label == pref() + title + suff())
                return;
            label.Remove(size_t(0), pref().Len());
            label.RemoveLast(suff().Len());
            bar->SetMenuLabel(id, label);
            break;
        }
    }
    if (int id = bar->FindMenu(title); id != wxNOT_FOUND)
        bar->SetMenuLabel(id, pref() + title + suff());
    else
        bar->SetMenuLabel(items_cnt - to_remove + idx, pref() + bar->GetMenuLabel(items_cnt - to_remove + idx) + suff());

}
static MainFrame::ETabType get_tab_bt_selected(wxMenuBar* bar, MainFrame::ESettingsLayout layout) {
    size_t items_cnt = bar->GetMenuCount();
    size_t to_remove = 3;
    if (layout == MainFrame::ESettingsLayout::Old) {
        to_remove = 5;
    } else if (layout == MainFrame::ESettingsLayout::Tabs) {
        to_remove = 7;
    }
    int32_t idx_selected = -1;
    for (size_t id = items_cnt - to_remove; id < items_cnt; id++) {
        wxString label = bar->GetMenuLabel(id);
        if (label.First(pref()) == 0) {
            idx_selected = id - items_cnt + to_remove;
            break;
        }
    }
    if (idx_selected < 0) return MainFrame::ETabType::LastPlater;
    if (layout == MainFrame::ESettingsLayout::Old) {
        if (idx_selected == 0) return MainFrame::ETabType::LastPlater;
        return MainFrame::ETabType((uint8_t)MainFrame::ETabType::LastPlater + (uint8_t)idx_selected);
    } else if (layout == MainFrame::ESettingsLayout::Tabs) {
        return MainFrame::ETabType((uint8_t)MainFrame::ETabType::Plater3D + (uint8_t)idx_selected);
    } else if (layout == MainFrame::ESettingsLayout::Dlg) {
        MainFrame::ETabType((uint8_t)MainFrame::ETabType::PrintSettings + (uint8_t)idx_selected);
    }
    return MainFrame::ETabType::Plater3D;
}

static void add_tabs_as_menu(wxMenuBar* bar, MainFrame* main_frame, wxWindow* bar_parent)
{
    PrinterTechnology pt = main_frame->plater() ? main_frame->plater()->printer_technology() : ptFFF;

    if (main_frame->get_layout() == MainFrame::ESettingsLayout::Dlg)
        append_tab_menu_items_to_menubar(bar, pt, main_frame->get_layout());

    bar_parent->Bind(wxEVT_MENU_OPEN, [main_frame, bar](wxMenuEvent& event) {
        wxMenu* const menu = event.GetMenu();
        if (!menu || menu->GetMenuItemCount() > 0) {
            // If we are here it means that we open regular menu and not a tab used as a menu
            event.Skip(); // event.Skip() is verry important to next processing of the wxEVT_UPDATE_UI by this menu items.
                          // If wxEVT_MENU_OPEN will not be pocessed in next event queue then MenuItems of this menu will never caught wxEVT_UPDATE_UI 
                          // and, as a result, "check/radio value" will not be updated
            return;
        }

        // update tab selection

        const wxString& title = menu->GetTitle();
        if (title == _L("Platter"))
            main_frame->select_tab(MainFrame::ETabType::LastPlater);
        else if (title == _L("3D view"))
            main_frame->select_tab(MainFrame::ETabType::Plater3D);
        else if (title == _L("Sliced preview"))
            main_frame->select_tab(MainFrame::ETabType::PlaterPreview);
        else if (title == _L("Gcode preview"))
            main_frame->select_tab(MainFrame::ETabType::PlaterGcode);
        else if (title == _L("Print Settings"))
            main_frame->select_tab(MainFrame::ETabType::PrintSettings);
        else if (title == _L("Filament Settings"))
            main_frame->select_tab(MainFrame::ETabType::FilamentSettings);
        else if (title == _L("Material Settings"))
            main_frame->select_tab(MainFrame::ETabType::FilamentSettings);
        else if (title == _L("Printer Settings"))
            main_frame->select_tab(MainFrame::ETabType::PrinterSettings);

        // update markers for selected/unselected menu items
        update_marker_for_tabs_menu(bar, title, 0, main_frame->get_layout());
    });
}

void MainFrame::show_tabs_menu(bool show)
{
    while (m_menubar->GetMenuCount() >= MAINFRAME_MENU_ITEM_COUNT + 1) {
        if (wxMenu* menu = m_menubar->Remove(MAINFRAME_MENU_ITEM_COUNT))
            delete menu;
    }
    if (show)
        append_tab_menu_items_to_menubar(m_menubar, plater() ? plater()->printer_technology() : ptFFF, this->get_layout());
}
#endif // _USE_CUSTOM_NOTEBOOK

void MainFrame::update_layout()
{
    auto restore_to_creation = [this]() {
        auto clean_sizer = [](wxSizer* sizer) {
            while (!sizer->GetChildren().IsEmpty()) {
                sizer->Detach(0);
            }
        };

        // On Linux m_plater needs to be removed from m_tabpanel before to reparent it
        //clear if previous was old
        m_tabpanel_stop_event = true;
        int plater_page_id = m_tabpanel->FindPage(m_plater);
        if (plater_page_id != wxNOT_FOUND)
            m_tabpanel->RemovePage(plater_page_id);

        if (m_plater->GetParent() != this)
            m_plater->Reparent(this);
#ifndef _USE_CUSTOM_NOTEBOOK
        for (int i = 0; i < m_tabpanel->GetPageCount();  i++) {
            m_tabpanel->SetPageImage(i, -1);
        }
        m_tabpanel->SetImageList(nullptr); //clear
#endif

        if (m_tabpanel->GetParent() != this)
            m_tabpanel->Reparent(this);

        //clear if previous was hidden
        plater_page_id = (m_plater_page != nullptr) ? m_tabpanel->FindPage(m_plater_page) : wxNOT_FOUND;
        if (plater_page_id != wxNOT_FOUND) {
            m_tabpanel->DeletePage(plater_page_id);
            m_plater_page = nullptr;
        }

#ifdef _USE_CUSTOM_NOTEBOOK
        if (!wxGetApp().tabs_as_menu()) {
            Notebook* notebook = static_cast<Notebook*>(m_tabpanel);
            if (m_layout == ESettingsLayout::Tabs) {
                //remove fake buttons
                // (3D view already deleted)
                notebook->CleanBt();
            } else if (m_layout == ESettingsLayout::Old || m_layout == ESettingsLayout::Hidden) {
                notebook->GetBtnsListCtrl()->RemoveSpacer(0);
            }
        }
#else
        //clear if previous was tabs
        for (int i = 0; i < m_tabpanel->GetPageCount() - 3; i++)
            if (m_tabpanel->GetPage(i)->GetChildren().empty() && m_tabpanel->GetPage(i)->GetSizer()->GetItemCount() > 0) {
                clean_sizer(m_tabpanel->GetPage(i)->GetSizer());
            }
        if (m_tabpanel->GetPageCount() >= 6 && m_tabpanel->GetPage(0)->GetChildren().size() == 0 && m_tabpanel->GetPage(1)->GetChildren().size() == 0 && m_tabpanel->GetPage(2)->GetChildren().size() == 0) {
            m_tabpanel->DeletePage(2);
            m_tabpanel->DeletePage(1);
            m_tabpanel->DeletePage(0);
        }
        // ensure wehave only the 3 settings tabs
        while (m_tabpanel->GetPageCount() > 3) {
            m_tabpanel->DeletePage(0);
        }
#endif

        clean_sizer(m_main_sizer);
        clean_sizer(m_settings_dialog.GetSizer());

        if (m_settings_dialog.IsShown())
            m_settings_dialog.Close();

        m_tabpanel->Hide();
        m_tabpanel_stop_event = false;
        m_plater->Hide();
        m_plater->enable_view_toolbar(true);
        m_plater->set_force_preview(Preview::ForceState::NoForce);

        Layout();
    };

    ESettingsLayout layout = wxGetApp().is_gcode_viewer() ? ESettingsLayout::GCodeViewer :
           (wxGetApp().app_config->get("old_settings_layout_mode") == "1" ? ESettingsLayout::Old :
            wxGetApp().app_config->get("tab_settings_layout_mode") == "1" ? ESettingsLayout::Tabs :
            wxGetApp().app_config->get("new_settings_layout_mode") == "1" ? ( wxGetApp().tabs_as_menu() ? ESettingsLayout::Old : ESettingsLayout::Hidden) :
            wxGetApp().app_config->get("dlg_settings_layout_mode") == "1" ? ESettingsLayout::Dlg :
#ifdef __WXMSW__
                ESettingsLayout::Tabs);
#else
                ESettingsLayout::Old);
#endif

    if (m_layout == layout)
        return;

    wxBusyCursor busy;

    Freeze();

    // Remove old settings
    if (m_layout != ESettingsLayout::Unknown)
        restore_to_creation();
    else //init with view_toolbar by default
        m_plater->enable_view_toolbar(true);

#ifdef __WXMSW__
    enum class State {
        noUpdate,
        fromDlg,
        toDlg
    };
    State update_scaling_state = //m_layout == ESettingsLayout::Unknown   ? State::noUpdate   : // don't scale settings dialog from the application start
                                 m_layout == ESettingsLayout::Dlg       ? State::fromDlg    :
                                 layout   == ESettingsLayout::Dlg       ? State::toDlg      : State::noUpdate;
#endif //__WXMSW__

    ESettingsLayout old_layout = m_layout;
    m_layout = layout;
    if (m_plater && m_layerpreview_menu_item)
        m_layerpreview_menu_item->Enable(m_layout == ESettingsLayout::Tabs || m_layout == ESettingsLayout::Old);

    // From the very beginning the Print settings should be selected
    m_last_selected_setting_tab = 0;
    m_last_selected_plater_tab = 999;


#ifdef _USE_CUSTOM_NOTEBOOK
    int icon_size = 0;
    try {
        icon_size = atoi(wxGetApp().app_config->get("tab_icon_size").c_str());
    }
    catch (std::exception e) {}
#endif

    // Set new settings
    switch (m_layout)
    {
    case ESettingsLayout::Unknown:
    {
        break;
    }case ESettingsLayout::Old:
    {
        //layout
        m_plater->Reparent(m_tabpanel);
#ifdef _USE_CUSTOM_NOTEBOOK
        m_plater->Layout();
        if (!wxGetApp().tabs_as_menu())
            dynamic_cast<Notebook*>(m_tabpanel)->InsertBtPage(0, m_plater, _L("Platter"), std::string("plater"), icon_size, true);
        else
#endif
        m_tabpanel->InsertPage(0, m_plater, _L("Platter"));
#ifdef _USE_CUSTOM_NOTEBOOK
        if (!wxGetApp().tabs_as_menu())
            dynamic_cast<Notebook*>(m_tabpanel)->GetBtnsListCtrl()->InsertSpacer(1, 40);
#endif
        m_main_sizer->Add(m_tabpanel, 1, wxEXPAND | wxTOP, 1);
        update_icon();
        // show
        m_plater->Show();
        m_tabpanel->Show();
        // update Tabs
        if (old_layout == ESettingsLayout::Dlg)
            if (int sel = m_tabpanel->GetSelection(); sel != wxNOT_FOUND)
                m_tabpanel->SetSelection(sel+1);// call SetSelection to correct layout after switching from Dlg to Old mode
#ifdef _USE_CUSTOM_NOTEBOOK
        if (wxGetApp().tabs_as_menu())
            show_tabs_menu(true);
#endif
        break;
    }
    case ESettingsLayout::Tabs:
    {
        // don't use view_toolbar here
        m_plater->enable_view_toolbar(false);
        bool need_freeze = !this->IsFrozen();
        if(need_freeze) this->Freeze();
#ifdef _USE_CUSTOM_NOTEBOOK
        m_plater->Reparent(m_tabpanel);
        m_plater->Layout();
        if (!wxGetApp().tabs_as_menu()) {
            Notebook* notebook = static_cast<Notebook*>(m_tabpanel);
            notebook->InsertBtPage(0, m_plater, _L("3D view"), std::string("editor_menu"), icon_size, true);
            notebook->InsertFakeBtPage(1, 0, _L("Sliced preview"), std::string("layers"), icon_size, false);
            notebook->InsertFakeBtPage(2, 0, _L("Gcode preview"), std::string("preview_menu"), icon_size, false);
            notebook->GetBtnsListCtrl()->InsertSpacer(3, 40);
            notebook->GetBtnsListCtrl()->GetPageButton(0)->Bind(wxCUSTOMEVT_NOTEBOOK_BT_PRESSED, [this](wxCommandEvent& event) {
                this->m_plater->select_view_3D("3D");
                //not that useful
                //this->select_tab(MainFrame::ETabType::Plater3D); // select Plater
                });
            notebook->GetBtnsListCtrl()->GetPageButton(1)->Bind(wxCUSTOMEVT_NOTEBOOK_BT_PRESSED, [this](wxCommandEvent& event) {
                if (this->m_plater->get_force_preview() != Preview::ForceState::ForceExtrusions) {
                    this->m_plater->set_force_preview(Preview::ForceState::ForceExtrusions);
                    this->m_plater->select_view_3D("Preview");
                    this->m_plater->refresh_print();
                } else
                    this->m_plater->select_view_3D("Preview");
                //this->select_tab(MainFrame::ETabType::PlaterPreview); // select Plater
                });
            notebook->GetBtnsListCtrl()->GetPageButton(2)->Bind(wxCUSTOMEVT_NOTEBOOK_BT_PRESSED, [this](wxCommandEvent& event) {
                if (this->m_plater->get_force_preview() != Preview::ForceState::ForceGcode) {
                    this->m_plater->set_force_preview(Preview::ForceState::ForceGcode);
                    this->m_plater->select_view_3D("Preview");
                    this->m_plater->refresh_print();
                } else
                    this->m_plater->select_view_3D("Preview");
                //this->select_tab(MainFrame::ETabType::PlaterGcode); // select Plater
                });
        } else {
            m_tabpanel->InsertPage(0, m_plater, _L("Platter")); // empty panel just for Platter tab */
        }
        m_main_sizer->Add(m_tabpanel, 1, wxEXPAND | wxTOP, 1);
        update_icon();
        // show
        m_plater->Show();
        m_tabpanel->Show();
        // update Tabs
        if (old_layout == ESettingsLayout::Dlg)
            if (int sel = m_tabpanel->GetSelection(); sel != wxNOT_FOUND)
                m_tabpanel->SetSelection(sel + 1);// call SetSelection to correct layout after switching from Dlg to Old mode
        if (wxGetApp().tabs_as_menu())
            show_tabs_menu(true);
#else
        wxPanel* first_panel = new wxPanel(m_tabpanel);
        m_tabpanel->InsertPage(0, first_panel, _L("3D view"));
        m_tabpanel->InsertPage(1, new wxPanel(m_tabpanel), _L("Sliced preview"));
        m_tabpanel->InsertPage(2, new wxPanel(m_tabpanel), _L("Gcode preview"));
        if (m_tabpanel->GetPageCount() == 6) {
            m_tabpanel->GetPage(0)->SetSizer(new wxBoxSizer(wxVERTICAL));
            m_tabpanel->GetPage(1)->SetSizer(new wxBoxSizer(wxVERTICAL));
            m_tabpanel->GetPage(2)->SetSizer(new wxBoxSizer(wxVERTICAL));
            update_icon();
        }
        m_plater->Reparent(first_panel);
        first_panel->GetSizer()->Add(m_plater, 1, wxEXPAND);
        m_tabpanel->ChangeSelection(0);
        m_main_sizer->Add(m_tabpanel, 1, wxEXPAND);
        m_plater->Show();
        m_tabpanel->Show();
        if (need_freeze) this->Thaw();
#endif
        if (need_freeze) this->Thaw();
        break;
    }
    case ESettingsLayout::Hidden:
    {
        m_main_sizer->Add(m_plater, 1, wxEXPAND);
        m_tabpanel->Hide();
        m_main_sizer->Add(m_tabpanel, 1, wxEXPAND);
        m_plater_page = new wxPanel(m_tabpanel);
#ifdef _USE_CUSTOM_NOTEBOOK
        if (!wxGetApp().tabs_as_menu())
            dynamic_cast<Notebook*>(m_tabpanel)->InsertBtPage(0, m_plater_page, _L("Platter"), std::string("plater"), icon_size, true);
        else
#endif
        m_tabpanel->InsertPage(0, m_plater_page, _L("Platter")); // empty panel just for Platter tab */
#ifdef _USE_CUSTOM_NOTEBOOK
        if (!wxGetApp().tabs_as_menu())
            dynamic_cast<Notebook*>(m_tabpanel)->GetBtnsListCtrl()->InsertSpacer(1, 40);
#endif
        update_icon();
        m_plater->Show();
        break;
    }
    case ESettingsLayout::Dlg:
    {
        m_main_sizer->Add(m_plater, 1, wxEXPAND);
        m_tabpanel->Reparent(&m_settings_dialog);
        m_settings_dialog.GetSizer()->Add(m_tabpanel, 1, wxEXPAND | wxTOP, 2);
        update_icon();
        m_tabpanel->Show();
        m_plater->Show();

#ifdef _USE_CUSTOM_NOTEBOOK
        if (wxGetApp().tabs_as_menu())
            show_tabs_menu(false);
#endif
        break;
    }
    case ESettingsLayout::GCodeViewer:
    {
        m_main_sizer->Add(m_plater, 1, wxEXPAND);
        m_plater->set_bed_shape({ { 0.0, 0.0 }, { 200.0, 0.0 }, { 200.0, 200.0 }, { 0.0, 200.0 } }, 0.0, {}, {}, true);
        m_plater->get_collapse_toolbar().set_enabled(false);
        m_plater->collapse_sidebar(true);
        m_plater->Show();
        break;
    }
    }

#ifdef _USE_CUSTOM_NOTEBOOK
    // Sizer with buttons for mode changing
    m_plater->sidebar().show_mode_sizer(wxGetApp().tabs_as_menu() || ( m_layout != ESettingsLayout::Old && m_layout != ESettingsLayout::Tabs));
#endif

#ifdef __WXMSW__
    if (update_scaling_state != State::noUpdate)
    {
        int mainframe_dpi   = get_dpi_for_window(this);
        int dialog_dpi      = get_dpi_for_window(&m_settings_dialog);
        if (mainframe_dpi != dialog_dpi) {
            wxSize oldDPI = update_scaling_state == State::fromDlg ? wxSize(dialog_dpi, dialog_dpi) : wxSize(mainframe_dpi, mainframe_dpi);
            wxSize newDPI = update_scaling_state == State::toDlg   ? wxSize(dialog_dpi, dialog_dpi) : wxSize(mainframe_dpi, mainframe_dpi);

            if (update_scaling_state == State::fromDlg)
                this->enable_force_rescale();
            else
                (&m_settings_dialog)->enable_force_rescale();

            wxWindow* win { nullptr };
            if (update_scaling_state == State::fromDlg)
                win = this;
            else
                win = &m_settings_dialog;

#if wxVERSION_EQUAL_OR_GREATER_THAN(3,1,3)
            m_tabpanel->MSWUpdateOnDPIChange(oldDPI, newDPI);
            win->GetEventHandler()->AddPendingEvent(wxDPIChangedEvent(oldDPI, newDPI));
#else
            win->GetEventHandler()->AddPendingEvent(DpiChangedEvent(EVT_DPI_CHANGED_SLICER, newDPI, win->GetRect()));
#endif // wxVERSION_EQUAL_OR_GREATER_THAN
        }
    }
#endif //__WXMSW__

//#ifdef __APPLE__
//    // Using SetMinSize() on Mac messes up the window position in some cases
//    // cf. https://groups.google.com/forum/#!topic/wx-users/yUKPBBfXWO0
//    // So, if we haven't possibility to set MinSize() for the MainFrame, 
//    // set the MinSize() as a half of regular  for the m_plater and m_tabpanel, when settings layout is in slNew mode
//    // Otherwise, MainFrame will be maximized by height
//    if (m_layout == ESettingsLayout::Hidden) {
//        wxSize size = wxGetApp().get_min_size();
//        size.SetHeight(int(0.5 * size.GetHeight()));
//        m_plater->SetMinSize(size);
//        m_tabpanel->SetMinSize(size);
//    }
//#endif
    
#ifdef __APPLE__
    m_plater->sidebar().change_top_border_for_mode_sizer(m_layout != ESettingsLayout::Tabs && m_layout != ESettingsLayout::Old);
#endif
    
    Layout();
    Thaw();
}

// Called when closing the application and when switching the application language.
void MainFrame::shutdown()
{
#ifdef _WIN32
	if (m_hDeviceNotify) {
	::UnregisterDeviceNotification(HDEVNOTIFY(m_hDeviceNotify));
	m_hDeviceNotify = nullptr;
	}
 	if (m_ulSHChangeNotifyRegister) {
        SHChangeNotifyDeregister(m_ulSHChangeNotifyRegister);
        m_ulSHChangeNotifyRegister = 0;
 	}
#endif // _WIN32

    if (m_plater != nullptr) {
        m_plater->stop_jobs();

        //close calibration dialog if opened
        wxGetApp().change_calibration_dialog(nullptr, nullptr);

        // Unbinding of wxWidgets event handling in canvases needs to be done here because on MAC,
        // when closing the application using Command+Q, a mouse event is triggered after this lambda is completed,
        // causing a crash
        m_plater->unbind_canvas_event_handlers();

        // Cleanup of canvases' volumes needs to be done here or a crash may happen on some Linux Debian flavours
        // see: https://github.com/prusa3d/PrusaSlicer/issues/3964
        m_plater->reset_canvas_volumes();
    }



    // Weird things happen as the Paint messages are floating around the windows being destructed.
    // Avoid the Paint messages by hiding the main window.
    // Also the application closes much faster without these unnecessary screen refreshes.
    // In addition, there were some crashes due to the Paint events sent to already destructed windows.
    this->Show(false);

    if (m_settings_dialog.IsShown())
        // call Close() to trigger call to lambda defined into GUI_App::persist_window_geometry()
        m_settings_dialog.Close();

    if (m_plater != nullptr) {
	// Stop the background thread (Windows and Linux).
	// Disconnect from a 3DConnextion driver (OSX).
    m_plater->get_mouse3d_controller().shutdown();
	// Store the device parameter database back to appconfig.
    m_plater->get_mouse3d_controller().save_config(*wxGetApp().app_config);
    }

    // Stop the background thread of the removable drive manager, so that no new updates will be sent to the Plater.
    wxGetApp().removable_drive_manager()->shutdown();
	//stop listening for messages from other instances
	wxGetApp().other_instance_message_handler()->shutdown(this);
    // Save the slic3r.ini.Usually the ini file is saved from "on idle" callback,
    // but in rare cases it may not have been called yet.
    wxGetApp().app_config->save();
//         if (m_plater)
//             m_plater->print = undef;
//         Slic3r::GUI::deregister_on_request_update_callback();

    // set to null tabs and a plater
    // to avoid any manipulations with them from App->wxEVT_IDLE after of the mainframe closing 
    wxGetApp().tabs_list.clear();
    wxGetApp().plater_ = nullptr;
}

//for settings when switching from fff to sla
void MainFrame::change_tab(Tab* old_tab, Tab* new_tab)
{
#ifdef _USE_CUSTOM_NOTEBOOK
    if (!wxGetApp().tabs_as_menu())
    {
        int icon_size = 0;
        try {
            icon_size = atoi(wxGetApp().app_config->get("tab_icon_size").c_str());
        }
        catch (std::exception e) {}

        Notebook* notebook = dynamic_cast<Notebook*>(m_tabpanel);
        int page_id = m_tabpanel->FindPage(old_tab);
        int bt_id = notebook->FindFirstBtPage(old_tab);
        if (page_id >= 0 && page_id < m_tabpanel->GetPageCount()) {
            m_tabpanel->GetPage(page_id)->Show(false);
            bool has_spacer = notebook->GetBtnsListCtrl()->HasSpacer(bt_id);
            m_tabpanel->RemovePage(page_id);
            notebook->InsertBtPage(bt_id, new_tab, new_tab->title(), new_tab->icon_name(icon_size, new_tab->get_printer_technology()), icon_size, false);
            if(has_spacer)
                notebook->GetBtnsListCtrl()->InsertSpacer(bt_id, 40);
#ifdef __linux__ // the tabs apparently need to be explicitly shown on Linux (pull request #1563)
            m_tabpanel->GetPage(page_id)->Show(true);
#endif // __linux__
        }
    }
    else
#endif
    {
        int page_id = m_tabpanel->FindPage(old_tab);
        if (page_id >= 0 && page_id < m_tabpanel->GetPageCount()) {
            m_tabpanel->GetPage(page_id)->Show(false);
            m_tabpanel->RemovePage(page_id);
            m_tabpanel->InsertPage(page_id, new_tab, new_tab->title());
#ifdef __linux__ // the tabs apparently need to be explicitly shown on Linux (pull request #1563)
            m_tabpanel->GetPage(page_id)->Show(true);
#endif // __linux__
            MainFrame::update_icon();
        }
    }
}

void MainFrame::update_title()
{
    wxString title = wxEmptyString;
    bool has_name = false;
    if (m_plater != nullptr) {
        // m_plater->get_project_filename() produces file name including path, but excluding extension.
        // Don't try to remove the extension, it would remove part of the file name after the last dot!
        wxString project = from_path(into_path(m_plater->get_project_filename()).filename());
//        wxString dirty_marker = (!m_plater->model().objects.empty() && m_plater->is_project_dirty()) ? "*" : "";
        wxString dirty_marker = m_plater->is_project_dirty() ? "*" : "";
        if (!dirty_marker.empty() || !project.empty()) {
            if (!dirty_marker.empty() && project.empty()) {
                if (!m_plater->model().objects.empty())
                    project = m_plater->get_project_filename();
                if (project.empty())
                    project = _L("Untitled");
            }
            title = dirty_marker + project + " - ";
        }
    }

    std::string build_id = wxGetApp().is_editor() ? SLIC3R_BUILD_ID : GCODEVIEWER_BUILD_ID;
    size_t 		idx_plus = build_id.find('+');
    if (idx_plus != build_id.npos) {
    	// Parse what is behind the '+'. If there is a number, then it is a build number after the label, and full build ID is shown.
    	int commit_after_label;
    	if (! boost::starts_with(build_id.data() + idx_plus + 1, "UNKNOWN") && 
            (build_id.at(idx_plus + 1) == '-' || sscanf(build_id.data() + idx_plus + 1, "%d-", &commit_after_label) == 0)) {
    		// It is a release build.
    		build_id.erase(build_id.begin() + idx_plus, build_id.end());    		
#if defined(_WIN32) && ! defined(_WIN64)
    		// People are using 32bit slicer on a 64bit machine by mistake. Make it explicit.
            build_id += " 32 bit";
#endif
    	}
    }

    title += wxString(SLIC3R_APP_NAME) + "_" + wxString(SLIC3R_VERSION) ;
    if (wxGetApp().is_editor() && !has_name)
        title += (" " + _L(SLIC3R_BASED_ON));

    SetTitle(title);
}

void MainFrame::init_tabpanel()
{
    // wxNB_NOPAGETHEME: Disable Windows Vista theme for the Notebook background. The theme performance is terrible on Windows 10
    // with multiple high resolution displays connected.
#ifdef _USE_CUSTOM_NOTEBOOK
    if (wxGetApp().tabs_as_menu()) {
        m_tabpanel = new wxSimplebook(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxNB_TOP | wxTAB_TRAVERSAL | wxNB_NOPAGETHEME);
        wxGetApp().UpdateDarkUI(m_tabpanel);
    }
    else
        m_tabpanel = new Notebook(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxNB_TOP | wxTAB_TRAVERSAL | wxNB_NOPAGETHEME, true);
#else
    m_tabpanel = new wxNotebook(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxNB_TOP | wxTAB_TRAVERSAL | wxNB_NOPAGETHEME);
#endif

#ifndef __WXOSX__ // Don't call SetFont under OSX to avoid name cutting in ObjectList
    m_tabpanel->SetFont(Slic3r::GUI::wxGetApp().normal_font());
#endif
    m_tabpanel->Hide();
    m_settings_dialog.set_tabpanel(m_tabpanel);

#ifndef _USE_CUSTOM_NOTEBOOK
    int icon_size = 0;
    try {
        icon_size = atoi(wxGetApp().app_config->get("tab_icon_size").c_str());
    }
    catch (std::exception e) {}
    // icons for m_tabpanel tabs
    wxImageList* img_list = nullptr;
    if (icon_size >= 8) {
        std::vector<std::string> icon_list =  { "editor_menu", "layers", "preview_menu", "cog", "spool_cog",  "printer_cog",  "resin_cog",    "sla_printer_cog" };
        if (icon_size < 16)
            icon_list =                       { "editor_menu", "layers", "preview_menu", "cog", "spool",      "printer",      "resin",        "sla_printer" };
        for (std::string icon_name : icon_list) {
            const wxBitmap& bmp = create_scaled_bitmap(icon_name, this, icon_size);
            if (img_list == nullptr)
                img_list = new wxImageList(bmp.GetWidth(), bmp.GetHeight());
            img_list->Add(bmp);
        }
    }
    m_tabpanel->AssignImageList(img_list);
#endif
#ifdef __WXMSW__
    m_tabpanel->Bind(wxEVT_BOOKCTRL_PAGE_CHANGED, [this](wxBookCtrlEvent& e) {
#else
    m_tabpanel->Bind(wxEVT_NOTEBOOK_PAGE_CHANGED, [this](wxBookCtrlEvent& e) {
#endif
        if (m_tabpanel_stop_event)
            return;
        // merill: ????? it should already be called by on_change... like other events
        //if (int old_selection = e.GetOldSelection();
        //    old_selection != wxNOT_FOUND && old_selection < static_cast<int>(m_tabpanel->GetPageCount())) {
        //    Tab* old_tab = dynamic_cast<Tab*>(m_tabpanel->GetPage(old_selection));
        //    if (old_tab)
        //        old_tab->validate_custom_gcodes();
        //}

        wxWindow* panel = m_tabpanel->GetCurrentPage();
        Tab* tab = dynamic_cast<Tab*>(panel);

        // There shouldn't be a case, when we try to select a tab, which doesn't support a printer technology
        if (panel == nullptr || (tab != nullptr && !tab->supports_printer_technology(m_plater->printer_technology())))
            return;

        std::vector<Tab*>& tabs_list = wxGetApp().tabs_list;
        int last_selected_plater_tab = m_last_selected_plater_tab;
        int last_selected_setting_tab = m_last_selected_setting_tab;
        if (tab && std::find(tabs_list.begin(), tabs_list.end(), tab) != tabs_list.end()) {
            // On GTK, the wxEVT_NOTEBOOK_PAGE_CHANGED event is triggered
            // before the MainFrame is fully set up.
            tab->OnActivate();
            if (this->m_layout == ESettingsLayout::Dlg)
                last_selected_setting_tab = m_tabpanel->GetSelection();
            else
                last_selected_setting_tab = m_tabpanel->GetSelection() - 1;
        } else if (this->m_layout == ESettingsLayout::Tabs) {
#ifdef _USE_CUSTOM_NOTEBOOK
            int bt_idx_sel = 0;
            if (wxGetApp().tabs_as_menu()) {
                bt_idx_sel = (uint8_t)get_tab_bt_selected(this->m_menubar, this->get_layout());
            } else {
                Notebook* notebook = static_cast<Notebook*>(m_tabpanel);
                //get the selected button, not the selected panel
                bt_idx_sel = notebook->GetBtSelection();
            }
            if (bt_idx_sel == 0) {
                this->m_plater->select_view_3D("3D");
            } else if (bt_idx_sel == 1) {
                if (this->m_plater->get_force_preview() != Preview::ForceState::ForceExtrusions) {
                    this->m_plater->set_force_preview(Preview::ForceState::ForceExtrusions);
                    this->m_plater->select_view_3D("Preview");
                    this->m_plater->refresh_print();
                } else
                    this->m_plater->select_view_3D("Preview");
            } else if (bt_idx_sel == 2) {
                if (this->m_plater->get_force_preview() != Preview::ForceState::ForceGcode) {
                    this->m_plater->set_force_preview(Preview::ForceState::ForceGcode);
                    this->m_plater->select_view_3D("Preview");
                    this->m_plater->refresh_print();
                } else
                    this->m_plater->select_view_3D("Preview");
            }
            m_last_selected_plater_tab = bt_idx_sel;
#else

            if (last_selected_plater_tab == m_tabpanel->GetSelection()) {
#ifdef __APPLE__
                BOOST_LOG_TRIVIAL(debug) << "Page changed to the same one (" << m_last_selected_plater_tab << ") no need to do anything\n";
#endif
                return;
            }
            bool need_freeze = !this->IsFrozen();
            bool need_freeze_plater = false;
            if(need_freeze) Freeze();
            else {
                need_freeze_plater = !m_plater->IsFrozen();
                if (need_freeze_plater) m_plater->Freeze();
            }
#ifdef __APPLE__
            BOOST_LOG_TRIVIAL(debug) << "I switched to tab  " << m_tabpanel->GetSelection() << " and so i need to change the panel position & content\n";
#endif
            size_t new_tab = m_tabpanel->GetSelection();

            size_t max = 0;
            for (int i = 0; i < 3; i++)
                max = std::max(max, m_tabpanel->GetPage(i)->GetSizer()->GetItemCount());
#ifdef __APPLE__
            BOOST_LOG_TRIVIAL(debug) << " 1 - hide & clear the sizers: " << max << "->";
#endif
            for(int i=0;i<3;i++)
                m_tabpanel->GetPage(i)->GetSizer()->Clear();
            max = 0;
            for (int i = 0; i < 3; i++)
                max = std::max(max, m_tabpanel->GetPage(i)->GetSizer()->GetItemCount());
#ifdef __APPLE__
            BOOST_LOG_TRIVIAL(debug) << max << "\n";
#endif

            m_plater->Reparent(m_tabpanel->GetCurrentPage());
#ifdef __APPLE__
            BOOST_LOG_TRIVIAL(debug) << " 2 - change parent from tab " << m_last_selected_plater_tab << " to tab " << m_tabpanel->GetSelection() << "\n";
#endif
            if (m_tabpanel->GetSelection() == 0)
                this->m_plater->select_view_3D("3D");
            else if (m_tabpanel->GetSelection() == 1) {
                if (this->m_plater->get_force_preview() != Preview::ForceState::ForceExtrusions) {
                    this->m_plater->set_force_preview(Preview::ForceState::ForceExtrusions);
                    this->m_plater->select_view_3D("Preview");
                    this->m_plater->refresh_print();
                }else
                    this->m_plater->select_view_3D("Preview");
            }
            else if (m_tabpanel->GetSelection() == 2) {
                if (this->m_plater->get_force_preview() != Preview::ForceState::ForceGcode) {
                    this->m_plater->set_force_preview(Preview::ForceState::ForceGcode);
                    this->m_plater->select_view_3D("Preview");
                    this->m_plater->refresh_print();
                }else
                    this->m_plater->select_view_3D("Preview");
            }
#ifdef __APPLE__
            BOOST_LOG_TRIVIAL(debug) << " 3 - redraw\n";
            BOOST_LOG_TRIVIAL(debug) << " 4 - add to new sizer: " << m_tabpanel->GetCurrentPage()->GetSizer()->GetItemCount() << "->";
#endif
            m_tabpanel->GetCurrentPage()->GetSizer()->Add(m_plater, 1, wxEXPAND);
#ifdef __APPLE__
            BOOST_LOG_TRIVIAL(debug) << m_tabpanel->GetCurrentPage()->GetSizer()->GetItemCount() << "\n";
#endif
            m_plater->Show();
#ifdef __APPLE__
            BOOST_LOG_TRIVIAL(debug) << "End of change for the panel position & content, tab is "<< m_tabpanel->GetSelection() <<"\n";
#endif
            m_last_selected_plater_tab = m_tabpanel->GetSelection();

            if (need_freeze) Thaw();
            else if (need_freeze_plater) m_plater->Thaw();
#ifdef __APPLE__
            m_tabpanel->ChangeSelection(new_tab);
            m_tabpanel->Refresh();
            BOOST_LOG_TRIVIAL(debug) << "Macos: force tab selection to  "<< new_tab <<" : " << m_tabpanel->GetSelection() << "\n";
#endif
            m_plater->SetFocus();
#endif
        } else {
            select_tab(MainFrame::ETabType::LastPlater); // select Plater
            m_last_selected_plater_tab = 999;
        }
    });

    m_plater = new Plater(this, this);
    m_plater->Hide();

    wxGetApp().plater_ = m_plater;

    if (wxGetApp().is_editor())
        create_preset_tabs();

    m_plater->init_after_tabs();

    if (m_plater) {
        // load initial config
        auto full_config = wxGetApp().preset_bundle->full_config();
        m_plater->on_config_change(full_config);

        // Show a correct number of filament fields.
        // nozzle_diameter is undefined when SLA printer is selected
        if (full_config.has("nozzle_diameter")) {
            m_plater->on_extruders_change(full_config.option<ConfigOptionFloats>("nozzle_diameter")->values.size());
        }
    }
}

#ifdef WIN32
void MainFrame::register_win32_callbacks()
{
    //static GUID GUID_DEVINTERFACE_USB_DEVICE  = { 0xA5DCBF10, 0x6530, 0x11D2, 0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED };
    //static GUID GUID_DEVINTERFACE_DISK        = { 0x53f56307, 0xb6bf, 0x11d0, 0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b };
    //static GUID GUID_DEVINTERFACE_VOLUME      = { 0x71a27cdd, 0x812a, 0x11d0, 0xbe, 0xc7, 0x08, 0x00, 0x2b, 0xe2, 0x09, 0x2f };
    static GUID GUID_DEVINTERFACE_HID           = { 0x4D1E55B2, 0xF16F, 0x11CF, 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 };

    // Register USB HID (Human Interface Devices) notifications to trigger the 3DConnexion enumeration.
    DEV_BROADCAST_DEVICEINTERFACE NotificationFilter = { 0 };
    NotificationFilter.dbcc_size = sizeof(DEV_BROADCAST_DEVICEINTERFACE);
    NotificationFilter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    NotificationFilter.dbcc_classguid = GUID_DEVINTERFACE_HID;
    m_hDeviceNotify = ::RegisterDeviceNotification(this->GetHWND(), &NotificationFilter, DEVICE_NOTIFY_WINDOW_HANDLE);

// or register for file handle change?
//      DEV_BROADCAST_HANDLE NotificationFilter = { 0 };
//      NotificationFilter.dbch_size = sizeof(DEV_BROADCAST_HANDLE);
//      NotificationFilter.dbch_devicetype = DBT_DEVTYP_HANDLE;

    // Using Win32 Shell API to register for media insert / removal events.
    LPITEMIDLIST ppidl;
    if (SHGetSpecialFolderLocation(this->GetHWND(), CSIDL_DESKTOP, &ppidl) == NOERROR) {
        SHChangeNotifyEntry shCNE;
        shCNE.pidl       = ppidl;
        shCNE.fRecursive = TRUE;
        // Returns a positive integer registration identifier (ID).
        // Returns zero if out of memory or in response to invalid parameters.
        m_ulSHChangeNotifyRegister = SHChangeNotifyRegister(this->GetHWND(),        // Hwnd to receive notification
            SHCNE_DISKEVENTS,                                                       // Event types of interest (sources)
            SHCNE_MEDIAINSERTED | SHCNE_MEDIAREMOVED,
            //SHCNE_UPDATEITEM,                                                     // Events of interest - use SHCNE_ALLEVENTS for all events
            WM_USER_MEDIACHANGED,                                                   // Notification message to be sent upon the event
            1,                                                                      // Number of entries in the pfsne array
            &shCNE);                                                                // Array of SHChangeNotifyEntry structures that 
                                                                                    // contain the notifications. This array should 
                                                                                    // always be set to one when calling SHChnageNotifyRegister
                                                                                    // or SHChangeNotifyDeregister will not work properly.
        assert(m_ulSHChangeNotifyRegister != 0);    // Shell notification failed
    } else {
        // Failed to get desktop location
        assert(false); 
    }

    {
        static constexpr int device_count = 1;
        RAWINPUTDEVICE devices[device_count] = { 0 };
        // multi-axis mouse (SpaceNavigator, etc.)
        devices[0].usUsagePage = 0x01;
        devices[0].usUsage = 0x08;
        if (! RegisterRawInputDevices(devices, device_count, sizeof(RAWINPUTDEVICE)))
            BOOST_LOG_TRIVIAL(error) << "RegisterRawInputDevices failed";
    }
}
#endif // _WIN32

void MainFrame::create_preset_tabs()
{
    wxGetApp().update_label_colours_from_appconfig();
    add_created_tab(new TabPrint(m_tabpanel));
    add_created_tab(new TabFilament(m_tabpanel));
    add_created_tab(new TabSLAPrint(m_tabpanel));
    add_created_tab(new TabSLAMaterial(m_tabpanel));
    add_created_tab(new TabPrinter(m_tabpanel));
}

void MainFrame::add_created_tab(Tab* panel)
{
    panel->create_preset_tab();

    const auto printer_tech = wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology();

    if (panel->supports_printer_technology(printer_tech)) {
#ifdef _USE_CUSTOM_NOTEBOOK
        if (!wxGetApp().tabs_as_menu()) {
            int icon_size = 0;
            try {
                icon_size = atoi(wxGetApp().app_config->get("tab_icon_size").c_str());
            }
            catch (std::exception e) {}
            dynamic_cast<Notebook*>(m_tabpanel)->InsertBtPage(m_tabpanel->GetPageCount(), panel, panel->title(), panel->icon_name(icon_size, printer_tech), icon_size);
        } else
#endif
        m_tabpanel->AddPage(panel, panel->title());
    }
}

bool MainFrame::is_active_and_shown_tab(Tab* tab)
{
    int page_id = m_tabpanel->FindPage(tab);

    if (m_tabpanel->GetSelection() != page_id)
        return false;

    if (m_layout == ESettingsLayout::Dlg)
        return m_settings_dialog.IsShown();

    if (m_layout == ESettingsLayout::Hidden)
        return m_main_sizer->IsShown(m_tabpanel);
    
    return true;
}

bool MainFrame::can_start_new_project() const
{
    return m_plater && (!m_plater->get_project_filename(".3mf").IsEmpty() || 
                        GetTitle().StartsWith('*')||
                        wxGetApp().has_current_preset_changes() || 
                        !m_plater->model().objects.empty() );
}

bool MainFrame::can_save() const
{
    return (m_plater != nullptr) &&
        !m_plater->canvas3D()->get_gizmos_manager().is_in_editing_mode(false) &&
        m_plater->is_project_dirty();
}

bool MainFrame::can_save_as() const
{
    return (m_plater != nullptr) &&
        !m_plater->canvas3D()->get_gizmos_manager().is_in_editing_mode(false);
}

void MainFrame::save_project()
{
    save_project_as(m_plater->get_project_filename(".3mf"));
}

bool MainFrame::save_project_as(const wxString& filename)
{
    bool ret = (m_plater != nullptr) ? m_plater->export_3mf(into_path(filename)) : false;
    if (ret) {
        // Make a copy of the active presets for detecting changes in preset values.
        wxGetApp().update_saved_preset_from_current_preset();
        // Save the names of active presets and project specific config into ProjectDirtyStateManager.
        // Reset ProjectDirtyStateManager's state as saved, mark active UndoRedo step as saved with project.
        m_plater->reset_project_dirty_after_save();
    }
    return ret;
}

bool MainFrame::can_export_model() const
{
    return (m_plater != nullptr) && !m_plater->model().objects.empty();
}

bool MainFrame::can_export_toolpaths() const
{
    return (m_plater != nullptr) && (m_plater->printer_technology() == ptFFF) && m_plater->is_preview_shown() && m_plater->is_preview_loaded() && m_plater->has_toolpaths_to_export();
}

bool MainFrame::can_export_supports() const
{
    if ((m_plater == nullptr) || (m_plater->printer_technology() != ptSLA) || m_plater->model().objects.empty())
        return false;

    bool can_export = false;
    const PrintObjects& objects = m_plater->sla_print().objects();
    for (const SLAPrintObject* object : objects)
    {
        if (object->has_mesh(slaposPad) || object->has_mesh(slaposSupportTree))
        {
            can_export = true;
            break;
        }
    }
    return can_export;
}

bool MainFrame::can_export_gcode() const
{
    if (m_plater == nullptr)
        return false;

    if (m_plater->model().objects.empty())
        return false;

    if (m_plater->is_export_gcode_scheduled())
        return false;

    // TODO:: add other filters

    return true;
}

bool MainFrame::can_send_gcode() const
{
    if (m_plater && ! m_plater->model().objects.empty())
        if (const DynamicPrintConfig *cfg = wxGetApp().preset_bundle->physical_printers.get_selected_printer_config(); cfg)
            if (const auto *print_host_opt = cfg->option<ConfigOptionString>("print_host"); print_host_opt)
                return ! print_host_opt->value.empty();
    return false;
}

bool MainFrame::can_export_gcode_sd() const
{
	if (m_plater == nullptr)
		return false;

	if (m_plater->model().objects.empty())
		return false;

	if (m_plater->is_export_gcode_scheduled())
		return false;

	// TODO:: add other filters

	return wxGetApp().removable_drive_manager()->status().has_removable_drives;
}

bool MainFrame::can_eject() const
{
	return wxGetApp().removable_drive_manager()->status().has_eject;
}

bool MainFrame::can_slice() const
{
    bool bg_proc = wxGetApp().app_config->get("background_processing") == "1";
    return (m_plater != nullptr) ? !m_plater->model().objects.empty() && !bg_proc : false;
}

bool MainFrame::can_change_view() const
{
    switch (m_layout)
    {
    default:                   { return false; }
    case ESettingsLayout::Hidden: { return m_plater->IsShown(); }
    case ESettingsLayout::Dlg: { return true; }
    case ESettingsLayout::Old: 
    case ESettingsLayout::Tabs: { 
        int page_id = m_tabpanel->GetSelection();
        return page_id != wxNOT_FOUND && selected_tab() <= ETabType::LastPlater;
    }
    case ESettingsLayout::GCodeViewer: { return true; }
    }
}

bool MainFrame::can_select() const
{
    return (m_plater != nullptr) && !m_plater->model().objects.empty();
}

bool MainFrame::can_deselect() const
{
    return (m_plater != nullptr) && !m_plater->is_selection_empty();
}

bool MainFrame::can_delete() const
{
    return (m_plater != nullptr) && !m_plater->is_selection_empty();
}

bool MainFrame::can_delete_all() const
{
    return (m_plater != nullptr) && !m_plater->model().objects.empty();
}

bool MainFrame::can_reslice() const
{
    return (m_plater != nullptr) && !m_plater->model().objects.empty();
}

void MainFrame::on_dpi_changed(const wxRect& suggested_rect)
{
    wxGetApp().update_fonts(this);
    this->SetFont(this->normal_font());

#ifdef _USE_CUSTOM_NOTEBOOK
    // update common mode sizer
    if (!wxGetApp().tabs_as_menu())
        dynamic_cast<Notebook*>(m_tabpanel)->Rescale();
#endif

    // update Plater
    wxGetApp().plater()->msw_rescale();

    // update Tabs
    if (m_layout != ESettingsLayout::Dlg) // Do not update tabs if the Settings are in the separated dialog
    for (auto tab : wxGetApp().tabs_list)
        tab->msw_rescale();

    for (size_t id = 0; id < m_menubar->GetMenuCount(); id++)
        msw_rescale_menu(m_menubar->GetMenu(id));

    // Workarounds for correct Window rendering after rescale

    /* Even if Window is maximized during moving,
     * first of all we should imitate Window resizing. So:
     * 1. cancel maximization, if it was set
     * 2. imitate resizing
     * 3. set maximization, if it was set
     */
    const bool is_maximized = this->IsMaximized();
    if (is_maximized)
        this->Maximize(false);

    /* To correct window rendering (especially redraw of a status bar)
     * we should imitate window resizing.
     */
    const wxSize& sz = this->GetSize();
    this->SetSize(sz.x + 1, sz.y + 1);
    this->SetSize(sz);

    this->Maximize(is_maximized);
}

void MainFrame::on_sys_color_changed()
{
    wxBusyCursor wait;

    // update label colors in respect to the system mode
    wxGetApp().init_label_colours();
#ifdef __WXMSW__
    wxGetApp().UpdateDarkUI(m_tabpanel);
 //   m_statusbar->update_dark_ui();
#endif
#ifdef _USE_CUSTOM_NOTEBOOK
    // update common mode sizer
    if (!wxGetApp().tabs_as_menu())
        dynamic_cast<Notebook*>(m_tabpanel)->Rescale();
#endif

    // update Plater
    wxGetApp().plater()->sys_color_changed();

    // update Tabs
    for (auto tab : wxGetApp().tabs_list)
        tab->sys_color_changed();

    MenuFactory::sys_color_changed(m_menubar);

    this->Refresh();
}

#ifdef _MSC_VER
    // \xA0 is a non-breaking space. It is entered here to spoil the automatic accelerators,
    // as the simple numeric accelerators spoil all numeric data entry.
    // note: same for letters
    // note: don't work anymore, it doesn't show them to the right. Revert to " - "
static const wxString sep = " - ";//\t\xA0
static const wxString sep_space = "\xA0";
#else
static const wxString sep = " - ";
static const wxString sep_space = "";
#endif

static wxMenu* generate_help_menu()
{
    wxMenu* helpMenu = new wxMenu();
    append_menu_item(helpMenu, wxID_ANY, wxString::Format(_L("%s Releases"), SLIC3R_APP_NAME), wxString::Format(_L("Open the %s releases page in your browser"), SLIC3R_APP_NAME),
        [](wxCommandEvent&) { wxGetApp().open_browser_with_warning_dialog(SLIC3R_DOWNLOAD, nullptr, false); });
    append_menu_item(helpMenu, wxID_ANY, wxString::Format(_L("%s wiki"), SLIC3R_APP_NAME), wxString::Format(_L("Open the %s wiki in your browser"), SLIC3R_APP_NAME),
        [](wxCommandEvent&) { wxGetApp().open_browser_with_warning_dialog("http://github.com/" SLIC3R_GITHUB "/wiki"); });
    append_menu_item(helpMenu, wxID_ANY, wxString::Format(_L("%s website"), SLIC3R_APP_NAME), _L("Open the Slic3r website in your browser"),
        [](wxCommandEvent&) { wxGetApp().open_browser_with_warning_dialog("http://slic3r.org"); });
    //#        my $versioncheck = $self->_append_menu_item($helpMenu, "Check for &Updates...", "Check for new Slic3r versions", sub{
    //#            wxTheApp->check_version(1);
    //#        });
    //#        $versioncheck->Enable(wxTheApp->have_version_check);
    append_menu_item(helpMenu, wxID_ANY, wxString::Format(_L("Slic3r Manual")),
        wxString::Format(_L("Open the Slic3r Manual in your browser")),
        //            [this](wxCommandEvent&) { wxGetApp().open_web_page_localized("http://manual.slic3r.org"); });
        //        append_menu_item(helpMenu, wxID_ANY, wxString::Format(_L("%s &Manual"), SLIC3R_APP_NAME),
        //                                             wxString::Format(_L("Open the %s manual in your browser"), SLIC3R_APP_NAME),
        [](wxCommandEvent&) { wxGetApp().open_browser_with_warning_dialog("http://manual.slic3r.org/"); });
    helpMenu->AppendSeparator();
    append_menu_item(helpMenu, wxID_ANY, _L("System &Info"), _L("Show system information"),
        [](wxCommandEvent&) { wxGetApp().system_info(); });
    append_menu_item(helpMenu, wxID_ANY, _L("Show &Configuration Folder"), _L("Show user configuration folder (datadir)"),
        [](wxCommandEvent&) { Slic3r::GUI::desktop_open_datadir_folder(); });
    append_menu_item(helpMenu, wxID_ANY, _L("Report an I&ssue"), wxString::Format(_L("Report an issue on %s"), SLIC3R_APP_NAME),
        [](wxCommandEvent&) { wxGetApp().open_browser_with_warning_dialog("http://github.com/" SLIC3R_GITHUB "/issues/new", nullptr, false); });

    if (wxGetApp().is_editor())
        append_menu_item(helpMenu, wxID_ANY, wxString::Format(_L("&About %s"), SLIC3R_APP_NAME), _L("Show about dialog"),
            [](wxCommandEvent&) { Slic3r::GUI::about(); });
    else
        append_menu_item(helpMenu, wxID_ANY, wxString::Format(_L("&About %s"), GCODEVIEWER_APP_NAME), _L("Show about dialog"),
            [](wxCommandEvent&) { Slic3r::GUI::about(); });
    append_menu_item(helpMenu, wxID_ANY, _L("Show Tip of the Day") 
#if 0//debug
        + "\tCtrl+Shift+T"
#endif
        ,_L("Opens Tip of the day notification in bottom right corner or shows another tip if already opened."),
        [](wxCommandEvent&) { wxGetApp().plater()->get_notification_manager()->push_hint_notification(false); });
    helpMenu->AppendSeparator();
    append_menu_item(helpMenu, wxID_ANY, _L("Keyboard Shortcuts") + sep + "&?", _L("Show the list of the keyboard shortcuts"),
        [](wxCommandEvent&) { wxGetApp().keyboard_shortcuts(); });
#if ENABLE_THUMBNAIL_GENERATOR_DEBUG
    helpMenu->AppendSeparator();
    append_menu_item(helpMenu, wxID_ANY, "DEBUG gcode thumbnails", "DEBUG ONLY - read the selected gcode file and generates png for the contained thumbnails",
        [](wxCommandEvent&) { wxGetApp().gcode_thumbnails_debug(); });
#endif // ENABLE_THUMBNAIL_GENERATOR_DEBUG

    return helpMenu;
}

static void add_common_view_menu_items(wxMenu* view_menu, MainFrame* mainFrame, std::function<bool(void)> can_change_view)
{
    // The camera control accelerators are captured by GLCanvas3D::on_char(). So be sure to don't activate the accelerator by using '\t'
    append_menu_item(view_menu, wxID_ANY, _L("Iso") + sep + "&0", _L("Iso View"), [mainFrame](wxCommandEvent&) { mainFrame->select_view("iso"); },
        "", nullptr, [can_change_view]() { return can_change_view(); }, mainFrame);
    view_menu->AppendSeparator();
    //TRN To be shown in the main menu View->Top 
    append_menu_item(view_menu, wxID_ANY, _L("Top") + sep + "&1", _L("Top View"), [mainFrame](wxCommandEvent&) { mainFrame->select_view("top"); },
        "", nullptr, [can_change_view]() { return can_change_view(); }, mainFrame);
    //TRN To be shown in the main menu View->Bottom 
    append_menu_item(view_menu, wxID_ANY, _L("Bottom") + sep + "&2", _L("Bottom View"), [mainFrame](wxCommandEvent&) { mainFrame->select_view("bottom"); },
        "", nullptr, [can_change_view]() { return can_change_view(); }, mainFrame);
    append_menu_item(view_menu, wxID_ANY, _L("Front") + sep + "&3", _L("Front View"), [mainFrame](wxCommandEvent&) { mainFrame->select_view("front"); },
        "", nullptr, [can_change_view]() { return can_change_view(); }, mainFrame);
    append_menu_item(view_menu, wxID_ANY, _L("Rear") + sep + "&4", _L("Rear View"), [mainFrame](wxCommandEvent&) { mainFrame->select_view("rear"); },
        "", nullptr, [can_change_view]() { return can_change_view(); }, mainFrame);
    append_menu_item(view_menu, wxID_ANY, _L("Left") + sep + "&5", _L("Left View"), [mainFrame](wxCommandEvent&) { mainFrame->select_view("left"); },
        "", nullptr, [can_change_view]() { return can_change_view(); }, mainFrame);
    append_menu_item(view_menu, wxID_ANY, _L("Right") + sep + "&6", _L("Right View"), [mainFrame](wxCommandEvent&) { mainFrame->select_view("right"); },
        "", nullptr, [can_change_view]() { return can_change_view(); }, mainFrame);
}

void MainFrame::init_menubar_as_editor()
{
#ifdef __APPLE__
    wxMenuBar::SetAutoWindowMenu(false);
#endif

    // File menu
    wxMenu* fileMenu = new wxMenu;
    {
        append_menu_item(fileMenu, wxID_ANY, _L("&New Project") + "\tCtrl+N", _L("Start a new project"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->new_project(); }, "", nullptr,
            [this](){return m_plater != nullptr && can_start_new_project(); }, this);
        append_menu_item(fileMenu, wxID_ANY, _L("&Open Project") + dots + "\tCtrl+O", _L("Open a project file"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->load_project(); }, "open", nullptr,
            [this](){return m_plater != nullptr; }, this);

        wxMenu* recent_projects_menu = new wxMenu();
        wxMenuItem* recent_projects_submenu = append_submenu(fileMenu, recent_projects_menu, wxID_ANY, _L("Recent projects"), "");
        m_recent_projects.UseMenu(recent_projects_menu);
        Bind(wxEVT_MENU, [this](wxCommandEvent& evt) {
            size_t file_id = evt.GetId() - wxID_FILE1;
            wxString filename = m_recent_projects.GetHistoryFile(file_id);
            if (wxFileExists(filename)) {
                if (wxGetApp().can_load_project())
                    m_plater->load_project(filename);
            }
            else
            {
                //wxMessageDialog msg(this, _L("The selected project is no longer available.\nDo you want to remove it from the recent projects list?"), _L("Error"), wxYES_NO | wxYES_DEFAULT);
                MessageDialog msg(this, _L("The selected project is no longer available.\nDo you want to remove it from the recent projects list?"), _L("Error"), wxYES_NO | wxYES_DEFAULT);
                if (msg.ShowModal() == wxID_YES)
                {
                    m_recent_projects.RemoveFileFromHistory(file_id);
                        std::vector<std::string> recent_projects;
                        size_t count = m_recent_projects.GetCount();
                        for (size_t i = 0; i < count; ++i)
                        {
                            recent_projects.push_back(into_u8(m_recent_projects.GetHistoryFile(i)));
                        }
                    wxGetApp().app_config->set_recent_projects(recent_projects);
                    wxGetApp().app_config->save();
                }
            }
            }, wxID_FILE1, wxID_FILE9);

        std::vector<std::string> recent_projects = wxGetApp().app_config->get_recent_projects();
        std::reverse(recent_projects.begin(), recent_projects.end());
        for (const std::string& project : recent_projects)
        {
            m_recent_projects.AddFileToHistory(from_u8(project));
        }

        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt) { evt.Enable(m_recent_projects.GetCount() > 0); }, recent_projects_submenu->GetId());

        append_menu_item(fileMenu, wxID_ANY, _L("&Save Project") + "\tCtrl+S", _L("Save current project file"),
            [this](wxCommandEvent&) { save_project(); }, "save", nullptr,
            [this](){return m_plater != nullptr && can_save(); }, this);
#ifdef __APPLE__
        append_menu_item(fileMenu, wxID_ANY, _L("Save Project &as") + dots + "\tCtrl+Shift+S", _L("Save current project file as"),
#else
        append_menu_item(fileMenu, wxID_ANY, _L("Save Project &as") + dots + "\tCtrl+Alt+S", _L("Save current project file as"),
#endif // __APPLE__
            [this](wxCommandEvent&) { save_project_as(); }, "save", nullptr,
            [this](){return m_plater != nullptr && can_save_as(); }, this);

        fileMenu->AppendSeparator();

        wxMenu* import_menu = new wxMenu();
        append_menu_item(import_menu, wxID_ANY, _L("Import STL/3MF/STEP/OBJ/AM&F") + dots + "\tCtrl+I", _L("Load a model"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->add_model(); }, "import_plater", nullptr,
            [this](){return m_plater != nullptr; }, this);
        
        append_menu_item(import_menu, wxID_ANY, _L("Import STL (Imperial Units)"), _L("Load an model saved with imperial units"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->add_model(true); }, "import_plater", nullptr,
            [this](){return m_plater != nullptr; }, this);
        
        append_menu_item(import_menu, wxID_ANY, _L("Import SL1 / SL1S Archive") + dots, _L("Load an SL1 / Sl1S archive"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->import_sl1_archive(); }, "import_plater", nullptr,
            [this](){return m_plater != nullptr && !m_plater->is_any_job_running(); }, this);
    
        import_menu->AppendSeparator();
        append_menu_item(import_menu, wxID_ANY, _L("Import &Config") + dots + "\tCtrl+L", _L("Load exported configuration file"),
            [this](wxCommandEvent&) { load_config_file(); }, "import_config", nullptr,
            []() {return true; }, this);
        append_menu_item(import_menu, wxID_ANY, _L("Import Prusa Config") + dots, _L("Load configuration file exported from PrusaSlicer"),
            [this](wxCommandEvent&) { load_config_file(true); }, "import_prusa_config", nullptr,
            []() {return true; }, this);
        append_menu_item(import_menu, wxID_ANY, _L("Import Config from &Project") + dots +"\tCtrl+Alt+L", _L("Load configuration from project file"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->extract_config_from_project(); }, "import_config", nullptr,
            []() {return true; }, this);
        import_menu->AppendSeparator();
        append_menu_item(import_menu, wxID_ANY, _L("Import Config &Bundle") + dots, _L("Load presets from a bundle"),
            [this](wxCommandEvent&) { load_configbundle(); }, "import_config_bundle", nullptr,
            []() {return true; }, this);
        append_menu_item(import_menu, wxID_ANY, _L("Import Prusa Config Bundle") + dots, _L("Load presets from a PrusaSlicer bundle"),
            [this](wxCommandEvent&) { load_configbundle(wxEmptyString, true); }, "import_prusa_config_bundle", nullptr,
            []() {return true; }, this);
        append_submenu(fileMenu, import_menu, wxID_ANY, _L("&Import"), "");

        wxMenu* export_menu = new wxMenu();
        wxMenuItem* item_export_gcode = append_menu_item(export_menu, wxID_ANY, _L("Export &G-code") + dots +"\tCtrl+G", _L("Export current plate as G-code"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->export_gcode(false); }, "export_gcode", nullptr,
            [this](){return can_export_gcode(); }, this);
        m_changeable_menu_items.push_back(item_export_gcode);
        wxMenuItem* item_send_gcode = append_menu_item(export_menu, wxID_ANY, _L("S&end G-code") + dots +"\tCtrl+Shift+G", _L("Send to print current plate as G-code"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->send_gcode(); }, "export_gcode", nullptr,
            [this](){return can_send_gcode(); }, this);
        m_changeable_menu_items.push_back(item_send_gcode);
		append_menu_item(export_menu, wxID_ANY, _L("Export G-code to SD Card / Flash Drive") + dots + "\tCtrl+U", _L("Export current plate as G-code to SD card / Flash drive"),
			[this](wxCommandEvent&) { if (m_plater) m_plater->export_gcode(true); }, "export_to_sd", nullptr,
			[this]() {return can_export_gcode_sd(); }, this);
        export_menu->AppendSeparator();
        append_menu_item(export_menu, wxID_ANY, _L("Export &Plate") + dots, _L("Export current plate (options available in the dialog)"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->export_platter(); }, "export_plater", nullptr,
            [this](){return can_export_model(); }, this);
        // now via option in the export dialog
        //append_menu_item(export_menu, wxID_ANY, _L("Export Plate as STL &Including Supports") + dots, _L("Export current plate as STL including supports"),
        //    [this](wxCommandEvent&) { if (m_plater) m_plater->export_stl(true); }, "export_plater", nullptr,
        //    [this](){return can_export_supports(); }, this);
// Deprecating AMF export. Let's wait for user feedback.
//        append_menu_item(export_menu, wxID_ANY, _L("Export Plate as &AMF") + dots, _L("Export current plate as AMF"),
//            [this](wxCommandEvent&) { if (m_plater) m_plater->export_amf(); }, "export_plater", nullptr,
//            [this](){return can_export_model(); }, this);
        export_menu->AppendSeparator();
        append_menu_item(export_menu, wxID_ANY, _L("Export &Toolpaths as OBJ") + dots, _L("Export toolpaths as OBJ"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->export_toolpaths_to_obj(); }, "export_plater", nullptr,
            [this]() {return can_export_toolpaths(); }, this);
        export_menu->AppendSeparator();
        append_menu_item(export_menu, wxID_ANY, _L("Export &Config") + dots +"\tCtrl+E", _L("Export current configuration to file"),
            [this](wxCommandEvent&) { export_config(); }, "export_config", nullptr,
            []() {return true; }, this);
        append_menu_item(export_menu, wxID_ANY, _L("Export Config &Bundle") + dots, _L("Export all presets to file"),
            [this](wxCommandEvent&) { export_configbundle(); }, "export_config_bundle", nullptr,
            []() {return true; }, this);
        append_menu_item(export_menu, wxID_ANY, _L("Export Config Bundle With Physical Printers") + dots, _L("Export all presets including physical printers to file"),
            [this](wxCommandEvent&) { export_configbundle(true); }, "export_config_bundle", nullptr,
            []() {return true; }, this);
        export_menu->AppendSeparator();
        append_menu_item(export_menu, wxID_ANY, _L("Export to &Prusa Config") + dots, _L("Export current configuration to file, with only settings compatible with PrusaSlicer"),
            [this](wxCommandEvent&) { export_config(true); }, "export_prusa_config", nullptr,
            []() {return true; }, this);
        append_submenu(fileMenu, export_menu, wxID_ANY, _L("&Export"), "");

		append_menu_item(fileMenu, wxID_ANY, _L("Ejec&t SD Card / Flash Drive") + dots + "\tCtrl+T", _L("Eject SD card / Flash drive after the G-code was exported to it."),
			[this](wxCommandEvent&) { if (m_plater) m_plater->eject_drive(); }, "eject_sd", nullptr,
			[this]() {return can_eject(); }, this);

        fileMenu->AppendSeparator();

#if 0
        m_menu_item_repeat = nullptr;
        append_menu_item(fileMenu, wxID_ANY, _L("Quick Slice") +dots+ "\tCtrl+U", _L("Slice a file into a G-code"),
            [this](wxCommandEvent&) {
                wxTheApp->CallAfter([this]() {
                    quick_slice();
                    m_menu_item_repeat->Enable(is_last_input_file());
                }); }, "cog_go.png");
        append_menu_item(fileMenu, wxID_ANY, _L("Quick Slice and Save As") +dots +"\tCtrl+Alt+U", _L("Slice a file into a G-code, save as"),
            [this](wxCommandEvent&) {
            wxTheApp->CallAfter([this]() {
                    quick_slice(qsSaveAs);
                    m_menu_item_repeat->Enable(is_last_input_file());
                }); }, "cog_go.png");
        m_menu_item_repeat = append_menu_item(fileMenu, wxID_ANY, _L("Repeat Last Quick Slice") +"\tCtrl+Shift+U", _L("Repeat last quick slice"),
            [this](wxCommandEvent&) {
            wxTheApp->CallAfter([this]() {
                quick_slice(qsReslice);
            }); }, "cog_go.png");
        m_menu_item_repeat->Enable(false);
        fileMenu->AppendSeparator();
#endif
        m_menu_item_reslice_now = append_menu_item(fileMenu, wxID_ANY, _L("(Re)Slice No&w") + "\tCtrl+R", _L("Start new slicing process"),
            [this](wxCommandEvent&) { reslice_now(); }, "re_slice", nullptr,
            [this]() { return m_plater != nullptr && can_reslice(); }, this);
        fileMenu->AppendSeparator();
        append_menu_item(fileMenu, wxID_ANY, _L("&Repair STL file") + dots, _L("Automatically repair an STL file"),
            [this](wxCommandEvent&) { repair_stl(); }, "wrench", nullptr,
            []() { return true; }, this);
        fileMenu->AppendSeparator();
        append_menu_item(fileMenu, wxID_ANY, _L("&G-code Preview") + dots, _L("Open G-code viewer"),
            [this](wxCommandEvent&) { start_new_gcodeviewer_open_file(this); }, "", nullptr);
        fileMenu->AppendSeparator();
        append_menu_item(fileMenu, wxID_EXIT, _L("&Quit"), GUI::format_wxstr(_L("Quit %s"), SLIC3R_APP_NAME),
            [this](wxCommandEvent&) { Close(false); }, "exit");
    }

    // Edit menu
    //some are protected by can_change_view() to be able to use them differently in setting items
    wxMenu* editMenu = nullptr;
    if (m_plater != nullptr)
    {
        editMenu = new wxMenu();
    #ifdef __APPLE__
        // Backspace sign
        wxString hotkey_delete = "\u232b";
    #else
        wxString hotkey_delete = "Del";
    #endif
        append_menu_item(editMenu, wxID_ANY, _L("&Select All") + "\tCtrl+A",
            _L("Selects all objects"), [this](wxCommandEvent&) { m_plater->select_all(); },
            "", nullptr, [this](){return can_select() && can_change_view(); }, this);
        append_menu_item(editMenu, wxID_ANY, _L("D&eselect All") + "\tEsc",
            _L("Deselects all objects"), [this](wxCommandEvent&) { m_plater->deselect_all(); },
            "", nullptr, [this](){return can_deselect() && can_change_view(); }, this);
        editMenu->AppendSeparator();
        append_menu_item(editMenu, wxID_ANY, _L("&Delete Selected") + "\t" + /*hotkey_delete don't use the real escape key, or it will prevent del on some fields*/ "Dèl",
            _L("Deletes the current selection"),[this](wxCommandEvent&) { m_plater->remove_selected(); },
            "remove_menu", nullptr, [this](){return can_delete() && can_change_view(); }, this);
        append_menu_item(editMenu, wxID_ANY, _L("Delete &All") + "\tCtrl+" + hotkey_delete,
            _L("Deletes all objects"), [this](wxCommandEvent&) { m_plater->reset_with_confirm(); },
            "delete_all_menu", nullptr, [this](){return can_delete_all() && can_change_view(); }, this);

        editMenu->AppendSeparator();
        append_menu_item(editMenu, wxID_ANY, _L("&Undo") + "\tCtrl+Z",
            _L("Undo"), [this](wxCommandEvent&) { m_plater->undo(); },
            "undo_menu", nullptr, [this](){return m_plater->can_undo() && can_change_view(); }, this);
        append_menu_item(editMenu, wxID_ANY, _L("&Redo") + "\tCtrl+Y",
            _L("Redo"), [this](wxCommandEvent&) { m_plater->redo(); },
            "redo_menu", nullptr, [this](){return m_plater->can_redo() && can_change_view(); }, this);

        editMenu->AppendSeparator();
        append_menu_item(editMenu, wxID_ANY, _L("&Copy") + "\tCtrl+C",
            _L("Copy selection to clipboard"), [this](wxCommandEvent&) { m_plater->copy_selection_to_clipboard(); },
            "copy_menu", nullptr, [this](){return m_plater->can_copy_to_clipboard() && can_change_view(); }, this);
        append_menu_item(editMenu, wxID_ANY, _L("&Paste") + "\tCtrl+V",
            _L("Paste clipboard"), [this](wxCommandEvent&) { m_plater->paste_from_clipboard(); },
            "paste_menu", nullptr, [this](){return m_plater->can_paste_from_clipboard() && can_change_view(); }, this);
        
        editMenu->AppendSeparator();
#ifdef __APPLE__
        append_menu_item(editMenu, wxID_ANY, _L("Re&load from Disk") + dots + "\tCtrl+Shift+R",
            _L("Reload the platter from disk"), [this](wxCommandEvent&) { m_plater->reload_all_from_disk(); },
            "", nullptr, [this]() {return !m_plater->model().objects.empty(); }, this);
#else
        append_menu_item(editMenu, wxID_ANY, _L("Re&load from Disk") + "\t" + "F5",
            _L("Reload the plater from disk"), [this](wxCommandEvent&) { m_plater->reload_all_from_disk(); },
            "", nullptr, [this]() {return !m_plater->model().objects.empty(); }, this);
#endif // __APPLE__

        editMenu->AppendSeparator();
        append_menu_item(editMenu, wxID_ANY, _L("Searc&h") + "\tCtrl+F",
            _L("Search in settings"), [this](wxCommandEvent&) { m_plater->search(/*m_tabpanel->GetCurrentPage() == */m_plater->IsShown() && can_change_view()); },
            "search", nullptr, []() { return true; }, this);
    }

    // Window menu
    auto windowMenu = new wxMenu();
    {
        if (m_plater) {
            append_menu_item(windowMenu, wxID_HIGHEST + 1, _L("3D &Platter Tab") + "\tCtrl+1", _L("Show the editor of the input models"),
                [this](wxCommandEvent&) { select_tab(ETabType::Plater3D); }, "editor_menu", nullptr,
                []() {return true; }, this);
            m_layerpreview_menu_item = append_menu_item(windowMenu, wxID_HIGHEST + 2, _L("Layer previe&w Tab") + "\tCtrl+2", _L("Show the layers from the slicing process"),
                [this](wxCommandEvent&) { select_tab(ETabType::PlaterPreview); }, "layers", nullptr,
                []() {return true; }, this);
            append_menu_item(windowMenu, wxID_HIGHEST + 3, _L("GCode Pre&view Tab") + "\tCtrl+3", _L("Show the preview of the gcode output"),
                [this](wxCommandEvent&) { select_tab(ETabType::PlaterGcode); }, "preview_menu", nullptr,
                []() {return true; }, this);
            windowMenu->AppendSeparator();
        }
        append_menu_item(windowMenu, wxID_HIGHEST + 4, _L("P&rint Settings Tab") + "\tCtrl+4", _L("Show the print settings"),
            [this/*, tab_offset*/](wxCommandEvent&) { select_tab(ETabType::PrintSettings); }, "cog", nullptr,
            []() {return true; }, this);
        wxMenuItem* item_material_tab = append_menu_item(windowMenu, wxID_HIGHEST + 5, _L("&Filament Settings Tab") + "\tCtrl+5", _L("Show the filament settings"),
            [this/*, tab_offset*/](wxCommandEvent&) { select_tab(ETabType::FilamentSettings); }, "spool", nullptr,
            []() {return true; }, this);
        m_changeable_menu_items.push_back(item_material_tab);
        wxMenuItem* item_printer_tab = append_menu_item(windowMenu, wxID_HIGHEST + 6, _L("Print&er Settings Tab") + "\tCtrl+6", _L("Show the printer settings"),
            [this/*, tab_offset*/](wxCommandEvent&) { select_tab(ETabType::PrinterSettings); }, "printer", nullptr,
            []() {return true; }, this);
        m_changeable_menu_items.push_back(item_printer_tab);

        windowMenu->AppendSeparator();
        append_menu_item(windowMenu, wxID_ANY, _L("Shape Gallery"), _L("Open the dialog to modify shape gallery"),
            [this](wxCommandEvent&) { 
                GalleryDialog dlg(this, true);
                if (dlg.ShowModal() == wxID_OK) {
                    wxArrayString input_files;
                    dlg.get_input_files(input_files);
                    if (!input_files.IsEmpty())
                        m_plater->sidebar().obj_list()->load_shape_object_from_gallery(input_files);
                }
            }, "shape_gallery", nullptr, []() {return true; }, this);
        
        windowMenu->AppendSeparator();
        append_menu_item(windowMenu, wxID_ANY, _L("Print &Host Upload Queue") + "\tCtrl+J", _L("Display the Print Host Upload Queue window"),
            [this](wxCommandEvent&) { m_printhost_queue_dlg->Show(); }, "upload_queue", nullptr, []() {return true; }, this);
        
        windowMenu->AppendSeparator();
        append_menu_item(windowMenu, wxID_ANY, _L("Open New Instance") + "\tCtrl+Shift+" + "I", wxString::Format(_L("Open a new %s instance"), SLIC3R_APP_NAME),
            [this](wxCommandEvent&) { start_new_slicer(); }, "", nullptr, [this]() {return m_plater != nullptr && wxGetApp().app_config->get("single_instance") != "1"; }, this);

        windowMenu->AppendSeparator();
        append_menu_item(windowMenu, wxID_ANY, _L("Compare Presets")/* + "\tCtrl+F"*/, _L("Compare presets"), 
            [this](wxCommandEvent&) { diff_dialog.show();}, "compare", nullptr, []() {return true; }, this);
    }

    // View menu
    wxMenu* viewMenu = nullptr;
    if (m_plater) {
        viewMenu = new wxMenu();
        add_common_view_menu_items(viewMenu, this, std::bind(&MainFrame::can_change_view, this));
        viewMenu->AppendSeparator();
        append_menu_check_item(viewMenu, wxID_ANY, _L("Show &Labels") + sep + "&e", _L("Show object/instance labels in 3D scene"),
            [this](wxCommandEvent&) { m_plater->show_view3D_labels(!m_plater->are_view3D_labels_shown()); /* only called on clic, real event is handled by GLCanvas3D::on_char */ }, this,
            [this]() { return m_plater->is_view3D_shown(); }, [this]() { return m_plater->are_view3D_labels_shown(); }, this);
        append_menu_check_item(viewMenu, wxID_ANY, _L("&Collapse Sidebar") + "\t" + "Shift+" + sep_space + "Tab", _L("Collapse sidebar"),
            [this](wxCommandEvent&) { m_plater->collapse_sidebar(!m_plater->is_sidebar_collapsed()); }, this,
            [this]() { return can_change_view(); }, [this]() { return m_plater->is_sidebar_collapsed(); }, this);
#ifndef __APPLE__
        // OSX adds its own menu item to toggle fullscreen.
        append_menu_check_item(viewMenu, wxID_ANY, _L("&Fullscreen") + "\t" + "F11", _L("Fullscreen"),
            [this](wxCommandEvent&) { this->ShowFullScreen(!this->IsFullScreen(), 
                // wxFULLSCREEN_ALL: wxFULLSCREEN_NOMENUBAR | wxFULLSCREEN_NOTOOLBAR | wxFULLSCREEN_NOSTATUSBAR | wxFULLSCREEN_NOBORDER | wxFULLSCREEN_NOCAPTION
                wxFULLSCREEN_NOSTATUSBAR | wxFULLSCREEN_NOBORDER | wxFULLSCREEN_NOCAPTION); }, 
            this, []() { return true; }, [this]() { return this->IsFullScreen(); }, this);
#endif // __APPLE__
    }

    // calibration menu
    m_calibration_menu = nullptr;
    if (wxGetApp().is_editor())
    {
        m_calibration_menu = new wxMenu();
        append_menu_item(m_calibration_menu, wxID_ANY, _(L("Introduction")), _(L("How to use this menu and calibrations.")),
            [this](wxCommandEvent&) { wxGetApp().html_dialog(); });
        m_calibration_menu->AppendSeparator();
        append_menu_item(m_calibration_menu, wxID_ANY, _(L("Bed/Extruder leveling")), _(L("Create a test print to help you to level your printer bed.")),
            [this](wxCommandEvent&) { wxGetApp().bed_leveling_dialog(); });
        m_calibration_menu->AppendSeparator();
        append_menu_item(m_calibration_menu, wxID_ANY, _(L("Filament Flow calibration")), _(L("Create a test print to help you to set your filament extrusion multiplier.")),
            [this](wxCommandEvent&) { wxGetApp().flow_ratio_dialog(); });
        append_menu_item(m_calibration_menu, wxID_ANY, _(L("Filament temperature calibration")), _(L("Create a test print to help you to set your filament temperature.")),
            [this](wxCommandEvent&) { wxGetApp().filament_temperature_dialog(); });
        append_menu_item(m_calibration_menu, wxID_ANY, _(L("Extruder retraction calibration")), _(L("Create a test print to help you to set your retraction length.")),
            [this](wxCommandEvent&) { wxGetApp().calibration_retraction_dialog(); });
        m_calibration_menu->AppendSeparator();
        append_menu_item(m_calibration_menu, wxID_ANY, _(L("Bridge flow calibration")), _(L("Create a test print to help you to set your bridge flow ratio.")),
            [this](wxCommandEvent&) { wxGetApp().bridge_tuning_dialog(); });
        append_menu_item(m_calibration_menu, wxID_ANY, _(L("Ironing pattern calibration")), _(L("Create a test print to help you to set your over-bridge flow ratio and ironing pattern.")),
            [this](wxCommandEvent&) { wxGetApp().over_bridge_dialog(); });
        m_calibration_menu->AppendSeparator();
        append_menu_item(m_calibration_menu, wxID_ANY, _(L("Calibration cube")), _(L("Print a calibration cube, for various calibration goals.")),
            [this](wxCommandEvent&) { wxGetApp().calibration_cube_dialog(); });
    }

    // objects menu
    wxMenu* generationMenu = nullptr;
    if (wxGetApp().is_editor())
    {
        generationMenu  = new wxMenu();
        append_menu_item(generationMenu, wxID_ANY, _(L("FreeCad python script")), _(L("Create an object by writing little easy script.")),
            [this](wxCommandEvent&) { wxGetApp().freecad_script_dialog(); });
        append_menu_item(generationMenu, wxID_ANY, _(L("Script help page")), _(L("How to use the FreeCad python script window.")),
            [this](wxCommandEvent&) { wxLaunchDefaultBrowser("https://github.com/supermerill/SuperSlicer/wiki/FreePySCAD-script-window"); });
        generationMenu->AppendSeparator();
        append_menu_item(generationMenu, wxID_ANY, _(L("Mosaic from picture")), _(L("Create an mosaic-like tile with filament changes.")),
            [this](wxCommandEvent&) { wxGetApp().tiled_canvas_dialog(); });

    }

    // Help menu
    auto helpMenu = generate_help_menu();

    // menubar
    // assign menubar to frame after appending items, otherwise special items
    // will not be handled correctly
    m_menubar = new wxMenuBar();
    m_menubar->Append(fileMenu, _L("&File"));
    if (editMenu) m_menubar->Append(editMenu, _L("&Edit"));
    m_menubar->Append(windowMenu, _L("&Window"));
    if (viewMenu) m_menubar->Append(viewMenu, _L("&View"));
    if (m_calibration_menu) m_menubar->Append(m_calibration_menu, _L("C&alibration"));
    if (generationMenu) m_menubar->Append(generationMenu, _L("&Generate"));
    // Add additional menus from C++
    wxGetApp().add_config_menu(m_menubar);
    m_menubar->Append(helpMenu, _L("&Help"));

#ifdef _USE_CUSTOM_NOTEBOOK
    if (wxGetApp().tabs_as_menu()) {
        add_tabs_as_menu(m_menubar, this, this);
    }
#endif
    SetMenuBar(m_menubar);

#ifdef __APPLE__
    // This fixes a bug on Mac OS where the quit command doesn't emit window close events
    // wx bug: https://trac.wxwidgets.org/ticket/18328
    wxMenu* apple_menu = m_menubar->OSXGetAppleMenu();
    if (apple_menu != nullptr) {
        apple_menu->Bind(wxEVT_MENU, [this](wxCommandEvent &) {
            Close();
        }, wxID_EXIT);
    }
#endif // __APPLE__

    if (plater()->printer_technology() == ptSLA)
        update_menubar();
}

void MainFrame::open_menubar_item(const wxString& menu_name,const wxString& item_name)
{
    if (m_menubar == nullptr)
        return;
    // Get menu object from menubar
    int     menu_index = m_menubar->FindMenu(menu_name);
    wxMenu* menu       = m_menubar->GetMenu(menu_index);
    if (menu == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "Mainframe open_menubar_item function couldn't find menu: " << menu_name;
        return;
    }
    // Get item id from menu
    int     item_id   = menu->FindItem(item_name);
    if (item_id == wxNOT_FOUND)
    {
        // try adding three dots char
        item_id = menu->FindItem(item_name + dots);
    }
    if (item_id == wxNOT_FOUND)
    {
        BOOST_LOG_TRIVIAL(error) << "Mainframe open_menubar_item function couldn't find item: " << item_name;
        return;
    }
    // wxEVT_MENU will trigger item
    wxPostEvent((wxEvtHandler*)menu, wxCommandEvent(wxEVT_MENU, item_id));
}

void MainFrame::init_menubar_as_gcodeviewer()
{
    wxMenu* fileMenu = new wxMenu;
    {
        append_menu_item(fileMenu, wxID_ANY, _L("&Open G-code") + dots + sep + GUI::shortkey_ctrl_prefix() + "O", _L("Open a G-code file"),
            [this](wxCommandEvent&) { if (m_plater != nullptr) m_plater->load_gcode(); }, "open", nullptr,
            [this]() {return m_plater != nullptr; }, this);
#ifdef __APPLE__
        append_menu_item(fileMenu, wxID_ANY, _L("Re&load from Disk") + dots + "\tCtrl+Shift+R",
            _L("Reload the platter from disk"), [this](wxCommandEvent&) { m_plater->reload_gcode_from_disk(); },
            "", nullptr, [this]() { return !m_plater->get_last_loaded_gcode().empty(); }, this);
#else
        append_menu_item(fileMenu, wxID_ANY, _L("Re&load from Disk") + sep + "F5",
            _L("Reload the plater from disk"), [this](wxCommandEvent&) { m_plater->reload_gcode_from_disk(); },
            "", nullptr, [this]() { return !m_plater->get_last_loaded_gcode().empty(); }, this);
#endif // __APPLE__
        fileMenu->AppendSeparator();
        append_menu_item(fileMenu, wxID_ANY, _L("Export &Toolpaths as OBJ") + dots, _L("Export toolpaths as OBJ"),
            [this](wxCommandEvent&) { if (m_plater != nullptr) m_plater->export_toolpaths_to_obj(); }, "export_plater", nullptr,
            [this]() {return can_export_toolpaths(); }, this);
        append_menu_item(fileMenu, wxID_ANY, wxString::Format(_L("O&pen %s"), SLIC3R_APP_NAME) + dots, wxString::Format(_L("Open %s"), SLIC3R_APP_NAME),
            [](wxCommandEvent&) { start_new_slicer(); }, "", nullptr,
            []() {return true; }, this);
        fileMenu->AppendSeparator();
        append_menu_item(fileMenu, wxID_EXIT, _L("&Quit"), wxString::Format(_L("Quit %s"), SLIC3R_APP_NAME),
            [this](wxCommandEvent&) { Close(false); });
    }

    // View menu
    wxMenu* viewMenu = nullptr;
    if (m_plater != nullptr) {
        viewMenu = new wxMenu();
        add_common_view_menu_items(viewMenu, this, std::bind(&MainFrame::can_change_view, this));
    }

    // helpmenu
    auto helpMenu = generate_help_menu();

    m_menubar = new wxMenuBar();
    m_menubar->Append(fileMenu, _L("&File"));
    if (viewMenu != nullptr) m_menubar->Append(viewMenu, _L("&View"));
    // Add additional menus from C++
    wxGetApp().add_config_menu(m_menubar);
    m_menubar->Append(helpMenu, _L("&Help"));
    SetMenuBar(m_menubar);

#ifdef __APPLE__
    // This fixes a bug on Mac OS where the quit command doesn't emit window close events
    // wx bug: https://trac.wxwidgets.org/ticket/18328
    wxMenu* apple_menu = m_menubar->OSXGetAppleMenu();
    if (apple_menu != nullptr) {
        apple_menu->Bind(wxEVT_MENU, [this](wxCommandEvent&) {
            Close();
            }, wxID_EXIT);
    }
#endif // __APPLE__
}

void MainFrame::update_menubar()
{
    if (wxGetApp().is_gcode_viewer())
        return;

    const bool is_fff = plater()->printer_technology() == ptFFF;

    m_changeable_menu_items[miExport]       ->SetItemLabel((is_fff ? _L("Export &G-code")         : _L("E&xport"))        + dots    + "\tCtrl+G");
    m_changeable_menu_items[miSend]         ->SetItemLabel((is_fff ? _L("S&end G-code")           : _L("S&end to print")) + dots    + "\tCtrl+Shift+G");

    m_changeable_menu_items[miMaterialTab]  ->SetItemLabel((is_fff ? _L("&Filament Settings Tab") : _L("Mate&rial Settings Tab"))   + "\tCtrl+5");
    m_changeable_menu_items[miMaterialTab]  ->SetBitmap(create_menu_bitmap(is_fff ? "spool"   : "resin"));

    m_changeable_menu_items[miPrinterTab]   ->SetBitmap(create_menu_bitmap(is_fff ? "printer" : "sla_printer"));

    if (m_calibration_menu) {
        int id = m_menubar->FindMenu(m_calibration_menu->GetTitle());
        if (id != wxNOT_FOUND) {
            m_menubar->EnableTop(id, is_fff);
        }
    }
}

#if 0
// To perform the "Quck Slice", "Quick Slice and Save As", "Repeat last Quick Slice" and "Slice to SVG".
void MainFrame::quick_slice(const int qs)
{
//     my $progress_dialog;
    wxString input_file;
//  eval
//     {
    // validate configuration
    auto config = wxGetApp().preset_bundle->full_config();
    auto valid = config.validate();
    if (! valid.empty()) {
        show_error(this, valid);
        return;
    }

    // select input file
    if (!(qs & qsReslice)) {
        wxFileDialog dlg(this, _L("Choose a file to slice (STL/OBJ/AMF/3MF/PRUSA):"),
            wxGetApp().app_config->get_last_dir(), "",
            file_wildcards(FT_MODEL), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() != wxID_OK)
            return;
        input_file = dlg.GetPath();
        if (!(qs & qsExportSVG))
            m_qs_last_input_file = input_file;
    }
    else {
        if (m_qs_last_input_file.IsEmpty()) {
            //wxMessageDialog dlg(this, _L("No previously sliced file."),
            MessageDialog dlg(this, _L("No previously sliced file."),
                _L("Error"), wxICON_ERROR | wxOK);
            dlg.ShowModal();
            return;
        }
        if (std::ifstream(m_qs_last_input_file.ToUTF8().data())) {
            //wxMessageDialog dlg(this, _L("Previously sliced file (")+m_qs_last_input_file+_L(") not found."),
            MessageDialog dlg(this, _L("Previously sliced file (")+m_qs_last_input_file+_L(") not found."),
                _L("File Not Found"), wxICON_ERROR | wxOK);
            dlg.ShowModal();
            return;
        }
        input_file = m_qs_last_input_file;
    }
    auto input_file_basename = get_base_name(input_file);
    wxGetApp().app_config->update_skein_dir(get_dir_name(input_file));

    auto bed_shape = Slic3r::Polygon::new_scale(config.option<ConfigOptionPoints>("bed_shape")->values);
//     auto print_center = Slic3r::Pointf->new_unscale(bed_shape.bounding_box().center());
// 
//     auto sprint = new Slic3r::Print::Simple(
//         print_center = > print_center,
//         status_cb = > [](int percent, const wxString& msg) {
//         m_progress_dialog->Update(percent, msg+"…");
//     });

    // keep model around
    auto model = Slic3r::Model::read_from_file(input_file.ToUTF8().data());

//     sprint->apply_config(config);
//     sprint->set_model(model);

    // Copy the names of active presets into the placeholder parser.
//     wxGetApp().preset_bundle->export_selections(sprint->placeholder_parser);

    // select output file
    wxString output_file;
    if (qs & qsReslice) {
        if (!m_qs_last_output_file.IsEmpty())
            output_file = m_qs_last_output_file;
    } 
    else if (qs & qsSaveAs) {
        // The following line may die if the output_filename_format template substitution fails.
        wxFileDialog dlg(this, from_u8((boost::format(_utf8(L("Save %s file as:"))) % ((qs & qsExportSVG) ? _L("SVG") : _L("G-code"))).str()),
            wxGetApp().app_config->get_last_output_dir(get_dir_name(output_file)), get_base_name(input_file), 
            qs & qsExportSVG ? file_wildcards(FT_SVG) : file_wildcards(FT_GCODE),
            wxFD_SAVE | (wxGetApp().app_config->get_show_overwrite_dialog() ? wxFD_OVERWRITE_PROMPT : 0));
        if (dlg.ShowModal() != wxID_OK)
            return;
        output_file = dlg.GetPath();
        if (!(qs & qsExportSVG))
            m_qs_last_output_file = output_file;
        wxGetApp().app_config->update_last_output_dir(get_dir_name(output_file));
    } 
    else if (qs & qsExportPNG) {
        wxFileDialog dlg(this, _L("Save zip file as:"),
            wxGetApp().app_config->get_last_output_dir(get_dir_name(output_file)),
            get_base_name(output_file), "*.sl1", wxFD_SAVE | (wxGetApp().app_config->get_show_overwrite_dialog() ? wxFD_OVERWRITE_PROMPT : 0));
        if (dlg.ShowModal() != wxID_OK)
            return;
        output_file = dlg.GetPath();
        }

    // show processbar dialog
    m_progress_dialog = new wxProgressDialog(_L("Slicing") + dots,
    // TRN "Processing input_file_basename"
                                             from_u8((boost::format(_utf8(L("Processing %s"))) % (input_file_basename + dots)).str()),
        100, nullptr, wxPD_AUTO_HIDE);
    m_progress_dialog->Pulse();
    {
//         my @warnings = ();
//         local $SIG{ __WARN__ } = sub{ push @warnings, $_[0] };

//         sprint->output_file(output_file);
//         if (export_svg) {
//             sprint->export_svg();
//         }
//         else if(export_png) {
//             sprint->export_png();
//         }
//         else {
//             sprint->export_gcode();
//         }
//         sprint->status_cb(undef);
//         Slic3r::GUI::warning_catcher($self)->($_) for @warnings;
    }
    m_progress_dialog->Destroy();
    m_progress_dialog = nullptr;

    auto message = format(_L("%1% was successfully sliced."), input_file_basename);
//     wxTheApp->notify(message);
    //wxMessageDialog(this, message, _L("Slicing Done!"), wxOK | wxICON_INFORMATION).ShowModal();
    MessageDialog(this, message, _L("Slicing Done!"), wxOK | wxICON_INFORMATION).ShowModal();
//     };
//     Slic3r::GUI::catch_error(this, []() { if (m_progress_dialog) m_progress_dialog->Destroy(); });
}
#endif

void MainFrame::reslice_now()
{
    if (m_plater)
        m_plater->reslice();
}

void MainFrame::repair_stl()
{
    wxString input_file;
    {
        wxFileDialog dlg(this, _L("Select the STL file to repair:"),
            wxGetApp().app_config->get_last_dir(), "",
            file_wildcards(FT_STL), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() != wxID_OK)
            return;
        input_file = dlg.GetPath();
        }

    wxString output_file = input_file;
    {
        wxFileDialog dlg( this, L("Save OBJ file (less prone to coordinate errors than STL) as:"),
                                        get_dir_name(output_file), get_base_name(output_file, ".obj"),
                                        file_wildcards(FT_OBJ), wxFD_SAVE | (wxGetApp().app_config->get_show_overwrite_dialog() ? wxFD_OVERWRITE_PROMPT : 0));
        if (dlg.ShowModal() != wxID_OK)
            return;
        output_file = dlg.GetPath();
        }

    Slic3r::TriangleMesh tmesh;
    tmesh.ReadSTLFile(input_file.ToUTF8().data());
    tmesh.WriteOBJFile(output_file.ToUTF8().data());
    Slic3r::GUI::show_info(this, L("Your file was repaired."), L("Repair"));
}

void MainFrame::export_config(bool to_prusa)
{
    // Generate a cummulative configuration for the selected print, filaments and printer.
    auto config = wxGetApp().preset_bundle->full_config();
    // Validate the cummulative configuration.
    auto valid = config.validate();
    if (! valid.empty()) {
        show_error(this, valid);
        return;
    }
    // Ask user for the file name for the config file.
    wxFileDialog dlg(this, _L("Save configuration as:"),
        !m_last_config.IsEmpty() ? get_dir_name(m_last_config) : wxGetApp().app_config->get_last_dir(),
        !m_last_config.IsEmpty() ? get_base_name(m_last_config) : "config.ini",
        file_wildcards(FT_INI), wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    wxString file;
    if (dlg.ShowModal() == wxID_OK)
        file = dlg.GetPath();
    if (!file.IsEmpty()) {
        wxGetApp().app_config->update_config_dir(get_dir_name(file));
        m_last_config = file;
        config.save(file.ToUTF8().data(), to_prusa);
    }
}

// Load a config file containing a Print, Filament & Printer preset.
void MainFrame::load_config_file(bool from_prusa)
{
    if (!wxGetApp().check_and_save_current_preset_changes(_L("Loading of a configuration file"), "", false))
        return;
    wxFileDialog dlg(this, _L("Select configuration to load:"),
            !m_last_config.IsEmpty() ? get_dir_name(m_last_config) : wxGetApp().app_config->get_last_dir(),
            "config.ini", "INI files (*.ini, *.gcode)|*.ini;*.INI;*.gcode;*.g", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
	wxString file;
    if (dlg.ShowModal() == wxID_OK)
        file = dlg.GetPath();
    if (! file.IsEmpty() && this->load_config_file(file.ToUTF8().data(), from_prusa)) {
        wxGetApp().app_config->update_config_dir(get_dir_name(file));
        m_last_config = file;
    }
}

// Load a config file containing a Print, Filament & Printer preset from command line.
bool MainFrame::load_config_file(const std::string &path, bool from_prusa)
{
    try {
        ConfigSubstitutions config_substitutions = wxGetApp().preset_bundle->load_config_file(path, ForwardCompatibilitySubstitutionRule::Enable, from_prusa);
        if (!config_substitutions.empty())
            show_substitutions_info(config_substitutions, path);
    } catch (const std::exception &ex) {
        show_error(this, ex.what());
        return false;
    }
    wxGetApp().load_current_presets();
    return true;
}

void MainFrame::export_configbundle(bool export_physical_printers /*= false*/)
{
    if (!wxGetApp().check_and_save_current_preset_changes(_L("Exporting configuration bundle"),
                                                          _L("Some presets are modified and the unsaved changes will not be exported into configuration bundle."), false, true))
        return;
    // validate current configuration in case it's dirty
    auto err = wxGetApp().preset_bundle->full_config().validate();
    if (! err.empty()) {
        show_error(this, err);
        return;
    }
    // Ask user for a file name.
    wxFileDialog dlg(this, _L("Save presets bundle as:"),
        !m_last_config.IsEmpty() ? get_dir_name(m_last_config) : wxGetApp().app_config->get_last_dir(),
        SLIC3R_APP_KEY "_config_bundle.ini",
        file_wildcards(FT_INI), wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    wxString file;
    if (dlg.ShowModal() == wxID_OK)
        file = dlg.GetPath();
    if (!file.IsEmpty()) {
        // Export the config bundle.
        wxGetApp().app_config->update_config_dir(get_dir_name(file));
        try {
            wxGetApp().preset_bundle->export_configbundle(file.ToUTF8().data(), false, export_physical_printers);
        } catch (const std::exception& ex) {
			show_error(this, ex.what());
        }
    }
}

// Loading a config bundle with an external file name used to be used
// to auto - install a config bundle on a fresh user account,
// but that behavior was not documented and likely buggy.
void MainFrame::load_configbundle(wxString file/* = wxEmptyString*/, bool from_prusa/* = false*/)
{
    if (!wxGetApp().check_and_save_current_preset_changes(_L("Loading of a configuration bundle"), "", false))
        return;
    if (file.IsEmpty()) {
        wxFileDialog dlg(this, _L("Select configuration to load:"),
            !m_last_config.IsEmpty() ? get_dir_name(m_last_config) : wxGetApp().app_config->get_last_dir(),
            "config.ini", file_wildcards(FT_INI), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() != wxID_OK)
            return;
        file = dlg.GetPath();
		}

    wxGetApp().app_config->update_config_dir(get_dir_name(file));

    size_t presets_imported = 0;
    PresetsConfigSubstitutions config_substitutions;
    try {
        PresetBundle::LoadConfigBundleAttributes lcba{ PresetBundle::LoadConfigBundleAttribute::SaveImported };
        // Report all substitutions.
        std::tie(config_substitutions, presets_imported) = wxGetApp().preset_bundle->load_configbundle(
            file.ToUTF8().data(), 
            (from_prusa ? lcba | PresetBundle::LoadConfigBundleAttribute::ConvertFromPrusa : lcba),
            ForwardCompatibilitySubstitutionRule::Enable);
    } catch (const std::exception &ex) {
        show_error(this, ex.what());
        return;
    }

    if (! config_substitutions.empty())
        show_substitutions_info(config_substitutions);

    // Load the currently selected preset into the GUI, update the preset selection box.
	wxGetApp().load_current_presets();

    const auto message = wxString::Format(_L("%d presets successfully imported."), presets_imported);
    Slic3r::GUI::show_info(this, message, wxString("Info"));
}

// Load a provied DynamicConfig into the Print / Filament / Printer tabs, thus modifying the active preset.
// Also update the plater with the new presets.
void MainFrame::load_config(const DynamicPrintConfig& config)
{
	PrinterTechnology printer_technology = wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology();
	const auto       *opt_printer_technology = config.option<ConfigOptionEnum<PrinterTechnology>>("printer_technology");
	if (opt_printer_technology != nullptr && opt_printer_technology->value != printer_technology) {
		printer_technology = opt_printer_technology->value;
		this->plater()->set_printer_technology(printer_technology);
	}
#if 0
    for (auto tab : wxGetApp().tabs_list)
		if (tab->supports_printer_technology(printer_technology)) {
			if (tab->type() == Slic3r::Preset::TYPE_PRINTER)
				static_cast<TabPrinter*>(tab)->update_pages();
        tab->load_config(config);
		}
    if (m_plater)
        m_plater->on_config_change(config);
#else
	// Load the currently selected preset into the GUI, update the preset selection box.
    //FIXME this is not quite safe for multi-extruder printers,
    // as the number of extruders is not adjusted for the vector values.
    // (see PresetBundle::update_multi_material_filament_presets())
    // Better to call PresetBundle::load_config() instead?
    for (auto tab : wxGetApp().tabs_list)
        if (tab->supports_printer_technology(printer_technology)) {
            // Only apply keys, which are present in the tab's config. Ignore the other keys.
			for (const std::string &opt_key : tab->get_config()->diff(config))
				// Ignore print_settings_id, printer_settings_id, filament_settings_id etc.
				if (! boost::algorithm::ends_with(opt_key, "_settings_id"))
					tab->get_config()->option(opt_key)->set(config.option(opt_key));
        }
    
    wxGetApp().load_current_presets();
#endif
}

void MainFrame::select_tab(Tab* tab)
{
    if (!tab)
        return;
    ETabType tab_type = ETabType::LastSettings;
    switch (tab->type()) {
    case Preset::Type::TYPE_FFF_FILAMENT:
    case Preset::Type::TYPE_SLA_MATERIAL:
        tab_type = ETabType::FilamentSettings;
        break;
    case Preset::Type::TYPE_FFF_PRINT:
    case Preset::Type::TYPE_SLA_PRINT:
        tab_type = ETabType::PrintSettings;
        break;
    case Preset::Type::TYPE_PRINTER:
        tab_type = ETabType::PrinterSettings;
        break;
    }
    select_tab(tab_type);

}

MainFrame::ETabType MainFrame::next_preview_tab()
{
    if (m_layout == ESettingsLayout::Tabs) {
        MainFrame::ETabType current_tab = selected_tab();
        MainFrame::ETabType next_tab = MainFrame::ETabType(uint8_t(current_tab) + 1);
        if (next_tab == MainFrame::ETabType::LastPlater) next_tab = MainFrame::ETabType::Plater3D;
        select_tab(next_tab, true);
        return next_tab;
    } else {
        if (m_plater->is_view3D_shown()) {
            m_plater->select_view_3D("Preview");
            return /*m_plater->can_display_gcode()*/ MainFrame::ETabType::PlaterGcode;
        } else {
            m_plater->select_view_3D("3D");
            return MainFrame::ETabType::Plater3D;
        }
    }
}

MainFrame::ETabType MainFrame::selected_tab() const
{
    if (m_layout == ESettingsLayout::Old) {
        if (m_tabpanel->GetSelection() == 0) {
            if (m_plater->is_view3D_shown()) {
                return ETabType::Plater3D;
            } else {
                return ETabType::PlaterGcode;
            }
        } else {
            return ETabType((uint8_t)ETabType::PrintSettings + m_tabpanel->GetSelection() - 1);
        }
    } else if (m_layout == ESettingsLayout::Tabs) {
#ifdef _USE_CUSTOM_NOTEBOOK
        int bt_idx_sel = 0;
        if (wxGetApp().tabs_as_menu()) {
            bt_idx_sel = m_tabpanel->GetSelection();
            //FIXME: get the menu button instead of the tab that is only likethe "old"
        } else {
            Notebook* notebook = static_cast<Notebook*>(m_tabpanel);
            //get the selected button, not the selected panel
            bt_idx_sel = notebook->GetBtSelection();
        }
        if (bt_idx_sel < 3) {
            return ETabType((uint8_t)ETabType::Plater3D + bt_idx_sel);
        } else {
            return ETabType((uint8_t)ETabType::PrintSettings + bt_idx_sel - 3);
        }
#else
        if (m_tabpanel->GetSelection() < 3) {
            return ETabType((uint8_t)ETabType::Plater3D + m_tabpanel->GetSelection());
        } else {
            return ETabType((uint8_t)ETabType::PrintSettings + m_tabpanel->GetSelection() - 3);
        }
#endif
    } else if (m_layout == ESettingsLayout::Hidden) {
        if (!m_main_sizer->IsShown(m_tabpanel)) {
            if (m_plater->is_view3D_shown()) {
                return ETabType::Plater3D;
            } else {
                return ETabType::PlaterGcode;
            }
        } else {
            return ETabType((uint8_t)ETabType::PrintSettings + m_tabpanel->GetSelection() - 1);
        }
    } else if (m_layout == ESettingsLayout::Dlg) {
        if (!m_settings_dialog.GetSizer()->IsShown(m_tabpanel)) {
            if (m_plater->is_view3D_shown()) {
                return ETabType::Plater3D;
            } else {
                return ETabType::PlaterGcode;
            }
        } else {
            return ETabType::Plater3D;
        }
    }
    return ETabType::Plater3D;
}

void MainFrame::select_tab(ETabType tab /* = Any*/, bool keep_tab_type)
{
    bool tabpanel_was_hidden = false;

    //failsafe
    if (!wxGetApp().is_editor()) {
        assert(tab == ETabType::PlaterGcode);
        tab = ETabType::PlaterGcode;
    }

    // Controls on page are created on active page of active tab now.
    // We should select/activate tab before its showing to avoid an UI-flickering
    auto select = [this, tab](bool was_hidden) {
        // when tab == -1, it means we should show the last selected tab
        size_t new_selection = 0;
        if (tab <= ETabType::LastPlater) {
            //select plater
            new_selection = (uint8_t)tab;
            if (tab == ETabType::LastPlater)
                new_selection = m_last_selected_plater_tab > 2 ? 0 : m_last_selected_plater_tab;
            if (m_layout != ESettingsLayout::Tabs)
                new_selection = 0;

        } else if (tab <= ETabType::LastSettings) {
            //select setting
            new_selection = (uint8_t)tab - (uint8_t)ETabType::PrintSettings;
            if (tab == ETabType::LastSettings) 
                new_selection = m_last_selected_setting_tab > 2 ? 0 : m_last_selected_setting_tab;
            //push to the correct position
            if (m_layout == ESettingsLayout::Tabs)
                new_selection = new_selection + 3;
            else if (m_layout != ESettingsLayout::Dlg)
                new_selection = new_selection + 1;
        }

#ifndef _USE_CUSTOM_NOTEBOOK
        if (m_tabpanel->GetPageCount() == 0) return; // failsafe
        if (m_tabpanel->GetSelection() != (int)new_selection)
            m_tabpanel->SetSelection(new_selection);
#else
        if (wxGetApp().tabs_as_menu()) {
            int page_idx = new_selection;
            if (m_layout == ESettingsLayout::Tabs) {
                if (page_idx < 3)
                    page_idx = 0;
                else
                    page_idx -= 2;
            }
            if (Tab* cur_tab = dynamic_cast<Tab*>(m_tabpanel->GetPage(page_idx)))
                update_marker_for_tabs_menu((m_layout != ESettingsLayout::Dlg ? m_menubar : m_settings_dialog.menubar()), cur_tab->title(), new_selection, m_layout);
            else if (tab == ETabType::LastPlater && m_layout == ESettingsLayout::Old)
                m_plater->get_current_canvas3D()->render();
            else if (m_layout != ESettingsLayout::Dlg)
                update_marker_for_tabs_menu( m_menubar, "", new_selection, m_layout);
            int last_sel = m_tabpanel->GetSelection();
            m_tabpanel->SetSelection(page_idx);
            if (m_layout == ESettingsLayout::Tabs) { //as it's not done by the button callback, as it call this, it has to
                if (last_sel > 0 && page_idx < 3 && (page_idx == m_last_selected_plater_tab || m_last_selected_plater_tab > 2)) {
                    // hack to set a correct refresh of the app (can't find anythign else that worked) when going from settings to last plater
                    if (tab == ETabType::Plater3D || (tab == ETabType::LastPlater && m_last_selected_plater_tab == 0)) {
                        this->m_plater->select_view_3D("Preview");
                    } else {
                        this->m_plater->select_view_3D("3D");
                    }
                }
                if (tab == ETabType::Plater3D || (tab == ETabType::LastPlater && m_last_selected_plater_tab == 0)) {
                    this->m_plater->select_view_3D("3D");
                } else if (tab == ETabType::PlaterPreview || (tab == ETabType::LastPlater && m_last_selected_plater_tab == 1)) {
                    if (this->m_plater->get_force_preview() != Preview::ForceState::ForceExtrusions) {
                        this->m_plater->set_force_preview(Preview::ForceState::ForceExtrusions);
                        this->m_plater->select_view_3D("Preview");
                        this->m_plater->refresh_print();
                    } else
                        this->m_plater->select_view_3D("Preview");
                } else if (tab == ETabType::PlaterGcode || (tab == ETabType::LastPlater && m_last_selected_plater_tab == 2)) {
                    if (this->m_plater->get_force_preview() != Preview::ForceState::ForceGcode) {
                        this->m_plater->set_force_preview(Preview::ForceState::ForceGcode);
                        this->m_plater->select_view_3D("Preview");
                        this->m_plater->refresh_print();
                    } else
                        this->m_plater->select_view_3D("Preview");
                }
            }
        } else {
            Notebook* notebook = static_cast<Notebook*>(m_tabpanel);
            if (notebook->GetPageCount() == 0) return; // failsafe
            if (notebook->GetBtSelection() != (int)new_selection)
                notebook->SetBtSelection(new_selection);
        }
#endif
        if (tab == ETabType::LastPlater && m_layout == ESettingsLayout::Old)
            m_plater->canvas3D()->render();
        else if (was_hidden) {
            Tab* cur_tab = dynamic_cast<Tab*>(m_tabpanel->GetPage(new_selection));
            if (cur_tab)
                cur_tab->OnActivate();
        }
    };

    if (m_layout != ESettingsLayout::Tabs) {
        if (tab == ETabType::Plater3D || (tab == ETabType::LastPlater && m_last_selected_plater_tab == 0)) {
            m_plater->select_view_3D("3D");
        } else if (tab == ETabType::PlaterPreview || (tab == ETabType::LastPlater && m_last_selected_plater_tab == 1)) {
            m_plater->select_view_3D("Preview");
        } else if (tab == ETabType::PlaterGcode || (tab == ETabType::LastPlater && m_last_selected_plater_tab == 2)) {
            m_plater->select_view_3D("Preview");
        }
    }

    if (m_layout == ESettingsLayout::Dlg) {
        if (keep_tab_type)
            return;
        if (tab <= ETabType::LastPlater) {
            if (m_settings_dialog.IsShown())
                this->SetFocus();
            // plater should be focused for correct navigation inside search window
            if (m_plater->canvas3D()->is_search_pressed())
                m_plater->SetFocus();
            return;
        }
        // Show/Activate Settings Dialog
#ifdef __WXOSX__ // Don't call SetFont under OSX to avoid name cutting in ObjectList
        if (m_settings_dialog.IsShown())
            m_settings_dialog.Hide();
        else
            tabpanel_was_hidden = true;
            
        select(tabpanel_was_hidden);
        m_tabpanel->Show();
        m_settings_dialog.Show();
#else
        if (m_settings_dialog.IsShown()) {
            select(false);
            m_settings_dialog.SetFocus();
        }
        else {
            tabpanel_was_hidden = true;
            select(tabpanel_was_hidden);
            m_tabpanel->Show();
            m_settings_dialog.Show();
        }
#endif
        if (m_settings_dialog.IsIconized())
            m_settings_dialog.Iconize(false);
    }
    else if (m_layout == ESettingsLayout::Hidden) {
        if (keep_tab_type && m_tabpanel->GetSelection()>0)
            return;
        m_main_sizer->Show(m_plater, tab <= ETabType::LastPlater);
        tabpanel_was_hidden = !m_main_sizer->IsShown(m_tabpanel);
        select(tabpanel_was_hidden);
        m_main_sizer->Show(m_tabpanel, tab > ETabType::LastPlater);

        // plater should be focused for correct navigation inside search window
        if (tab <= ETabType::LastPlater /*&& m_plater->canvas3D()->is_search_pressed()*/)
            m_plater->SetFocus();
        Layout();
    }
    else if (m_layout == ESettingsLayout::Old) {
        if (keep_tab_type && m_tabpanel->GetSelection() > 0)
            return;
        else
            select(false);
    }
#ifdef _USE_CUSTOM_NOTEBOOK
    else if (m_layout == ESettingsLayout::Tabs && !wxGetApp().tabs_as_menu()) {
#else
    else if (m_layout == ESettingsLayout::Tabs) {
#endif
#ifdef _USE_CUSTOM_NOTEBOOK
        Notebook* notebook = static_cast<Notebook*>(m_tabpanel);
        //get the selected button, not the selected panel
        int bt_idx_sel = notebook->GetBtSelection();
        if (keep_tab_type && ((bt_idx_sel >= 3 && tab <= ETabType::LastPlater) || (bt_idx_sel < 3 && tab > ETabType::LastPlater))) {
#else
        if (keep_tab_type && ( (m_tabpanel->GetSelection() >=3 && tab <= ETabType::LastPlater) || (m_tabpanel->GetSelection() < 3 && tab > ETabType::LastPlater))) {
#endif
            return;
        } else {
            select(false);
#ifndef _USE_CUSTOM_NOTEBOOK
            //force update if change from plater to plater (as it doesn't change the real tab, have to tell him to really update
            if (m_tabpanel->GetSelection() != int(tab) && m_tabpanel->GetSelection() < int(ETabType::LastPlater)) {
                wxBookCtrlEvent evt = wxBookCtrlEvent(wxEVT_BOOKCTRL_PAGE_CHANGED);
                evt.SetOldSelection(m_tabpanel->GetSelection());
                wxPostEvent(m_tabpanel->GetEventHandler(), evt);
            }
#endif
        }
    }
    else
        select(false);

    // When we run application in ESettingsLayout::Hidden or ESettingsLayout::Dlg mode, tabpanel is hidden from the very beginning
    // and as a result Tab::update_changed_tree_ui() function couldn't update m_is_nonsys_values values,
    // which are used for update TreeCtrl and "revert_buttons".
    // So, force the call of this function for Tabs, if tab panel was hidden
    if (tabpanel_was_hidden)
        for (auto cur_tab : wxGetApp().tabs_list)
            cur_tab->update_changed_tree_ui();

    //// when tab == -1, it means we should show the last selected tab
    //size_t new_selection = tab == (size_t)(-1) ? m_last_selected_tab : (m_layout == ESettingsLayout::Dlg && tab != 0) ? tab - 1 : tab;
    //if (m_tabpanel->GetSelection() != new_selection)
    //    m_tabpanel->SetSelection(new_selection);
    //if (tabpanel_was_hidden)
    //    static_cast<Tab*>(m_tabpanel->GetPage(new_selection))->OnActivate();
}

// Set a camera direction, zoom to all objects.
void MainFrame::select_view(const std::string& direction)
{
     if (m_plater)
         m_plater->select_view(direction);
}

// #ys_FIXME_to_delete
void MainFrame::on_presets_changed(SimpleEvent &event)
{
    auto *tab = dynamic_cast<Tab*>(event.GetEventObject());
    wxASSERT(tab != nullptr);
    if (tab == nullptr) {
        return;
    }

    // Update preset combo boxes(Print settings, Filament, Material, Printer) from their respective tabs.
    auto presets = tab->get_presets();
    if (m_plater != nullptr && presets != nullptr) {

        // FIXME: The preset type really should be a property of Tab instead
        Slic3r::Preset::Type preset_type = tab->type();
        if (preset_type == Slic3r::Preset::TYPE_INVALID) {
            wxASSERT(false);
            return;
        }

        m_plater->on_config_change(*tab->get_config());
        m_plater->sidebar().update_presets(preset_type);
    }
}

// #ys_FIXME_to_delete
void MainFrame::on_value_changed(wxCommandEvent& event)
{
    auto *tab = dynamic_cast<Tab*>(event.GetEventObject());
    wxASSERT(tab != nullptr);
    if (tab == nullptr)
        return;

    auto opt_key = event.GetString();
    if (m_plater) {
        m_plater->on_config_change(*tab->get_config()); // propagate config change events to the plater
        if (opt_key == "extruders_count") {
            auto value = event.GetInt();
            //to update filaments gui
            m_plater->on_extruders_change(value);
        }
    }
}

void MainFrame::on_config_changed(DynamicPrintConfig* config) const
{
    if (m_plater)
        m_plater->on_config_change(*config); // propagate config change events to the plater
}

void MainFrame::add_to_recent_projects(const wxString& filename)
{
    if (wxFileExists(filename))
    {
        m_recent_projects.AddFileToHistory(filename);
        std::vector<std::string> recent_projects;
        size_t count = m_recent_projects.GetCount();
        for (size_t i = 0; i < count; ++i)
        {
            recent_projects.push_back(into_u8(m_recent_projects.GetHistoryFile(i)));
        }
        wxGetApp().app_config->set_recent_projects(recent_projects);
        wxGetApp().app_config->save();
    }
}

void MainFrame::technology_changed()
{
    // upadte DiffDlg
    diff_dialog.update_presets();

    // update menu titles
    PrinterTechnology pt = plater()->printer_technology();
    if (int id = m_menubar->FindMenu(pt == ptFFF ? _L("Material Settings") : _L("Filament Settings")); id != wxNOT_FOUND)
        m_menubar->SetMenuLabel(id , pt == ptSLA ? _L("Material Settings") : _L("Filament Settings"));

    //if (wxGetApp().tab_panel()->GetSelection() != wxGetApp().tab_panel()->GetPageCount() - 1)
    //    wxGetApp().tab_panel()->SetSelection(wxGetApp().tab_panel()->GetPageCount() - 1);

}

//
// Called after the Preferences dialog is closed and the program settings are saved.
// Update the UI based on the current preferences.
void MainFrame::update_ui_from_settings()
{
//    const bool bp_on = wxGetApp().app_config->get("background_processing") == "1";
//     m_menu_item_reslice_now->Enable(!bp_on);
//    m_plater->sidebar().show_reslice(!bp_on);
//    m_plater->sidebar().show_export(bp_on);
//    m_plater->sidebar().Layout();

    if (m_plater)
        m_plater->update_ui_from_settings();
    for (auto tab: wxGetApp().tabs_list)
        tab->update_ui_from_settings();
}

std::string MainFrame::get_base_name(const wxString &full_name, const char *extension) const 
{
    boost::filesystem::path filename = boost::filesystem::path(full_name.wx_str()).filename();
    if (extension != nullptr)
		filename = filename.replace_extension(extension);
    return filename.string();
}

std::string MainFrame::get_dir_name(const wxString &full_name) const 
{
    return boost::filesystem::path(full_name.wx_str()).parent_path().string();
}


// ----------------------------------------------------------------------------
// SettingsDialog
// ----------------------------------------------------------------------------

SettingsDialog::SettingsDialog(MainFrame* mainframe)
:DPIFrame(NULL, wxID_ANY, wxString(SLIC3R_APP_NAME) + " - " + _L("Settings"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_FRAME_STYLE, "settings_dialog"),
//: DPIDialog(mainframe, wxID_ANY, wxString(SLIC3R_APP_NAME) + " - " + _L("Settings"), wxDefaultPosition, wxDefaultSize,
//        wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER | wxMINIMIZE_BOX | wxMAXIMIZE_BOX, "settings_dialog"),
    m_main_frame(mainframe)
{
    if (wxGetApp().is_gcode_viewer())
        return;

#if defined(__WXMSW__)
    // ys_FIXME! temporary workaround for correct font scaling
    // Because of from wxWidgets 3.1.3 auto rescaling is implemented for the Fonts,
    // From the very beginning set dialog font to the wxSYS_DEFAULT_GUI_FONT
    this->SetFont(wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT));
#else
    this->SetFont(wxGetApp().normal_font());
    this->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
#endif // __WXMSW__

    // Load the icon either from the exe, or from the ico file.
#if _WIN32
    {
        TCHAR szExeFileName[MAX_PATH];
        GetModuleFileName(nullptr, szExeFileName, MAX_PATH);
        SetIcon(wxIcon(szExeFileName, wxBITMAP_TYPE_ICO));
    }
#else
    SetIcon(wxIcon(var(SLIC3R_APP_KEY "_128px.png"), wxBITMAP_TYPE_PNG));
#endif // _WIN32

    this->Bind(wxEVT_SHOW, [this](wxShowEvent& evt) {

        auto key_up_handker = [this](wxKeyEvent& evt) {
            if ((evt.GetModifiers() & wxMOD_CONTROL) != 0) {
                switch (evt.GetKeyCode()) {
                case '1': { m_main_frame->select_tab(MainFrame::ETabType::Plater3D); break; }
                case '2': { m_main_frame->select_tab(MainFrame::ETabType::PlaterPreview); break; }
                case '3': { m_main_frame->select_tab(MainFrame::ETabType::PlaterGcode); break; }
                case '4': { m_main_frame->select_tab(MainFrame::ETabType::PrintSettings); break; }
                case '5': { m_main_frame->select_tab(MainFrame::ETabType::FilamentSettings); break; }
                case '6': { m_main_frame->select_tab(MainFrame::ETabType::PrinterSettings); break; }
#ifdef __APPLE__
                case 'f':
#else /* __APPLE__ */
                case WXK_CONTROL_F:
#endif /* __APPLE__ */
                case 'F': { m_main_frame->plater()->search(false); break; }
                default:break;
                }
            }
        };

        if (evt.IsShown()) {
            if (m_tabpanel != nullptr)
                m_tabpanel->Bind(wxEVT_KEY_UP, key_up_handker);
        }
        else {
            if (m_tabpanel != nullptr)
                m_tabpanel->Unbind(wxEVT_KEY_UP, key_up_handker);
        }
        });

    //just hide the Frame on closing
    this->Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& evt) { this->Hide(); });

#ifdef _USE_CUSTOM_NOTEBOOK
    if (wxGetApp().tabs_as_menu()) {
        // menubar
        m_menubar = new wxMenuBar();
        add_tabs_as_menu(m_menubar, mainframe, this);
        this->SetMenuBar(m_menubar);
    }
#endif

    // initialize layout
    auto sizer = new wxBoxSizer(wxVERTICAL);
    sizer->SetSizeHints(this);
    SetSizer(sizer);
    Fit();

    const wxSize min_size = wxSize(85 * em_unit(), 50 * em_unit());
#ifdef __APPLE__
    // Using SetMinSize() on Mac messes up the window position in some cases
    // cf. https://groups.google.com/forum/#!topic/wx-users/yUKPBBfXWO0
    SetSize(min_size);
#else
    SetMinSize(min_size);
    SetSize(GetMinSize());
#endif
    Layout();
}

void SettingsDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    if (wxGetApp().is_gcode_viewer())
        return;

    const int& em = em_unit();
    const wxSize& size = wxSize(85 * em, 50 * em);

#ifdef _USE_CUSTOM_NOTEBOOK
    // update common mode sizer
    if (!wxGetApp().tabs_as_menu())
        dynamic_cast<Notebook*>(m_tabpanel)->Rescale();
#endif

    // update Tabs
    for (auto tab : wxGetApp().tabs_list)
        tab->msw_rescale();

    SetMinSize(size);
    Fit();
    Refresh();
}


} // namespace GUI
} // namespace Slic3r
