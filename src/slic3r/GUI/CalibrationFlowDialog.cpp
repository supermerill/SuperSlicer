///|/ Copyright (c) SuperSlicer 2026 Durand R?mi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "CalibrationFlowDialog.hpp"

#include "I18N.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/AppConfig.hpp"
// #include "Jobs/ArrangeJob2.hpp"
#include "GLCanvas3D.hpp"
#include "GUI.hpp"
#include "GUI_ObjectList.hpp"
#include "Jobs/Worker.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include <wx/scrolwin.h>
#include <wx/display.h>
#include <wx/file.h>
#include <wx/wupdlock.h>
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

void CalibrationFlowDialog::create_buttons(wxStdDialogButtonSizer* buttons){
    wxButton* bt = new wxButton(this, wxID_FILE1, _L("Generate 10% intervals around current value"));
    bt->Bind(wxEVT_BUTTON, &CalibrationFlowDialog::create_geometry_10, this);
    buttons->Add(bt);
    buttons->AddSpacer(20);
    bt = new wxButton(this, wxID_FILE2, _L("Generate 2% intervals below current value"));
    bt->Bind(wxEVT_BUTTON, &CalibrationFlowDialog::create_geometry_2_5, this);
    buttons->Add(bt);
}

void CalibrationFlowDialog::create_geometry_10(wxCommandEvent &event_args)
{
    Plater *plat = this->main_frame->plater();
    if (!plat->new_project(L("Flow 10 percent calibration")))
        return;
    create_geometry(80.f, 10.f);
}

void CalibrationFlowDialog::create_geometry_2_5(wxCommandEvent &event_args)
{
    Plater *plat = this->main_frame->plater();
    if (!plat->new_project(L("Flow 2 percent calibration")))
        return;
    create_geometry(92.f, 2.F);
}

void CalibrationFlowDialog::create_geometry(float start, float delta) {
    // wait for slicing end if needed
    wxGetApp().Yield();

    Plater* plat = this->main_frame->plater();
    Model& model = plat->model();
    
    std::unique_ptr<wxWindowUpdateLocker> freeze_gui = std::make_unique<wxWindowUpdateLocker>(this);
    bool autocenter = gui_app->app_config->get("autocenter") == "1";
    if (autocenter) {
        //disable auto-center for this calibration.
        gui_app->app_config->set("autocenter", "0");
    }

    std::vector<size_t> objs_idx = plat->load_files(std::vector<std::string>{
            (boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_flow" / "filament_flow_test_cube.amf").string(),
            (boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_flow" / "filament_flow_test_cube.amf").string(),
            (boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_flow" / "filament_flow_test_cube.amf").string(),
            (boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_flow" / "filament_flow_test_cube.amf").string(),
            (boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_flow" / "filament_flow_test_cube.amf").string()},
        LoadFileOption::LoadModel | LoadFileOption::DontUpdateDirs);


    assert(objs_idx.size() == 5);
    const DynamicPrintConfig* print_config = this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->get_config();
    const DynamicPrintConfig* printerConfig = this->gui_app->get_tab(Preset::TYPE_PRINTER)->get_config();
    
    /// --- scale ---
    // model is created for a 0.4 nozzle, scale xy with nozzle size.
    const ConfigOptionFloats* nozzle_diameter_config = printerConfig->option<ConfigOptionFloats>("nozzle_diameter");
    assert(nozzle_diameter_config->size() > 0);
    float nozzle_diameter = nozzle_diameter_config->get_at(0);
    float xyScale = nozzle_diameter / 0.4;
    //scale z to have 6 layers
    const ConfigOptionFloatOrPercent* first_layer_height_setting = print_config->option<ConfigOptionFloatOrPercent>("first_layer_height");
    double first_layer_height = first_layer_height_setting->get_effective_value(nozzle_diameter);
    double layer_height = nozzle_diameter / 2.;
    layer_height = check_z_step(layer_height, printerConfig->option<ConfigOptionFloat>("z_step")->value); // If z_step is not 0 the slicer will scale to the nearest multiple of z_step so account for that here
    first_layer_height = std::max(first_layer_height, nozzle_diameter / 2.);

    // (zscale / 2) represents the midpoint of the filament_flow_test_cube. Note: we ned to use the height of filament_flow_test_cube before scaling. 
    float z_origin = 0.5f;
    float zscale = first_layer_height + 5 * layer_height;
    //do scaling
    if (xyScale < 0.9 || 1.2 < xyScale) {
        for (size_t i = 0; i < 5; i++)
            model.objects()[objs_idx[i]].scale(xyScale, xyScale, zscale); // base: 10 10 1
    } else {
        for (size_t i = 0; i < 5; i++)
            model.objects()[objs_idx[i]].scale(1, 1, zscale);
    }
    //// move to bed
    //for (size_t i = 0; i < 5; i++)
    //    model.objects()[objs_idx[i]].translate(0, 0, ); // base: 10 10 1


    //add sub-part after scale
    float zscale_number = (first_layer_height + layer_height) / 0.4;
    /* zshift is calculated using the following:
    z_origin: we go back to 0 by removing it.
    ((first_layer_height + layer_height) / 2) represents the midpoint of our indicator tab (it is scaled to be 2 layers tall)
    The 0.3 constant is the same as the delta calculated in add_part below, this should probably be calculated per the model object
    */
    float zshift_number = -z_origin + ((first_layer_height + layer_height) / 2) + 0.3;
    
    // it's rotated but not around the good origin: correct that
    double init_z_rotate_angle = Geometry::deg2rad(plat->config()->opt_float("init_z_rotate"));
    Matrix3d rot_matrix = Eigen::Quaterniond(Eigen::AngleAxisd(init_z_rotate_angle, Vec3d{0,0,1})).toRotationMatrix();
    auto     translate_from_rotation = [&rot_matrix, &model, &objs_idx](int idx, Vec3d &&translation) {
            ModelVolume *vol = model.objects()[objs_idx[idx]].volumes[model.objects()[objs_idx[idx]].volumes.size()-1];
            Geometry::Transformation trsf = vol->get_transformation();
            trsf.set_offset(rot_matrix * translation - translation + trsf.get_offset());
            vol->set_transformation(trsf);
        };
    
    if (delta == 10.f && start == 80.f) {
        add_part(&model.objects()[objs_idx[0]], (boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_flow" / "m20.amf").string(), Vec3d{ 10 * xyScale,0,zshift_number }, Vec3d{ xyScale , xyScale, zscale_number});
        add_part(&model.objects()[objs_idx[1]], (boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_flow" / "m10.amf").string(), Vec3d{ 10 * xyScale,0,zshift_number }, Vec3d{ xyScale , xyScale, zscale_number });
        add_part(&model.objects()[objs_idx[2]], (boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_flow" / "_0.amf").string(), Vec3d{ 10 * xyScale,0,zshift_number }, Vec3d{ xyScale , xyScale, zscale_number });
        add_part(&model.objects()[objs_idx[3]], (boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_flow" / "p10.amf").string(), Vec3d{ 10 * xyScale,0,zshift_number }, Vec3d{ xyScale , xyScale, zscale_number });
        add_part(&model.objects()[objs_idx[4]], (boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_flow" / "p20.amf").string(), Vec3d{ 10 * xyScale,0,zshift_number }, Vec3d{ xyScale , xyScale, zscale_number });
    } else if (delta == 2.f && start == 92.f) {
        add_part(&model.objects()[objs_idx[0]], (boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_flow" / "m8.amf").string(), Vec3d{ 10 * xyScale,0,zshift_number }, Vec3d{ xyScale , xyScale, zscale_number});
        add_part(&model.objects()[objs_idx[1]], (boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_flow" / "m6.amf").string(), Vec3d{ 10 * xyScale,0,zshift_number }, Vec3d{ xyScale , xyScale, zscale_number});
        add_part(&model.objects()[objs_idx[2]], (boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_flow" / "m4.amf").string(), Vec3d{ 10 * xyScale,0,zshift_number }, Vec3d{ xyScale , xyScale, zscale_number});
        add_part(&model.objects()[objs_idx[3]], (boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_flow" / "m2.amf").string(), Vec3d{ 10 * xyScale,0,zshift_number }, Vec3d{ xyScale , xyScale, zscale_number});
        add_part(&model.objects()[objs_idx[4]], (boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_flow" / "_0.amf").string(), Vec3d{ 10 * xyScale,0,zshift_number }, Vec3d{ xyScale , xyScale, zscale_number});
    }
    for (size_t i = 0; i < 5; i++) {
        translate_from_rotation(i, Vec3d{ 10 * xyScale, 0, zscale/2 - z_origin });
        add_part(&model.objects()[objs_idx[i]], (boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_flow" / "O.amf").string(), Vec3d{ 0,0, zscale }, Vec3d{xyScale , xyScale, layer_height / 0.2}); // base: 0.2mm height
    }

    
    /// --- translate ---;
    bool has_to_arrange = true;
    const double brim_width = nozzle_diameter * 3.5;

    /// --- main config, please modify object config when possible ---
    DynamicPrintConfig new_print_config = *print_config; //make a copy
    new_print_config.set_key_value("complete_objects", new ConfigOptionBool(true));
    //if skirt, use only one
    if (print_config->option<ConfigOptionInt>("skirts")->get_int() > 0 && print_config->option<ConfigOptionInt>("skirt_height")->get_int() > 0) {
        new_print_config.set_key_value("complete_objects_one_skirt", new ConfigOptionBool(true));
    }

    /// --- custom config ---
    for (size_t i = 0; i < 5; i++) {
        //brim to have some time to build up pressure in the nozzle
        model.objects()[objs_idx[i]].config.set_key_value("brim_width", new ConfigOptionFloat(brim_width));
        model.objects()[objs_idx[i]].config.set_key_value("thin_perimeters", new ConfigOptionPercent(0));
        model.objects()[objs_idx[i]].config.set_key_value("external_perimeter_overlap", new ConfigOptionPercent(80));
        model.objects()[objs_idx[i]].config.set_key_value("perimeter_overlap", new ConfigOptionPercent(80));
        if (PrintConfigDef::instance().get("brim_ears") != nullptr)
            model.objects()[objs_idx[i]].config.set_key_value("brim_ears", new ConfigOptionBool(false));
        model.objects()[objs_idx[i]].config.set_key_value("perimeters", new ConfigOptionInt(3));
        model.objects()[objs_idx[i]].config.set_key_value("only_one_perimeter_top", new ConfigOptionBool(true));
        model.objects()[objs_idx[i]].config.set_key_value("enforce_full_fill_volume", new ConfigOptionBool(true));
        model.objects()[objs_idx[i]].config.set_key_value("solid_infill_every_layers", new ConfigOptionInt(1));
        model.objects()[objs_idx[i]].config.set_key_value("thin_walls", new ConfigOptionBool(true));
        model.objects()[objs_idx[i]].config.set_key_value("thin_walls_min_width", new ConfigOptionFloatOrPercent(50,true));
        model.objects()[objs_idx[i]].config.set_key_value("gap_fill_enabled", new ConfigOptionBool(true)); 
        model.objects()[objs_idx[i]].config.set_key_value("layer_height", new ConfigOptionFloat(layer_height));
        model.objects()[objs_idx[i]].config.set_key_value("first_layer_height", new ConfigOptionFloatOrPercent(first_layer_height, false));
        model.objects()[objs_idx[i]].config.set_key_value("external_infill_margin", new ConfigOptionFloatOrPercent(100, true));
        model.objects()[objs_idx[i]].config.set_key_value("solid_fill_pattern", new ConfigOptionEnum<InfillPattern>(ipRectilinear));
        model.objects()[objs_idx[0]].config.set_key_value("infill_filled_solid", new ConfigOptionBool(true));
        model.objects()[objs_idx[i]].config.set_key_value("top_fill_pattern", new ConfigOptionEnum<InfillPattern>(ipSmooth));
        //disable ironing post-process
        model.objects()[objs_idx[i]].config.set_key_value("ironing", new ConfigOptionBool(false));
        //set extrusion mult: 80 90 100 110 120
        model.objects()[objs_idx[i]].config.set_key_value("print_extrusion_multiplier", new ConfigOptionPercent(start + (float)i * delta));
    }

    //update plater
    //GLCanvas3D::set_warning_freeze(false);
    this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->load_config(new_print_config);
    plat->on_config_change(new_print_config);
    plat->changed_objects(objs_idx);
    this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->update_dirty();
    //update everything, easier to code.
    ObjectList* obj = this->gui_app->obj_list();
    obj->update_after_undo_redo();
    freeze_gui.reset();
    // arrange if needed, after new settings, to take them into account
    if (has_to_arrange) {
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
