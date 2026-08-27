///|/ Copyright (c) SuperSlicer 2026 Durand R?mi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "CalibrationFlowSpeedDialog.hpp"

#include <string>

#include <wx/display.h>
#include <wx/file.h>
#include <wx/scrolwin.h>

#include "libslic3r/Config/AppConfig.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Config/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include "GLCanvas3D.hpp"
#include "GUI.hpp"
#include "GUI_ObjectList.hpp"
#include "I18N.hpp"
#include "Jobs/ArrangeJob.hpp"
#include "Jobs/Worker.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include "Widgets/ComboBox.hpp"
#include "wxExtensions.hpp"
#if ENABLE_SCROLLABLE
static wxSize get_screen_size(wxWindow* window)
{
    const auto idx = wxDisplay::GetFromWindow(window);
    wxDisplay display(idx != wxNOT_FOUND ? idx : 0u);
    return display.GetClientArea().GetSize();
}
#endif // ENABLE_SCROLLABLE

namespace Slic3r {
namespace GUI {

void CalibrationFlowSpeedDialog::create_buttons(wxStdDialogButtonSizer* buttons){
    const wxSize size(6 * em_unit(), wxDefaultCoord);
    const wxSize bt_size(18 * em_unit(), wxDefaultCoord);

    wxString choices_gram[] = { "0.2","0.5","1","2","5","10" };
    //cmb_gram = new wxComboBox(this, wxID_ANY, wxString{ "1" }, wxDefaultPosition, wxDefaultSize, 5, choices_gram);
    cmb_gram = new ComboBox(this, wxID_ANY, wxString{"1"}, wxDefaultPosition, size, 5, choices_gram);
    cmb_gram->SetToolTip(_L("Choose the size of the patch to print (in gramme). A bigger weight allow to have more precision but it takes longer to print"));
    cmb_gram->SetSelection(2);
    
    wxString choices_nb[] = { "1","2","3","4","5","6","7","8" };
    //cmb_nb_steps = new wxComboBox(this, wxID_ANY, wxString{ "4" }, wxDefaultPosition, wxDefaultSize, 8, choices_nb);
    cmb_nb_steps = new ComboBox(this, wxID_ANY, wxString{"4"}, wxDefaultPosition, size, 8, choices_nb);
    cmb_nb_steps->SetToolTip(_L("Select the number of patches."));
    cmb_nb_steps->SetSelection(4);

    // check max speed (constrained by filament max volumetric flow)

    const DynamicPrintConfig* print_config = this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->get_config();
    const DynamicPrintConfig* printer_config = this->gui_app->get_tab(Preset::TYPE_PRINTER)->get_config();
    const DynamicPrintConfig* filament_config = this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT)->get_config();
    float max_vol_flow = filament_config->option("filament_max_volumetric_speed")->get_float(0);
    float max_speed = filament_config->option("filament_max_speed")->get_float(0);
    float curr_speed = print_config->get_computed_value("solid_infill_speed", 0);

    // compute max speed
    if (max_vol_flow == 0 && max_speed == 0)
        max_speed = curr_speed * 10;
    if (max_vol_flow > 0) {
        float layer_height = print_config->option("layer_height")->get_float();
        float nz = printer_config->option("nozzle_diameter")->get_float(0);
        layer_height = std::max(layer_height, float(print_config->option("first_layer_height")->get_effective_value(nz)));
        float filament_max_overlap = filament_config->option("filament_max_overlap")->get_float();
        Flow  flow                 = Flow::new_from_config(FlowRole::frSolidInfill, *print_config, nz, layer_height,
                                          filament_max_overlap / 100.f, false);
        float current_flow = flow.mm3_per_mm();
        if (max_speed > 0) {
            max_speed = std::min(max_speed, curr_speed * max_vol_flow / current_flow);
        } else {
            max_speed = curr_speed * max_vol_flow / current_flow;
        }
        max_speed = std::min(max_speed, curr_speed * 10);
        if (printer_config->option<ConfigOptionEnum<MachineLimitsUsage>>("machine_limits_usage")->value != MachineLimitsUsage::Ignore) {
            float max_feedarete_x = printer_config->option("machine_max_feedrate_x")->get_float(0);
            max_speed             = std::min(max_speed, max_feedarete_x);
        }
    }

    float min_speed = std::max(filament_config->option("min_print_speed")->get_float(0), 0.);
    if (min_speed <= 0)
        min_speed = curr_speed / 10;
    txt_min_speed = new wxTextCtrl(this, wxID_ANY, Slic3r::from_dot_to_local(Slic3r::to_string_nozero(min_speed, 4)), wxDefaultPosition, size);
    txt_min_speed->SetToolTip(_L("Speed of the first patch."));

    txt_max_speed = new wxTextCtrl(this, wxID_ANY, Slic3r::from_dot_to_local(Slic3r::to_string_nozero(max_speed, 4)), wxDefaultPosition, size);
    txt_max_speed->SetToolTip(_L("Speed of the first patch."));

    txt_min_flow = new wxTextCtrl(this, wxID_ANY, Slic3r::from_dot_to_local(Slic3r::to_string_nozero(0.9, 4)), wxDefaultPosition, size);
    txt_min_flow->SetToolTip(_L("Minimum extrusion multiplier."));

    txt_max_flow = new wxTextCtrl(this, wxID_ANY, Slic3r::from_dot_to_local(Slic3r::to_string_nozero(1.3, 4)), wxDefaultPosition, size);
    txt_max_flow->SetToolTip(_L("Maximum extrusion multiplier."));

    wxString choices_min_overlap[] = { "0","10","20","30","40","50","60","70","80","90"};
    //cmb_min_overlap = new wxComboBox(this, wxID_ANY, wxString{ "4" }, wxDefaultPosition, wxDefaultSize, 8, choices_min_overlap);
    cmb_min_overlap = new ComboBox(this, wxID_ANY, wxString{ "4" }, wxDefaultPosition, size, 8, choices_min_overlap);
    cmb_min_overlap->SetToolTip(_L("Minimum overlap (0%: llnes don't touch (bad but easy to print)"
        " ; 100%: no empty spaces (almost impossible, the filament isn't liquid enough)."));
    cmb_min_overlap->SetSelection(6);
    
    wxString choices_max_overlap[] = { "100","90","80","70","60","50","30", "10"};
    //cmb_max_overlap = new wxComboBox(this, wxID_ANY, wxString{ "4" }, wxDefaultPosition, wxDefaultSize, 8, choices_max_overlap);
    cmb_max_overlap = new ComboBox(this, wxID_ANY, wxString{ "4" }, wxDefaultPosition, size, 8, choices_max_overlap);
    cmb_max_overlap->SetToolTip(_L("Maximum overlap (0%: llnes don't touch (bad but easy to print)"
        " ; 100%: no empty spaces (almost impossible, the filament isn't liquid enough)."));
    cmb_max_overlap->SetSelection(0);
    
    wxBoxSizer* vertical =new wxBoxSizer(wxVERTICAL);
    wxBoxSizer* hsizer_common =new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer* hsizer_overlap =new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer* hsizer_flow =new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer* hsizer_speed =new wxBoxSizer(wxHORIZONTAL);

    hsizer_common->Add(new wxStaticText(this, wxID_ANY, _L("Patch weight:"), wxDefaultPosition, {15 * em_unit(), -1}, wxALIGN_RIGHT));
    hsizer_common->Add(cmb_gram);
    hsizer_common->Add(new wxStaticText(this, wxID_ANY, wxString(" ") + _L("g"), wxDefaultPosition, {4 * em_unit(), -1}, wxALIGN_LEFT));
    hsizer_common->AddSpacer(20);
    hsizer_common->Add(new wxStaticText(this, wxID_ANY, _L("steps:"), wxDefaultPosition, {8 * em_unit(), -1}, wxALIGN_RIGHT));
    hsizer_common->Add(cmb_nb_steps);
    //hsizer_common->AddSpacer(20);
    
    hsizer_flow->Add(new wxStaticText(this, wxID_ANY, _L("Extrusion multiplier:") + wxString(" ") + _L("min:"), wxDefaultPosition, {15 * em_unit(), -1}, wxALIGN_RIGHT));
    hsizer_flow->Add(txt_min_flow);
    hsizer_flow->Add(new wxStaticText(this, wxID_ANY, wxString(" "), wxDefaultPosition, {4 * em_unit(), -1}, wxALIGN_LEFT));
    hsizer_flow->AddSpacer(20);
    hsizer_flow->Add(new wxStaticText(this, wxID_ANY, _L("max:"), wxDefaultPosition, {8 * em_unit(), -1}, wxALIGN_RIGHT));
    hsizer_flow->Add(txt_max_flow);
    hsizer_flow->Add(new wxStaticText(this, wxID_ANY, wxString(" "), wxDefaultPosition, {4 * em_unit(), -1}, wxALIGN_LEFT));
    hsizer_flow->AddSpacer(20);
    wxButton* bt_flow = new wxButton(this, wxID_FILE2, _L("Generate for multiple flows"), wxDefaultPosition, bt_size);
    bt_flow->Bind(wxEVT_BUTTON, &CalibrationFlowSpeedDialog::create_flow, this);
    hsizer_flow->Add(bt_flow);

    hsizer_overlap->Add(new wxStaticText(this, wxID_ANY, _L("min overlap:"), wxDefaultPosition, {15 * em_unit(), -1}, wxALIGN_RIGHT));
    hsizer_overlap->Add(cmb_min_overlap);
    hsizer_overlap->Add(new wxStaticText(this, wxID_ANY, wxString(" ") + _L("%"), wxDefaultPosition, {4 * em_unit(), -1}, wxALIGN_LEFT));
    hsizer_overlap->AddSpacer(20);
    hsizer_overlap->Add(new wxStaticText(this, wxID_ANY, _L("max overlap:"), wxDefaultPosition, {8 * em_unit(), -1}, wxALIGN_RIGHT));
    hsizer_overlap->Add(cmb_max_overlap);
    hsizer_overlap->Add(new wxStaticText(this, wxID_ANY, wxString(" ") + _L("%"), wxDefaultPosition, {4 * em_unit(), -1}, wxALIGN_LEFT));
    hsizer_overlap->AddSpacer(20);
    wxButton* bt_overlap = new wxButton(this, wxID_FILE3, _L("Generate for multiple overlaps"), wxDefaultPosition, bt_size); 
    bt_overlap->Bind(wxEVT_BUTTON, &CalibrationFlowSpeedDialog::create_overlap, this);
    hsizer_overlap->Add(bt_overlap);

    hsizer_speed->Add(new wxStaticText(this, wxID_ANY, _L("min speed:"), wxDefaultPosition, {15 * em_unit(), -1}, wxALIGN_RIGHT));
    hsizer_speed->Add(txt_min_speed);
    hsizer_speed->Add(new wxStaticText(this, wxID_ANY, wxString(" ") + _L("mm/s"), wxDefaultPosition, {4 * em_unit(), -1}, wxALIGN_LEFT));
    hsizer_speed->AddSpacer(20);
    hsizer_speed->Add(new wxStaticText(this, wxID_ANY, _L("max speed:"), wxDefaultPosition, {8 * em_unit(), -1}, wxALIGN_RIGHT));
    hsizer_speed->Add(txt_max_speed);
    hsizer_speed->Add(new wxStaticText(this, wxID_ANY, wxString(" ") + _L("mm/s"), wxDefaultPosition, {4 * em_unit(), -1}, wxALIGN_LEFT));
    hsizer_speed->AddSpacer(20);
    wxButton* bt_speed = new wxButton(this, wxID_FILE1, _L("Generate for multiple speeds"), wxDefaultPosition, bt_size);
    bt_speed->Bind(wxEVT_BUTTON, &CalibrationFlowSpeedDialog::create_speed, this);
    hsizer_speed->Add(bt_speed);

    vertical->Add(hsizer_common);
    vertical->AddSpacer(5);
    vertical->Add(hsizer_flow);
    vertical->AddSpacer(5);
    vertical->Add(hsizer_overlap);
    vertical->AddSpacer(5);
    vertical->Add(hsizer_speed);

    buttons->Add(vertical);
}


std::tuple<float, float, Flow> CalibrationFlowSpeedDialog::get_cube_size(float overlap) {
    const DynamicPrintConfig* print_config = this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->get_config();
    const DynamicPrintConfig* printer_config = this->gui_app->get_tab(Preset::TYPE_PRINTER)->get_config();
    const DynamicPrintConfig* filament_config = this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT)->get_config();

    // compute patch size

    float density = filament_config->option("filament_density")->get_float(0);
    if (density<=0)
        density = 1.25;
    // g/cm3 to g/mm3
    density = density / 1000.f;

    std::string str = cmb_gram->GetValue().ToStdString();
    float weight = parse_float_all_locale(str);

    float nz = printer_config->option("nozzle_diameter")->get_float(0);
    // get max height
    float max_height = print_config->option("extruder_clearance_height")->get_float();
    // multiple of layer height
    float layer_height = print_config->option("layer_height")->get_float();
    layer_height = std::max(layer_height, float(print_config->option("first_layer_height")->get_effective_value(nz)));
    max_height = int(max_height / layer_height) * layer_height;

    //compute flow
    Flow flow;
    float mult_size_for_overlap = 1;
    if (overlap < 100) {
        //Flow flow = Flow::new_from_config(FlowRole::frSolidInfill, *print_config, nz, layer_height, filament_max_overlap / 100.f, false);
        flow = Flow::Flow::new_from_config_width(FlowRole::frSolidInfill, 
            *print_config->option<ConfigOptionFloatOrPercent>("solid_infill_extrusion_width"),
            *print_config->option<ConfigOptionFloatOrPercent>("solid_infill_extrusion_spacing"),
            nz, layer_height, overlap / 100.f);
        float width = flow.width();
        Flow flow_full = Flow::new_from_width(width, nz, layer_height, 1);
        //same width, -> flow has higher spacing, same flow
        assert(std::abs(flow.mm3_per_mm() - flow_full.mm3_per_mm()) < EPSILON);
        assert(flow.spacing() > flow_full.spacing());
        mult_size_for_overlap = std::sqrt(flow.spacing() / flow_full.spacing());
    } else {
        float filament_max_overlap = filament_config->option("filament_max_overlap")->get_float();
        flow = Flow::new_from_config(FlowRole::frSolidInfill, *print_config, nz, layer_height, filament_max_overlap / 100.f, false);
    }

    //compute square size
    for(size_t max_iter = 0; max_iter < 100; max_iter++) {
        // get cube size
        // x*y*z*density = weight
        // x*y = weight / (z*density)
        float size_x = std::sqrt(weight / (density * max_height));
        // enlarge if overlap < 1
        if (mult_size_for_overlap != 1) {
            size_x -= flow.width() * 2.09; // remove external perimeter
            size_x *= mult_size_for_overlap;
            size_x += flow.width() * 2.09; // re-add external perimeter
        }

        // add external perimeter width/spacing diff
        float spacing_diff = layer_height * float(1. - 0.25 * PI);
        size_x += spacing_diff;

        if (size_x > max_height / 2)
            return {size_x, max_height, flow};

        max_height = max_height / 2;
        max_height = int(max_height / layer_height) * layer_height;
    }
    assert(false);
    return { 0, 0 , Flow::bridging_flow(0.2f, 0.2f)};
}

void CalibrationFlowSpeedDialog::create_overlap(wxCommandEvent &event_args) 
{
    std::string str_parse = cmb_min_overlap->GetValue().ToStdString();
    float min_overlap = parse_float_all_locale(str_parse);
    str_parse = cmb_max_overlap->GetValue().ToStdString();
    float max_overlap = parse_float_all_locale(str_parse);

    //const DynamicPrintConfig* print_config = this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->get_config();
    //const DynamicPrintConfig* printer_config = this->gui_app->get_tab(Preset::TYPE_PRINTER)->get_config();
    //const DynamicPrintConfig* filament_config = this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT)->get_config();
    //DynamicPrintConfig full_config = *print_config;
    //full_config.apply(*printer_config);
    //full_config.apply(*filament_config);
    //float speed = full_config.get_computed_value("solid_infill_speed", 0);

    const DynamicPrintConfig* filament_config = this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT)->get_config();
    float extrusion_mult = filament_config->option("extrusion_multiplier")->get_float(0);

    create_geometry(extrusion_mult, extrusion_mult, 0, 0, min_overlap, max_overlap);
}

void CalibrationFlowSpeedDialog::create_flow(wxCommandEvent &event_args) 
{
    std::string str_parse_min = txt_min_flow->GetValue().ToStdString();
    float min_flow = parse_float_all_locale(str_parse_min);
    std::string str_parse_max = txt_max_flow->GetValue().ToStdString();
    float max_flow = parse_float_all_locale(str_parse_max);

    //const DynamicPrintConfig* print_config = this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->get_config();
    //const DynamicPrintConfig* printer_config = this->gui_app->get_tab(Preset::TYPE_PRINTER)->get_config();
    //const DynamicPrintConfig* filament_config = this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT)->get_config();
    //DynamicPrintConfig full_config = *print_config;
    //full_config.apply(*printer_config);
    //full_config.apply(*filament_config);
    //float speed = full_config.get_computed_value("solid_infill_speed", 0);

    const DynamicPrintConfig* print_config = this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->get_config();
    const DynamicPrintConfig* filament_config = this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT)->get_config();
    float overlap      = print_config->option("solid_infill_overlap")->get_float();
    float filament_max_overlap = filament_config->option("filament_max_overlap")->get_float();
    overlap = std::min(overlap, filament_max_overlap);
    float extrusion_mult = filament_config->option("extrusion_multiplier")->get_float(0);

    create_geometry(min_flow, max_flow, 0, 0, std::min(80.f, overlap), std::min(80.f, overlap));
}

void CalibrationFlowSpeedDialog::create_speed(wxCommandEvent &event_args) 
{
    std::string str_parse = txt_min_speed->GetValue().ToStdString();
    float min_speed = parse_float_all_locale(str_parse);
    str_parse = txt_max_speed->GetValue().ToStdString();
    float max_speed = parse_float_all_locale(str_parse);

    const DynamicPrintConfig* print_config = this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->get_config();
    const DynamicPrintConfig* filament_config = this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT)->get_config();
    float overlap      = print_config->option("solid_infill_overlap")->get_float();
    float filament_max_overlap = filament_config->option("filament_max_overlap")->get_float();
    overlap = std::min(overlap, filament_max_overlap);

    float extrusion_mult = filament_config->option("extrusion_multiplier")->get_float(0);

    create_geometry(extrusion_mult, extrusion_mult, min_speed, max_speed, overlap, overlap);
}

void CalibrationFlowSpeedDialog::create_geometry(
    float min_flow, //0-2
    float max_flow, //0-2
    float min_speed, //0-150+ mm/s
    float max_speed, //0-150+ mm/s
    float min_overlap, //0-100 %
    float max_overlap //0-100 %
    ) {

    Plater* plat = this->main_frame->plater();
    Model& model = plat->model();
    if (!plat->new_project(L("Flow calibration")))
        return;
    // wait for slicing end if needed
    wxGetApp().Yield();

    //GLCanvas3D::set_warning_freeze(true);
    bool autocenter = gui_app->app_config->get("autocenter") == "1";
    if (autocenter) {
        //disable auto-center for this calibration.
        gui_app->app_config->set("autocenter", "0");
    }

    std::string str_parse = cmb_nb_steps->GetValue().ToStdString();
    int         nb_steps  = std::stoi(str_parse);

    const DynamicPrintConfig* print_config = this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->get_config();
    const DynamicPrintConfig* printer_config = this->gui_app->get_tab(Preset::TYPE_PRINTER)->get_config();
    const DynamicPrintConfig* filament_config = this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT)->get_config();

    model.clear_objects();
    std::vector<ModelObject*> objs;
    std::vector<Flow> objs_flow;
    for (size_t i = 0; i < nb_steps; i++) {
        // if overlap, compute the size
        float overlap = max_overlap;
        if (nb_steps > 1 && min_overlap < max_overlap)
            overlap   = min_overlap + i * (max_overlap - min_overlap) / (nb_steps - 1);
        auto [cube_xy,cube_z, flow] = get_cube_size(overlap);
        // create name
        std::string name = "cube";
        if (nb_steps > 1) {
            if (min_overlap < max_overlap) {
                name += std::string("_") + std::to_string(int(overlap));
            } else if (min_speed < max_speed && nb_steps > 1) {
                const float speed   = min_speed + i * (max_speed - min_speed) / (nb_steps - 1);
                name += std::string("_") + std::to_string(int(speed));
            } else if (min_flow < max_flow && nb_steps > 1) {
                const float flow   = min_flow + i * (max_flow - min_flow) / (nb_steps - 1);
                name += std::string("_") + std::to_string(int(flow * 100 + EPSILON));
            }
        }
        // create object
        objs.push_back(model.add_object(name.c_str(), "", Slic3r::make_cube(cube_xy, cube_xy, cube_z)));
        objs.back()->add_instance();
        objs_flow.push_back(flow);
    }

    /// --- main config, please modify object config when possible ---
    DynamicPrintConfig new_print_config = *print_config; //make a copy
    new_print_config.set_key_value("complete_objects", new ConfigOptionBool(true));
    //if skirt, use only one
    //if (print_config->option<ConfigOptionInt>("skirts")->get_int() > 0 && print_config->option<ConfigOptionInt>("skirt_height")->get_int() > 0) {
    //    new_print_config.set_key_value("complete_objects_one_skirt", new ConfigOptionBool(true));
    //}

    // same for printer config
    DynamicPrintConfig new_printer_config = *printer_config; //make a copy
    new_printer_config.option<ConfigOptionFloatsOrPercents>("seam_gap")->set_at(FloatOrPercent{0, false}, 0);

    // same for filament config
    DynamicPrintConfig new_filament_config = *filament_config; //make a copy
    new_filament_config.option<ConfigOptionFloats>("slowdown_below_layer_time")->set_at(0, 0);
    
    /// --- custom config ---
    float nz = printer_config->option("nozzle_diameter")->get_float(0);
    float layer_height = print_config->option("layer_height")->get_float();
    layer_height = std::max(layer_height, float(print_config->option("first_layer_height")->get_effective_value(nz)));
    const float extrusion_mult = filament_config->option("extrusion_multiplier")->get_float(0);
    assert(objs_flow.size() == objs.size());
    assert(nb_steps == objs.size());
    for (size_t i = 0; i < nb_steps; i++) {

        if (min_flow < max_flow) {
            if (nb_steps == 1) {
                objs[i]->config.set_key_value("print_extrusion_multiplier", 
                    new ConfigOptionPercent(100 * (min_flow + max_flow) / (2 * extrusion_mult)));
            } else {
                objs[i]->config.set_key_value("print_extrusion_multiplier", 
                    new ConfigOptionPercent(100 * (min_flow + i * (max_flow - min_flow) / (nb_steps - 1)) / extrusion_mult));
            }
        }

        float overlap = max_overlap;
        if (nb_steps > 1 && min_overlap < max_overlap)
            overlap = min_overlap + i * (max_overlap - min_overlap) / (nb_steps - 1);
        objs[i]->config.set_key_value("perimeter_overlap", new ConfigOptionPercent(overlap));
        objs[i]->config.set_key_value("external_perimeter_overlap", new ConfigOptionPercent(overlap));
        objs[i]->config.set_key_value("solid_infill_overlap", new ConfigOptionPercent(overlap));
        objs[i]->config.set_key_value("top_solid_infill_overlap", new ConfigOptionPercent(overlap));

        Flow flow = objs_flow[i];
        objs[i]->config.set_key_value("solid_infill_extrusion_width", new ConfigOptionFloatOrPercent(flow.width(), false));
        objs[i]->config.set_key_value("top_infill_extrusion_width", new ConfigOptionFloatOrPercent(flow.width(), false));
        objs[i]->config.set_key_value("perimeter_extrusion_width", new ConfigOptionFloatOrPercent(flow.width(), false));
        objs[i]->config.set_key_value("external_perimeter_extrusion_width", new ConfigOptionFloatOrPercent(flow.width(), false));
        // keep first_layer_extrusion_width, it doesn't change the weight.
        //objs[i]->config.set_key_value("first_layer_extrusion_width", new ConfigOptionFloatOrPercent(flow.width(), false));

        objs[i]->config.set_key_value("first_layer_size_compensation", new ConfigOptionFloat(0));

        // no brim (but a skirt for primming)
        if (PrintConfigDef::instance().get("brim_ears") != nullptr)
            objs[i]->config.set_key_value("brim_ears", new ConfigOptionBool(false));
        objs[i]->config.set_key_value("brim_width", new ConfigOptionFloat(0));

        objs[i]->config.set_key_value("enforce_full_fill_volume", new ConfigOptionBool(true));
        objs[i]->config.set_key_value("solid_infill_every_layers", new ConfigOptionInt(1));
        objs[i]->config.set_key_value("layer_height", new ConfigOptionFloat(layer_height));
        objs[i]->config.set_key_value("first_layer_height", new ConfigOptionFloatOrPercent(layer_height, false));
        objs[i]->config.set_key_value("solid_fill_pattern", new ConfigOptionEnum<InfillPattern>(ipRectilinear));
        objs[i]->config.set_key_value("top_fill_pattern", new ConfigOptionEnum<InfillPattern>(ipRectilinear));
        objs[i]->config.set_key_value("perimeter_generator", new ConfigOptionEnum<PerimeterGeneratorType>(PerimeterGeneratorType::Classic));
        //disable ironing post-process
        objs[i]->config.set_key_value("ironing", new ConfigOptionBool(false));
        //set speed
        if (nb_steps > 1 && min_speed < max_speed) {
            float speed = float(min_speed + i * double(max_speed - min_speed) / (nb_steps - 1));
            objs[i]->config.set_key_value("perimeter_speed", new ConfigOptionFloatOrPercent(speed, false));
            objs[i]->config.set_key_value("external_perimeter_speed", new ConfigOptionFloatOrPercent(speed, false));
            objs[i]->config.set_key_value("solid_infill_speed", new ConfigOptionFloatOrPercent(speed, false));
            objs[i]->config.set_key_value("top_solid_infill_speed", new ConfigOptionFloatOrPercent(speed, false));
        }
        // keep first_layer_speed.
    }

    //update plater
    //GLCanvas3D::set_warning_freeze(false);
    this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->load_config(new_print_config);
    plat->on_config_change(new_print_config);
    this->gui_app->get_tab(Preset::TYPE_PRINTER)->load_config(new_printer_config);
    plat->on_config_change(new_printer_config);
    this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT)->load_config(new_filament_config);
    plat->on_config_change(new_filament_config);
    //plat->changed_objects(objs_idx);
    this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->update_dirty();

    //update everything, easier to code.
    ObjectList* obj = this->gui_app->obj_list();
    obj->update_after_undo_redo();

    // arrange if needed, after new settings, to take them into account
    if (true) { //has_to_arrange) {
        //update print config (done at reslice but we need it here)
        if (plat->printer_technology() == ptFFF)
            plat->fff_print().apply(plat->model(), *plat->config());
        Worker &ui_job_worker = plat->get_ui_job_worker();
        plat->arrange(ui_job_worker, false);
        ui_job_worker.wait_for_current_job(20000);
    }

    plat->reslice();

    if (autocenter) {
        //re-enable auto-center after this calibration.
        gui_app->app_config->set("autocenter", "1");
    }
}

} // namespace GUI
} // namespace Slic3r
