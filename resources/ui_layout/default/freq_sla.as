// quick settings support

int s_support_sla_get(string &out get_val)
{
	bool supports_enable = get_bool("supports_enable");
	if (!supports_enable) { // None
		return 0;
	}
	bool support_enforcers_only = get_bool("support_enforcers_only");
	if (support_enforcers_only) { // Support on enforcer only
		return 2;
	}
	bool support_buildplate_only = get_bool("support_buildplate_only");
	if (support_buildplate_only) { // Support on build plate only
		return 1;
	}
	// everywhere
	return 3;
}

void s_support_sla_set(string &in new_val, int idx)
{
	if(idx == 0) { // None
		back_initial_value("support_enforcers_only");
		back_initial_value("support_buildplate_only");
		set_bool("supports_enable", false);
	} else if(idx == 1) { // Support on build plate only
		set_bool("support_buildplate_only", true);
		set_bool("support_enforcers_only", false);
		set_bool("supports_enable", true);
	} else if(idx == 2) { // For support enforcers only
		set_bool("support_buildplate_only", false);
		set_bool("support_enforcers_only", true);
		set_bool("support_material", true);
	} else { // everywhere
		set_bool("support_buildplate_only", false);
		set_bool("support_enforcers_only", false);
		set_bool("supports_enable", true);
	}
}


// quick settings pad

int s_pad_get(string &out get_val)
{
	bool pad_enable = get_bool("pad_enable");
	if (!pad_enable) { // None
		return 0;
	}
	bool pad_around_object = get_bool("pad_around_object");
	if (!pad_around_object) { // Below object
		return 1;
	}
	// Around object
	return 2;
}

void s_pad_set(string &in new_val, int idx)
{
	if(idx == 0) { // None
		back_initial_value("pad_around_object");
		set_bool("pad_enable", false);
	} else if(idx == 1) { // Below object
		set_bool("pad_around_object", false);
		set_bool("pad_enable", true);
	} else { // Around object
		set_bool("pad_around_object", true);
		set_bool("pad_enable", true);
	}
}
