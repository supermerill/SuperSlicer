#ifndef slic3r_GUI_CreateMMUTiledCanvas_hpp_
#define slic3r_GUI_CreateMMUTiledCanvas_hpp_

#include <map>
#include <vector>
#include <regex>

#include "OptionsGroup.hpp"

#include "GUI_App.hpp"

#include <wx/combobox.h>
#include <wx/gbsizer.h>

namespace Slic3r {
namespace GUI {
    class CreateMMUTiledCanvas;

    class MyDynamicConfig : public DynamicConfig
    {
    public:
        ConfigDef config_def;
        const ConfigDef* def() const override { return &config_def; }
    };

    class BasicDrawPane : public wxPanel
    {

    public:
        wxSize previous_size;

        wxBitmap bmp;
        wxBitmap cache;
        CreateMMUTiledCanvas* parent;

        BasicDrawPane(wxWindow* parent, MyDynamicConfig* config);

        void paintEvent(wxPaintEvent& evt);
        void paintNow();

        void render(wxDC& dc);

        void loadImage(std::string filepath);
        void redrawImage();

        // some useful events
        /*
         void mouseMoved(wxMouseEvent& event);
         void mouseDown(wxMouseEvent& event);
         void mouseWheelMoved(wxMouseEvent& event);
         void mouseReleased(wxMouseEvent& event);
         void rightClick(wxMouseEvent& event);
         void mouseLeftWindow(wxMouseEvent& event);
         void keyPressed(wxKeyEvent& event);
         void keyReleased(wxKeyEvent& event);
         */

        DECLARE_EVENT_TABLE()
    };

    //color managment
    // 1: create a ColorEntry per pixel color, add them to sorting list & color list
    // 1b: create an entry per spool (if set), add them to color list
    // 2: count pixels
    // 3: if spool assoc is set, use the spool color as print color (skip if spool unset) and remove it from sorting list & color list
    // 4: get smallest amount of pixel color, set merged to nearest color and add to it my pixels (and if it has a spool, use the spool) and remove it from sorting list & color list
    // 5: repeat 5 as long as the color list

    struct ColorEntry;
    struct CmbColorAssoc {
        virtual bool is_auto() = 0;
        virtual ColorEntry* get_print_color() = 0;
        virtual wxWindow* get_widget() = 0;
        virtual wxSizer* get_sizer() = 0;
        virtual bool is_detached() = 0;
        virtual void detach() = 0;
        virtual void attach() = 0;
        virtual void set_color_index(int idx) = 0;
    };
    struct ColorEntry {
        wxColor real_color;
        int nb_pixels_real;
        int nb_pixels_sum;
        //wxColor merged_auto;
        ColorEntry* printing_color;
        CmbColorAssoc* widget_spool;
        ColorEntry(wxColor color) : real_color(color), nb_pixels_real(0), nb_pixels_sum(0), /*merged_auto(color), */printing_color(nullptr), widget_spool(nullptr){}
        virtual ~ColorEntry() {
            if (widget_spool && widget_spool->is_detached()) {
                //widget_spool->get_sizer()->Clear(true);
            }
        }
        void reset_merge() {
            nb_pixels_sum = 0;
        }
        void reset() {
            nb_pixels_real = 0;
            //merged_auto = real_color;
            reset_merge();
        }
        virtual wxColour get_printed_color(bool use_spool = true) const {
            if (printing_color && printing_color != this) // has a conversion
                if(printing_color->is_spool()) // don't use conversion to spool if forbidden
                    return printing_color->get_printed_color(use_spool);
            return real_color;
        }
        void refresh_spool() {
            if (!widget_spool->is_auto()) {
                printing_color = widget_spool->get_print_color();
            }
        }
        void add_pixel() {
            nb_pixels_real++;
            nb_pixels_sum++;
        }
        // bad, but easy to code
        virtual bool is_spool() { return false; }

    };

    struct ColorEntrySpool : public ColorEntry
    {
        wxColourPickerCtrl* widget;
        ColorEntrySpool(wxColourPickerCtrl* spool_widget) : ColorEntry(*wxBLACK), widget(spool_widget) {}
        wxColour get_printed_color(bool use_spool = true) const override {
            return widget->GetColour();
        }
        bool is_spool() override { return true; }
    };
class CreateMMUTiledCanvas : public DPIDialog
{

public:
    CreateMMUTiledCanvas(GUI_App* app, MainFrame* mainframe);
    virtual ~CreateMMUTiledCanvas();

    void create_main_tab(wxPanel* tab);
    void create_color_tab(wxPanel* tab);

    BasicDrawPane* get_canvas() { return m_canvas; }
    void refresh_description();
    int find_extruder(wxColour color);

    void close_me(wxCommandEvent& event_args);

    void on_dpi_changed(const wxRect& suggested_rect) override;
    void create_geometry(wxCommandEvent& event_args);

    void recompute_colors();
    void recreate_color_conversion();
    void refresh_color_conversion(int delete_idx, bool is_add_not_del);
    wxBoxSizer* all_lines_conversion;

    void load_config();
    void save_config();

    std::shared_ptr<ConfigOptionsGroup> group_size;
    std::shared_ptr<ConfigOptionsGroup> group_colors;
    MyDynamicConfig m_config;

    wxStaticText* m_txt_extruder_count;
    wxTextCtrl* m_filename_ctrl;

    //std::vector<wxColourPickerCtrl*> m_clr_bts;
    std::vector<ColorEntrySpool> m_spools;
    std::vector<ColorEntry> m_pixel_colors;
    std::vector<ColorEntry*> m_used_colors;

    wxPanel* m_color_tab;

    BasicDrawPane* m_canvas;

    MainFrame* m_main_frame;
    GUI_App* m_gui_app;
    bool m_dirty = false;

};

} // namespace GUI
} // namespace Slic3r

#endif
