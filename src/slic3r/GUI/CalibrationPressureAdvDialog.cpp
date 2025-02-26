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
//#include "Jobs/job.hpp" 2.7 requirement?
#include <unordered_map>

#pragma optimize("", off)

#undef NDEBUG
#include <assert.h>

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
//improvement if users have milti toolheads/swapping create a custom command for setting PA, add in welcome page for them to add this custom command for PA into filament notes-custom variables
//  since they will most likely have a klipper macro for chaning toolheads/doing other stuff.
//BUG: some localization issues results in broken PA calculations since ',' and '.' get swapped. this is because of manual text entry as strings.

/*FlowRole string_to_flow_role(const std::string& role_str) {
    static std::unordered_map<std::string, FlowRole> role_map = {
        {"InternalInfill", FlowRole::frInfill},
        {"BridgeInfill", FlowRole::frSupportMaterialInterface},                     // special calc required
        {"ExternalPerimeter", FlowRole::frExternalPerimeter},
        {"GapFill", FlowRole::frSupportMaterialInterface},                          // special calc required
        {"InternalBridgeInfill", FlowRole::frSupportMaterialInterface},             // special calc required
        {"Ironing", FlowRole::frSupportMaterialInterface},                          // special calc required
        {"OverhangPerimeter", FlowRole::frExternalPerimeter},                       //overhangs use the same flow config as external perimeter TODO: confirm this.
        {"Perimeter", FlowRole::frPerimeter},
        {"SolidInfill", FlowRole::frSolidInfill},
        {"SupportMaterial", FlowRole::frSupportMaterial},
        {"SupportMaterialInterface", FlowRole::frSupportMaterialInterface},
        {"ThinWall", FlowRole::frSupportMaterialInterface},                         // special calc required
        {"TopSolidInfill", FlowRole::frTopSolidInfill},
        {"FirstLayer", FlowRole::frSupportMaterialInterface}                        // special calc required
    };

    return role_map[role_str];
}

ExtrusionRole string_to_er_role(const std::string& role_str) {
    static std::unordered_map<std::string, ExtrusionRole> role_map = {
        {"InternalInfill", ExtrusionRole::erInternalInfill},
        {"BridgeInfill", ExtrusionRole::erBridgeInfill},
        {"ExternalPerimeter", ExtrusionRole::erExternalPerimeter},
        {"GapFill", ExtrusionRole::erGapFill},
        {"InternalBridgeInfill", ExtrusionRole::erInternalBridgeInfill},
        {"Ironing", ExtrusionRole::erIroning},
        {"OverhangPerimeter", ExtrusionRole::erOverhangPerimeter},
        {"Perimeter", ExtrusionRole::erPerimeter},
        {"SolidInfill", ExtrusionRole::erSolidInfill},
        {"SupportMaterial", ExtrusionRole::erSupportMaterial},
        {"SupportMaterialInterface", ExtrusionRole::erSupportMaterialInterface},
        {"ThinWall", ExtrusionRole::erThinWall},
        {"TopSolidInfill", ExtrusionRole::erTopSolidInfill},
        {"FirstLayer", ExtrusionRole::erCustom}
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
    {"OverhangPerimeter", "overhangs_width"},//special calc required, TODO:find out where/how this is calculated 'overhangs_width' is not the same width config as others, it considers this value when calculating flow
    {"Perimeter", "perimeter_extrusion_width"},
    {"SolidInfill", "solid_infill_extrusion_width"},
    {"SupportMaterial", "support_material_extrusion_width"},// support material layer_height can go up/down depending on config.
    {"SupportMaterialInterface", "support_material_extrusion_width"},//SupportMaterialInterface and SupportMaterialInterface shares same width calculations?
    {"ThinWall", "thin_walls_min_width"},//not fully suported
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
    {"ThinWall", "thin_walls_acceleration"},
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
    double first_layer_spacing = full_print_config.get_abs_value("first_layer_extrusion_spacing", nozzle_diameter);
    double first_layer_flow_ratio = full_print_config.get_computed_value("first_layer_flow_ratio");
    double first_layer_size_compensation = full_print_config.get_computed_value("first_layer_size_compensation");
    double infill_every_layers = full_print_config.get_computed_value("infill_every_layers");
    double support_material_layer_height = full_print_config.get_computed_value("support_material_layer_height");

    double base_layer_height = full_print_config.get_computed_value("layer_height");
    double er_width = full_print_config.get_abs_value("solid_infill_extrusion_width", nozzle_diameter);
    double er_accel = full_print_config.get_computed_value("solid_infill_acceleration");
    double er_speed = full_print_config.get_computed_value("solid_infill_speed");
    double er_spacing = full_print_config.get_abs_value("external_perimeter_extrusion_spacing",nozzle_diameter);
    double thin_walls_min_width = full_print_config.get_abs_value("thin_walls_min_width", nozzle_diameter);

    double default_er_width = full_print_config.get_abs_value("extrusion_width", nozzle_diameter);
    double default_er_speed = full_print_config.get_computed_value("default_speed");
    double default_er_accel = full_print_config.get_computed_value("default_acceleration");
    double default_er_spacing = full_print_config.get_abs_value("extrusion_spacing", nozzle_diameter);
    double spacing_ratio = full_print_config.get_computed_value("perimeter_overlap");
    double spacing_ratio_external = full_print_config.get_computed_value("external_perimeter_overlap");
    double filament_max_overlap = full_print_config.get_computed_value("filament_max_overlap",0);//maybe check for extruderID ?
    const int extruder_count = wxGetApp().extruders_edited_cnt();
    double combined_layer_height = infill_every_layers * base_layer_height;
    double min_layer_height = full_print_config.get_computed_value("min_layer_height", extruder_count);
    double max_layer_height = full_print_config.get_computed_value("max_layer_height", extruder_count);

    bool infill_dense = full_print_config.get_bool("infill_dense");
    if (combined_layer_height > nozzle_diameter){
        combined_layer_height = nozzle_diameter;
    }
    Flow first_layer_flow = Flow::new_from_config(FlowRole::frPerimeter, *print_config, nozzle_diameter, first_layer_height, 1.f, true);
    Flow base_flow = Flow::new_from_config(FlowRole::frExternalPerimeter, *print_config, nozzle_diameter, base_layer_height, filament_max_overlap, false);// used for switch statement, not fully coded yet.
    double default_first_layer_width = first_layer_flow.width();
    double default_first_layer_spacing = first_layer_flow.spacing();

    bool defaults_broken = false;
    if(default_er_width == 0 || default_er_spacing == 0){//if their default value config is broken fix it :)
        Flow broken_config_flow = Flow::new_from_config(FlowRole::frExternalPerimeter, *print_config, nozzle_diameter, base_layer_height, spacing_ratio_external, false);
        //default_er_width = broken_config_flow.width()
        //default_er_spacing = broken_config_flow.spacing();// might work? or is it better to use nozzle diameter if config is broken?

        default_er_width = nozzle_diameter;
        default_er_spacing = default_er_width - base_layer_height * float(1. - 0.25 * PI) * spacing_ratio_external; //rounded_rectangle_extrusion_spacing
        defaults_broken = true;
    }
    //what if defaults broken/not set for speed/accell too??


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


    int count_numbers = 0;
    int count_borders = 0;
    std::vector<Eigen::Vector3d> bend_90_positions;
    std::vector<Eigen::Vector3d> number_positions;

    for (int id_item = 0; id_item < currentTestCount; id_item++) {

        count_numbers = 0;
        count_borders = 0;
        bend_90_positions.clear();
        number_positions.clear();
        
        auto pa_result = calc_PA_values(id_item);
        std::vector<double> pa_values = pa_result.first;
        int count_increments = pa_result.second;
        extrusion_role = dynamicExtrusionRole[id_item]->GetValue().ToStdString();
        if (extrusion_role == "SupportMaterial" && support_material_layer_height != 0){
            combined_layer_height = support_material_layer_height;
        }
        else if (extrusion_role == "InternalInfill" && infill_dense == false && infill_every_layers > 1){
            combined_layer_height = infill_every_layers * base_layer_height;
        }

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
            /*bool enable_switch = false;
            if(enable_switch == true){//still needs work :)
            for (const std::string& role_str : choice_extrusion_role) {
                ExtrusionRole extrusion_role = string_to_er_role(role_str);
                FlowRole flow_role = string_to_flow_role(role_str);
                double modified_layer_height = base_layer_height;
                if (infill_every_layers > 1 && role_str == "InternalInfill" && infill_dense == false){
                    modified_layer_height = combined_layer_height;
                }
                else if (role_str == "SupportMaterial"){//this one might be tricky to do, since supports layerheight can go up/down based on config. maybe load 3 90_bend models for supports with low,high, middle layer heights
                    if (support_material_layer_height == 0){
                        double average_layer_height = (min_layer_height + max_layer_height) / 2;
                        modified_layer_height = average_layer_height;
                    }
                    else{
                        modified_layer_height = support_material_layer_height;
                    }
                }
                else if (role_str == "FirstLayer"){
                    modified_layer_height = first_layer_height;
                }

                switch (extrusion_role) {
                    case ExtrusionRole::erInternalInfill:
                        base_flow = Flow::new_from_config(flow_role, *print_config, nozzle_diameter, modified_layer_height, filament_max_overlap, false);
                        break;
                    case ExtrusionRole::erBridgeInfill:// this will be tricky because bridges don't get any "layersquish" so the 90_bend model will have to have "empty" layers to help simulate a bridge
                        base_flow = Flow::new_from_width(float width, float nozzle_diameter, float height, float spacing_ratio, bool bridge = false);
                        //base_flow = Flow::new_from_config(flow_role, *print_config, nozzle_diameter, base_layer_height, filament_max_overlap, false);
                        break;
                    case ExtrusionRole::erExternalPerimeter:
                        base_flow = Flow::new_from_config(flow_role, *print_config, nozzle_diameter, base_layer_height, filament_max_overlap, false);
                        break;
                    case ExtrusionRole::erGapFill:// i don't think i can adjust width/spacing for this one. only speed related config. unless i scale the 90_bend model wrong so it DOES get gap fill ? won't work for arachne, or will it ?
                        //base_flow = Flow::new_from_config(flow_role, *print_config, nozzle_diameter, base_layer_height, filament_max_overlap, false);
                        break;
                    case ExtrusionRole::erInternalBridgeInfill:
                        //base_flow = Flow::new_from_config(flow_role, *print_config, nozzle_diameter, modified_layer_height, filament_max_overlap, false);
                        break;
                    case ExtrusionRole::erIroning:
                        //base_flow = Flow::new_from_config(flow_role, *print_config, nozzle_diameter, modified_layer_height, filament_max_overlap, false);
                        break;
                    case ExtrusionRole::erOverhangPerimeter:
                        base_flow = Flow::new_from_config(flow_role, *print_config, nozzle_diameter, base_layer_height, filament_max_overlap, false);
                        break;
                    case ExtrusionRole::erPerimeter:
                        base_flow = Flow::new_from_config(flow_role, *print_config, nozzle_diameter, base_layer_height, filament_max_overlap, false);
                        break;
                    case ExtrusionRole::erSolidInfill:
                        base_flow = Flow::new_from_config(flow_role, *print_config, nozzle_diameter, base_layer_height, filament_max_overlap, false);
                        break;
                    case ExtrusionRole::erSupportMaterial:
                        base_flow = Flow::new_from_config(flow_role, *print_config, nozzle_diameter, modified_layer_height, filament_max_overlap, false);
                        break;
                    case ExtrusionRole::erSupportMaterialInterface:
                        base_flow = Flow::new_from_config(flow_role, *print_config, nozzle_diameter, base_layer_height, filament_max_overlap, false);
                        break;
                    case ExtrusionRole::erThinWall://maybe scale the 90_bend models down so they get detected as thin_walls_min_width config ? this will result in a "single wall" 90_bend model hmmm..
                        //base_flow = Flow::new_from_config(flow_role, *print_config, nozzle_diameter, base_layer_height, filament_max_overlap, false);
                        break;
                    case ExtrusionRole::erTopSolidInfill:
                        base_flow = Flow::new_from_config(flow_role, *print_config, nozzle_diameter, base_layer_height, filament_max_overlap, false);
                        break;
                    case ExtrusionRole::erCustom://first_layer
                        base_flow = Flow::new_from_config(flow_role, *print_config, nozzle_diameter, modified_layer_height, 1.f, true);
                        break;
                    default:
                        base_flow = Flow::new_from_config(FlowRole::frExternalPerimeter, *print_config, nozzle_diameter, base_layer_height, filament_max_overlap, false);//unsupported roles.
                        continue;
                }
                break;
            }
            }*/

            for (int i = 0; i < sizeof(choice_extrusion_role) / sizeof(choice_extrusion_role[0]); i++) {

                if (er_width_ToOptionKey.find(extrusion_role) != er_width_ToOptionKey.end()) {

                    //look at maps to match speed/width ect to the selected ER role
                    er_width = print_config->get_abs_value(er_width_ToOptionKey[extrusion_role].c_str(), nozzle_diameter);
                    er_speed = full_print_config.get_computed_value(er_speed_ToOptionKey[extrusion_role].c_str());
                    er_accel = full_print_config.get_computed_value(er_accel_ToOptionKey[extrusion_role].c_str());
                    er_spacing = print_config->get_abs_value(er_spacing_ToOptionKey[extrusion_role].c_str(), nozzle_diameter);
                    first_layer_flow = Flow::new_from_config(FlowRole::frPerimeter, *print_config, nozzle_diameter, first_layer_height, 1.f, true);
                    first_layer_width = first_layer_flow.width();
                    first_layer_spacing = first_layer_flow.spacing();

                    //potential BUG if any of the values are 0 everything else would fail, pull the default value too and assign that
                    er_width = (er_width != 0) ? er_width : default_er_width;
                    er_speed = (er_speed != 0) ? er_speed : default_er_speed;
                    er_accel = (er_accel != 0) ? er_accel : default_er_accel;
                    er_spacing = (er_spacing != 0) ? er_spacing : default_er_spacing;
                    first_layer_width = (first_layer_width != 0) ? first_layer_width : first_layer_flow.width();
                    first_layer_spacing = (first_layer_spacing != 0) ? first_layer_spacing : first_layer_flow.spacing();

                    er_width = std::round((er_width * 100 / nozzle_diameter) * 100.0) / 100.0;
                    first_layer_width = std::round((first_layer_width * 100 / nozzle_diameter) * 100.0) / 100.0;

                } else {
                    er_width = print_config->get_abs_value("solid_infill_extrusion_width", nozzle_diameter); //used for gapfill_width/bridges selection. TODO: add the bits for this here since gapfill/bridges need special calculations
                    er_width = std::round((er_width * 100 / nozzle_diameter) * 100.0) / 100.0;
                    first_layer_width = default_first_layer_width;
                    first_layer_spacing = default_first_layer_spacing;
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

        double xyzScale = nozzle_diameter / 0.4;
        double er_width_to_scale = magical_scaling(nozzle_diameter, er_width, filament_max_overlap, spacing_ratio, spacing_ratio_external, base_layer_height, er_spacing);
        //double er_width_to_scale_first_layer = magical_scaling(nozzle_diameter, first_layer_width, filament_max_overlap, spacing_ratio, spacing_ratio_external, first_layer_height, first_layer_spacing);//prob not needed?
        double er_width_to_scale_first_layer_border = first_layer_flow.width() + 3 * first_layer_flow.spacing() + adjustment_factor;//total_width_with_overlap

        if (infill_every_layers > 1 && extrusion_role == "InternalInfill" && infill_dense == false){
            er_width_to_scale = magical_scaling(nozzle_diameter, er_width, filament_max_overlap, spacing_ratio, spacing_ratio_external, combined_layer_height, er_spacing);
        }
        if (extrusion_role == "SupportMaterial" && support_material_layer_height != 0){
            er_width_to_scale = magical_scaling(nozzle_diameter, er_width, filament_max_overlap, spacing_ratio, spacing_ratio_external, combined_layer_height, er_spacing);
        }

        //-- magical scaling 
        pressure_tower.emplace_back();

        double z_scaled_model_height = initial_model_height * (first_layer_height / initial_model_height); //mm
        double xy_scaled_90_bend_x = initial_90_bend_x * er_width_to_scale;             // mm
        double xy_scaled_90_bend_y = initial_90_bend_y * er_width_to_scale;             // mm
        //double first_layer_xy_scaled_90_bend_x = initial_90_bend_x * er_width_to_scale_first_layer; // mm for 90_bend width scaled for first_layer prob not needed?
        //double first_layer_xy_scaled_90_bend_y = initial_90_bend_y * er_width_to_scale_first_layer; // mm for 90_bend width scaled for first_layer prob not needed?
        double xy_scaled_border_x = er_width_to_scale_first_layer_border;               // mm
        double xy_scaled_border_y = er_width_to_scale_first_layer_border;               // mm
        double xy_scaled_number_x = initial_number_x * xyzScale * er_width_to_scale;    // mm
        double xy_scaled_number_y = initial_number_y * xyzScale * er_width_to_scale;    // mm
        double xy_scaled_point_xy = initial_point_xy * xyzScale * er_width_to_scale;    // mm


        double thickness_offset = 0.0;
        double bend_90_y_pos = 0.0;
        double z_scale_90_bend = (first_layer_height + (base_layer_height * 4)) / initial_model_height;//force constant 5 layer height for model
        double z_90_bend_pos = (first_layer_height + (base_layer_height * 4)) / 2;
        double z_scale_others = first_layer_height / initial_model_height;
        double z_others_pos = first_layer_height / 2;
        std::set<std::string> added_roles;

        for (int nb_90_bends = 0; nb_90_bends < count_increments; nb_90_bends++) {
            std::string er_role = extrusion_role;
            double y_offset = 0.0;
            bool role_found = false;

            if (extrusion_role == "Verify") {
                y_offset = 10.0 /* * nozzle_diameter*/;
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

            if (infill_every_layers > 1 && extrusion_role == "InternalInfill" && infill_dense == false){
                er_width_to_scale = magical_scaling(nozzle_diameter, er_width, filament_max_overlap, spacing_ratio, spacing_ratio_external, combined_layer_height, er_spacing);
                z_90_bend_pos = (first_layer_height + (combined_layer_height * 5)) / 2;
                z_scale_90_bend = (first_layer_height + (combined_layer_height * 5)) / initial_model_height;//force constant 6 layer height for model even if combing layers, needed for infill selected role
            }
            if (extrusion_role == "SupportMaterial" && support_material_layer_height != 0 ){
                er_width_to_scale = magical_scaling(nozzle_diameter, er_width, filament_max_overlap, spacing_ratio, spacing_ratio_external, combined_layer_height, er_spacing);
                z_90_bend_pos = (first_layer_height + (combined_layer_height * 5)) / 2;
                z_scale_90_bend = (first_layer_height + (combined_layer_height * 5)) / initial_model_height;//TOFIX: support material layer heights can change if its set to "0"
            }


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
           
            //thickness_offset = ((er_width / 100.0) * nozzle_diameter * xy_scaled_90_bend_y * 4) + (nozzle_diameter * 4);
            //double scaled_thickness_offset = ((xy_scaled_90_bend_x - initial_90_bend_x) + (xy_scaled_90_bend_y - initial_90_bend_y));
            //double real_offset = thickness_offset + scaled_thickness_offset;

            /*thickness_offset = ((er_width / 100.0) * nozzle_diameter * 4) + (nozzle_diameter * 4);

            double scaled_thickness_offset = thickness_offset * (((xy_scaled_90_bend_x / initial_90_bend_x) + (xy_scaled_90_bend_y / initial_90_bend_y)) / 2.0 - 1);
            double real_offset = thickness_offset + scaled_thickness_offset;*/

            bend_90_positions.push_back(modelPosition);
            bend_90_y_pos = modelPosition.y() + thickness_offset;
            //bend_90_y_pos = modelPosition.y() + real_offset;
            
            
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
                        count_numbers++;
                    }
                }

                //TOFIX: on odd/uneven PA_values array results in the boreders being a little too short. this is mainly because i force load in the final end_pa value, cascades into loading extra 90_bend model.

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
                        /*scale*/Vec3d{ scaled_l_border_x_percentage, scaled_lr_border_y_percentage, z_scale_others }, false);count_borders++;         //Left border
                
                add_part(model.objects[objs_idx[id_item]], (boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_pressure" / "pa_border.3mf").string(),
                    Vec3d{ right_border_x_pos , bend_pos_mid.y(), z_others_pos },
                        /*scale*/Vec3d{ scaled_r_border_x_percentage , scaled_lr_border_y_percentage , z_scale_others}, false);count_borders++;        //right border
                

                add_part(model.objects[objs_idx[id_item]], (boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_pressure" / "pa_border.3mf").string(),//on odd number of count_increments the bottom border is not joined to the side borders. fixing this bug will require
                    Vec3d{ tb_border_x_pos , bend_pos_first.y() - (xy_scaled_90_bend_y / 2) - (xy_scaled_border_y / 2) - nozzle_diameter, z_others_pos },                      // adding more if/else statements to add the extra offset and apply this to all other calculations for the border scale and position.
                        /*scale*/Vec3d{ scaled_tb_border_x_percentage , scaled_tb_border_y_percentage, z_scale_others }, false);count_borders++;       //bottom border
                //----------
                add_part(model.objects[objs_idx[id_item]], (boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_pressure" / "pa_border.3mf").string(),
                    Vec3d{ tb_border_x_pos , bend_pos_last.y() + (xy_scaled_90_bend_y / 2) + (xy_scaled_border_y / 2) + nozzle_diameter, z_others_pos },
                        /*scale*/Vec3d{ scaled_tb_border_x_percentage, scaled_tb_border_y_percentage, z_scale_others}, false);count_borders++;         //top border
                //  scale model in percentage from original models xy values!


                if (id_item < 10){ //will break if max test count goes higher. ie currentTestCount
                    add_part(model.objects[objs_idx[id_item]],(boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_pressure" / (std::to_string(id_item) + std::string(".3mf"))).string(),
                        Vec3d{ number_pos_mid.x(), bend_pos_first.y() - (xy_scaled_90_bend_y / 2) + (xy_scaled_number_y / 2), z_scaled_model_height },
                            /*scale*/Vec3d{ xyzScale * er_width_to_scale, xyzScale * er_width_to_scale, z_scale_others * 2 }, false);count_borders++;      // currentTestCount identifer
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
                                /*scale*/Vec3d{ xyzScale * er_width_to_scale, xyzScale + (xyzScale / 2), z_scale_others * 2 }, false);;
                        xpos -= (xy_scaled_number_x / 2);

                    } else if (std::isdigit(pa_values_string[j])) {
                        add_part(model.objects[objs_idx[id_item]],(boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_pressure" / (pa_values_string[j] + std::string(".3mf"))).string(),
                            Vec3d{ xpos, ypos, z_scaled_model_height },// might need to re size the numbers a touch. they get marked as "thin walls"
                                /*scale*/Vec3d{ xyzScale * er_width_to_scale, xyzScale * er_width_to_scale, z_scale_others * 2 }, false);;//TOCHECK: if any numbers get gapfill
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
    //check if setting any config values to 45° breaks it. or it might be the default value for rotation adding part?
    new_print_config.set_key_value("avoid_crossing_perimeters", new ConfigOptionBool(false));
    new_print_config.set_key_value("complete_objects", new ConfigOptionBool(false)); //true is required for multi tests on single plate?
    new_print_config.set_key_value("first_layer_flow_ratio", new ConfigOptionPercent(100));
    new_print_config.set_key_value("first_layer_size_compensation", new ConfigOptionFloat(0));
    new_print_config.set_key_value("xy_inner_size_compensation", new ConfigOptionFloat(0));
    new_print_config.set_key_value("xy_outer_size_compensation", new ConfigOptionFloat(0));
    new_print_config.set_key_value("print_custom_variables", new ConfigOptionString("calibration_print"));//created this as an extra check for when generating gcode to not include "feature_gcode"
                                                                                                          // unless i disable the "generate" button if the keywords are detected in the custom gcode ?


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

        if (extrusion_role == "Verify") {// have to keep it in range
            count_increments = sizeof(choice_extrusion_role) / sizeof(choice_extrusion_role[0]);
        }
        if (extrusion_role == "SupportMaterial" && support_material_layer_height != 0){
            combined_layer_height = support_material_layer_height;
        }
        else if (extrusion_role == "InternalInfill" && infill_dense == false && infill_every_layers > 1){
            combined_layer_height = infill_every_layers * base_layer_height;
        }

        auto last_90_bend_scale = model.objects[objs_idx[id_item]]->volumes[count_increments]->get_scaling_factor();
        Eigen::Vector3d bend_90_mesh = model.objects[objs_idx[id_item]]->volumes[count_increments]->mesh().size();
        double model_height = bend_90_mesh.z() * last_90_bend_scale.z();

        std::string set_first_layer_prefix = (gcfKlipper == flavor) ? "SET_PRESSURE_ADVANCE ADVANCE=" :
                                         (gcfMarlinFirmware == flavor) ? "M900 K" :
                                         (gcfRepRap == flavor) ? "M572 S" : "";
        std::string region_prefix = "{if layer_z <= " + std::to_string(first_layer_height) + "}" + set_first_layer_prefix + std::to_string(first_pa) + "; first layer [layer_z] {endif}";

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
        model.objects[objs_idx[id_item]]->config.set_key_value("perimeter_overlap", new ConfigOptionPercent(100));
        model.objects[objs_idx[id_item]]->config.set_key_value("seam_position", new ConfigOptionEnum<SeamPosition>(spRear)); //spRear or spCost //BUG: should be fixed in 2.7 merge/SS 2.5.59.7, when this is changed the "perimeters & shell" doesn't turn red indicating a change.
        model.objects[objs_idx[id_item]]->config.set_key_value("top_solid_layers", new ConfigOptionInt(0));
        model.objects[objs_idx[id_item]]->config.set_key_value("region_gcode", new ConfigOptionString(region_prefix + " \n" ));

        int style = 2;
        if (extrusion_role != "Verify") {//don't apply layer ranges to the main object for verify mode.
            if(style == 1){//BUG:using this one "works" untill you clear the plate, and it gets stuck in a infinite loop trying to delete nodes, see line 781 of objectDataViewModel.cpp

                if (infill_every_layers > 1 && extrusion_role == "InternalInfill" && infill_dense == false) {
                    ModelConfig range_conf;

                    range_conf.set_key_value("layer_height", new ConfigOptionFloatOrPercent(combined_layer_height, false));
                    model.objects[objs_idx[id_item]]->layer_config_ranges[std::pair<double,double>(first_layer_height, 8)] = range_conf;

                    wxGetApp().obj_list()->layers_editing();
                }
            }
            if(style == 2){
                if (infill_every_layers > 1 && extrusion_role == "InternalInfill" && infill_dense == false) {

                    wxGetApp().obj_list()->layers_editing(id_item);//could prob use this same thing for the unsupported roles since they need a different layer_height/width
                    auto existing_range = model.objects[objs_idx[id_item]]->layer_config_ranges.find(std::pair<double, double>(0.0f, 2.0f));// Find the default existing range {0.0f, 2.0f}

                    if (existing_range != model.objects[objs_idx[id_item]]->layer_config_ranges.end()) {
                        ModelConfig new_range_conf = existing_range->second;

                        new_range_conf.set_key_value("layer_height", new ConfigOptionFloatOrPercent(combined_layer_height, false));
                        model.objects[objs_idx[id_item]]->layer_config_ranges.erase(existing_range);
                        model.objects[objs_idx[id_item]]->layer_config_ranges[std::pair<double, double>(first_layer_height, model_height + first_layer_height)] = new_range_conf;
                    }
                }
                if (extrusion_role == "SupportMaterial" && support_material_layer_height != 0 ){

                    wxGetApp().obj_list()->layers_editing(id_item);//could prob use this same thing for the unsupported roles since they need a different layer_height/width
                    auto existing_range = model.objects[objs_idx[id_item]]->layer_config_ranges.find(std::pair<double, double>(0.0f, 2.0f));// Find the default existing range {0.0f, 2.0f}

                    if (existing_range != model.objects[objs_idx[id_item]]->layer_config_ranges.end()) {
                        ModelConfig new_range_conf = existing_range->second;

                        new_range_conf.set_key_value("layer_height", new ConfigOptionFloatOrPercent(combined_layer_height, false));
                        model.objects[objs_idx[id_item]]->layer_config_ranges.erase(existing_range);
                        model.objects[objs_idx[id_item]]->layer_config_ranges[std::pair<double, double>(first_layer_height, model_height + first_layer_height)] = new_range_conf;
                    }
                }
            }
        }
        size_t num_part = 0;
        const int extra_vol = 1;
        std::set<std::string> added_roles;
        for (ModelObject* part : pressure_tower[id_item]) {//loop though each part/volume and assign the modifers for the 90_bend model.

            std::string er_role = extrusion_role;
            bool role_found = false;
            if (extrusion_role == "Verify") {
                for (size_t i = 0; i < sizeof(choice_extrusion_role) / sizeof(choice_extrusion_role[0]); i++) {
                    er_role = choice_extrusion_role[num_part];
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

            double er_width_to_scale = magical_scaling(nozzle_diameter, er_width, filament_max_overlap, spacing_ratio, spacing_ratio_external, base_layer_height, er_spacing);
            if (infill_every_layers > 1 && extrusion_role == "InternalInfill" && infill_dense == false) {
                er_width_to_scale = magical_scaling(nozzle_diameter, er_width, filament_max_overlap, spacing_ratio, spacing_ratio_external, combined_layer_height, er_spacing);
            }
            if (extrusion_role == "SupportMaterial" && support_material_layer_height != 0 ){
                er_width_to_scale = magical_scaling(nozzle_diameter, er_width, filament_max_overlap, spacing_ratio, spacing_ratio_external, combined_layer_height, er_spacing);
            }
            double er_width_to_scale_first_layer_match_base2 = magical_scaling(nozzle_diameter, er_width, filament_max_overlap, spacing_ratio, spacing_ratio_external, first_layer_height, first_layer_spacing);

            double xy_scaled_90_bend_x = initial_90_bend_x * er_width_to_scale;             // mm
            double xy_scaled_90_bend_y = initial_90_bend_y * er_width_to_scale;             // mm
            double first_layer_xy_scaled_90_bend_x_match = initial_90_bend_x * er_width_to_scale_first_layer_match_base2; // mm for 90_bend width scaled for first_layer to match er role width
            double first_layer_xy_scaled_90_bend_y_match = initial_90_bend_y * er_width_to_scale_first_layer_match_base2; // mm for 90_bend width scaled for first_layer to match er role width



            /*double size_diff_x = xy_scaled_90_bend_x - first_layer_xy_scaled_90_bend_x_match;
            double size_diff_y = xy_scaled_90_bend_y - first_layer_xy_scaled_90_bend_y_match;
            double scaling_factor_x = size_diff_x / xy_scaled_90_bend_x;
            double scaling_factor_y = size_diff_y / xy_scaled_90_bend_y;

            double adjusted_first_layer_width_x2 = er_width * (1 + scaling_factor_x);
            double adjusted_first_layer_width_y2 = er_width * (1 + scaling_factor_y);*/ //check if below is good before deleteing


            double adjusted_first_layer_width_x = er_width * (1 + (xy_scaled_90_bend_x - first_layer_xy_scaled_90_bend_x_match) / xy_scaled_90_bend_x);
            double adjusted_first_layer_width_y = er_width * (1 + (xy_scaled_90_bend_y - first_layer_xy_scaled_90_bend_y_match) / xy_scaled_90_bend_y);
            double adjusted_first_average = (adjusted_first_layer_width_x + adjusted_first_layer_width_y) / 2;

            std::string set_advance_prefix = 
                (gcfKlipper == flavor) ? (smooth_time ? "SET_PRESSURE_ADVANCE SMOOTH_TIME=" : "SET_PRESSURE_ADVANCE ADVANCE=") :
                (gcfMarlinFirmware == flavor) ? "M900 K" :
                (gcfRepRap == flavor) ? "M572 S" : "";


            /// --- custom config ---
            // config for the 90_bend model
            model.objects[objs_idx[id_item]]->volumes[num_part + extra_vol]->config.set_key_value("first_layer_extrusion_width", new ConfigOptionFloatOrPercent(adjusted_first_average, true));//TODO: check if this is now perfect
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
                //will need to adjust layerheight for infill,support, other er roles that needs a different layerheight for verify mode.

                /*ModelConfig range_conf;
                range_conf.set_key_value("layer_height", new ConfigOptionFloatOrPercent(combined_layer_height, false));
                model.objects[objs_idx[id_item]]->volumes[num_part + extra_vol]->layer_config_ranges[std::pair<double,double>(first_layer_height, 8)] = range_conf;

                wxGetApp().obj_list()->layers_editing();
                auto list = wxGetApp().obj_list();*/

                    /*if (infill_every_layers > 1 && extrusion_role == "InternalInfill" && infill_dense == false) {

                        wxGetApp().obj_list()->layers_editing(id_item);//could prob use this same thing for the unsupported roles since they need a different layer_height
                        auto existing_range = model.objects[objs_idx[id_item]]->volumes[num_part + extra_vol]->layer_config_ranges.find(std::pair<double, double>(0.0f, 2.0f));// Find the default existing range {0.0f, 2.0f}

                        if (existing_range != model.objects[objs_idx[id_item]]->volumes[num_part + extra_vol]->layer_config_ranges.end()) {
                            ModelConfig new_range_conf = existing_range->second;

                            new_range_conf.set_key_value("layer_height", new ConfigOptionFloatOrPercent(combined_layer_height, false));
                            model.objects[objs_idx[id_item]]->volumes[num_part + extra_vol]->layer_config_ranges.erase(existing_range);
                            model.objects[objs_idx[id_item]]->volumes[num_part + extra_vol]->layer_config_ranges[std::pair<double, double>(first_layer_height, model_height + first_layer_height)] = new_range_conf;
                        }
                    }
                    else if (extrusion_role == supports /ect){
                    }*/
            }
            else{
                model.objects[objs_idx[id_item]]->volumes[num_part + extra_vol]->config.set_key_value("region_gcode", new ConfigOptionString(set_advance_prefix + std::to_string(pa_values[num_part]) + " ; " + er_role + "\n"));
            }
            num_part++;
            //model.objects[objs_idx[id_item]]->ensure_on_bed(); // put at the correct z (kind of arrange-z)) shouldn't be needed though.
            //model.objects[objs_idx[id_item]]->center_around_origin();
        }


        bool enable_region_gcode_for_numbers = false;// this still needa a bit of work, the first layer ends up getting messed up surfaces. might be a config thing?
        //                                              unless i need to change the numbers height and z position?
        if (enable_region_gcode_for_numbers == true){
            std::string set_advance_prefix = (gcfKlipper == flavor) ? (smooth_time ? "SET_PRESSURE_ADVANCE SMOOTH_TIME=" : "SET_PRESSURE_ADVANCE ADVANCE=") :
                                             (gcfMarlinFirmware == flavor) ? "M900 K" :
                                             (gcfRepRap == flavor) ? "M572 S" : "";

            int pa_index = 0;
            int nb_number = 0;

            while (nb_number < number_positions.size()) {

                // Skip borders or out-of-bounds or odd pa_index
                if ((nb_number >= count_numbers && nb_number < count_numbers + count_borders) ||
                    num_part >= model.objects[objs_idx[id_item]]->volumes.size() || 
                    pa_index % 2 == 1) {
                    if (pa_index % 2 == 1) pa_index++; // increment pa_index to match how numbers are loaded
                    else {
                        num_part++;
                        nb_number++;
                    }
                    continue; // Skip to the next iteration same way numbers get loaded.
                }

                // Apply the PA value to the number set stays inline with 90_bend models
                for (int number_set = 0; number_set < count_numbers; number_set++){
                    model.objects[objs_idx[id_item]]->volumes[number_set + num_part + extra_vol]->config.set_key_value(
                        "region_gcode",
                        new ConfigOptionString(set_advance_prefix + std::to_string(pa_values[pa_index]) + " ; "));
                    
                    nb_number++;
                }
                pa_index++;
                num_part += count_numbers;
            }
        }
    }

    //update plater
    this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->load_config(new_print_config);
    plat->on_config_change(new_print_config);
    this->gui_app->get_tab(Preset::TYPE_PRINTER)->load_config(new_printer_config);
    plat->on_config_change(new_printer_config);
    //enable it later as a safeguard?, shouldn't be needed though.
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
    // BUG:(borders don't slice. with 2+ generated models.) after updating to 2.5.59.11 the generating 2+ models they have "sinking label" have to click "drop to bed" to resolve, clicking "arrange" doesn't fix issue. -fixed
    if (has_to_arrange) {
        //update print config (done at reslice but we need it here)
        if (plat->printer_technology() == ptFFF)
            plat->fff_print().apply(plat->model(), *plat->config());
        std::shared_ptr<ProgressIndicatorStub> fake_statusbar = std::make_shared<ProgressIndicatorStub>();
        ArrangeJob arranger(std::dynamic_pointer_cast<ProgressIndicator>(fake_statusbar), plat);
        arranger.prepare_all();
        arranger.process();
        arranger.finalize();

        /*ArrangeJob arranger(ArrangeJob::Full);
        //Ctl ctl;
        bool canceled = false;   // Example value; adjust as necessary based on user input
        std::exception_ptr e;    // Exception handling

        arranger.prepare_all();
        //arranger.process(ctl);
        arranger.finalize(canceled, e);
        ArrangeJob();*/
        
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
    
    //assert(er_width > 1.0 && "er_width should be above 1.0 as it's a percentage value");
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
    const AppConfig* app_config = get_app_config();

    bool dark_mode = app_config->get_bool("dark_color_mode");// i guss these can be set as global variables?
    std::string user_color = app_config->get("color_dark");
    wxColour text_color("#" + user_color);

    std::string color_background = dark_mode ? "333233" : "ffffff";//dark grey and white. whats the offical dark mode color ?
    wxColour background_color("#" + color_background);

    std::string prefix = (gcfMarlinFirmware == flavor) ? " LA " : ((gcfKlipper == flavor || gcfRepRap == flavor) ? " PA " : "unsupported firmware type");



    if (prefix != "unsupported firmware type") {

        wxPanel* mainPanel = new wxPanel(this, wxID_ANY);
        mainPanel->SetBackgroundColour(background_color);
        mainPanel->Raise();

        // Create a vertical sizer for the panel
        wxBoxSizer* panelSizer = new wxBoxSizer(wxVERTICAL);
        mainPanel->SetSizer(panelSizer);

        // Create the common controls sizer
        wxBoxSizer* commonSizer = new wxBoxSizer(wxHORIZONTAL);

        wxString number_of_runs[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "10" };//setting this any higher will break loading the model for the ID
        nbRuns = new wxComboBox(mainPanel, wxID_ANY, wxString{ "1" }, wxDefaultPosition, wxDefaultSize, 10, number_of_runs, wxCB_READONLY);
        nbRuns->SetToolTip(_L("Select the number of tests to generate, max 6 is recommended due to bed size limits"));
        nbRuns->SetSelection(0);
        nbRuns->Bind(wxEVT_COMBOBOX, &CalibrationPressureAdvDialog::on_row_change, this);

        wxStaticText* text_generate_count = new wxStaticText(mainPanel, wxID_ANY, _L("Number of" + prefix + "tests: "));
        text_generate_count->SetForegroundColour(text_color);
        commonSizer->Add(text_generate_count, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
        commonSizer->Add(nbRuns, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
        
        // Create a button for generating models
        wxButton* generateButton = new wxButton(mainPanel, wxID_FILE1, _L("Generate"));
        generateButton->Bind(wxEVT_BUTTON, &CalibrationPressureAdvDialog::create_geometry, this);
        commonSizer->Add(generateButton, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);

        commonSizer->AddSpacer(50);// move the close button to the right a little, or align it on the far right side?

        wxButton* closeButton = new wxButton(mainPanel, wxID_CLOSE, _L("Close"));
        closeButton->Bind(wxEVT_BUTTON, &CalibrationPressureAdvDialog::close_me_wrapper, this);
        commonSizer->Add(closeButton, 0, wxALL, 5);

        panelSizer->Add(commonSizer, 0, wxALL, 10);
        dynamicSizer = new wxBoxSizer(wxVERTICAL);
        panelSizer->Add(dynamicSizer, 1, wxEXPAND | wxALL, 5);
        buttons->Add(mainPanel, 1, wxEXPAND | wxALL, 10);

        currentTestCount = wxAtoi(nbRuns->GetValue());
        create_row_controls(dynamicSizer, currentTestCount);
    } else {

        wxStaticText* incompatiable_text = new wxStaticText(this, wxID_ANY, _L(prefix));
        incompatiable_text->SetForegroundColour(*wxRED); // Set the text color to red for the incompatiable firmware tpe
        buttons->Add(incompatiable_text);
    }
}

void CalibrationPressureAdvDialog::create_row_controls(wxBoxSizer* parentSizer, int row_count) {

    const AppConfig* app_config = get_app_config();
    bool dark_mode = app_config->get_bool("dark_color_mode");

    std::string user_color_text = app_config->get("color_dark");
    wxColour text_color("#" + user_color_text);

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
    //improvements: pull retraction value and auto select a better suited start/end value?
    //              
    int current_selection = 2;//start selection at ExternalPerimeter

    if (!dynamicExtrusionRole.empty()) {// If there's a previous selection, find the index of the last selected role
        std::string last_selected_er_role = dynamicExtrusionRole[currentTestCount-1]->GetValue().ToStdString();
        for (int j = 0; j < sizeof(choices_extrusion_role) / sizeof(choices_extrusion_role[0]); j++) {
            if (choices_extrusion_role[j] == wxString(last_selected_er_role)) {
                current_selection = j + 1;
                break;
            }
        }
    }
    current_selection = std::min(current_selection, static_cast<int>(sizeof(choices_extrusion_role) / sizeof(choices_extrusion_role[0]) - 1));

    for (int i = 0; i < row_count; i++) {
        wxBoxSizer* rowSizer = new wxBoxSizer(wxHORIZONTAL);

        wxComboBox* firstPaCombo = new wxComboBox(parentSizer->GetContainingWindow(), wxID_ANY, wxString{ choices_first_layerPA[3] }, wxDefaultPosition, wxSize(80, -1), 6, choices_first_layerPA);
        wxStaticText* text_first_l_prefix = new wxStaticText(parentSizer->GetContainingWindow(), wxID_ANY, _L("First Layers" + prefix + "value: "));
        text_first_l_prefix->SetForegroundColour(text_color);
        rowSizer->Add(text_first_l_prefix, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
        firstPaCombo->SetToolTip(_L("Select the" + prefix + "value to be used for the first layer only.\n(this gets added to 'before_layer_gcode' area)"));
        rowSizer->Add(firstPaCombo, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
        dynamicFirstPa.push_back(firstPaCombo);

        rowSizer->AddSpacer(15);

        wxComboBox* startPaCombo = new wxComboBox(parentSizer->GetContainingWindow(), wxID_ANY, wxString{ choices_start_PA[0]}, wxDefaultPosition, wxSize(80, -1), 6, choices_start_PA);
        wxStaticText* text_start_value = new wxStaticText(parentSizer->GetContainingWindow(), wxID_ANY, _L("Start value: "));
        text_start_value->SetForegroundColour(text_color);
        rowSizer->Add(text_start_value, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
        startPaCombo->SetToolTip(_L("Select the starting" + prefix + "value to be used.\n (you can manually type in values!)"));
        startPaCombo->SetSelection(0);
        rowSizer->Add(startPaCombo, 0, wxALIGN_CENTER_VERTICAL);
        dynamicStartPa.push_back(startPaCombo);

        rowSizer->AddSpacer(15);

        wxComboBox* endPaCombo = new wxComboBox(parentSizer->GetContainingWindow(), wxID_ANY, wxString{ choices_end_PA[0] }, wxDefaultPosition, wxSize(80, -1), 10, choices_end_PA);
        wxStaticText* text_end_value = new wxStaticText(parentSizer->GetContainingWindow(), wxID_ANY, _L("End value: "));
        text_end_value->SetForegroundColour(text_color);
        rowSizer->Add(text_end_value, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
        endPaCombo->SetToolTip(_L("Select the ending" + prefix + "value to be used.\n (you can manually type in values!)"));
        endPaCombo->SetSelection(0);
        rowSizer->Add(endPaCombo, 0, wxALIGN_CENTER_VERTICAL);
        dynamicEndPa.push_back(endPaCombo);

        rowSizer->AddSpacer(15);

        wxComboBox* paIncrementCombo = new wxComboBox(parentSizer->GetContainingWindow(), wxID_ANY, wxString{ choices_increment_PA[3] }, wxDefaultPosition, wxSize(80, -1), 8, choices_increment_PA);
        wxStaticText* text_increment = new wxStaticText(parentSizer->GetContainingWindow(), wxID_ANY, _L("Increment by: "));
        text_increment->SetForegroundColour(text_color);
        rowSizer->Add(text_increment, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
        paIncrementCombo->SetToolTip(_L("Select the incremental value.\n (you can manually type in values!)"));
        paIncrementCombo->SetSelection(3);
        rowSizer->Add(paIncrementCombo, 0, wxALIGN_CENTER_VERTICAL);
        dynamicPaIncrement.push_back(paIncrementCombo);

        rowSizer->AddSpacer(15);

        wxComboBox* erPaCombo = new wxComboBox(parentSizer->GetContainingWindow(), wxID_ANY, wxString{ choices_extrusion_role[current_selection] }, wxDefaultPosition, wxSize(200, -1), 15, choices_extrusion_role, wxCB_READONLY);//disable user edit this one :)
        wxStaticText* text_extrusion_role = new wxStaticText(parentSizer->GetContainingWindow(), wxID_ANY, _L("Extrusion role: "));
        text_extrusion_role->SetForegroundColour(text_color);
        rowSizer->Add(text_extrusion_role, 1, wxALIGN_CENTER_VERTICAL | wxALL, 5);
        erPaCombo->SetToolTip(_L("Select the extrusion role you want to generate a calibration for"));
        erPaCombo->SetSelection(current_selection);
        rowSizer->Add(erPaCombo, 0, wxALIGN_CENTER_VERTICAL);
        dynamicExtrusionRole.push_back(erPaCombo);

        // Increment selection for the next row
        current_selection++;
        if (current_selection >= sizeof(choices_extrusion_role) / sizeof(choices_extrusion_role[0])) {
            current_selection = 0; // Wrap around: SetSelection does it's own memory access checks so this shouldn't be needed. but it's a nice safe guard to have.
        }

        if (prefix == " PA ") {//klipper only feature ?
            rowSizer->AddSpacer(15);
            wxCheckBox* enableST = new wxCheckBox(parentSizer->GetContainingWindow(), wxID_ANY, _L(""), wxDefaultPosition, wxDefaultSize);
            wxStaticText* text_smooth_time = new wxStaticText(parentSizer->GetContainingWindow(), wxID_ANY, _L("Smooth time: "));
            text_smooth_time->SetForegroundColour(text_color);
            rowSizer->Add(text_smooth_time, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
            //enableST->SetToolTip(_L("Generate smooth time values"));
            enableST->SetToolTip(_L("This parameter defines the duration over which extruder velocity changes are averaged, helping to smooth out rapid changes in extrusion pressure. Shorter times (e.g., 0.01 seconds) are beneficial for fast printing, while longer times (e.g., 0.4 seconds) are better for slower printing. The default value is 0.04 seconds."));
            enableST->SetValue(false);
            rowSizer->Add(enableST, 1, wxALIGN_CENTER_VERTICAL);
            dynamicEnableST.push_back(enableST);
        }

        parentSizer->Add(rowSizer, 0, wxALL, 2);// change this to make each row have a larger/smaller 'gap' between them
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

void CalibrationPressureAdvDialog::close_me_wrapper(wxCommandEvent& event) {
    this->close_me(event);
}
} // namespace GUI
} // namespace Slic3r
#pragma optimize("", on)