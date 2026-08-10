///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "GLGizmoGenericFacetPainting.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <utility>

#include <GL/glew.h>

#include <wx/utils.h>

#include "libslic3r/Model.hpp"

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

namespace Slic3r::GUI {

static wxString translated_annotation_label(const GenericFacetsAnnotationDefinition &definition,
                                            const std::string &label)
{
    return I18N::translate_in_domain(label, definition.translation_domain);
}

GLGizmoGenericFacetPainting::GLGizmoGenericFacetPainting(GLCanvas3D &parent,
                                                         const std::string &icon_filename,
                                                         unsigned int sprite_id,
                                                         GenericFacetsAnnotationDefinition definition,
                                                         int shortcut_key)
    : GLGizmoPainterBase(parent, icon_filename, sprite_id)
    , m_definition(std::move(definition))
    , m_initial_shortcut_key(shortcut_key)
{
    if (!m_definition.icon_svg.empty())
        this->set_icon_svg_data(m_definition.icon_svg);
}

void GLGizmoGenericFacetPainting::on_shutdown()
{
    m_parent.toggle_model_objects_visibility(true);
}

bool GLGizmoGenericFacetPainting::on_init()
{
    m_shortcut_key = m_initial_shortcut_key;

    m_desc["clipping_of_view"] = _L("Clipping of view") + ": ";
    m_desc["reset_direction"]  = _L("Reset direction");
    m_desc["cursor_size"]      = _L("Brush size") + ": ";
    m_desc["cursor_type"]      = _L("Brush shape") + ": ";
    m_desc["enforce_caption"]  = _L("Left mouse button") + ": ";
    m_desc["enforce"]          = translated_annotation_label(m_definition, m_definition.enforce_label);
    m_desc["block_caption"]    = _L("Right mouse button") + ": ";
    m_desc["block"]            = translated_annotation_label(m_definition, m_definition.block_label);
    m_desc["remove_caption"]   = _L("Shift + Left mouse button") + ": ";
    m_desc["remove"]           = _L("Remove selection");
    m_desc["remove_all"]       = _L("Remove all selection");
    m_desc["circle"]           = _L("Circle");
    m_desc["sphere"]           = _L("Sphere");

    return true;
}

std::string GLGizmoGenericFacetPainting::on_get_name() const
{
    return I18N::translate_utf8_in_domain(m_definition.label, m_definition.translation_domain);
}

void GLGizmoGenericFacetPainting::render_painter_gizmo()
{
    const Selection& selection = m_parent.get_selection();

    glsafe(::glEnable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));

    render_triangles(selection);

    m_c->object_clipper()->render_cut();
    m_c->instances_hider()->render_cut();
    render_cursor();

    glsafe(::glDisable(GL_BLEND));
}

void GLGizmoGenericFacetPainting::on_render_input_window(float x, float y, float bottom_limit)
{
    if (!m_c->selection_info()->model_object())
        return;

    const float approx_height = m_imgui->scaled(12.5f);
    y = std::min(y, bottom_limit - approx_height);
    m_imgui->set_next_window_pos(x, y, ImGuiCond_Always);
    m_imgui->begin(get_name(), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);

    // The dialog is sized from the widest label before controls are drawn.
    // Without this first pass, long plugin labels could overlap sliders or
    // force ImGui to resize while the user is painting.
    const float clipping_slider_left = std::max(m_imgui->calc_text_size(m_desc.at("clipping_of_view")).x,
                                                m_imgui->calc_text_size(m_desc.at("reset_direction")).x)
                                           + m_imgui->scaled(1.5f);
    const float cursor_size_slider_left = m_imgui->calc_text_size(m_desc.at("cursor_size")).x + m_imgui->scaled(1.f);

    const float cursor_type_radio_left   = m_imgui->calc_text_size(m_desc["cursor_type"]).x + m_imgui->scaled(1.f);
    const float cursor_type_radio_sphere = m_imgui->calc_text_size(m_desc["sphere"]).x + m_imgui->scaled(2.5f);
    const float cursor_type_radio_circle = m_imgui->calc_text_size(m_desc["circle"]).x + m_imgui->scaled(2.5f);

    const float button_width = m_imgui->calc_text_size(m_desc.at("remove_all")).x + m_imgui->scaled(1.f);
    const float minimal_slider_width = m_imgui->scaled(4.f);

    float caption_max    = 0.f;
    float total_text_max = 0.f;
    for (const std::string &text_key : std::array<std::string, 3>{"enforce", "block", "remove"}) {
        caption_max    = std::max(caption_max, m_imgui->calc_text_size(m_desc[text_key + "_caption"]).x);
        total_text_max = std::max(total_text_max, m_imgui->calc_text_size(m_desc[text_key]).x);
    }
    total_text_max += caption_max + m_imgui->scaled(1.f);
    caption_max    += m_imgui->scaled(1.f);

    const float sliders_left_width = std::max(cursor_size_slider_left, clipping_slider_left);
    const float slider_icon_width  = m_imgui->get_slider_icon_size().x;
    float       window_width       = minimal_slider_width + sliders_left_width + slider_icon_width;
    window_width = std::max(window_width, total_text_max);
    window_width = std::max(window_width, button_width);
    window_width = std::max(window_width, cursor_type_radio_left + cursor_type_radio_sphere + cursor_type_radio_circle);

    auto draw_text_with_caption = [this, &caption_max](const wxString& caption, const wxString& text) {
        m_imgui->text_colored(ImGuiWrapper::get_COL_LIGHT(), caption);
        ImGui::SameLine(caption_max);
        m_imgui->text(text);
    };

    for (const std::string &text_key : std::array<std::string, 3>{"enforce", "block", "remove"})
        draw_text_with_caption(m_desc.at(text_key + "_caption"), m_desc.at(text_key));

    ImGui::Separator();

    const float max_tooltip_width = ImGui::GetFontSize() * 20.0f;

    ImGui::AlignTextToFramePadding();
    m_imgui->text(m_desc.at("cursor_size"));
    ImGui::SameLine(sliders_left_width);
    ImGui::PushItemWidth(window_width - sliders_left_width - slider_icon_width);
    m_imgui->slider_float("##cursor_radius", &m_cursor_radius, CursorRadiusMin, CursorRadiusMax, "%.2f", 1.0f, true, _L("Alt + Mouse wheel"));

    ImGui::AlignTextToFramePadding();
    m_imgui->text(m_desc.at("cursor_type"));

    float cursor_type_offset = cursor_type_radio_left + (window_width - cursor_type_radio_left - cursor_type_radio_sphere - cursor_type_radio_circle + m_imgui->scaled(0.5f)) / 2.f;
    ImGui::SameLine(cursor_type_offset);
    ImGui::PushItemWidth(cursor_type_radio_sphere);
    if (m_imgui->radio_button(m_desc["sphere"], m_cursor_type == TriangleSelector::CursorType::SPHERE))
        m_cursor_type = TriangleSelector::CursorType::SPHERE;

    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_L("Paints all facets inside, regardless of their orientation."), max_tooltip_width);

    ImGui::SameLine(cursor_type_offset + cursor_type_radio_sphere);
    ImGui::PushItemWidth(cursor_type_radio_circle);
    if (m_imgui->radio_button(m_desc["circle"], m_cursor_type == TriangleSelector::CursorType::CIRCLE))
        m_cursor_type = TriangleSelector::CursorType::CIRCLE;

    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_L("Ignores facets facing away from the camera."), max_tooltip_width);

    ImGui::Separator();
    if (m_c->object_clipper()->get_position() == 0.f) {
        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc.at("clipping_of_view"));
    }
    else {
        if (m_imgui->button(m_desc.at("reset_direction"))) {
            wxGetApp().CallAfter([this]() {
                m_c->object_clipper()->set_position_by_ratio(-1., false);
            });
        }
    }

    float clp_dist = float(m_c->object_clipper()->get_position());
    ImGui::SameLine(sliders_left_width);
    ImGui::PushItemWidth(window_width - sliders_left_width - slider_icon_width);
    if (m_imgui->slider_float("##clp_dist", &clp_dist, 0.f, 1.f, "%.2f", 1.0f, true, _L("Ctrl + Mouse wheel")))
        m_c->object_clipper()->set_position_by_ratio(clp_dist, true);

    ImGui::Separator();
    if (m_imgui->button(m_desc.at("remove_all"))) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _L("Reset selection"), UndoRedo::SnapshotType::GizmoAction);
        ModelObject *mo  = m_c->selection_info()->model_object();
        int          idx = -1;
        for (ModelVolume *mv : mo->volumes)
            if (mv->is_model_part()) {
                ++idx;
                m_triangle_selectors[idx]->reset();
                m_triangle_selectors[idx]->request_update_render_data();
            }

        update_model_object();
        m_parent.set_as_dirty();
    }

    m_imgui->end();
}

void GLGizmoGenericFacetPainting::update_model_object() const
{
    bool updated = false;
    ModelObject* mo = m_c->selection_info()->model_object();
    int idx = -1;
    for (ModelVolume* mv : mo->volumes) {
        if (!mv->is_model_part())
            continue;
        ++idx;
        updated |= mv->facets_annotation_mutable(m_definition.key).set(*m_triangle_selectors[idx].get());
    }

    if (updated) {
        const ModelObjectPtrs& mos = wxGetApp().model().object_ptrs();
        wxGetApp().obj_list()->update_info_items(std::find(mos.begin(), mos.end(), mo) - mos.begin());

        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    }
}

void GLGizmoGenericFacetPainting::update_from_model_object()
{
    wxBusyCursor wait;

    const ModelObject* mo = m_c->selection_info()->model_object();
    m_triangle_selectors.clear();

    for (const ModelVolume* mv : mo->volumes) {
        if (!mv->is_model_part())
            continue;

        // This mesh does not account for the possible Z up SLA offset.
        const TriangleMesh* mesh = &mv->mesh();

        m_triangle_selectors.emplace_back(std::make_unique<TriangleSelectorGUI>(*mesh));

        // A missing annotation means the user has not painted this key yet.
        // TriangleSelectorGUI starts empty, so deserialization is only needed
        // when the ModelVolume contains data for this generic painting key.
        const FacetsAnnotation *annotation = mv->facets_annotation(m_definition.key);
        if (annotation != nullptr)
            m_triangle_selectors.back()->deserialize(annotation->get_data(), false);
        m_triangle_selectors.back()->request_update_render_data();
    }
}

PainterGizmoType GLGizmoGenericFacetPainting::get_painter_type() const
{
    return PainterGizmoType::GENERIC_FACET;
}

wxString GLGizmoGenericFacetPainting::handle_snapshot_action_name(bool shift_down, GLGizmoPainterBase::Button button_down) const
{
    if (shift_down)
        return _L("Remove selection");
    return button_down == Button::Left ? m_desc.at("enforce") : m_desc.at("block");
}

std::string GLGizmoGenericFacetPainting::get_gizmo_entering_text() const
{
    return "Entering " + m_definition.label;
}

std::string GLGizmoGenericFacetPainting::get_gizmo_leaving_text() const
{
    return "Leaving " + m_definition.label;
}

std::string GLGizmoGenericFacetPainting::get_action_snapshot_name() const
{
    return "Paint-on " + m_definition.label + " editing";
}

} // namespace Slic3r::GUI
