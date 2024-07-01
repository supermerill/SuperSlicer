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

void CalibrationPressureAdvDialog::create_buttons(wxStdDialogButtonSizer* buttons){

    const DynamicPrintConfig* printer_config = this->gui_app->get_tab(Preset::TYPE_PRINTER)->get_config();
    GCodeFlavor flavor = printer_config->option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")->value; //there a better way to only load the flavor ?

    wxString choices_first_layerPA[] = {
        "0.025",
        "0.030",
        "0.035",
        "0.040",
        "0.045",
        "0.050"
    };
    firstPa = new wxComboBox(this, wxID_ANY, wxString{ "0.040" }, wxDefaultPosition, wxDefaultSize, 6, choices_first_layerPA);
    firstPa->SetToolTip(_L("Select the first layer PA value to be used for the first layer only."));
    firstPa->SetSelection(3);// starting at 0!


    wxString choices_start_PA[] = {
        "0.0",
        "0.010",
        "0.020",
        "0.030",
        "0.040",
        "0.050"
    };
    startPa = new wxComboBox(this, wxID_ANY, wxString{ "0.0" }, wxDefaultPosition, wxDefaultSize, 6, choices_start_PA);
    startPa->SetToolTip(_L("Select the starting PA value to be used."));
    startPa->SetSelection(0);

    wxString choices_end_PA[] = {
        "0.10",
        "0.20",
        "0.30",
        "0.40",
        "0.50",
        "0.60",
        "0.70",
        "0.80",
        "0.90",
        "1.00"
    };
    endPa = new wxComboBox(this, wxID_ANY, wxString{ "0.10" }, wxDefaultPosition, wxDefaultSize, 10, choices_end_PA);
    endPa->SetToolTip(_L("Select the ending PA value to be used."));
    endPa->SetSelection(0);

    wxString choices_increment_PA[] = {
        "0.0010",///1000 hits
        "0.0025",
        "0.0035",
        "0.005", //200 hits
        "0.006",
        "0.007",
        "0.01",//100 hits
        "0.1"//10 hits
    };
    paIncrement = new wxComboBox(this, wxID_ANY, wxString{ "0.0025" }, wxDefaultPosition, wxDefaultSize, 8, choices_increment_PA);
    paIncrement->SetToolTip(_L("Select the PA increment amount."));
    paIncrement->SetSelection(1);

    wxString choices_extrusion_role[] = {
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
    "FirstLayer",
    "Verify"//if this selected, disable/hide other buttons?
            // 'verify' this choice will require the user to manually add in the PA numbers with the GUI from their realworld tests.
            //      the code will then load a 90_bend for each ER role, and give each bend seperate ER speed/width/ect values
            //      when printed and user added in the PA numbers correctly. it should make it easy to spot what ER roles need adjusting.
            //TODO: once the main pressure advance feature is added, this can pull that values and insert here to save the manual adding in the numbers.
    };
    erPa = new wxComboBox(this, wxID_ANY, wxString{ "InternalInfill" }, wxDefaultPosition, wxDefaultSize, 15, choices_extrusion_role);
    erPa->SetToolTip(_L("Select the extrusion role you want to generate a calibration for"));
    erPa->SetSelection(0);


    wxString number_of_runs[] = {"1","2","3","4","5"};
    nbRuns = new wxComboBox(this, wxID_ANY, wxString{ "1" }, wxDefaultPosition, wxDefaultSize, 5, number_of_runs);
    nbRuns->SetToolTip(_L("Select the number of tests to generate, max 2 is reccomended due to bed size limits"));
    nbRuns->SetSelection(0);

    enableST = new wxCheckBox(this, wxID_ANY, _L(""), wxDefaultPosition, wxDefaultSize );
    enableST->SetToolTip(_L("generate smooth time values"));
    enableST->SetValue(false);

    // TODO : add another row of boxes for the 2nd/3rd ect of tests to create, user adjust parameters of new row for the 2nd/3rd test
    //      this will allow multi plate PA tests to be run


    std::string prefix = (gcfMarlinFirmware == flavor) ? " LA " : ((gcfKlipper == flavor || gcfRepRap == flavor) ? " PA " : "unsupported firmware type");

    if (prefix != "unsupported firmware type"){
        buttons->Add(new wxStaticText(this, wxID_ANY, _L("Number of tests: ")));
        buttons->Add(nbRuns);
        buttons->AddSpacer(15);
        buttons->Add(new wxStaticText(this, wxID_ANY, _L("First Layers" + prefix + "value: ")));
        buttons->Add(firstPa);
        buttons->AddSpacer(15);
        buttons->Add(new wxStaticText(this, wxID_ANY, _L("Starting" + prefix + "value: ")));
        buttons->Add(startPa);
        buttons->AddSpacer(15);
        buttons->Add(new wxStaticText(this, wxID_ANY, _L("Ending" + prefix + "value: ")));
        buttons->Add(endPa);
        buttons->AddSpacer(15);
        buttons->Add(new wxStaticText(this, wxID_ANY, _L(prefix + "increments: ")));
        buttons->Add(paIncrement);
        buttons->AddSpacer(15);
        buttons->Add(new wxStaticText(this, wxID_ANY, _L("Extrusion role: ")));
        buttons->Add(erPa);
        if (gcfKlipper == flavor) {
            buttons->AddSpacer(15);
            buttons->Add(new wxStaticText(this, wxID_ANY, _L("Smooth time: ")));
            buttons->Add(enableST);
        }
        buttons->AddSpacer(25);

        wxButton* bt = new wxButton(this, wxID_FILE1, _L("Generate"));
        bt->Bind(wxEVT_BUTTON, &CalibrationPressureAdvDialog::create_geometry, this);
        buttons->Add(bt);
        //this->CenterOnParent();
    }
    else{
        buttons->Add(new wxStaticText(this, wxID_ANY, _L(prefix)));
    }
}

void CalibrationPressureAdvDialog::create_geometry(wxCommandEvent& event_args) {
    /*
    firstPa
    startPa
    endPa
    paIncrement
    erPa
    enableST
    */
    double first_pa, start_pa, end_pa, pa_increment = 0.01;
    bool smooth_time = enableST->IsChecked();
    size_t nb_runs = nbRuns->GetSelection();
    nb_runs=nb_runs+1;
    first_pa = firstPa->GetValue().ToDouble(&first_pa);
    
    if (!firstPa->GetValue().ToDouble(&first_pa)) {
        first_pa = 0.025;
    }
    start_pa = startPa->GetValue().ToDouble(&start_pa);
    if (!startPa->GetValue().ToDouble(&start_pa)) {
        start_pa = 0.0;
    }
    end_pa = endPa->GetValue().ToDouble(&end_pa);
    if (!endPa->GetValue().ToDouble(&end_pa)) {
        end_pa = 1.0;
    }
    pa_increment = paIncrement->GetValue().ToDouble(&pa_increment);
    if (!paIncrement->GetValue().ToDouble(&pa_increment)) {
        pa_increment = 0.05;
    }    

    std::string extrusion_role = erPa->GetValue().ToStdString();
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
    {"InternalBridgeInfill", "bridge_internal_acceleration"},
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

/*
struct ExtrusionSettings {// think a struct is better instead of all the maps ?
    std::string extrusionWidth;
    std::string acceleration;
    std::string speed;
};

    std::unordered_map<std::string, ExtrusionSettings> extrusionRoleToOptionKey = {
        {"InternalInfill", {"infill_extrusion_width", "infill_acceleration", "placeholder"}},
        //{"BridgeInfill", {"placeholder", "bridge_acceleration", "placeholder"}},//special calc required
        {"ExternalPerimeter", {"external_perimeter_extrusion_width", "external_perimeter_acceleration"}},
        //{"GapFill", {"placeholder", "gap_fill_acceleration"}},//special calc required
        //{"InternalBridgeInfill", {"placeholder", "bridge_internal_acceleration"}},//special calc required
        {"Ironing", {"top_infill_extrusion_width", "ironing_acceleration"}},
        {"OverhangPerimeter", {"overhangs_width", "overhangs_acceleration"}},
        {"Perimeter", {"perimeter_extrusion_width", "perimeter_acceleration"}},
        {"SolidInfill", {"solid_infill_extrusion_width", "solid_infill_acceleration"}},
        {"SupportMaterial", {"support_material_extrusion_width", "support_material_acceleration"}},
        {"SupportMaterialInterface", {"support_material_extrusion_width", "support_material_interface_acceleration"}},
        {"ThinWall", {"external_perimeter_extrusion_width", "thin_walls_acceleration"}},
        {"TopSolidInfill", {"top_infill_extrusion_width", "top_solid_infill_acceleration"}}
    };*/
    
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
    for (size_t i = 0; i < nb_runs; i++){
        items.emplace_back((boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_pressure" / "base_plate.3mf").string());
    }
    std::vector<size_t> objs_idx = plat->load_files(items, true, false, false, false);
    assert(objs_idx.size() == nb_runs);
    const DynamicPrintConfig* print_config = this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->get_config();
    const DynamicPrintConfig* filament_config = this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT)->get_config();
    const DynamicPrintConfig* printer_config = this->gui_app->get_tab(Preset::TYPE_PRINTER)->get_config();
    
    // --- scale ---
    //models is created for nozzles from 0.1-2mm walls should be nozzle_size*4 spaced, scale xy model by widths down is futher
    const ConfigOptionFloats* nozzle_diameter_config = printer_config->option<ConfigOptionFloats>("nozzle_diameter");
    assert(nozzle_diameter_config->values.size() > 0);
    double nozzle_diameter = nozzle_diameter_config->values[0];//get extruderID too?
    double first_layer_height = print_config->get_abs_value("first_layer_height", nozzle_diameter);
    double base_layer_height = print_config->get_computed_value("layer_height",0);
    GCodeFlavor flavor = printer_config->option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")->value;
    
    double er_width = print_config->get_abs_value("solid_infill_extrusion_width", nozzle_diameter);
    double er_accel = print_config->get_abs_value("solid_infill_acceleration", nozzle_diameter);
    double er_speed = print_config->get_abs_value("solid_infill_speed", nozzle_diameter);
    double er_spacing = print_config->get_abs_value("external_perimeter_extrusion_spacing",1.0);

    double default_er_width = print_config->get_abs_value("extrusion_width", nozzle_diameter);
    double default_er_speed = print_config->get_abs_value("default_speed", nozzle_diameter);
    double default_er_accel = print_config->get_abs_value("default_acceleration", nozzle_diameter);
    double default_er_spacing = print_config->get_abs_value("extrusion_spacing", nozzle_diameter);
    double spacing_ratio = print_config->get_abs_value("perimeter_overlap",1.0);
    double spacing_ratio_external = print_config->get_abs_value("external_perimeter_overlap",1.0);
    double filament_max_overlap = filament_config->get_computed_value("filament_max_overlap",0);//maybe check for extruderID ?

    if (extrusion_role == "Verify") {
        countincrements = 13;
        er_width = default_er_width;
        er_spacing = default_er_spacing;
        er_width = er_width * 100 / nozzle_diameter;
        er_width = std::round(er_width * 100.0) / 100.0;  // Change number to percentage and round
    }
    else{
        for (int i = 0; i < sizeof(choice_extrusion_role) / sizeof(choice_extrusion_role[0]); i++) {
            
            if (er_width_ToOptionKey.find(extrusion_role) != er_width_ToOptionKey.end()) {

                er_width = print_config->get_abs_value(er_width_ToOptionKey[extrusion_role].c_str(), nozzle_diameter);//look at maps at match speed/width ect to the selecter ER role
                er_speed = print_config->get_abs_value(er_speed_ToOptionKey[extrusion_role].c_str(), nozzle_diameter);//need to load this here??
                er_accel = print_config->get_abs_value(er_accel_ToOptionKey[extrusion_role].c_str(), nozzle_diameter);//need to load this here??
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
                er_width = std::round(er_width * 100.0) / 100.0;  // Change number to percentage and round

            }
        
        }
    }
    

    //-- magical scaling is done here :)
    //the 90_bend models need to be scaled correctly so there is no 'gapfill' since gapfill will effect results.
    double xyzScale = nozzle_diameter / 0.4;
    double er_width_to_scale = magical_scaling(nozzle_diameter,er_width,filament_max_overlap,spacing_ratio,spacing_ratio_external,base_layer_height,er_spacing);
    //-- magical scaling 
    //std::vector < std::vector<ModelObject*>> pressure_tower;

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

    for (size_t id_item = 0; id_item < nb_runs; id_item++) {
        
        //pressure_tower.emplace_back();

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

                    er_width = std::round((print_config->get_abs_value(er_width_ToOptionKey[role].c_str(), nozzle_diameter) * 100 / nozzle_diameter) * 100.0) / 100.0;
                    er_spacing = print_config->get_abs_value(er_spacing_ToOptionKey[role].c_str(), nozzle_diameter);
                    er_width_to_scale = magical_scaling(nozzle_diameter, er_width, filament_max_overlap, spacing_ratio, spacing_ratio_external, base_layer_height, er_spacing);
                    thickness_offset = nozzle_diameter * er_width_to_scale * 2;
                
                    add_part(model.objects[objs_idx[id_item]], 
                            (boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_pressure" / "scaled_with_nozzle_size" / bend_90_nozzle_size_3mf).string(),
                            Vec3d{ -0.8, (initial_90_bend_y/2) * nb_bends , (z_scale_factor/2) }, Vec3d{ er_width_to_scale, er_width_to_scale, z_scale_90_bend });
                    
                    Eigen::Vector3d modelPosition(-0.8, (initial_90_bend_y/2) * nb_bends, (z_scale_factor/2) );
                    bend_90_positions.push_back(modelPosition);
                    nb_bends++;
                }
                else{
                    er_width = std::round((default_er_width * 100 / nozzle_diameter) * 100.0) / 100.0;
                    er_spacing = default_er_spacing;
                    er_width_to_scale = magical_scaling(nozzle_diameter, er_width, filament_max_overlap, spacing_ratio, spacing_ratio_external, base_layer_height, er_spacing);
                    thickness_offset = nozzle_diameter * er_width_to_scale * 2;

                    add_part(model.objects[objs_idx[id_item]], 
                        (boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_pressure" / "scaled_with_nozzle_size" / bend_90_nozzle_size_3mf).string(),
                        Vec3d{ -0.8, (initial_90_bend_y/2) * nb_bends , (z_scale_factor/2) }, Vec3d{ er_width_to_scale, er_width_to_scale, z_scale_90_bend });
                    
                    Eigen::Vector3d modelPosition(-0.8, (initial_90_bend_y/2) * nb_bends, (z_scale_factor/2) );
                    bend_90_positions.push_back(modelPosition);
                    nb_bends++;
                
                }
                
            }
        }
        else{//not verify
            for (int nb_bends = 0; nb_bends < countincrements; nb_bends++){
                //const double magical_transformation_y_pos = 10.47;

                add_part(model.objects[objs_idx[id_item]], 
                        (boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_pressure" / "scaled_with_nozzle_size" / bend_90_nozzle_size_3mf).string(),
                        Vec3d{ -0.8, double(nb_bends) * (thickness_offset*2) *2 , (z_scale_factor/2) }, Vec3d{ er_width_to_scale, er_width_to_scale, z_scale_90_bend });
                
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
                //----------
            }
        //}

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


    /// --- translate ---
    //bool autocenter = gui_app->app_config->get("autocenter") == "1";
    bool has_to_arrange = plat->config()->opt_float("init_z_rotate") != 0;
    if (nb_runs == 1) {
        //model.objects[objs_idx[0]]->translate({ bed_min.x() + bed_size.x() / 2, bed_min.y() + bed_size.y() / 2, zscale_number });
        has_to_arrange = true;
    } else {
        has_to_arrange = true;
    }
    
    /*if (!autocenter) {
        const ConfigOptionPoints* bed_shape = printer_config->option<ConfigOptionPoints>("bed_shape");
        Vec2d bed_size = BoundingBoxf(bed_shape->values).size();
        Vec2d bed_min = BoundingBoxf(bed_shape->values).min;
        model.objects[objs_idx[0]]->translate({ bed_min.x() + bed_size.x() / 2, bed_min.y() + bed_size.y() / 2, 5 * xyzScale - 5 });
    }*/

    /// --- main config, modify object config when possible ---
    DynamicPrintConfig new_print_config = *print_config;
    DynamicPrintConfig new_printer_config = *printer_config;
    new_print_config.set_key_value("brim_width", new ConfigOptionFloat(0));
    new_print_config.set_key_value("complete_objects", new ConfigOptionBool(false)); //true is required for multi tests on single plate.
    new_print_config.set_key_value("gap_fill_enabled", new ConfigOptionBool(true)); //should be false?, enabled for testing
    new_print_config.set_key_value("top_solid_layers", new ConfigOptionInt(0));  //BUG: top layers set to 0 the top layer has a "void" where the top layer would normally be, this might be a config thing that needs to get changed or a slicing bug
    new_print_config.set_key_value("bottom_solid_layers", new ConfigOptionInt(1));
    new_print_config.set_key_value("fill_density", new ConfigOptionPercent(0));
    new_print_config.set_key_value("min_width_top_surface", new ConfigOptionFloatOrPercent(0.0,false));
    new_print_config.set_key_value("bottom_fill_pattern", new ConfigOptionEnum<InfillPattern>(ipMonotonicWGapFill));
    new_print_config.set_key_value("seam_position", new ConfigOptionEnum<SeamPosition>(spRear)); //BUG: should be fixed in 2.7 merge/SS 2.5.59.7, when this is changed the "perimeters & shell" doesn't turn red indicating a change.
    new_print_config.set_key_value("avoid_crossing_perimeters", new ConfigOptionBool(false));
    new_print_config.set_key_value("perimeter_overlap", new ConfigOptionPercent(100));
    new_print_config.set_key_value("external_perimeter_overlap", new ConfigOptionPercent(100));
    new_print_config.set_key_value("per_objects_gcode", new ConfigOptionString(bend_90_nozzle_size_3mf+",")); // this is the model other parts in code will search for and insert the PA/ect numbers

    new_printer_config.set_key_value("before_layer_gcode", new ConfigOptionString("{if layer_num == 0} SET_PRESSURE_ADVANCE ADVANCE=" + std::to_string(first_pa) + " {endif}")); //maybe i can save the existing value into a variable then add to it ?

    assert(filament_temp_item_name.size() == nb_runs);
    assert(model.objects.size() == nb_runs);

    for (size_t i = 0; i < nb_runs; i++) {
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

        //size_t num_part = 0;
        //const int extra_vol = 1;
        //for (ModelObject* part : pressure_tower[i]) {//loop though each part/volume and assign the modifers

        ModelObject *current_obj = model.objects[objs_idx[i]];
        for (int num_part = 1; num_part < countincrements+1; num_part++){

            std::string er_role ="";
            if (extrusion_role == "Verify") {
                er_role = choice_extrusion_role[num_part];
                if (er_width_ToOptionKey.find(er_role) != er_width_ToOptionKey.end()) {

                    er_width = std::round((print_config->get_abs_value(er_width_ToOptionKey[er_role].c_str(), nozzle_diameter) * 100 / nozzle_diameter) * 100.0) / 100.0;
                    er_speed = print_config->get_abs_value(er_speed_ToOptionKey[er_role].c_str(), nozzle_diameter);
                    er_accel = print_config->get_abs_value(er_accel_ToOptionKey[er_role].c_str(), nozzle_diameter);
                }
                else{
                    er_width = std::round((default_er_width * 100 / nozzle_diameter) * 100.0) / 100.0;
                    er_speed = default_er_speed;
                    er_accel = default_er_accel;
                }
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
            //current_obj->volumes[num_part]->config.set_key_value("print_retract_length", new ConfigOptionFloat(retraction_start + num_part * retraction_steps));
            current_obj->volumes[num_part]->config.set_key_value("perimeter_extrusion_width", new ConfigOptionFloatOrPercent(er_width, true));

            /*model.objects[objs_idx[i]]->volumes[num_part + extra_vol]->config.set_key_value("perimeter_extrusion_width", new ConfigOptionFloatOrPercent(er_width, true));
            model.objects[objs_idx[i]]->volumes[num_part + extra_vol]->config.set_key_value("external_perimeter_extrusion_width", new ConfigOptionFloatOrPercent(er_width, true));//TODO: check widths and ect breaks if any values are in mm/percentage
            model.objects[objs_idx[i]]->volumes[num_part + extra_vol]->config.set_key_value("perimeter_speed", new ConfigOptionFloatOrPercent(er_speed, false));
            model.objects[objs_idx[i]]->volumes[num_part + extra_vol]->config.set_key_value("external_perimeter_speed", new ConfigOptionFloatOrPercent(er_speed, false));
            model.objects[objs_idx[i]]->volumes[num_part + extra_vol]->config.set_key_value("gap_fill_speed", new ConfigOptionFloatOrPercent(er_speed, false));
            model.objects[objs_idx[i]]->volumes[num_part + extra_vol]->config.set_key_value("perimeter_acceleration", new ConfigOptionFloatOrPercent(er_accel, false));
            model.objects[objs_idx[i]]->volumes[num_part + extra_vol]->config.set_key_value("external_perimeter_acceleration", new ConfigOptionFloatOrPercent(er_accel, false));
            model.objects[objs_idx[i]]->volumes[num_part + extra_vol]->config.set_key_value("gap_fill_acceleration", new ConfigOptionFloatOrPercent(er_accel, false));*/

           current_obj->volumes[num_part]->config.set_key_value("perimeter_extrusion_width", new ConfigOptionFloatOrPercent(er_width, true));
           current_obj->volumes[num_part]->config.set_key_value("external_perimeter_extrusion_width", new ConfigOptionFloatOrPercent(er_width, true));//TODO: check widths and ect breaks if any values are in mm/percentage
           current_obj->volumes[num_part]->config.set_key_value("perimeter_speed", new ConfigOptionFloatOrPercent(er_speed, false));
           current_obj->volumes[num_part]->config.set_key_value("external_perimeter_speed", new ConfigOptionFloatOrPercent(er_speed, false));
           current_obj->volumes[num_part]->config.set_key_value("gap_fill_speed", new ConfigOptionFloatOrPercent(er_speed, false));
           current_obj->volumes[num_part]->config.set_key_value("perimeter_acceleration", new ConfigOptionFloatOrPercent(er_accel, false));
           current_obj->volumes[num_part]->config.set_key_value("external_perimeter_acceleration", new ConfigOptionFloatOrPercent(er_accel, false));
           current_obj->volumes[num_part]->config.set_key_value("gap_fill_acceleration", new ConfigOptionFloatOrPercent(er_accel, false));

            if (extrusion_role == "Verify") {
                //model.objects[objs_idx[i]]->volumes[num_part + extra_vol]->config.set_key_value("per_objects_gcode", new ConfigOptionString(set_advance_prefix + " ; " + er_role ));//user manual type in values
                current_obj->volumes[num_part]->config.set_key_value("per_objects_gcode", new ConfigOptionString(set_advance_prefix + " ; " + er_role ));//user manual type in values
            }
            else{//add '\n' in?
                //model.objects[objs_idx[i]]->volumes[num_part + extra_vol]->config.set_key_value("per_objects_gcode", new ConfigOptionString(set_advance_prefix + std::to_string(pa_values[num_part]) + " ; " + extrusion_role ));
                current_obj->volumes[num_part]->config.set_key_value("per_objects_gcode", new ConfigOptionString(set_advance_prefix + std::to_string(pa_values[num_part]) + " ; " + extrusion_role ));
            }
        }
    }

    //update plater
    this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->load_config(new_print_config);
    plat->on_config_change(new_print_config);
    this->gui_app->get_tab(Preset::TYPE_PRINTER)->load_config(new_printer_config);
    plat->on_config_change(new_printer_config);
    plat->changed_objects(objs_idx);
    this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->update_dirty();
    this->gui_app->get_tab(Preset::TYPE_PRINTER)->update_dirty();
    plat->is_preview_shown();
    //update everything, easier to code.
    ObjectList* obj = this->gui_app->obj_list();
    obj->update_after_undo_redo();


    // arrange if needed, after new settings, to take them into account
    // BUG:(borders don't slice. with 2+ generated models.) after updating to 2.5.59.11 the generating 2+ models they have "sinking label" have to click "drop to bed" to resolve, clicking "arrange" doesn't fix issue.
    // BUG: disovered manually moving the model in z lettting it drop down, then trying to slice can make it crash - (exception thrown 3073 - gcode.cpp)
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
        //plat->reslice(); //forces a slice of plater.
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

} // namespace GUI
} // namespace Slic3r
#pragma optimize("", on)