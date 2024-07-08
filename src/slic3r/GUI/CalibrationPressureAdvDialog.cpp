#include "CalibrationPressureAdvDialog.hpp"
#include "I18N.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "GLCanvas3D.hpp"
#include "GUI.hpp"
#include "GUI_ObjectList.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include <wx/scrolwin.h>
#include <wx/display.h>
#include <wx/file.h>
#include "wxExtensions.hpp"
#include "Jobs/ArrangeJob.hpp"
#include <unordered_map>

#pragma optimize("", off)
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

//BUG: custom gcode ' between extrusion role changes' should that be before or after region gcode?

void CalibrationPressureAdvDialog::create_geometry(wxCommandEvent& event_args) {
    /*
    firstPa
    startPa
    endPa
    paIncrement
    erPa
    enableST
    */
   
    std::string  choice_extrusion_role[] = {
    "InternalInfill",
    "BridgeInfill",
    "ExternalPerimeter",
    "GapFill",
    "InternalBridgeInfill",
    "Ironing",
    "OverhangPerimeter",
    "Perimeter",
    "SolidInfill",
    "SupportMaterial",
    "SupportMaterialInterface",
    "ThinWall",
    "TopSolidInfill",
    "FirstLayer"//i've got added them all right?
    };

   std::unordered_map<std::string, std::string> er_width_ToOptionKey = {
    {"InternalInfill", "infill_extrusion_width"},
    //{"BridgeInfill", "placeholder"},//special calc required
    {"ExternalPerimeter", "external_perimeter_extrusion_width"},
    //{"GapFill", "placeholder"},//special calc required
    //{"InternalBridgeInfill", "placeholder"},//special calc required, TODO:find out where/how this is calculated
    {"Ironing", "top_infill_extrusion_width"},
    {"OverhangPerimeter", "overhangs_width"},
    {"Perimeter", "perimeter_extrusion_width"},
    {"SolidInfill", "solid_infill_extrusion_width"},
    {"SupportMaterial", "support_material_extrusion_width"},
    {"SupportMaterialInterface", "support_material_extrusion_width"},
    {"ThinWall", "external_perimeter_extrusion_width"},
    {"TopSolidInfill", "top_infill_extrusion_width"},
    {"FirstLayer", "first_layer_extrusion_width"}

    };

    std::unordered_map<std::string, std::string> er_accel_ToOptionKey = {
    {"InternalInfill", "infill_acceleration"},
    {"BridgeInfill", "bridge_acceleration"},
    {"ExternalPerimeter", "external_perimeter_acceleration"},
    {"GapFill", "gap_fill_acceleration"},
    {"InternalBridgeInfill", "internal_bridge_acceleration"},
    {"Ironing", "ironing_acceleration"},
    {"OverhangPerimeter", "overhangs_acceleration"},
    {"Perimeter", "perimeter_acceleration"},
    {"SolidInfill", "solid_infill_acceleration"},
    {"SupportMaterial", "support_material_acceleration"},
    {"SupportMaterialInterface", "support_material_interface_acceleration"},
    {"ThinWall", "top_solid_infill_acceleration"},
    {"TopSolidInfill", "top_solid_infill_acceleration"},
    {"FirstLayer", "first_layer_acceleration"}
    };

    std::unordered_map<std::string, std::string> er_spacing_ToOptionKey = {
    {"InternalInfill", "infill_extrusion_spacing"},
    //{"BridgeInfill", "placeholder"},
    {"ExternalPerimeter", "external_perimeter_extrusion_spacing"},
    //{"GapFill", "placeholder"},//special calc required for commented ones
    //{"InternalBridgeInfill", "placeholder"},
    //{"Ironing", "ironing_spacing"}, TOFIX? TYPE: coFloat
    {"Ironing", "top_infill_extrusion_spacing"},
    {"OverhangPerimeter", "external_perimeter_extrusion_spacing"},
    {"Perimeter", "perimeter_extrusion_spacing"},
    {"SolidInfill", "solid_infill_extrusion_spacing"},
    {"SupportMaterial", "external_perimeter_extrusion_spacing"}, //TOFIX? TYPE: coFloat
    {"SupportMaterialInterface", "external_perimeter_extrusion_spacing"}, //TOFIX? TYPE: coFloat
    {"ThinWall", "external_perimeter_extrusion_spacing"},
    {"TopSolidInfill", "top_infill_extrusion_spacing"},
    {"FirstLayer", "first_layer_extrusion_spacing"}
    };

    std::unordered_map<std::string, std::string> er_speed_ToOptionKey = {
    {"InternalInfill", "infill_speed"},
    {"BridgeInfill", "bridge_speed"},
    {"ExternalPerimeter", "external_perimeter_speed"},
    {"GapFill", "gap_fill_speed"},
    {"InternalBridgeInfill", "bridge_speed_internal"},
    {"Ironing", "ironing_speed"},
    {"OverhangPerimeter", "overhangs_speed"},
    {"Perimeter", "perimeter_speed"},
    {"SolidInfill", "solid_infill_speed"},
    {"SupportMaterial", "support_material_speed"},
    {"SupportMaterialInterface", "support_material_interface_speed"},
    {"ThinWall", "thin_walls_speed"},
    {"TopSolidInfill", "top_solid_infill_speed"},
    {"FirstLayer", "first_layer_speed"}
    };


    Plater* plat = this->main_frame->plater();
    Model& model = plat->model();
    if (!plat->new_project(L("Pressure calibration")))
        return;

    bool autocenter = gui_app->app_config->get("autocenter") == "1";
    if (autocenter) {
        //disable auto-center for this calibration.
        gui_app->app_config->set("autocenter", "0");
    }
    
    std::vector<std::string> items;
    //for (size_t i = 0; i < nb_runs; i++){
    for (int i = 0; i < currentTestCount; i++) {
        items.emplace_back((boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_pressure" / "base_plate.3mf").string());
    }
    std::vector<size_t> objs_idx = plat->load_files(items, true, false, false, false);
    //assert(objs_idx.size() == nb_runs);
    assert(objs_idx.size() == currentTestCount);
    const DynamicPrintConfig* print_config = this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->get_config();
    const DynamicPrintConfig* filament_config = this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT)->get_config();
    const DynamicPrintConfig* printer_config = this->gui_app->get_tab(Preset::TYPE_PRINTER)->get_config();

    DynamicPrintConfig full_print_config;
    full_print_config.apply(*print_config);
    full_print_config.apply(*printer_config);
    full_print_config.apply(*filament_config);
    
    // --- scale ---
    //models is created for nozzles from 0.1-2mm walls should be nozzle_size*4 spaced, scale xy model by widths down is futher

    GCodeFlavor flavor = printer_config->option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")->value;
    const ConfigOptionFloats* nozzle_diameter_config = printer_config->option<ConfigOptionFloats>("nozzle_diameter");
    assert(nozzle_diameter_config->size() > 0);
    double nozzle_diameter = nozzle_diameter_config->get_at(0);//get extruderID too?

    double first_layer_height = full_print_config.get_computed_value("first_layer_height");
    double base_layer_height = full_print_config.get_computed_value("layer_height");
    double er_width = print_config->get_abs_value("solid_infill_extrusion_width", nozzle_diameter);
    double er_accel = full_print_config.get_computed_value("solid_infill_acceleration");
    double er_speed = full_print_config.get_computed_value("solid_infill_speed");
    double er_spacing = print_config->get_abs_value("external_perimeter_extrusion_spacing",1.0);

    double default_er_width = print_config->get_abs_value("extrusion_width", nozzle_diameter);
    double default_er_speed = full_print_config.get_computed_value("default_speed");
    double default_er_accel = full_print_config.get_computed_value("default_acceleration");
    double default_er_spacing = print_config->get_abs_value("extrusion_spacing", nozzle_diameter);
    double spacing_ratio = full_print_config.get_computed_value("perimeter_overlap");
    double spacing_ratio_external = full_print_config.get_computed_value("external_perimeter_overlap");
    double filament_max_overlap = filament_config->get_computed_value("filament_max_overlap",0);//maybe check for extruderID ?


    // --- translate ---
    //bool autocenter = gui_app->app_config->get("autocenter") == "1";
    bool has_to_arrange = plat->config()->opt_float("init_z_rotate") != 0;
    has_to_arrange = true;

    
    /*if (!autocenter) {
        const ConfigOptionPoints* bed_shape = printer_config->option<ConfigOptionPoints>("bed_shape");
        Vec2d bed_size = BoundingBoxf(bed_shape->values).size();
        Vec2d bed_min = BoundingBoxf(bed_shape->values).min;
        model.objects[objs_idx[0]]->translate({ bed_min.x() + bed_size.x() / 2, bed_min.y() + bed_size.y() / 2, 5 * xyzScale - 5 });
    }*/
    

    std::vector < std::vector<ModelObject*>> pressure_tower;
    bool smooth_time = false;

    std::string nozzle_diameter_str = std::to_string(nozzle_diameter);
    nozzle_diameter_str.erase(nozzle_diameter_str.find_last_not_of('0') + 2, std::string::npos);

    
    if (nozzle_diameter_str.back() == '.') {//if nozzle_diameter_str broke fix it by adding '0' to end, prob not needed?
        nozzle_diameter_str += '0';
    }

    /*size_t decimal_pos = nozzle_diameter_str.find('.');
    // maybe adjust for this ?
    // some users might have 0.0x nozzle size. if that's the case then they should just need to create the file and it should load. ie; 90_bend_0.450.3mf
    if (decimal_pos != std::string::npos) {
        size_t non_zero_pos = nozzle_diameter_str.find_first_not_of('0', decimal_pos + 2);
        nozzle_diameter_str.erase(non_zero_pos, std::string::npos);
    }*/

    std::string bend_90_nozzle_size_3mf = "90_bend_" + nozzle_diameter_str + ".3mf";
    std::string extrusion_role = dynamicExtrusionRole[0]->GetValue().ToStdString();

    for (int id_item = 0; id_item < currentTestCount; id_item++) {
        //need to move this to another function.....
        wxString firstPaValue = dynamicFirstPa[id_item]->GetValue();
        wxString startPaValue = dynamicStartPa[id_item]->GetValue();
        wxString endPaValue = dynamicEndPa[id_item]->GetValue();
        wxString paIncrementValue = dynamicPaIncrement[id_item]->GetValue();
        wxString erPaValue = dynamicExtrusionRole[id_item]->GetValue();
        smooth_time = dynamicEnableST[id_item]->GetValue();

        double first_pa = wxAtof(firstPaValue);
        double start_pa = wxAtof(startPaValue);
        double end_pa = wxAtof(endPaValue);
        double pa_increment = wxAtof(paIncrementValue);
        extrusion_role = dynamicExtrusionRole[id_item]->GetValue().ToStdString();

        int countincrements = 0;
        int sizeofarray = static_cast<int>((end_pa - start_pa) / pa_increment) + 2;//'+2' needed for odd/even numbers 
        std::vector<double> pa_values(sizeofarray);
        std::vector<std::string> c_pa_values_c(sizeofarray);

        double incremented_pa_value = start_pa;
        while (incremented_pa_value <= end_pa + pa_increment / 2) {//this makes a number to be used to load x number of 90 bend models for the PA test.
            if (incremented_pa_value <= end_pa) {
                double rounded_Pa = std::round(incremented_pa_value * 1000000.0) / 1000000.0;
                pa_values[countincrements] = rounded_Pa;//store PA numbers in array to be used later.
                c_pa_values_c[countincrements] = rounded_Pa;
                countincrements++;
                incremented_pa_value += pa_increment;
            }
            else { 
                pa_values[countincrements] = end_pa;
                countincrements++;//failsafe if werid input numbers are provided that can't add the "ending pa" number to the array.
            break; }

        }// is there a limit of how many models SS can load ? might be good to set a failsafe just so it won't load 10k+ models...

        std::string set_advance_prefix ="";
        if (gcfKlipper == flavor) {
            if(smooth_time == false){
                set_advance_prefix = "SET_PRESSURE_ADVANCE ADVANCE=";
            }
            else{
                set_advance_prefix = "SET_PRESSURE_ADVANCE SMOOTH_TIME=";
            }
        }
        else if (gcfMarlinFirmware == flavor) {
            set_advance_prefix = "M900 K";
        }
        else if(gcfRepRap == flavor){
            set_advance_prefix = "M572 S";
        }

        if (extrusion_role == "Verify") {
            countincrements = 13;
            er_width = default_er_width;
            er_spacing = default_er_spacing;
            er_width = er_width * 100 / nozzle_diameter;
            er_width = std::round(er_width * 100.0) / 100.0;
        }
        else{
            for (int i = 0; i < sizeof(choice_extrusion_role) / sizeof(choice_extrusion_role[0]); i++) {

                if (er_width_ToOptionKey.find(extrusion_role) != er_width_ToOptionKey.end()) {

                    //look at maps to match speed/width ect to the selected ER role
                    er_width = print_config->get_abs_value(er_width_ToOptionKey[extrusion_role].c_str(), nozzle_diameter);//look at maps to match speed/width ect to the selected ER role
                    er_speed = full_print_config.get_computed_value(er_speed_ToOptionKey[extrusion_role].c_str());
                    er_accel = full_print_config.get_computed_value(er_accel_ToOptionKey[extrusion_role].c_str());
                    er_spacing = print_config->get_abs_value(er_spacing_ToOptionKey[extrusion_role].c_str(), nozzle_diameter);

                    //potential BUG if any of the values are 0 everything else would fail, need to pull the default value too and assign that?
                    if(er_width == 0){er_width =default_er_width; }
                    if(er_speed == 0){er_speed =default_er_speed; }
                    if(er_accel == 0){er_accel =default_er_accel; }
                    if(er_spacing == 0){er_spacing = default_er_spacing; }

                    er_width = er_width * 100 / nozzle_diameter;
                    er_width = std::round(er_width * 100.0) / 100.0;
                } else {
                    er_width = print_config->get_abs_value("solid_infill_extrusion_width", nozzle_diameter); //used for gapfill_width/bridges selection. TODO: add the bits for this here since gapfill/bridges need special calculations
                    er_width = er_width * 100 / nozzle_diameter;
                    er_width = std::round(er_width * 100.0) / 100.0;

                }
            }
        }

        //-- magical scaling is done here :)
        //the 90_bend models need to be scaled correctly so there is no 'gapfill' since gapfill will effect results.
        double xyzScale = nozzle_diameter / 0.4;
        double er_width_to_scale = magical_scaling(nozzle_diameter,er_width,filament_max_overlap,spacing_ratio,spacing_ratio_external,base_layer_height,er_spacing);
        //-- magical scaling 

        pressure_tower.emplace_back();

        double initial_model_height = 0.2;
        double initial_90_bend_x = 41.20;//fusion=41.200 mm
        double initial_90_bend_y = 20.93;//fusion=20.930 mm
        double initial_number_x = 2.06;//fusion=2.063 mm
        double initial_number_y = 4.12;//fusion=4.125 mm
        double initial_border_x = 1.6;//fusion= 1.6mm
        double initial_point_xy = 0.69;//fusion = 0.687 mm

        double z_scaled_model_height = initial_model_height * (first_layer_height / initial_model_height);
        double xy_scaled_90_bend_x = initial_90_bend_x * er_width_to_scale; 
        double xy_scaled_90_bend_y = initial_90_bend_y * er_width_to_scale;
        double xy_scaled_x = initial_border_x * er_width_to_scale;
        double xy_scaled_number_x = initial_number_x * xyzScale * er_width_to_scale;
        double xy_scaled_number_y = initial_number_y * xyzScale * er_width_to_scale;
        double xy_scaled_point_xy = initial_point_xy * xyzScale * er_width_to_scale;


        double thickness_offset = nozzle_diameter * er_width_to_scale * 2;
        //double z_scale_90_bend = xyzScale * 1.8 / initial_model_height;
        double z_scale_90_bend = (xyzScale + first_layer_height) / initial_model_height;
        double z_scale_factor = 0.0;
        double new_z_world_coords = first_layer_height / 2.0 -base_layer_height;

        if(base_layer_height <= first_layer_height){//normal conditions firstlayer is greater than base
            z_scale_factor = first_layer_height / initial_model_height;
        }else{
            z_scale_factor = first_layer_height + first_layer_height;
        }
            // BUG: output error if first layer height is lower than base layer height
            //      this can cause the numbers to not "show up" on the preview because the z scale is calculated wrong.
            // ie; first_layer_height=0.1 and base_layer_height =0.20
            //BUG: if first/base layer height are both .02 numbers don't show up when sliced. doesn't happen with windows, it did for linux ?
        

        
        std::vector<Eigen::Vector3d> bend_90_positions;
        std::vector<Eigen::Vector3d> number_positions;

        if (extrusion_role == "Verify") {
            
            int nb_bends = 0;
            for (const std::string& role : choice_extrusion_role) {//dynamic add and scale each 90bend model per extrusion role.

                if (er_width_ToOptionKey.find(role) != er_width_ToOptionKey.end()) {

                    er_width = std::round((full_print_config.get_computed_value(er_width_ToOptionKey[role].c_str(), nozzle_diameter) * 100 / nozzle_diameter) * 100.0) / 100.0;
                    er_spacing = full_print_config.get_computed_value(er_spacing_ToOptionKey[role].c_str(), nozzle_diameter);
                    er_width_to_scale = magical_scaling(nozzle_diameter, er_width, filament_max_overlap, spacing_ratio, spacing_ratio_external, base_layer_height, er_spacing);
                    thickness_offset = nozzle_diameter * er_width_to_scale * 2;

                    add_part(model.objects[objs_idx[id_item]], 
                            (boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_pressure" / "scaled_with_nozzle_size" / bend_90_nozzle_size_3mf).string(),
                            Vec3d{ -0.8, (initial_90_bend_y/2) * nb_bends , (z_scale_factor/2) }, Vec3d{ er_width_to_scale, er_width_to_scale, z_scale_90_bend });
                            pressure_tower.back().push_back(model.objects[objs_idx[id_item]]);
                    
                    Eigen::Vector3d modelPosition(-0.8, (initial_90_bend_y/2) * nb_bends, (z_scale_factor/2) );
                    bend_90_positions.push_back(modelPosition);
                    nb_bends++;
                }
                else{//ER role not found in map; ie not currently supported.
                    er_width = std::round((default_er_width * 100 / nozzle_diameter) * 100.0) / 100.0;
                    er_spacing = default_er_spacing;
                    er_width_to_scale = magical_scaling(nozzle_diameter, er_width, filament_max_overlap, spacing_ratio, spacing_ratio_external, base_layer_height, er_spacing);
                    thickness_offset = nozzle_diameter * er_width_to_scale * 2;

                    add_part(model.objects[objs_idx[id_item]], 
                        (boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_pressure" / "scaled_with_nozzle_size" / bend_90_nozzle_size_3mf).string(),
                        Vec3d{ -0.8, (initial_90_bend_y/2) * nb_bends , (z_scale_factor/2) }, Vec3d{ er_width_to_scale, er_width_to_scale, z_scale_90_bend });
                        pressure_tower.back().push_back(model.objects[objs_idx[id_item]]);
                    
                    Eigen::Vector3d modelPosition(-0.8, (initial_90_bend_y/2) * nb_bends, (z_scale_factor/2) );
                    bend_90_positions.push_back(modelPosition);
                    nb_bends++;
                
                }
                
            }
        }
        else{//not verify
            for (int nb_bends = 0; nb_bends < countincrements; nb_bends++){//TODO: BUG: need to fix this for the multi test plates. i should be able to have single if statement to change the "verify" role positions and only have a single 'add_part' fr the 90_bend model.
                //const double magical_transformation_y_pos = 10.47;

                add_part(model.objects[objs_idx[id_item]], 
                        (boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_pressure" / "scaled_with_nozzle_size" / bend_90_nozzle_size_3mf).string(),
                        Vec3d{ -0.8, double(nb_bends) * (thickness_offset*2) *2 , (z_scale_factor/2) }, Vec3d{ er_width_to_scale, er_width_to_scale, z_scale_90_bend });
                        pressure_tower.back().push_back(model.objects[objs_idx[id_item]]);
                
                Eigen::Vector3d modelPosition(-0.8, double(nb_bends) * (thickness_offset*2) *2 , (z_scale_factor/2) );
                bend_90_positions.push_back(modelPosition);
            }
        }

        for (int nb_bends = 0; nb_bends < countincrements;nb_bends++){

            if(nb_bends == 1 && extrusion_role != "Verify") {//only load once. this onyl determines when the borders get loaded, keeping at top of list makes it easier to scroll down to. it can't be '0' since it needs the numbers positions!

                const double extra_size_y = xy_scaled_90_bend_y / 4;
                const double extra_size_x = xy_scaled_number_x;

                const double magical_transformation_x_pos = 20.60; //what is this, and how is this calculated ? >:(
                const double magical_transformation_y_pos = 10.47; //load a model without moving its pos to find see what it is.the number doesn't seem to change regardless of layer heights/nozzle size
                const double magical_transformation_z_pos = 0.1;
                Eigen::Vector3d bend_pos_first = bend_90_positions[0];
                Eigen::Vector3d bend_pos_mid = bend_90_positions[countincrements/2];
                Eigen::Vector3d bend_pos_last = bend_90_positions[countincrements-1];

                Eigen::Vector3d number_pos_first = number_positions[0];
                Eigen::Vector3d number_pos_mid = number_positions[3];
                Eigen::Vector3d number_pos_last = number_positions[6];
                double numbers_total_width = (number_pos_last.x() + (xy_scaled_number_x / 2)) - (number_pos_first.x() - (xy_scaled_number_x / 2));

                double scaled_r_border_x_percentage = ((numbers_total_width + extra_size_x) / initial_border_x) * 100;
                double scaled_r_border_x_mm = (scaled_r_border_x_percentage / 100) * initial_border_x;
                double scaled_tb_border_x = scaled_r_border_x_mm + xy_scaled_90_bend_x;
                double scaled_tb_border_x_percentage = ((scaled_tb_border_x /* + extra_size_x*/) / initial_border_x) * 100;

                
                double total_height = (bend_pos_last.y() + (xy_scaled_90_bend_y / 2)) - (bend_pos_first.y() - (xy_scaled_90_bend_y / 2));
                double scaled_border_y_percentage = ((total_height + extra_size_y) / initial_90_bend_y) * 100;
                double border_scaled_y = (initial_border_x*(xy_scaled_x * 1.5)) / initial_90_bend_y;//TODO: need to adjust scale for larger nozzle sizes


                double right_border_x_pos = number_pos_mid.x();
                double top_border_x_pos = ((number_pos_last.x() + (xy_scaled_number_x / 2)) + (bend_pos_first.x() - (xy_scaled_90_bend_x / 2))) / 2;
                double left_border_x_pos = bend_pos_first.x() - (xy_scaled_90_bend_x / 2);

                //----------
                add_part(model.objects[objs_idx[id_item]], 
                    (boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_pressure" / "pa_border.3mf").string(),
                    Vec3d{ left_border_x_pos + magical_transformation_x_pos, bend_pos_mid.y(), new_z_world_coords - magical_transformation_z_pos }, //need to fix to adjust for nozzle_diameter since it breaks bottom_solid_layers
                                    /*scale*/Vec3d{ xy_scaled_x * 1.5, scaled_border_y_percentage*0.01, z_scale_factor }); // Left border
                //----------
                add_part(model.objects[objs_idx[id_item]], (boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_pressure" / "pa_border.3mf").string(),
                    Vec3d{ right_border_x_pos + magical_transformation_x_pos , bend_pos_mid.y(), new_z_world_coords - magical_transformation_z_pos },
                                    /*scale*/Vec3d{ scaled_r_border_x_percentage*0.01 , scaled_border_y_percentage*0.01 , z_scale_factor});// right border
                
                bool enable_top_bottom = true;
                if(enable_top_bottom == true){//remove later
                    //----------
                    add_part(model.objects[objs_idx[id_item]], (boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_pressure" / "pa_border.3mf").string(),
                        Vec3d{ top_border_x_pos + magical_transformation_x_pos , bend_pos_first.y() - (xy_scaled_90_bend_y /1.8), new_z_world_coords - magical_transformation_z_pos }, //need to fix to adjust for nozzle_diameter since it breaks bottom_solid_layers
                                        /*scale*/Vec3d{ scaled_tb_border_x_percentage*0.01, border_scaled_y, z_scale_factor });//bottom border
                    //----------
                    add_part(model.objects[objs_idx[id_item]], (boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_pressure" / "pa_border.3mf").string(),
                        Vec3d{ top_border_x_pos + magical_transformation_x_pos , bend_pos_last.y() + (xy_scaled_90_bend_y /1.8) , new_z_world_coords - magical_transformation_z_pos }, //need to fix to adjust for nozzle_diameter since it breaks bottom_solid_layers
                                        /*scale*/Vec3d{ scaled_tb_border_x_percentage*0.01, border_scaled_y, z_scale_factor});//top border
                }

                //  position in printer coords are half of scaled size!
                //  scale model in percentage from original models xy values!

            }
            if (extrusion_role != "Verify") {// possible to load the words for each ER role?

                if (nb_bends % 2 == 1) { // Skip generating every second number
                    continue;
                }

                Eigen::Vector3d bend_90_pos = bend_90_positions[nb_bends];
                const double magical_transformation_y_pos = 10.47;
                const double magical_transformation_num_x_pos = 1.03;
                const double magical_transformation_num_y_pos = 2.06;// -2.03
                const double magical_transformation_z_pos = 0.12;//0.1 is the transformation value, but set slightly higher so numbers would be "inside" right border this might be dependant on z_scale_factor

                double bend_90_y = bend_90_pos.y() + magical_transformation_y_pos + (xy_scaled_90_bend_y/2);
                double bend_90_x = bend_90_pos.x() + magical_transformation_num_x_pos;
                double xpos_initial = bend_90_x + (xy_scaled_90_bend_x/2) - xy_scaled_number_x + nozzle_diameter;
                double ypos_inital = bend_90_y /*+ (xy_scaled_number_y/2)*/;
                double ypos_point = bend_90_y - (xy_scaled_number_y/2) - nozzle_diameter;

                double xpos = xpos_initial;
                double ypos = ypos_inital;
                
                std::string pa_values_string = std::to_string(pa_values[nb_bends]);
                std::string threemf =".3mf";
            
                for (int j = 0; j < 7; ++j) {//not sure how the code will respond with a positive array list? ie ; 100.2 this moves decimal point thus breaking the code from loading model since "..3mf" not a real file

                    std::string numered3mfpath = pa_values_string[j] + threemf;
                    
                    if (pa_values_string[j] == '.') {

                        add_part(model.objects[objs_idx[id_item]], (boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_pressure" / "point.3mf").string(),
                            Vec3d{ xpos + xy_scaled_number_x + nozzle_diameter , ypos_point, z_scaled_model_height - magical_transformation_z_pos }, Vec3d{ xyzScale * er_width_to_scale, xyzScale+(xyzScale/2), z_scale_factor*2 });

                        Eigen::Vector3d modelPosition(xpos + xy_scaled_number_x + nozzle_diameter + magical_transformation_num_x_pos, ypos_point, z_scaled_model_height - magical_transformation_z_pos );
                        number_positions.push_back(modelPosition);
                        xpos = xpos + xy_scaled_point_xy + (nozzle_diameter * 2 );
                    }
                    else if (std::isdigit(pa_values_string[j])) {
                        
                        add_part(model.objects[objs_idx[id_item]], (boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_pressure" / numered3mfpath).string(),
                            Vec3d{ xpos + xy_scaled_number_x + nozzle_diameter /* +magical_transformation_num_x_pos */, ypos, z_scaled_model_height - magical_transformation_z_pos }, Vec3d{ xyzScale * er_width_to_scale, xyzScale * er_width_to_scale, z_scale_factor*2 });
                        
                        Eigen::Vector3d modelPosition(xpos + xy_scaled_number_x + nozzle_diameter + magical_transformation_num_x_pos, ypos, z_scaled_model_height - magical_transformation_z_pos );
                        number_positions.push_back(modelPosition);
                        xpos = xpos + xy_scaled_number_x + nozzle_diameter /* +magical_transformation_num_x_pos */;
                    }
                }
            }
        }
    
    }
    /// --- main config ---
    // => settings that are for object or region should be added to the model (see below, in the for loop), not here
    DynamicPrintConfig new_print_config = *print_config;
    DynamicPrintConfig new_printer_config = *printer_config;
    new_print_config.set_key_value("avoid_crossing_perimeters", new ConfigOptionBool(false));
    new_print_config.set_key_value("complete_objects", new ConfigOptionBool(false)); //true is required for multi tests on single plate.

    //fix this later need to fix the loops
    //new_printer_config.set_key_value("before_layer_gcode", new ConfigOptionString(std::string("{if layer_num == 0} ") + set_advance_prefix + std::to_string(first_pa) + " {endif}"));


    //assert(filament_temp_item_name.size() == nb_runs);
    //assert(model.objects.size() == nb_runs);
    assert(objs_idx.size() == currentTestCount);
    for (int id_item = 0; id_item < currentTestCount; id_item++) {

        wxString firstPaValue = dynamicFirstPa[id_item]->GetValue();
        wxString startPaValue = dynamicStartPa[id_item]->GetValue();
        wxString endPaValue = dynamicEndPa[id_item]->GetValue();
        wxString paIncrementValue = dynamicPaIncrement[id_item]->GetValue();
        wxString erPaValue = dynamicExtrusionRole[id_item]->GetValue();
        smooth_time = dynamicEnableST[id_item]->GetValue();

        double first_pa = wxAtof(firstPaValue);
        double start_pa = wxAtof(startPaValue);
        double end_pa = wxAtof(endPaValue);
        double pa_increment = wxAtof(paIncrementValue);
        extrusion_role = dynamicExtrusionRole[id_item]->GetValue().ToStdString();

        int countincrements = 0;
        int sizeofarray = static_cast<int>((end_pa - start_pa) / pa_increment) + 2;//'+2' needed for odd/even numbers 
        std::vector<double> pa_values(sizeofarray);
        std::vector<std::string> c_pa_values_c(sizeofarray);

        double incremented_pa_value = start_pa;
        while (incremented_pa_value <= end_pa + pa_increment / 2) {//this makes a number to be used to load x number of 90 bend models for the PA test.
            if (incremented_pa_value <= end_pa) {
                double rounded_Pa = std::round(incremented_pa_value * 1000000.0) / 1000000.0;
                pa_values[countincrements] = rounded_Pa;//store PA numbers in array to be used later.
                c_pa_values_c[countincrements] = rounded_Pa;
                countincrements++;
                incremented_pa_value += pa_increment;
            }
            else { 
                pa_values[countincrements] = end_pa;
                countincrements++;//failsafe if werid input numbers are provided that can't add the "ending pa" number to the array.
            break; }

        }// is there a limit of how many models SS can load ? might be good to set a failsafe just so it won't load 10k+ models...

        /*
        gcfRepRap,
        gcfSprinter,
        gcfRepetier,
        gcfTeacup,
        gcfMakerWare,
        gcfMarlinLegacy,
        gcfMarlinFirmware,
        gcfLerdge,
        gcfKlipper,
        gcfSailfish,
        gcfMach3,
        gcfMachinekit,
        gcfSmoothie,
        gcfNoExtrusion*/

        // config for the this model
        model.objects[objs_idx[id_item]]->config.set_key_value("bottom_fill_pattern", new ConfigOptionEnum<InfillPattern>(ipMonotonicWGapFill));
        model.objects[objs_idx[id_item]]->config.set_key_value("bottom_solid_layers", new ConfigOptionInt(1));
        model.objects[objs_idx[id_item]]->config.set_key_value("brim_width", new ConfigOptionFloat(0));
        model.objects[objs_idx[id_item]]->config.set_key_value("external_perimeter_overlap", new ConfigOptionPercent(100));
        model.objects[objs_idx[id_item]]->config.set_key_value("fill_density", new ConfigOptionPercent(0));
        model.objects[objs_idx[id_item]]->config.set_key_value("gap_fill_enabled", new ConfigOptionBool(true)); //should be false?, enabled for testing
        model.objects[objs_idx[id_item]]->config.set_key_value("min_width_top_surface", new ConfigOptionFloatOrPercent(0.0,false));
        model.objects[objs_idx[id_item]]->config.set_key_value("only_one_perimeter_top", new ConfigOptionBool(false));
        model.objects[objs_idx[id_item]]->config.set_key_value("only_one_perimeter_first_layer", new ConfigOptionBool(true));
        model.objects[objs_idx[id_item]]->config.set_key_value("perimeter_overlap", new ConfigOptionPercent(100));
        model.objects[objs_idx[id_item]]->config.set_key_value("seam_position", new ConfigOptionEnum<SeamPosition>(spRear)); //BUG: should be fixed in 2.7 merge/SS 2.5.59.7, when this is changed the "perimeters & shell" doesn't turn red indicating a change.
        model.objects[objs_idx[id_item]]->config.set_key_value("top_solid_layers", new ConfigOptionInt(0));

        size_t num_part = 0;
        const int extra_vol = 1;
        for (ModelObject* part : pressure_tower[id_item]) {//loop though each part/volume and assign the modifers

            std::string er_role ="";
            if (extrusion_role == "Verify") {
                er_role = choice_extrusion_role[num_part];
            }
            else{
                er_role = extrusion_role;
            }
            if (er_width_ToOptionKey.find(er_role) != er_width_ToOptionKey.end()) {
                er_width = std::round((full_print_config.get_computed_value(er_width_ToOptionKey[er_role].c_str(), nozzle_diameter) * 100 / nozzle_diameter) * 100.0) / 100.0;
                er_speed = full_print_config.get_computed_value(er_speed_ToOptionKey[er_role].c_str(), nozzle_diameter);
                er_accel = full_print_config.get_computed_value(er_accel_ToOptionKey[er_role].c_str(), nozzle_diameter);
            }
            else{
                er_width = std::round((default_er_width * 100 / nozzle_diameter) * 100.0) / 100.0;
                er_speed = default_er_speed;
                er_accel = default_er_accel;
            }


            std::string set_advance_prefix ="";
            if (gcfKlipper == flavor) {
                if(smooth_time == false){
                    set_advance_prefix = "SET_PRESSURE_ADVANCE ADVANCE=";
                }
                else{
                    set_advance_prefix = "SET_PRESSURE_ADVANCE SMOOTH_TIME=";
                }
            }
            else if (gcfMarlinFirmware == flavor) {
                set_advance_prefix = "M900 K";
            }
            else if(gcfRepRap == flavor){
                set_advance_prefix = "M572 S";
            }

            er_width = (er_width == 0) ? std::round((default_er_width * 100 / nozzle_diameter) * 100.0) / 100.0 : er_width;
            er_speed = (er_speed == 0) ? default_er_speed : er_speed;
            er_accel = (er_accel == 0) ? default_er_accel : er_accel;

            /// --- custom config --- // this is for forcing each model to have x print modifiers
            model.objects[objs_idx[id_item]]->volumes[num_part + extra_vol]->config.set_key_value("perimeter_extrusion_width", new ConfigOptionFloatOrPercent(er_width, true));
            model.objects[objs_idx[id_item]]->volumes[num_part + extra_vol]->config.set_key_value("external_perimeter_extrusion_width", new ConfigOptionFloatOrPercent(er_width, true));//TODO: check widths and ect breaks if any values are in mm/percentage
            model.objects[objs_idx[id_item]]->volumes[num_part + extra_vol]->config.set_key_value("perimeter_speed", new ConfigOptionFloatOrPercent(er_speed, false));
            model.objects[objs_idx[id_item]]->volumes[num_part + extra_vol]->config.set_key_value("external_perimeter_speed", new ConfigOptionFloatOrPercent(er_speed, false));
            model.objects[objs_idx[id_item]]->volumes[num_part + extra_vol]->config.set_key_value("gap_fill_speed", new ConfigOptionFloatOrPercent(er_speed, false));
            model.objects[objs_idx[id_item]]->volumes[num_part + extra_vol]->config.set_key_value("perimeter_acceleration", new ConfigOptionFloatOrPercent(er_accel, false));
            model.objects[objs_idx[id_item]]->volumes[num_part + extra_vol]->config.set_key_value("external_perimeter_acceleration", new ConfigOptionFloatOrPercent(er_accel, false));
            model.objects[objs_idx[id_item]]->volumes[num_part + extra_vol]->config.set_key_value("gap_fill_acceleration", new ConfigOptionFloatOrPercent(er_accel, false));

            if (extrusion_role == "Verify") {
                model.objects[objs_idx[id_item]]->volumes[num_part + extra_vol]->config.set_key_value("region_gcode", new ConfigOptionString(set_advance_prefix + " ; " + er_role ));//user manual type in values
                new_printer_config.set_key_value("before_layer_gcode", new ConfigOptionString(std::string("{if layer_num == 0} ") + set_advance_prefix + std::to_string(first_pa) + " {endif}"));
            }
            else{//add '\n' in?
                model.objects[objs_idx[id_item]]->volumes[num_part + extra_vol]->config.set_key_value("region_gcode", new ConfigOptionString(set_advance_prefix + std::to_string(pa_values[num_part]) + " ; " + er_role ));
                new_printer_config.set_key_value("before_layer_gcode", new ConfigOptionString(std::string("{if layer_num == 0} ") + set_advance_prefix + std::to_string(first_pa) + " {endif}"));
            }
            num_part++;
        }

    }

    //update plater
    this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->load_config(new_print_config);
    plat->on_config_change(new_print_config);
    this->gui_app->get_tab(Preset::TYPE_PRINTER)->load_config(new_printer_config);
    plat->on_config_change(new_printer_config);
    for (size_t obj_idx : objs_idx) { model.objects[obj_idx]->ensure_on_bed(); } // put at the correct z (kind of arrange-z))
    plat->changed_objects(objs_idx);
    this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->update_dirty();
    this->gui_app->get_tab(Preset::TYPE_PRINTER)->update_dirty();
    plat->is_preview_shown();
    //update everything, easier to code.
    ObjectList* obj = this->gui_app->obj_list();
    obj->update_after_undo_redo();

    // arrange if needed, after new settings, to take them into account
    if (has_to_arrange) {
        //update print config (done at reslice but we need it here)
        if (plat->printer_technology() == ptFFF)
            plat->fff_print().apply(plat->model(), *plat->config());
        std::shared_ptr<ProgressIndicatorStub> fake_statusbar = std::make_shared<ProgressIndicatorStub>();
        ArrangeJob arranger(std::dynamic_pointer_cast<ProgressIndicator>(fake_statusbar), plat);
        arranger.prepare_all();
        arranger.process();
        arranger.finalize();
    }

    if (extrusion_role != "Verify") {//don't auto slice so user can manual add PA values
        plat->reslice(); //forces a slice of plater.
    }

    if (autocenter) {
        //re-enable auto-center after this calibration.
        gui_app->app_config->set("autocenter", "1");
    }
}

double CalibrationPressureAdvDialog::magical_scaling(double nozzle_diameter, double er_width, double filament_max_overlap, double spacing_ratio, double spacing_ratio_external, double base_layer_height, double er_spacing ){
    
    double xyzScale = nozzle_diameter / 0.4;
    double er_width_decimal = er_width * nozzle_diameter / 100.0;//models are generated to be default width of x4 lines for the walls ie; 0.4mm nozzle is 1.6mm thick walls
    double er_width_to_scale =1.0;
    double overlap_ratio = 1;
    if (filament_max_overlap) {overlap_ratio = filament_max_overlap;}

    spacing_ratio = std::min(overlap_ratio * 0.5f, spacing_ratio_external / 2.0);
    double new_scale_spacing = er_width_decimal-base_layer_height*float(1. -0.25 *PI)* spacing_ratio;
    double spacing_value = std::round((new_scale_spacing / nozzle_diameter) * 100); //spacing_value = Round((Spacing / Max Nozzle Diameter) * 100)
    er_spacing = (std::round(spacing_value * 10000) / 10000) *0.01;


    if (xyzScale > 4 ) {
        er_width_to_scale = 1.0;
    }
    else{
        er_width_to_scale = er_spacing -(nozzle_diameter/2*0.01);//need to scale slightly under to help with models being correct TODO: test more configurations of nozzle sizes/layer heights
        //if use has the 'wrong' min layer height for a nozzle size, the model will get filled with "gapfill" not a normal extrusion, need to test more for what variables 'break' it                          
    }

    return er_width_to_scale;
}

void CalibrationPressureAdvDialog::create_buttons(wxStdDialogButtonSizer* buttons){
    const DynamicPrintConfig* printer_config = this->gui_app->get_tab(Preset::TYPE_PRINTER)->get_config();
    GCodeFlavor flavor = printer_config->option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")->value;

    std::string prefix = (gcfMarlinFirmware == flavor) ? " LA " : ((gcfKlipper == flavor || gcfRepRap == flavor) ? " PA " : "unsupported firmware type");


    if (prefix != "unsupported firmware type") {

        wxString number_of_runs[] = { "1", "2", "3", "4", "5" };
        nbRuns = new wxComboBox(this, wxID_ANY, wxString{ "1" }, wxDefaultPosition, wxDefaultSize, 5, number_of_runs);
        nbRuns->SetToolTip(_L("Select the number of tests to generate, max 2 is recommended due to bed size limits"));
        nbRuns->SetSelection(0);
        nbRuns->Bind(wxEVT_COMBOBOX, &CalibrationPressureAdvDialog::on_row_change, this);

        dynamicSizer = new wxBoxSizer(wxVERTICAL);
        buttons->Add(dynamicSizer, 1, wxEXPAND | wxALL, 5);
   
        wxBoxSizer* commonSizer = new wxBoxSizer(wxHORIZONTAL);
        commonSizer->Add(new wxStaticText(this, wxID_ANY, _L("Number of tests: ")));
        commonSizer->Add(nbRuns);
        dynamicSizer->Add(commonSizer, 0, wxALL, 5);
        currentTestCount = wxAtoi(nbRuns->GetValue());


        wxButton* bt = new wxButton(this, wxID_FILE1, _L("Generate"));
        bt->Bind(wxEVT_BUTTON, &CalibrationPressureAdvDialog::create_geometry, this);
        dynamicSizer->Add(bt, 0, wxALL, 5);

        create_row_controls(dynamicSizer, currentTestCount);
    } else {
        buttons->Add(new wxStaticText(this, wxID_ANY, _L(prefix)));
    }

    //this->SetSizerAndFit(dynamicSizer);
}

void CalibrationPressureAdvDialog::create_row_controls(wxBoxSizer* parent_sizer, int row_count) {
    wxString choices_first_layerPA[] = { "0.025", "0.030", "0.035", "0.040", "0.045", "0.050" };
    wxString choices_start_PA[] = { "0.0", "0.010", "0.020", "0.030", "0.040", "0.050" };
    wxString choices_end_PA[] = { "0.10", "0.20", "0.30", "0.40", "0.50", "0.60", "0.70", "0.80", "0.90", "1.00" };
    wxString choices_increment_PA[] = { "0.0010", "0.0025", "0.0035", "0.005", "0.006", "0.007", "0.01", "0.1" };
    wxString choices_extrusion_role[] = {
        "InternalInfill", "BridgeInfill", "ExternalPerimeter", "GapFill", "InternalBridgeInfill",
        "Ironing", "OverhangPerimeter", "Perimeter", "SolidInfill", "SupportMaterial",
        "SupportMaterialInterface", "ThinWall", "TopSolidInfill", "FirstLayer", "Verify"
    };
    const DynamicPrintConfig* printer_config = this->gui_app->get_tab(Preset::TYPE_PRINTER)->get_config();
    GCodeFlavor flavor = printer_config->option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")->value;
    std::string prefix = (gcfMarlinFirmware == flavor) ? " LA " : ((gcfKlipper == flavor || gcfRepRap == flavor) ? " PA " : "unsupported firmware type");

    for (int i = 0; i < row_count; i++) {
        wxBoxSizer* rowSizer = new wxBoxSizer(wxHORIZONTAL);

        wxComboBox* firstPaCombo = new wxComboBox(this, wxID_ANY, wxString{ "0.040" }, wxDefaultPosition, wxDefaultSize, 6, choices_first_layerPA);
        //rowSizer->Add(new wxStaticText(this, wxID_ANY, _L( prefix + " Test " + std::to_string(i) + ": " ))); rowSizer->AddSpacer(5);
        rowSizer->Add(new wxStaticText(this, wxID_ANY, _L("First Layers" + prefix + "value: ")));
        firstPaCombo->SetToolTip(_L("Select the first layer" + prefix +" value to be used for the first layer only."));
        firstPaCombo->SetSelection(3);
        rowSizer->Add(firstPaCombo);
        dynamicFirstPa.push_back(firstPaCombo);

        rowSizer->AddSpacer(15);

        wxComboBox* startPaCombo = new wxComboBox(this, wxID_ANY, wxString{ "0.0" }, wxDefaultPosition, wxDefaultSize, 6, choices_start_PA);
        rowSizer->Add(new wxStaticText(this, wxID_ANY, _L("Starting" + prefix + "value: ")));
        startPaCombo->SetToolTip(_L("Select the starting " + prefix + " value to be used."));
        startPaCombo->SetSelection(0);
        rowSizer->Add(startPaCombo);
        dynamicStartPa.push_back(startPaCombo);

        rowSizer->AddSpacer(15);

        wxComboBox* endPaCombo = new wxComboBox(this, wxID_ANY, wxString{ "0.10" }, wxDefaultPosition, wxDefaultSize, 10, choices_end_PA);
        rowSizer->Add(new wxStaticText(this, wxID_ANY, _L("Ending" + prefix + "value: ")));
        endPaCombo->SetToolTip(_L("Select the ending " + prefix + " value to be used."));
        endPaCombo->SetSelection(0);
        rowSizer->Add(endPaCombo);
        dynamicEndPa.push_back(endPaCombo);

        rowSizer->AddSpacer(15);

        wxComboBox* paIncrementCombo = new wxComboBox(this, wxID_ANY, wxString{ "0.005" }, wxDefaultPosition, wxDefaultSize, 8, choices_increment_PA);
        rowSizer->Add(new wxStaticText(this, wxID_ANY, _L(prefix + "increments: ")));
        paIncrementCombo->SetToolTip(_L("Select the " + prefix + " increment amount."));
        paIncrementCombo->SetSelection(3);
        rowSizer->Add(paIncrementCombo);
        dynamicPaIncrement.push_back(paIncrementCombo);

        rowSizer->AddSpacer(15);

        wxComboBox* erPaCombo = new wxComboBox(this, wxID_ANY, wxString{ "InternalInfill" }, wxDefaultPosition, wxDefaultSize, 15, choices_extrusion_role);
        rowSizer->Add(new wxStaticText(this, wxID_ANY, _L("Extrusion role: ")));
        erPaCombo->SetToolTip(_L("Select the extrusion role you want to generate a calibration for"));
        erPaCombo->SetSelection(0);
        rowSizer->Add(erPaCombo);
        dynamicExtrusionRole.push_back(erPaCombo);

        if (prefix == " PA ") {//klipper only feature ?
            rowSizer->AddSpacer(15);
            wxCheckBox* enableST = new wxCheckBox(this, wxID_ANY, _L(""), wxDefaultPosition, wxDefaultSize);
            enableST->SetToolTip(_L("Generate smooth time values"));
            enableST->SetValue(false);
            rowSizer->Add(new wxStaticText(this, wxID_ANY, _L("Smooth time: ")));
            rowSizer->Add(enableST);
            dynamicEnableST.push_back(enableST);
        }

        parent_sizer->Add(rowSizer, 0, wxALL, 5);
        dynamicRowcount.push_back(rowSizer);
    }
}

void CalibrationPressureAdvDialog::on_row_change(wxCommandEvent& event) {
    int new_test_count = wxAtoi(nbRuns->GetValue());

    wxSize auto_size = GetSize();
    //wxSize auto_size = DoGetBestSize();

    if (new_test_count > currentTestCount) {
        create_row_controls(dynamicSizer, new_test_count - currentTestCount);
    } else if (new_test_count < currentTestCount) {
        for (int i = currentTestCount - 1; i >= new_test_count; --i) {
            wxBoxSizer* row = dynamicRowcount.back();
            dynamicSizer->Detach(row);
            row->Clear(true);
            delete row;
            dynamicRowcount.pop_back();
            dynamicFirstPa.pop_back();
            dynamicStartPa.pop_back();
            dynamicEndPa.pop_back();
            dynamicPaIncrement.pop_back();
            dynamicExtrusionRole.pop_back();
            dynamicEnableST.pop_back();
        }
    }

    currentTestCount = new_test_count;
    dynamicSizer->Layout();
    this->Fit();
    
    //this->SetSize(1600,600);
    this->SetSize(auto_size); //makes GUI flash on updating

}


} // namespace GUI
} // namespace Slic3r
#pragma optimize("", on)