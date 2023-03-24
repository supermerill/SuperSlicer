//////////////////////////////////////////
// Api for SuperSlicer scripted widgets:
//
//////// callable Functions //////
//
// -- to print on the console, for debugging --
// void print(string &out)
// void print_float(float)
//
// -- to get the value of real settings --
//  bool  get_bool(string &in key)
//  int   get_int(string &in key)
//    can be used by type int and enum (return the index)
//  float get_float(string &in key)
//    can be used by type float, percent and flaot_or_percent
//  float get_computed_float(string &in key)
//    get the float computed value of the field. Useful if it's a floatOrPercent that is computable.
//  bool  is_percent(string &in key)
//  void  get_string(string &in key, string &out get_val)
//    can be used by type string and enum (return the enum_value, not the label)
//
// -- to set the value of real settings --
//  void set_bool(string &in key, bool new_val)
//  void set_int(string &in key, int new_val)
//    if an enum, it's the index
//  void set_float(string &in key, float new_val)
//    if a float_or_percent, unset the percent flag at the same time
//  void set_percent(string &in key, float new_val)
//    if a float_or_percent, set the percent flag at the same time
//  void set_string(string &in key, string &in new_val))
//    if an enum, it's one of the enum_value
//
//  void back_initial_value(string &in key)
//    revert the setting to the last saved value (same as a click on the reset arrow)
//
//  ask_for_refresh()
//    ask for a OPTNAME_set() if in a OPTNAME_get()
//
//////// Functions to define for each script widget //////// 
//
// note that you can't call set_thing() in an OPTNAME_get(), you can only call these in an OPTNAME_set()
//
// type bool:
//   int OPTNAME_get() 
//      will return 1 if checkd, 0 if unchecked and -1 if half-checked (not all os, will be uncehcked if not available)
//   void OPTNAME_set(bool set)
//
// type int:
//   int OPTNAME_get()
//   void OPTNAME_set(int set)
//
// type float & percent:
//   float OPTNAME_get()
//   void OPTNAME_set(float set)
//
// type float_or_percent:
//   float OPTNAME_get(bool &out is_percent)
//   void OPTNAME_set(float set, bool is_percent)
//
// type string:
//   void OPTNAME_get(string &out get)
//   void OPTNAME_set(string &in set)
//
// type enum:
//   int OPTNAME_get(string &out enum_value)
//      Only the return value is used unless it's out of bounds, then it tries to use the enum_value
//   void OPTNAME_set(string &in set_enum_value, int set_idx)
//
//

//overhangs : quick set/unset like the one in prusalicer

int s_overhangs_get()
{
	if (get_float("overhangs_width_speed") == 0) return 0;
	float width = get_float("overhangs_width");
	bool percent = is_percent("overhangs_width");
	if((percent && width > 50.f) || ((!percent) && width > 0.2f)) return 1;
	return -1;
}

void s_overhangs_set(bool set)
{
	if (set) {
		set_percent("overhangs_width_speed", 55.f);
		float width = get_float("overhangs_width");
		bool percent = is_percent("overhangs_width");
		if((percent && width < 50.f) || ((!percent) && width < 0.2f))
			set_percent("overhangs_width", 75.f);
	} else {
		set_float("overhangs_width_speed", 0.);
	}
}

void s_overhangs_reset(bool set)
{
	back_initial_value("overhangs_width_speed");
	back_initial_value("overhangs_width");
}

// "not thick bridge" like in prusaslicer

float compute_overlap()
{
	float height = get_float("layer_height");
	float width = get_computed_float("solid_infill_extrusion_width");
	if(height <= 0) return 1;
	if(width <= 0) return 1;
	float solid_spacing = (width - height * 0.215);
	float solid_flow = height * solid_spacing;
	float bridge_spacing = sqrt(solid_flow*1.2739);
	float round_val = floor((bridge_spacing / solid_spacing) * 1000. + 0.5) / 1000.;
	return round_val;
}

int s_not_thick_bridge_get()
{
	bool is_set = false;
	get_custom_bool(0,"not_thick_bridge", is_set);
	if(is_set){
		//set other vars
		ask_for_refresh();
		return 1;
	}
	return 0;
}

void s_not_thick_bridge_reset(bool set)
{
	set_custom_bool(0,"not_thick_bridge", false);
	back_initial_value("bridge_type");
	back_initial_value("bridge_overlap");
}

void s_not_thick_bridge_set(bool set)
{
	bool var_set = false;
	get_custom_bool(0,"not_thick_bridge", var_set);
	if (var_set != set) {
		set_custom_bool(0,"not_thick_bridge", set);
	}
	if (set) {
		if (get_int("bridge_type") != 2)
			set_int("bridge_type", 2);
		float overlap = compute_overlap();
		set_float("bridge_overlap", overlap);
		set_float("bridge_overlap_min", overlap);
	} else if (var_set != set) {
		back_initial_value("bridge_type");
		back_initial_value("bridge_overlap");
		back_initial_value("bridge_overlap_min");
	}
}

// seam position
//    spRandom [spNearest] spAligned spRear [spCustom] spCost
// ("Cost-based") ("Random") ("Aligned") ("Rear")
// -> Corners Nearest Random Aligned Rear Custom

//    spRandom spAllRandom [spNearest] spAligned spExtrAligned spRear [spCustom] spCost
// ("Cost-based") ("Scattered") ("Random") ("Aligned") ("Contiguous") ("Rear")
// -> Corners Nearest Scattered Random  Aligned Contiguous Rear Custom
float user_angle = 0;
float user_travel = 0;

int s_seam_position_get(string &out get_val)
{
	int pos = get_int("seam_position");
	string seam_pos;
	get_string("seam_position", seam_pos);
	if(pos < 7){
		if (pos == 0) return 2;// Scattered
		if (pos == 1) return 3;// Random
		return pos + 1;
	} else {
		float angle = get_float("seam_angle_cost");
		float travel = get_float("seam_travel_cost");
		if(angle >= 1. && travel <= 0.8) return 0; //corner
		if(angle <= 1. && travel >= 1.0) return 1; //nearest
		user_angle = angle;
		user_travel = travel;
	}
	return 7;
}

void s_seam_position_set(string &in set_val, int idx)
{
	if (idx == 2 ) {
		set_int("seam_position", 0); // Scattered
	} else if (idx == 3) {
		set_int("seam_position", 1); // Random
	} else if (idx == 4) {
		set_int("seam_position", 3); // Aligned
	} else if (idx == 5) {
		set_int("seam_position", 4); // Contiguous
	} else if (idx == 6) {
		set_int("seam_position", 5); // Rear
	} else if (idx <= 1) {
		set_int("seam_position", 7);
		if (idx == 0) { //corner
			set_percent("seam_angle_cost", 120);
			set_percent("seam_travel_cost", 40);
		} else { // == 1 // nearest
			set_percent("seam_angle_cost", 80);
			set_percent("seam_travel_cost", 100);
		}
	} else {
		set_int("seam_position", 7); // custom
		if(user_angle > 0 || user_travel > 0){
			set_percent("seam_angle_cost", user_angle);
			set_percent("seam_travel_cost", user_travel);
		} else {
			back_initial_value("seam_angle_cost");
			back_initial_value("seam_travel_cost");
		}
	}
}

// s_wall_thickness
// set the perimeter_spacing & external_perimeter_spacing
// as m * 2 perimeter_spacing + n * 2 * external_perimeter_spacing = o * s_wall_thickness

float s_wall_thickness_get()
{
	int nb_peri = 2;
	if (!get_custom_int(0,"wall_thickness_lines", nb_peri)) nb_peri = 2;
	float ps = get_computed_float("perimeter_extrusion_spacing");
	float eps = get_computed_float("external_perimeter_extrusion_spacing");
	//print("s_wall_thickness_get "+ps+" "+eps+" *"+nb_peri+"\n");
	if (nb_peri == 0) return 0; // fake 'disable'
	if (nb_peri < 2) nb_peri = 2; // too thin value
	if( eps > 100000) return 0;
	if( ps > 100000) return 0;
	return eps * 2 + (nb_peri-2) * ps;
}

void s_wall_thickness_set(float new_val)
{
	float diameter = get_float("nozzle_diameter");
	float nb = new_val / diameter;
	int int_nb = int(floor(nb+0.1));
	//print("float "+nb+" cast into "+int_nb+"\n");
	if (int_nb > 1 && int_nb < 4) {
		float ext_spacing = new_val / int_nb;
		set_float("external_perimeter_extrusion_spacing", ext_spacing);
		set_float("perimeter_extrusion_spacing", ext_spacing);
		set_custom_int(0,"wall_thickness_lines", int_nb);
	} else if(int_nb > 3) {
		//try with thin external
		float ext_spacing = diameter;
		float spacing = (new_val - ext_spacing * 2) / (int_nb - 2);
		if (spacing > diameter * 1.5) {
			// too different, get back to same value
			ext_spacing = new_val / int_nb;
			spacing = ext_spacing;
		}
		set_float("external_perimeter_extrusion_spacing", ext_spacing);
		set_float("perimeter_extrusion_spacing", spacing);
		set_custom_int(0,"wall_thickness_lines", int_nb);
	} else if(new_val == 0) {
		// fake 'disable' to not confuse people susi#2700
		set_custom_int(0,"wall_thickness_lines", 0);
		back_initial_value("external_perimeter_extrusion_spacing");
		back_initial_value("perimeter_extrusion_spacing");
	} else {
		back_custom_initial_value(0,"wall_thickness_lines");
		back_initial_value("external_perimeter_extrusion_spacing");
		back_initial_value("perimeter_extrusion_spacing");
		// refresh the displayed value to a valid one
		ask_for_refresh();
	}
//	ask_for_refresh();
}

// quick settings brim

float last_brim_val = 5;

int s_brim_get()
{
	float bw = get_float("brim_width");
	if (bw > 0) {
		last_brim_val = bw;
		return 1;
	}
	return 0;
}

void s_brim_set(bool new_val)
{
	if(new_val) {
		float bw = get_float("brim_width");
		set_float("brim_width", last_brim_val);
	} else {
			set_float("brim_width", 0);
	}
}

// quick settings support

int s_support_fff_get(string &out get_val)
{
	bool support_material = get_bool("support_material");
	if (!support_material) { // None
		return 0;
	}
	bool support_material_auto = get_bool("support_material_auto");
	if (!support_material_auto) { // For support enforcers only
		return 2;
	}
	bool support_material_buildplate_only = get_bool("support_material_buildplate_only");
	if (support_material_buildplate_only) { // Support on build plate only
		return 1;
	}
	// everywhere
	return 3;
}

void s_support_fff_set(string &in new_val, int idx)
{
	if(idx == 0) { // None
		back_initial_value("support_material_buildplate_only");
		back_initial_value("support_material_auto");
		set_bool("support_material", false);
	} else if(idx == 1) { // Support on build plate only
		set_bool("support_material_buildplate_only", true);
		set_bool("support_material_auto", true);
		set_bool("support_material", true);
	} else if(idx == 2) { // For support enforcers only
		set_bool("support_material_buildplate_only", false);
		set_bool("support_material_auto", false);
		set_bool("support_material", true);
	} else if(idx == 3) { // everywhere
		set_bool("support_material_buildplate_only", false);
		set_bool("support_material_auto", true);
		set_bool("support_material", true);
	}
}

//TODO to replicate prusa:
// brim_type
// cooling
// xy compensation (both)


//test:
//	setting:script:bool:easy:depends$enforce_full_fill_volume:label$fullfill-lol:s_fullfill
//	setting:script:int:easy:depends$perimeters:label$perimeters-lol:s_perimeter
//	setting:script:float:easy:depends$top_solid_min_thickness:label$thickness-lol:s_thickness
//	setting:script:percent:easy:depends$bridge_flow_ratio:label$bridgeflow-lol:s_bridgeflow
//	setting:script:string:easy:depends$notes:label$notes-lol:s_notes
//	setting:script:enum$b$bof$m$mouaif:easy:depends$no_perimeter_unsupported_algo:label$noperi-lol:s_noperi

int s_fullfill_get()
{
	if (get_bool("enforce_full_fill_volume")) return 1;
	return 0;
}
void s_fullfill_set(bool set)
{
	set_bool("enforce_full_fill_volume", set);
}


int s_perimeter_get()
{
	return get_int("perimeters");
}
void s_perimeter_set(int set)
{
	set_int("perimeters", set);
}


float s_thickness_get()
{
	return get_float("top_solid_min_thickness");
}
void s_thickness_set(float set)
{
	set_float("top_solid_min_thickness", set);
}


float s_bridgeflow_get()
{
	return get_float("bridge_flow_ratio");
}
void s_bridgeflow_set(float set)
{
	set_percent("bridge_flow_ratio", set);
}


void s_notes_get(string &out get_val)
{
	get_string("notes", get_val);
}
void s_notes_set(string &out set_val)
{
	set_string("notes", set_val);
}


int s_noperi_get(string &out get_val)
{
	return get_int("no_perimeter_unsupported_algo") == 0 ? 0 : 1;
}
void s_noperi_set(string &out set_val, int idx)
{
	//set_int("no_perimeter_unsupported_algo", idx == 0 ? 0 : 3);
	if (idx == 0) set_int("no_perimeter_unsupported_algo",0);
	else set_string("no_perimeter_unsupported_algo", "filled");
}
