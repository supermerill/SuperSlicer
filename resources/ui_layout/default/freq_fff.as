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


// quick settings bed type (nematx)

int s_bed_fff_get(string &out get_val)
{
    int bed_temperature = get_int("bed_temperature");
    int fl_bed_temperature = get_int("first_layer_bed_temperature");
    if (bed_temperature == fl_bed_temperature) {
        if (bed_temperature == 130) {
            return 1; //glue
        }
        if (bed_temperature == 170) {
            return 2; //noglue
        }
    }
    return 0; // custom
}

void s_bed_fff_set(string &in new_val, int idx)
{
	if(idx == 0) { // custom
		back_initial_value("bed_temperature");
		back_initial_value("first_layer_bed_temperature");
	} else if(idx == 1) { // glue
		set_int("bed_temperature", 130);
		set_int("first_layer_bed_temperature", 130);
	} else if(idx == 2) { // noglue
		set_int("bed_temperature", 170);
		set_int("first_layer_bed_temperature", 170);
	}
}

