#include "CalibrationPressureAdvDialog.hpp"
#include "I18N.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/AppConfig.hpp"
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
// BUG: output error if first layer height is lower than base layer height
//      this can cause the numbers to not "show up" on the preview because the z scale is calculated wrong.
// ie; first_layer_height=0.1 and base_layer_height =0.20
//BUG: if first/base layer height are both .02 numbers don't show up when sliced. doesn't happen with windows, it did for linux ?
//BUG: first_layer_height and base_layer_height both the same wil mess with numbers scale in z ?
//TODO: does marlin/reprap need other values for this?


/*
FlowRole string_to_flow_role(const std::string& role_str) {
    static std::unordered_map<std::string, FlowRole> role_map = {
        {"InternalInfill", FlowRole::frInfill},
        {"BridgeInfill", FlowRole::frSupportMaterialInterface},                     // special calc required
        {"ExternalPerimeter", FlowRole::frExternalPerimeter},
        {"GapFill", FlowRole::frSupportMaterialInterface},                          // special calc required
        {"InternalBridgeInfill", FlowRole::frSupportMaterialInterface},             // special calc required
        {"Ironing", FlowRole::frSupportMaterialInterface},                          // special calc required
        {"OverhangPerimeter", FlowRole::frSupportMaterialInterface},                // special calc required ?
        {"Perimeter", FlowRole::frPerimeter},
        {"SolidInfill", FlowRole::frSolidInfill},
        {"SupportMaterial", FlowRole::frSupportMaterial},
        {"SupportMaterialInterface", FlowRole::frSupportMaterialInterface},
        {"ThinWall", FlowRole::frSupportMaterialInterface},                         // special calc required
        {"TopSolidInfill", FlowRole::frTopSolidInfill},
        {"FirstLayer", FlowRole::frSupportMaterialInterface}                        // special calc required
    };

    return role_map[role_str];
}*/

void CalibrationPressureAdvDialog::create_geometry(wxCommandEvent& event_args) {
   
    std::string choice_extrusion_role[] = {
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
    //{"Ironing", "top_infill_extrusion_width"},//not fully suported
    {"OverhangPerimeter", "overhangs_width"},
    {"Perimeter", "perimeter_extrusion_width"},
    {"SolidInfill", "solid_infill_extrusion_width"},
    {"SupportMaterial", "support_material_extrusion_width"},
    {"SupportMaterialInterface", "support_material_extrusion_width"},//SupportMaterialInterface and SupportMaterialInterface shares same width calculations?
    {"ThinWall", "external_perimeter_extrusion_width"},//not fully suported
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
    {"ThinWall", "external_perimeter_extrusion_spacing"}, //TOFIX?
    {"TopSolidInfill", "top_infill_extrusion_spacing"},
    {"FirstLayer", "first_layer_extrusion_spacing"}
    };

    std::unordered_map<std::string, std::string> er_speed_ToOptionKey = {
    {"InternalInfill", "infill_speed"},
    {"BridgeInfill", "bridge_speed"},
    {"ExternalPerimeter", "external_perimeter_speed"},
    {"GapFill", "gap_fill_speed"},
    {"InternalBridgeInfill", "internal_bridge_speed"},
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
    for (int i = 0; i < currentTestCount; i++) {
        items.emplace_back((boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_pressure" / "base_plate.3mf").string());
    }
    std::vector<size_t> objs_idx = plat->load_files(items, true, false, false, false);
    assert(objs_idx.size() == currentTestCount);
    const DynamicPrintConfig* print_config = this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->get_config();
    const DynamicPrintConfig* filament_config = this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT)->get_config();
    const DynamicPrintConfig* printer_config = this->gui_app->get_tab(Preset::TYPE_PRINTER)->get_config();

    DynamicPrintConfig full_print_config;
    full_print_config.apply(*print_config);
    full_print_config.apply(*printer_config);
    full_print_config.apply(*filament_config);

    GCodeFlavor flavor = printer_config->option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")->value;
    const ConfigOptionFloats* nozzle_diameter_config = printer_config->option<ConfigOptionFloats>("nozzle_diameter");
    assert(nozzle_diameter_config->size() > 0);
    double nozzle_diameter = nozzle_diameter_config->get_at(0);//get extruderID too?


    double first_layer_height = full_print_config.get_computed_value("first_layer_height");
    double first_layer_width = full_print_config.get_abs_value("first_layer_extrusion_width", nozzle_diameter);
    double first_layer_extrusion_spacing = full_print_config.get_abs_value("first_layer_extrusion_spacing", nozzle_diameter);
    double first_layer_flow_ratio = full_print_config.get_computed_value("first_layer_flow_ratio");
    double first_layer_size_compensation = full_print_config.get_computed_value("first_layer_size_compensation");

    double base_layer_height = full_print_config.get_computed_value("layer_height");
    double er_width = full_print_config.get_abs_value("solid_infill_extrusion_width", nozzle_diameter);
    double er_accel = full_print_config.get_computed_value("solid_infill_acceleration");
    double er_speed = full_print_config.get_computed_value("solid_infill_speed");
    double er_spacing = full_print_config.get_abs_value("external_perimeter_extrusion_spacing",nozzle_diameter);

    double default_er_width = full_print_config.get_abs_value("extrusion_width", nozzle_diameter);
    double default_er_speed = full_print_config.get_computed_value("default_speed");
    double default_er_accel = full_print_config.get_computed_value("default_acceleration");
    double default_er_spacing = full_print_config.get_abs_value("extrusion_spacing", nozzle_diameter);
    double spacing_ratio = full_print_config.get_computed_value("perimeter_overlap");
    double spacing_ratio_external = full_print_config.get_computed_value("external_perimeter_overlap");
    double filament_max_overlap = full_print_config.get_computed_value("filament_max_overlap",0);//maybe check for extruderID ?
    Flow first_layer_flow = Flow::new_from_config(FlowRole::frPerimeter, *print_config, nozzle_diameter, first_layer_height, 1.f, true);


    bool defaults_broken = false;
    if(default_er_width == 0 || default_er_spacing == 0){//if their default value config is broken fix it :)
        //Flow broken_config_flow = Flow::new_from_config(FlowRole::frExternalPerimeter, *print_config, nozzle_diameter, first_layer_height, spacing_ratio_external, false);
        //broken_config_flow.spacing() broken_config_flow.width();
        default_er_width = nozzle_diameter;
        default_er_spacing = default_er_width - base_layer_height * float(1. - 0.25 * PI) * spacing_ratio_external; //rounded_rectangle_extrusion_spacing
        defaults_broken = true;
    }


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
    // some users might have 0.0x nozzle size. if that's the case then they should just need to create the file and it should load. ie; 90_bend_0.450.3mf
    if (decimal_pos != std::string::npos) {
        size_t non_zero_pos = nozzle_diameter_str.find_first_not_of('0', decimal_pos + 2);
        nozzle_diameter_str.erase(non_zero_pos, std::string::npos);
    }*/

    std::string bend_90_nozzle_size_3mf = "90_bend_" + nozzle_diameter_str + ".3mf";
    std::string extrusion_role = dynamicExtrusionRole[0]->GetValue().ToStdString();

    for (int id_item = 0; id_item < currentTestCount; id_item++) {
        
        auto pa_result = calc_PA_values(id_item);
        std::vector<double> pa_values = pa_result.first;
        int count_increments = pa_result.second;
        extrusion_role = dynamicExtrusionRole[id_item]->GetValue().ToStdString();

        /*

        double first_pa = wxAtof(firstPaValue);
        */

        if (extrusion_role == "Verify") {
            //count_increments = 13;
            count_increments = sizeof(choice_extrusion_role) / sizeof(choice_extrusion_role[0]);
            er_width = default_er_width;
            er_spacing = default_er_spacing;
            er_width = er_width * 100 / nozzle_diameter;
            er_width = std::round(er_width * 100.0) / 100.0;

        }
        else{
            /*for (const std::string& role_str : choice_extrusion_role) {
                FlowRole extrusion_role = string_to_flow_role(role_str);

                switch (extrusion_role) {
                    case FlowRole::frExternalPerimeter:
                        base_flow = Flow::new_from_config(extrusion_role, *print_config, nozzle_diameter, base_layer_height, filament_max_overlap, false);
                        break;
                    case FlowRole::frPerimeter:
                        base_flow = Flow::new_from_config(extrusion_role, *print_config, nozzle_diameter, base_layer_height, filament_max_overlap, false);
                        break;
                    case FlowRole::frInfill:
                        base_flow = Flow::new_from_config(extrusion_role, *print_config, nozzle_diameter, base_layer_height, filament_max_overlap, false);
                        break;
                    case FlowRole::frSolidInfill:
                        base_flow = Flow::new_from_config(extrusion_role, *print_config, nozzle_diameter, base_layer_height, filament_max_overlap, false);
                        break;
                    case FlowRole::frTopSolidInfill:
                        base_flow = Flow::new_from_config(extrusion_role, *print_config, nozzle_diameter, base_layer_height, filament_max_overlap, false);
                        break;
                    case FlowRole::frSupportMaterial:
                        base_flow = Flow::new_from_config(extrusion_role, *print_config, nozzle_diameter, base_layer_height, filament_max_overlap, false);
                        break;
                    case FlowRole::frSupportMaterialInterface:
                        base_flow = Flow::new_from_config(extrusion_role, *print_config, nozzle_diameter, base_layer_height, filament_max_overlap, false);
                        break;
                    default:
                        base_flow = Flow::new_from_config(FlowRole::frExternalPerimeter, *print_config, nozzle_diameter, base_layer_height, filament_max_overlap, false);//unsupported roles.
                        continue;
                }
                break;
            }*/

            for (int i = 0; i < sizeof(choice_extrusion_role) / sizeof(choice_extrusion_role[0]); i++) {

                if (er_width_ToOptionKey.find(extrusion_role) != er_width_ToOptionKey.end()) {

                    //look at maps to match speed/width ect to the selected ER role
                    er_width = print_config->get_abs_value(er_width_ToOptionKey[extrusion_role].c_str(), nozzle_diameter);
                    er_speed = full_print_config.get_computed_value(er_speed_ToOptionKey[extrusion_role].c_str());
                    er_accel = full_print_config.get_computed_value(er_accel_ToOptionKey[extrusion_role].c_str());
                    er_spacing = print_config->get_abs_value(er_spacing_ToOptionKey[extrusion_role].c_str(), nozzle_diameter);

                    //potential BUG if any of the values are 0 everything else would fail, pull the default value too and assign that
                    er_width = (er_width != 0) ? er_width : default_er_width;
                    er_speed = (er_speed != 0) ? er_speed : default_er_speed;
                    er_accel = (er_accel != 0) ? er_accel : default_er_accel;
                    er_spacing = (er_spacing != 0) ? er_spacing : default_er_spacing;


                    //TODO: create assert check to see if value is not 0?
                    er_width = std::round((er_width * 100 / nozzle_diameter) * 100.0) / 100.0;

                } else {
                    er_width = print_config->get_abs_value("solid_infill_extrusion_width", nozzle_diameter); //used for gapfill_width/bridges selection. TODO: add the bits for this here since gapfill/bridges need special calculations
                    er_width = std::round((er_width * 100 / nozzle_diameter) * 100.0) / 100.0;
                    //er_width = er_width * 100 / nozzle_diameter;
                    //er_width = std::round(er_width * 100.0) / 100.0;

                }
            }
            if(defaults_broken == true){//if their config is broken fix it :)
                default_er_width = nozzle_diameter;
                default_er_spacing = default_er_width - base_layer_height * float(1. - 0.25 * PI) * spacing_ratio_external; //rounded_rectangle_extrusion_spacing
            }
        }


        //-- magical scaling is done here :)
        //the 90_bend models need to be scaled correctly so there is no 'gapfill' since gapfill will effect results.
        double adjustment_factor = first_layer_flow.width() - first_layer_flow.spacing();
        double total_width_with_overlap = first_layer_flow.width() + 3 * first_layer_flow.spacing() + adjustment_factor;
        double xyzScale = nozzle_diameter / 0.4;
        double er_width_to_scale = magical_scaling(nozzle_diameter, er_width, filament_max_overlap, spacing_ratio, spacing_ratio_external, base_layer_height, er_spacing);
        double er_width_to_scale_first_layer = total_width_with_overlap;
        //TODO: add assert checks on er_width_to_scale ?
        //-- magical scaling 

        pressure_tower.emplace_back();

        double initial_model_height = 0.2;
        //models is created per nozzles size 0.1-2mm walls are nozzle_size*4 thick
        //exported origin point is center of the model in xyz
        double initial_90_bend_x = 42.00;  //size in x= 42.0 mm, model half x=21.0
        double initial_90_bend_y = 21.0;   //size in y= 21.0 mm, model half y=10.5
        double initial_number_x  = 2.0;    //size in x= 2.0 mm , model half x=1.0
        double initial_number_y  = 4.0;    //size in y= 4.0 mm , model half y=2.0
        double initial_border_x  = 1.6;    //size in x= 1.6 mm , model half x=0.8
        double initial_border_y  = 21.0;   //size in y= 21.0 mm, model half y=10.5
        double initial_point_xy  = 0.60;   //size in xy= 0.6mm , model half xy=0.3 'point' model origin is bottom right offset x=0.7 y= -1.7(center of model +x1,y2 for bottom right edges)
        double x_offset_90_bend  = 1.2;    //apex of 90° bend is offset from origin


        double z_scaled_model_height = initial_model_height * (first_layer_height / initial_model_height); //mm
        double xy_scaled_90_bend_x = initial_90_bend_x * er_width_to_scale;             // mm
        double xy_scaled_90_bend_y = initial_90_bend_y * er_width_to_scale;             // mm
        double xy_scaled_border_x = er_width_to_scale_first_layer;                      // mm
        double xy_scaled_border_y = er_width_to_scale_first_layer;                      // mm
        double xy_scaled_number_x = initial_number_x * xyzScale * er_width_to_scale;    // mm
        double xy_scaled_number_y = initial_number_y * xyzScale * er_width_to_scale;    // mm
        double xy_scaled_point_xy = initial_point_xy * xyzScale * er_width_to_scale;    // mm


        double thickness_offset = 0.0;
        double bend_90_y_pos = 0.0;
        double z_scale_90_bend = (first_layer_height + (base_layer_height * 4)) / initial_model_height;//force constant 5 layer height for model
        double z_90_bend_pos = (first_layer_height + (base_layer_height * 4)) / 2;
        double z_scale_others = first_layer_height / initial_model_height;
        double z_others_pos = first_layer_height / 2;


        
        std::vector<Eigen::Vector3d> bend_90_positions;
        std::vector<Eigen::Vector3d> number_positions;
        std::set<std::string> added_roles;

        for (int nb_90_bends = 0; nb_90_bends < count_increments; nb_90_bends++) {
            std::string er_role = extrusion_role;
            double y_offset = 0.0;
            bool role_found = false;

            if (extrusion_role == "Verify") {
                y_offset = 2.0 /* * nozzle_diameter*/;
                for (size_t i = 0; i < sizeof(choice_extrusion_role) / sizeof(choice_extrusion_role[0]); i++) {
                    er_role = choice_extrusion_role[i];
                    if (er_width_ToOptionKey.find(er_role) != er_width_ToOptionKey.end() && added_roles.find(er_role) == added_roles.end()) {
                        added_roles.insert(er_role);
                        role_found = true;
                        break;
                    }
                    else{role_found = false;}//role not found and not currently supported by calibration tool.
                }
            } else {
                role_found = (er_width_ToOptionKey.find(er_role) != er_width_ToOptionKey.end());
            }

            if (role_found == true) {
                er_width =   print_config->get_abs_value(er_width_ToOptionKey[er_role].c_str(), nozzle_diameter);
                er_spacing = print_config->get_abs_value(er_spacing_ToOptionKey[er_role].c_str(), nozzle_diameter);

                er_width = (er_width != 0) ? er_width : default_er_width;//found supported role but it has 0 value, need to give it defaults.
                er_spacing = (er_spacing != 0) ? er_spacing : default_er_spacing;

            } else {
                er_width = default_er_width;
                er_spacing = default_er_spacing;
            }

            er_width = std::round((er_width * 100 / nozzle_diameter) * 100.0) / 100.0;
            er_width_to_scale = magical_scaling(nozzle_diameter, er_width, filament_max_overlap, spacing_ratio, spacing_ratio_external, base_layer_height, er_spacing);

            add_part(model.objects[objs_idx[id_item]], 
                (boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_pressure" / "scaled_with_nozzle_size" / bend_90_nozzle_size_3mf).string(),
                    Vec3d{ x_offset_90_bend, bend_90_y_pos , z_90_bend_pos }, 
                    /*scale*/Vec3d{ er_width_to_scale, er_width_to_scale, z_scale_90_bend }, false);

            pressure_tower.back().push_back(model.objects[objs_idx[id_item]]);
            Eigen::Vector3d modelPosition( x_offset_90_bend, bend_90_y_pos + y_offset , z_90_bend_pos );

            // thickness offset that moves each '90_bend' model in Y
            //thickness_offset = ((er_width / 100) * nozzle_diameter) * 4 + (nozzle_diameter * 2);// pretty tight gap
            //thickness_offset = ((er_width / 100) * nozzle_diameter) * 4 + (nozzle_diameter * 2.5);
            thickness_offset = ((er_width / 100) * nozzle_diameter) * 4 + (nozzle_diameter * 4);// larger gap
            bend_90_positions.push_back(modelPosition);
            bend_90_y_pos = modelPosition.y() + thickness_offset;
            
            
        }
    
        for (int nb_bends = 0; nb_bends < count_increments;nb_bends++){

            if(nb_bends == 1 && extrusion_role != "Verify") {//only load once. this only determines when the borders get loaded, keeping at top of list makes it easier to scroll down to. it can't be '0' since it needs the numbers positions!

                Eigen::Vector3d bend_pos_first = bend_90_positions[0];
                Eigen::Vector3d bend_pos_mid = bend_90_positions[count_increments/2];
                Eigen::Vector3d bend_pos_last = bend_90_positions[count_increments-1];

                Eigen::Vector3d number_pos_first = number_positions[0];
                Eigen::Vector3d number_pos_mid = number_positions[0];
                Eigen::Vector3d number_pos_last = number_positions[0];


                if (!number_positions.empty()) {

                    for (size_t j = 0; j < number_positions.size(); j++) {
                        if (j == number_positions.size() / 2) {
                            number_pos_mid = number_positions[j];
                        }
                        number_pos_last = number_positions[j];
                    }
                }

                double numbers_total_width = (number_pos_last.x() + (xy_scaled_number_x / 2)) - (number_pos_first.x() - (xy_scaled_number_x / 2));// scaled to include gap between end of 90_bend and first number,perfection
                double total_height = (bend_pos_last.y() + (xy_scaled_90_bend_y / 2)) - (bend_pos_first.y() - (xy_scaled_90_bend_y / 2));
                double scalred_r_border_x_mm = numbers_total_width + (nozzle_diameter * 2);
                double left_border_x_offset = (bend_pos_mid.x() - (xy_scaled_90_bend_x/2) - nozzle_diameter + ( xy_scaled_border_x / 2) ) - (bend_pos_mid.x() - (xy_scaled_90_bend_x/2));//left border is positioned slightly inside the 90_bend model this is that distance.
                double tb_total_width_mm = (xy_scaled_border_x - left_border_x_offset) + xy_scaled_90_bend_x + scalred_r_border_x_mm;
                
                double scaled_l_border_x_percentage  = xy_scaled_border_x / initial_border_x;
                double scaled_r_border_x_percentage  = (numbers_total_width + (nozzle_diameter * 2)) / initial_border_x ;
                double scaled_lr_border_y_percentage = (total_height + xy_scaled_border_y) / initial_90_bend_y;
                double scaled_tb_border_x_percentage = tb_total_width_mm  / initial_border_x;
                double scaled_tb_border_y_percentage  = xy_scaled_border_y / initial_border_y;
                
                double left_border_x_pos = bend_pos_mid.x() - (xy_scaled_90_bend_x/2) - nozzle_diameter;
                double right_border_x_pos = bend_pos_mid.x() + (xy_scaled_90_bend_x / 2) + (scalred_r_border_x_mm / 2);

                double left_edge_pos = bend_pos_mid.x() - (xy_scaled_90_bend_x / 2) - xy_scaled_border_x + left_border_x_offset;
                double right_edge_pos = (xy_scaled_90_bend_x / 2) + scalred_r_border_x_mm + bend_pos_mid.x();
                double center = (left_edge_pos + right_edge_pos) / 2;
                double tb_border_x_pos = center;


                add_part(model.objects[objs_idx[id_item]], (boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_pressure" / "pa_border.3mf").string(),
                        Vec3d{ left_border_x_pos , bend_pos_mid.y(), z_others_pos },
                        /*scale*/Vec3d{ scaled_l_border_x_percentage, scaled_lr_border_y_percentage, z_scale_others });         //Left border
                
                add_part(model.objects[objs_idx[id_item]], (boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_pressure" / "pa_border.3mf").string(),
                    Vec3d{ right_border_x_pos , bend_pos_mid.y(), z_others_pos },
                        /*scale*/Vec3d{ scaled_r_border_x_percentage , scaled_lr_border_y_percentage , z_scale_others});        //right border
                

                add_part(model.objects[objs_idx[id_item]], (boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_pressure" / "pa_border.3mf").string(),//on odd number of count_increments the bottom border is not joined to the side borders. fixing this bug will require
                    Vec3d{ tb_border_x_pos , bend_pos_first.y() - (xy_scaled_90_bend_y / 2) - (xy_scaled_border_y / 2) - nozzle_diameter, z_others_pos },                      // adding more if/else statements to add the extra offset and apply this to all other calculations for the border scale and position.
                        /*scale*/Vec3d{ scaled_tb_border_x_percentage , scaled_tb_border_y_percentage, z_scale_others });       //bottom border
                //----------
                add_part(model.objects[objs_idx[id_item]], (boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_pressure" / "pa_border.3mf").string(),
                    Vec3d{ tb_border_x_pos , bend_pos_last.y() + (xy_scaled_90_bend_y / 2) + (xy_scaled_border_y / 2) + nozzle_diameter, z_others_pos },
                        /*scale*/Vec3d{ scaled_tb_border_x_percentage, scaled_tb_border_y_percentage, z_scale_others});         //top border
                //  scale model in percentage from original models xy values!


                if (id_item < 10){ //will break if max test count goes higher.
                    add_part(model.objects[objs_idx[id_item]],(boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_pressure" / (std::to_string(id_item) + std::string(".3mf"))).string(),
                        Vec3d{ number_pos_mid.x(), bend_pos_first.y() - (xy_scaled_90_bend_y / 2) + (xy_scaled_number_y / 2), z_scaled_model_height },
                            /*scale*/Vec3d{ xyzScale * er_width_to_scale, xyzScale * er_width_to_scale, z_scale_others * 2 });      // currentTestCount identifer
                }
            }


            if (extrusion_role != "Verify") {// possible to load the words for each ER role and slice the Er role next to it's 90_bend ?

                if (nb_bends % 2 == 1){
                    continue;// Skip generating every second number
                }

                Eigen::Vector3d bend_90_pos = bend_90_positions[nb_bends];
                std::string pa_values_string = std::to_string(pa_values[nb_bends]);

                double xpos = bend_90_pos.x() + (xy_scaled_90_bend_x / 2) + (xy_scaled_number_x / 2) + nozzle_diameter;
                //double ypos = bend_90_pos.y() + (xy_scaled_90_bend_y / 2) - (xy_scaled_number_y / 2)
                double ypos = bend_90_pos.y() + (xy_scaled_90_bend_y / 2) - (xy_scaled_number_y / 2) + (nozzle_diameter * 3);
                //double ypos = bend_90_pos.y() + (xy_scaled_90_bend_y / 2) - (xy_scaled_number_y / 2) + (er_width / 100 * 2);//TODO: perfect this position so it's centered with the notch on the 90_bend                                                                                                    
                                                                                                     //  will need to calculate that number sizing/positioning to offset it by.

                for (size_t j = 0; j < pa_values_string.length(); ++j) {//not sure how the code will respond with a positive array list? ie ; 100.2 this moves decimal point thus breaking the code from loading model since "..3mf" not a real file

                    if (pa_values_string[j] == '.') {
                        add_part(model.objects[objs_idx[id_item]],(boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_pressure" / "point.3mf").string(),
                            Vec3d{ xpos - (xy_scaled_number_x / 2), ypos /* - xy_scaled_point_xy - xyzScale*/, z_scaled_model_height },// point gets moved to wrong position on all nozzle_sizes, guessing it's exported offset position doesn't get scaled with the model.
                                /*scale*/Vec3d{ xyzScale * er_width_to_scale, xyzScale + (xyzScale / 2), z_scale_others * 2 });
                        xpos -= (xy_scaled_number_x / 2);

                    } else if (std::isdigit(pa_values_string[j])) {
                        add_part(model.objects[objs_idx[id_item]],(boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_pressure" / (pa_values_string[j] + std::string(".3mf"))).string(),
                            Vec3d{ xpos, ypos, z_scaled_model_height },// might need to re size the numbers a touch. they get marked as "thin walls"
                                /*scale*/Vec3d{ xyzScale * er_width_to_scale, xyzScale * er_width_to_scale, z_scale_others * 2 });//TOCHECK: if any numbers get gapfill
                    }

                    Eigen::Vector3d modelPosition(xpos + (xy_scaled_number_x / 2) + nozzle_diameter + (xy_scaled_number_x / 2), ypos, z_scaled_model_height);
                    number_positions.push_back(modelPosition);
                    xpos = modelPosition.x();
                }
            }
        }
    }

    /// --- main config ---
    // => settings that are for object or region should be added to the model (see below, in the for loop), not here
    DynamicPrintConfig new_print_config = *print_config;
    DynamicPrintConfig new_printer_config = *printer_config;
    new_print_config.set_key_value("avoid_crossing_perimeters", new ConfigOptionBool(false));
    new_print_config.set_key_value("complete_objects", new ConfigOptionBool(false)); //true is required for multi tests on single plate?
    new_print_config.set_key_value("first_layer_flow_ratio", new ConfigOptionPercent(100));
    new_print_config.set_key_value("first_layer_size_compensation", new ConfigOptionFloat(0));
    new_print_config.set_key_value("xy_inner_size_compensation", new ConfigOptionFloat(0));
    new_print_config.set_key_value("xy_outer_size_compensation", new ConfigOptionFloat(0));


    //assert(filament_temp_item_name.size() == nb_runs);
    //assert(model.objects.size() == nb_runs);
    assert(objs_idx.size() == currentTestCount);
    for (int id_item = 0; id_item < currentTestCount; id_item++) {

        auto pa_result = calc_PA_values(id_item);
        std::vector<double> pa_values = pa_result.first;
        int count_increments = pa_result.second;

        wxString firstPaValue = dynamicFirstPa[id_item]->GetValue();
        double first_pa = wxAtof(firstPaValue);
        smooth_time = dynamicEnableST[id_item]->GetValue();
        extrusion_role = dynamicExtrusionRole[id_item]->GetValue().ToStdString();



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

        // config modifers for the base model
        model.objects[objs_idx[id_item]]->config.set_key_value("bottom_fill_pattern", new ConfigOptionEnum<InfillPattern>(ipMonotonicWGapFill));// ipConcentric or ipConcentricGapFill ?
        model.objects[objs_idx[id_item]]->config.set_key_value("thin_walls", new ConfigOptionBool(true));
        model.objects[objs_idx[id_item]]->config.set_key_value("bottom_solid_layers", new ConfigOptionInt(1));
        model.objects[objs_idx[id_item]]->config.set_key_value("brim_width", new ConfigOptionFloat(0));
        model.objects[objs_idx[id_item]]->config.set_key_value("external_perimeter_overlap", new ConfigOptionPercent(100));
        model.objects[objs_idx[id_item]]->config.set_key_value("fill_density", new ConfigOptionPercent(0));
        model.objects[objs_idx[id_item]]->config.set_key_value("gap_fill_enabled", new ConfigOptionBool(true)); //should be false?, enabled for testing
        model.objects[objs_idx[id_item]]->config.set_key_value("min_width_top_surface", new ConfigOptionFloatOrPercent(0.0,false));
        model.objects[objs_idx[id_item]]->config.set_key_value("only_one_perimeter_top", new ConfigOptionBool(false));
        model.objects[objs_idx[id_item]]->config.set_key_value("only_one_perimeter_first_layer", new ConfigOptionBool(false));//, if borderers - right are scaled correctly there shouldn't be any gap fill in them. it would be nice to keep the *4 extrusion lines for the borders only.
                                                                                                                              //  can this be applied to the right border only ?
        model.objects[objs_idx[id_item]]->config.set_key_value("perimeter_overlap", new ConfigOptionPercent(100));
        model.objects[objs_idx[id_item]]->config.set_key_value("seam_position", new ConfigOptionEnum<SeamPosition>(spRear)); //spRear or spCost //BUG: should be fixed in 2.7 merge/SS 2.5.59.7, when this is changed the "perimeters & shell" doesn't turn red indicating a change.
        model.objects[objs_idx[id_item]]->config.set_key_value("top_solid_layers", new ConfigOptionInt(0));

        size_t num_part = 0;
        const int extra_vol = 1;
        std::set<std::string> added_roles;
        for (ModelObject* part : pressure_tower[id_item]) {//loop though each part/volume and assign the modifers for the 90_bend model.

            std::string er_role = extrusion_role;
            bool role_found = false;
            if (extrusion_role == "Verify") {
                for (size_t i = 0; i < sizeof(choice_extrusion_role) / sizeof(choice_extrusion_role[0]); i++) {
                    er_role = choice_extrusion_role[i];
                    if (er_width_ToOptionKey.find(er_role) != er_width_ToOptionKey.end() && added_roles.find(er_role) == added_roles.end()) {
                        //er_role = er_role;
                        added_roles.insert(er_role);
                        role_found = true;
                        break;
                    }
                    else{role_found = false;}//role not found and not currently supported by calibration tool.
                }
            } else {
                role_found = (er_width_ToOptionKey.find(er_role) != er_width_ToOptionKey.end());
            }

            if (role_found == true /*&& defaults_broken == false*/) {
                er_width = print_config->get_abs_value(er_width_ToOptionKey[er_role].c_str(), nozzle_diameter);
                er_speed = full_print_config.get_computed_value(er_speed_ToOptionKey[er_role].c_str(), nozzle_diameter);
                er_accel = full_print_config.get_computed_value(er_accel_ToOptionKey[er_role].c_str(), nozzle_diameter);
                er_spacing = print_config->get_abs_value(er_spacing_ToOptionKey[er_role].c_str(), nozzle_diameter);

                er_width = (er_width != 0) ? er_width : default_er_width;
                er_speed = (er_speed != 0) ? er_speed : default_er_speed;
                er_accel = (er_accel != 0) ? er_accel : default_er_accel;
                er_spacing = (er_spacing != 0) ? er_spacing : default_er_spacing;


            } else {
                //instead of loading defaults for everything only load defaults for broken/unsupported values.
                er_width = default_er_width;
                er_speed = default_er_speed;
                er_accel = default_er_accel;
                er_spacing = default_er_spacing;
                
                
                //er_width = print_config->get_abs_value(er_width_ToOptionKey[er_role].c_str(), nozzle_diameter);
                er_speed = full_print_config.get_computed_value(er_speed_ToOptionKey[er_role].c_str(), nozzle_diameter);
                er_accel = full_print_config.get_computed_value(er_accel_ToOptionKey[er_role].c_str(), nozzle_diameter);
                //er_spacing = print_config->get_abs_value(er_spacing_ToOptionKey[er_role].c_str(), nozzle_diameter);

                //er_width = (er_width != 0) ? er_width : default_er_width;
                er_speed = (er_speed != 0) ? er_speed : default_er_speed;
                er_accel = (er_accel != 0) ? er_accel : default_er_accel;
                //er_spacing = (er_spacing != 0) ? er_spacing : default_er_spacing;

                er_role = "defaults for " + er_role + " width spacing";
            }
            er_width = std::round((er_width * 100 / nozzle_diameter) * 100.0) / 100.0;

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

            /// --- custom config ---
            // config for the 90_bend model

            model.objects[objs_idx[id_item]]->volumes[num_part + extra_vol]->config.set_key_value("first_layer_extrusion_width", new ConfigOptionFloatOrPercent(er_width, true));// need to adjust this to remove the first_layer gap fill for 90 bend models.
            model.objects[objs_idx[id_item]]->volumes[num_part + extra_vol]->config.set_key_value("first_layer_extrusion_spacing", new ConfigOptionFloatOrPercent(er_spacing, false));// ^^ not getting applied to model on slicing
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
                //new_printer_config.set_key_value("before_layer_gcode", new ConfigOptionString(std::string("{if layer_num == 0} ") + set_advance_prefix + std::to_string(first_pa) + " {endif}"));
            }
            else{//add '\n' in?
                model.objects[objs_idx[id_item]]->volumes[num_part + extra_vol]->config.set_key_value("region_gcode", new ConfigOptionString(set_advance_prefix + std::to_string(pa_values[num_part]) + " ; " + er_role ));
                //new_printer_config.set_key_value("before_layer_gcode", new ConfigOptionString(std::string("{if layer_num == 0} ") + set_advance_prefix + std::to_string(first_pa) + " {endif}"));// TOFIX:  if multi tests need to adjust this to add syntax for volumes.
            }
            new_printer_config.set_key_value("before_layer_gcode", new ConfigOptionString(std::string("{if layer_num == 0} ") + set_advance_prefix + std::to_string(first_pa) + " {endif}"));// TOFIX:  if multi tests need to adjust this to add syntax for volumes.
            num_part++;
            //model.objects[objs_idx[id_item]]->ensure_on_bed(); // put at the correct z (kind of arrange-z))
            //model.objects[objs_idx[id_item]]->center_around_origin();
        }
    }

    //update plater
    this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->load_config(new_print_config);
    plat->on_config_change(new_print_config);
    this->gui_app->get_tab(Preset::TYPE_PRINTER)->load_config(new_printer_config);
    plat->on_config_change(new_printer_config);
    //enable it later as a safeguard, shouldn't be needed though.
    //for (size_t obj_idx : objs_idx) { model.objects[obj_idx]->ensure_on_bed(); } // put at the correct z (kind of arrange-z))
    //for (size_t obj_idx : objs_idx) { model.objects[obj_idx]->center_around_origin();}
    plat->changed_objects(objs_idx);
    this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->update_dirty();
    this->gui_app->get_tab(Preset::TYPE_PRINTER)->update_dirty();
    plat->is_preview_shown();
    //update everything, easier to code.
    ObjectList* obj = this->gui_app->obj_list();
    obj->update_after_undo_redo();

    // arrange if needed, after new settings, to take them into account
    // BUG:(borders don't slice. with 2+ generated models.) after updating to 2.5.59.11 the generating 2+ models they have "sinking label" have to click "drop to bed" to resolve, clicking "arrange" doesn't fix issue.
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

double CalibrationPressureAdvDialog::magical_scaling(double nozzle_diameter, double er_width, double filament_max_overlap, double spacing_ratio, double spacing_ratio_external, double base_layer_height, double er_spacing) {
    
    double xyzScale = nozzle_diameter / 0.4;
    double er_width_decimal = er_width * nozzle_diameter / 100.0;//models are generated to be default width of x4 lines for the walls ie; 0.4mm nozzle is 1.6mm thick walls + extra for ER role widths
    double er_width_to_scale = 1.0;
    double overlap_ratio = 1;
    if (filament_max_overlap) {
        overlap_ratio = filament_max_overlap;
    }

    spacing_ratio = std::min(overlap_ratio * 0.5f, spacing_ratio_external / 2.0);
    double new_scale_spacing = er_width_decimal - base_layer_height * float(1.0 - 0.25 * PI) * spacing_ratio;
    double spacing_value = std::round((new_scale_spacing / nozzle_diameter) * 100); //spacing_value = Round((Spacing / Max Nozzle Diameter) * 100)
    er_spacing = (std::round(spacing_value * 10000) / 10000) * 0.01;


    if (xyzScale > 4) {
        er_width_to_scale = 1.0;
    } else {
        er_width_to_scale = er_spacing - (nozzle_diameter / 2 * 0.01);//need to scale slightly under to help with models being correct TODO: test more configurations of nozzle sizes/layer heights
        //if use has the 'wrong' min layer height for a nozzle size, the model will get filled with "gapfill" not a normal extrusion, need to test more for what variables 'break' it                          
    }

    return er_width_to_scale;
}

void CalibrationPressureAdvDialog::create_buttons(wxStdDialogButtonSizer* buttons) {
    const DynamicPrintConfig* printer_config = this->gui_app->get_tab(Preset::TYPE_PRINTER)->get_config();
    GCodeFlavor flavor = printer_config->option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")->value;

    std::string prefix = (gcfMarlinFirmware == flavor) ? " LA " : ((gcfKlipper == flavor || gcfRepRap == flavor) ? " PA " : "unsupported firmware type");

    if (prefix != "unsupported firmware type") {

        wxString number_of_runs[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "10" };//setting this any higher will break loading the model for the ID
        nbRuns = new wxComboBox(this, wxID_ANY, wxString{ "1" }, wxDefaultPosition, wxDefaultSize, 10, number_of_runs, wxCB_READONLY);
        nbRuns->SetToolTip(_L("Select the number of tests to generate, max 6 is recommended due to bed size limits"));
        nbRuns->SetSelection(0);
        nbRuns->Bind(wxEVT_COMBOBOX, &CalibrationPressureAdvDialog::on_row_change, this);

        dynamicSizer = new wxBoxSizer(wxVERTICAL);

        wxBoxSizer* commonSizer = new wxBoxSizer(wxHORIZONTAL);
        commonSizer->Add(new wxStaticText(this, wxID_ANY, _L("Number of" + prefix + "tests: ")), 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
        commonSizer->Add(nbRuns, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
        
        // Create a button for generating
        wxButton* generateButton = new wxButton(this, wxID_FILE1, _L("Generate"));
        generateButton->Bind(wxEVT_BUTTON, &CalibrationPressureAdvDialog::create_geometry, this);
        commonSizer->Add(generateButton, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);


        dynamicSizer->Add(commonSizer, 0, wxALL, 10);
        buttons->Add(dynamicSizer, 1, wxEXPAND | wxALL, 5);

        currentTestCount = wxAtoi(nbRuns->GetValue());
        create_row_controls(dynamicSizer, currentTestCount);
    } else {
        buttons->Add(new wxStaticText(this, wxID_ANY, _L(prefix)));
    }
}

void CalibrationPressureAdvDialog::create_row_controls(wxBoxSizer* parent_sizer, int row_count) {
    //
    //wxArrayInt
    //wxArrayDouble
    //wxArrayDouble choices_first_layerPA[] = { 0.025, 0.030, 0.035, 0.040, 0.045, 0.050 };
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
    //improvements: pull retraction value and auto select a better suited start/end value.
    //              on new row creation auto select another ER role
    //              

    for (int i = 0; i < row_count; i++) {
        wxBoxSizer* rowSizer = new wxBoxSizer(wxHORIZONTAL);

        wxComboBox* firstPaCombo = new wxComboBox(this, wxID_ANY, wxString{choices_first_layerPA[3]}, wxDefaultPosition, wxDefaultSize, 6, choices_first_layerPA);
        rowSizer->Add(new wxStaticText(this, wxID_ANY, _L("First Layers" + prefix + "value: ")));
        firstPaCombo->SetToolTip(_L("Select the" + prefix +"value to be used for the first layer only.\n(this gets added to 'before_layer_gcode' area)"));
        firstPaCombo->SetSelection(3);
        rowSizer->Add(firstPaCombo);
        dynamicFirstPa.push_back(firstPaCombo);

        rowSizer->AddSpacer(15);

        wxComboBox* startPaCombo = new wxComboBox(this, wxID_ANY, wxString{ choices_start_PA[0]}, wxDefaultPosition, wxDefaultSize, 6, choices_start_PA);
        rowSizer->Add(new wxStaticText(this, wxID_ANY, _L("Start value: ")));
        startPaCombo->SetToolTip(_L("Select the starting" + prefix + "value to be used.\n (you can manually type in values!)"));
        startPaCombo->SetSelection(0);
        rowSizer->Add(startPaCombo);
        dynamicStartPa.push_back(startPaCombo);

        rowSizer->AddSpacer(15);

        wxComboBox* endPaCombo = new wxComboBox(this, wxID_ANY, wxString{ choices_end_PA[0] }, wxDefaultPosition, wxDefaultSize, 10, choices_end_PA);
        rowSizer->Add(new wxStaticText(this, wxID_ANY, _L("End value: ")));
        endPaCombo->SetToolTip(_L("Select the ending" + prefix + "value to be used.\n (you can manually type in values!)"));
        endPaCombo->SetSelection(0);
        rowSizer->Add(endPaCombo);
        dynamicEndPa.push_back(endPaCombo);

        rowSizer->AddSpacer(15);

        wxComboBox* paIncrementCombo = new wxComboBox(this, wxID_ANY, wxString{ choices_increment_PA[3] }, wxDefaultPosition, wxDefaultSize, 8, choices_increment_PA);
        rowSizer->Add(new wxStaticText(this, wxID_ANY, _L("Increment by: ")));
        paIncrementCombo->SetToolTip(_L("Select the incremental value.\n (you can manually type in values!)"));
        paIncrementCombo->SetSelection(3);
        rowSizer->Add(paIncrementCombo);
        dynamicPaIncrement.push_back(paIncrementCombo);

        rowSizer->AddSpacer(15);

        wxComboBox* erPaCombo = new wxComboBox(this, wxID_ANY, wxString{ choices_extrusion_role[2 + i] }, wxDefaultPosition, wxDefaultSize, 15, choices_extrusion_role, wxCB_READONLY);//disable user edit this one :)
        rowSizer->Add(new wxStaticText(this, wxID_ANY, _L("Extrusion role: ")));
        erPaCombo->SetToolTip(_L("Select the extrusion role you want to generate a calibration for"));
        erPaCombo->SetSelection(2+i);//improvement: pull the last rows value and go up/down from there
        rowSizer->Add(erPaCombo);
        dynamicExtrusionRole.push_back(erPaCombo);

        if (prefix == " PA ") {//klipper only feature ?
            rowSizer->AddSpacer(15);
            wxCheckBox* enableST = new wxCheckBox(this, wxID_ANY, _L(""), wxDefaultPosition, wxDefaultSize);
            rowSizer->Add(new wxStaticText(this, wxID_ANY, _L("Smooth time: ")));
            //enableST->SetToolTip(_L("Generate smooth time values"));
            enableST->SetToolTip(_L("This parameter defines the duration over which extruder velocity changes are averaged, helping to smooth out rapid changes in extrusion pressure. Shorter times (e.g., 0.01 seconds) are beneficial for fast printing, while longer times (e.g., 0.4 seconds) are better for slower printing. The default value is 0.04 seconds."));
            enableST->SetValue(false);
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

std::pair<std::vector<double>, int> CalibrationPressureAdvDialog::calc_PA_values(int id_item) {
    wxString firstPaValue = dynamicFirstPa[id_item]->GetValue();
    wxString startPaValue = dynamicStartPa[id_item]->GetValue();
    wxString endPaValue = dynamicEndPa[id_item]->GetValue();
    wxString paIncrementValue = dynamicPaIncrement[id_item]->GetValue();
    wxString erPaValue = dynamicExtrusionRole[id_item]->GetValue();

    double first_pa;
    firstPaValue.ToDouble(&first_pa);

    double start_pa;
    startPaValue.ToDouble(&start_pa);
    double end_pa;
    endPaValue.ToDouble(&end_pa);
    double pa_increment;
    paIncrementValue.ToDouble(&pa_increment);

    int countincrements = 0;
    int sizeofarray = static_cast<int>((end_pa - start_pa) / pa_increment) + 2;//'+2' needed for odd/even numbers 
    std::vector<double> pa_values(sizeofarray);

    double incremented_pa_value = start_pa;
    while (incremented_pa_value <= end_pa + pa_increment / 2) {
        if (incremented_pa_value <= end_pa) {
            double rounded_pa = std::round(incremented_pa_value * 1000000.0) / 1000000.0;
            pa_values[countincrements] = rounded_pa;
            countincrements++;
            incremented_pa_value += pa_increment;
        } else {
            pa_values[countincrements] = end_pa;
            countincrements++;//failsafe if werid input numbers are provided that can't add the "ending pa" number to the array.
            break;
        }
    }// is there a limit of how many models SS can load ? might be good to set a failsafe just so it won't load 10k+ models...

    return std::make_pair(pa_values, countincrements);
}


} // namespace GUI
} // namespace Slic3r
#pragma optimize("", on)