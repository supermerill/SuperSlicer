///|/ Copyright (c) Prusa Research 2018 - 2023 Enrico Turri @enricoturri1966, Filip Sykala @Jony01, Vojtěch Bubník @bubnikv, Oleksandra Iushchenko @YuSanka, Lukáš Matěna @lukasmatena, Vojtěch Král @vojtechkral
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_GUI_Preview_hpp_
#define slic3r_GUI_Preview_hpp_

#include <wx/panel.h>

#include "libslic3r/Point.hpp"
#include "libslic3r/CustomGCode.hpp"

#include <string>
#include "libslic3r/GCode/GCodeProcessor.hpp"

#include "GCodeViewer.hpp"

class wxGLCanvas;
class wxBoxSizer;
class wxStaticText;
class wxComboBox;
class wxComboCtrl;
class wxCheckBox;

namespace Slic3r {

class DynamicPrintConfig;
class Print;
class BackgroundSlicingProcess;
class Model;

namespace DoubleSlider {
    class Control;
};

namespace GUI {

class GLCanvas3D;
class GLToolbar;
class Bed3D;
struct Camera;
class Plater;
#ifdef _WIN32
class BitmapComboBox;
#endif

// ----------------------------------------------------------------------------
// titlepanel
// ----------------------------------------------------------------------------
class wxTitledPanel : public wxPanel
{
public:
    std::string name;
    std::string title;
    virtual GLCanvas3D* get_canvas3d() = 0;
    virtual void set_as_dirty() = 0;
    virtual void select_view(const std::string& direction) = 0;
};

class View3D : public wxTitledPanel
{
    wxGLCanvas* m_canvas_widget;
    GLCanvas3D* m_canvas;

public:
    View3D(wxWindow* parent, Bed3D& bed, Model* model, DynamicPrintConfig* config, BackgroundSlicingProcess* process);
    virtual ~View3D();

    wxGLCanvas* get_wxglcanvas() { return m_canvas_widget; }
    GLCanvas3D* get_canvas3d() override { return m_canvas; }

    void set_as_dirty() override;
    void bed_shape_changed();

    void select_view(const std::string& direction) override;
    void select_all();
    void deselect_all();
    void delete_selected();
    void mirror_selection(Axis axis);

    bool is_layers_editing_enabled() const;
    bool is_layers_editing_allowed() const;
    void enable_layers_editing(bool enable);

    bool is_dragging() const;
    bool is_reload_delayed() const;

    void reload_scene(bool refresh_immediately, bool force_full_scene_refresh = false);
    void render();

private:
    bool init(wxWindow* parent, Bed3D& bed, Model* model, DynamicPrintConfig* config, BackgroundSlicingProcess* process);
};

class Preview : public wxTitledPanel
{
    wxGLCanvas* m_canvas_widget { nullptr };
    GLCanvas3D* m_canvas { nullptr };
    wxBoxSizer* m_left_sizer { nullptr };
    wxBoxSizer* m_layers_slider_sizer { nullptr };
    wxPanel* m_bottom_toolbar_panel { nullptr };

    DynamicPrintConfig* m_config;
    BackgroundSlicingProcess& m_process;
    GCodeProcessorResult& m_gcode_result;

#ifdef __linux__
    // We are getting mysterious crashes on Linux in gtk due to OpenGL context activation GH #1874 #1955.
    // So we are applying a workaround here.
    bool m_volumes_cleanup_required { false };
#endif /* __linux__ */

    // Calling this function object forces Plater::schedule_background_process.
    std::function<void()> m_schedule_background_process;

    unsigned int m_number_extruders { 1 };
    bool m_keep_current_preview_type{ false };
    //fields to see what color to display
    bool m_has_switched_to_color = false;
    bool m_has_switched_to_extruders = false;

    bool m_loaded { false };

    DoubleSlider::Control* m_layers_slider{ nullptr };
    DoubleSlider::Control* m_moves_slider{ nullptr };

public:
    enum class OptionType : unsigned int
    {
        Travel,
        Wipe,
        Retractions,
        Unretractions,
        Seams,
        ToolChanges,
        ColorChanges,
        PausePrints,
        CustomGCodes,
        CenterOfGravity,
        Shells,
        ToolMarker,
    };

    enum class ForceState : unsigned int {
        NoForce,
        ForceExtrusions,
        ForceGcode
    };

Preview(wxWindow* parent, Bed3D& bed, Model* model, DynamicPrintConfig* config, BackgroundSlicingProcess& process, 
    GCodeProcessorResult& gcode_result, std::function<void()> schedule_background_process = []() {});
    virtual ~Preview();

    wxGLCanvas* get_wxglcanvas() { return m_canvas_widget; }
    GLCanvas3D* get_canvas3d() override { return m_canvas; }

    void set_as_dirty();

    void bed_shape_changed();
    void select_view(const std::string& direction);
    void set_drop_target(wxDropTarget* target);

    void load_gcode_shells();
    void load_print(bool keep_z_range = false);
    void reload_print(bool keep_volumes = false);
    void refresh_print();
    void set_force_state(ForceState new_force_state = ForceState::NoForce) { current_force_state = new_force_state; }
    ForceState get_force_state() { return current_force_state; }

    void msw_rescale();
    void sys_color_changed();
    void jump_layers_slider(wxKeyEvent& evt);
    void move_layers_slider(wxKeyEvent& evt);
    void edit_layers_slider(wxKeyEvent& evt);

    bool is_loaded() const { return m_loaded; }

    void update_moves_slider();
    void enable_moves_slider(bool enable);
    void move_moves_slider(wxKeyEvent& evt);
    void hide_layers_slider();

    bool can_display_gcode();
    bool can_display_volume();
    void reset_gcode_toolpaths();

    void set_keep_current_preview_type(bool value) { m_keep_current_preview_type = value; }

    void set_layers_slider_values_range(int bottom, int top);

private:
    ForceState current_force_state = ForceState::NoForce;

    bool init(wxWindow* parent, Bed3D& bed, Model* model);

    void bind_event_handlers();
    void unbind_event_handlers();

    void on_size(wxSizeEvent& evt);

    // Create/Update/Reset double slider on 3dPreview
    wxBoxSizer* create_layers_slider_sizer();
    void check_layers_slider_values(std::vector<CustomGCode::Item>& ticks_from_model,
        const std::vector<double>& layers_z);
    void reset_layers_slider();
    void update_layers_slider(const std::vector<double>& layers_z, bool show_gcode_data = false, bool keep_z_range = false);
    void update_layers_slider_mode();
    // update vertical DoubleSlider after keyDown in canvas
    void update_layers_slider_from_canvas(wxKeyEvent& event);

    void load_print_as_fff(bool keep_z_range = false);
    void load_print_as_sla();

    void on_layers_slider_scroll_changed(wxCommandEvent& event);
    void on_moves_slider_scroll_changed(wxCommandEvent& event);
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_Preview_hpp_
